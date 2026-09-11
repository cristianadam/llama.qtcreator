#pragma once
#include "tool.h"

#include <QJsonObject>
#include <QVector>

namespace LlamaCpp::Tools {

class WebSearchTool : public Tool
{
public:
    QString name() const override;
    QString streamingSummary(const QString &partialArguments) const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

struct SearchResult
{
    QString title;
    QString url;
    QString snippet;
};

//! Backend selection for the websearch tool.
struct WebSearchConfig
{
    //! One of "exa" (default), "google", "duckduckgo".
    QString provider;
    QString exaUrl;
    QString exaApiKey;
    QString googleUrl;
    QString googleApiKey;
    QString googleCx;
    QString duckDuckGoUrl;

    static WebSearchConfig fromSettings();
};

//! Extracts the LLM‑friendly text from an MCP (JSON‑RPC) tools/call
//! response.  Handles both plain JSON and server‑sent‑events bodies
//! (the first "data: " line carrying a "text" content item).
//! Returns an empty string when no text content could be extracted.
QString parseMcpSearchResponse(const QString &body);

//! Parses the JSON of a Google Custom Search (customsearch/v1) response.
QVector<SearchResult> parseGoogleResults(const QJsonObject &response);

//! Parses the HTML of a DuckDuckGo Lite (lite.duckduckgo.com) search
//! results page.  Returns at most \a maxResults results, in page order.
QVector<SearchResult> parseDuckDuckGoResults(const QString &body, int maxResults);

//! Appends the (optional) API key query parameter to an Exa MCP endpoint.
QString exaEndpointUrl(const QString &baseUrl, const QString &apiKey);

//! Formats the results in the numbered "title / url / snippet" layout.
QString formatResults(const QString &query, const QVector<SearchResult> &results);

} // namespace LlamaCpp::Tools
