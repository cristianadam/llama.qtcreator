#pragma once
#include "tool.h"

#include <QJsonObject>

namespace LlamaCpp::Tools {

class WriteTool : public Tool
{
public:
    QString name() const override;
    QString streamingSummary(const QString &partialArguments) const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString detailsMarkdown(const QJsonObject &args, const QString &result) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

} // namespace LlamaCpp::Tools
