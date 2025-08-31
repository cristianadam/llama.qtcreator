#pragma once

#include <QAbstractTableModel>
#include <QDateTime>
#include <QString>
#include <QVector>

#include "llamatypes.h"

namespace LlamaCpp {

class ConversationsModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    explicit ConversationsModel(QObject *parent = nullptr);

    /* Standard QAbstractItemModel overrides */
    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;
    QVariant headerData(int section,
                        Qt::Orientation orientation,
                        int role = Qt::DisplayRole) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    /* Convenience API for the caller */
    void addConversation(const Conversation &c);
    void clear();
    QList<Conversation> allConversations() const;

    /* Custom role definition */
    enum ConversationRoles {
        TimestampRole = Qt::UserRole + 1,     // raw epoch value
        ConversationIdRole = Qt::UserRole + 2 // raw epoch value
    };
    Q_ENUM(ConversationRoles)

protected:
    QHash<int, QByteArray> roleNames() const override;

private:
    QVector<Conversation> m_items;
};

} // namespace LlamaCpp
