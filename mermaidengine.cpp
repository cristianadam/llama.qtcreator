#include "mermaidengine.h"

#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QFont>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QThread>
#include <QVariant>
#include <QtGlobal>

#include <llamatr.h>

#include <cstring>

using namespace LlamaCpp;

Q_LOGGING_CATEGORY(llamaChatMermaid, "llama.cpp.chat.mermaid", QtWarningMsg)

namespace {

QByteArray readResource(const QString &path)
{
    QFile file(path); // ":/..." resource paths; a real file of the same name wins
    if (file.open(QIODevice::ReadOnly))
        return file.readAll();
    return {};
}

constexpr int kMaxCacheEntries = 64;
constexpr qint64 kRenderTimeoutMs = 30'000;

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
        QFont f(family && family[0] ? QString::fromUtf8(family) : QStringLiteral("sans-serif"));
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
        // the layout is at least deterministic.
        width = double(t.size()) * sizePx * 0.6;
        ascent = sizePx * 0.8;
        descent = sizePx * 0.2;
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
    qCDebug(llamaChatMermaid) << message;
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
        qCWarning(llamaChatMermaid) << "JS error in" << name << ":"
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

// Qt's SVG module aborts a chunk of the element tree when it meets a
// <filter>/<fedropshadow> it does not understand (mermaid's "neo" look adds a
// drop shadow to boxes), silently dropping the neighbouring elements. Strip
// the filters and the references to them.
QByteArray stripFilters(const QByteArray &svg)
{
    QString out(QString::fromUtf8(svg));
    out.remove(QRegularExpression(QStringLiteral("<filter\\b[^>]*>.*?</filter>"),
                                 QRegularExpression::DotMatchesEverythingOption));
    out.remove(QRegularExpression(QStringLiteral("\\s*filter=\"url\\([^)]*\\)\"")));
    return out.toUtf8();
}

// Qt's SVG module supports <tspan> only as a direct child of <text>: a tspan
// nested in a tspan is dropped ("Could not add child element ... types are
// incorrect"), which leaves mermaid's node labels detached from their boxes.
// Mermaid emits exactly this shape for class/flowchart labels:
//   <tspan x=.. y=.. dy=.. class="text-outer-tspan row" font-weight="">
//     <tspan class="text-inner-tspan" font-weight="normal">+name:</tspan>
//     <tspan class="text-inner-tspan" font-weight="normal"> string</tspan>
//   </tspan>
// (one inner tspan per word fragment). Flatten the outer tspan into a single
// <tspan> whose text is the concatenation of the inner texts; the outer
// positioning attributes win, and the inner font-* attributes are carried
// over when the outer does not set them. Structures that would lose
// positioning are left alone.
QByteArray flattenNestedTspans(const QByteArray &svg)
{
    QString out(QString::fromUtf8(svg));
    // An outer tspan whose entire content is a sequence of inner tspans
    // (mermaid's word fragments; no interleaved bare text).
    const QRegularExpression re(QStringLiteral(
            "<tspan\\b([^>]*)>(\\s*<tspan\\b[^>]*>[^<]*</tspan>(?:\\s*<tspan\\b[^>]*>[^<]*</tspan>)*)\\s*</tspan>"));
    const QRegularExpression innerRe(QStringLiteral("<tspan\\b([^>]*)>([^<]*)</tspan>"));
    static const QStringList kPositioning
        = {QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("dx"),
           QStringLiteral("dy"), QStringLiteral("rotate"), QStringLiteral("startOffset")};
    static const QStringList kFontAttrs = {QStringLiteral("font-weight"),
                                           QStringLiteral("font-style")};
    auto attrValue = [](const QString &attrs, const QString &name) -> QString {
        const QString needle = name + QLatin1Char('=');
        int start = 0;
        while (true) {
            const int i = attrs.indexOf(needle, start);
            if (i < 0)
                return QString();
            // Attribute names are whitespace-separated: reject partial hits.
            if (i > 0 && !attrs.at(i - 1).isSpace()) {
                start = i + 1;
                continue;
            }
            int j = i + needle.size();
            while (j < attrs.size() && attrs.at(j).isSpace())
                ++j;
            if (j < attrs.size() && attrs.at(j) == QLatin1Char('"')) {
                const int e = attrs.indexOf(QLatin1Char('"'), j + 1);
                if (e > j)
                    return attrs.mid(j + 1, e - j - 1);
            }
            return QString();
        }
    };
    const auto hasPositioning = [&](const QString &attrs) {
        for (const QString &p : kPositioning)
            if (!attrValue(attrs, p).isEmpty())
                return true;
        return false;
    };
    for (int guard = 0; guard < 10; ++guard) {
        QString result;
        int lastEnd = 0;
        bool changed = false;
        QRegularExpressionMatchIterator it = re.globalMatch(out);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            const QString outer = m.captured(1);
            const QString inners = m.captured(2);
            const QString original = out.mid(m.capturedStart(0), m.capturedLength(0));

            // Collect the inner tspans.
            QStringList innerAttrs;
            QStringList innerTexts;
            QRegularExpressionMatchIterator innerIt = innerRe.globalMatch(inners);
            while (innerIt.hasNext()) {
                const QRegularExpressionMatch im = innerIt.next();
                innerAttrs.append(im.captured(1));
                innerTexts.append(im.captured(2));
            }
            if (innerAttrs.isEmpty() || innerAttrs.size() != innerTexts.size())
                continue;

            QString replacement;
            bool flatten = true;
            for (const QString &attrs : innerAttrs) {
                if (hasPositioning(attrs)) {
                    // Merging would lose the inner's positioning. The single-
                    // inner case is safe when the outer does not position
                    // either (the inner attributes then take over 1:1).
                    flatten = !(innerAttrs.size() == 1 && !hasPositioning(outer));
                    break;
                }
            }
            if (!flatten) {
                result += out.mid(lastEnd, m.capturedStart(0) - lastEnd) + original;
                lastEnd = m.capturedEnd(0);
                continue;
            }

            QString merged = outer;
            for (const QString &f : kFontAttrs) {
                QString innerValue;
                for (const QString &attrs : innerAttrs) {
                    innerValue = attrValue(attrs, f);
                    if (!innerValue.isEmpty())
                        break;
                }
                const QString outerValue = attrValue(outer, f);
                if (innerValue.isEmpty() || innerValue == outerValue)
                    continue;
                if (outerValue.isEmpty())
                    merged.replace(f + QStringLiteral("=\"\""),
                                   f + QStringLiteral("=\"") + innerValue + QStringLiteral("\""));
                else
                    merged += QStringLiteral(" ") + f + QStringLiteral("=\"")
                            + innerValue + QStringLiteral("\"");
            }
            if (innerAttrs.size() == 1 && hasPositioning(innerAttrs.first())) {
                // Carry the single inner's positioning over to the merged
                // tspan (the outer does not set any of it itself).
                for (const QString &p : kPositioning) {
                    const QString v = attrValue(innerAttrs.first(), p);
                    if (v.isEmpty())
                        continue;
                    if (merged.contains(p + QLatin1Char('=')))
                        continue;
                    merged += QStringLiteral(" ") + p + QStringLiteral("=\"") + v + QStringLiteral("\"");
                }
            }
            QString text;
            for (const QString &t : innerTexts)
                text += t;
            replacement = QStringLiteral("<tspan") + merged + '>' + text + "</tspan>";
            changed = true;

            result += out.mid(lastEnd, m.capturedStart(0) - lastEnd) + replacement;
            lastEnd = m.capturedEnd(0);
        }
        if (!changed)
            break;
        out = result + out.mid(lastEnd);
    }
    return out.toUtf8();
}

// Remove constructs Qt's SVG module cannot represent.
QByteArray sanitizeSvgForQt(const QByteArray &svg)
{
    return stripFilters(flattenNestedTspans(svg));
}

} // namespace

MermaidEngine *MermaidEngine::instance()
{
    static MermaidEngine engine;
    return &engine;
}

MermaidEngine::MermaidEngine()
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

MermaidEngine::~MermaidEngine()
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

void MermaidEngine::warmUp()
{
    QMutexLocker locker(&m_mutex);
    if (!m_worker->isRunning())
        m_worker->start();
}

bool MermaidEngine::isInitialized() const
{
    return m_ready.loadRelaxed() != 0;
}

QByteArray MermaidEngine::render(const QString &source, const QString &theme)
{
    const QByteArray keySource = source.toUtf8();
    const QByteArray cacheInput = keySource + '\0' + theme.toUtf8();
    const QString cacheKey = QString::fromLatin1(
            QCryptographicHash::hash(cacheInput, QCryptographicHash::Md5).toHex().left(16));

    RenderJob *job = nullptr;
    {
        QMutexLocker locker(&m_mutex);
        if (m_cache.contains(cacheKey))
            return m_cache.value(cacheKey);
        if (!m_worker->isRunning())
            m_worker->start();
        job = new RenderJob;
        job->source = source;
        job->theme = theme;
        m_jobs.push(job);
        m_jobAvailable.wakeOne();
    }

    if (!job->done.tryAcquire(1, kRenderTimeoutMs)) {
        qCWarning(llamaChatMermaid) << "render timed out after" << kRenderTimeoutMs << "ms";
        return {}; // the worker frees the job once it notices the reader gave up
    }
    const QByteArray svg = job->result;
    job->consumed.release();

    if (!svg.isEmpty()) {
        QMutexLocker locker(&m_mutex);
        if (m_cache.size() >= kMaxCacheEntries)
            m_cache.clear();
        m_cache.insert(cacheKey, svg);
    }
    return svg;
}

void MermaidEngine::workerMain()
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

        job->result = ready ? workerRender(job->source, job->theme) : QByteArray();
        job->done.release();
        // Hand the job back to the reader; it may have timed out and given up
        // (in which case this is the only owner and frees it after a while).
        job->consumed.tryAcquire(1, 5'000);
        delete job;
    }
    deinit();
}

bool MermaidEngine::ensureInitialized()
{
    if (m_ctx)
        return true;

    const QByteArray domShim = readResource(QStringLiteral(":/mermaid/domshim.js"));
    const QByteArray mermaidBundle = readResource(QStringLiteral(":/mermaid/mermaid.min.js"));
    if (domShim.isEmpty() || mermaidBundle.isEmpty()) {
        qCWarning(llamaChatMermaid) << "failed to load the bundled mermaid resources";
        return false;
    }

    m_rt = JS_NewRuntime();
    // Mermaid's bundled module graph has a deep top-level initialization
    // chain that overflows quickjs's default 1 MB stack budget (faster
    // still under ASAN), so give it more.
    JS_SetMaxStackSize(m_rt, 8 * 1024 * 1024);
    m_ctx = JS_NewContext(m_rt);

    JSValue global = JS_GetGlobalObject(m_ctx);
    JSValue host = JS_NewObject(m_ctx);
    JS_SetPropertyStr(m_ctx, host, "measureText", JS_NewCFunction(m_ctx, hostMeasureText, "__qtMeasureText", 5));
    JS_SetPropertyStr(m_ctx, host, "log", JS_NewCFunction(m_ctx, hostLog, "__qtLog", 0));
    JS_SetPropertyStr(m_ctx, global, "__qt", host);
    JS_FreeValue(m_ctx, global);

    // DOM shim first (it installs document/window and friends), then the
    // mermaid bundle, which evaluates its modules against the shim on load.
    if (!evalScript(m_ctx, domShim, "domshim.js")
        || !evalScript(m_ctx, mermaidBundle, "mermaid.min.js")) {
        deinit();
        return false;
    }
    return true;
}

void MermaidEngine::deinit()
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

QByteArray MermaidEngine::workerRender(const QString &source, const QString &theme)
{
    if (!ensureInitialized())
        return {};

    // Hand the input to the script via globals (avoids string escaping).
    JSValue global = JS_GetGlobalObject(m_ctx);
    JS_SetPropertyStr(m_ctx, global, "__qtInput",
                      JS_NewStringLen(m_ctx, source.toUtf8().constData(), source.toUtf8().size()));
    JS_SetPropertyStr(m_ctx, global, "__qtTheme", JS_NewString(m_ctx, theme.toUtf8().constData()));
    JS_SetPropertyStr(m_ctx, global, "__qtId", JS_NewInt32(m_ctx, m_nextDiagramId++));
    // Drop the previous render's result, otherwise the job pump below would
    // immediately "resolve" with the stale value.
    JSValue resultProp = JS_GetPropertyStr(m_ctx, global, "__qtResult");
    if (!JS_IsUndefined(resultProp))
        JS_SetPropertyStr(m_ctx, global, "__qtResult", JS_UNDEFINED);
    JS_FreeValue(m_ctx, resultProp);
    JS_FreeValue(m_ctx, global);

    // mermaid.initialize() must be called again before every render when the
    // theme can change between renders; it only updates the config. Note that
    // mermaid.render()'s third argument is a *containing element*, not a
    // config object - passing one makes d3.select() treat it as a DOM node
    // and the render fails.
    // The script runs in the global scope, so the const declarations are
    // wrapped in a block: re-declaring a top-level const on the next render
    // would be a SyntaxError.
    const QByteArray script =
        "{\n"
        "const p0 = (async () => {\n"
        "  mermaid.initialize({ startOnLoad: false, securityLevel: 'loose', htmlLabels: false, "
        "theme: __qtTheme, flowchart: { useMaxWidth: false } });\n"
        "  try {\n"
        "    const r = await mermaid.render('qtc-md-' + __qtId, __qtInput);\n"
        "    return JSON.stringify({ ok: true, svg: r.svg });\n"
        "  } catch (e) {\n"
        "    return JSON.stringify({ ok: false, error: String((e && e.message) || e), stack: e && e.stack ? String(e.stack) : '' });\n"
        "  }\n"
        "})();\n"
        "p0.then(r => { globalThis.__qtResult = r; },\n"
        "     e => { globalThis.__qtResult = JSON.stringify({ ok: false, error: String(e) }); });\n"
        "}";

    JSValue evalResult = JS_Eval(m_ctx, script.constData(), script.size(), "<mermaid-render>", JS_EVAL_TYPE_GLOBAL);
    JS_FreeValue(m_ctx, evalResult);
    if (JS_HasException(m_ctx)) {
        JSValue ex = JS_GetException(m_ctx);
        const char *msg = JS_ToCString(m_ctx, ex);
        qCWarning(llamaChatMermaid) << "render script failed:" << (msg ? msg : "(unknown)");
        JS_FreeCString(m_ctx, msg);
        JS_FreeValue(m_ctx, ex);
        return {};
    }

    // QuickJS has no event loop; pump its job queue (microtasks plus the
    // microtask-backed timers from the shim) until the promise settles.
    // This runs on the worker thread, so sleeping here never blocks the UI.
    QByteArray json;
    const QElapsedTimer timer;
    for (int spin = 0; spin < 100'000; ++spin) {
        if (timer.elapsed() > kRenderTimeoutMs) {
            qCWarning(llamaChatMermaid) << "render timed out after" << kRenderTimeoutMs << "ms";
            return {};
        }
        if (JS_ExecutePendingJob(m_rt, &m_ctx) <= 0)
            QThread::msleep(1);

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
    }
    if (json.isEmpty()) {
        qCWarning(llamaChatMermaid) << "render did not settle in time";
        return {};
    }

    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonObject obj = doc.object();
    if (!obj.value("ok").toBool()) {
        qCDebug(llamaChatMermaid) << "invalid diagram:" << obj.value("error").toString()
                                    << obj.value("stack").toString();
        return {};
    }
    const QByteArray svg = sanitizeSvgForQt(obj.value("svg").toString().toUtf8());
    return svg;
}
