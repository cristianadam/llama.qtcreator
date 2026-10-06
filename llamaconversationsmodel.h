#pragma once

#include <QAbstractItemModel>
#include <QDateTime>
#include <QHash>
#include <QString>
#include <QVector>

#include "llamatypes.h"

namespace LlamaCpp {

/**
 * Tree model of the conversations.  Task (sub‑agent) conversations are
 * nested as children of the conversation that spawned them (via
 * Conversation::parentId).  Top‑level conversations have an empty parentId.
 */
class ConversationsModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    explicit ConversationsModel(QObject *parent = nullptr);
    ~ConversationsModel() override;

    /* Standard QAbstractItemModel overrides */
    QModelIndex index(int row, int column, const QModelIndex &parent = QModelIndex()) const override;
    QModelIndex parent(const QModelIndex &index) const override;
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    /* Convenience API for the caller */
    void setConversations(const QList<Conversation> &conversations);
    QList<Conversation> allConversations() const;

    /* Custom role definition */
    enum ConversationRoles {
        TimestampRole = Qt::UserRole + 1,     // raw epoch value
        ConversationIdRole = Qt::UserRole + 2 // raw conversation id
    };
    Q_ENUM(ConversationRoles)

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    struct Node;
    Node *nodeFromIndex(const QModelIndex &index) const;
    void buildTree(const QList<Conversation> &conversations);
    static void collectConversations(Node *node, QList<Conversation> &res);

    Node *m_root{nullptr};
};

} // namespace LlamaCpp
