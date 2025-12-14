#pragma once
#include "tool.h"

namespace ProjectExplorer {
class Task;
}

namespace LlamaCpp {

class ListIssuesTool : public Tool
{
public:
    ListIssuesTool();

    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    void run(const QJsonObject &args,
             std::function<void(const QString &output, bool ok)> done) const override;
};

} // namespace LlamaCpp
