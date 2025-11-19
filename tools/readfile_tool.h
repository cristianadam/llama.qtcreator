#pragma once
#include "tool.h"

namespace LlamaCpp::Tools {

class ReadFileTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString run(const QJsonObject &args) const override;
};

} // namespace LlamaCpp::Tools
