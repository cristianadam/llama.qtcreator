#pragma once
#include "tool.h"

#include <QJsonObject>

namespace LlamaCpp::Tools {

/**
 * "todo_write" – maintains the conversation's task list for tracking
 * progress on multi-step work (like opencode's todowrite tool).
 *
 * The tool is stateless: the model always sends the complete list and the
 * latest version simply replaces the previous one.  The list is rendered
 * as a markdown checklist in the tool's details view.
 */
class TodoWriteTool : public Tool
{
public:
    QString name() const override;
    QString streamingSummary(const QString &partialArguments) const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &args) const override;
    QString detailsMarkdown(const QJsonObject &args,
                                    const QString &result,
                                    bool ok) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
};

} // namespace LlamaCpp::Tools
