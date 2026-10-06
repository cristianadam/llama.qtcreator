#pragma once
#include "tool.h"

namespace LlamaCpp::Tools {

struct SearchState;

class SearchTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString streamingSummary(const QString &partialArguments) const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString detailsMarkdown(const QJsonObject &arguments,
                                    const QString &result,
                                    bool ok) const override;
    QString summaryPreview(const QJsonObject &arguments,
                           const QString &result,
                           bool ok) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
    void abort() override;

private:
    //! The state of the currently running search (nullptr when idle), so
    //! abort() can kill the ripgrep process.
    mutable SearchState *m_state = nullptr;
};

} // namespace LlamaCpp::Tools
