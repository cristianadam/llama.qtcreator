#pragma once

#include <QObject>

#include "factory.h"
#include "mcpclient.h"
#include "tool.h"

#include <QPair>
#include <QStringList>
#include <QUrl>
#include <memory>

namespace LlamaCpp {

/*!
 * Connects the chat tools to the Qt Creator MCP server.
 *
 * The MCP server is built into Qt Creator (the "mcpserver" plugin) and
 * exposes the IDE itself – building, running, opening and inspecting
 * projects – as MCP tools. The bridge watches Core::McpManager for the
 * builtin server's URL, keeps an McpClient connected to it and publishes
 * its tools through the ToolFactory (as a remote tool provider), so the
 * rest of llama.qtcreator treats them exactly like local tools.
 */
class McpBridge : public QObject, public RemoteToolProvider
{
    Q_OBJECT
public:
    static McpBridge &instance();

    /*! Starts watching for the builtin Qt Creator MCP server. Safe to call
        multiple times. */
    void start();

    // RemoteToolProvider
    QStringList toolNames() const override;
    std::unique_ptr<Tool> createTool(const QString &name) const override;

    bool isMcpTool(const QString &name) const { return m_client.isToolKnown(name); }
    bool isConnected() const { return m_client.isConnected(); }
    Tools::McpToolInfo toolInfo(const QString &name) const { return m_client.toolInfo(name); }

    /*! Forwards a tool call to the MCP server. */
    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  std::function<void(const QString &output, bool ok)> callback)
    {
        m_client.callTool(name, arguments, std::move(callback));
    }

signals:
    void toolsChanged();

private:
    explicit McpBridge();

    void onServersChanged();
    QPair<QUrl, QStringList> builtInServerConnection() const;

    Tools::McpClient m_client;
    QUrl m_currentUrl;
    QStringList m_currentHeaders;
    bool m_started = false;
};

} // namespace LlamaCpp
