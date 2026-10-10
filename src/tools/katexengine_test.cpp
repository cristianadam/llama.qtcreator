// Plugin self-test for the KaTeX math render engine, run inside Qt Creator
// itself:
//
//   Qt Creator -pluginpath <this build dir> -test llamacpp[,KaTeXEngineTest]
//
// Exercises the real usage path: the engine's worker thread, the bundled
// QuickJS runtime, the katex2svg flattener with real Qt font metrics, and
// the produced SVG. The standalone unit tests cover the markdown
// integration; this one covers the engine in a live IDE process.

#ifdef WITH_TESTS

#include "katexengine.h"

#include <QtTest/QtTest>

#include <QImage>
#include <QPainter>
#include <QRectF>
#include <QSvgRenderer>
#include <QThread>

namespace LlamaCpp {

namespace {

class KaTeXEngineTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase()
    {
        KaTeXEngine::instance()->warmUp();
        // Give the worker a moment to evaluate the bundles.
        for (int i = 0; i < 100 && !KaTeXEngine::instance()->isInitialized(); ++i)
            QThread::msleep(50);
        QVERIFY(KaTeXEngine::instance()->isInitialized());
    }

    // Renders \a tex through the engine and paints the SVG into a bitmap;
    // returns true when the render succeeded and the picture has ink
    // (i.e. is not blank).
    static bool rendersToInk(const QString &tex, bool display)
    {
        const auto r = KaTeXEngine::instance()->render(tex, display,
                                                       QStringLiteral("#000000"), 16);
        if (r.svg.isEmpty() || r.width <= 0 || r.height <= 0)
            return false;

        QSvgRenderer svg(r.svg);
        if (!svg.isValid())
            return false;
        QImage image(r.width, r.height, QImage::Format_ARGB32);
        image.fill(Qt::white);
        QPainter painter(&image);
        svg.render(&painter, QRectF(0, 0, r.width, r.height));
        painter.end();
        for (int y = 0; y < image.height(); ++y) {
            for (int x = 0; x < image.width(); ++x) {
                if (qGray(image.pixel(x, y)) < 250)
                    return true;
            }
        }
        return false;
    }

    void testRendersInlineMath()
    {
        QVERIFY(rendersToInk(QStringLiteral("e^{i\\pi}+1=0"), false));
        QVERIFY(rendersToInk(QStringLiteral("a_1^2+b_2^2"), false));
        QVERIFY(rendersToInk(QStringLiteral("x \\in \\mathbb{R}"), false));
    }

    void testRendersDisplayMath()
    {
        QVERIFY(rendersToInk(QStringLiteral("\\frac{-b \\pm \\sqrt{b^2-4ac}}{2a}"), true));
        QVERIFY(rendersToInk(QStringLiteral("\\begin{pmatrix}a & b \\\\ c & d\\end{pmatrix}"),
                             true));
        QVERIFY(rendersToInk(QStringLiteral("\\sum_{i=1}^{n} i^2"), true));
    }

    void testRejectsInvalidMath()
    {
        const auto r = KaTeXEngine::instance()->render(QStringLiteral("\\nonsense{"),
                                                       false, QStringLiteral("#000000"), 16);
        QVERIFY(r.svg.isEmpty());
    }
};

} // namespace

namespace Internal {

QObject *createKaTeXEngineTest()
{
    return new KaTeXEngineTest;
}

} // namespace Internal

} // namespace LlamaCpp

#include "katexengine_test.moc"

#endif // WITH_TESTS
