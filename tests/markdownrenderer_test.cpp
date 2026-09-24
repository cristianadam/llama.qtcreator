#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QTextImageFormat>
#include <QTimer>
#include <QtTest/QtTest>

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

class MarkdownRendererTest : public QObject
{
    Q_OBJECT
private slots:
    void plainParagraph();
    void multiParagraph();
    void codeBlock();
    void list();
    void thinkingSection();
    void detailsSummaryMarkdown();
    void detailsSummaryInlineHtml();
    void toolCallCollapsedByDataToolAttribute();
    void toolCallSummaryWithOutputPreview();
    void collapsedSectionStaysCollapsed();
    void svgCodeBlockRendersAsImage();
    void svgCodeBlockWithoutLanguageTag();
    void brokenSvgFallsBackToCode();
};

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

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MarkdownRendererTest tc;
    return QTest::qExec(&tc);
}

#include "markdownrenderer_test.moc"
