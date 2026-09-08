#include <QApplication>
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

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MarkdownRendererTest tc;
    return QTest::qExec(&tc);
}

#include "markdownrenderer_test.moc"
