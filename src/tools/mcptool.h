#pragma once

#include "mcpbridge.h"
#include "tool.h"

namespace LlamaCpp::Tools {

/*!
 * A tool that lives on the Qt Creator MCP server, not in this plugin.
 *
 * The JSON definition, discovery and invocation are all delegated to the
 * McpBridge (which talks MCP over HTTP to the IDE). This class only adapts
 * the tool to the local Tool interface used by the chat.
 */
class McpTool : public Tool
{
public:
    explicit McpTool(const QString &name);

    QString name() const override { return m_name; }

    QString streamingSummary(const QString &partialArguments) const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &arguments) const override;
    QString detailsMarkdown(const QJsonObject &arguments,
                                    const QString &result,
                                    bool ok) const override;

    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;

private:
    QString m_name;
};

} // namespace LlamaCpp::Tools
