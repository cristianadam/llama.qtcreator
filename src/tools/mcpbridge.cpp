#include "mcpbridge.h"

#include "llamatr.h"
#include "llamasettings.h"
#include "mcptool.h"
#include "mcpserverconfig.h"

#include <coreplugin/mcp/mcpmanager.h>

#include <algorithm>

namespace LlamaCpp {

// ID under which the builtin MCP server plugin registers itself with
// Core::McpManager (see the "mcpserver" plugin of Qt Creator).
static const char kQtCreatorMcpServerId[] = "QTCREATOR.BUILTIN.MCP.SERVER";

// Display name of the builtin server (also its tool group name).
static const char kBuiltInServerName[] = "Qt Creator";

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
}

QString McpBridge::builtInServerName()
{
    return QString::fromUtf8(kBuiltInServerName);
}

void McpBridge::start()
{
    if (m_started)
        return;
    m_started = true;

    connect(&Core::McpManager::instance(),
            &Core::McpManager::mcpServersChanged,
            this,
            &McpBridge::onBuiltInServersChanged);
    // The tools settings page adds / edits / removes configured servers;
    // pick that up without a restart.
    connect(&settings(),
            &LlamaSettings::subAspectChanged,
            this,
            [this](Utils::BaseAspect *aspect) {
                if (aspect == static_cast<Utils::BaseAspect *>(&settings().mcpServersJson))
                    syncServers();
            });

    onBuiltInServersChanged();
}

QStringList McpBridge::toolNames() const
{
    QStringList names;
    QSet<QString> seen;
    for (const Server &server : m_servers) {
        for (const Tools::McpToolInfo &info : server.client->tools()) {
            // Tool names must be unique across servers; the earlier server
            // (in priority order) wins.
            if (seen.contains(info.name))
                continue;
            seen.insert(info.name);
            names << info.name;
        }
    }
    return names;
}

std::unique_ptr<Tool> McpBridge::createTool(const QString &name) const
{
    if (!findServerForTool(name))
        return nullptr;
    return std::make_unique<Tools::McpTool>(name);
}

const McpBridge::Server *McpBridge::findServerForTool(const QString &name) const
{
    for (const Server &server : m_servers)
        if (server.client && server.client->isToolKnown(name))
            return &server;
    return nullptr;
}

bool McpBridge::isMcpTool(const QString &name) const
{
    return findServerForTool(name) != nullptr;
}

QString McpBridge::serverForTool(const QString &name) const
{
    const Server *server = findServerForTool(name);
    return server ? server->name : QString();
}

Tools::McpToolInfo McpBridge::toolInfo(const QString &name) const
{
    const Server *server = findServerForTool(name);
    return server ? server->client->toolInfo(name) : Tools::McpToolInfo{};
}

void McpBridge::callTool(const QString &name,
                         const QJsonObject &arguments,
                         std::function<void(const QString &output, bool ok)> callback)
{
    const Server *server = findServerForTool(name);
    if (!server) {
        callback(Tr::tr("The MCP tool \"%1\" is not available (no connected MCP "
                        "server serves it).")
                       .arg(name),
                 false);
        return;
    }
    // The call mutates only the client's own state (pending calls, …), not
    // the bridge's server list, so the const-ness can be dropped there.
    const_cast<Tools::McpClient *>(server->client)->callTool(name, arguments, std::move(callback));
}

QList<McpBridge::ServerStatus> McpBridge::serverStatuses() const
{
    QList<ServerStatus> result;
    result.reserve(m_servers.size());
    for (const Server &server : m_servers) {
        ServerStatus status;
        status.name = server.name;
        status.connected = server.client->isConnected();
        status.url = server.name == QLatin1String(kBuiltInServerName) ? m_builtInUrl
                                                                      : server.client->url();
        result.append(status);
    }
    return result;
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

void McpBridge::onBuiltInServersChanged()
{
    const auto [url, headers] = builtInServerConnection();
    m_builtInActive = !url.isEmpty();
    m_builtInUrl = url;
    m_builtInHeaders = headers;
    syncServers();
}

// Reconciles m_servers with the desired server set: the builtin Qt Creator
// server (when available) first, then the user-configured servers in their
// stored order. Existing clients are kept (connectTo() re-handshakes only
// when the URL or headers changed); clients for removed servers are
// disconnected and deleted.
void McpBridge::syncServers()
{
    QVector<QPair<QString, QPair<QUrl, QStringList>>> desired;
    if (m_builtInActive)
        desired.append(
            {QString::fromUtf8(kBuiltInServerName), QPair{m_builtInUrl, m_builtInHeaders}});
    for (const Tools::McpServerConfig &config :
             Tools::McpServerConfig::fromJson(settings().mcpServersJson()))
        desired.append({config.name, {config.url, config.headers}});

    // Drop clients for servers that are gone.
    for (auto it = m_servers.begin(); it != m_servers.end();) {
        const bool wanted = std::any_of(desired.begin(),
                                        desired.end(),
                                        [&it](const auto &d) { return d.first == it->name; });
        if (!wanted) {
            it->client->disconnectFromServer();
            it->client->deleteLater();
            it = m_servers.erase(it);
        } else {
            ++it;
        }
    }

    // Create the remaining clients (if new) and (re)connect them.
    for (const auto &entry : desired) {
        Server *existing = nullptr;
        for (Server &server : m_servers)
            if (server.name == entry.first)
                existing = &server;

        if (!existing) {
            m_servers.append(Server{});
            existing = &m_servers.last();
            existing->name = entry.first;
            existing->client = new Tools::McpClient(this);
            connect(existing->client,
                    &Tools::McpClient::toolsChanged,
                    this,
                    &McpBridge::toolsChanged);
            connect(existing->client,
                    &Tools::McpClient::connectedChanged,
                    this,
                    &McpBridge::connectionChanged);
        }

        // connectTo() is a no-op when URL and headers are unchanged.
        existing->client->connectTo(entry.second.first, entry.second.second);
    }
}

} // namespace LlamaCpp
