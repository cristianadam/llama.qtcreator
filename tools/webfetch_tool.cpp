#include "webfetch_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "web_utils.h"

#include <QRegularExpression>
#include <QUrl>

namespace LlamaCpp::Tools {

namespace {

constexpr int kDefaultTimeoutSec = 30;
constexpr int kMaxTimeoutSec = 120;
constexpr qint64 kMaxResponseBytes = 5 * 1024 * 1024;
constexpr int kMaxOutputChars = 50000;

const QRegularExpression::PatternOptions kReCiDot
    = QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption;

QString decodeEntities(QString s)
{
    s.replace(QStringLiteral("&amp;"), QLatin1String("&"))
        .replace(QStringLiteral("&lt;"), QLatin1String("<"))
        .replace(QStringLiteral("&gt;"), QLatin1String(">"))
        .replace(QStringLiteral("&quot;"), QLatin1String("\""))
        .replace(QStringLiteral("&#x27;"), QLatin1String("'"))
        .replace(QStringLiteral("&#39;"), QLatin1String("'"))
        .replace(QStringLiteral("&apos;"), QLatin1String("'"))
        .replace(QStringLiteral("&nbsp;"), QLatin1String(" "));
    return s;
}

QRegularExpression makeRe(const QString &pattern, QRegularExpression::PatternOptions options = kReCiDot)
{
    return QRegularExpression(pattern, options);
}

/*! Runs \a fn on every match of \a rx in \a in and splices the results
    back into the string (a capture‑aware replace). */
QString replaceMatches(const QString &in,
                       const QRegularExpression &rx,
                       const std::function<QString(const QRegularExpressionMatch &)> &fn)
{
    QString out;
    int last = 0;
    for (auto it = rx.globalMatch(in); it.hasNext(); ) {
        const QRegularExpressionMatch m = it.next();
        out += in.mid(last, int(m.capturedStart()) - last);
        out += fn(m);
        last = int(m.capturedEnd());
    }
    out += in.mid(last);
    return out;
}

/*! Removes non‑content sections (comments, scripts, styles, head, …). */
QString removeBoilerplate(const QString &html)
{
    QString s = html;
    s.remove(makeRe(QStringLiteral("<!--.*?-->")));
    const char *sections[]
        = { "script", "style", "noscript", "template", "svg", "canvas", "iframe" };
    for (const char *section : sections)
        s.remove(makeRe(QString(QLatin1Char('<') + section + QLatin1String("[^>]*>.*?</"))
                          + section + QLatin1Char('>')));
    // \s after "head" keeps <header> intact
    s.remove(makeRe(QStringLiteral("<head(?:\\s[^>]*)?>.*?</head>")));
    return s;
}

QString stripInner(const QString &s)
{
    QString t = s;
    t.remove(makeRe(QStringLiteral("<[^>]*>"), QRegularExpression::CaseInsensitiveOption));
    t = decodeEntities(t);
    t.replace(makeRe(QStringLiteral("\\s+"), QRegularExpression::NoPatternOption),
              QLatin1String(" "));
    return t.trimmed();
}

/*! Like stripInner but keeps the line structure (for <pre> blocks). */
QString verbatimCode(const QString &s)
{
    QString t = s;
    t.remove(makeRe(QStringLiteral("<[^>]*>"), QRegularExpression::CaseInsensitiveOption));
    t = decodeEntities(t);

    QStringList lines;
    for (const QString &raw : t.split(QLatin1Char('\n'))) {
        int end = raw.size();
        while (end > 0 && (raw.at(end - 1) == QLatin1Char(' ')
                           || raw.at(end - 1) == QLatin1Char('\t')))
            --end;
        lines.append(raw.left(end));
    }
    while (!lines.isEmpty() && lines.first().isEmpty())
        lines.removeFirst();
    while (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();
    return lines.join(QLatin1Char('\n'));
}

QString cleanLines(const QString &s)
{
    QStringList lines;
    int blanks = 0;
    for (const QString &raw : s.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            if (blanks < 1)
                lines.append(line);
            ++blanks;
        } else {
            lines.append(line);
            blanks = 0;
        }
    }
    return lines.join(QLatin1Char('\n')).trimmed();
}

} // namespace

const bool registered = [] {
    ToolFactory::instance().registerCreator(WebFetchTool{}.name(),
                                            []() { return std::make_unique<WebFetchTool>(); });
    return true;
}();

QString WebFetchTool::name() const
{
    return QStringLiteral("webfetch");
}

QString WebFetchTool::streamingSummary(const QString &partialArgs) const
{
    // "url" is the first field, so a usable summary appears almost
    // immediately while the arguments are still streaming in.
    int idx = partialArgs.indexOf(QStringLiteral("\"url\""));
    if (idx == -1)
        return {};
    idx = partialArgs.indexOf(QLatin1Char(':'), idx);
    if (idx == -1)
        return {};
    const int start = partialArgs.indexOf(QLatin1Char('"'), idx + 1);
    if (start == -1)
        return {};
    int end = start + 1;
    while (end < partialArgs.size()) {
        const QChar c = partialArgs.at(end);
        if (c == QLatin1Char('"') || c == QLatin1Char('\\'))
            break;
        ++end;
    }
    const QString url = partialArgs.mid(start + 1, end - start - 1);
    if (url.isEmpty())
        return {};
    return Tr::tr("fetch %1").arg(url);
}

QString WebFetchTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "webfetch",
            "description": "Fetches a URL and returns its content as compact, LLM‑friendly text. HTML pages have their markup stripped and are converted to lightweight markdown (or plain text); other content types are returned as‑is. Use this to read documentation, web pages, or any http(s) resource. The returned content is truncated at 50000 characters.",
            "parameters": {
                "type": "object",
                "properties": {
                    "url": { "type": "string", "description": "The fully‑formed URL to fetch (http:// or https://)." },
                    "format": { "type": "string", "enum": ["markdown", "text", "html"], "description": "Output format. \"markdown\" (default): HTML stripped to light markdown. \"text\": visible plain text. \"html\": raw page source." },
                    "timeout": { "type": "integer", "description": "Optional timeout in seconds. Defaults to 30, maximum 120." }
                },
                "required": ["url"],
                "strict": true
            }
        }
    })raw";
}

QString WebFetchTool::oneLineSummary(const QJsonObject &args) const
{
    return Tr::tr("fetch %1").arg(args.value("url").toString());
}

QString normalizeFetchUrl(const QString &url)
{
    const QString trimmed = url.trimmed();
    if (!trimmed.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)
        && !trimmed.startsWith(QLatin1String("https://"), Qt::CaseInsensitive))
        return {};
    const QUrl u(trimmed);
    return u.isValid() ? u.toString() : QString();
}

QString htmlToMarkdown(const QString &html)
{
    QString s = removeBoilerplate(html);

    // <pre> blocks: preserve verbatim as fenced code blocks
    QVector<QString> codeBlocks;
    s = replaceMatches(s, makeRe(QStringLiteral("<pre[^>]*>(.*?)</pre>")),
                       [&codeBlocks](const QRegularExpressionMatch &m) {
                           codeBlocks.append(verbatimCode(m.captured(1)));
                           return QString(QChar(0x01)) + QStringLiteral("PRE")
                                + QString::number(codeBlocks.size() - 1) + QString(QChar(0x01));
                       });

    // Images
    s = replaceMatches(s, makeRe(QStringLiteral("<img[^>]*>")),
                       [](const QRegularExpressionMatch &m) {
                           const QString tag = m.captured(0);
                           auto attr = [&tag](const char *name) -> QString {
                               const QRegularExpression rx(QStringLiteral("\\b")
                                                               + QString::fromLatin1(name)
                                                               + QStringLiteral(
                                                                     "\\s*=\\s*([\"'])([^\"']*)\\1"),
                                                           QRegularExpression::CaseInsensitiveOption);
                               const auto a = rx.match(tag);
                               return a.hasMatch() ? a.captured(2) : QString();
                           };
                           const QString src = attr("src");
                           if (src.isEmpty())
                               return QString();
                           return QStringLiteral("![%1](%2)").arg(attr("alt"), src);
                       });

    // Links
    s = replaceMatches(
        s,
        makeRe(QStringLiteral("<a\\s[^>]*href\\s*=\\s*([\"'])([^\"']*)\\1[^>]*>(.*?)</a>")),
        [](const QRegularExpressionMatch &m) {
            const QString url = m.captured(2);
            const QString text = stripInner(m.captured(3));
            if (text.isEmpty())
                return url;
            return QStringLiteral("[%1](%2)").arg(text, url);
        });

    // Headings
    for (int level = 1; level <= 6; ++level) {
        const QString rxPattern
            = QStringLiteral("<h%1[^>]*>(.*?)</h%1>").arg(level);
        s = replaceMatches(s, makeRe(rxPattern), [level](const QRegularExpressionMatch &m) {
            return QString(level, QLatin1Char('#')) + QLatin1Char(' ')
                 + stripInner(m.captured(1)) + QLatin1Char('\n');
        });
    }

    // Lists, emphasis, code (opening and closing tags)
    s.replace(makeRe(QStringLiteral("<li[^>]*>")), QStringLiteral("- "));
    s.replace(makeRe(QStringLiteral("<(/?(?:strong|b)\\b[^>]*)>")), QStringLiteral("**"));
    s.replace(makeRe(QStringLiteral("<(/?(?:em|i)\\b[^>]*)>")), QLatin1String("*"));
    s.replace(makeRe(QStringLiteral("<(/?code\\b[^>]*)>")), QLatin1String("`"));
    s.replace(makeRe(QStringLiteral("<br\\s*/?>")), QLatin1String("\n"));

    // Block‑level closing tags become line breaks
    s.replace(makeRe(QStringLiteral(
                        "</(?:p|div|tr|table|ul|ol|li|section|article|header|footer|main|nav|blockquote|h[1-6])\\s*>")),
              QLatin1String("\n"));

    // Drop whatever markup is left
    s.remove(makeRe(QStringLiteral("<[^>]*>")));
    s = decodeEntities(s);
    s = cleanLines(s);

    // Restore code blocks
    for (int i = 0; i < codeBlocks.size(); ++i) {
        const QString marker = QString(QChar(0x01)) + QStringLiteral("PRE") + QString::number(i)
                             + QString(QChar(0x01));
        s.replace(marker, QStringLiteral("```\n%1\n```").arg(codeBlocks.at(i)));
    }

    return s;
}

QString htmlToText(const QString &html)
{
    QString s = removeBoilerplate(html);
    s.replace(makeRe(QStringLiteral("<br\\s*/?>")), QLatin1String("\n"));
    s.replace(makeRe(QStringLiteral(
                        "</(?:p|div|tr|table|ul|ol|li|section|article|header|footer|main|nav|blockquote|h[1-6])\\s*>")),
              QLatin1String("\n"));
    s.remove(makeRe(QStringLiteral("<[^>]*>")));
    s = decodeEntities(s);
    return cleanLines(s);
}

void WebFetchTool::run(const QJsonObject &args,
                       std::function<void(const QString &, bool)> done) const
{
    const QString rawUrl = args.value("url").toString().trimmed();
    const QString url = normalizeFetchUrl(rawUrl);
    if (url.isEmpty())
        return done(Tr::tr("Invalid URL \"%1\": it must start with http:// or https://")
                        .arg(rawUrl),
                    false);

    QString format = args.value("format").toString(QStringLiteral("markdown")).toLower();
    if (format != QLatin1String("markdown") && format != QLatin1String("text")
        && format != QLatin1String("html"))
        format = QLatin1String("markdown");

    const int timeoutSec = qBound(1, args.value("timeout").toInt(kDefaultTimeoutSec), kMaxTimeoutSec);

    httpGet(url,
            timeoutSec,
            kMaxResponseBytes,
            [url, format, done](const QByteArray &body,
                                const QString &contentType,
                                const QString &error) {
                if (!error.isEmpty()) {
                    const QString msg = error.contains(QStringLiteral("size limit"))
                        ? Tr::tr("Fetch failed for %1: response too large (limit 5 MB).").arg(url)
                        : Tr::tr("Fetch failed for %1: %2").arg(url, error);
                    return done(msg, false);
                }

                const QString mime = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
                QString content = QString::fromUtf8(body);
                if (mime.contains(QLatin1String("html"))) {
                    if (format == QLatin1String("markdown"))
                        content = htmlToMarkdown(content);
                    else if (format == QLatin1String("text"))
                            content = htmlToText(content);
                }
                content = cleanLines(content.trimmed());
                if (content.size() > kMaxOutputChars)
                    content = content.left(kMaxOutputChars)
                           + Tr::tr("\n\n[... content truncated at %1 characters ...]")
                                 .arg(kMaxOutputChars);
                return done(content, true);
            });
}

} // namespace LlamaCpp::Tools
