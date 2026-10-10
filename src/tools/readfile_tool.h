#pragma once
#include "tool.h"

namespace LlamaCpp::Tools {

class ReadFileTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString detailsMarkdown(const QJsonObject &args,
                            const QString &result,
                            bool ok) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

} // namespace LlamaCpp::Tools
