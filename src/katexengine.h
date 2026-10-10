#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QHash>
#include <QMutex>
#include <QSemaphore>
#include <QString>
#include <QWaitCondition>

#include <functional>
#include <queue>

extern "C" {
#include <quickjs.h>
}

class QObject;
class QThread;

namespace LlamaCpp {

// Renders LaTeX math to a standalone SVG by running KaTeX (katex.min.js)
// inside the QuickJS engine that Qt Creator ships (QtCreator::quickjsng) and
// flattening KaTeX's HTML output to SVG (3rdparty/katex/katex2svg.js, in the
// same context). Text measurement is bridged to Qt via a host function so
// glyph widths use the real KaTeX font faces, which are bundled as resources
// (3rdparty/katex/fonts/) and registered as application fonts.
//
// Like MermaidEngine, the QuickJS runtime is confined to a dedicated worker
// thread: the one-time cost of evaluating the bundles (warmUp()) and every
// render run there. render() can be called from any thread (typically the
// GUI thread) and blocks until the worker finishes; KaTeX renders are fast
// (a few ms), so math is rendered synchronously by the markdown renderer.
// Call warmUp() early (plugin startup) so the first formula does not pay
// the initialization cost.
//
// Results are cached per (source, display, color, size) tuple.
class KaTeXEngine
{
public:
    static KaTeXEngine *instance();

    struct Rendered
    {
        QByteArray svg;     // empty when the math is invalid
        double width = 0;   // px
        double height = 0;  // px
        double baseline = 0; // px from the top of the image to the math baseline
    };

    // Synchronously renders \a source (LaTeX, the content of a $...$ or
    // $$...$$ span) to SVG. \a display selects display mode ($$...$$),
    // \a color is the SVG fill colour (the surrounding text colour), \a
    // fontSize the surrounding font size in px. Returns an empty svg (and
    // logs a warning) when the math is invalid or the render times out;
    // the caller should fall back to showing the source as plain text.
    //
    // Blocks the calling thread until the worker is done; renders are fast,
    // and repeated feeds hit the cache.
    Rendered render(const QString &source, bool display, const QString &color, int fontSize);

    // Starts the worker thread and evaluates the bundles in the background.
    // Cheap and idempotent; call it at plugin startup so the one-time
    // initialization cost is paid off the critical path.
    void warmUp();

    bool isInitialized() const;

private:
    KaTeXEngine();
    ~KaTeXEngine();

    // Runs on the worker thread: evaluates the bundles, then services the
    // render job queue until the engine is destroyed.
    void workerMain();
    // Worker-thread only: renders one formula (bundles already evaluated).
    Rendered workerRender(const QString &source, bool display, const QString &color, int fontSize);

    // Worker-thread only.
    bool ensureInitialized();
    void deinit();

    // GUI-thread only (QFontDatabase): registers the bundled KaTeX font
    // faces as application fonts. Idempotent; called from warmUp() and
    // render() so the faces are available no matter which one ran first.
    void registerFonts();

    struct RenderJob
    {
        QString source;
        bool display = false;
        QString color;
        int fontSize = 0;
        Rendered result;
        QSemaphore done;      // released by the worker once result is filled
        QSemaphore consumed;  // released by the reader (skipped on timeout)
    };

    QThread *m_worker = nullptr;
    QMutex m_mutex;   // guards m_jobs, m_cache, m_quit
    QWaitCondition m_jobAvailable;
    std::queue<RenderJob *> m_jobs;
    bool m_quit = false;
    QAtomicInt m_ready = 0;  // bundles evaluated (worker thread)
    JSRuntime *m_rt = nullptr;
    JSContext *m_ctx = nullptr;
    bool m_fontsRegistered = false;  // GUI thread
    QHash<QString, Rendered> m_cache;
};

#ifdef WITH_TESTS
namespace Internal {
// Test object for the "Qt Creator -test llamacpp" run; see
// tools/katexengine_test.cpp.
QObject *createKaTeXEngineTest();
} // namespace Internal
#endif // WITH_TESTS

} // namespace LlamaCpp
