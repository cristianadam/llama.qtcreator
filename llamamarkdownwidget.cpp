#include <QBuffer>
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QList>
#include <QMovie>
#include <QResizeEvent>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QToolTip>

#include <coreplugin/icore.h>

#include <3rdparty/md4c/src/md4c-html.h>
#include <3rdparty/md4c/src/md4c.h>
#include <coreplugin/editormanager/editormanager.h>
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

class HoverFilter : public QObject
{
    Q_OBJECT
public:
    explicit HoverFilter(QObject *parent = nullptr)
        : QObject(parent)
    {}
    bool eventFilter(QObject *obj, QEvent *event)
    {
        if (event->type() == QEvent::MouseMove) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            QTextEdit *te = qobject_cast<QTextEdit *>(obj->parent());
            if (!te)
                return QObject::eventFilter(obj, event);

            QTextCursor cur = te->cursorForPosition(me->pos());
            if (!cur.isNull()) {
                QTextCharFormat fmt = cur.charFormat();
                if (fmt.isAnchor()) {
                    QString url = fmt.anchorHref();
                    emit linkHovered(url);
                    return true; // we handled it
                }
            }
        }
        return QObject::eventFilter(obj, event);
    }

signals:
    void linkHovered(const QString &link);
};

MarkdownLabel::MarkdownLabel(QWidget *parent)
    : QTextBrowser(parent)
{
    setTextInteractionFlags(Qt::TextBrowserInteraction);

    setReadOnly(true);
    setOpenLinks(false);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);

    auto policy = QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(document(), &QTextDocument::contentsChanged, this, &MarkdownLabel::updateGeometry);

    viewport()->setMouseTracking(true);

    HoverFilter *hoverFilter = new HoverFilter(this);
    viewport()->installEventFilter(hoverFilter);

    QBuffer *buffer = new QBuffer(this);
    QMovie *spinnerMovie = new QMovie(buffer, "spinner", this);
    spinnerMovie->setScaledSize(QSize(14, 14));

    setMovie(spinnerMovie);

    connect(hoverFilter, &HoverFilter::linkHovered, this, [](const QString &link) {
        const int idx = link.indexOf(':');
        const QString command = link.left(idx);

        if (command == "copy")
            QToolTip::showText(QCursor::pos(), Tr::tr("Copy the code below to Clipboard"));
        else if (command == "save")
            QToolTip::showText(QCursor::pos(), Tr::tr("Save the code below into a file on disk"));
        else if (command == "details-toggle")
            QToolTip::showText(QCursor::pos(), Tr::tr("Toggle the details of the tool usage"));
        else
            QToolTip::showText(QCursor::pos(), link);
    });

    connect(this, &QTextBrowser::anchorClicked, this, [this](const QUrl &url) {
        const QString link = url.toString();
        const int idx = link.indexOf(':');
        const QString command = link.left(idx);
        const int argument = link.mid(idx + 1).toInt();

        if (command == "copy") {
            if (argument >= 0 && argument < m_data.codeBlocks.size())
                emit copyToClipboard(m_data.codeBlocks[argument].verbatimCode,
                                     m_data.codeBlocks[argument].hightlightedCode);
        } else if (command == "save") {
            if (argument >= 0 && argument < m_data.codeBlocks.size())
                emit saveToFile(m_data.codeBlocks[argument].fileName.value_or(QString()),
                                m_data.codeBlocks[argument].verbatimCode);
        } else if (command == "details-toggle") {
            toggleDetailsBlock(argument);
        } else {
            if (url.isLocalFile())
                Core::EditorManager::openEditor(FilePath::fromUrl(url));
            else
                QDesktopServices::openUrl(url);
        }
    });
}

void MarkdownLabel::setMarkdown(const QString &markdown)
{
    // We don't want something faster than 30fps, which would cause UI freezes
    if (m_markdownConversionTimer.isValid() && m_markdownConversionTimer.elapsed() < 33)
        return;

    m_markdownConversionTimer.start();
    auto guard = qScopeGuard([this] { m_markdownConversionTimer.restart(); });

    // Make sure the widget’s minimum width is large enough for the
    // longest line in the markdown.
    adjustMinimumWidth(markdown);

    setStyleSheet();

    // Render the markdown to a new `Data` instance.
    auto result = markdownToHtml(markdown);
    if (!result) { // error handling – keep widget in a sane state
        qWarning() << "Markdown rendering failed:" << result.error();
        return;
    }
    Data newData = std::move(result.value());

    // Update the document: delete obsolete sections and insert the new ones
    updateDocumentHtmlSections(newData);

    // Store the new data and finish.
    m_data = std::move(newData);

    document()->setTextWidth(viewport()->width());
    updateGeometry();
}

void MarkdownLabel::setStyleSheet()
{
    QString css = replaceThemeColorNamesWithRGBNames(R"##(
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
        table.codeblock {
          margin: 0px 0px;
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
          padding: 4px 4px;
          border: 1px solid Token_Foreground_Muted;
        }

        table th,
        table td {
          padding: 6px 6px;
          border: 1px solid Token_Foreground_Muted;
        }
        table.codeblock td {
          padding: 6px 6px;
          border: 1px solid Token_Foreground_Muted;
        }

        table tr {
          background-color: Token_Background_Default;
        }
        table.codeblock tr {
          background-color: Token_Background_Muted;
        }
        table.codeblock .copy-save-links {
          text-align: right;
          padding: 2px 12px;
        }

        a.heroicons {
          color: Token_Text_Default;
          font-family: heroicons_outline;
          font-size: medium;
        }

        a.details {
          color: Token_Text_Default;
        }

    )##");

    document()->setDefaultStyleSheet(css);
}

void MarkdownLabel::resizeEvent(QResizeEvent *event)
{
    QTextEdit::resizeEvent(event);                 // keep normal behaviour
    document()->setTextWidth(viewport()->width()); // re‑wrap at new width
    updateGeometry();                              // notify layout
}

int MarkdownLabel::heightForWidth(int w) const
{
    // Ask the document what height it needs for w px
    // (this does not change the widget’s real geometry)
    QTextDocument *doc = const_cast<QTextDocument *>(document());
    doc->setTextWidth(w);
    return qRound(doc->size().height() + 8);
}

void MarkdownLabel::invalidate()
{
    m_markdownConversionTimer.invalidate();
}

QSize MarkdownLabel::sizeHint() const
{
    QSize sh = QTextBrowser::sizeHint();
    sh.setWidth(minimumWidth());
    return sh;
}

void MarkdownLabel::setMovie(QMovie *movie)
{
    if (m_spinner == movie)
        return;

    if (m_spinner)
        disconnect(m_spinner, &QMovie::frameChanged, this, &MarkdownLabel::onSpinnerFrameChanged);

    m_spinner = movie;

    if (m_spinner) {
        connect(m_spinner,
                &QMovie::frameChanged,
                this,
                &MarkdownLabel::onSpinnerFrameChanged,
                Qt::QueuedConnection);
        // make sure the movie is started – it will also be started lazily
        // the first time loadResource() is called.
        if (m_spinner->state() != QMovie::Running)
            m_spinner->start();
    }
}

QVariant MarkdownLabel::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String("spinner")) {
        if (!m_spinner) {
            qWarning() << "MarkdownLabel: no movie set for spinner resource";
            return QVariant();
        }

        m_spinnerUrls.insert(name);

        // The movie already keeps the correct device pixel ratio.
        QImage frame = m_spinner->currentImage();
        if (frame.isNull()) {
            // The movie may not have produced a frame yet – force an update.
            m_spinner->jumpToNextFrame();
            frame = m_spinner->currentImage();
        }

        // Return a QImage – the layout will paint it directly.
        return frame;
    }

    // Default handling for everything else (e.g. normal file URLs).
    return QTextBrowser::loadResource(type, name);
}

void MarkdownLabel::onSpinnerFrameChanged(int)
{
    const QImage frame = m_spinner->currentImage();
    for (const QUrl &url : m_spinnerUrls)
        document()->addResource(QTextDocument::ImageResource, url, frame);

    viewport()->update();
}

void MarkdownLabel::adjustMinimumWidth(const QString &markdown)
{
    const QStringList lines = markdown.split('\n');
    const QString longestLine = *std::ranges::max_element(lines, std::less{}, &QString::length);
    QFontMetrics fm(font());
    const int longestLineWidth = fm.horizontalAdvance(longestLine) + 20;

    if (minimumWidth() == 0 || (lines.size() < 5 && minimumWidth() < longestLineWidth))
        setMinimumWidth(longestLineWidth);
}

int MarkdownLabel::commonPrefixLength(const QList<QByteArray> &a, const QList<QByteArray> &b) const
{
    const int n = std::min(a.size(), b.size());
    int i = 0;
    while (i < n && a[i] == b[i])
        ++i;
    return i;
}

void MarkdownLabel::removeHtmlSection(int index)
{
    QTextCursor cur(document());
    const auto &range = m_insertedHtmlSection[index];
    cur.setPosition(range.first);
    cur.setPosition(range.second, QTextCursor::KeepAnchor);
    cur.removeSelectedText();
}

void MarkdownLabel::insertHtmlSection(const QByteArray &html, int index)
{
    QTextCursor cur(document());
    cur.movePosition(QTextCursor::End);
    int start = cur.position();

    insertHtml(QString::fromUtf8(html));

    int end = cur.position();
    m_insertedHtmlSection[index] = {start, end};
}

void MarkdownLabel::updateDocumentHtmlSections(const Data &newData)
{
    const auto &oldSections = m_data.outputHtmlSections;
    const auto &newSections = newData.outputHtmlSections;
    const int common = commonPrefixLength(oldSections, newSections);

    // Delete sections that are no longer needed (from the end).
    for (int i = oldSections.size() - 1; i >= common; --i)
        removeHtmlSection(i);

    // Insert the new sections that appear after the common prefix.
    for (int i = common; i < newSections.size(); ++i)
        insertHtmlSection(newSections[i], i);
}

void MarkdownLabel::toggleDetailsBlock(int id)
{
    if (id < 0 || id >= m_data.detailsBlocks.size())
        return;

    auto &block = m_data.detailsBlocks[id];
    const QString newHtml = block.expanded ? block.collapsedHtml : block.expandedHtml;

    QTextCursor cur(document());

    // Remove the old section (if it already exists)
    if (block.range.first != -1) {
        cur.setPosition(block.range.first);
        cur.setPosition(block.range.second, QTextCursor::KeepAnchor);
        cur.removeSelectedText();
    }

    const int startPos = cur.position();
    insertHtml(newHtml);
    const int endPos = cur.position();

    block.range = {startPos, endPos};
    block.expanded = !block.expanded;
}

void MarkdownLabel::markdownHtmlCallback(const MD_CHAR *data, MD_SIZE length, void *user_data)
{
    Data *out = static_cast<Data *>(user_data);
    QByteArray line(data, length);

    auto createLink = [](const QString &name, auto indexOrId, const QString &label) -> QByteArray {
        QString href = QString("%1:%2").arg(name).arg(indexOrId);

        // Choose the image based on the command name
        QString heroIconText;
        if (name == "copy") {
            heroIconText = "E";
        } else if (name == "save") {
            heroIconText = "F";
        }

        return QString("<a class=\"heroicons\" href=\"%1\">%2</a>")
            .arg(href)
            .arg(heroIconText)
            .toUtf8();
    };

    auto toVerbatimText = [](const QByteArray &line) -> QString {
        QString newLine = QString::fromUtf8(line);
        newLine.replace("\n", "<br>");
        newLine.replace(" ", "&nbsp;");

        return QTextDocumentFragment::fromHtml(newLine).toPlainText();
    };

    auto insertSourceFileCopySave = [&]() {
        out->outputHtml.append("<tr>");
        out->outputHtml.append(
            "<th class=\"copy-save-links\"><span style=\"font-size:small\">"
            + out->codeBlocks.last().fileName.value().toUtf8() + "</span>" + "&nbsp;&nbsp;"
            + createLink("copy", out->codeBlocks.size() - 1, Tr::tr("Copy")) + "&nbsp;&nbsp;"
            + createLink("save", out->codeBlocks.size() - 1, Tr::tr("Save")) + "</th>");
        out->outputHtml.append("</tr>\n");
        out->outputHtml.append("<tr><td>\n");
    };

    auto insertCopySave = [&]() {
        out->outputHtml.append("<tr>");
        out->outputHtml.append(
            "<th class=\"copy-save-links\">"
            + createLink("copy", out->codeBlocks.size() - 1, Tr::tr("Copy")) + "&nbsp;&nbsp;"
            + createLink("save", out->codeBlocks.size() - 1, Tr::tr("Save")) + "</th>");
        out->outputHtml.append("</tr>\n");
        out->outputHtml.append("<tr><td>\n");
    };

    auto processOneLine = [&]() {
        QString verbatimLine = toVerbatimText(line);
        out->codeBlocks.last().verbatimCode.append(verbatimLine);

        if (out->awaitingNewLine) {
            out->codeBlocks.last().hightlightedCode.append("<br>");
            out->outputHtml.append("<br>");
            out->awaitingNewLine = false;
        }

        QString highlightedLine = out->highlighter->hightlightCodeLine(verbatimLine);
        auto newLineIndex = highlightedLine.lastIndexOf("<br>");
        if (newLineIndex != -1) {
            out->awaitingNewLine = true;
            highlightedLine = highlightedLine.left(newLineIndex);
        }

        out->codeBlocks.last().hightlightedCode.append(highlightedLine);
        out->outputHtml.append(highlightedLine.toUtf8());
    };

    static const QRegularExpression detailsRegex(R"(^<details><summary>(.+?)</summary>$)",
                                                 QRegularExpression::DotMatchesEverythingOption);
    auto detailsMatch = detailsRegex.match(QString::fromUtf8(line));
    if (detailsMatch.hasMatch() && out->state == Data::NormalHtml) {
        out->state = Data::DetailsContent;
        MarkdownLabel::DetailsBlock db;
        db.summary = detailsMatch.captured(1).trimmed();
        out->detailsBlocks.append(db);
        out->detailsBuffer.clear();

        return;
    }
    if (out->state == Data::DetailsContent && line == "</details>") {
        Data innerOut;
        innerOut.codeBlocks = out->codeBlocks;
        for (const QByteArray &innerLine : out->detailsBuffer) {
            markdownHtmlCallback(innerLine.data(),
                                 innerLine.size(),
                                 reinterpret_cast<void *>(&innerOut));
        }
        if (!innerOut.outputHtml.isEmpty())
            innerOut.outputHtmlSections << innerOut.outputHtml;
        out->codeBlocks = innerOut.codeBlocks;

        QByteArray innerHtml = innerOut.outputHtmlSections.join("");

        // Build the two HTML fragments we will toggle between
        const int id = out->detailsBlocks.size() - 1; // unique id for this block
        const QString arrowDown = "<span style=\"font-family: heroicons_outline\">N</span>";
        const QString arrowUp = "<span style=\"font-family: heroicons_outline\">M</span>";

        const QString collapsed = QString("<a href=\"details-toggle:%1\" class=\"details\">"
                                          "%2&nbsp;%3</a>")
                                      .arg(id)
                                      .arg(out->detailsBlocks.last().summary)
                                      .arg(arrowDown);

        const QString expanded = QString("<a href=\"details-toggle:%1\" class=\"details\">"
                                         "%2&nbsp;%3</a>"
                                         "<div>%4</div>")
                                     .arg(id)
                                     .arg(out->detailsBlocks.last().summary)
                                     .arg(arrowUp)
                                     .arg(innerHtml);

        // store them
        out->detailsBlocks[id].collapsedHtml = collapsed;
        out->detailsBlocks[id].expandedHtml = expanded;
        out->detailsBlocks[id].expanded = false;

        // Insert the *collapsed* version into the document
        out->outputHtml.append(collapsed.toUtf8());
        out->outputHtmlSections << out->outputHtml;
        out->outputHtml.clear();

        // reset state – we are back to normal HTML parsing
        out->state = Data::NormalHtml;
        out->detailsBuffer.clear();
        return;
    }

    if (out->state == Data::DetailsContent) {
        out->detailsBuffer.append(line);
        return;
    }

    // Break the output into logical sections, this way we could cache some of the output
    // in the QTextBrowser's document
    if (line == "<h1>" || line == "<h2>" || line == "<h3>" || line == "<h4>" || line == "<h5>"
        || line == "<h6>" || line == "<br>\n") {
        if (!out->outputHtml.isEmpty()) {
            out->outputHtml.append("<br>");
            out->outputHtmlSections << out->outputHtml;
            out->outputHtml.clear();
        }
    }

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
        out->codeBlocks.last().language = QString::fromUtf8(line);
        out->highlighter->setDefinition(definitionForName(out->codeBlocks.last().language));
    } else if (line == "\"" && out->state == Data::LanguageName) {
        out->state = Data::PreCodeEndQuote;
    } else if (line == ">" && out->state == Data::PreCodeEndQuote) {
        out->state = Data::PreCodeEndTag;
    } else if (line == ">" && out->state == Data::PreCode) {
        out->state = Data::PreCodeEndTag;
    } else if (line == "</code></pre>\n") {
        out->state = Data::NormalHtml;
        out->awaitingNewLine = false;
        out->outputHtml.append("</td></tr></table>\n");
    } else if (out->state == Data::PreCodeEndTag) {
        out->state = Data::Code;
        out->outputHtml.append("<table class=\"codeblock\">\n");

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
        } else if (out->codeBlocks.last().language == "cmake") {
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

    out->outputHtml.append(data, length);
}

Utils::expected<MarkdownLabel::Data, QString> MarkdownLabel::markdownToHtml(const QString &markdown)
{
    if (markdown.isEmpty())
        return {};

    // md4c expects UTF‑8 data
    QByteArray md = markdown.toUtf8();

    Data out;
    out.outputHtml.reserve(md.size() * 4); // heuristic

    // Render Markdown to HTML
    int rc = md_html(reinterpret_cast<const MD_CHAR *>(md.constData()),
                     static_cast<MD_SIZE>(md.size()),
                     markdownHtmlCallback,
                     reinterpret_cast<void *>(&out),
                     MD_DIALECT_GITHUB,
                     0);

    if (rc != 0)
        return Utils::make_unexpected(QString("md4c failed to render"));

    if (!out.outputHtml.isEmpty()) {
        out.outputHtmlSections << out.outputHtml;
        out.outputHtml.clear();
    }

    return out;
}
} // namespace LlamaCpp

#include "llamamarkdownwidget.moc"
