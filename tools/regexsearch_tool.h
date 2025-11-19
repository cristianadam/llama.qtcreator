#pragma once
#include "tool.h"

namespace LlamaCpp {

class RegexSearchTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString run(const QJsonObject &args) const override;
};

} // namespace LlamaCpp
