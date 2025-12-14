#pragma once
#include "tool.h"

namespace LlamaCpp {

class EditFileTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
    QString detailsMarkdown(const QJsonObject &args, const QString &result) const override;
};

} // namespace LlamaCpp
