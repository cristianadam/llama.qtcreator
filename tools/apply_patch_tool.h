#pragma once
#include "tool.h"

namespace LlamaCpp {

class ApplyPatchTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString streamingSummary(const QString &partialArguments) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
    QString detailsMarkdown(const QJsonObject &args,
                                    const QString &result,
                                    bool ok) const override;
    QString summaryPreview(const QJsonObject &args, const QString &result, bool ok) const override;
};

} // namespace LlamaCpp
