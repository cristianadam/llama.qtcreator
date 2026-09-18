#include "mcpclient.h"

#include "llamatr.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>

namespace LlamaCpp::Tools {

Q_LOGGING_CATEGORY(lmcp, "llama.cpp.mcp", QtInfoMsg)

namespace {

// The MCP protocol version spoken to the Qt Creator MCP server.
constexpr char kProtocolVersion[] = "2025-06-18";

QJsonObject rpcRequest(quint32 id, const QString &method, const QJsonObject &params)
{
    QJsonObject message;
    message["jsonrpc"] = QStringLiteral("2.0");
    message["id"] = int(id);
    message["method"] = method;
    message["params"] = params;
    return message;
}

} // namespace

// Note: no "destroyed" handler that aborts pending replies here – the
// replies are children of the QNetworkAccessManager member, and members
// are destroyed before the QObject base class (which emits destroyed),
// so such a handler would already read freed state.
McpClient::McpClient(QObject *parent)
    : QObject(parent)
{
}

McpClient::~McpClient() = default;

void McpClient::connectTo(const QUrl &url, const QStringList &httpHeaders)
{
    if (m_state == State::Handshaking)
        return;
    // Re‑handshake also when only the headers changed (e.g. the server's
    // authentication token was rotated on the same port).
    if (m_state == State::Ready && url == m_url && httpHeaders == m_requestHeaders)
        return;

    m_url = url;
    m_requestHeaders = httpHeaders;
    startHandshake();
}

void McpClient::startHandshake()
{
    m_sessionId.clear();
    m_toolListRetried = false;
    m_state = State::Handshaking;
    emit connectedChanged();

    qCInfo(lmcp) << "Connecting to MCP server" << m_url.toString();

    QJsonObject params;
    params["protocolVersion"] = QLatin1String(kProtocolVersion);
    params["capabilities"] = QJsonObject();
    QJsonObject clientInfo;
    clientInfo["name"] = QStringLiteral("llama.qtcreator");
    clientInfo["title"] = QStringLiteral("llama.qtcreator (Qt Creator)");
    clientInfo["version"] = QStringLiteral("1.0");
    params["clientInfo"] = clientInfo;

    sendRequest(QStringLiteral("initialize"), params,
                [this](const QJsonObject &response) {
                    if (response.isEmpty()) {
                        fail(Tr::tr("Cannot reach the Qt Creator MCP server."));
                        return;
                    }

                    if (response.contains("error")) {
                        fail(Tr::tr("MCP initialize failed: %1")
                                  .arg(response["error"].toObject()["message"].toString()));
                        return;
                    }

                    // Announce that the client is ready (no response expected).
                    sendNotification(QStringLiteral("notifications/initialized"), QJsonObject());

                    // Discover the available tools.
                    requestToolList();
                });
}

void McpClient::requestToolList()
{
    if (m_state != State::Ready && m_state != State::Handshaking)
        return;

    sendRequest(QStringLiteral("tools/list"),
                QJsonObject(),
                [this](const QJsonObject &listResponse) {
                    if (listResponse.isEmpty()) {
                        fail(Tr::tr("Cannot reach the Qt Creator MCP server."));
                        return;
                    }

                    if (listResponse.contains("error")) {
                        fail(Tr::tr("MCP tools/list failed: %1")
                                  .arg(listResponse["error"].toObject()["message"]
                                           .toString()));
                        return;
                    }

                    QJsonObject result = listResponse["result"].toObject();
                    QJsonArray tools = result["tools"].toArray();

                    QList<McpToolInfo> newTools;
                    newTools.reserve(tools.size());
                    for (const QJsonValue &v : tools) {
                        const QJsonObject t = v.toObject();
                        McpToolInfo info;
                        info.name = t["name"].toString();
                        info.description = t["description"].toString();
                        info.inputSchema = t["inputSchema"].toObject();
                        if (!info.name.isEmpty())
                            newTools.append(info);
                    }

                    m_state = State::Ready;
                    setTools(newTools);

                    if (newTools.isEmpty()) {
                        // The server's tool registration can lag slightly behind
                        // the handshake (tools are contributed by plugins). Re-
                        // request once after a short delay so a late
                        // registration does not leave us with a stale empty
                        // list.
                        if (!m_toolListRetried) {
                            m_toolListRetried = true;
                            qCInfo(lmcp) << "MCP server reported no tools; "
                                          << "retrying tools/list once after 1 s.";
                            QTimer::singleShot(1000, this, [this] { requestToolList(); });
                        }
                        return;
                    }

                    qCInfo(lmcp).noquote()
                        << "Connected to MCP server" << m_url.toString()
                        << "with" << m_tools.size() << "tools:"
                        << m_toolNames.values().join(", ");
                });
}

void McpClient::disconnectFromServer()
{
    if (m_state == State::Disconnected)
        return;

    m_state = State::Disconnected;
    m_sessionId.clear();
    failPending(Tr::tr("Connection to the Qt Creator MCP server was closed."));

    const bool hadTools = !m_tools.isEmpty();
    m_tools.clear();
    m_toolNames.clear();

    emit connectedChanged();
    if (hadTools)
        emit toolsChanged();

    qCInfo(lmcp) << "Disconnected from MCP server";
}

McpToolInfo McpClient::toolInfo(const QString &name) const
{
    for (const McpToolInfo &info : m_tools) {
        if (info.name == name)
            return info;
    }
    return {};
}

void McpClient::callTool(const QString &name,
                         const QJsonObject &arguments,
                         ToolCallback callback)
{
    if (m_state != State::Ready) {
        callback(Tr::tr("The Qt Creator MCP server is not available. "
                        "Enable the \"Qt Creator MCP Server\" plugin in Qt Creator settings."),
                 false);
        return;
    }

    QJsonObject params;
    params["name"] = name;
    params["arguments"] = arguments;

    const quint32 requestId = m_nextRequestId++;
    m_pendingToolCalls.insert(requestId, std::move(callback));
    // Note: this closure's m_pending entry is already erased by the time it
    // runs (see the post() callback below), so no removal is needed here.
    m_pending[requestId] = [this, requestId, name](const QJsonObject &response) {
        const ToolCallback cb = m_pendingToolCalls.take(requestId);

        if (response.contains("error")) {
            if (cb)
                cb(Tr::tr("MCP tool \"%1\" failed: %2")
                           .arg(name,
                                response["error"].toObject()["message"].toString()),
                   false);
            return;
        }

        QJsonObject result = response["result"].toObject();
        const bool isError = result["isError"].toBool(false);

        QString output;
        QJsonArray content = result["content"].toArray();
        QStringList parts;
        for (const QJsonValue &v : content) {
            const QJsonObject item = v.toObject();
            if (item["type"].toString() == QLatin1String("text"))
                parts << item["text"].toString();
        }
        output = parts.join(QLatin1Char('\n'));

        // Fall back to the compact JSON in case the tool returned no
        // human-readable text content.
        if (output.isEmpty() && !result.isEmpty())
            output = QString::fromUtf8(
                QJsonDocument(result).toJson(QJsonDocument::Compact));

        qCDebug(lmcp).noquote() << "MCP tool" << name << "finished, isError:" << isError;

        if (cb)
            cb(output, !isError);
    };

    post(rpcRequest(requestId, QStringLiteral("tools/call"), params),
         [this, requestId](int httpStatus, const QByteArray &contentType, const QByteArray &body) {
             if (httpStatus == 404) {
                 // The server restarted and our session is stale – redo the
                 // handshake so subsequent calls work, and ask for a retry.
                 m_pending.remove(requestId);
                 const ToolCallback cb = m_pendingToolCalls.take(requestId);
                 if (cb) {
                     qCWarning(lmcp) << "Stale MCP session detected – re-handshaking.";
                     startHandshake();
                     cb(Tr::tr("The Qt Creator MCP server restarted; the call was not "
                               "executed. Please try again."),
                        false);
                 }
                 return;
             }

             if (httpStatus >= 400) {
                 m_pending.remove(requestId);
                 const ToolCallback cb = m_pendingToolCalls.take(requestId);
                 if (cb)
                     cb(Tr::tr("MCP server returned HTTP %1 for tool call.").arg(httpStatus),
                        false);
                 return;
             }

             const QJsonObject response = extractJsonRpcResponse(body, contentType, requestId);
             if (response.isEmpty()) {
                 m_pending.remove(requestId);
                 const ToolCallback cb = m_pendingToolCalls.take(requestId);
                 if (cb)
                     cb(Tr::tr("MCP server returned an empty or malformed response."), false);
                 return;
             }

             auto pendingIt = m_pending.find(requestId);
             if (pendingIt == m_pending.end())
                 return;
             // Move the handler out of the map before erasing it: the closure
             // is too large to fit in std::function's small buffer, so it is
             // heap-allocated and would be freed mid-execution if the map
             // entry were destroyed while the handler is still running.
             const auto handler = std::move(pendingIt.value());
             m_pending.erase(pendingIt);
             handler(response);
         });
}

void McpClient::sendRequest(
    const QString &method,
    const QJsonObject &params,
    const std::function<void(const QJsonObject &response)> &onResponse)
{
    const quint32 requestId = m_nextRequestId++;
    m_pending[requestId] = onResponse;

    post(rpcRequest(requestId, method, params),
         [this, requestId](int httpStatus, const QByteArray &contentType, const QByteArray &body) {
             if (httpStatus >= 400) {
                 const auto handler = m_pending.take(requestId);
                 if (handler)
                     handler(QJsonObject{
                         {"error",
                          QJsonObject{{"message",
                                       QStringLiteral("HTTP %1").arg(httpStatus)}}}});
                 return;
             }

             const QJsonObject response = extractJsonRpcResponse(body, contentType, requestId);
             auto it = m_pending.find(requestId);
             if (it == m_pending.end())
                 return;
             // Same as in callTool: keep the handler alive in a local while
             // erasing its (heap-allocated) entry from the map.
             const auto handler = std::move(it.value());
             m_pending.erase(it);
             // An empty response indicates a malformed body.
             handler(response);
         });
}

void McpClient::sendNotification(const QString &method, const QJsonObject &params)
{
    QJsonObject message;
    message["jsonrpc"] = QStringLiteral("2.0");
    message["method"] = method;
    message["params"] = params;
    post(message, [](int, const QByteArray &, const QByteArray &) {});
}

void McpClient::post(const QJsonObject &rpcMessage,
                     const std::function<void(int, const QByteArray &, const QByteArray &)> &handler)
{
    QNetworkRequest request(m_url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    // The streamable-HTTP endpoint requires both media types in Accept.
    request.setRawHeader("Accept", "application/json, text/event-stream");
    // Request‑specific headers registered for the server (e.g. the
    // "Authorization: Bearer <token>" header when token auth is enabled).
    for (const QString &header : m_requestHeaders) {
        const int separator = header.indexOf(QLatin1Char(':'));
        if (separator <= 0)
            continue;
        request.setRawHeader(header.left(separator).trimmed().toUtf8(),
                             header.mid(separator + 1).trimmed().toUtf8());
    }
    if (!m_sessionId.isEmpty())
        request.setRawHeader("mcp-session-id", m_sessionId.toUtf8());

    QNetworkReply *reply = m_network.post(request,
                                          QJsonDocument(rpcMessage).toJson(QJsonDocument::Compact));

    QPointer<QNetworkReply> guard(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, handler, guard]() {
        if (guard.isNull())
            return;

        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                                   .toInt();
        const QByteArray contentType =
            reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        const QByteArray session = reply->rawHeader("mcp-session-id");
        if (!session.isEmpty())
            m_sessionId = QString::fromUtf8(session);

        const QByteArray body = reply->readAll();
        handler(status, contentType, body);
        reply->deleteLater();
    });
}

void McpClient::setTools(const QList<McpToolInfo> &newTools)
{
    m_tools = newTools;
    m_toolNames.clear();
    m_toolNames.reserve(m_tools.size());
    for (const McpToolInfo &info : m_tools)
        m_toolNames.insert(info.name);
    emit toolsChanged();
}

void McpClient::fail(const QString &reason)
{
    qCWarning(lmcp) << reason;
    m_state = State::Disconnected;
    m_sessionId.clear();
    failPending(reason);

    const bool hadTools = !m_tools.isEmpty();
    m_tools.clear();
    m_toolNames.clear();

    emit connectedChanged();
    if (hadTools)
        emit toolsChanged();
}

void McpClient::failPending(const QString &reason)
{
    const auto toolCallbacks = m_pendingToolCalls.values();
    m_pendingToolCalls.clear();
    m_pending.clear();
    for (const ToolCallback &cb : toolCallbacks)
        if (cb)
            cb(reason, false);
}

QJsonObject McpClient::extractJsonRpcResponse(const QByteArray &body,
                                              const QByteArray &contentType,
                                              quint32 expectedId)
{
    // Server-sent events: one JSON-RPC message per "data: ..." line.
    if (contentType.contains("text/event-stream")) {
        QJsonObject best;
        const QList<QByteArray> lines = body.split('\n');
        for (const QByteArray &rawLine : lines) {
            QByteArray line = rawLine.trimmed();
            if (!line.startsWith("data:"))
                continue;
            line = line.mid(5).trimmed();
            if (line.isEmpty() || line == QLatin1String("[DONE]").toUtf8())
                continue;

            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if (err.error != QJsonParseError::NoError || !doc.isObject())
                continue;

            const QJsonObject msg = doc.object();
            if (expectedId != 0 && msg["id"].toInt() == int(expectedId))
                return msg;
            // Keep the last message carrying a result (skip pure notifications).
            if (msg.contains("result") || msg.contains("error"))
                best = msg;
        }
        return best;
    }

    // Plain JSON body.
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return {};
    return doc.object();
}

} // namespace LlamaCpp::Tools
