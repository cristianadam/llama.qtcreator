#pragma once
#include "tool.h"

namespace LlamaCpp {

class EditFileTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString run(const QJsonObject &args) const override;
    QString detailsMarkdown(const QJsonObject &args,
                            const QString & /*unusedResult*/) const override;
};

} // namespace LlamaCpp
