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
    //! One of "exa" (default), "google", "brave" or "tavily".
    QString provider;
    QString exaUrl;
    QString exaApiKey;
    QString googleUrl;
    QString googleApiKey;
    QString googleCx;
    QString braveUrl;
    QString braveApiKey;
    QString tavilyUrl;
    QString tavilyApiKey;

    static WebSearchConfig fromSettings();
};

//! Extracts the LLM‑friendly text from an MCP (JSON‑RPC) tools/call
//! response.  Handles both plain JSON and server‑sent‑events bodies
//! (the first "data: " line carrying a "text" content item).
//! Returns an empty string when no text content could be extracted.
QString parseMcpSearchResponse(const QString &body);

//! Parses the JSON of a Google Custom Search (customsearch/v1) response.
QVector<SearchResult> parseGoogleResults(const QJsonObject &response);

//! Parses the JSON of a Brave Search API (web/search) response.
QVector<SearchResult> parseBraveResults(const QJsonObject &response);

//! Parses the JSON of a Tavily (search) response.
QVector<SearchResult> parseTavilyResults(const QJsonObject &response);

//! Appends the (optional) API key query parameter to an Exa MCP endpoint.
QString exaEndpointUrl(const QString &baseUrl, const QString &apiKey);

//! Formats the results in the numbered "title / url / snippet" layout.
QString formatResults(const QString &query, const QVector<SearchResult> &results);

} // namespace LlamaCpp::Tools
