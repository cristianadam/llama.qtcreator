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

#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QRegularExpression>
#include <QSvgRenderer>

#include <limits>

namespace LlamaCpp {

namespace {

class MermaidEngineTest : public QObject
{
    Q_OBJECT

private slots:
    void testClassDiagramRenders();
    void testGanttDiagramHasNonZeroWidth();
    void testSecondRenderIsCachedAndConsistent();
    void testInvalidDiagramFallsBack();
    void testRenderedSvgHasCompleteViewBox();
    void testClassLabelsStayInsideBoxes();
    void testRasterizedSvgBackgroundIsTransparent();

private:
    static QString diamondSource();
};

} // namespace

void MermaidEngineTest::testClassDiagramRenders()
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

void MermaidEngineTest::testGanttDiagramHasNonZeroWidth()
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

void MermaidEngineTest::testSecondRenderIsCachedAndConsistent()
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

void MermaidEngineTest::testInvalidDiagramFallsBack()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render("flowchart LR\n  A -->", "default");
    QVERIFY2(svg.isEmpty(), "an invalid diagram must not produce SVG");
}

QString MermaidEngineTest::diamondSource()
{
    return QStringLiteral("classDiagram\n"
           "    class A { +A() +aMethod() }\n"
           "    class B { +B() +bMethod() }\n"
           "    class C { +C() +cMethod() }\n"
           "    class D { +D() +dMethod() }\n"
           "    A <|-- B\n"
           "    A <|-- C\n"
           "    B <|-- D\n"
           "    C <|-- D");
}

void MermaidEngineTest::testRenderedSvgHasCompleteViewBox()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render(diamondSource(), "default");
    QVERIFY2(!svg.isEmpty(), "the diamond class diagram must render to SVG");
    // Mermaid emits the root as width="100%" with no height attribute, for
    // which QSvgRenderer::defaultSize() is unreliable (it can report 0x0 or
    // Wx0 depending on the process). The viewBox is the stable source of
    // truth for the drawing's aspect ratio, so it must be complete.
    const QRegularExpression re(QStringLiteral("viewBox=\"(-?[\\d.]+) (-?[\\d.]+) ([\\d.]+) ([\\d.]+)\""));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(svg));
    QVERIFY2(m.hasMatch(), "the SVG root must carry a viewBox");
    QVERIFY2(m.captured(3).toDouble() > 0, "the viewBox width must be positive");
    QVERIFY2(m.captured(4).toDouble() > 0, "the viewBox height must be positive");
}

void MermaidEngineTest::testClassLabelsStayInsideBoxes()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render(diamondSource(), "default");
    const QString text = QString::fromUtf8(svg);
    QVERIFY2(!svg.isEmpty(), "the diamond class diagram must render to SVG");

    // For every class node, the title/member/method rows (placed via group
    // translate()) must sit inside the box's own y range. A box sized from
    // a stale or truncated getBBox() leaves the rows outside, i.e. the
    // classic "labels floating beside their box" regression.
    const QRegularExpression nodeRe(QStringLiteral(
        R"qc(<g class="node[^"]*" id="[^"]*classId-[^"]*"[^>]*transform="translate\()qc"));
    // The box's fill path is the first stroke-less path in the node group.
    const QRegularExpression boxPathRe(QStringLiteral(
        R"qc(<path d="M\s*-?[\d.]+\s+(-?[\d.]+)\s+L[\d. -]+L[\d. -]+L[\d. -]+" stroke="none")qc"));
    const QRegularExpression rowRe(QStringLiteral(
        R"qc(class="(label-group|members-group|methods-group) text" transform="translate\([^,]+,\s*(-?[\d.e]+)\))qc"));

    QVector<QRegularExpressionMatch> nodes;
    for (auto it = nodeRe.globalMatch(text); it.hasNext();)
        nodes.append(it.next());
    QVERIFY2(nodes.size() == 4, "the diamond diagram has four class nodes");

    for (int i = 0; i < nodes.size(); ++i) {
        const int start = nodes.at(i).capturedStart();
        const int end = i + 1 < nodes.size() ? nodes.at(i + 1).capturedStart() : text.size();
        const QString node = text.mid(start, end - start);

        const QRegularExpressionMatch box = boxPathRe.match(node);
        QVERIFY2(box.hasMatch(), "each class node must draw its box path");
        // The fill path is M(x1, y1) L(x2, y2) L(x3, y3) L(x4, y4): the y
        // values sit at the odd indices of the coordinate list.
        QVector<QRegularExpressionMatch> nums;
        for (auto it = QRegularExpression(QStringLiteral("-?[\\d.]+")).globalMatch(box.captured(0));
             it.hasNext();)
            nums.append(it.next());
        double top = std::numeric_limits<double>::max();
        double bottom = std::numeric_limits<double>::lowest();
        for (int k = 1; nums.size() > k; k += 2) {
            const double y = nums.at(k).captured().toDouble();
            top = qMin(top, y);
            bottom = qMax(bottom, y);
        }
        QVERIFY2(bottom > top, "the class box must have a positive height");

        int rows = 0;
        for (auto it = rowRe.globalMatch(node); it.hasNext(); ++rows) {
            const QRegularExpressionMatch row = it.next();
            const double rowY = row.captured(2).toDouble();
            // The row's baseline must be inside the box, well clear of the
            // bottom edge (two text lines below a misplaced row would stick
            // out of the box).
            QVERIFY2(rowY >= top - 2.0 && rowY <= bottom - 12.0,
                     qPrintable(QStringLiteral("%1 row at y=%2 outside the box y=[%3, %4]")
                                    .arg(row.captured(1))
                                    .arg(rowY, 0, 'f', 1)
                                    .arg(top, 0, 'f', 1)
                                    .arg(bottom, 0, 'f', 1)));
        }
        QVERIFY2(rows == 3, "each class node has title, member and method rows");
    }
}

void MermaidEngineTest::testRasterizedSvgBackgroundIsTransparent()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render(diamondSource(), "default");
    QVERIFY2(!svg.isEmpty(), "the diamond class diagram must render to SVG");

    // Rasterize exactly like MarkdownLabel::renderSvgResource does: a
    // transparent ARGB image at device resolution, sized from the viewBox
    // (the SVG root carries no usable width/height). The diagram does not
    // fill its canvas, so the background must come out fully transparent.
    // Colored low-alpha "noise" here is what the user sees as an
    // uninitialized-memory background in the chat.
    QSvgRenderer renderer(svg);
    QVERIFY2(renderer.isValid(), "the engine SVG must be a valid SVG document");
    const QRectF vb = renderer.viewBoxF();
    QVERIFY2(vb.width() > 0 && vb.height() > 0, "the viewBox must be complete");

    constexpr double targetWidth = 320.0;
    constexpr qreal dpr = 2.0;
    const double scale = targetWidth / vb.width();
    const double w = targetWidth;
    const double h = vb.height() * scale;
    QImage image(QSize(qCeil(w * dpr), qCeil(h * dpr)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    QPainter painter(&image);
    renderer.render(&painter, QRectF(0, 0, w, h));
    painter.end();

    const QList<QPoint> corners = {
        QPoint(0, 0),
        QPoint(image.width() - 1, 0),
        QPoint(0, image.height() - 1),
        QPoint(image.width() - 1, image.height() - 1),
    };
    for (const QPoint &p : corners) {
        const QRgb pixel = image.pixel(p);
        QVERIFY2(qAlpha(pixel) == 0,
                 qPrintable(QStringLiteral("background pixel at (%1, %2) is not transparent: rgba(%3, %4, %5, %6)")
                                .arg(p.x())
                                .arg(p.y())
                                .arg(qRed(pixel))
                                .arg(qGreen(pixel))
                                .arg(qBlue(pixel))
                                .arg(qAlpha(pixel))));
    }
    // The diagram content stays clear of the top edge of the viewBox; the
    // whole first device row must therefore be (almost) fully transparent.
    int transparent = 0;
    for (int x = 0; x < image.width(); ++x)
        if (qAlpha(image.pixel(x, 0)) == 0)
            ++transparent;
    QVERIFY2(double(transparent) / image.width() > 0.9,
             qPrintable(QStringLiteral("top row is %1%\% transparent, expected ~100%")
                            .arg(100.0 * transparent / image.width(), 0, 'f', 1)));
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
