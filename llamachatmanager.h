#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include <memory>

#include "llamatypes.h"

namespace LlamaCpp {

class Storage;
class Tool;

struct ToolCall
{
    QString name;      // e.g. "python"
    QString arguments; // the JSON string that is being streamed
    QString id;        // tool-call identifier
};

class ChatManager : public QObject
{
    Q_OBJECT
public:
    static ChatManager &instance(); // singleton

    void refreshServerProps();

    bool isGenerating(const QString &convId) const;
    ViewingChat getViewingChat(const QString &convId) const;
    QVector<Message> filterByLeafNodeId(const QVector<Message> &messages,
                                        qint64 leafNodeId,
                                        bool includeRoot);

    void sendMessage(const QString &convId,
                     qint64 leafNodeId,
                     const QString &content,
                     const QList<QVariantMap> &extra,
                     std::function<void(qint64)> onChunk);
    void stopGenerating(const QString &convId);
    void replaceMessageAndGenerate(const QString &convId,
                                   qint64 parentNodeId,
                                   const QString &content,
                                   const QList<QVariantMap> &extra,
                                   std::function<void(qint64)> onChunk);

    LlamaCppServerProps serverProps() const;

    //! True if the current model's chat template looks like it supports
    //! thinking/reasoning control (same heuristics as the llama.cpp web UI).
    bool serverSupportsThinking() const;

    struct ModelEntry
    {
        QString id;
        QString status; // "loaded"/"loading"/"unloaded"/...; empty in single-model mode
    };

    //! Models reported by the server's /models endpoint.
    QList<ModelEntry> models() const;
    QString selectedModel() const;
    void refreshModels();
    //! Switches the active model; in router mode this also asks the server
    //! to load the model when it is not loaded yet.
    void selectModel(const QString &id);

    void generateMessage(const QString &convId,
                         qint64 leafNodeId,
                         std::function<void(qint64)> onChunk);

    Conversation currentConversation();
    void setCurrentConversation(const QString &convId);

    Conversation createConversation(const QString &name);

    //! Creates a conversation used by the "task" tool as a sub‑agent session.
    //! Unlike createConversation() it does not make the new conversation the
    //! active one, so the UI keeps showing the parent conversation.
    Conversation createTaskConversation(const QString &name);

    //! Sets the system prompt and the whitelist of tools that may be used in
    //! a task conversation.  When the whitelist is empty every enabled tool
    //! is available.  "task" itself is never advertised in a sub‑conversation
    //! (no sub‑agent recursion).
    void configureTaskConversation(const QString &convId,
                                   const QString &systemPrompt,
                                   const QStringList &allowedTools);

    void deleteConversation(const QString &convId);
    void renameConversation(const QString &convId, const QString &name);
    void deleteMessageBranch(const QString &convId, qint64 msgId);
    QPair<int,int> getBranchStats(const QString &convId, qint64 msgId) const;

    QList<Conversation> allConversations();

    void summarizeConversationTitle(const QString &convId,
                                    qint64 leafNodeId,
                                    std::function<void(const QString &)> onSuccess);

    void followUpQuestions(const QString &convId,
                           qint64 leafNodeId,
                           std::function<void(const QStringList &)> onSuccess);

    void cancelTitleSummary(const QString &convId);
    void cancelFollowUp(const QString &convId);

    void executeToolAndSendResult(const QString &convId,
                                  const LlamaCpp::Message &msg,
                                  const ToolCall &tool,
                                  std::function<void(qint64)> onChunk);

signals:
    void modelsUpdated();

    // emitted when the active conversation changes – UI can react
    void messageAppended(const LlamaCpp::Message &msg, qint64 pendingId);
    void pendingMessageChanged(const LlamaCpp::Message &msg);

    void conversationCreated(const QString &convId);
    void conversationRenamed(const QString &convId);
    void conversationDeleted(const QString &convId);

    //! Emitted when a task conversation has reached its end: the final
    //! assistant message was committed and no tool call keeps the loop
    //! alive.  @p ok is false when the stream was aborted or produced no
    //! content at all.
    void taskConversationFinished(const QString &convId, const QString &content, bool ok);

    void serverPropsUpdated();
    void followUpQuestionsReceived(const QString &convId,
                                   qint64 leafNodeId,
                                   const QStringList &quetions);
    void messageExtraUpdated(const LlamaCpp::Message &msg, const QList<QVariantMap> &newExtra);
    void messageDeleted(const QString &convId);

private:
    explicit ChatManager(QObject *parent = nullptr);
    void initServerProps();
    void updateModelPolling();

    QJsonArray normalizeMsgsForAPI(const QVector<Message> &msgs);

    void sendChatRequest(const QString &convId,
                         const std::function<void(QJsonObject &payload)> &payloadBuilder,
                         std::function<void(qint64)> onChunk);

    // Fills the payload for auxiliary requests (conversation title, follow‑up
    // suggestions). Unlike addCommonPayloadParams() it does NOT apply the
    // user's sampling settings and max_tokens: the token budget is capped at
    // @p maxTokens and thinking is turned off, so these housekeeping calls
    // stay short and do not starve real chat generations on the server.
    void addAuxiliaryPayloadParams(QJsonObject &payload, int maxTokens) const;

    // internal state
    Storage *m_storage;
    bool m_showSettings{false};

    QNetworkAccessManager m_network;
    LlamaCppServerProps m_serverProps;

    QList<ModelEntry> m_models;
    QString m_selectedModel;
    QTimer *m_modelPollTimer{nullptr}; // polls /models while a model is loading
    QString m_activeConvId;

    QHash<QString, Message> m_pendingMessages;
    QHash<QString, QNetworkReply *> m_abortControllers;
    QHash<QString, QNetworkReply *> m_titleSummaryReplies;
    QHash<QString, QNetworkReply *> m_followUpReplies;
    QVector<ToolCall> m_toolCalls;
    QHash<QString, std::shared_ptr<LlamaCpp::Tool>> m_streamingTools;

    //! Number of tools currently executing (per conversation).  Tools run
    //! asynchronously, so a conversation stays "busy" until every in‑flight
    //! tool has reported back.
    QHash<QString, int> m_runningTools;

    // Task‑conversation (sub‑agent) state
    struct TaskConversationConfig
    {
        QString systemPrompt;
        QStringList allowedTools; // empty = every enabled tool
    };
    QSet<QString> m_taskConversations;
    QHash<QString, TaskConversationConfig> m_taskConfigs;
    bool m_taskConvCreationPending{false};
};
} // namespace LlamaCpp
