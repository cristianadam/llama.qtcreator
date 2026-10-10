#pragma once

#include <QObject>

#include "factory.h"
#include "mcpclient.h"
#include "tool.h"

#include <QPair>
#include <QUrl>
#include <QVector>
#include <memory>

namespace LlamaCpp {

/*!
 * Connects the chat tools to MCP servers.
 *
 * Supports the MCP server built into Qt Creator (the "mcpserver" plugin,
 * which exposes the IDE itself – building, running, opening and
 * inspecting projects – as MCP tools) in addition to arbitrary
 * user-configured servers (managed on the "Tools" settings page, stored
 * in the McpServers setting as Tools::McpServerConfig entries).
 *
 * The bridge watches Core::McpManager for the builtin server's URL, keeps
 * an McpClient per server and publishes their tools through the
 * ToolFactory (as a remote tool provider), so the rest of llama.qtcreator
 * treats them exactly like local tools.
 *
 * Tool names must be unique across servers: when two servers expose the
 * same tool name, the earlier server wins (the builtin server first, then
 * the configured servers in their stored order).
 */
class McpBridge : public QObject, public RemoteToolProvider
{
    Q_OBJECT
public:
    //! Connection state of one server, for display on the settings page.
    struct ServerStatus
    {
        QString name;
        QUrl url;
        bool connected = false;
    };

    static McpBridge &instance();

    //! Display name of the builtin Qt Creator MCP server (its tool group
    //! name); reserved – configured servers must not use it.
    static QString builtInServerName();

    /*! Starts watching for the builtin Qt Creator MCP server and connects
        the configured servers. Safe to call multiple times. */
    void start();

    // RemoteToolProvider
    QStringList toolNames() const override;
    std::unique_ptr<Tool> createTool(const QString &name) const override;

    bool isMcpTool(const QString &name) const;
    //! Display name of the server that serves the tool, empty if none.
    QString serverForTool(const QString &name) const;
    Tools::McpToolInfo toolInfo(const QString &name) const;

    /*! Forwards a tool call to the server that serves it. */
    void callTool(const QString &name,
                  const QJsonObject &arguments,
                  std::function<void(const QString &output, bool ok)> callback);

    /*! All servers (builtin first, then the configured ones in stored
        order) with their current connection state. */
    QList<ServerStatus> serverStatuses() const;

signals:
    void toolsChanged();
    void connectionChanged();

private:
    struct Server
    {
        QString name;
        Tools::McpClient *client = nullptr;
    };

    explicit McpBridge();

    const Server *findServerForTool(const QString &name) const;
    void syncServers();
    void onBuiltInServersChanged();
    QPair<QUrl, QStringList> builtInServerConnection() const;

    QVector<Server> m_servers; // builtin server first, then the configured ones
    QUrl m_builtInUrl;
    QStringList m_builtInHeaders;
    bool m_builtInActive = false;
    bool m_started = false;
};

} // namespace LlamaCpp
