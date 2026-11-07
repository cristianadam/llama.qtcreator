#include "websearch_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "llamasettings.h"
#include "web_utils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>

namespace LlamaCpp::Tools {

namespace {

constexpr int kDefaultMaxResults = 5;
constexpr int kMaxResults = 10;
constexpr int kTimeoutSec = 30;
constexpr qint64 kMaxResponseBytes = 1 * 1024 * 1024;

const char kExaDefaultUrl[] = "https://mcp.exa.ai/mcp";
const char kGoogleDefaultUrl[] = "https://www.googleapis.com/customsearch/v1";
const char kBraveDefaultUrl[] = "https://api.search.brave.com/res/v1/web/search";
const char kTavilyDefaultUrl[] = "https://api.tavily.com/search";
const char kDuckDuckGoDefaultUrl[] = "https://html.duckduckgo.com/html/";
const char kExaToolName[] = "web_search_exa";

//! A current desktop Firefox user agent for the host platform – the
//! DuckDuckGo HTML endpoint serves results to browser‑like clients only.
QString duckDuckGoUserAgent()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("Mozilla/5.0 (Macintosh; Intel Mac OS X 15.8; rv:159.0) "
                          "Gecko/20100101 Firefox/159.0");
#elif defined(Q_OS_WIN)
    return QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:159.0) "
                          "Gecko/20100101 Firefox/159.0");
#else
    return QStringLiteral("Mozilla/5.0 (X11; Linux x86_64; rv:159.0) "
                          "Gecko/20100101 Firefox/159.0");
#endif
}

QString mcpTextContent(const QJsonObject &payload)
{
    const QJsonArray content = payload.value(QStringLiteral("result")).toObject()
                                   .value(QStringLiteral("content")).toArray();
    for (const QJsonValue &value : content) {
        const QJsonObject item = value.toObject();
        if (item.value(QStringLiteral("type")).toString() == QLatin1String("text")
            && item.contains(QStringLiteral("text")))
            return item.value(QStringLiteral("text")).toString();
    }
    return {};
}

QString mcpTextFromJson(const QByteArray &data)
{
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return mcpTextContent(doc.object());
}

} // namespace

const bool registered = [] {
    ToolFactory::instance().registerCreator(WebSearchTool{}.name(),
                                            []() { return std::make_unique<WebSearchTool>(); });
    return true;
}();

QString WebSearchTool::name() const
{
    return QStringLiteral("websearch");
}

QString WebSearchTool::streamingSummary(const QString &partialArgs) const
{
    // "query" is the first field, so a usable summary appears almost
    // immediately while the arguments are still streaming in.
    int idx = partialArgs.indexOf(QStringLiteral("\"query\""));
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
    const QString query = partialArgs.mid(start + 1, end - start - 1);
    if (query.isEmpty())
        return {};
    return Tr::tr("search %1").arg(query);
}

QString WebSearchTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "websearch",
            "description": "Searches the web and returns a numbered list of results with title, URL and snippet. Use this to find the right page to read, then use webfetch to retrieve its full content.",
            "parameters": {
                "type": "object",
                "properties": {
                    "query": { "type": "string", "description": "The search query." },
                    "max_results": { "type": "integer", "description": "Maximum number of results to return. Defaults to 5, maximum 10." }
                },
                "required": ["query"],
                "strict": true
            }
        }
    })raw";
}

QString WebSearchTool::oneLineSummary(const QJsonObject &args) const
{
    return Tr::tr("search %1").arg(args.value("query").toString());
}

WebSearchConfig WebSearchConfig::fromSettings()
{
    WebSearchConfig config;
    config.provider = settings().webSearchProvider().trimmed().toLower();
    config.exaUrl = settings().webSearchExaUrl().trimmed();
    config.exaApiKey = settings().webSearchExaApiKey().trimmed();
    config.googleUrl = settings().webSearchGoogleUrl().trimmed();
    config.googleApiKey = settings().webSearchGoogleApiKey().trimmed();
    config.googleCx = settings().webSearchGoogleCx().trimmed();
    config.braveUrl = settings().webSearchBraveUrl().trimmed();
    config.braveApiKey = settings().webSearchBraveApiKey().trimmed();
    config.tavilyUrl = settings().webSearchTavilyUrl().trimmed();
    config.tavilyApiKey = settings().webSearchTavilyApiKey().trimmed();
    return config;
}

bool WebSearchConfig::isConfigured() const
{
    if (provider == QLatin1String("duckduckgo"))
        return true; // keyless
    if (provider == QLatin1String("google"))
        return !googleApiKey.isEmpty() && !googleCx.isEmpty();
    if (provider == QLatin1String("brave"))
        return !braveApiKey.isEmpty();
    if (provider == QLatin1String("tavily"))
        return !tavilyApiKey.isEmpty();
    // Exa. The hosted endpoint needs an API key; a *custom* endpoint
    // (e.g. a local proxy) does not. The settings aspect pre-fills the hosted
    // URL as its default, so only a URL differing from it counts as custom -
    // otherwise the tool would be advertised to every keyless user.
    const bool customEndpoint = !exaUrl.isEmpty() && exaUrl != QLatin1String(kExaDefaultUrl);
    return customEndpoint || !exaApiKey.isEmpty();
}

QString parseMcpSearchResponse(const QString &body)
{
    const QString trimmed = body.trimmed();
    if (trimmed.startsWith(QLatin1Char('{'))) {
        if (const QString text = mcpTextFromJson(trimmed.toUtf8()); !text.isEmpty())
            return text;
    }
    // Server‑sent events: pick the first "data:" line that carries text content.
    for (const QString &line : body.split(QLatin1Char('\n'))) {
        if (!line.startsWith(QLatin1String("data:")))
            continue;
        if (const QString text = mcpTextFromJson(line.mid(5).trimmed().toUtf8()); !text.isEmpty())
            return text;
    }
    return {};
}

QVector<SearchResult> parseGoogleResults(const QJsonObject &response)
{
    QVector<SearchResult> results;
    for (const QJsonValue &value : response.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString url = item.value(QStringLiteral("link")).toString();
        if (url.isEmpty())
            continue;
        SearchResult r;
        r.title = item.value(QStringLiteral("title")).toString();
        r.url = url;
        r.snippet = item.value(QStringLiteral("snippet")).toString();
        results.append(r);
    }
    return results;
}

QVector<SearchResult> parseBraveResults(const QJsonObject &response)
{
    // Brave web/search response: { "web": { "results": [ { "title",
    // "url", "description", … }, … ] } }
    QVector<SearchResult> results;
    const QJsonObject web = response.value(QStringLiteral("web")).toObject();
    for (const QJsonValue &value : web.value(QStringLiteral("results")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString url = item.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            continue;
        SearchResult r;
        r.title = item.value(QStringLiteral("title")).toString();
        r.url = url;
        r.snippet = item.value(QStringLiteral("description")).toString();
        results.append(r);
    }
    return results;
}

QVector<SearchResult> parseTavilyResults(const QJsonObject &response)
{
    // Tavily /search response: { "results": [ { "title", "url", "content",
    // … }, … ] }
    QVector<SearchResult> results;
    for (const QJsonValue &value : response.value(QStringLiteral("results")).toArray()) {
        const QJsonObject item = value.toObject();
        const QString url = item.value(QStringLiteral("url")).toString();
        if (url.isEmpty())
            continue;
        SearchResult r;
        r.title = item.value(QStringLiteral("title")).toString();
        r.url = url;
        r.snippet = item.value(QStringLiteral("content")).toString();
        results.append(r);
    }
    return results;
}

QString duckDuckGoResultUrl(const QString &href)
{
    // Result links go through the DDG redirector
    // ("//duckduckgo.com/l/?uddg=<percent‑encoded target>&rut=…"); unwrap
    // the percent‑encoded target, fall back to the raw (resolved) link.
    // The value is extracted manually – QUrlQuery's "Prettified" parsing
    // decodes the %26/%3D escapes of the *outer* query and would split the
    // encoded target at its (decoded) inner "&".
    QString h = href;
    if (h.startsWith(QLatin1String("//")))
        h.prepend(QLatin1String("https:"));
    // The href may carry "&" as an "&amp;" entity; normalize it so the
    // uddg value can be cut at the next item.
    h.replace(QStringLiteral("&amp;"), QLatin1String("&"));

    const int queryStart = h.indexOf(QLatin1Char('?'));
    if (queryStart != -1) {
        for (const QString &item : h.mid(queryStart + 1).split(QLatin1Char('&'))) {
            if (item.startsWith(QLatin1String("uddg="))) {
                const QString decoded = QUrl::fromPercentEncoding(item.mid(5).toUtf8());
                if (!decoded.isEmpty())
                    return decoded;
            }
        }
    }
    return h;
}

QVector<SearchResult> parseDuckDuckGoResults(const QString &html)
{
    // Each organic result carries exactly one title anchor
    // (class="result__a"), followed by its snippet
    // (class="result__snippet") before the next result starts.
    static const QRegularExpression titleRe(
        QStringLiteral("class=\"result__a\"\\s+href=\"([^\"]*)\"[^>]*>(.*?)</a>"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression snippetRe(
        QStringLiteral("class=\"result__snippet\"[^>]*>(.*?)</a>"),
        QRegularExpression::CaseInsensitiveOption
            | QRegularExpression::DotMatchesEverythingOption);

    struct Title
    {
        QString href;
        QString text;
        int start;
        int end;
    };
    QVector<Title> titles;
    for (auto it = titleRe.globalMatch(html); it.hasNext(); ) {
        const QRegularExpressionMatch m = it.next();
        titles.append({m.captured(1), m.captured(2), int(m.capturedStart()), int(m.capturedEnd())});
    }

    QVector<SearchResult> results;
    for (int i = 0; i < titles.size(); ++i) {
        const int windowEnd = (i + 1 < titles.size()) ? titles[i + 1].start : html.size();
        const QString window = html.mid(titles[i].end, windowEnd - titles[i].end);

        SearchResult r;
        r.url = duckDuckGoResultUrl(titles[i].href);
        r.title = stripHtmlTags(titles[i].text);
        const QRegularExpressionMatch m = snippetRe.match(window);
        if (m.hasMatch())
            r.snippet = stripHtmlTags(m.captured(1));
        results.append(r);
    }
    return results;
}

QString exaEndpointUrl(const QString &baseUrl, const QString &apiKey)
{
    if (baseUrl.isEmpty())
        return {};
    if (apiKey.isEmpty())
        return baseUrl;
    QUrl url(baseUrl);
    QUrlQuery query(url.query());
    query.addQueryItem(QStringLiteral("exaApiKey"), apiKey);
    url.setQuery(query);
    return url.toString();
}

QString formatResults(const QString &query, const QVector<SearchResult> &results)
{
    QString out = Tr::tr("Search results for \"%1\":").arg(query);
    int n = 1;
    for (const SearchResult &r : std::as_const(results)) {
        out += QStringLiteral("\n\n%1. %2\n   %3").arg(n++).arg(r.title, r.url);
        if (!r.snippet.isEmpty())
            out += QStringLiteral("\n   %1").arg(r.snippet);
    }
    return out;
}

void WebSearchTool::run(const QJsonObject &args,
                        std::function<void(const QString &, bool)> done) const
{
    const QString query = args.value("query").toString().trimmed();
    if (query.isEmpty())
        return done(Tr::tr("Tool error: \"query\" must be a non-empty string."), false);

    const int maxResults = qBound(1,
                                  args.value("max_results").toInt(kDefaultMaxResults),
                                  kMaxResults);

    const WebSearchConfig config = WebSearchConfig::fromSettings();

    if (config.provider == QLatin1String("duckduckgo")) {
        QUrl u(kDuckDuckGoDefaultUrl);
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), query);
        u.setQuery(q);

        QList<QPair<QByteArray, QByteArray>> headers;
        headers.append({QByteArrayLiteral("User-Agent"),
                        QByteArray::fromStdString(duckDuckGoUserAgent().toStdString())});
        headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("text/html")});

        return httpGet(u.toString(),
                       headers,
                       kTimeoutSec,
                       kMaxResponseBytes,
                       [this, query, maxResults, done](const QByteArray &body,
                                                       const QString &,
                                                       const QString &error) {
                           // The reply was deleted before this callback ran.
                           m_reply = nullptr;
                           if (!error.isEmpty())
                               return done(Tr::tr("Search failed: %1").arg(error), false);
                           const QString html = QString::fromUtf8(body);
                           // DDG's rate‑limit ("anomaly detection") page
                           // instead of a results page.
                           if (html.contains(QLatin1String("anomaly"), Qt::CaseInsensitive))
                               return done(Tr::tr("Search failed: DuckDuckGo is rate‑limiting "
                                                  "requests right now; wait a minute and try "
                                                  "again."),
                                           false);
                           QVector<SearchResult> results = parseDuckDuckGoResults(html);
                           if (results.size() > maxResults)
                               results.resize(maxResults);
                           if (results.isEmpty())
                               return done(Tr::tr("No results found for \"%1\".").arg(query),
                                           true);
                           return done(formatResults(query, results), true);
                       },
                       &m_reply);
    }

    if (config.provider == QLatin1String("google")) {
        const QString url = config.googleUrl.isEmpty() ? kGoogleDefaultUrl : config.googleUrl;
        if (config.googleApiKey.isEmpty() || config.googleCx.isEmpty())
            return done(Tr::tr("Search failed: the Google backend is not configured. "
                               "Set the API key and the search engine ID (cx) in the "
                               "Llama.cpp settings."),
                        false);
        QUrl u(url);
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("key"), config.googleApiKey);
        q.addQueryItem(QStringLiteral("cx"), config.googleCx);
        q.addQueryItem(QStringLiteral("q"), query);
        q.addQueryItem(QStringLiteral("num"), QString::number(maxResults));
        u.setQuery(q);

        return httpGet(u.toString(),
                       kTimeoutSec,
                       kMaxResponseBytes,
                       [this, query, done](const QByteArray &body,
                                           const QString &,
                                           const QString &error) {
                           // The reply was deleted before this callback ran.
                           m_reply = nullptr;
                           if (!error.isEmpty())
                               return done(Tr::tr("Search failed: %1").arg(error), false);
                           QJsonParseError parseError;
                           const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                           if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                               return done(Tr::tr("Search failed: invalid response from the "
                                                  "search provider."),
                                           false);
                           const QJsonObject response = doc.object();
                           const QVector<SearchResult> results = parseGoogleResults(response);
                           if (results.isEmpty()) {
                               const QString message
                                   = response.value(QStringLiteral("error")).toObject()
                                         .value(QStringLiteral("message")).toString();
                               return done(Tr::tr("Search failed: %1")
                                               .arg(message.isEmpty()
                                                         ? Tr::tr("no results found for \"%1\"")
                                                               .arg(query)
                                                         : message),
                                           false);
                           }
                           return done(formatResults(query, results), true);
                       },
                       &m_reply);
    }

    if (config.provider == QLatin1String("brave")) {
        const QString url = config.braveUrl.isEmpty() ? kBraveDefaultUrl : config.braveUrl;
        if (config.braveApiKey.isEmpty())
            return done(Tr::tr("Search failed: the Brave backend is not configured. "
                               "Set the API key in the Llama.cpp settings."),
                        false);
        QUrl u(url);
        QUrlQuery q;
        q.addQueryItem(QStringLiteral("q"), query);
        q.addQueryItem(QStringLiteral("count"), QString::number(maxResults));
        u.setQuery(q);

        // Brave authenticates via a custom header, so the GET-with-headers
        // overload is used instead of the plain one.
        QList<QPair<QByteArray, QByteArray>> headers;
        headers.append({QByteArrayLiteral("Accept"), QByteArrayLiteral("application/json")});
        headers.append({QByteArrayLiteral("X-Subscription-Token"),
                        QByteArray::fromStdString(config.braveApiKey.toStdString())});

        return httpGet(u.toString(),
                       headers,
                       kTimeoutSec,
                       kMaxResponseBytes,
                       [this, query, done](const QByteArray &body,
                                           const QString &,
                                           const QString &error) {
                           // The reply was deleted before this callback ran.
                           m_reply = nullptr;
                           if (!error.isEmpty())
                               return done(Tr::tr("Search failed: %1").arg(error), false);
                           QJsonParseError parseError;
                           const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                           if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                               return done(Tr::tr("Search failed: invalid response from the "
                                                  "search provider."),
                                           false);
                           const QVector<SearchResult> results
                               = parseBraveResults(doc.object());
                           if (results.isEmpty())
                               return done(Tr::tr("No results found for \"%1\".").arg(query),
                                           true);
                           return done(formatResults(query, results), true);
                       },
                       &m_reply);
    }

    if (config.provider == QLatin1String("tavily")) {
        const QString url = config.tavilyUrl.isEmpty() ? kTavilyDefaultUrl : config.tavilyUrl;
        if (config.tavilyApiKey.isEmpty())
            return done(Tr::tr("Search failed: the Tavily backend is not configured. "
                               "Set the API key in the Llama.cpp settings."),
                        false);

        QJsonObject request;
        request[QStringLiteral("query")] = query;
        request[QStringLiteral("max_results")] = maxResults;

        QList<QPair<QByteArray, QByteArray>> headers;
        headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
        headers.append({QByteArrayLiteral("Authorization"),
                        QByteArrayLiteral("Bearer ")
                            + QByteArray::fromStdString(config.tavilyApiKey.toStdString())});

        return httpPost(url,
                        QJsonDocument(request).toJson(QJsonDocument::Compact),
                        headers,
                        kTimeoutSec,
                        kMaxResponseBytes,
                        [this, query, done](const QByteArray &body,
                                            const QString &,
                                            const QString &error) {
                          // The reply was deleted before this callback ran.
                          m_reply = nullptr;
                          if (!error.isEmpty())
                              return done(Tr::tr("Search failed: %1").arg(error), false);
                          QJsonParseError parseError;
                          const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
                          if (parseError.error != QJsonParseError::NoError || !doc.isObject())
                              return done(Tr::tr("Search failed: invalid response from the "
                                                 "search provider."),
                                          false);
                          const QVector<SearchResult> results = parseTavilyResults(doc.object());
                          if (results.isEmpty())
                              return done(Tr::tr("No results found for \"%1\".").arg(query), true);
                          return done(formatResults(query, results), true);
                      },
                      &m_reply);
    }

    // Default: Exa via its hosted MCP endpoint (JSON‑RPC tools/call).
    const QString base = config.exaUrl.isEmpty() ? kExaDefaultUrl : config.exaUrl;
    const QString endpoint = exaEndpointUrl(base, config.exaApiKey);
    if (endpoint.isEmpty())
        return done(Tr::tr("Search failed: the Exa endpoint URL is not configured."), false);

    QJsonObject arguments;
    arguments[QStringLiteral("query")] = query;
    arguments[QStringLiteral("type")] = QStringLiteral("auto");
    arguments[QStringLiteral("numResults")] = maxResults;
    arguments[QStringLiteral("livecrawl")] = QStringLiteral("fallback");
    QJsonObject params;
    params[QStringLiteral("name")] = kExaToolName;
    params[QStringLiteral("arguments")] = arguments;
    QJsonObject request;
    request[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    request[QStringLiteral("id")] = 1;
    request[QStringLiteral("method")] = QStringLiteral("tools/call");
    request[QStringLiteral("params")] = params;

    QList<QPair<QByteArray, QByteArray>> headers;
    headers.append({QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/json")});
    headers.append({QByteArrayLiteral("Accept"),
                    QByteArrayLiteral("application/json, text/event-stream")});

    return httpPost(endpoint,
                    QJsonDocument(request).toJson(QJsonDocument::Compact),
                    headers,
                    kTimeoutSec,
                    kMaxResponseBytes,
                    [this, query, done](const QByteArray &body,
                                        const QString &,
                                        const QString &error) {
                        // The reply was deleted before this callback ran.
                        m_reply = nullptr;
                        if (!error.isEmpty())
                            return done(Tr::tr("Search failed: %1").arg(error), false);
                        const QString text = parseMcpSearchResponse(QString::fromUtf8(body));
                        if (text.isEmpty())
                            return done(Tr::tr("No results found for \"%1\".").arg(query), true);
                        const QString out
                            = Tr::tr("Search results for \"%1\":\n\n%2").arg(query, text);
                        return done(out, true);
                    },
                    &m_reply);
}

void WebSearchTool::abort()
{
    if (m_reply) {
        m_reply->abort();
        m_reply = nullptr;
    }
}

} // namespace LlamaCpp::Tools
