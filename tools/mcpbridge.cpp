#include "mcpbridge.h"

#include "llamatr.h"
#include "mcptool.h"

#include <coreplugin/mcp/mcpmanager.h>

namespace LlamaCpp {

// ID under which the builtin MCP server plugin registers itself with
// Core::McpManager (see the "mcpserver" plugin of Qt Creator).
static const char kQtCreatorMcpServerId[] = "QTCREATOR.BUILTIN.MCP.SERVER";

McpBridge &McpBridge::instance()
{
    // Intentionally leaked: a QObject singleton that is destroyed during the
    // static-destruction phase at exit (after the QCoreApplication and the
    // Core::McpManager it talks to are already gone) leads to
    // use‑after‑free.
    static McpBridge *inst = new McpBridge;
    return *inst;
}

McpBridge::McpBridge()
    : QObject(nullptr)
{
    connect(&m_client, &Tools::McpClient::toolsChanged, this, &McpBridge::toolsChanged);
}

void McpBridge::start()
{
    if (m_started)
        return;
    m_started = true;

    connect(&Core::McpManager::instance(),
            &Core::McpManager::mcpServersChanged,
            this,
            &McpBridge::onServersChanged);
    onServersChanged();
}

QStringList McpBridge::toolNames() const
{
    QStringList names;
    names.reserve(m_client.tools().size());
    for (const Tools::McpToolInfo &info : m_client.tools())
        names << info.name;
    return names;
}

std::unique_ptr<Tool> McpBridge::createTool(const QString &name) const
{
    if (!m_client.isToolKnown(name))
        return nullptr;
    return std::make_unique<Tools::McpTool>(name);
}

QPair<QUrl, QStringList> McpBridge::builtInServerConnection() const
{
    const QList<Core::McpManager::ServerInfo> servers = Core::McpManager::mcpServers();
    for (const Core::McpManager::ServerInfo &info : servers) {
        if (info.id != QLatin1String(kQtCreatorMcpServerId))
            continue;
        if (const QUrl *url = std::get_if<QUrl>(&info.launchInfo))
            // httpHeaders carries the server's authentication header ("Authorization: Bearer …")
            // when token auth is enabled, empty otherwise.
            return {*url, info.httpHeaders};
        return {};
    }
    return {};
}

void McpBridge::onServersChanged()
{
    const auto [url, headers] = builtInServerConnection();

    if (url.isEmpty()) {
        if (m_currentUrl.isValid()) {
            // The builtin server went away (plugin disabled, port changed,
            // …) – drop the connection.
            m_currentUrl = QUrl();
            m_currentHeaders.clear();
            m_client.disconnectFromServer();
        }
        return;
    }

    m_currentUrl = url;
    m_currentHeaders = headers;
    m_client.connectTo(url, headers);
}

} // namespace LlamaCpp
