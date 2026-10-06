#pragma once
#include "tool.h"

class QNetworkReply;

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
    void abort() override;

private:
    //! The in‑flight reply (nullptr when idle), so abort() can cancel the
    //! request.  Deleted by web_utils before the done callback runs.
    mutable QNetworkReply *m_reply = nullptr;
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
