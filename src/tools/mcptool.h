#pragma once

#include "mcpbridge.h"
#include "tool.h"

#include <memory>

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
    void abort() override;

private:
    //! State of the in-flight call, shared with its done callback so the
    //! callback never touches a destroyed McpTool.
    struct CallState
    {
        McpBridge::ToolCallHandle handle;
        bool finished = false; // done was invoked
    };

    QString m_name;
    mutable std::shared_ptr<CallState> m_call; // null before run()
};

} // namespace LlamaCpp::Tools
