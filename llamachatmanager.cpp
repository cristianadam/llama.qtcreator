#include <QCoreApplication>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QNetworkReply>

#include "llamachatmanager.h"
#include "llamasettings.h"
#include "llamastorage.h"

Q_LOGGING_CATEGORY(llamaChatNetwork, "llama.cpp.chat.network", QtWarningMsg)

namespace LlamaCpp {

ChatManager &ChatManager::instance()
{
    static ChatManager inst;
    return inst;
}

ChatManager::ChatManager(QObject *parent)
    : QObject(parent)
    , m_storage(&Storage::instance())
{
    initServerProps();

    connect(m_storage, &Storage::messageAppended, this, &ChatManager::messageAppended);
    connect(m_storage, &Storage::conversationCreated, [this](const QString &convId) {
        m_activeConvId = convId;
        emit conversationCreated(convId);
    });
    connect(m_storage, &Storage::conversationRenamed, this, &ChatManager::conversationRenamed);
    connect(m_storage, &Storage::conversationDeleted, this, &ChatManager::conversationDeleted);
}

static QNetworkReply *getServerProps(QNetworkAccessManager *manager,
                                     const QString &baseUrl,
                                     const QString &apiKey)
{
    QUrl url(baseUrl + "/props");
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!apiKey.isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + apiKey).toUtf8());

    return manager->get(req);
}

void ChatManager::refreshServerProps()
{
    initServerProps();
}

void ChatManager::initServerProps()
{
    QNetworkReply *reply = getServerProps(&m_network,
                                          settings().chatEndpoint.value(),
                                          settings().chatApiKey.value());
    QObject::connect(reply, &QNetworkReply::finished, [reply, this]() {
        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(llamaChatNetwork) << "Failed to fetch server props:" << reply->errorString();
            reply->deleteLater();
            return;
        }
        QByteArray b = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(b);
        QJsonObject obj = doc.object();
        m_serverProps.build_info = obj.value("build_info").toString();
        m_serverProps.model_path = obj.value("model_path").toString();
        m_serverProps.n_ctx = obj.value("n_ctx").toInt();
        QJsonObject mod = obj.value("modalities").toObject();
        m_serverProps.modalities.vision = mod.value("vision").toBool();
        m_serverProps.modalities.audio = mod.value("audio").toBool();
        reply->deleteLater();

        emit serverPropsUpdated();
    });
}

bool ChatManager::isGenerating(const QString &convId) const
{
    return m_pendingMessages.contains(convId);
}

ViewingChat ChatManager::getViewingChat(const QString &convId) const
{
    ViewingChat vc{m_storage->getOneConversation(convId), m_storage->getMessages(convId)};
    return vc;
}

QVector<Message> ChatManager::filterByLeafNodeId(const QVector<Message> &messages,
                                                 qint64 leafNodeId,
                                                 bool includeRoot)
{
    return m_storage->filterByLeafNodeId(messages, leafNodeId, includeRoot);
}

void ChatManager::sendMessage(const QString &convId,
                              qint64 leafNodeId,
                              const QString &content,
                              const QList<QVariantMap> &extra,
                              std::function<void(qint64)> onChunk)
{
    if (isGenerating(convId) || content.trimmed().isEmpty())
        return;

    Message newMsg;
    auto now = QDateTime::currentMSecsSinceEpoch();
    newMsg.id = now;
    newMsg.convId = convId;
    newMsg.type = "text";
    newMsg.timestamp = now;
    newMsg.role = "user";
    newMsg.content = content;
    newMsg.parent = leafNodeId;
    newMsg.children.clear();
    newMsg.extra = extra; // simple wrapper – see MessageExtra

    // create conversation if needed
    if (newMsg.convId.isEmpty() || newMsg.convId.isNull()) {
        Conversation c = m_storage->createConversation(content.left(256));
        newMsg.convId = c.id;
        leafNodeId = c.currNode;
    }

    m_storage->appendMsg(newMsg, leafNodeId);
    onChunk(newMsg.id);

    // generate assistant reply
    generateMessage(newMsg.convId, newMsg.id, onChunk);
}

static void readSSEStream(QNetworkReply *reply,
                          std::function<void(const QJsonObject &)> onChunk,
                          std::function<void(const QString &)> onError)
{
    QObject::connect(reply, &QNetworkReply::readyRead, [reply, onChunk, onError]() {
        QByteArray buffer = reply->readAll();

        qCDebug(llamaChatNetwork).noquote() << "readSSEStream:" << buffer;

        // split on \n\n (SSE format)
        while (true) {
            int pos = buffer.indexOf("\n\n");
            if (pos < 0)
                break;
            QByteArray line = buffer.left(pos);
            buffer.remove(0, pos + 2);

            // parse the data line
            if (line.startsWith("data: "))
                line = line.mid(6);
            if (line.isEmpty() || line == "[DONE]")
                continue;

            QJsonParseError err;
            QJsonDocument doc = QJsonDocument::fromJson(line, &err);
            if (err.error != QJsonParseError::NoError) {
                onError(err.errorString());
                return;
            }
            onChunk(doc.object());
        }
    });
}

void ChatManager::generateMessage(const QString &convId,
                                  qint64 leafNodeId,
                                  std::function<void(qint64)> onChunk)
{
    if (isGenerating(convId))
        return;

    auto currMsgs = m_storage->getMessages(convId);
    auto leafMsgs = m_storage->filterByLeafNodeId(currMsgs, leafNodeId, false);

    // prepare pending msg
    Message pending{};
    pending.id = QDateTime::currentMSecsSinceEpoch() + 1; // not to have the same id as the user
    pending.convId = convId;
    pending.type = "text";
    pending.timestamp = pending.id;
    pending.role = "assistant";
    pending.content = QString();
    pending.parent = leafNodeId;
    pending.children.clear();
    m_pendingMessages.insert(convId, pending);
    m_abortControllers[convId] = nullptr; // will hold the reply

    // build request payload
    QJsonObject payload;
    payload["messages"] = normalizeMsgsForAPI(leafMsgs);
    payload["stream"] = true;
    payload["cache_prompt"] = true;
    payload["reasoning_format"] = "none";
    payload["samplers"] = settings().samplers.value();
    payload["temperature"] = settings().temperature.value();
    payload["dynatemp_range"] = settings().dynatemp_range.value();
    payload["dynatemp_exponent"] = settings().dynatemp_exponent.value();
    payload["top_k"] = settings().top_k.value();
    payload["top_p"] = settings().top_p.value();
    payload["min_p"] = settings().min_p.value();
    payload["typical_p"] = settings().typical_p.value();
    payload["xtc_probability"] = settings().xtc_probability.value();
    payload["xtc_threshold"] = settings().xtc_threshold.value();
    payload["repeat_last_n"] = settings().repeat_last_n.value();
    payload["repeat_penalty"] = settings().repeat_penalty.value();
    payload["presence_penalty"] = settings().presence_penalty.value();
    payload["frequency_penalty"] = settings().frequency_penalty.value();
    payload["dry_multiplier"] = settings().dry_multiplier.value();
    payload["dry_base"] = settings().dry_base.value();
    payload["dry_allowed_length"] = settings().dry_allowed_length.value();
    payload["dry_penalty_last_n"] = settings().dry_penalty_last_n.value();
    payload["max_tokens"] = settings().max_tokens.value();
    payload["timings_per_token"] = settings().showTokensPerSecond.value();

    // parse custom JSON if present
    if (!settings().customJson.value().isEmpty()) {
        QJsonDocument d = QJsonDocument::fromJson(settings().customJson.value().toUtf8());
        if (d.isObject())
            for (auto it = d.object().constBegin(); it != d.object().constEnd(); ++it)
                payload[it.key()] = it.value();
    }

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());
    m_abortControllers[convId] = reply;
    qCDebug(llamaChatNetwork).noquote() << "Request:" << QJsonDocument(payload).toJson();

    // read SSE
    readSSEStream(
        reply,
        // onChunk
        [this, convId, onChunk](const QJsonObject &chunk) {
            if (chunk.contains("error")) {
                qCWarning(llamaChatNetwork)
                    << "SSE error:" << chunk["error"].toObject()["message"].toString();
                return;
            }

            QJsonArray choices = chunk["choices"].toArray();
            if (!choices.isEmpty()) {
                QString added = choices[0].toObject()["delta"].toObject()["content"].toString();
                if (!added.isEmpty()) {
                    Message &pm = m_pendingMessages[convId];
                    pm.content += added;

                    emit pendingMessageChanged(pm);
                }
            }
            if (settings().showTokensPerSecond.value() && chunk.contains("timings")) {
                QJsonObject t = chunk["timings"].toObject();
                TimingReport tr;
                tr.prompt_n = t["prompt_n"].toInt();
                tr.prompt_ms = t["prompt_ms"].toInt();
                tr.predicted_n = t["predicted_n"].toInt();
                tr.predicted_ms = t["predicted_ms"].toInt();
                m_pendingMessages[convId].timings = tr;
            }
            onChunk(-1); // caller will scroll to bottom
        },
        //Error
        [this, convId](const QString &err) {
            qCWarning(llamaChatNetwork) << "SSE stream error:" << err;
            m_pendingMessages.remove(convId);
            if (m_abortControllers.contains(convId))
                m_abortControllers[convId]->deleteLater();
        });

    QObject::connect(reply, &QNetworkReply::finished, [this, convId, reply] {
        reply->deleteLater();
        m_abortControllers.remove(convId);

        Message pm = m_pendingMessages.take(convId);
        if (!pm.content.isNull() && !pm.content.isEmpty()) {
            m_storage->appendMsg(pm, pm.parent);

            // After the first assistant reply we ask the server to give us
            // a short title (emoji‑rich) for the conversation.
            if (pm.role == "assistant") {
                auto msgs = m_storage->getMessages(convId);
                // A newly created conversation will have exactly three messages
                // (root + user + assistant) after the first reply.
                if (msgs.size() == 3) {
                    summarizeConversationTitle(convId, pm.id, [this, convId](const QString &title) {
                        QString shortTitle = title;
                        const QString endToken = "<|end|>";
                        auto endIdx = title.indexOf(endToken);
                        if (endIdx != -1) {
                            shortTitle = title.mid(endIdx + endToken.size());
                        }
                        renameConversation(convId, shortTitle);
                    });
                }
            }
        }
    });
}

Conversation ChatManager::currentConversation()
{
    return m_storage->getOneConversation(m_activeConvId);
}

void ChatManager::setCurrentConversation(const QString &convId)
{
    m_activeConvId = convId;
}

Conversation ChatManager::createConversation(const QString &name)
{
    return m_storage->createConversation(name);
}

QList<Conversation> ChatManager::allConversations()
{
    return m_storage->getAllConversations();
}

void ChatManager::summarizeConversationTitle(const QString &convId,
                                             qint64 leafNodeId,
                                             std::function<void(const QString &)> onSuccess)
{
    auto msgs = m_storage->getMessages(convId);
    auto leafMsgs = m_storage->filterByLeafNodeId(msgs, leafNodeId, false);

    QJsonArray msgArray = normalizeMsgsForAPI(leafMsgs);
    QJsonObject payload;

    // Append the prompt that asks for the title
    QJsonArray parts;
    QJsonObject txt;
    txt["type"] = "text";
    txt["text"] = "Summarize the title of the conversation in a few words including one emoji. Use "
                  "plain text, no markdown.";
    parts.append(txt);
    QJsonObject prompt;
    prompt["role"] = "user";
    prompt["content"] = parts;

    msgArray.append(prompt);
    payload["messages"] = msgArray;

    // Use the same generation settings – but no streaming
    payload["stream"] = false;
    payload["cache_prompt"] = true;
    payload["reasoning_format"] = "none";
    payload["samplers"] = settings().samplers.value();
    payload["temperature"] = settings().temperature.value();
    payload["dynatemp_range"] = settings().dynatemp_range.value();
    payload["dynatemp_exponent"] = settings().dynatemp_exponent.value();
    payload["top_k"] = settings().top_k.value();
    payload["top_p"] = settings().top_p.value();
    payload["min_p"] = settings().min_p.value();
    payload["typical_p"] = settings().typical_p.value();
    payload["xtc_probability"] = settings().xtc_probability.value();
    payload["xtc_threshold"] = settings().xtc_threshold.value();
    payload["repeat_last_n"] = settings().repeat_last_n.value();
    payload["repeat_penalty"] = settings().repeat_penalty.value();
    payload["presence_penalty"] = settings().presence_penalty.value();
    payload["frequency_penalty"] = settings().frequency_penalty.value();
    payload["dry_multiplier"] = settings().dry_multiplier.value();
    payload["dry_base"] = settings().dry_base.value();
    payload["dry_allowed_length"] = settings().dry_allowed_length.value();
    payload["dry_penalty_last_n"] = settings().dry_penalty_last_n.value();
    payload["max_tokens"] = settings().max_tokens.value();
    payload["timings_per_token"] = settings().showTokensPerSecond.value();

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());

    QObject::connect(reply, &QNetworkReply::finished, [reply, onSuccess, this, convId]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(llamaChatNetwork) << "Title summary request failed:" << reply->errorString();
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            qCWarning(llamaChatNetwork) << "Title summary JSON malformed";
            return;
        }

        QJsonObject obj = doc.object();
        QJsonArray choices = obj.value("choices").toArray();
        if (choices.isEmpty())
            return;

        QJsonObject choice = choices[0].toObject();
        QJsonObject message = choice.value("message").toObject();
        QString title = message.value("content").toString().trimmed();

        if (!title.isEmpty())
            onSuccess(title);
    });
}

void ChatManager::stopGenerating(const QString &convId)
{
    if (!m_pendingMessages.contains(convId))
        return;

    m_pendingMessages.remove(convId);
    if (m_abortControllers.contains(convId)) {
        auto controller = m_abortControllers.take(convId);
        controller->abort();
        controller->deleteLater();
    }
}

void ChatManager::replaceMessageAndGenerate(const QString &convId,
                                            qint64 parentNodeId,
                                            const QString &content,
                                            const QList<QVariantMap> &extra,
                                            std::function<void(qint64)> onChunk)
{
    if (isGenerating(convId))
        return;

    if (!content.isEmpty()) {
        auto now = QDateTime::currentMSecsSinceEpoch();
        Message newMsg;
        newMsg.id = now;
        newMsg.convId = convId;
        newMsg.type = "text";
        newMsg.timestamp = now;
        newMsg.role = "user";
        newMsg.content = content;
        newMsg.parent = parentNodeId;
        newMsg.children.clear();
        newMsg.extra = extra;

        m_storage->appendMsg(newMsg, parentNodeId);
        parentNodeId = newMsg.id;
    }

    onChunk(parentNodeId);
    generateMessage(convId, parentNodeId, onChunk);
}

LlamaCppServerProps ChatManager::serverProps() const
{
    return m_serverProps;
}

QJsonArray ChatManager::normalizeMsgsForAPI(const QVector<Message> &msgs)
{
    QJsonArray res;
    for (const Message &msg : msgs) {
        if (msg.role != "user" || msg.extra.isEmpty()) {
            QJsonObject out;
            out["role"] = msg.role;
            out["content"] = msg.content;
            res.append(out);
            continue;
        }

        // user msg with extra – build array of parts
        QJsonArray parts;
        for (const QVariantMap &e : msg.extra) {
            if (e.value("type").toString() == "context") {
                QJsonObject p;
                p["type"] = "text";
                p["text"] = e.value("content").toString();
                parts.append(p);
            } else if (e.value("type").toString() == "textFile") {
                QJsonObject p;
                p["type"] = "text";
                p["text"] = QString("File: %1\nContent:\n\n%2")
                                .arg(e.value("name").toString())
                                .arg(e.value("content").toString());
                parts.append(p);
            } else if (e.value("type").toString() == "imageFile") {
                QJsonObject p;
                p["type"] = "image_url";
                p["image_url"] = QJsonObject{{"url", e.value("base64Url").toString()}};
                parts.append(p);
            } else if (e.value("type").toString() == "audioFile") {
                QJsonObject p;
                p["type"] = "input_audio";
                p["input_audio"] = QJsonObject{{"data", e.value("base64Data").toString()},
                                               {"format",
                                                e.value("mimeType").toString().contains("wav")
                                                    ? "wav"
                                                    : "mp3"}};
                parts.append(p);
            }
        }

        // user text at the end
        QJsonObject txt;
        txt["type"] = "text";
        txt["text"] = msg.content;
        parts.append(txt);

        QJsonObject out;
        out["role"] = msg.role;
        out["content"] = parts;
        res.append(out); // we only need the role/content to build APIMessage
    }
    return res;
}

void LlamaCpp::ChatManager::deleteConversation(const QString &convId)
{
    m_storage->deleteConversation(convId);
}

void ChatManager::renameConversation(const QString &convId, const QString &name)
{
    m_storage->renameConversation(convId, name);
}

} // namespace LlamaCpp
