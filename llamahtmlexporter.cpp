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

// The export stylesheet's colour tokens, resolved to the current Qt Creator
// theme colours.  When the theme is not available (e.g. in the tests) the
// tokens fall back to a light palette matching the chat renderer's
// defaults.
struct StyleColors
{
    QString backgroundDefault, backgroundMuted, textDefault, textMuted;
    QString textAccent, accentDefault, strokeMuted, strokeSubtle;
};

// Light-palette fallbacks for the export stylesheet's colour tokens, used
// when the Qt Creator theme is not available (e.g. in the tests).  Shared
// by themeColors() and themeStyleSheet() so the two never drift apart.
static const QMap<QString, QString> &themeColorFallbacks()
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
    return fallbacks;
}

static StyleColors themeColors()
{
    // Resolve every token in a single probe sheet and read the values back.
    const QString probe = QStringLiteral(
        "p{a:Token_Background_Default;b:Token_Background_Muted;"
        "c:Token_Text_Default;d:Token_Text_Muted;e:Token_Text_Accent;"
        "f:Token_Accent_Default;g:Token_Stroke_Muted;h:Token_Stroke_Subtle;}");
    const QString resolved = replaceThemeColorNamesWithRGBNames(probe, themeColorFallbacks());
    auto value = [&resolved](QChar key) -> QString {
        const QRegularExpression re(QStringLiteral("\\b%1:([^;]+)").arg(QString(key)));
        const QRegularExpressionMatch m = re.match(resolved);
        return m.hasMatch() ? m.captured(1) : QString();
    };
    return {value('a'), value('b'), value('c'), value('d'),
            value('e'), value('f'), value('g'), value('h')};
}

// Resolves the Token_* colour names in a style sheet to the current Qt
// Creator theme colours (same fallbacks as themeColors()).
static QString themeStyleSheet(const QString &styleSheet)
{
    return replaceThemeColorNamesWithRGBNames(styleSheet, themeColorFallbacks());
}

// ---------------------------------------------------------------------------
// Inlining the <style> rules
// ---------------------------------------------------------------------------

// Adds the export stylesheet's rules as style attributes on every element,
// so the markup keeps its look when pasted into an editor that drops
// <style> blocks (most WYSIWYG / "what you see is what you get" editors
// only carry inline styles across).  The <style> block is kept as well:
// the inlined styles mirror it rule for rule, so the two never disagree
// when the document is opened on its own.
//
// The HTML is fully generated here (markus + the helpers above), so instead
// of a generic CSS engine the walk applies the known selectors directly,
// using a tag stack for the ancestor-dependent rules (.msg children,
// pre code, p/li/td code, tbody row parity).
static QString inlineStyles(const QString &html, const StyleColors &c)
{
    static const QString kSans
        = QStringLiteral("-apple-system, \"Segoe UI\", Roboto, \"Helvetica Neue\", Arial, sans-serif");
    static const QString kMono = QStringLiteral("\"SF Mono\", Menlo, Consolas, monospace");
    static const QSet<QString> kVoidTags
        = {QStringLiteral("img"), QStringLiteral("hr"), QStringLiteral("br"),
           QStringLiteral("meta"), QStringLiteral("link"),
           QStringLiteral("input")}; // markus' self-closing task-list checkboxes

    struct Frame
    {
        QString tag;
        bool isMsg = false; // <div class="msg ...">
        bool msgHasElementChild = false;
        bool skipEdgeMargins = false; // img.math.display: the sheet's higher
                                      // specificity keeps its 8px margins
        int tbodyRows = 0;
        int styleAttrEnd = -1; // offset in the output of the closing quote of
                               // this element's style attribute (-1: none)
        int lastChildStyleEnd = -1; // for .msg frames: same, for the last
                                    // element child (":last-child" margin)
    };

    class StyleBuilder
    {
    public:
        void add(const QString &declaration)
        {
            if (!declaration.isEmpty())
                m_parts << declaration;
        }
        // Semicolon‑separated; the ":last-child" margin is appended in
        // place before the closing quote, so no trailing separator.
        QString toString() const { return m_parts.join(QLatin1Char(';')); }

    private:
        QStringList m_parts;
    };

    QVector<Frame> stack;
    QString out;
    out.reserve(html.size() * 3 / 2);

    const QRegularExpression tagRe(QStringLiteral("<[a-zA-Z/!?][^>]*>"));
    auto it = tagRe.globalMatch(html);
    int lastEnd = 0;
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += html.mid(lastEnd, m.capturedStart() - lastEnd);
        lastEnd = m.capturedEnd();
        const QString tagText = m.captured(0);
        const QChar second = tagText.at(1);

        // Doctype and comments: copy verbatim.  (The <style> element goes
        // through the normal path below as an unknown tag; its CSS content
        // contains '>' characters in its selectors but no '<', so it passes
        // through as plain text between tags.)
        if (second != QLatin1Char('/') && !second.isLetter()) {
            out += tagText;
            continue;
        }

        // Closing tag: pop the frame.  A frame closing directly inside a
        // .msg bubble is its last element child so far — remember it so the
        // ".msg > :last-child" bottom margin can be added in place when the
        // bubble itself closes.
        if (second == QLatin1Char('/')) {
            if (!stack.isEmpty()) {
                const Frame closed = stack.takeLast();
                if (!stack.isEmpty()) {
                    if (stack.last().isMsg && !closed.skipEdgeMargins)
                        stack.last().lastChildStyleEnd = closed.styleAttrEnd;
                } else if (closed.isMsg && closed.lastChildStyleEnd >= 0) {
                    // The builder leaves no trailing separator, so add one.
                    out.insert(closed.lastChildStyleEnd,
                               QStringLiteral(";margin-bottom:0"));
                }
            }
            out += tagText;
            continue;
        }

        // Tag name and class attribute.
        int nameEnd = 1;
        while (nameEnd < tagText.size() && tagText.at(nameEnd).isLetterOrNumber())
            ++nameEnd;
        const QString name = tagText.mid(1, nameEnd - 1).toLower();
        QString classes;
        const int classPos = tagText.indexOf(QStringLiteral("class=\""));
        if (classPos >= 0) {
            const int start = classPos + 7;
            const int end = tagText.indexOf(QLatin1Char('"'), start);
            if (end > start)
                classes = tagText.mid(start, end - start);
        }
        const QStringList classList = classes.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        const auto hasClass = [&classList](const char *cls) {
            return classList.contains(QLatin1String(cls));
        };
        const auto hasAncestor = [&stack](const char *tag) {
            for (int i = stack.size() - 1; i >= 0; --i)
                if (stack[i].tag == QLatin1String(tag))
                    return true;
            return false;
        };

        // The syntax‑highlighter already emits style attributes; no CSS rule
        // targets those spans beyond what they inherit, so leave them alone.
        if (tagText.contains(QLatin1String("style="))) {
            out += tagText;
            if (!stack.isEmpty() && stack.last().isMsg)
                stack.last().msgHasElementChild = true;
            continue;
        }

        StyleBuilder b;
        const bool isMsg = name == QLatin1String("div") && hasClass("msg");
        if (name == QLatin1String("body")) {
            b.add(QStringLiteral("font-family:%1").arg(kSans));
            b.add(QStringLiteral("font-size:14px"));
            b.add(QStringLiteral("line-height:1.5"));
            b.add(QStringLiteral("background:%1").arg(c.backgroundDefault));
            b.add(QStringLiteral("color:%1").arg(c.textDefault));
            b.add(QStringLiteral("max-width:860px"));
            b.add(QStringLiteral("margin:0 auto"));
            b.add(QStringLiteral("padding:24px 16px"));
        } else if (name == QLatin1String("h1")) {
            b.add(QStringLiteral("font-size:20px"));
        } else if (isMsg) {
            b.add(QStringLiteral("border-radius:8px"));
            b.add(QStringLiteral("padding:8px 12px"));
            b.add(QStringLiteral("overflow-wrap:break-word"));
            if (hasClass("user")) {
                b.add(QStringLiteral("background:%1").arg(c.backgroundMuted));
            } else if (hasClass("assistant")) {
                b.add(QStringLiteral("background:%1").arg(c.backgroundDefault));
                b.add(QStringLiteral("border:1px solid %1").arg(c.strokeMuted));
            } else if (hasClass("tool")) {
                b.add(QStringLiteral("background:%1").arg(c.backgroundDefault));
                b.add(QStringLiteral("border:1px solid %1").arg(c.strokeSubtle));
            }
        } else if (name == QLatin1String("div") && hasClass("chat")) {
            b.add(QStringLiteral("display:flex"));
            b.add(QStringLiteral("flex-direction:column"));
            b.add(QStringLiteral("gap:12px"));
        } else if (name == QLatin1String("p")) {
            b.add(QStringLiteral("margin:8px 0"));
        } else if (name == QLatin1String("pre")) {
            b.add(QStringLiteral("background:%1").arg(c.backgroundMuted));
            b.add(QStringLiteral("border:1px solid %1").arg(c.strokeSubtle));
            b.add(QStringLiteral("border-radius:8px"));
            b.add(QStringLiteral("padding:8px 10px"));
            b.add(QStringLiteral("overflow-x:auto"));
            b.add(QStringLiteral("font-family:%1").arg(kMono));
            b.add(QStringLiteral("font-size:13px"));
        } else if (name == QLatin1String("code")) {
            b.add(QStringLiteral("font-family:%1").arg(kMono));
            if (hasAncestor("pre")) {
                b.add(QStringLiteral("background:none"));
                b.add(QStringLiteral("padding:0"));
            } else if (hasAncestor("p") || hasAncestor("li") || hasAncestor("td")) {
                b.add(QStringLiteral("background:%1").arg(c.backgroundMuted));
                b.add(QStringLiteral("border-radius:4px"));
                b.add(QStringLiteral("padding:1px 4px"));
                b.add(QStringLiteral("font-size:90%"));
            }
        } else if (name == QLatin1String("a")) {
            b.add(QStringLiteral("color:%1").arg(c.accentDefault));
        } else if (name == QLatin1String("blockquote")) {
            b.add(QStringLiteral("border-left:3px solid %1").arg(c.strokeMuted));
            b.add(QStringLiteral("margin:8px 0"));
            b.add(QStringLiteral("padding:2px 12px"));
            b.add(QStringLiteral("color:%1").arg(c.textMuted));
        } else if (name == QLatin1String("hr")) {
            b.add(QStringLiteral("border:none"));
            b.add(QStringLiteral("border-top:1px solid %1").arg(c.strokeMuted));
            b.add(QStringLiteral("margin:12px 0"));
        } else if (name == QLatin1String("table")) {
            b.add(QStringLiteral("border-collapse:collapse"));
            b.add(QStringLiteral("margin:8px 0"));
        } else if (name == QLatin1String("th") || name == QLatin1String("td")) {
            b.add(QStringLiteral("border:1px solid %1").arg(c.strokeMuted));
            b.add(QStringLiteral("padding:4px 8px"));
        } else if (name == QLatin1String("tr")) {
            // "tbody tr:nth-child(odd)": the header rows live in <thead>, so
            // only rows directly under <tbody> count towards the parity.
            if (!stack.isEmpty() && stack.last().tag == QLatin1String("tbody")) {
                ++stack.last().tbodyRows;
                if (stack.last().tbodyRows % 2 == 1)
                    b.add(QStringLiteral("background:%1").arg(c.backgroundMuted));
            }
        } else if (name == QLatin1String("details")) {
            b.add(QStringLiteral("margin:8px 0"));
        } else if (name == QLatin1String("summary")) {
            b.add(QStringLiteral("cursor:pointer"));
            b.add(QStringLiteral("color:%1").arg(c.textAccent));
        } else if (name == QLatin1String("img")) {
            b.add(QStringLiteral("max-width:100%"));
            if (hasClass("math")) {
                b.add(QStringLiteral("vertical-align:bottom"));
                if (hasClass("display")) {
                    b.add(QStringLiteral("display:block"));
                    b.add(QStringLiteral("margin:8px auto"));
                }
            }
        } else if (name == QLatin1String("span") && hasClass("math")) {
            b.add(QStringLiteral("color:%1").arg(c.textDefault));
        }

        // ".msg > :first-child": zero the top margin of the bubble's first
        // element child.  img.math.display is exempt: in the sheet its
        // "margin: 8px auto" wins over ".msg > :first-child" on specificity.
        const bool skipEdgeMargins
            = name == QLatin1String("img") && hasClass("math") && hasClass("display");
        if (!stack.isEmpty() && stack.last().isMsg && !stack.last().msgHasElementChild
                && !skipEdgeMargins)
            b.add(QStringLiteral("margin-top:0"));

        // Append the style attribute at the end of the tag (after any class),
        // remembering where its value ends so a later ":last-child" margin can
        // be appended in place.
        int styleAttrEnd = -1;
        const QString style = b.toString();
        QString newTag;
        if (!style.isEmpty()) {
            QString open = tagText;
            QString close = QStringLiteral(">");
            if (open.endsWith(QStringLiteral(" />"))) {
                open.chop(3);
                close = QStringLiteral(" />");
            } else {
                open.chop(1); // drop the '>'
            }
            const int valueStart = open.size() + 8; // after ' style="'
            open += QStringLiteral(" style=\"%1\"").arg(style);
            newTag = open + close;
            styleAttrEnd = out.size() + valueStart + style.size();
        } else {
            newTag = tagText;
        }
        out += newTag;

        if (!stack.isEmpty() && stack.last().isMsg)
            stack.last().msgHasElementChild = true;
        if (!kVoidTags.contains(name)) {
            Frame f;
            f.tag = name;
            f.isMsg = isMsg;
            f.skipEdgeMargins = skipEdgeMargins;
            f.styleAttrEnd = styleAttrEnd;
            stack.append(f);
        }
    }
    out += html.mid(lastEnd);
    return out;
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

// The bubble markup for a single message, without inlined styles.  The
// public entry points run the result through inlineStyles() exactly once
// (running it twice would leave the tag stack of the second pass desynced:
// the "style=" skip branch pushes no frame, while closing tags always pop).
static QString messageHtml(const Message &msg)
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

QString HtmlExporter::messageToHtml(const Message &msg)
{
    // Every bubble carries its styles inline as well as in the document's
    // <style> block, so a single message pasted into a WYSIWYG editor keeps
    // its look.
    const QString html = messageHtml(msg);
    return html.isEmpty() ? html : inlineStyles(html, themeColors());
}

QString HtmlExporter::conversationHtml(const QString &title, const QVector<Message> &messages)
{
    // Un‑inlined bubbles: the whole document goes through inlineStyles()
    // exactly once below (see messageHtml()).
    QString body;
    for (const Message &msg : std::as_const(messages))
        body += messageHtml(msg);

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

    const QString document = QStringLiteral(
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

    // Inline the stylesheet onto every element as well: WYSIWYG editors
    // that import pasted HTML drop <style> blocks but keep style attributes.
    return inlineStyles(document, themeColors());
}

} // namespace LlamaCpp
