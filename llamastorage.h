#pragma once

#include <QSqlDatabase>
#include <QVariant>
#include <QVector>

namespace LlamaCpp {

struct Conversation;
struct Message;

class Storage : public QObject
{
    Q_OBJECT
public:
    Storage();
    static Storage &instance(); // singleton

    QList<Conversation> getAllConversations();
    Conversation getOneConversation(const QString &convId);
    Conversation createConversation(const QString &name);
    void renameConversation(const QString &convId, const QString &name);
    void deleteConversation(const QString &convId);

    QVector<Message> getMessages(const QString &convId);
    void appendMsg(Message &msg, qint64 parentNodeId);
    QVector<Message> filterByLeafNodeId(const QVector<Message> &msgs,
                                        qint64 leafNodeId,
                                        bool includeRoot);
    bool updateMessageExtra(const LlamaCpp::Message &msg, const QList<QVariantMap> &extra);
    bool updateMessageContent(const Message &msg);

signals:
    void conversationCreated(const QString &convId);
    void conversationRenamed(const QString &convId);
    void conversationDeleted(const QString &convId);
    void messageAppended(const LlamaCpp::Message &msg, qint64 pendingId);
    void messageExtraUpdated(const LlamaCpp::Message &msg, const QList<QVariantMap> &newExtra);
    void messageContentUpdated(const LlamaCpp::Message &msg);

private:
    QSqlDatabase db;
};

} // namespace LlamaCpp
