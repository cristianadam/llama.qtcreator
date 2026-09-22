#pragma once

#include "tool.h"

namespace LlamaCpp::Tools {

/**
 * "task" – delegates a self‑contained subtask to a sub‑agent that works in a
 * separate conversation (like opencode's task tool).  When the sub‑agent
 * finishes, its final report is returned as the tool result in the parent
 * conversation; the sub‑conversation stays in the conversation list so the
 * user (or the model) can go back to it for further work.
 *
 * Supported sub‑agent types:
 *   "explore"  – read‑only tools, meant for codebase exploration
 *   "general"  – every enabled tool except "task" and "ask_user"
 */
class TaskTool : public LlamaCpp::Tool
{
public:
    QString name() const override;
    QString streamingSummary(const QString &partialArguments) const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &arguments) const override;
    QString detailsMarkdown(const QJsonObject &arguments,
                                    const QString &result,
                                    bool ok) const override;

    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

//! The system prompt for the given subagent type ("explore" or "general").
QString taskSystemPromptFor(const QString &subagentType);

//! The whitelist of tools the given subagent type may use, filtered against
//! \a availableTools.  "explore" is restricted to the read‑only tools,
//! "general" gets everything except "task" and "ask_user".
QStringList taskToolsFor(const QString &subagentType, const QStringList &availableTools);

//! Strips a leading thinking section (think‑token block) from a stored
//! assistant message so that only the final report remains.
QString taskFinalReport(const QString &content);

} // namespace LlamaCpp::Tools
