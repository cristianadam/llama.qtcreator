#pragma once
#include "tool.h"

namespace LlamaCpp::Tools {

class PythonTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
    QString detailsMarkdown(const QJsonObject &arguments, const QString &result) const override;
};

} // namespace LlamaCpp::Tools
