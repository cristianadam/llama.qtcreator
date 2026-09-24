// Plugin self-test for the mermaid render engine, run inside Qt Creator
// itself:
//
//   Qt Creator -pluginpath <this build dir> -test llamacpp[,MermaidEngineTest]
//
// Exercises the real usage path: the engine's worker thread, the bundled
// QuickJS runtime, the DOM shim with real Qt font metrics, and the SVG
// sanitizer. The standalone unit tests cover the markdown integration;
// this one covers the engine in a live IDE process.

#ifdef WITH_TESTS

#include "mermaidengine.h"

#include <QtTest/QtTest>

#include <QRegularExpression>

namespace LlamaCpp {

namespace {

class MermaidEngineTest : public QObject
{
    Q_OBJECT

private slots:
    void classDiagramRenders();
    void ganttDiagramHasNonZeroWidth();
    void secondRenderIsCachedAndConsistent();
    void invalidDiagramFallsBack();
};

} // namespace

void MermaidEngineTest::classDiagramRenders()
{
    // The engine is warmed up at plugin startup (LlamaPlugin::
    // extensionsInitialized); wait for the worker to finish evaluating the
    // bundle before timing the first render.
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);

    const QByteArray svg
        = MermaidEngine::instance()->render("classDiagram\n"
                                            "    class A { +A() +aMethod() }\n"
                                            "    class B { +B() +bMethod() }\n"
                                            "    A <|-- B",
                                            "default");
    QVERIFY2(!svg.isEmpty(), "the class diagram must render to SVG");
    // The nested <tspan> pairs mermaid emits must be flattened: Qt's SVG
    // module drops a <tspan> nested in a <tspan> (labels detach from the
    // boxes).
    const QRegularExpression nestedTspan(QStringLiteral("<tspan\\b[^>]*>\\s*<tspan"));
    QVERIFY2(nestedTspan.match(QString::fromUtf8(svg)).hasMatch() == false,
             "nested tspans must be flattened");
    // The theme CSS stays in the SVG (Qt >= 6.10 Qt SVG supports <style>).
    QVERIFY2(svg.contains("<style"), "the theme stylesheet must survive");
    // Filters are stripped (Qt aborts the element tree on them).
    QVERIFY2(!svg.contains("<filter"), "filters must be stripped");
}

void MermaidEngineTest::ganttDiagramHasNonZeroWidth()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render("gantt\n"
                                            "  title Project timeline\n"
                                            "  a: 2026-01-01, 10d\n"
                                            "  b: after a, 5d",
                                            "default");
    QVERIFY2(!svg.isEmpty(), "the gantt diagram must render to SVG");
    // Mermaid sizes the gantt to the render container's width; a zero
    // viewBox width means the DOM shim reported a zero-size container.
    const int vb = svg.indexOf("viewBox=");
    QVERIFY2(vb >= 0, "the SVG must have a viewBox");
    const QRegularExpression re(QStringLiteral("viewBox=\"0 0 (\\d+) (\\d+)\""));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(svg));
    QVERIFY2(m.hasMatch(), qPrintable(QString::fromUtf8(svg.left(200))));
    QVERIFY2(m.captured(1).toInt() > 0,
             qPrintable(QStringLiteral("the gantt viewBox width must be non-zero, got ")
                            + m.captured(1)));
}

void MermaidEngineTest::secondRenderIsCachedAndConsistent()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QString source = "flowchart LR\n  A[Start] --> B[End]";
    const QByteArray first = MermaidEngine::instance()->render(source, "default");
    QVERIFY2(!first.isEmpty(), "the flowchart must render to SVG");
    const QByteArray second = MermaidEngine::instance()->render(source, "default");
    QCOMPARE(first, second);
    // A different theme is a different diagram.
    const QByteArray dark = MermaidEngine::instance()->render(source, "dark");
    QVERIFY2(!dark.isEmpty(), "the dark-themed flowchart must render to SVG");
    QVERIFY(first != dark);
}

void MermaidEngineTest::invalidDiagramFallsBack()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render("flowchart LR\n  A -->", "default");
    QVERIFY2(svg.isEmpty(), "an invalid diagram must not produce SVG");
}

namespace Internal {

QObject *createMermaidEngineTest()
{
    return new MermaidEngineTest;
}

} // namespace Internal

} // namespace LlamaCpp

#include "mermaidengine_test.moc"

#endif // WITH_TESTS
