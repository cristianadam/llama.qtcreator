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

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    MarkdownRendererTest tc;
    return QTest::qExec(&tc);
}

#include "markdownrenderer_test.moc"
