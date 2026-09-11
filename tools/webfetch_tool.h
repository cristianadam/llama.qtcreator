#pragma once
#include "tool.h"

namespace LlamaCpp::Tools {

class WebFetchTool : public Tool
{
public:
    QString name() const override;
    QString streamingSummary(const QString &partialArguments) const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

/*! Trims \a url and requires an http(s) scheme.  Returns the normalized
    URL, or an empty string when it is not a fetchable URL. */
QString normalizeFetchUrl(const QString &url);

/*! Converts an HTML document to lightweight, LLM‑friendly markdown:
    headings, lists, links, code and bold/italic are kept, everything
    else (scripts, styles, navigation markup, …) is reduced to plain text. */
QString htmlToMarkdown(const QString &html);

/*! Extracts the visible plain text of an HTML document. */
QString htmlToText(const QString &html);

} // namespace LlamaCpp::Tools
