#pragma once
#include "tool.h"

namespace LlamaCpp {

class AskUserTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;

    // The LLM itself will never call `run()` for this tool – the UI pops a
    // dialog.  Still we provide a stub.
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

} // namespace LlamaCpp
