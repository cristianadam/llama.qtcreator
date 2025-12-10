#include <QCoreApplication>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QProcess>

#include <coreplugin/documentmanager.h>

#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>

#include "llamachatmanager.h"
#include "llamasettings.h"
#include "llamastorage.h"
#include "llamathinkingsectionparser.h"
#include "llamatr.h"
#include "tools/factory.h"
#include "tools/tool.h"

Q_LOGGING_CATEGORY(llamaChatNetwork, "llama.cpp.chat.network", QtWarningMsg)
Q_LOGGING_CATEGORY(llamaChatTools, "llama.cpp.chat.tools", QtWarningMsg)

using namespace ProjectExplorer;
using namespace Utils;

namespace LlamaCpp {

static void addCommonPayloadParams(QJsonObject &payload)
{
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
}

static void addToolsToPayload(QJsonObject &payload)
{
    if (settings().tools.value().isEmpty())
        return;

    QJsonArray toolsArr;
    for (const QString &toolStr : settings().tools.value()) {
        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(toolStr.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject()) {
            qWarning() << "Invalid tool JSON:" << err.errorString();
            continue;
        }
        toolsArr.append(doc.object());
    }

    payload["tools"] = toolsArr;
}

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
    connect(m_storage, &Storage::messageExtraUpdated, this, &ChatManager::messageExtraUpdated);
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
        QJsonObject genSettings = obj.value("default_generation_settings").toObject();
        m_serverProps.n_ctx = genSettings.value("n_ctx").toInt();
        QJsonObject mod = obj.value("modalities").toObject();
        m_serverProps.modalities.vision = mod.value("vision").toBool();
        m_serverProps.modalities.audio = mod.value("audio").toBool();
        reply->deleteLater();

        ThinkingSectionParser::setTokensFromServerProps(m_serverProps);
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
    if (isGenerating(convId) || content.trimmed().isEmpty() || convId.isEmpty())
        return;

    Message newMsg;
    auto now = QDateTime::currentMSecsSinceEpoch();
    newMsg.convId = convId;
    newMsg.type = "text";
    newMsg.timestamp = now;
    newMsg.role = "user";
    newMsg.content = content;
    newMsg.parent = leafNodeId;
    newMsg.children.clear();
    newMsg.extra = extra; // simple wrapper – see MessageExtra

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
    pending.id = QDateTime::currentMSecsSinceEpoch();
    pending.convId = convId;
    pending.type = "text";
    pending.timestamp = pending.id;
    pending.role = "assistant";
    pending.content = QString();
    pending.parent = leafNodeId;
    pending.children.clear();
    m_pendingMessages.insert(convId, pending);
    m_abortControllers[convId] = nullptr; // will hold the reply

    sendChatRequest(
        convId,
        [leafMsgs, this](QJsonObject &payload) {
            payload["messages"] = normalizeMsgsForAPI(leafMsgs);
            if (m_toolsSupport)
                addToolsToPayload(payload);

            // custom JSON from settings (if any)
            if (!settings().customJson.value().isEmpty()) {
                QJsonDocument d = QJsonDocument::fromJson(settings().customJson.value().toUtf8());
                if (d.isObject())
                    for (auto it = d.object().constBegin(); it != d.object().constEnd(); ++it)
                        payload[it.key()] = it.value();
            }
        },
        onChunk);
}

void ChatManager::followUpQuestions(const QString &convId,
                                    qint64 leafNodeId,
                                    std::function<void(const QStringList &)> onSuccess)
{
    auto allMsgs = m_storage->getMessages(convId);
    auto leafMsgs = m_storage->filterByLeafNodeId(allMsgs, leafNodeId, false);
    QJsonArray msgArray = normalizeMsgsForAPI(leafMsgs);

    QJsonArray parts;
    QJsonObject txt;
    txt["type"] = "text";
    txt["text"] = "Generate up to five follow up questions in the context of the "
                  "current conversation. The questions are from the user point of view. "
                  "Only questions, no explanations. Use the language used in the conversation. "
                  "Return the questions in the form of a JSON array as plain text strings, "
                  "no markdown.";
    parts.append(txt);
    QJsonObject prompt;
    prompt["role"] = "user";
    prompt["content"] = parts;
    msgArray.append(prompt);

    QJsonObject payload;
    payload["messages"] = msgArray;
    payload["stream"] = false;
    payload["cache_prompt"] = true;
    payload["reasoning_format"] = "none";
    addCommonPayloadParams(payload);

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());
    m_followUpReplies.insert(convId, reply);

    QObject::connect(reply, &QNetworkReply::finished, [reply, onSuccess, convId, this]() {
        m_followUpReplies.remove(convId);
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            qCWarning(llamaChatNetwork) << "Follow‑up request failed:" << reply->errorString();
            return;
        }

        QByteArray data = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(data);
        if (!doc.isObject()) {
            qCWarning(llamaChatNetwork) << "Follow‑up JSON malformed";
            return;
        }

        QJsonObject obj = doc.object();
        QJsonArray choices = obj.value("choices").toArray();
        if (choices.isEmpty())
            return;

        QJsonObject choice = choices[0].toObject();
        QJsonObject message = choice.value("message").toObject();

        // Skip the thinking part
        auto [thinking, content] = ThinkingSectionParser::parseThinkingSection(
            message.value("content").toString().trimmed());

        if (content.isEmpty())
            return;

        // Sometimes the model continues "thinking" also in the answer
        const QString startOfArray("[\"");
        const QString endOfArray("\"]");

        if (!content.startsWith(startOfArray)) {
            auto startOfArrayIdx = content.lastIndexOf(startOfArray);
            if (startOfArrayIdx != -1)
                content = content.mid(startOfArrayIdx);
        }

        // Sometimes we have \n``` at the end
        if (!content.endsWith(endOfArray)) {
            auto endOfArrayIdx = content.lastIndexOf(endOfArray);
            if (endOfArrayIdx != -1)
                content = content.left(endOfArrayIdx + endOfArray.size());
        }

        // `content` should be a JSON array of strings (plain text).
        QJsonParseError err;
        QJsonDocument arrDoc = QJsonDocument::fromJson(content.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            qCWarning(llamaChatNetwork) << "Could not parse follow‑up array:" << err.errorString();
            qCWarning(llamaChatNetwork) << "The faulty content was:" << content.toUtf8();
            return;
        }

        if (!arrDoc.isArray())
            return;

        QStringList questions;
        for (const QJsonValue &v : arrDoc.array())
            questions.append(v.toString());

        if (onSuccess)
            onSuccess(questions);
    });
}

void ChatManager::cancelTitleSummary(const QString &convId)
{
    if (m_titleSummaryReplies.contains(convId)) {
        auto r = m_titleSummaryReplies.take(convId);
        r->abort();
        r->deleteLater();
    }
}

void ChatManager::cancelFollowUp(const QString &convId)
{
    if (m_followUpReplies.contains(convId)) {
        auto r = m_followUpReplies.take(convId);
        r->abort();
        r->deleteLater();
    }
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
                  "the language used in the conversation. Use plain text, no markdown.";
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
    addCommonPayloadParams(payload);

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());
    m_titleSummaryReplies.insert(convId, reply);

    QObject::connect(reply, &QNetworkReply::finished, [reply, onSuccess, this, convId]() {
        m_titleSummaryReplies.remove(convId);
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

static Message createToolMessage(const Message &assistantCall)
{
    Message toolMsg;
    toolMsg.convId = assistantCall.convId;
    toolMsg.type = "text";
    toolMsg.timestamp = QDateTime::currentMSecsSinceEpoch();
    toolMsg.role = "tool";
    toolMsg.content = QString();
    toolMsg.parent = assistantCall.id; // link to the assistant that called it
    toolMsg.children.clear();

    toolMsg.extra << assistantCall.extra; // copy the tool calling for display
    return toolMsg;
}

QJsonArray ChatManager::normalizeMsgsForAPI(const QVector<Message> &msgs)
{
    QJsonArray res;

    const QString sysMsgText = LlamaCpp::settings().systemMessage.value();
    if (!sysMsgText.trimmed().isEmpty()) {
        QJsonObject sys;
        sys["role"] = "system";
        sys["content"] = sysMsgText;
        res.append(sys);
    }

    for (const Message &msg : msgs) {
        if (msg.role != "user" || msg.extra.isEmpty()) {
            QJsonObject out;
            out["role"] = msg.role;
            out["content"] = msg.content;

            if (msg.role == "assistant") {
                for (const QVariantMap &e : msg.extra) {
                    if (e.contains("tool_calls"))
                        out["tool_calls"] = e["tool_calls"].toJsonArray();
                }
            } else if (msg.role == "tool") {
                for (const QVariantMap &e : msg.extra) {
                    if (e.contains("tool_result"))
                        res.append(e["tool_result"].toJsonObject());
                }
            }

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

void ChatManager::sendChatRequest(const QString &convId,
                                  const std::function<void(QJsonObject &)> &payloadBuilder,
                                  std::function<void(qint64)> onChunk)
{
    QJsonObject payload;
    payloadBuilder(payload); // <- fills the request‑specific fields

    payload["stream"] = true;
    payload["cache_prompt"] = true;
    payload["reasoning_format"] = "none";
    addCommonPayloadParams(payload);

    QNetworkRequest req(QUrl(settings().chatEndpoint.value() + "/v1/chat/completions"));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    if (!settings().chatApiKey.value().isEmpty())
        req.setRawHeader("Authorization", ("Bearer " + settings().chatApiKey.value()).toUtf8());

    QNetworkReply *reply = m_network.post(req, QJsonDocument(payload).toJson());
    m_abortControllers[convId] = reply; // (same map used elsewhere)
    qCDebug(llamaChatNetwork).noquote() << "Request payload:" << QJsonDocument(payload).toJson();

    readSSEStream(
        reply,
        [this, convId, onChunk](const QJsonObject &chunk) {
            if (chunk.contains("error")) {
                qCWarning(llamaChatNetwork)
                    << "SSE error:" << chunk["error"].toObject()["message"].toString();
                return;
            }

            if (settings().showTokensPerSecond.value() && chunk.contains("timings")) {
                QJsonObject t = chunk["timings"].toObject();
                TimingReport tr;
                tr.prompt_n = t["prompt_n"].toDouble();
                tr.prompt_ms = t["prompt_ms"].toDouble();
                tr.predicted_n = t["predicted_n"].toDouble();
                tr.predicted_ms = t["predicted_ms"].toDouble();
                m_pendingMessages[convId].timings = tr;
            }

            QJsonArray choices = chunk["choices"].toArray();
            if (!choices.isEmpty()) {
                QString added = choices[0].toObject()["delta"].toObject()["content"].toString();
                Message &pm = m_pendingMessages[convId];
                if (!added.isEmpty()) {
                    pm.content += added;
                    emit pendingMessageChanged(pm);
                }

                for (const QJsonValue &choiceVal : choices) {
                    const QJsonObject &choice = choiceVal.toObject();
                    const QJsonObject &delta = choice["delta"].toObject();
                    if (!delta.contains("tool_calls"))
                        continue;

                    const QJsonArray &toolCalls = delta["tool_calls"].toArray();
                    for (const QJsonValue &tcVal : toolCalls) {
                        const QJsonObject &tc = tcVal.toObject();
                        const QString toolId = tc["id"].toString();
                        const int index = tc["index"].toInt();

                        while (index >= m_toolCalls.size())
                            m_toolCalls.emplace_back();

                        if (index < 0)
                            continue;

                        ToolCall &tool = m_toolCalls[index];
                        if (!toolId.isEmpty())
                            tool.id = toolId;

                        if (tc.contains("function")) {
                            const QJsonObject &func = tc["function"].toObject();
                            if (func.contains("name"))
                                tool.name = func["name"].toString();
                            if (func.contains("arguments"))
                                tool.arguments += func["arguments"].toString();
                        }

                        QJsonParseError err;
                        QJsonDocument parsed = QJsonDocument::fromJson(tool.arguments.toUtf8(),
                                                                       &err);
                        if (err.error == QJsonParseError::NoError) {
                            QVariantMap extra;
                            extra["tool_calls"] = QJsonArray{
                                QJsonObject{{"id", tool.id},
                                            {"type", "function"},
                                            {"function",
                                             QJsonObject{{"name", tool.name},
                                                         {"arguments", tool.arguments}}}}};
                            pm.extra << extra;

                            m_toolCalls.remove(index);
                        }

                        emit pendingMessageChanged(pm);
                    }
                }
            }
            onChunk(-1); // UI scroll‑to‑bottom
        },
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
        m_storage->appendMsg(pm, pm.parent);

        if (pm.role == "assistant") {
            auto msgs = m_storage->getMessages(convId);
            const bool doSummarization = msgs.size() == 3;
            bool haveToolExecution = false;

            for (const QVariantMap &e : pm.extra) {
                if (e.contains("tool_calls")) {
                    // Store the "tool_calls" into the database
                    m_storage->updateMessageExtra(pm, pm.extra);

                    QJsonArray array = e["tool_calls"].toJsonArray();
                    for (const QJsonValue &v : array) {
                        QJsonObject obj = v.toObject();
                        ToolCall tool;
                        tool.id = obj["id"].toString();
                        obj = obj["function"].toObject();
                        tool.name = obj["name"].toString();
                        tool.arguments = obj["arguments"].toString();

                        executeToolAndSendResult(convId, pm, tool, [](qint64) {});
                        haveToolExecution = true;
                    }
                }
            }

            if (doSummarization) { // first assistant reply
                summarizeConversationTitle(convId, pm.id, [this, convId](const QString &title) {
                    auto [thinking, shortTitle] = ThinkingSectionParser::parseThinkingSection(title);
                    renameConversation(convId, shortTitle);
                });
            }

            if (!haveToolExecution)
                followUpQuestions(convId,
                                  pm.id,
                                  [this, convId, leafNodeId = pm.id](const QStringList &questions) {
                                      emit followUpQuestionsReceived(convId, leafNodeId, questions);
                                  });
        }
    });
}

void ChatManager::executeToolAndSendResult(const QString &convId,
                                           const Message &assistantMsg,
                                           const ToolCall &tool,
                                           std::function<void(qint64)> onChunk)
{
    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(tool.arguments.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError) {
        qCWarning(llamaChatNetwork)
            << "Tool args JSON malformed:" << err.errorString() << tool.arguments.toUtf8();
        return;
    }

    qCInfo(llamaChatTools).noquote() << "Calling tool:" << tool.name << "with arguments:\n"
                                     << tool.arguments;

    std::unique_ptr<Tool> realTool = ToolFactory::instance().create(tool.name);
    if (!realTool) {
        qCWarning(llamaChatNetwork) << "Unsupported tool:" << tool.name;
        return;
    }

    Message toolMsg = createToolMessage(assistantMsg);
    m_storage->appendMsg(toolMsg, assistantMsg.id);
    onChunk(toolMsg.id);

    auto toolFinished = [this, convId, toolMsg, tool, onChunk](const QString &toolOutput,
                                                               bool ok) mutable {
        QJsonObject toolJsonMsg;
        toolJsonMsg["role"] = "tool";
        toolJsonMsg["tool_call_id"] = tool.id;
        toolJsonMsg["name"] = tool.name;
        toolJsonMsg["content"] = toolOutput;
        QVariantMap toolResultExtra;
        toolResultExtra["tool_result"] = toolJsonMsg;
        toolResultExtra["tool_status"] = ok ? QStringLiteral("success") : "failed";

        toolMsg.extra << toolResultExtra;
        m_storage->updateMessageExtra(toolMsg, toolMsg.extra);

        // generate assistant reply
        generateMessage(toolMsg.convId, toolMsg.id, onChunk);
    };

    if (realTool) {
        realTool->run(doc.object(), std::move(toolFinished));
    } else {
        qCWarning(llamaChatNetwork) << "Unsupported tool:" << tool.name;
        return;
    }
}

void ChatManager::onToolsSupportEnabled(bool enabled)
{
    m_toolsSupport = enabled;
}

void ChatManager::deleteConversation(const QString &convId)
{
    m_storage->deleteConversation(convId);
}

void ChatManager::renameConversation(const QString &convId, const QString &name)
{
    if (name.isEmpty())
        return;

    m_storage->renameConversation(convId, name);
}

} // namespace LlamaCpp
