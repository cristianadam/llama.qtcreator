#pragma once

#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QTimer>
#include <QUrl>

#include <QList>
#include <QMap>
#include <QMultiMap>
#include <QSet>
#include <QString>
#include <QStringList>
#include <functional>

namespace LlamaCpp::Tools {

/*! Metadata of a tool served by an MCP server (from "tools/list"). */
struct McpToolInfo
{
    QString name;
    QString description;
    QJsonObject inputSchema;
};

/*!
 * Minimal Model Context Protocol (MCP) streamable-HTTP client.
 *
 * Implements just enough of the protocol for llama.qtcreator to discover
 * and invoke tools served by the Qt Creator MCP server:
 *
 *   initialize -> notifications/initialized -> tools/list -> tools/call
 *
 * Responses may arrive either as a plain application/json body or as a
 * text/event-stream (SSE) body; both are handled transparently.
 *
 * All methods must be called from the main thread.
 */
class McpClient : public QObject
{
    Q_OBJECT
public:
    using ToolCallback = std::function<void(const QString &output, bool ok)>;

    explicit McpClient(QObject *parent = nullptr);
    ~McpClient() override;

    /*! Connects to the given MCP server endpoint. \a httpHeaders are
        request headers to send with every request, as "Name: value" strings
        (e.g. the "Authorization: Bearer …" header of a token-protected
        server). */
    void connectTo(const QUrl &url, const QStringList &httpHeaders = {});
    void disconnectFromServer();

    bool isConnected() const { return m_state == State::Ready; }

    const QList<McpToolInfo> &tools() const { return m_tools; }
    bool isToolKnown(const QString &name) const { return m_toolNames.contains(name); }
    McpToolInfo toolInfo(const QString &name) const;

    /*! Calls the given tool. \a callback is invoked exactly once, on the
        main thread, with the textual result and a success flag. */
    void callTool(const QString &name, const QJsonObject &arguments, ToolCallback callback);

signals:
    void connectedChanged();
    void toolsChanged();

private:
    enum class State { Disconnected, Handshaking, Ready };

    void sendRequest(
        const QString &method,
        const QJsonObject &params,
        const std::function<void(const QJsonObject &response)> &onResponse);
    void sendNotification(const QString &method, const QJsonObject &params);
    void post(const QJsonObject &rpcMessage,
              const std::function<void(int httpStatus,
                                       const QByteArray &contentType,
                                       const QByteArray &body)> &handler);

    void startHandshake();
    void requestToolList();
    void setTools(const QList<McpToolInfo> &newTools);
    void fail(const QString &reason);
    void failPending(const QString &reason);

    static QJsonObject extractJsonRpcResponse(const QByteArray &body,
                                              const QByteArray &contentType,
                                              quint32 expectedId);

    State m_state = State::Disconnected;
    QUrl m_url;
    QStringList m_requestHeaders; // raw "Name: value" strings
    QString m_sessionId;
    QNetworkAccessManager m_network;
    QList<McpToolInfo> m_tools;
    QSet<QString> m_toolNames;
    quint32 m_nextRequestId = 1;
    QMap<quint32, std::function<void(const QJsonObject &)>> m_pending;
    // Tool calls in flight (kept so they can be failed on disconnect)
    QMultiMap<quint32, ToolCallback> m_pendingToolCalls;
    // Set once tools/list returned an empty list (a retry is in flight or
    // done); reset on each handshake.
    bool m_toolListRetried = false;
};

} // namespace LlamaCpp::Tools

#ifdef WITH_TESTS
namespace LlamaCpp {
namespace Internal {
// Test object for the "Qt Creator -test llamacpp" run; see
// tools/mcpclient_test.cpp.
QObject *createMcpClientTest();
} // namespace Internal
} // namespace LlamaCpp
#endif // WITH_TESTS
