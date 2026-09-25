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

#include <cstdlib>
#include <cstring>
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
    void testViewBoxTightAroundContent();
    void testClassLabelsStayInsideBoxes();
    void testClassDividersStayInsideBoxes();
    void testEdgeLabelTextSitsOnBackground();
    void testArrowMarkersHaveNoCenteringViewBox();
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
    // The engine tightens the viewBox origin to the content, so only the
    // width/height (3rd/4th field) are asserted; the origin may be nonzero.
    const QRegularExpression re(QStringLiteral(
        "viewBox=\"[-0-9.e]+ [-0-9.e]+ ([0-9.e]+) ([0-9.e]+)\""));
    const QRegularExpressionMatch m = re.match(QString::fromUtf8(svg));
    QVERIFY2(m.hasMatch(), qPrintable(QString::fromUtf8(svg.left(200))));
    QVERIFY2(m.captured(1).toDouble() > 0,
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
           "    A <|-- B : inherits\n"
           "    A <|-- C : inherits\n"
           "    B <|-- D : inherits\n"
           "    C <|-- D : inherits");
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

void MermaidEngineTest::testViewBoxTightAroundContent()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render(diamondSource(), "default");
    const QString text = QString::fromUtf8(svg);
    QVERIFY2(!svg.isEmpty(), "the diamond class diagram must render to SVG");

    // The chat canvas is sized from the viewBox and the diagram is centered
    // in it. Mermaid's own viewBox is its *layout*'s bounding box (node
    // groups are drawn around their centers and dagre reserves column and
    // label space), so without the engine tightening it the canvas comes
    // out several times the size of the drawing and the diagram floats
    // off-center in the chat. Assert the viewBox is close to the drawn
    // content (the class boxes, which bound the edges and labels here).
    const QRegularExpression vbRe(QStringLiteral(
        "viewBox=\"([-0-9.e]+) ([-0-9.e]+) ([0-9.e]+) ([0-9.e]+)\""));
    const QRegularExpressionMatch vb = vbRe.match(text);
    QVERIFY2(vb.hasMatch(), "the SVG root must carry a viewBox");
    const double vbW = vb.captured(3).toDouble();
    const double vbH = vb.captured(4).toDouble();

    // Node centers: transform="translate(cx, cy)".
    const QRegularExpression nodeRe(QStringLiteral(
        "<g class=\"node[^\"]*\" id=\"[^\"]*classId-[^\"]*\"[^>]*transform=\"translate[(]([-0-9.e]+),\\s*([-0-9.e]+)[)]"));
    // The box outline: a four-corner rect path in node-local coordinates.
    const QRegularExpression boxRe(QStringLiteral(
        "M[-0-9.e ]+L[-0-9.e ]+L[-0-9.e ]+L[-0-9.e ]+"));
    const QRegularExpression numRe(QStringLiteral("[-0-9.e]+"));

    QVector<QRegularExpressionMatch> nodes;
    for (auto it = nodeRe.globalMatch(text); it.hasNext();)
        nodes.append(it.next());
    QVERIFY2(nodes.size() == 4, "the diamond diagram has four class nodes");

    bool first = true;
    double contentTop = 0;
    double contentBottom = 0;
    double contentLeft = 0;
    double contentRight = 0;
    for (int i = 0; i < nodes.size(); ++i) {
        const QRegularExpressionMatch node = nodes.at(i);
        const double cx = node.captured(1).toDouble();
        const double cy = node.captured(2).toDouble();
        const int start = node.capturedStart();
        const int end = i + 1 < nodes.size() ? nodes.at(i + 1).capturedStart() : text.size();
        const QRegularExpressionMatch box = boxRe.match(text.mid(start, end - start));
        QVERIFY2(box.hasMatch(), "each class node must draw its box path");
        QVector<double> nums;
        for (auto nIt = numRe.globalMatch(box.captured(0)); nIt.hasNext();)
            nums.append(nIt.next().captured().toDouble());
        QVERIFY2(nums.size() == 8, "the box path has four corners");
        double x1 = 0;
        double x2 = 0;
        double y1 = 0;
        double y2 = 0;
        for (int k = 0; k < nums.size(); k += 2) {
            // Node-local corner, offset to the node center.
            const double x = cx + nums.at(k);
            const double y = cy + nums.at(k + 1);
            if (k == 0) {
                x1 = x2 = x;
                y1 = y2 = y;
            } else {
                x1 = qMin(x1, x);
                x2 = qMax(x2, x);
                y1 = qMin(y1, y);
                y2 = qMax(y2, y);
            }
        }
        if (first) {
            contentTop = y1;
            contentBottom = y2;
            contentLeft = x1;
            contentRight = x2;
            first = false;
        } else {
            contentTop = qMin(contentTop, y1);
            contentBottom = qMax(contentBottom, y2);
            contentLeft = qMin(contentLeft, x1);
            contentRight = qMax(contentRight, x2);
        }
    }
    // The edges and the edge labels of the diamond stay inside the range of
    // the node boxes, so the boxes bound the drawn content. The viewBox
    // must fit that range tightly (the engine adds a small fixed padding);
    // a layout-sized viewBox is far larger.
    const double contentW = contentRight - contentLeft;
    const double contentH = contentBottom - contentTop;
    QVERIFY2(qAbs(vbW - contentW) <= 40.0,
             qPrintable(QStringLiteral("viewBox width %1 does not fit the content width %2")
                            .arg(vbW, 0, 'f', 1).arg(contentW, 0, 'f', 1)));
    QVERIFY2(qAbs(vbH - contentH) <= 40.0,
             qPrintable(QStringLiteral("viewBox height %1 does not fit the content height %2")
                            .arg(vbH, 0, 'f', 1).arg(contentH, 0, 'f', 1)));
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
    // NOTE: the patterns in this file are plain (escaped) string literals,
    // not C++11 raw strings: Qt 6.11's moc mis-tokenizes a file containing
    // several R"(...)" raw strings with embedded quotes and bails out with
    // a bogus "missing ')' in macro usage" (see testClassDividers... below,
    // which used to trigger it at build time).
    // NOTE: the patterns in this file are plain (escaped) string literals,
    // not C++11 raw strings: Qt 6.11's moc mis-tokenizes a file containing
    // several R"(...)" raw strings with embedded quotes and bails out with
    // a bogus "missing ')' in macro usage". Likewise the literal parens
    // are matched as [(] / [)] rather than \( / \): Qt 6.11's QRegularExpression
    // (PCRE2 JIT) intermittently fails to match long literal prefixes
    // followed by an escaped paren, reporting 0 hits for patterns that
    // PCRE agrees on.
    const QRegularExpression nodeRe(QStringLiteral("<g class=\"node[^\"]*\" id=\"[^\"]*classId-[^\"]*\"[^>]*transform=\"translate[(]"));
    // The box's fill path is the first stroke-less path in the node group.
    const QRegularExpression boxPathRe(QStringLiteral("<path d=\"M\\s*-?[\\d.]+\\s+(-?[\\d.]+)\\s+L[\\d. -]+L[\\d. -]+L[\\d. -]+\" stroke=\"none\""));
    const QRegularExpression rowRe(QStringLiteral(
        "class=\"(label-group|members-group|methods-group) text\" transform=\"translate[(][^,]+,\\s*(-?[\\d.e]+)\\)\""));

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

void MermaidEngineTest::testClassDividersStayInsideBoxes()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render(diamondSource(), "default");
    const QString text = QString::fromUtf8(svg);
    QVERIFY2(!svg.isEmpty(), "the diamond class diagram must render to SVG");

    // mermaid's neo renderer draws the section dividers of a class with no
    // member rows using the box's *height* as width, so the lines stick out
    // of the box on both sides (mermaid's own degenerate no-members math);
    // the engine clamps them to the box x range (clampDividersToBoxes).
    const QRegularExpression nodeRe(QStringLiteral("<g class=\"node[^\"]*\" id=\"[^\"]*classId-[^\"]*\"[^>]*>"));
    const QRegularExpression pathRe(QStringLiteral("<path\\b[^>]*\\bd=\"([^\"]*)\"[^>]*>"));
    const QRegularExpression dividerRe(QStringLiteral("<g class=\"divider[^\"]*\"[^>]*>\\s*<path\\b[^>]*\\bd=\"([^\"]*)\"[^>]*>"));

    QVector<QRegularExpressionMatch> nodes;
    for (auto it = nodeRe.globalMatch(text); it.hasNext();)
        nodes.append(it.next());
    QVERIFY2(nodes.size() == 4, "the diamond diagram has four class nodes");

    for (int i = 0; i < nodes.size(); ++i) {
        const int start = nodes.at(i).capturedStart();
        const int end = i + 1 < nodes.size() ? nodes.at(i + 1).capturedStart() : text.size();
        const QString node = text.mid(start, end - start);

        double x1 = 0;
        double x2 = 0;
        bool haveBox = false;
        for (auto it = pathRe.globalMatch(node); it.hasNext();) {
            const QString d = it.next().captured(1);
            bool straight = true;
            for (auto cIt = QRegularExpression(QStringLiteral("[A-Za-z]")).globalMatch(d); cIt.hasNext();) {
                const QChar c = cIt.next().captured().at(0);
                if (!QStringLiteral("MmLlZz").contains(c)) {
                    straight = false;
                    break;
                }
            }
            if (!straight)
                continue;
            QVector<double> nums;
            for (auto nIt = QRegularExpression(QStringLiteral("-?[\\d.]+")).globalMatch(d); nIt.hasNext();)
                nums.append(nIt.next().captured().toDouble());
            if (nums.size() < 4 || nums.size() % 2 != 0)
                continue;
            x1 = x2 = nums.first();
            for (int k = 0; k < nums.size(); k += 2) {
                x1 = qMin(x1, nums.at(k));
                x2 = qMax(x2, nums.at(k));
            }
            if (x2 > x1) {
                haveBox = true;
                break;
            }
        }
        QVERIFY2(haveBox, "each class node must draw its box path");

        int dividers = 0;
        for (auto it = dividerRe.globalMatch(node); it.hasNext();) {
            const QString d = it.next().captured(1);
            QVector<double> nums;
            for (auto nIt = QRegularExpression(QStringLiteral("-?[\\d.]+")).globalMatch(d); nIt.hasNext();)
                nums.append(nIt.next().captured().toDouble());
            if (nums.size() < 2)
                continue;
            ++dividers;
            // x coordinates sit at the even indices of the number list
            // (M/L/C are all x,y pairs in a divider path).
            double dx1 = std::numeric_limits<double>::max();
            double dx2 = std::numeric_limits<double>::lowest();
            int k = 0;
            for (auto nIt = QRegularExpression(QStringLiteral("-?[\\d.]+")).globalMatch(d); nIt.hasNext();) {
                const double v = nIt.next().captured().toDouble();
                if (k % 2 == 0) {
                    dx1 = qMin(dx1, v);
                    dx2 = qMax(dx2, v);
                }
                ++k;
            }
            QVERIFY2(dx1 >= x1 - 0.5 && dx2 <= x2 + 0.5,
                     qPrintable(QStringLiteral("divider x=[%1, %2] sticks out of the box x=[%3, %4]")
                                    .arg(dx1, 0, 'f', 1).arg(dx2, 0, 'f', 1)
                                    .arg(x1, 0, 'f', 1).arg(x2, 0, 'f', 1)));
        }
        QVERIFY2(dividers == 2, "each class node draws two section dividers");
    }
}

void MermaidEngineTest::testEdgeLabelTextSitsOnBackground()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render(diamondSource(), "default");
    const QString text = QString::fromUtf8(svg);
    QVERIFY2(!svg.isEmpty(), "the diamond class diagram must render to SVG");

    // Qt SVG ignores tspan x/y and honors the <text> text-anchor: the label
    // text therefore renders centered on its own x attribute (0 when
    // absent), while mermaid places the background rect from the text's
    // getBBox(). If the shim's bbox disagrees with the anchor (it used to
    // report a left-anchored box for middle-anchored text), the text ends
    // up a half-label away from its background rect in the chat.
    const QRegularExpression labelRe(QStringLiteral("<g class=\"label\" data-id=\"id_[^\"]*\"[^>]*>\\s*<g>\\s*<rect class=\"background\"[^>]*\\bx=\"(-?[\\d.e]+)\"[^>]*\\bwidth=\"([\\d.e]+)\"[^>]*/>\\s*<text\\b([^>]*)>"));
    int labels = 0;
    for (auto it = labelRe.globalMatch(text); it.hasNext();) {
        const QRegularExpressionMatch m = it.next();
        const double rectCenter = m.captured(1).toDouble() + m.captured(2).toDouble() / 2.0;
        const QString attrs = m.captured(3);
        double textX = 0.0; // no x attribute: Qt starts the text at 0
        const QRegularExpression xRe(QStringLiteral("\\bx=\"(-?[\\d.e]+)\""));
        const QRegularExpressionMatch xm = xRe.match(attrs);
        if (xm.hasMatch())
            textX = xm.captured(1).toDouble();
        QVERIFY2(qAbs(textX - rectCenter) < 1.0,
                 qPrintable(QStringLiteral("edge label text at x=%1 but its background rect is centered at %2")
                                .arg(textX, 0, 'f', 1).arg(rectCenter, 0, 'f', 1)));
        ++labels;
    }
    QVERIFY2(labels == 4, "the diamond diagram has four edge labels");
}

void MermaidEngineTest::testArrowMarkersHaveNoCenteringViewBox()
{
    QTRY_VERIFY_WITH_TIMEOUT(MermaidEngine::instance()->isInitialized(), 60000);
    const QByteArray svg
        = MermaidEngine::instance()->render(diamondSource(), "default");
    const QString text = QString::fromUtf8(svg);
    QVERIFY2(!svg.isEmpty(), "the diamond class diagram must render to SVG");

    // Qt 6.11's QSvgRenderer misplaces <marker>s that carry a viewBox
    // (the content is dropped at the viewBox center and refX/refY are
    // lost), shifting the arrowheads off their edges. The DOM shim skips
    // storing the pure-vertical-centering viewBoxes mermaid adds to its
    // "-margin" arrowhead markers; assert they are gone from the output.
    const QRegularExpression markerRe(QStringLiteral("<marker\\b[^>]*\\bid=\"[^\"]*-extension(Start|End)-margin\"[^>]*>"));
    int markers = 0;
    for (auto it = markerRe.globalMatch(text); it.hasNext();) {
        const QString tag = it.next().captured(0);
        QVERIFY2(!tag.contains(QStringLiteral("viewBox=")),
                 qPrintable(QStringLiteral("arrowhead marker must not carry a centering viewBox: ") + tag.left(160)));
        ++markers;
    }
    QVERIFY2(markers == 2, "the inheritance arrows use the extension -margin markers");
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
    // Dirty the heap first: the mermaid engine (or QuickJS in a full IDE
    // session) has allocated and freed a lot of memory, so a fresh QImage
    // buffer is usually reused dirty pages. Without an explicit
    // fill(Qt::transparent) in renderSvgResource, that garbage is exactly
    // what the user saw as a noisy background behind the diagram.
    {
        const int dirtyBytes = 1 << 20;
        void *dirty = malloc(dirtyBytes);
        if (dirty) {
            std::memset(dirty, 0xA5, dirtyBytes);
            free(dirty);
        }
    }
    QImage image(QSize(qCeil(w * dpr), qCeil(h * dpr)), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent); // must mirror renderSvgResource()
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
