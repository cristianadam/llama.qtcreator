#include "llamahtmlexporter.h"

#include <QColor>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QVector>

#include <functional>

#include <3rdparty/markus/markus.h>

#include "llamatheme.h"
#include "llamathinkingsectionparser.h"
#include "llamasyntaxhighlighter.h"
#include "markdownrenderer.h"
#include "tools/factory.h"

namespace LlamaCpp {

using DiagramSvg = MarkdownRenderer::DiagramSvg;

// ---------------------------------------------------------------------------
// Small HTML helpers
// ---------------------------------------------------------------------------

static QString escapeHtml(QString text)
{
    text.replace(QLatin1String("&"), QLatin1String("&amp;"));
    text.replace(QLatin1String("<"), QLatin1String("&lt;"));
    text.replace(QLatin1String(">"), QLatin1String("&gt;"));
    text.replace(QLatin1String("\""), QLatin1String("&quot;"));
    return text;
}

// The inverse of the escaping markus applies to code‑block content
// (entities first, &amp; last).
static QString unescapeHtmlCode(QString text)
{
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return text;
}

static QString replaceMatches(const QString &text,
                              const QRegularExpression &re,
                              const std::function<QString(const QRegularExpressionMatch &)> &fn)
{
    QString out;
    int lastEnd = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += text.mid(lastEnd, m.capturedStart() - lastEnd);
        out += fn(m);
        lastEnd = m.capturedEnd();
    }
    out += text.mid(lastEnd);
    return out;
}

// ---------------------------------------------------------------------------
// Syntax‑highlighted code
// ---------------------------------------------------------------------------

// Runs \a content through the shared KSyntaxHighlighting highlighter and
// returns it as escaped text with per‑run <span> styling (the same colour
// scheme the chat view uses).
static QString highlightCode(const QString &language, const QString &content)
{
    if (language.isEmpty())
        return escapeHtml(content);

    QVector<HighlightFragment> fragments;
    SyntaxHighlighter highlighter;
    highlighter.setDefinition(syntaxDefinitionForName(language));
    highlighter.highlight(content, QTextCharFormat(), fragments);

    QString out;
    for (const HighlightFragment &f : std::as_const(fragments)) {
        const QString text = escapeHtml(f.text);
        // name() (not isValid()): the colour scheme may hand out CMYK‑space
        // colours, which report isValid() == false but convert to a hex name.
        const QString fg = f.format.foreground().color().name(QColor::HexRgb);
        QString style;
        if (!fg.isEmpty())
            style += QStringLiteral("color:%1;").arg(fg);
        if (f.format.fontWeight() == QFont::Bold)
            style += QLatin1String("font-weight:bold;");
        if (f.format.fontItalic())
            style += QLatin1String("font-style:italic;");
        if (style.isEmpty() || text.isEmpty()) {
            out += text;
            continue;
        }
        out += QStringLiteral("<span style=\"%1\">%2</span>").arg(style, text);
    }
    return out;
}

// Replaces every <pre><code ...>escaped</code></pre> emitted by markus with
// the syntax‑highlighted equivalent.
static QString highlightCodeBlocks(const QString &html)
{
    const QRegularExpression re(QStringLiteral(
        "<pre><code(?: class=\"language-([^\"]*)\")?>(.*?)</code></pre>"),
        QRegularExpression::DotMatchesEverythingOption);
    return replaceMatches(html, re, [](const QRegularExpressionMatch &m) -> QString {
        return QStringLiteral("<pre><code>")
               + highlightCode(m.captured(1), unescapeHtmlCode(m.captured(2)))
               + QStringLiteral("</code></pre>");
    });
}

// ---------------------------------------------------------------------------
// Diagrams (persisted mermaid/KaTeX SVGs)
// ---------------------------------------------------------------------------

static QString svgDataUrl(const DiagramSvg &svg)
{
    return QStringLiteral("data:image/svg+xml;base64,") + QString::fromLatin1(svg.svg.toBase64());
}

// Same substitutions as MarkdownRenderer::embedDiagramSvgs(), but the
// rendered pictures become self‑contained <img> tags (base64 data URLs)
// instead of raw SVG text.  Diagrams without a cached SVG are left untouched.
static QString embedDiagramSvgsHtml(QString content, const QMap<QString, DiagramSvg> &diagrams)
{
    if (diagrams.isEmpty())
        return content;

    auto lookup = [&diagrams](const QString &key) -> const DiagramSvg * {
        const auto it = diagrams.constFind(key);
        return it == diagrams.constEnd() ? nullptr : &it.value();
    };

    // ```mermaid blocks -> <details> section with the rendered picture in
    // the header and the source in the body (mirroring the chat UI).
    const QRegularExpression mermaidRe(
        QStringLiteral("```(?:mermaid|mmd)\\s*\\n(.*?)\\n?```"),
        QRegularExpression::DotMatchesEverythingOption);
    content = replaceMatches(content, mermaidRe, [lookup](const QRegularExpressionMatch &m) -> QString {
        const DiagramSvg *svg = lookup(MarkdownRenderer::mermaidDiagramKey(m.captured(1).trimmed()));
        if (!svg)
            return m.captured(0);
        return QStringLiteral("<details>\n<summary>Mermaid diagram</summary>\n\n")
               + QStringLiteral("<img src=\"%1\" alt=\"Mermaid diagram\">\n\n```mermaid\n")
                       .arg(svgDataUrl(*svg))
               + m.captured(1).trimmed() + QStringLiteral("\n```\n\n</details>");
    });

    // Math spans -> inline SVGs. Display math first (it contains the inline
    // delimiters); spans without a cached SVG are kept verbatim.
    const QRegularExpression displayRe(QStringLiteral("\\$\\$\\s*(.*?)\\s*\\$\\$"),
                                       QRegularExpression::DotMatchesEverythingOption);
    content = replaceMatches(content, displayRe, [lookup](const QRegularExpressionMatch &m) -> QString {
        const DiagramSvg *svg
            = lookup(MarkdownRenderer::katexDiagramKey(m.captured(1).trimmed(), true));
        // The alt carries the formula without delimiters: a $...$ in the
        // attribute would be matched again by the inline-math pass below.
        return svg ? QStringLiteral("<img class=\"math display\" src=\"%1\" alt=\"%2\">")
                       .arg(svgDataUrl(*svg), escapeHtml(m.captured(1).trimmed()))
                   : m.captured(0);
    });
    // The content must not start or end with whitespace, and the delimiters
    // must not be escaped or part of a $$ pair (plain‑dollar prices like
    // "$5 and $10" must stay text).
    const QRegularExpression inlineRe(QStringLiteral(
        "(?<!\\\\)\\$([^\\s$](?:[^$\\n]*[^\\s$])?)\\$(?<!\\\\)(?!\\$)"));
    content = replaceMatches(content, inlineRe, [lookup](const QRegularExpressionMatch &m) -> QString {
        const DiagramSvg *svg = lookup(MarkdownRenderer::katexDiagramKey(m.captured(1), false));
        return svg ? QStringLiteral("<img class=\"math\" src=\"%1\" alt=\"%2\">")
                       .arg(svgDataUrl(*svg), escapeHtml(m.captured(1)))
                   : m.captured(0);
    });

    return content;
}

// Resolves the Token_* colour names in a style sheet to the current Qt
// Creator theme colours.  When the theme is not available (e.g. in the
// tests) the tokens fall back to a light palette matching the chat
// renderer's defaults.
static QString themeStyleSheet(const QString &styleSheet)
{
    static const QMap<QString, QString> fallbacks = {
        {QStringLiteral("Token_Background_Default"), QStringLiteral("#ffffff")},
        {QStringLiteral("Token_Background_Muted"), QStringLiteral("#f6f8fa")},
        {QStringLiteral("Token_Text_Default"), QStringLiteral("#1f2328")},
        {QStringLiteral("Token_Text_Muted"), QStringLiteral("#656d76")},
        {QStringLiteral("Token_Text_Accent"), QStringLiteral("#0969da")},
        {QStringLiteral("Token_Accent_Default"), QStringLiteral("#0969da")},
        {QStringLiteral("Token_Stroke_Muted"), QStringLiteral("#d0d7de")},
        {QStringLiteral("Token_Stroke_Subtle"), QStringLiteral("#d0d7de")},
    };
    return replaceThemeColorNamesWithRGBNames(styleSheet, fallbacks);
}

// markus renders <summary> content as a block, wrapping it in a <p> that is
// not valid phrasing content for <summary>; unwrap it.
static QString cleanUpSummaries(const QString &html)
{
    const QRegularExpression re(QStringLiteral("<summary><p>(.*?)</p>\\s*</summary>"),
                               QRegularExpression::DotMatchesEverythingOption);
    QString result = html;
    return result.replace(re, QStringLiteral("<summary>\\1</summary>"));
}

// ---------------------------------------------------------------------------
// Markdown -> HTML
// ---------------------------------------------------------------------------

static QString markdownToHtml(const QString &content, const QMap<QString, DiagramSvg> &diagrams)
{
    const QString prepared = embedDiagramSvgsHtml(content, diagrams);

    markus::Options options;
    // Mirror the chat renderer (GitHub‑flavoured Markdown).
    options.enable_tables = true;
    options.enable_autolink = true;
    options.enable_strikethrough = true;
    options.enable_tasklist = true;
    options.enable_latex_math = true;

    const std::pmr::string html
        = markus::MarkdownToHtml(prepared.toStdString(), options);
    return cleanUpSummaries(highlightCodeBlocks(QString::fromUtf8(html.data(), html.size())));
}

// ---------------------------------------------------------------------------
// Tool calls
// ---------------------------------------------------------------------------

static QString toolCallToHtml(const Message &msg)
{
    QString functionName;
    QString argumentsJson;
    QString functionResult;
    QString toolStatus;

    for (const QVariantMap &e : msg.extra) {
        if (e.contains("tool_calls")) {
            QJsonArray calls = e.value("tool_calls").toJsonArray();
            if (!calls.isEmpty()) {
                QJsonObject callObj = calls.first().toObject();
                functionName = callObj.value("function").toObject().value("name").toString();
                argumentsJson
                    = callObj.value("function").toObject().value("arguments").toString();
            }
        }
        if (e.contains("tool_result")) {
            QJsonObject result = e.value("tool_result").toJsonObject();
            if (!result.isEmpty())
                functionResult = toolResultText(result.value("content"));
        }
        if (e.contains("tool_status"))
            toolStatus = e.value("tool_status").toString(); // "success" / "failed"
    }

    if (functionName.isEmpty())
        return {};

    QString formattedArgs = argumentsJson;
    const QJsonDocument argDoc = QJsonDocument::fromJson(argumentsJson.toUtf8());
    if (argDoc.isObject())
        formattedArgs = QString::fromUtf8(QJsonDocument(argDoc.object()).toJson(QJsonDocument::Indented));

    // Reuse the tool's one‑line summary so the export matches what the UI
    // shows in the collapsed tool bubble.
    QString summary = QStringLiteral("Tool: %1").arg(functionName);
    if (const std::unique_ptr<Tool> tool = ToolFactory::instance().create(functionName))
        summary = tool->oneLineSummary(argDoc.object());
    if (!toolStatus.isEmpty())
        summary += QStringLiteral(" (%1)").arg(toolStatus);
    // The summary is plain text in the UI (markdown backticks around
    // arguments); mirror the monospace look with <code>.
    QString summaryHtml = escapeHtml(summary);
    summaryHtml.replace(QRegularExpression(QStringLiteral("`([^`]+)`")),
                        QStringLiteral("<code>\\1</code>"));

    if (functionResult.endsWith('\n'))
        functionResult.chop(1);

    QString html = QStringLiteral("<div class=\"msg tool\">\n<details>\n<summary>%1</summary>\n\n")
                       .arg(summaryHtml);
    html += QStringLiteral("<p><strong>Arguments</strong></p>\n<pre><code>");
    html += highlightCode(QLatin1String("json"), formattedArgs);
    html += QStringLiteral("</code></pre>\n");
    if (!functionResult.isEmpty()) {
        html += QStringLiteral("<p><strong>Result</strong></p>\n<pre><code>");
        html += highlightCode({}, functionResult);
        html += QStringLiteral("</code></pre>\n");
    }
    html += QStringLiteral("</details>\n</div>\n");
    return html;
}

// ---------------------------------------------------------------------------
// Messages and conversations
// ---------------------------------------------------------------------------

QString HtmlExporter::messageToHtml(const Message &msg)
{
    if (msg.role == "tool")
        return toolCallToHtml(msg);

    // Embed the persisted diagram SVGs so the export shows the rendered
    // pictures instead of the sources.
    const QMap<QString, DiagramSvg> diagrams = MarkdownRenderer::diagramSvgsFromExtra(msg.extra);

    if (msg.role == "user")
        return QStringLiteral("<div class=\"msg user\">\n")
               + markdownToHtml(msg.content, diagrams) + QStringLiteral("\n</div>\n");

    // Assistant (or anything else).
    QString processedContent = msg.content;
    processedContent.replace(ThinkingSectionParser::startToken(),
                             "<details>\n<summary>Thought</summary>\n\n");
    processedContent.replace(ThinkingSectionParser::endToken(),
                             "\n</details>\n\n");

    // A tool‑call‑only assistant message renders through its tool bubble,
    // whose call + result are exported with the tool message instead.
    if (processedContent.trimmed().isEmpty()) {
        for (const QVariantMap &e : msg.extra) {
            if (e.contains("tool_calls"))
                return {};
        }
    }

    return QStringLiteral("<div class=\"msg assistant\">\n")
           + markdownToHtml(processedContent, diagrams) + QStringLiteral("\n</div>\n");
}

QString HtmlExporter::conversationHtml(const QString &title, const QVector<Message> &messages)
{
    QString body;
    for (const Message &msg : std::as_const(messages))
        body += messageToHtml(msg);

    // Colours follow the current Qt Creator theme, like the chat view.
    const QString css = themeStyleSheet(R"(
        body {
            font-family: -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial, sans-serif;
            font-size: 14px;
            line-height: 1.5;
            background: Token_Background_Default;
            color: Token_Text_Default;
            max-width: 860px;
            margin: 0 auto;
            padding: 24px 16px;
        }
        h1 { font-size: 20px; }
        .chat { display: flex; flex-direction: column; gap: 12px; }
        .msg {
            border-radius: 8px;
            padding: 8px 12px;
            overflow-wrap: break-word;
        }
        .msg.user { background: Token_Background_Muted; }
        .msg.assistant {
            background: Token_Background_Default;
            border: 1px solid Token_Stroke_Muted;
        }
        .msg.tool {
            background: Token_Background_Default;
            border: 1px solid Token_Stroke_Subtle;
        }
        .msg p { margin: 8px 0; }
        .msg > :first-child { margin-top: 0; }
        .msg > :last-child { margin-bottom: 0; }
        pre {
            background: Token_Background_Muted;
            border: 1px solid Token_Stroke_Subtle;
            border-radius: 8px;
            padding: 8px 10px;
            overflow-x: auto;
            font-family: "SF Mono", Menlo, Consolas, monospace;
            font-size: 13px;
        }
        code { font-family: "SF Mono", Menlo, Consolas, monospace; }
        p code, li code, td code {
            background: Token_Background_Muted;
            border-radius: 4px;
            padding: 1px 4px;
            font-size: 90%;
        }
        pre code { background: none; padding: 0; }
        a { color: Token_Accent_Default; }
        blockquote {
            border-left: 3px solid Token_Stroke_Muted;
            margin: 8px 0;
            padding: 2px 12px;
            color: Token_Text_Muted;
        }
        hr { border: none; border-top: 1px solid Token_Stroke_Muted; margin: 12px 0; }
        table { border-collapse: collapse; margin: 8px 0; }
        th, td { border: 1px solid Token_Stroke_Muted; padding: 4px 8px; }
        /* Like the chat renderer: the header is plain (bold) and only the
           1st, 3rd, … body row gets the muted shading. */
        tbody tr:nth-child(odd) { background: Token_Background_Muted; }
        details { margin: 8px 0; }
        summary { cursor: pointer; color: Token_Text_Accent; }
        img { max-width: 100%; }
        span.math { color: Token_Text_Default; }
        /* The persisted math SVGs are re-boxed so their bottom edge sits one
           font descent below the math baseline (the chat view pins them to
           the line bottom); match that, or the formula floats a descent
           above the text (the img default is baseline alignment). */
        img.math { vertical-align: bottom; }
        /* Display math reads as its own centred line, like the chat view. */
        img.math.display { display: block; margin: 8px auto; }
    )");

    return QStringLiteral(
                   "<!DOCTYPE html>\n"
                   "<html>\n"
                   "<head>\n"
                   "<meta charset=\"utf-8\">\n"
                   "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
                   "<title>%1</title>\n"
                   "<style>\n%2</style>\n"
                   "</head>\n"
                   "<body>\n"
                   "<h1>%1</h1>\n"
                   "<div class=\"chat\">\n%3\n</div>\n"
                   "</body>\n"
                   "</html>\n")
        .arg(escapeHtml(title), css, body);
}

} // namespace LlamaCpp
