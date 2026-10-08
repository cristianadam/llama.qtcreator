#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QEventLoop>
#include <QTextImageFormat>
#include <QTimer>
#include <QTemporaryDir>
#include <QtSvg/QSvgRenderer>
#include <QtTest/QtTest>

#include <QSettings>

#include <utils/theme/theme.h>

#include "ansitext.h"
#include <markdownrenderer.h>

using namespace LlamaCpp;

// Feed \a full text into the renderer incrementally (as the LLM streams a
// reply) and verify the finished document contains the expected lines.
static QString streamText(MarkdownRenderer &renderer, const QString &full, int step = 7)
{
    for (int i = 0; i < full.size(); i += step)
        renderer.feed(full.left(i + step).toUtf8());
    renderer.feed(full.toUtf8());
    renderer.finish();

    return renderer.toPlainText();
}

static bool containsAll(const QString &text, const QStringList &lines)
{
    for (const QString &line : lines)
        if (!text.contains(line))
            return false;
    return true;
}

// Declared below (after the test class); used by the image tests.
static QList<QTextImageFormat> imageFormats(MarkdownRenderer &renderer);
static QString firstImageUrl(MarkdownRenderer &renderer);

// Mermaid diagrams render asynchronously on the engine's worker thread:
// after a finalized block is rendered, its placeholder block carries
// MermaidPendingKeyProp until the queued completion callback (which swaps the
// spinner for the picture, or removes it on failure) has run. Pump the event
// loop until no pending block remains, or report a timeout.
static bool waitForPendingMermaid(MarkdownRenderer &renderer, int timeoutMs = 60'000)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        bool pending = false;
        for (QTextBlock blk = renderer.document()->firstBlock(); blk.isValid();
             blk = blk.next()) {
            if (!blk.blockFormat()
                     .property(MarkdownRenderer::MermaidPendingKeyProp)
                     .toString()
                     .isEmpty())
                pending = true;
        }
        if (!pending)
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    }
    return false;
}


// Standalone test: no Qt Creator theme is installed, so install a minimal
// one and override the terminal ANSI tokens the tests assert on.
static void setTerminalAnsiColor(int index, const QColor &color)
{
    static Utils::Theme *theme = nullptr;
    static QTemporaryDir dir;
    if (!theme) {
        struct TestTheme : Utils::Theme
        {
            TestTheme() : Utils::Theme(QStringLiteral("test")) {}
        };
        theme = new TestTheme;
        Utils::setCreatorTheme(theme);
    }
    QSettings settings(dir.path() + QStringLiteral("/theme.ini"), QSettings::IniFormat);
    settings.setValue(QStringLiteral("Colors/TerminalAnsi%1").arg(index), color.name().mid(1)); // no leading "#": Theme::readSettings prepends it
    settings.sync();
    theme->readSettings(settings);
}

class MarkdownRendererTest : public QObject
{
    Q_OBJECT
private slots:
    void plainParagraph();
    void multiParagraph();
    void codeBlock();
    void terminalBlockDecodesAnsiColors();
    void ansiDecodeAndStrip();
    void list();
    void thinkingSection();
    void detailsSummaryMarkdown();
    void detailsSummaryInlineHtml();
    void toolCallCollapsedByDataToolAttribute();
    void toolCallSummaryWithOutputPreview();
    void collapsedSectionStaysCollapsed();
    void svgCodeBlockRendersAsImage();
    void dataUrlImageRendersAsImage();
    void scaledImageForDisplay();
    void svgCodeBlockWithoutLanguageTag();
    void brokenSvgFallsBackToCode();
    void mermaidCodeBlockRendersAsImage();
    void invalidMermaidFallsBackToCode();
    void inlineMathRendersAsImage();
    void displayMathRendersAsImage();
    void radicalSvgHasNoClipPath();
    void radicalHookClearsRadicand();
    void invalidMathShowsSource();
    void plainDollarsStayText();
    void mermaidServedFromPersistedCache();
    void mathServedFromPersistedCache();
    void finishFinalizesPendingBlocks();
};

static QString rectStr(const QRectF &r)
{
    return QStringLiteral("[%1, %2 %3x%4]")
        .arg(r.x(), 0, 'g', 6)
        .arg(r.y(), 0, 'g', 6)
        .arg(r.width(), 0, 'g', 6)
        .arg(r.height(), 0, 'g', 6);
}

// finish() Flush()es the streaming parser: whatever is pending becomes a
// stable block, and the next feed() starts a *new* block after it. So a
// message that is still streaming must never be finished mid-stream —
// rendering the first chunk with feed()+finish() and streaming the rest
// splits the first token into its own paragraph (this is how the ChatMessage
// ctor used to break the first line of every streamed reply).
void MarkdownRendererTest::finishFinalizesPendingBlocks()
{
    const QString full
        = QStringLiteral("I can use the **bash** tool to find out the date — "
                         "for example, by running the `date` command in the shell.");

    auto blockCount = [](MarkdownRenderer &r) {
        int n = 0;
        for (QTextBlock b = r.document()->firstBlock(); b.isValid(); b = b.next())
            ++n;
        return n;
    };

    // Correct streaming pattern: feed the growing buffer, finish once at the
    // end — one paragraph.
    {
        MarkdownRenderer renderer;
        renderer.feed(full.left(1).toUtf8());
        renderer.feed(full.left(4).toUtf8());
        renderer.feed(full.toUtf8());
        renderer.finish();
        QCOMPARE(blockCount(renderer), 1);
    }

    // The pitfall: finishing after the first chunk freezes it as a stable
    // block; the remainder streams in as a second paragraph.
    {
        MarkdownRenderer renderer;
        renderer.feed(full.left(1).toUtf8());
        renderer.finish();
        renderer.feed(full.left(4).toUtf8());
        renderer.feed(full.toUtf8());
        renderer.finish();
        QCOMPARE(blockCount(renderer), 2);
        QCOMPARE(renderer.document()->firstBlock().text(), QStringLiteral("I"));
    }
}

// The math renders synchronously (KaTeX is fast); find the first image URL
// in the document, if any.
static QList<QTextImageFormat> imageFormats(MarkdownRenderer &renderer)
{
    QList<QTextImageFormat> formats;
    QTextCursor cursor(renderer.document());
    // Skip the last character (paragraph separator): it can never be an
    // image, and selecting past it is out of range.
    for (int p = 0; p + 1 < renderer.document()->characterCount(); ++p) {
        // Select the character: a collapsed cursor's charFormat() reports
        // the *preceding* character's format (except at a block's start).
        cursor.setPosition(p);
        cursor.setPosition(p + 1, QTextCursor::KeepAnchor);
        if (const QTextCharFormat cf = cursor.charFormat(); cf.isImageFormat())
            formats.append(cf.toImageFormat());
    }
    return formats;
}

static QString firstImageUrl(MarkdownRenderer &renderer)
{
    const QList<QTextImageFormat> formats = imageFormats(renderer);
    return formats.isEmpty() ? QString() : formats.first().name();
}

static double imageFormatHeight(MarkdownRenderer &renderer, const QString &url)
{
    for (int p = 0; p < renderer.document()->characterCount(); ++p) {
        QTextCursor cursor(renderer.document());
        cursor.setPosition(p);
        const QTextCharFormat cf = cursor.charFormat();
        if (cf.isImageFormat() && cf.toImageFormat().name() == url)
            return cf.toImageFormat().height();
    }
    return -1;
}

void MarkdownRendererTest::plainParagraph()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString out = streamText(renderer, "Hello world, this is a reply.");
    QCOMPARE(out.trimmed(), QStringLiteral("Hello world, this is a reply."));
}

void MarkdownRendererTest::multiParagraph()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text = "First paragraph.\n\nSecond paragraph with **bold** and `code`.\n\n- item one\n- item two\n";
    const QString out = streamText(renderer, text);
    QVERIFY2(containsAll(out,
                         {
                             QStringLiteral("First paragraph."),
                             QStringLiteral("Second paragraph with bold and code."),
                             QStringLiteral("item one"),
                             QStringLiteral("item two"),
                         }),
              qPrintable(out));
}

void MarkdownRendererTest::codeBlock()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text = "Here is some code:\n\n```cpp\nint main() {\n    return 0;\n}\n```\n\nDone.\n";
    const QString out = streamText(renderer, text);
    QVERIFY2(containsAll(out,
                         {
                             QStringLiteral("Here is some code:"),
                             QStringLiteral("int main() {"),
                             QStringLiteral("return 0;"),
                             QStringLiteral("Done."),
                         }),
              qPrintable(out));
}

void MarkdownRendererTest::list()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text = "1. first\n2. second\n3. third\n";
    const QString out = streamText(renderer, text);
    QVERIFY2(containsAll(out, {QStringLiteral("first"),
                               QStringLiteral("second"),
                               QStringLiteral("third")}),
             qPrintable(out));
}

void MarkdownRendererTest::terminalBlockDecodesAnsiColors()
{
    setTerminalAnsiColor(1, QColor(200, 30, 30));
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text = QStringLiteral("```terminal\n$ ls\n\x1b[0;31mmissing.txt\x1b[0m\n```\n");
    const QString out = streamText(renderer, text);
    QVERIFY2(out.contains(QStringLiteral("missing.txt")), qPrintable(out));
    QVERIFY(!out.contains(QChar(0x1B)));

    // The colored run carries the terminal theme's red.
    bool found = false;
    for (QTextBlock blk = renderer.document()->firstBlock(); blk.isValid() && !found;
         blk = blk.next()) {
        for (QTextBlock::Iterator it = blk.begin(); it != blk.end(); ++it) {
            const QTextFragment &frag = it.fragment();
            if (frag.text() == QLatin1String("missing.txt")
                && frag.charFormat().foreground().color() == QColor(200, 30, 30))
                found = true;
        }
    }
    QVERIFY2(found, qPrintable(out));
}

void MarkdownRendererTest::ansiDecodeAndStrip()
{
    QCOMPARE(stripAnsiSequences(QStringLiteral("\x1b[31mred\x1b[0m plain")),
             QStringLiteral("red plain"));
    QCOMPARE(stripAnsiSequences(QStringLiteral("\x1b]0;title\x07text\x1b[K")),
             QStringLiteral("text"));
    QCOMPARE(stripAnsiSequences(QStringLiteral("no escapes")),
             QStringLiteral("no escapes"));

    setTerminalAnsiColor(1, QColor(200, 30, 30));
    const QVector<HighlightFragment> frags = decodeAnsiText(
            QStringLiteral("a\x1b[1;31mb\x1b[0m\x1b[38;5;196mc\x1b[38;2;1;2;3md"),
            QTextCharFormat());
    QString joined;
    for (const HighlightFragment &f : std::as_const(frags))
        joined += f.text;
    QCOMPARE(joined, QStringLiteral("abcd"));
    QCOMPARE(frags.size(), 4);

    QCOMPARE(frags.at(1).text, QStringLiteral("b"));
    QCOMPARE(frags.at(1).format.foreground().color(), QColor(200, 30, 30));
    QCOMPARE(frags.at(1).format.fontWeight(), int(QFont::Bold));
    QCOMPARE(frags.at(2).format.foreground().color(), QColor(255, 0, 0)); // 256-color 196
    QCOMPARE(frags.at(3).format.foreground().color(), QColor(1, 2, 3)); // truecolor

    // An incomplete sequence at the end (the block is still streaming) is
    // dropped; the next render carries the complete sequence.
    const QVector<HighlightFragment> partial =
        decodeAnsiText(QStringLiteral("x\x1b[3"), QTextCharFormat());
    QString partialJoined;
    for (const HighlightFragment &f : std::as_const(partial))
        partialJoined += f.text;
    QCOMPARE(partialJoined, QStringLiteral("x"));
}

void MarkdownRendererTest::thinkingSection()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);

    // Simulate a streamed reply with an open thinking section (as produced by
    // ThinkingSectionParser::replaceThinkingSections while streaming), then the
    // final form once completed.
    const QString thinking = "Let me think...";
    const QString answer = "The answer is 42.";
    const QString open = QStringLiteral("<details><summary>Thinking</summary>\n\n%1\n</details>\n")
                             .arg(thinking);
    const QString closed
        = QStringLiteral("<details><summary>Thought Process</summary>\n\n%1\n</details>\n\n%2\n")
              .arg(thinking, answer);

    for (int i = 0; i < open.size(); i += 7)
        renderer.feed(open.left(i + 7).toUtf8());
    renderer.feed(open.toUtf8());

    const QString out = streamText(renderer, closed);
    QVERIFY2(out.contains(answer), qPrintable(out));
}

void MarkdownRendererTest::detailsSummaryMarkdown()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // The <summary> is markdown (a heading with emphasis + code), not plain text.
    const QString text
        = "<details>\n<summary>\n\n#### Markdown *in* `summary`\n\n"
          "</summary>\n\nHi.\n\n</details>\n";
    const QString out = streamText(renderer, text);
    QVERIFY2(containsAll(out,
                          {
                              QStringLiteral("Markdown"),
                              QStringLiteral("summary"),
                              QStringLiteral("Hi."),
                          }),
              qPrintable(out));
    // The markdown must have been interpreted, not shown verbatim.
    QVERIFY(!out.contains(QLatin1String("####")));
    QVERIFY(!out.contains(QLatin1Char('`')));

    // The header block must carry the properties mousePressEvent relies on to
    // expand/collapse (a regression: the icon insert used to reset them), plus
    // the plain-text summary.
    QTextDocument *doc = renderer.document();
    const QTextBlockFormat headerFmt = doc->firstBlock().blockFormat();
    const int secId
        = headerFmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt();
    QVERIFY2(headerFmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool(),
             "header block must be a clickable toggle block");
    QVERIFY2(secId > 0, "header block must have a section id");
    QCOMPARE(headerFmt.property(MarkdownRenderer::DetailsSummaryTextProp).toString(),
             QStringLiteral("Markdown in summary"));

    // The body carries the same section id but is NOT a toggle (clickable) block.
    bool bodyFound = false;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid(); blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        if (fmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt() != secId)
            continue;
        if (!fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool())
            bodyFound = true;
    }
    QVERIFY2(bodyFound, "a body block must carry the section id");
}

void MarkdownRendererTest::detailsSummaryInlineHtml()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // Tool-result style header: a status icon (an inline-HTML span whose tag
    // markus splits across nodes) followed by a markdown one-liner.
    const QString text
        = "<details>\n<summary><span style=\"font-family: heroicons_outline;\">R</span>&nbsp;running python</summary>\n\nexit 0\n</details>\n";
    renderer.feed(text.toUtf8());
    renderer.finish();

    // The icon span must survive as HTML: Qt drops a tag that is not closed
    // within a single insertHtml() call, so a split <span>R</span> would be lost.
    QVERIFY2(renderer.toHtml().contains(QLatin1String("heroicons_outline")),
             qPrintable(renderer.toHtml()));
    QVERIFY2(renderer.toPlainText().contains(QStringLiteral("running python")),
             qPrintable(renderer.toPlainText()));
    QVERIFY2(renderer.toPlainText().contains(QStringLiteral("exit 0")),
             qPrintable(renderer.toPlainText()));

    // The icon font must be scoped to the icon glyph only. insertHtml() leaves
    // the cursor's char format set to the last inserted character, so without a
    // restore the following summary text would be tinted with the icon font.
    bool textInIconFont = false;
    for (auto it = renderer.document()->firstBlock().begin();
         it != renderer.document()->firstBlock().end(); ++it) {
        const QTextFragment frag = it.fragment();
        if (frag.text().contains(QStringLiteral("running"))
            && frag.charFormat().fontFamilies().toStringList()
                   .contains(QLatin1String("heroicons_outline")))
            textInIconFont = true;
    }
    QVERIFY2(!textInIconFont, "summary text must not inherit the icon font");
}

void MarkdownRendererTest::toolCallCollapsedByDataToolAttribute()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);

    // Tool calls are marked with a data-tool attribute on the <details> tag
    // (no zero-width-space workaround in the summary) and are collapsed by
    // default; plain details sections stay expanded.
    const QString text
        = "<details data-tool=\"true\"><summary>running mkdir -p out</summary>\n\n"
          "tool body\n</details>\n\n"
          "<details><summary>Thought Process</summary>\n\nother body\n</details>\n";
    renderer.feed(text.toUtf8());
    renderer.finish();

    QTextDocument *doc = renderer.document();
    bool toolHeaderFound = false;
    bool toolBodyHidden = false;
    bool otherBodyVisible = false;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid(); blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        // The header also carries the expand/collapse icon glyph at the end.
        if (blk.text().startsWith(QStringLiteral("running mkdir -p out"))) {
            toolHeaderFound = true;
            // The stored summary text must be the plain text, without any
            // zero-width space prefix.
            QCOMPARE(fmt.property(MarkdownRenderer::DetailsSummaryTextProp).toString(),
                     QStringLiteral("running mkdir -p out"));
        }
        if (blk.text() == QStringLiteral("tool body"))
            toolBodyHidden = !blk.isVisible();
        if (blk.text() == QStringLiteral("other body"))
            otherBodyVisible = blk.isVisible();
    }
    QVERIFY2(toolHeaderFound, "tool call header must be present");
    QVERIFY2(toolBodyHidden, "tool call body must be collapsed by default");
    QVERIFY2(otherBodyVisible, "plain details body must be expanded by default");
}

void MarkdownRendererTest::toolCallSummaryWithOutputPreview()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);

    // Finished tool calls carry a few lines of output in the summary itself
    // (one-line description, blank line, fenced preview), like pi and
    // opencode show on collapsed tool calls.
    const QString text
        = "<details data-tool=\"true\"><summary>bash ls -la\n\n```\nl4\nl5\n…\n```</summary>\n\n"
          "full output body\n</details>\n";
    renderer.feed(text.toUtf8());
    renderer.finish();

    QTextDocument *doc = renderer.document();
    bool headerFound = false;
    bool previewInHeader = false;
    bool previewVisible = false;
    bool bodyHidden = false;
    bool rawFence = false;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid(); blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        const bool isToggle = fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool();
        // The body is a non‑toggle block, so check it before skipping those.
        if (blk.text() == QStringLiteral("full output body"))
            bodyHidden = !blk.isVisible();
        // The fence must be interpreted, never rendered as raw backticks.
        if (blk.text().contains(QStringLiteral("```")))
            rawFence = true;
        if (!isToggle)
            continue;
        if (blk.text().startsWith(QStringLiteral("bash ls -la")))
            headerFound = true;
        if (blk.text().contains(QStringLiteral("l5"))) {
            previewInHeader = isToggle;
            previewVisible = blk.isVisible();
        }
    }
    QVERIFY2(headerFound, "tool call header must be present");
    QVERIFY2(previewInHeader, "output preview must be part of the (toggle) header");
    QVERIFY2(previewVisible, "output preview must be visible while collapsed");
    QVERIFY2(bodyHidden, "tool call body must stay collapsed");
    QVERIFY2(!rawFence, "code fences must not be rendered as raw backticks");
}

void MarkdownRendererTest::collapsedSectionStaysCollapsed()
{
    MarkdownRenderer renderer;
    renderer.resize(600, 400);
    renderer.show();
    renderer.document()->setTextWidth(500);
    QApplication::processEvents();

    const QString thinking = "Let me think...";
    const QString answer = "The answer is 42.";
    const QString open
        = QStringLiteral("<details><summary>Thinking</summary>\n\n%1\n</details>\n").arg(thinking);
    const QString closed
        = QStringLiteral("<details><summary>Thought Process</summary>\n\n%1\n</details>\n\n%2\n")
              .arg(thinking, answer);

    // Stream the open form fully and finish, so we have a stable section.
    for (int i = 0; i < open.size(); i += 7)
        renderer.feed(open.left(i + 7).toUtf8());
    renderer.feed(open.toUtf8());
    renderer.finish();
    QApplication::processEvents();

    // Locate the header (toggle) block and a body block of the section.
    QTextDocument *doc = renderer.document();
    QTextBlock header;
    int secId = -1;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid(); blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        if (!fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool())
            continue;
        header = blk;
        secId = fmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt();
        break;
    }
    QVERIFY2(header.isValid(), "expected a details header block");

    QTextBlock body;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid(); blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        if (fmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt() == secId
            && !fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool()) {
            body = blk;
            break;
        }
    }
    QVERIFY2(body.isValid(), "expected a details body block");
    QVERIFY2(body.isVisible(), "body should be expanded by default");

    // Click the header to collapse the section. Clicking the viewport is the
    // real user path; no scrolling in this test, so viewport coordinates equal
    // document coordinates.
    const QRectF br = doc->documentLayout()->blockBoundingRect(header);
    const QPoint docPos = br.topLeft().toPoint() + QPoint(5, 5);
    QTest::mouseClick(renderer.viewport(), Qt::LeftButton, Qt::NoModifier, docPos);
    QApplication::processEvents();
    QVERIFY2(!body.isVisible(), "body must collapse after clicking the header");

    // Feed a divergent (rewritten) buffer, which triggers a full reset and
    // re-render. The collapse must survive: section ids are ordinal and the
    // toggle state is preserved across reset().
    renderer.feed(closed.toUtf8());
    renderer.finish();
    QApplication::processEvents();

    QTextBlock body2;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid(); blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        if (fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool())
            continue;
        if (fmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt() > 0) {
            body2 = blk;
            break;
        }
    }
    QVERIFY2(body2.isValid(), "expected a re-rendered details body block");
    QVERIFY2(!body2.isVisible(), "body must stay collapsed after a divergent re-feed");
}

void MarkdownRendererTest::svgCodeBlockRendersAsImage()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text
        = QStringLiteral("Here you go:\n\n```svg\n"
                         "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"60\">\n"
                         "  <rect width=\"120\" height=\"60\" fill=\"#f00\"/>\n"
                         "</svg>\n```");
    streamText(renderer, text);

    // The block must be an image, not source text: scan the document for an
    // image char format.
    QString imageUrl;
    for (int p = 0; p < renderer.document()->characterCount() && imageUrl.isEmpty();
         ++p) {
        QTextCursor cursor(renderer.document());
        cursor.setPosition(p);
        if (cursor.charFormat().isImageFormat())
            imageUrl = cursor.charFormat().toImageFormat().name();
    }
    QVERIFY2(!imageUrl.isEmpty(), "expected an image in the document");
    QVERIFY2(imageUrl.startsWith(QLatin1String("llamasvg://")), qPrintable(imageUrl));
    QVERIFY(renderer.svgContentForUrl(QUrl(imageUrl)).contains("<rect"));

    // The picture lives in a <details> header; the source is in the body,
    // collapsed by default.
    QTextDocument *doc = renderer.document();
    int secId = 0;
    bool headerFound = false;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid() && !headerFound;
         blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        if (!fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool())
            continue;
        if (fmt.property(MarkdownRenderer::DetailsSummaryTextProp).toString()
            != QStringLiteral("SVG image"))
            continue;
        secId = fmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt();
        headerFound = true;
    }
    QVERIFY2(headerFound, "expected a details header with an SVG image summary");

    // The header is two toggle blocks: the centered picture, and the label
    // line below it (clicking either expands the source).
    QTextBlock imageBlock;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid() && !imageBlock.isValid();
         blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        if (fmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt() == secId
            && fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool())
            imageBlock = blk;
    }
    const QTextBlockFormat labelFmt = imageBlock.next().blockFormat();
    QCOMPARE(labelFmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt(), secId);
    QVERIFY2(labelFmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool(),
             "the label line must be a toggle block, too");

    QTextBlock body;
    for (QTextBlock blk = doc->firstBlock(); blk.isValid() && !body.isValid();
         blk = blk.next()) {
        const QTextBlockFormat fmt = blk.blockFormat();
        if (fmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt() == secId
            && !fmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool())
            body = blk;
    }
    QVERIFY2(body.isValid(), "expected a details body carrying the SVG source");
    QVERIFY2(!body.isVisible(), "the source body must be collapsed by default");
    QVERIFY(renderer.toPlainText().contains("<rect"));
}

void MarkdownRendererTest::dataUrlImageRendersAsImage()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // The tool bubble embeds image attachments (read_file on an image file)
    // as data URLs; the destination contains commas, which the parser must
    // keep inside the image link.
    const QString text
        = QStringLiteral("Result:\n\n![Image](data:image/png;base64,iVBORw0KGgo=)");
    streamText(renderer, text);

    QString imageUrl;
    for (int p = 0; p < renderer.document()->characterCount() && imageUrl.isEmpty();
         ++p) {
        QTextCursor cursor(renderer.document());
        cursor.setPosition(p);
        if (cursor.charFormat().isImageFormat())
            imageUrl = cursor.charFormat().toImageFormat().name();
    }
    QVERIFY2(!imageUrl.isEmpty(), "expected an image in the document");
    QCOMPARE(imageUrl, QStringLiteral("data:image/png;base64,iVBORw0KGgo="));
}

void MarkdownRendererTest::scaledImageForDisplay()
{
    // A null image stays null.
    QVERIFY(MarkdownRenderer::scaledImageForDisplay(QImage(), 500, 2.0).isNull());

    // Small images keep their native size and get no device pixel ratio
    // (up-scaling a small picture only makes it blurrier).
    QImage small(100, 50, QImage::Format_ARGB32);
    small.fill(Qt::red);
    const QImage smallOut = MarkdownRenderer::scaledImageForDisplay(small, 500, 2.0);
    QCOMPARE(smallOut.size(), QSize(100, 50));
    QCOMPARE(smallOut.devicePixelRatio(), 1.0);

    // A wide image is fitted to the width and rasterized at device
    // resolution: logical 500x250 at dpr 2 is a 1000x500 px raster.
    QImage wide(2000, 1000, QImage::Format_ARGB32);
    wide.fill(Qt::blue);
    const QImage wideOut = MarkdownRenderer::scaledImageForDisplay(wide, 500, 2.0);
    QCOMPARE(wideOut.devicePixelRatio(), 2.0);
    QCOMPARE(wideOut.width(), 1000);
    QCOMPARE(wideOut.height(), 500);

    // A tall image hits the 600 px height cap: logical 150x600 at dpr 1.
    QImage tall(1000, 4000, QImage::Format_ARGB32);
    tall.fill(Qt::green);
    const QImage tallOut = MarkdownRenderer::scaledImageForDisplay(tall, 500, 1.0);
    QCOMPARE(tallOut.width(), 150);
    QCOMPARE(tallOut.height(), 600);

    // The down-scaled raster actually carries the source content (the
    // stepped resampling must not lose the image).
    QVERIFY(wideOut.pixelColor(wideOut.width() / 2, wideOut.height() / 2)
                .blue() > 200);
}

void MarkdownRendererTest::svgCodeBlockWithoutLanguageTag()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // No language on the fence (the write/apply_patch tool views and many
    // models emit SVG this way) – detection must work from the content.
    const QString text
        = QStringLiteral("```\n"
                         "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\">\n"
                         "  <circle cx=\"5\" cy=\"5\" r=\"4\"/>\n"
                         "</svg>\n```");
    streamText(renderer, text);
    QVERIFY(renderer.toPlainText().contains(QChar(0xFFFC)));
    QVERIFY(renderer.toPlainText().contains(QLatin1String("<circle")));
}

void MarkdownRendererTest::brokenSvgFallsBackToCode()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // Contains the closing tag, but is not well-formed XML, so QSvgRenderer
    // rejects it and the block must stay a regular code block.
    const QString text
        = QStringLiteral("```svg\n<svg xmlns=\"http://www.w3.org/2000/svg\"><broken\n</svg>\n```");
    const QString out = streamText(renderer, text);
    QVERIFY2(out.contains(QStringLiteral("<broken")), qPrintable(out));
    QVERIFY2(!renderer.toPlainText().contains(QChar(0xFFFC)),
             "a broken SVG must not produce an image");
}

void MarkdownRendererTest::mermaidCodeBlockRendersAsImage()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text
        = QStringLiteral("```mermaid\n"
                         "flowchart LR\n"
                         "  A[Start] --> B[End]\n"
                         "```");
    streamText(renderer, text);

    // The diagram renders asynchronously: wait for the placeholder to be
    // swapped for the picture.
    QVERIFY2(waitForPendingMermaid(renderer),
             "timed out waiting for the async mermaid render");

    // The finalized block must be an image, not source text.
    QString imageUrl;
    for (int p = 0; p < renderer.document()->characterCount() && imageUrl.isEmpty();
         ++p) {
        QTextCursor cursor(renderer.document());
        cursor.setPosition(p);
        if (cursor.charFormat().isImageFormat())
            imageUrl = cursor.charFormat().toImageFormat().name();
    }
    QVERIFY2(!imageUrl.isEmpty(), "expected an image in the document");
    QVERIFY2(imageUrl.startsWith(QLatin1String("llamasvg://")), qPrintable(imageUrl));
    QVERIFY(renderer.svgContentForUrl(QUrl(imageUrl)).contains("<svg"));
    // The SVG is sanitized for Qt (no unsupported filters).
    QVERIFY(!renderer.svgContentForUrl(QUrl(imageUrl)).contains("<filter"));

    // The picture must sit in its own (centered) block; the caption line
    // stays below it, not appended to the picture's paragraph (regression:
    // the async swap used to eat the paragraph separator).
    QTextBlock imageBlock;
    for (QTextBlock blk = renderer.document()->firstBlock(); blk.isValid();
         blk = blk.next()) {
        if (!blk.blockFormat().property(MarkdownRenderer::DetailsToggleBlockProp).toBool())
            continue;
        for (auto it = blk.begin(); it != blk.end(); ++it)
            if (it.fragment().charFormat().isImageFormat())
                imageBlock = blk;
        if (imageBlock.isValid())
            break;
    }
    QVERIFY2(imageBlock.isValid(), "expected the picture in a details header block");
    QVERIFY2(!imageBlock.text().contains(QStringLiteral("Mermaid diagram")),
             "the caption must stay in its own block below the picture");
    const QTextBlockFormat captionFmt = imageBlock.next().blockFormat();
    QVERIFY2(captionFmt.property(MarkdownRenderer::DetailsToggleBlockProp).toBool(),
             "the caption line must directly follow the picture");
}

void MarkdownRendererTest::invalidMermaidFallsBackToCode()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text
        = QStringLiteral("```mermaid\n"
                         "flowchart LR\n"
                         "  A -->\n"
                         "```");
    const QString out = streamText(renderer, text);

    // The render runs asynchronously: wait for the failure to arrive and the
    // spinner placeholder to be removed.
    QVERIFY2(waitForPendingMermaid(renderer),
             "timed out waiting for the async mermaid render");
    // Invalid diagram: no image, the source stays visible as a code block.
    QVERIFY2(out.contains(QStringLiteral("A -->")), qPrintable(out));
    QVERIFY2(!renderer.toPlainText().contains(QChar(0xFFFC)),
             "an invalid mermaid diagram must not produce an image");
    // Removing the placeholder must not eat into the caption line below it.
    QVERIFY2(renderer.toPlainText().contains(QStringLiteral("Mermaid diagram")),
             "the caption must survive the placeholder removal");
}

void MarkdownRendererTest::inlineMathRendersAsImage()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text
        = QStringLiteral("The answer is $e^{i\\pi}+1=0$, which is nice.");
    const QString out = streamText(renderer, text);

    // The formula renders synchronously as an inline SVG image.
    const QString url = firstImageUrl(renderer);
    QVERIFY2(url.startsWith(QLatin1String("llamasvg://ktx-")), qPrintable(url));
    QVERIFY(renderer.svgContentForUrl(QUrl(url)).contains("<svg"));
    // The image is pinned to the line bottom (AlignBottom) with a re-boxed
    // SVG whose bottom sits one font descent below the math baseline, so the
    // formula's baseline lands on the text baseline (regression: the
    // tightly-cropped SVG used to float above the line).
    for (int p = 0; p < renderer.document()->characterCount(); ++p) {
        QTextCursor cursor(renderer.document());
        cursor.setPosition(p);
        const QTextCharFormat cf = cursor.charFormat();
        if (!cf.isImageFormat())
            continue;
        QCOMPARE(cf.toImageFormat().verticalAlignment(),
                 QTextCharFormat::AlignBottom);
        // The served SVG must be at least as tall as baseline + descent:
        // parse the root height and compare against the image format height.
        const QByteArray svg = renderer.svgContentForUrl(
                QUrl(cf.toImageFormat().name()));
        const int hAttr = svg.indexOf("height=\"");
        QVERIFY2(hAttr >= 0, qPrintable(svg.left(120)));
        const double svgHeight = QString::fromLatin1(
                svg.mid(hAttr + 8).split('"').first()).toDouble();
        const double imgHeight = cf.toImageFormat().height();
        QVERIFY2(qAbs(svgHeight - imgHeight) < 0.01,
                 qPrintable(QStringLiteral("svg height %1 vs image height %2")
                                 .arg(svgHeight)
                                 .arg(imgHeight)));
        break;
    }
    // The surrounding text survives.
    QVERIFY2(out.contains(QStringLiteral("The answer is")), qPrintable(out));
    QVERIFY2(out.contains(QStringLiteral("which is nice.")), qPrintable(out));
}

void MarkdownRendererTest::displayMathRendersAsImage()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text
        = QStringLiteral("$$\n\\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}\n$$");
    streamText(renderer, text);

    const QString url = firstImageUrl(renderer);
    QVERIFY2(url.startsWith(QLatin1String("llamasvg://ktx-")), qPrintable(url));
    QVERIFY(renderer.svgContentForUrl(QUrl(url)).contains("<svg"));

    // Free-standing math is scaled up (displayScale) relative to the same
    // formula inline, which stays at the surrounding text size.
    const double displayHeight = imageFormatHeight(renderer, url);
    MarkdownRenderer inlineRenderer;
    inlineRenderer.document()->setTextWidth(500);
    streamText(inlineRenderer,
               QStringLiteral("$\\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}$"));
    const double inlineHeight = imageFormatHeight(
            inlineRenderer, firstImageUrl(inlineRenderer));
    QVERIFY2(inlineHeight > 0, "inline math image must have a height");
    QVERIFY2(displayHeight > 1.15 * inlineHeight,
             qPrintable(QStringLiteral("display math (%1) must be clearly "
                                       "larger than inline math (%2)")
                             .arg(displayHeight)
                             .arg(inlineHeight)));
}

void MarkdownRendererTest::radicalSvgHasNoClipPath()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // Regression: the radical tail used to be emitted as an <svg> fragment
    // relying on `preserveAspectRatio="slice"` + clip-path. Qt's QSvgRenderer
    // mishandles clip-path (it fills the entire clip rect), so the tail
    // rendered as a solid black box. katex2svg.js now clips the flattened
    // path in JS and emits plain M/L/Z paths, so no clip-path may appear.
    const QString text = QStringLiteral("$$\n\\sqrt{b^2-4ac}\n$$");
    streamText(renderer, text);

    const QString url = firstImageUrl(renderer);
    QVERIFY2(url.startsWith(QLatin1String("llamasvg://ktx-")), qPrintable(url));
    const QString svg = renderer.svgContentForUrl(QUrl(url));
    QVERIFY2(svg.contains(QLatin1String("<svg")), qPrintable(svg.left(200)));
    QVERIFY2(!svg.contains(QLatin1String("clipPath")),
             "radical SVG must not use clipPath (Qt renders it as a black box)");
    QVERIFY2(!svg.contains(QLatin1String("clip-path")),
             "radical SVG must not use clip-path (Qt renders it as a black box)");

    // Sanity-check the rasterized ink: with the old bug the radical tail's
    // clip rect was filled solid, so a large fraction of the bounding box
    // would be dark. Count dark pixels and require the fill fraction to be
    // low (the radical is a thin stroke).
    QSvgRenderer svgRenderer(svg.toUtf8());
    QVERIFY2(svgRenderer.isValid(), "the math SVG must parse for QSvgRenderer");
    // Render at a fixed size with a white background (the document paints it
    // on white; transparency would skew the dark-pixel count).
    const QSize size(300, 60);
    QImage image(size, QImage::Format_ARGB32);
    image.fill(Qt::white);
    {
        QPainter painter(&image);
        svgRenderer.render(&painter);
    }
    int dark = 0;
    const QImage mono = image.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < mono.height(); ++y)
        for (int x = 0; x < mono.width(); ++x)
            if (qGray(mono.pixel(x, y)) < 128)
                ++dark;
    const double fillFraction = double(dark) / (mono.width() * mono.height());
    QVERIFY2(fillFraction < 0.5,
             qPrintable(QStringLiteral("ink fraction %1 is too high for a "
                                       "radical (black-box regression)")
                             .arg(fillFraction)));
}

void MarkdownRendererTest::radicalHookClearsRadicand()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // Regression: the radicand span's padding-left:1em was dropped for
    // elements with children, so the radical's hook was drawn over the
    // first glyph of the radicand. The hook (a path) must start to the
    // left of the radicand's first glyph (a <text> element).
    const QString text = QStringLiteral("$$\\sqrt{a^2 + b^2}$$");
    streamText(renderer, text);

    const QString url = firstImageUrl(renderer);
    QVERIFY2(url.startsWith(QLatin1String("llamasvg://ktx-")), qPrintable(url));
    const QString svg = renderer.svgContentForUrl(QUrl(url));

    // The first <path> is the radical hook (+ vinculum); its smallest x
    // coordinate must be left of the radicand's first glyph.
    double pathMinX = 1e9;
    const int dStart = svg.indexOf(QLatin1String("<path d=\""));
    if (dStart >= 0) {
        const int dEnd = svg.indexOf(QLatin1Char('"'), dStart + 9);
        // "M x y L x y ...": x coordinates are the even-indexed numbers.
        const QStringList nums = svg.mid(dStart + 9, dEnd - (dStart + 9))
                                     .split(QRegularExpression("[^-\\d.]+"),
                                            Qt::SkipEmptyParts);
        for (int i = 0; i + 1 < nums.size(); i += 2)
            pathMinX = qMin(pathMinX, nums.at(i).toDouble());
    }
    const QRegularExpression textRe(QStringLiteral(
            "<text x=\"([\\d.]+)\" y=\"([\\d.]+)\""));
    const QRegularExpressionMatch tm = textRe.match(svg);
    QVERIFY2(tm.hasMatch(), qPrintable(svg.left(200)));
    QVERIFY2(pathMinX < 1e9, "expected a radical path in the SVG");
    QVERIFY2(pathMinX < tm.captured(1).toDouble(),
             qPrintable(QStringLiteral("hook starts at %1, first glyph at %2 "
                                       "(the hook must clear the radicand)")
                             .arg(pathMinX)
                             .arg(tm.captured(1))));
}

void MarkdownRendererTest::invalidMathShowsSource()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    const QString text = QStringLiteral("Broken: $\\nonsense{$ here.");
    const QString out = streamText(renderer, text);

    // No image: the source (with its delimiters) stays visible.
    QVERIFY2(firstImageUrl(renderer).isEmpty(),
             "invalid math must not produce an image");
    QVERIFY2(out.contains(QStringLiteral("\\nonsense{")), qPrintable(out));
}

void MarkdownRendererTest::plainDollarsStayText()
{
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    // No matched $...$ pair (the second $ cannot close: a digit follows):
    // the text must come through verbatim, with no math image.
    const QString text = QStringLiteral("The ticket costs $5 and shipping is $10.");
    const QString out = streamText(renderer, text);

    QCOMPARE(out.trimmed(), text);
    QVERIFY2(firstImageUrl(renderer).isEmpty(), "no math image expected");
}

void MarkdownRendererTest::mermaidServedFromPersistedCache()
{
    // Render the diagram for real (async) and capture the SVG the renderer
    // reports as freshly rendered (this is what the chat UI persists with
    // the message).
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    QString key;
    MarkdownRenderer::DiagramSvg svg;
    int emissions = 0;
    QObject::connect(&renderer, &MarkdownRenderer::diagramRendered, [&](const QString &k,
                                                                        const MarkdownRenderer::DiagramSvg &s) {
        key = k;
        svg = s;
        ++emissions;
    });
    streamText(renderer,
               QStringLiteral("```mermaid\nflowchart LR\n  A[Start] --> B[End]\n```"));
    QVERIFY2(waitForPendingMermaid(renderer), "timed out waiting for the async render");
    QCOMPARE(emissions, 1);
    QVERIFY2(!key.isEmpty() && !svg.svg.isEmpty(), "expected a rendered diagram SVG");
    QCOMPARE(key, MarkdownRenderer::mermaidDiagramKey(QStringLiteral("flowchart LR\n  A[Start] --> B[End]")));

    // A renderer seeded with the persisted entry must show the picture
    // immediately (synchronously, no spinner, no engine run, no new
    // emission).
    MarkdownRenderer cached;
    cached.document()->setTextWidth(500);
    cached.seedDiagramCache(key, svg);
    int cachedEmissions = 0;
    QObject::connect(&cached, &MarkdownRenderer::diagramRendered, [&](const QString &, const MarkdownRenderer::DiagramSvg &) {
        ++cachedEmissions;
    });
    streamText(cached, QStringLiteral("```mermaid\nflowchart LR\n  A[Start] --> B[End]\n```"));

    // No pending placeholder: the picture was in place before any async
    // render could have run.
    for (QTextBlock blk = cached.document()->firstBlock(); blk.isValid();
         blk = blk.next()) {
        QVERIFY2(blk.blockFormat()
                     .property(MarkdownRenderer::MermaidPendingKeyProp)
                     .toString()
                     .isEmpty(),
                 "a cached diagram must not show a spinner placeholder");
    }
    const QString url = firstImageUrl(cached);
    QVERIFY2(url.startsWith(QLatin1String("llamasvg://")), qPrintable(url));
    QCOMPARE(cached.svgContentForUrl(QUrl(url)), svg.svg);
    QCOMPARE(cachedEmissions, 0);
}

void MarkdownRendererTest::mathServedFromPersistedCache()
{
    // Render the formula for real and capture the SVG (the chat UI persists
    // it with the message).
    MarkdownRenderer renderer;
    renderer.document()->setTextWidth(500);
    QString key;
    MarkdownRenderer::DiagramSvg svg;
    int emissions = 0;
    QObject::connect(&renderer, &MarkdownRenderer::diagramRendered, [&](const QString &k,
                                                                        const MarkdownRenderer::DiagramSvg &s) {
        key = k;
        svg = s;
        ++emissions;
    });
    streamText(renderer, QStringLiteral("The answer is $e^{i\\pi}+1=0$."));
    QCOMPARE(emissions, 1);
    QVERIFY2(!key.isEmpty() && !svg.svg.isEmpty(), "expected a rendered math SVG");
    QCOMPARE(key, MarkdownRenderer::katexDiagramKey(QStringLiteral("e^{i\\pi}+1=0"), false));

    // A seeded renderer serves the same SVG synchronously, with the same
    // image geometry, and does not re-render.
    MarkdownRenderer cached;
    cached.document()->setTextWidth(500);
    cached.seedDiagramCache(key, svg);
    int cachedEmissions = 0;
    QObject::connect(&cached, &MarkdownRenderer::diagramRendered, [&](const QString &, const MarkdownRenderer::DiagramSvg &) {
        ++cachedEmissions;
    });
    streamText(cached, QStringLiteral("The answer is $e^{i\\pi}+1=0$."));

    const QString url = firstImageUrl(cached);
    QVERIFY2(url.startsWith(QLatin1String("llamasvg://")), qPrintable(url));
    QCOMPARE(cached.svgContentForUrl(QUrl(url)), svg.svg);
    // The image box must keep the persisted geometry (re-boxed height).
    QCOMPARE(imageFormatHeight(cached, url), svg.height);
    QCOMPARE(cachedEmissions, 0);
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MarkdownRendererTest tc;
    return QTest::qExec(&tc);
}

#include "markdownrenderer_test.moc"
