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

#include <algorithm>
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

// mermaid's neo renderer draws the section dividers of a class with no
// member rows using the box's *height* as width, so the divider lines
// stick out of the box on both sides (mermaid's own layout math for the
// degenerate no-members case; visible in browsers too). Clamp each
// divider path to the x range of its node's box. A divider is always a
// horizontal line at the y of its first point. Runs on the finished SVG:
// at generation time mermaid sets the divider's d before the box path
// exists (the box is re-inserted at ":first-child" afterwards).
namespace {

// One past the '</g>' that closes the group opened at openTagStart.
int closeGroupAt(const QString &s, int openTagStart)
{
    int depth = 0;
    int i = openTagStart;
    while (i < s.size()) {
        int open = s.indexOf(QLatin1String("<g"), i);
        if (open >= 0 && (open + 2 >= s.size() || !s.at(open + 2).isLetterOrNumber()))
            ;
        else
            open = -1;
        const int close = s.indexOf(QLatin1String("</g>"), i);
        if (close < 0)
            return -1;
        if (open >= 0 && open < close) {
            const int tagEnd = s.indexOf(QLatin1Char('>'), open);
            if (tagEnd < 0)
                return -1;
            if (s.at(tagEnd - 1) != QLatin1Char('/'))
                ++depth;
            i = tagEnd + 1;
        } else {
            if (--depth == 0)
                return close + 4;
            i = close + 4;
        }
    }
    return -1;
}

// The x range of the first straight-line (M/L) box path in a node group.
bool boxXRange(const QString &node, double &x1, double &x2)
{
    static const QRegularExpression pathRe(QStringLiteral("<path\\b[^>]*\\bd=\"([^\"]*)\""));
    static const QRegularExpression cmdRe(QStringLiteral("[A-Za-z]"));
    static const QRegularExpression numRe(QStringLiteral("-?[\\d.]+"));
    for (auto it = pathRe.globalMatch(node); it.hasNext();) {
        const QString d = it.next().captured(1);
        bool straight = true;
        for (auto cIt = cmdRe.globalMatch(d); cIt.hasNext();) {
            const QChar c = cIt.next().captured().at(0);
            if (!QStringLiteral("MmLlZz").contains(c)) {
                straight = false;
                break;
            }
        }
        if (!straight)
            continue;
        QVector<double> nums;
        for (auto nIt = numRe.globalMatch(d); nIt.hasNext();)
            nums.append(nIt.next().captured().toDouble());
        if (nums.size() < 4 || nums.size() % 2 != 0)
            continue;
        x1 = x2 = nums.first();
        for (int i = 0; i < nums.size(); i += 2) {
            x1 = qMin(x1, nums.at(i));
            x2 = qMax(x2, nums.at(i));
        }
        if (x2 > x1)
            return true;
    }
    return false;
}

} // namespace

QByteArray clampDividersToBoxes(const QByteArray &svg)
{
    const QString input = QString::fromUtf8(svg);
    const QRegularExpression nodeRe(QStringLiteral(
        R"qc(<g\b[^>]*\bclass="[^"]*\bnode\b[^"]*"[^>]*>)qc"));
    const QRegularExpression dividerRe(
        R"qc(<g\b[^>]*\bclass="[^"]*\bdivider\b[^"]*"[^>]*>\s*<path\b[^>]*\bd="([^"]*)"[^>]*/?>\s*</g>)qc");
    const QRegularExpression numRe(QStringLiteral("-?[\\d.]+"));
    struct Fix {
        int start;
        int length;
        QString replacement;
    };
    QString out;
    int lastEnd = 0;
    bool changed = false;
    for (auto it = nodeRe.globalMatch(input); it.hasNext();) {
        const QRegularExpressionMatch node = it.next();
        const int nodeEnd = closeGroupAt(input, node.capturedStart());
        if (nodeEnd < 0)
            continue;
        const QString nodeText = input.mid(node.capturedStart(), nodeEnd - node.capturedStart());
        double x1 = 0;
        double x2 = 0;
        if (!boxXRange(nodeText, x1, x2))
            continue;
        QVector<Fix> fixes;
        for (auto dIt = dividerRe.globalMatch(nodeText); dIt.hasNext();) {
            const QRegularExpressionMatch div = dIt.next();
            const QString d = div.captured(1);
            QVector<double> nums;
            for (auto nIt = numRe.globalMatch(d); nIt.hasNext();)
                nums.append(nIt.next().captured().toDouble());
            if (nums.size() < 2)
                continue;
            // Already inside the box: leave it alone.
            if (nums.first() >= x1 - 0.5 && nums.at(nums.size() - 2) <= x2 + 0.5)
                continue;
            const double y = nums.at(1);
            const QString fmt = QStringLiteral("M%1 %2L%3 %2")
                                      .arg(QString::number(x1, 'g', 12))
                                      .arg(QString::number(y, 'g', 12))
                                      .arg(QString::number(x2, 'g', 12));
            fixes.append({static_cast<int>(div.capturedStart(1)),
                          static_cast<int>(div.capturedLength(1)), fmt});
        }
        if (fixes.isEmpty())
            continue;
        changed = true;
        QString processed = nodeText;
        // Apply back-to-front so earlier offsets stay valid.
        std::sort(fixes.begin(), fixes.end(),
                  [](const Fix &a, const Fix &b) { return a.start > b.start; });
        for (const Fix &fix : fixes)
            processed.remove(fix.start, fix.length).insert(fix.start, fix.replacement);
        out += input.mid(lastEnd, node.capturedStart() - lastEnd) + processed;
        lastEnd = nodeEnd;
    }
    if (!changed)
        return svg;
    return (out + input.mid(lastEnd)).toUtf8();
}

// Tighten the root viewBox to the drawn content.
//
// mermaid sizes the root <svg> from its *layout*'s bounding box (the dagre
// graph size plus 8px padding), not from what is actually drawn: node
// groups are translated to their centers and drawn symmetrically around the
// local origin, and dagre reserves column spacing and edge-label slots. The
// drawn content therefore sits in a smaller, top-left-biased sub-region of
// the viewBox, and a canvas sized from the viewBox shows the diagram
// off-center with dead space (most visible for class diagrams and for
// single-rank flowcharts, which come out several times taller than their
// content). Walk the element tree, accumulate the bounds of all drawn
// geometry (paths, rects, lines, polygons, circles, and text measured with
// QFontMetricsF), and rewrite the viewBox and the root's max-width style to
// the tight bounds plus mermaid's own 8px padding.
namespace {

struct TFrame
{
    double dx = 0;
    double dy = 0;
    double sx = 1;
    double sy = 1;
    double x(double v) const { return dx + sx * v; }
    double y(double v) const { return dy + sy * v; }
};

struct TBounds
{
    bool have = false;
    double minX = 0;
    double minY = 0;
    double maxX = 0;
    double maxY = 0;
    void extend(double x, double y)
    {
        if (!have) {
            minX = maxX = x;
            minY = maxY = y;
            have = true;
            return;
        }
        minX = qMin(minX, x);
        maxX = qMax(maxX, x);
        minY = qMin(minY, y);
        maxY = qMax(maxY, y);
    }
};

bool tAttr(const QString &attrs, const QString &name, double &out)
{
    const QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(name)
                               + QStringLiteral("=\"([-0-9.]+)\""));
    const QRegularExpressionMatch m = re.match(attrs);
    if (!m.hasMatch())
        return false;
    out = m.captured(1).toDouble();
    return true;
}

// Parse a transform="translate(a, b) [scale(k)]" into a frame relative to
// the parent. mermaid only emits translate; anything else (rotate, matrix)
// is treated as identity, which at worst leaves a bit of extra margin.
TFrame childFrame(const TFrame &parent, const QString &attrs)
{
    TFrame f = parent;
    double tx = 0;
    double ty = 0;
    double kx = 1;
    double ky = 1;
    const QRegularExpression trRe(QStringLiteral(
        "translate\\(\\s*([-0-9.e]+)[,\\s]+([-0-9.e]+)\\s*\\)"));
    const QRegularExpressionMatch tr = trRe.match(attrs);
    if (tr.hasMatch()) {
        tx = tr.captured(1).toDouble();
        ty = tr.captured(2).toDouble();
    }
    const QRegularExpression scRe(QStringLiteral("scale\\(\\s*([-0-9.e]+)\\s*\\)"));
    const QRegularExpressionMatch sc = scRe.match(attrs);
    if (sc.hasMatch()) {
        kx = ky = sc.captured(1).toDouble();
    }
    f.dx = parent.dx + parent.sx * tx;
    f.dy = parent.dy + parent.sy * ty;
    f.sx = parent.sx * kx;
    f.sy = parent.sy * ky;
    return f;
}

// Parse a path d-attribute, adding every coordinate (mapped through f) to
// b. Curve control points can slightly overshoot the drawn curve, which is
// a safe over-approximation for padding purposes.
void accumulatePath(const QString &d, const TFrame &f, TBounds &b)
{
    static const QRegularExpression numRe(QStringLiteral(
        "[-+]?(?:\\d+\\.?\\d*|\\.\\d+)(?:[eE][-+]?\\d+)?"));
    struct Seg
    {
        QChar cmd;
        QVector<double> nums;
    };
    QVector<Seg> segs;
    QChar implicitCmd = QChar();
    for (int i = 0; i < d.size(); ++i) {
        const QChar c = d.at(i);
        if (c.isSpace())
            continue;
        if (c.isLetter()) {
            segs.append({c, {}});
            // A repeated parameter list after M continues as L.
            implicitCmd = c.toUpper() == QLatin1Char('M')
                            ? (c.isLower() ? QLatin1Char('l') : QLatin1Char('L'))
                            : c;
            continue;
        }
        const QRegularExpressionMatch m = numRe.match(d, i);
        if (!m.hasMatch())
            continue;
        if (segs.isEmpty() && implicitCmd.isNull())
            implicitCmd = QLatin1Char('M');
        if (segs.isEmpty() || !segs.last().cmd.unicode()) {
            segs.append({implicitCmd, {}});
        }
        segs.last().nums.append(m.captured().toDouble());
        i = m.capturedEnd() - 1;
    }

    double lx = 0;
    double ly = 0;
    double lsx = 0;
    double lsy = 0;
    for (const Seg &seg : segs) {
        QChar c = seg.cmd.toUpper();
        const bool rel = seg.cmd.isLower();
        const QVector<double> &n = seg.nums;
        auto ap = [&](double x, double &outX, double y, double &outY) {
            if (rel) {
                outX = lx + x;
                outY = ly + y;
            } else {
                outX = x;
                outY = y;
            }
        };
        int k = 0;
        if (c.unicode() == 'Z' && n.isEmpty()) {
            // Close the subpath: the pen returns to its start.
            lx = lsx;
            ly = lsy;
            continue;
        }
        while (k < n.size()) {
            switch (c.unicode()) {
            case 'M':
            case 'L': {
                double x;
                double y;
                ap(n.at(k), x, n.at(k + 1), y);
                k += 2;
                lx = x;
                ly = y;
                if (c.unicode() == 'M') {
                    lsx = lx;
                    lsy = ly;
                    c = rel ? QLatin1Char('l') : QLatin1Char('L');
                }
                b.extend(f.x(lx), f.y(ly));
                break;
            }
            case 'H': {
                double x;
                ap(n.at(k), x, 0, ly);
                k += 1;
                lx = x;
                b.extend(f.x(lx), f.y(ly));
                break;
            }
            case 'V': {
                double y;
                ap(0, lx, n.at(k), y);
                k += 1;
                ly = y;
                b.extend(f.x(lx), f.y(ly));
                break;
            }
            case 'C':
            case 'S':
            case 'Q': {
                const int pairs = c.unicode() == 'C' ? 3 : 2;
                double x = lx;
                double y = ly;
                for (int p = 0; p < pairs; ++p) {
                    ap(n.at(k), x, n.at(k + 1), y);
                    k += 2;
                    b.extend(f.x(x), f.y(y));
                }
                lx = x;
                ly = y;
                break;
            }
            case 'T': {
                double x;
                double y;
                ap(n.at(k), x, n.at(k + 1), y);
                k += 2;
                lx = x;
                ly = y;
                b.extend(f.x(lx), f.y(ly));
                break;
            }
            case 'A': {
                // rx ry x-rotation large-arc-flag sweep-flag x y
                const double x = n.at(k + 5);
                const double y = n.at(k + 6);
                k += 7;
                if (rel) {
                    lx += x;
                    ly += y;
                } else {
                    lx = x;
                    ly = y;
                }
                b.extend(f.x(lx), f.y(ly));
                break;
            }
            case 'Z':
                lx = lsx;
                ly = lsy;
                break;
            default:
                return; // unknown command: give up on this path
            }
        }
    }
}

QString decodeEntities(const QString &s)
{
    QString r = s;
    r.replace(QLatin1String("&#39;"), QLatin1String("'"));
    r.replace(QLatin1String("&quot;"), QLatin1String("\""));
    r.replace(QLatin1String("&lt;"), QLatin1String("<"));
    r.replace(QLatin1String("&gt;"), QLatin1String(">"));
    r.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return r;
}

// Measure text the way the DOM shim does (QFontMetricsF on the worker
// thread), with a small slack so an under-estimate cannot clip a label.
struct TTextSize
{
    double width;
    double ascent;
    double descent;
};
TTextSize measureText(const QString &text, double sizePx)
{
    if (QGuiApplication::instance()) {
        QFont f(QStringLiteral("sans-serif"));
        f.setPixelSize(qMax(1, int(sizePx)));
        QFontMetricsF m(f);
        return {m.horizontalAdvance(text) * 1.05, m.ascent(), m.descent()};
    }
    return {double(text.size()) * sizePx * 0.65, sizePx * 0.8, sizePx * 0.2};
}

void accumulateText(const QString &tag, const QString &content,
                    const TFrame &f, TBounds &b)
{
    double x = 0;
    double y = 0;
    double fontSize = 16;
    double dy = 0;
    tAttr(tag, QStringLiteral("x"), x);
    tAttr(tag, QStringLiteral("y"), y);
    tAttr(tag, QStringLiteral("font-size"), fontSize);
    // dy is usually in em ("1em", "0.3em"); a bare number is px.
    double dyy = 0;
    if (tAttr(tag, QStringLiteral("dy"), dyy)) {
        const int emPos = tag.indexOf(QStringLiteral("dy=\""));
        const QString v = tag.mid(emPos + 4).left(
            tag.mid(emPos + 4).indexOf(QLatin1Char('"')));
        dy = v.endsWith(QLatin1String("em")) ? dyy * fontSize : dyy;
    }
    QString anchor = QStringLiteral("start");
    const QRegularExpression anchorRe(QStringLiteral(
        "text-anchor\\s*\\s*[:=]\\s*\\\"?\\s*(start|middle|end)"));
    const QRegularExpressionMatch am = anchorRe.match(tag);
    if (am.hasMatch())
        anchor = am.captured(1);

    y += dy;
    const TTextSize size = measureText(content, fontSize);
    double x1 = x;
    double x2 = x + size.width;
    if (anchor == QLatin1String("middle")) {
        x1 = x - size.width / 2.0;
        x2 = x + size.width / 2.0;
    } else if (anchor == QLatin1String("end")) {
        x1 = x - size.width;
        x2 = x;
    }
    b.extend(f.x(x1), f.y(y - size.ascent));
    b.extend(f.x(x2), f.y(y + size.descent));
}

} // namespace

QByteArray tightenViewBoxToContent(const QByteArray &svg)
{
    const QString input = QString::fromUtf8(svg);
    const int rootEnd = input.indexOf(QLatin1Char('>'));
    if (rootEnd < 0)
        return svg;
    const QRegularExpression vbRe(QStringLiteral(
        "\\bviewBox=\"([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)\\s+([-0-9.]+)\""));
    const QRegularExpressionMatch vb = vbRe.match(input.left(rootEnd));
    if (!vb.hasMatch())
        return svg;

    TBounds b;
    QVector<TFrame> stack{{}};
    // Note: the attribute capture must include a trailing '/' of a
    // self-closing tag (<g .../>) so selfClosing can be detected; a
    // separate /? before > would eat the slash out of the capture.
    const QRegularExpression tagRe(QStringLiteral(
        "<(g|path|rect|line|polygon|circle|text|style|defs|marker|symbol)\\b([^>]*)>|</(g|text|style|defs|marker|symbol)>"));
    static const QRegularExpression tagStripRe(QStringLiteral("<[^>]+>"));

    // "style", "defs", "marker" or "symbol" while inside such a block. None
    // of them contributes document-coordinate geometry: defs are
    // indirection, and marker/symbol content lives in its own coordinate
    // system, stamped at the use site (mermaid emits the "-margin"
    // arrowhead markers directly under the root <svg>, not in a defs, so
    // not skipping them drags their local coordinates into the bounds).
    QString skipper;
    int pos = rootEnd + 1;
    for (auto it = tagRe.globalMatch(input, pos); it.hasNext(); ) {
        const QRegularExpressionMatch m = it.next();
        if (!skipper.isEmpty()) {
            if (m.captured(3) == skipper)
                skipper.clear();
            pos = m.capturedEnd();
            continue;
        }
        const QString tag = m.captured(1);
        const QString attrs = m.captured(2);
        if (tag == QLatin1String("style") || tag == QLatin1String("defs")
            || tag == QLatin1String("marker") || tag == QLatin1String("symbol")) {
            if (!attrs.endsWith(QLatin1Char('/')))
                skipper = tag;
            pos = m.capturedEnd();
            continue;
        }
        if (!m.captured(3).isEmpty()) {
            // A closing tag.
            if (tag.isEmpty() && m.captured(3) == QLatin1String("g"))
                stack.removeLast();
            pos = m.capturedEnd();
            continue;
        }
        const bool selfClosing = attrs.endsWith(QLatin1Char('/'))
                || tag == QLatin1String("path")
                || tag == QLatin1String("rect")
                || tag == QLatin1String("line")
                || tag == QLatin1String("polygon")
                || tag == QLatin1String("circle");
        if (tag == QLatin1String("g")) {
            if (!selfClosing)
                stack.append(childFrame(stack.last(), attrs));
            pos = m.capturedEnd();
            continue;
        }
        // Geometry elements: their own transform (rare, but flowchart
        // shapes use it) composes with the ancestor group transforms.
        const TFrame frame = childFrame(stack.last(), attrs);
        if (tag == QLatin1String("path")) {
            const QRegularExpression dRe(QStringLiteral("\\bd=\"([^\"]*)\""));
            const QRegularExpressionMatch dm = dRe.match(attrs);
            if (dm.hasMatch())
                accumulatePath(dm.captured(1), frame, b);
        } else if (tag == QLatin1String("rect")
                   || tag == QLatin1String("line")) {
            double x1 = 0;
            double y1 = 0;
            double x2 = 0;
            double y2 = 0;
            double w = 0;
            double h = 0;
            if (tag == QLatin1String("rect")) {
                tAttr(attrs, QStringLiteral("x"), x1);
                tAttr(attrs, QStringLiteral("y"), y1);
                tAttr(attrs, QStringLiteral("width"), w);
                tAttr(attrs, QStringLiteral("height"), h);
                x2 = x1 + w;
                y2 = y1 + h;
            } else {
                tAttr(attrs, QStringLiteral("x1"), x1);
                tAttr(attrs, QStringLiteral("y1"), y1);
                tAttr(attrs, QStringLiteral("x2"), x2);
                tAttr(attrs, QStringLiteral("y2"), y2);
            }
            b.extend(frame.x(x1), frame.y(y1));
            b.extend(frame.x(x2), frame.y(y2));
        } else if (tag == QLatin1String("circle")) {
            double cx = 0;
            double cy = 0;
            double r = 0;
            tAttr(attrs, QStringLiteral("cx"), cx);
            tAttr(attrs, QStringLiteral("cy"), cy);
            tAttr(attrs, QStringLiteral("r"), r);
            b.extend(frame.x(cx - r), frame.y(cy - r));
            b.extend(frame.x(cx + r), frame.y(cy + r));
        } else if (tag == QLatin1String("polygon")) {
            const QRegularExpression ptsRe(QStringLiteral("\\bpoints=\"([^\"]*)\""));
            const QRegularExpressionMatch pm = ptsRe.match(attrs);
            if (pm.hasMatch()) {
                const QRegularExpression numRe(QStringLiteral("[-0-9.]+"));
                QVector<double> nums;
                for (auto nIt = numRe.globalMatch(pm.captured(1)); nIt.hasNext();)
                    nums.append(nIt.next().captured().toDouble());
                for (int i = 0; i + 1 < nums.size(); i += 2)
                    b.extend(frame.x(nums.at(i)), frame.y(nums.at(i + 1)));
            }
        } else if (tag == QLatin1String("text")) {
            if (selfClosing) {
                pos = m.capturedEnd();
                continue;
            }
            // Content runs to </text>; strip the tspan tags for measuring.
            const int close = input.indexOf(QLatin1String("</text>"), m.capturedEnd());
            const int contentEnd = close >= 0 ? close : input.size();
            QString content = decodeEntities(
                input.mid(m.capturedEnd(), contentEnd - m.capturedEnd())
                    .replace(tagStripRe, QString()));
            accumulateText(attrs, content.trimmed(), frame, b);
            pos = contentEnd + 7;
            // Restart the global match from the new position.
            it = tagRe.globalMatch(input, pos);
            continue;
        }
        pos = m.capturedEnd();
    }

    if (!b.have)
        return svg;
    // Mermaid's own padding is 8px; use 12 to leave slack for the text
    // width estimates (family-metric approximation, 5% slack).
    constexpr double kPadding = 12.0;
    const double nw = (b.maxX - b.minX) + 2 * kPadding;
    const double nh = (b.maxY - b.minY) + 2 * kPadding;
    if (nw < 1.0 || nh < 1.0)
        return svg;

    QString out = input;
    const QString newVb = QStringLiteral("viewBox=\"%1 %2 %3 %4\"")
                              .arg(b.minX - kPadding, 0, 'g', 12)
                              .arg(b.minY - kPadding, 0, 'g', 12)
                              .arg(nw, 0, 'g', 12)
                              .arg(nh, 0, 'g', 12);
    out.replace(vb.capturedStart(), vb.capturedLength(), newVb);
    // Keep the root's max-width style in sync with the new canvas width.
    const QRegularExpression mwRe(QStringLiteral("(max-width:\\s*)[-0-9.]+(px)"));
    const QRegularExpressionMatch mw = mwRe.match(out.left(rootEnd));
    if (mw.hasMatch())
        out.replace(mw.capturedStart(), mw.capturedLength(),
                    mw.captured(1) + QString::number(nw, 'g', 12) + mw.captured(2));
    return out.toUtf8();
}

// Remove constructs Qt's SVG module cannot represent.
QByteArray sanitizeSvgForQt(const QByteArray &svg)
{
    return tightenViewBoxToContent(clampDividersToBoxes(stripFilters(flattenNestedTspans(svg))));
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

void MermaidEngine::renderAsync(const QString &source, const QString &theme,
                                QObject *context,
                                const std::function<void(const QByteArray &svg)> &callback)
{
    const QByteArray keySource = source.toUtf8();
    const QByteArray cacheInput = keySource + '\0' + theme.toUtf8();
    const QString cacheKey = QString::fromLatin1(
            QCryptographicHash::hash(cacheInput, QCryptographicHash::Md5).toHex().left(16));

    QMutexLocker locker(&m_mutex);
    if (m_cache.contains(cacheKey)) {
        const QByteArray svg = m_cache.value(cacheKey);
        // Queued (never inline): the callback may mutate a document that is
        // being edited on this very thread, and the edit must not be
        // interrupted mid-flight.
        QMetaObject::invokeMethod(context, [callback, svg]() mutable { callback(svg); },
                                  Qt::QueuedConnection);
        return;
    }
    if (!m_worker->isRunning())
        m_worker->start();
    RenderJob *job = new RenderJob;
    job->source = source;
    job->theme = theme;
    job->context = context;
    job->callback = callback;
    m_jobs.push(job);
    m_jobAvailable.wakeOne();
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
        if (job->context) {
            // Async job: cache the result and deliver it on the context's
            // thread (queued, so the callback never runs while the worker is
            // holding this job).
            if (!job->result.isEmpty()) {
                QMutexLocker locker(&m_mutex);
                const QByteArray cacheInput = job->source.toUtf8() + '\0'
                                              + job->theme.toUtf8();
                const QString cacheKey = QString::fromLatin1(
                        QCryptographicHash::hash(cacheInput, QCryptographicHash::Md5)
                                .toHex()
                                .left(16));
                if (m_cache.size() >= kMaxCacheEntries)
                    m_cache.clear();
                m_cache.insert(cacheKey, job->result);
            }
            QMetaObject::invokeMethod(job->context,
                                      [cb = std::move(job->callback),
                                       svg = std::move(job->result)]() mutable {
                                          cb(svg);
                                      },
                                      Qt::QueuedConnection);
        } else {
            job->done.release();
            // Hand the job back to the reader; it may have timed out and given
            // up (in which case this is the only owner and frees it later).
            job->consumed.tryAcquire(1, 5'000);
        }
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
        // Note: JS_ExecutePendingJob() sets *pctx to NULL when the queue is
        // empty, so hand it a local, never &m_ctx.
        JSContext *jobCtx = nullptr;
        if (JS_ExecutePendingJob(m_rt, &jobCtx) <= 0)
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
