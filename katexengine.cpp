#include "katexengine.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QThread>
#include <QtGlobal>

#include <llamatr.h>

#include <cstring>

using namespace LlamaCpp;

Q_LOGGING_CATEGORY(llamaChatKatex, "llama.cpp.chat.katex", QtWarningMsg)

namespace {

QByteArray readResource(const QString &path)
{
    QFile file(path); // ":/..." Qt resource path (resources only; a same-named
                      // file on disk is not consulted)
    if (file.open(QIODevice::ReadOnly))
        return file.readAll();
    return {};
}

constexpr int kMaxCacheEntries = 256;
constexpr qint64 kRenderTimeoutMs = 10'000;
// render() blocks the calling (GUI) thread; while the bundles are still
// being evaluated (warmUp() not finished) cap the wait so a slow first
// launch degrades to the verbatim-source fallback instead of freezing the
// UI. Ready engines get the full timeout.
constexpr qint64 kNotReadyTimeoutMs = 2'000;

// The KaTeX font faces bundled in katex/fonts/ (MIT, see katex/ATTRIBUTION.md).
// The family names match the @font-face names of KaTeX's own stylesheet
// (katex.css), which katex2svg.js maps glyph classes to.
const char *const kKatexFontFiles[] = {
    "KaTeX_AMS-Regular.ttf",
    "KaTeX_Caligraphic-Bold.ttf",
    "KaTeX_Caligraphic-Regular.ttf",
    "KaTeX_Fraktur-Bold.ttf",
    "KaTeX_Fraktur-Regular.ttf",
    "KaTeX_Main-Bold.ttf",
    "KaTeX_Main-BoldItalic.ttf",
    "KaTeX_Main-Italic.ttf",
    "KaTeX_Main-Regular.ttf",
    "KaTeX_Math-BoldItalic.ttf",
    "KaTeX_Math-Italic.ttf",
    "KaTeX_SansSerif-Bold.ttf",
    "KaTeX_SansSerif-Italic.ttf",
    "KaTeX_SansSerif-Regular.ttf",
    "KaTeX_Script-Regular.ttf",
    "KaTeX_Size1-Regular.ttf",
    "KaTeX_Size2-Regular.ttf",
    "KaTeX_Size3-Regular.ttf",
    "KaTeX_Size4-Regular.ttf",
    "KaTeX_Typewriter-Regular.ttf"
};

// __qt.measureText(text, family, sizePx, weight, fontStyle)
//     -> { width, ascent, descent, height }
//
// Called from the engine's worker thread. QFontMetricsF only needs the font
// database (initialized with the QGuiApplication), not the GUI event loop, so
// measuring off the GUI thread keeps the UI responsive during renders.
JSValue hostMeasureText(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Q_UNUSED(this_val)
    Q_UNUSED(argc)
    const char *text = JS_ToCString(ctx, argv[0]);
    const char *family = JS_ToCString(ctx, argv[1]);
    double sizePx = 14.0;
    JS_ToFloat64(ctx, &sizePx, argv[2]);
    int weight = 400;
    JS_ToInt32(ctx, &weight, argv[3]);
    const char *style = JS_ToCString(ctx, argv[4]);
    const QString t = text ? QString::fromUtf8(text) : QString();

    double width = 0, ascent = 0, descent = 0, height = 0;
    if (QGuiApplication::instance()) {
        QFont f(family && family[0] ? QString::fromUtf8(family) : QStringLiteral("serif"));
        f.setPixelSize(int(sizePx));
        f.setWeight(QFont::Weight(weight));
        if (style && strcmp(style, "italic") == 0)
            f.setItalic(true);
        QFontMetricsF metrics(f);
        width = metrics.horizontalAdvance(t);
        ascent = metrics.ascent();
        descent = metrics.descent();
        height = metrics.height();
    } else {
        // No GUI application (should not happen in the IDE): approximate so
        // the layout is at least deterministic. Keep the factors in sync
        // with MermaidEngine's hostMeasureText fallback.
        width = double(t.size()) * sizePx * 0.6;
        ascent = sizePx * 0.75;
        descent = sizePx * 0.25;
        height = sizePx;
    }

    JSValue obj = JS_NewObject(ctx);
    JS_SetPropertyStr(ctx, obj, "width", JS_NewFloat64(ctx, width));
    JS_SetPropertyStr(ctx, obj, "ascent", JS_NewFloat64(ctx, ascent));
    JS_SetPropertyStr(ctx, obj, "descent", JS_NewFloat64(ctx, descent));
    JS_SetPropertyStr(ctx, obj, "height", JS_NewFloat64(ctx, height));

    JS_FreeCString(ctx, text);
    JS_FreeCString(ctx, family);
    JS_FreeCString(ctx, style);
    return obj;
}

// __qt.log(message) -> console sink for JS-side diagnostics.
JSValue hostLog(JSContext *ctx, JSValueConst this_val, int argc, JSValueConst *argv)
{
    Q_UNUSED(this_val)
    Q_UNUSED(argc)
    QString message;
    for (int i = 0; i < argc; ++i) {
        const char *s = JS_ToCString(ctx, argv[i]);
        message += (i ? QLatin1String(" ") : QLatin1String(""))
                + (s ? QString::fromUtf8(s) : QStringLiteral("undefined"));
        JS_FreeCString(ctx, s);
    }
    qCDebug(llamaChatKatex) << message;
    return JS_UNDEFINED;
}

bool evalScript(JSContext *ctx, const QByteArray &script, const char *name)
{
    JSValue result = JS_Eval(ctx, script.constData(), script.size(), name, JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(result)) {
        JSValue ex = JS_GetException(ctx);
        JSValue stack = JS_GetPropertyStr(ctx, ex, "stack");
        const char *stackStr = JS_ToCString(ctx, stack);
        const char *msg = JS_ToCString(ctx, ex);
        qCWarning(llamaChatKatex) << "JS error in" << name << ":"
                               << (msg ? msg : "(unknown)")
                               << (stackStr ? stackStr : "");
        JS_FreeCString(ctx, msg);
        JS_FreeCString(ctx, stackStr);
        JS_FreeValue(ctx, stack);
        JS_FreeValue(ctx, ex);
    }
    JS_FreeValue(ctx, result);
    return !JS_HasException(ctx);
}

} // namespace

KaTeXEngine *KaTeXEngine::instance()
{
    static KaTeXEngine engine;
    return &engine;
}

KaTeXEngine::KaTeXEngine()
{
    m_worker = new QThread;
    // QuickJS needs a generous stack (see JS_SetMaxStackSize below); the
    // default macOS thread stack (512 KB) overflows during bundle eval.
    m_worker->setStackSize(32 * 1024 * 1024);
    // QThread::started is emitted on the worker thread, so the (directly
    // connected) slot runs there: the QuickJS runtime lives on that thread
    // for its whole lifetime.
    // No context object: the engine outlives the thread (process lifetime),
    // and the direct connection makes the slot run on the worker thread.
    QObject::connect(m_worker, &QThread::started, [this] { workerMain(); });
}

KaTeXEngine::~KaTeXEngine()
{
    {
        QMutexLocker locker(&m_mutex);
        m_quit = true;
    }
    m_jobAvailable.wakeAll();
    m_worker->quit();
    m_worker->wait();
    delete m_worker;
}

void KaTeXEngine::warmUp()
{
    registerFonts();
    QMutexLocker locker(&m_mutex);
    if (!m_worker->isRunning())
        m_worker->start();
}

void KaTeXEngine::registerFonts()
{
    if (m_fontsRegistered)
        return;
    m_fontsRegistered = true;
    for (const char *file : kKatexFontFiles) {
        const QString path = QStringLiteral(":/katex/fonts/%1").arg(file);
        if (QFontDatabase::addApplicationFont(path) < 0)
            qCWarning(llamaChatKatex) << "failed to load the KaTeX font" << path;
    }
}

bool KaTeXEngine::isInitialized() const
{
    return m_ready.loadRelaxed() != 0;
}

KaTeXEngine::Rendered KaTeXEngine::render(const QString &source, bool display,
                                          const QString &color, int fontSize)
{
    const QByteArray cacheInput = source.toUtf8() + (display ? '\1' : '\2')
            + '\0' + color.toUtf8() + '\0' + QByteArray::number(fontSize);
    const QString cacheKey = QString::fromLatin1(
            QCryptographicHash::hash(cacheInput, QCryptographicHash::Md5).toHex().left(16));

    // The caller (markdown renderer, tests) runs on the GUI thread; make the
    // bundled font faces available in case warmUp() was never called.
    registerFonts();

    RenderJob *job = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        if (m_cache.contains(cacheKey))
            return m_cache.value(cacheKey);
        if (!m_worker->isRunning())
            m_worker->start();
        job = new RenderJob;
        job->source = source;
        job->display = display;
        job->color = color;
        job->fontSize = fontSize;
        m_jobs.push(job);
        m_jobAvailable.wakeOne();
    }

    const qint64 timeout = m_ready.loadRelaxed() ? kRenderTimeoutMs : kNotReadyTimeoutMs;
    if (!job->done.tryAcquire(1, timeout)) {
        qCWarning(llamaChatKatex) << "render timed out after" << timeout << "ms";
        return {}; // the worker frees the job once it notices the reader gave up
    }
    const Rendered rendered = job->result;
    job->consumed.release();

    if (!rendered.svg.isEmpty()) {
        QMutexLocker locker(&m_mutex);
        if (m_cache.size() >= kMaxCacheEntries)
            m_cache.clear();
        m_cache.insert(cacheKey, rendered);
    }
    return rendered;
}

void KaTeXEngine::workerMain()
{
    bool ready = ensureInitialized();
    m_ready.storeRelaxed(ready ? 1 : 0);

    for (;;) {
        RenderJob *job = nullptr;
        {
            QMutexLocker locker(&m_mutex);
            while (m_jobs.empty() && !m_quit)
                m_jobAvailable.wait(&m_mutex);
            if (m_quit && m_jobs.empty())
                break;
            if (!m_jobs.empty()) {
                job = m_jobs.front();
                m_jobs.pop();
            }
        }
        if (!job)
            continue;

        job->result = ready ? workerRender(job->source, job->display, job->color, job->fontSize)
                            : Rendered();
        job->done.release();
        // Hand the job back to the reader; it may have timed out and given
        // up (in which case this is the only owner and frees it later).
        job->consumed.tryAcquire(1, 5'000);
        delete job;
    }
    deinit();
}

bool KaTeXEngine::ensureInitialized()
{
    if (m_ctx)
        return true;

    const QByteArray katexBundle = readResource(QStringLiteral(":/katex/katex.min.js"));
    const QByteArray katex2svg = readResource(QStringLiteral(":/katex/katex2svg.js"));
    if (katexBundle.isEmpty() || katex2svg.isEmpty()) {
        qCWarning(llamaChatKatex) << "failed to load the bundled katex resources";
        return false;
    }

    m_rt = JS_NewRuntime();
    JS_SetMaxStackSize(m_rt, 8 * 1024 * 1024);
    m_ctx = JS_NewContext(m_rt);

    JSValue global = JS_GetGlobalObject(m_ctx);
    JSValue host = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, host, "measureText", JS_NewCFunction(m_ctx, hostMeasureText, "__qtMeasureText", 5));
    JS_SetPropertyStr(m_ctx, host, "log", JS_NewCFunction(m_ctx, hostLog, "__qtLog", 0));
    JS_SetPropertyStr(m_ctx, global, "__qt", host);
    JS_FreeValue(m_ctx, global);

    // KaTeX's UMD bundle installs globalThis.katex; katex2svg.js (which
    // needs it) installs globalThis.__katexToSvg. Neither needs a DOM.
    if (!evalScript(m_ctx, katexBundle, "katex.min.js")
        || !evalScript(m_ctx, katex2svg, "katex2svg.js")) {
        deinit();
        return false;
    }
    return true;
}

void KaTeXEngine::deinit()
{
    if (m_ctx) {
        JS_FreeContext(m_ctx);
        m_ctx = nullptr;
    }
    if (m_rt) {
        JS_FreeRuntime(m_rt);
        m_rt = nullptr;
    }
}

KaTeXEngine::Rendered KaTeXEngine::workerRender(const QString &source, bool display,
                                                const QString &color, int fontSize)
{
    if (!ensureInitialized())
        return {};

    // Hand the input to the script via globals (avoids string escaping).
    JSValue global = JS_GetGlobalObject(m_ctx);
    JS_SetPropertyStr(m_ctx, global, "__qtInput",
                      JS_NewStringLen(m_ctx, source.toUtf8().constData(), source.toUtf8().size()));
    JS_SetPropertyStr(m_ctx, global, "__qtDisplay", JS_NewBool(m_ctx, display));
    JS_SetPropertyStr(m_ctx, global, "__qtColor", JS_NewString(m_ctx, color.toUtf8().constData()));
    JS_SetPropertyStr(m_ctx, global, "__qtFontSize", JS_NewInt32(m_ctx, fontSize));
    // Drop the previous render's result, otherwise the job pump below would
    // immediately "resolve" with the stale value.
    JSValue resultProp = JS_GetPropertyStr(m_ctx, global, "__qtResult");
    if (!JS_IsUndefined(resultProp))
        JS_SetPropertyStr(m_ctx, global, "__qtResult", JS_UNDEFINED);
    JS_FreeValue(m_ctx, resultProp);
    JS_FreeValue(m_ctx, global);

    const QByteArray script =
        "(function () {\n"
        "  try {\n"
        "    var r = __katexToSvg(__qtInput, { displayMode: __qtDisplay, fontSize: __qtFontSize, color: __qtColor });\n"
        "    globalThis.__qtResult = r ? JSON.stringify(r) : JSON.stringify({ ok: false });\n"
        "  } catch (e) {\n"
        "    globalThis.__qtResult = JSON.stringify({ ok: false, error: String((e && e.message) || e) });\n"
        "  }\n"
        "})();";

    JSValue evalResult = JS_Eval(m_ctx, script.constData(), script.size(), "<katex-render>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(m_ctx, evalResult);
    if (JS_HasException(m_ctx)) {
        JSValue ex = JS_GetException(m_ctx);
        const char *msg = JS_ToCString(m_ctx, ex);
        qCWarning(llamaChatKatex) << "render script failed:" << (msg ? msg : "(unknown)");
        JS_FreeCString(m_ctx, msg);
        JS_FreeValue(m_ctx, ex);
        return {};
    }

    // KaTeX renders synchronously: the eval above has set the result
    // already (pump the job queue in case of stray microtasks first).
    const QElapsedTimer timer;
    QByteArray json;
    for (int spin = 0; spin < 1'000; ++spin) {
        if (timer.elapsed() > kRenderTimeoutMs)
            break;
        // Note: JS_ExecutePendingJob() sets *pctx to NULL when the queue is
        // empty, so hand it a local, never &m_ctx.
        JSContext *jobCtx = nullptr;
        if (JS_ExecutePendingJob(m_rt, &jobCtx) > 0)
            continue;
        JSValue g = JS_GetGlobalObject(m_ctx);
        JSValue result = JS_GetPropertyStr(m_ctx, g, "__qtResult");
        JS_FreeValue(m_ctx, g);
        if (!JS_IsUndefined(result)) {
            const char *s = JS_ToCString(m_ctx, result);
            if (s) {
                json = s;
                JS_FreeCString(m_ctx, s);
            }
            JS_FreeValue(m_ctx, result);
            break;
        }
        JS_FreeValue(m_ctx, result);
        QThread::msleep(1);
    }
    if (json.isEmpty()) {
        qCWarning(llamaChatKatex) << "render did not settle in time";
        return {};
    }

    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonObject obj = doc.object();
    if (obj.contains("ok") && !obj.value("ok").toBool()) {
        qCDebug(llamaChatKatex) << "invalid math:" << obj.value("error").toString();
        return {};
    }
    Rendered rendered;
    rendered.svg = obj.value("svg").toString().toUtf8();
    rendered.width = obj.value("width").toDouble();
    rendered.height = obj.value("height").toDouble();
    rendered.baseline = obj.value("baseline").toDouble();
    if (rendered.svg.isEmpty() || rendered.width <= 0 || rendered.height <= 0)
        return {};
    return rendered;
}
