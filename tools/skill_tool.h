#pragma once
#include "tool.h"

namespace LlamaCpp::Tools {

//! Loads a configured skill (see Skills) into the conversation: the
//! skill's Markdown instructions, its base directory and a sampled list
//! of the files that come with it – like opencode's "skill" tool.
class SkillTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

} // namespace LlamaCpp::Tools
