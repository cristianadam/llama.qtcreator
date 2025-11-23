#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QVariantMap>

#include "llamatypes.h"

namespace LlamaCpp {

class Storage;

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

    void generateMessage(const QString &convId,
                         qint64 leafNodeId,
                         std::function<void(qint64)> onChunk);

    Conversation currentConversation();
    void setCurrentConversation(const QString &convId);

    Conversation createConversation(const QString &name);
    void deleteConversation(const QString &convId);
    void renameConversation(const QString &convId, const QString &name);

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
    // emitted when the active conversation changes – UI can react
    void messageAppended(const LlamaCpp::Message &msg, qint64 pendingId);
    void pendingMessageChanged(const LlamaCpp::Message &msg);

    void conversationCreated(const QString &convId);
    void conversationRenamed(const QString &convId);
    void conversationDeleted(const QString &convId);

    void serverPropsUpdated();
    void followUpQuestionsReceived(const QString &convId,
                                   qint64 leafNodeId,
                                   const QStringList &quetions);
    void messageExtraUpdated(const LlamaCpp::Message &msg, const QList<QVariantMap> &newExtra);

public slots:
    void onToolsSupportEnabled(bool enabled);

private:
    explicit ChatManager(QObject *parent = nullptr);
    void initServerProps();

    QJsonArray normalizeMsgsForAPI(const QVector<Message> &msgs);

    void sendChatRequest(const QString &convId,
                         const std::function<void(QJsonObject &payload)> &payloadBuilder,
                         std::function<void(qint64)> onChunk);

    // internal state
    Storage *m_storage;
    bool m_showSettings{false};

    QNetworkAccessManager m_network;
    LlamaCppServerProps m_serverProps;
    QString m_activeConvId;

    QHash<QString, Message> m_pendingMessages;
    QHash<QString, QNetworkReply *> m_abortControllers;
    QHash<QString, QNetworkReply *> m_titleSummaryReplies;
    QHash<QString, QNetworkReply *> m_followUpReplies;
    QHash<QString, bool> m_continuationMode;
    QVector<ToolCall> m_toolCalls;
    bool m_toolsSupport{false};
};
} // namespace LlamaCpp
