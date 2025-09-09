#include <QClipboard>
#include <QDebug>
#include <QFile>
#include <QList>
#include <QResizeEvent>
#include <QTextDocumentFragment>
#include <QToolTip>

#include <coreplugin/icore.h>

#include <3rdparty/md4c/src/md4c-html.h>
#include <3rdparty/md4c/src/md4c.h>
#include <repository.h>

#include "llamahtmlhighlighter.h"
#include "llamamarkdownwidget.h"
#include "llamatheme.h"
#include "llamatr.h"

using namespace TextEditor;
using namespace Utils;

namespace LlamaCpp {

static KSyntaxHighlighting::Repository *highlightRepository()
{
    static KSyntaxHighlighting::Repository *repository = nullptr;
    if (!repository) {
        repository = new KSyntaxHighlighting::Repository();
        const FilePath dir = Core::ICore::resourcePath("generic-highlighter/syntax");
        if (dir.exists())
            repository->addCustomSearchPath(dir.parentDir().path());
        const FilePath userDir = Core::ICore::userResourcePath("generic-highlighter");
        if (userDir.exists())
            repository->addCustomSearchPath(userDir.path());
    }
    return repository;
}

static KSyntaxHighlighting::Definition definitionForName(const QString &name)
{
    return highlightRepository()->definitionForName(name);
}

MarkdownLabel::MarkdownLabel(QWidget *parent)
    : QLabel(parent)
{
    setTextInteractionFlags(Qt::TextBrowserInteraction);

    connect(this, &QLabel::linkHovered, this, [](const QString &link) {
        auto idx = link.indexOf(":");
        QString command = link.left(idx);

        if (command == "copy")
            QToolTip::showText(QCursor::pos(), Tr::tr("Copy the code below to Clipboard"));
        else if (command == "save")
            QToolTip::showText(QCursor::pos(), Tr::tr("Save the code below into a file on disk"));
    });
    connect(this, &QLabel::linkActivated, this, [this](const QString &link) {
        auto idx = link.indexOf(":");
        QString command = link.left(idx);
        int codeBlockIndex = link.mid(idx + 1).toInt();

        if (command == "copy") {
            if (codeBlockIndex >= 0 && codeBlockIndex < m_data.codeBlocks.size())
                emit copyToClipboard(m_data.codeBlocks[codeBlockIndex].verbatimCode,
                                     m_data.codeBlocks[codeBlockIndex].hightlightedCode);
        } else if (command == "save") {
            if (codeBlockIndex >= 0 && codeBlockIndex < m_data.codeBlocks.size())
                emit saveToFile(m_data.codeBlocks[codeBlockIndex].fileName.value_or(QString()),
                                m_data.codeBlocks[codeBlockIndex].verbatimCode);
        }
    });
}

void MarkdownLabel::setMarkdown(const QString &markdown)
{
    const QStringList lines = markdown.split("\n");
    const QString longestLine = *std::ranges::max_element(lines, std::less{}, &QString::length);
    QFontMetrics fm(font());
    const int longestLineWidth = fm.horizontalAdvance(longestLine) + 21;

    if (minimumWidth() == 0 || lines.size() < 5 && minimumWidth() < longestLineWidth)
        setMinimumWidth(fm.horizontalAdvance(longestLine) + 21);

    auto html = markdownToHtml(markdown);
    if (html) {
        setStyleSheet();

        // Inject the optional CSS before the </head> tag
        if (!m_css.isEmpty()) {
            const QByteArray headEnd = "</head>";
            int pos = html.value().indexOf(headEnd);
            if (pos != -1)
                html.value().insert(pos, QByteArray("\n<style>" + m_css + "</style>"));
            else
                html.value().prepend(QByteArray("<style>" + m_css + "</style>"));
        }

        setText(QString::fromUtf8(html.value()));
        setTextFormat(Qt::RichText);

        // emit rendered(html);
        // QFile htmlFile("rendered.html");
        // if (htmlFile.open(QFile::ReadWrite)) {
        //     htmlFile.write(html.value().toUtf8());
        // }
    } else {
        qWarning() << "Markdown conversion failed:" << html.error();
    }
}

void MarkdownLabel::setStyleSheet()
{
    if (!m_css.isEmpty())
        return;

    m_css = replaceThemeColorNamesWithRGBNames(R"##(
        hr {
          margin: 10px 0;
          background-color: Token_Foreground_Muted;
        }

        p {
          margin: 10px 0;
        }

        blockquote {
          margin: 0;
          color: Token_Text_Muted;
        }

        a {
          text-decoration: none;
        }

        a:hover {
          text-decoration: underline;
        }

        h1 code {
          font-size: large;
        }
        h2 code {
          font-size: large;
        }
        h3 code {
          font-size: large;
        }

        code {
          font-size: medium;
        }

        table {
            margin: 10px 0;
            width: 100%;
            border-collapse: collapse;
        }

        table th {
          background-color: Token_Background_Muted;
          padding: 6px 6px;
        }
        table.codeblock th {
          background-color: Token_Background_Default;
          font-weight: 400;
          padding: 0px 0px;
        }

        table th,
        table td {
          padding: 6px 6px;
          border: 1px solid Token_Foreground_Muted;
        }
        table.codeblock td {
          padding: 6px 6px;
        }

        table tr {
          background-color: Token_Background_Default;
        }
        table.codeblock tr {
          background-color: Token_Background_Muted;
        }

    )##")
                .toUtf8();
}

void MarkdownLabel::paintEvent(QPaintEvent *ev)
{
    return QLabel::paintEvent(ev);
}

Utils::expected<QByteArray, QString> MarkdownLabel::markdownToHtml(const QString &markdown)
{
    if (markdown.isEmpty())
        return {};

    // md4c expects UTF‑8 data
    QByteArray md = markdown.toUtf8();

    m_data = {};
    m_data.output_html.reserve(md.size() * 4); // heuristic

    // md4c output callback
    auto append_cb = [](const MD_CHAR *data, MD_SIZE length, void *user_data) -> void {
        Data *out = static_cast<Data *>(user_data);
        QByteArray line(data, length);

        auto createLink =
            [](const QString &name, qsizetype index, const QString &label) -> QByteArray {
            return QString("<a href=\"%1:%2\">%3</a>").arg(name).arg(index).arg(label).toUtf8();
        };

        auto toVerbatimText = [](const QByteArray &line) -> QString {
            QString newLine = QString::fromUtf8(line);
            newLine.replace("\n", "<br>");
            newLine.replace(" ", "&nbsp;");

            return QTextDocumentFragment::fromHtml(newLine).toPlainText();
        };

        auto insertSourceFileCopySave = [&]() {
            out->output_html.append("<tr>");
            out->output_html.append(
                "<th>" + out->codeBlocks.last().fileName.value().toUtf8() + "</th><th>"
                + createLink("copy", out->codeBlocks.size() - 1, Tr::tr("Copy")) + "</th><th>"
                + createLink("save", out->codeBlocks.size() - 1, Tr::tr("Save")) + "</th>");
            out->output_html.append("</tr>\n");
            out->output_html.append("<tr><td colspan=\"3\">\n");
        };

        auto insertCopySave = [&]() {
            out->output_html.append("<tr>");
            out->output_html.append(
                "<th>" + createLink("copy", out->codeBlocks.size() - 1, Tr::tr("Copy")) + "</th><th>"
                + createLink("save", out->codeBlocks.size() - 1, Tr::tr("Save")) + "</th>");
            out->output_html.append("</tr>\n");
            out->output_html.append("<tr><td colspan=\"2\">\n");
        };

        auto processOneLine = [&]() {
            QString verbatimLine = toVerbatimText(line);
            out->codeBlocks.last().verbatimCode.append(verbatimLine);

            if (out->awaitingNewLine) {
                out->codeBlocks.last().hightlightedCode.append("<br>");
                out->output_html.append("<br>");
                out->awaitingNewLine = false;
            }

            QString highlightedLine = out->highlighter->hightlightCodeLine(verbatimLine);
            auto newLineIndex = highlightedLine.lastIndexOf("<br>");
            if (newLineIndex != -1) {
                out->awaitingNewLine = true;
                highlightedLine = highlightedLine.left(newLineIndex);
            }

            out->codeBlocks.last().hightlightedCode.append(highlightedLine);
            out->output_html.append(highlightedLine.toUtf8());
        };

        if (line == "<pre><code" && out->state == Data::NormalHtml) {
            out->state = Data::PreCode;
            CodeBlock c;
            out->codeBlocks.append(c);
            out->awaitingNewLine = false;

            out->highlighter.reset(new HtmlHighlighter());
            // The generic definition needs to be loaded
            out->highlighter->setDefinition({});
        } else if (line == " class=\"language-" && out->state == Data::PreCode) {
            out->state = Data::Class;
        } else if (out->state == Data::Class) {
            out->state = Data::LanguageName;
            out->codeBlocks.last().language = QString::fromLatin1(line);

            out->highlighter->setDefinition(definitionForName(QString::fromLatin1(line)));
        } else if (line == "\"" && out->state == Data::LanguageName) {
            out->state = Data::PreCodeEndQuote;
        } else if (line == ">" && out->state == Data::PreCodeEndQuote) {
            out->state = Data::PreCodeEndTag;
        } else if (line == ">" && out->state == Data::PreCode) {
            out->state = Data::PreCodeEndTag;
        } else if (line == "</code></pre>\n") {
            out->state = Data::NormalHtml;
            out->awaitingNewLine = false;
            out->output_html.append("</td></tr></table>\n");
        } else if (out->state == Data::PreCodeEndTag) {
            out->state = Data::Code;
            out->output_html.append("<table class=\"codeblock\">\n");

            static const QRegularExpression
                cxxAndBashFileNameRegex(R"(^\s*(?:\/\/|#)\s*([a-zA-Z0-9_]+\.[a-zA-Z0-9]+).*$)",
                                        QRegularExpression::NoPatternOption);
            static const QRegularExpression
                cFileNameRegex(R"(^\s*/\*\s*([a-zA-Z0-9_]+\.[a-zA-Z0-9]+).*$)",
                               QRegularExpression::NoPatternOption);
            for (const auto &regex : {cxxAndBashFileNameRegex, cFileNameRegex}) {
                auto sourceFileMatch = regex.match(QString::fromUtf8(line));
                if (sourceFileMatch.hasMatch()) {
                    out->codeBlocks.last().fileName = sourceFileMatch.captured(1);
                    break;
                }
            }

            if (out->codeBlocks.last().fileName.has_value()) {
                insertSourceFileCopySave();

                out->state = Data::SourceFile;
                // skip the comment with the line
                return;
            } else if (out->codeBlocks.last().language.value_or("") == "cmake") {
                out->codeBlocks.last().fileName = "CMakeLists.txt";
                insertSourceFileCopySave();
            } else {
                insertCopySave();
            }

            processOneLine();
            return;
        } else if (out->state == Data::SourceFile) {
            if (line == "\n")
                // skip the empty line(s) after the source filename comment
                return;

            out->state = Data::Code;
            processOneLine();
            return;
        } else if (out->state == Data::Code) {
            processOneLine();
            return;
        }

        out->output_html.append(data, length);
    };

    // Render Markdown to HTML
    int rc = md_html(reinterpret_cast<const MD_CHAR *>(md.constData()),
                     static_cast<MD_SIZE>(md.size()),
                     append_cb,
                     reinterpret_cast<void *>(&m_data),
                     MD_DIALECT_GITHUB,
                     0);

    if (rc != 0)
        return Utils::make_unexpected(QString("md4c failed to render"));

    return m_data.output_html;
}
} // namespace LlamaCpp
