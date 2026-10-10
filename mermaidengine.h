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

// Renders mermaid diagrams to SVG by running the mermaid.js bundle inside the
// QuickJS engine that Qt Creator ships (QtCreator::quickjsng). Mermaid's
// render() pipeline expects a DOM, so a minimal DOM shim (3rdparty/mermaid/domshim.js,
// loaded into the same context) provides one; text measurement is bridged to
// Qt via a host function so diagram layout uses the real system fonts.
//
// The QuickJS runtime is not thread-safe and must be used from a single
// thread, so the engine owns a dedicated worker thread: the one-time cost of
// evaluating the ~5 MB bundle (warmUp()) and every render run there. render()
// can be called from any thread (typically the GUI thread) and blocks until
// the worker finishes; call warmUp() early (plugin startup) so the first
// diagram does not pay the initialization cost.
//
// Results are cached per (source, theme) pair.
class MermaidEngine
{
public:
    static MermaidEngine *instance();

    // Synchronously renders \a source (a ```mermaid code block body) to SVG.
    // Returns an empty QByteArray (and logs a warning) when the diagram is
    // invalid or rendering times out; the caller should fall back to showing
    // the source as a regular code block.
    //
    // Blocks the calling thread until the worker is done: use renderAsync()
    // from the GUI thread so the UI stays responsive.
    //
    // \a theme is a mermaid theme name ("default", "dark", ...).
    QByteArray render(const QString &source, const QString &theme);

    // Asynchronous counterpart of render(): the render runs on the worker
    // thread and \a callback is invoked on the thread of \a context (queued,
    // never inline) with the resulting SVG, or with an empty QByteArray when
    // the diagram is invalid or the render times out. A cached result is
    // delivered immediately (still queued, so the callback can never reenter
    // a document edit in flight). \a context must live on a thread with a
    // running event loop; if it is destroyed before the render finishes the
    // queued invocation is simply dropped (callers should treat a missing
    // callback as "the document moved on", as
    // MarkdownRenderer::onMermaidRendered does).
    void renderAsync(const QString &source, const QString &theme, QObject *context,
                     const std::function<void(const QByteArray &svg)> &callback);

    // Starts the worker thread and evaluates the mermaid bundle in the
    // background. Cheap and idempotent; call it at plugin startup so the
    // one-time initialization cost is paid off the critical path.
    void warmUp();

    bool isInitialized() const;

private:
    MermaidEngine();
    ~MermaidEngine();

    // Runs on the worker thread: evaluates the bundle, then services the
    // render job queue until the engine is destroyed.
    void workerMain();
    // Worker-thread only: renders one diagram (bundle already evaluated).
    QByteArray workerRender(const QString &source, const QString &theme);

    // Worker-thread only.
    bool ensureInitialized();
    void deinit();

    struct RenderJob
    {
        QString source;
        QString theme;
        QByteArray result;
        QSemaphore done;      // released by the worker once result is filled
        QSemaphore consumed;  // released by the reader (skipped on timeout)
        // Async jobs: the worker invokes the callback on the context's thread
        // (done/consumed are unused for those).
        QObject *context = nullptr;
        std::function<void(const QByteArray &svg)> callback;
    };

    QThread *m_worker = nullptr;
    QMutex m_mutex;   // guards m_jobs, m_cache, m_quit
    QWaitCondition m_jobAvailable;
    std::queue<RenderJob *> m_jobs;
    bool m_quit = false;
    QAtomicInt m_ready = 0;  // bundle evaluated (worker thread)
    JSRuntime *m_rt = nullptr;
    JSContext *m_ctx = nullptr;
    QHash<QString, QByteArray> m_cache;  // md5(source + '\0' + theme) -> svg
    int m_nextDiagramId = 0;
};

#ifdef WITH_TESTS
namespace Internal {
// Test object for the "Qt Creator -test llamacpp" run; see
// tools/mermaidengine_test.cpp.
QObject *createMermaidEngineTest();
} // namespace Internal
#endif // WITH_TESTS

} // namespace LlamaCpp
