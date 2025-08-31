#include "llamaconversationsmodel.h"
#include "llamachatmanager.h"
#include "llamatr.h"

namespace LlamaCpp {

ConversationsModel::ConversationsModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

int ConversationsModel::rowCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return static_cast<int>(m_items.size());
}

int ConversationsModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 3;
}

QVariant ConversationsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    if (index.row() < 0 || index.row() >= rowCount())
        return {};

    const Conversation &c = m_items.at(index.row());

    if (role == Qt::DisplayRole) {
        if (index.column() == 0) {
            return c.name;
        }
    } else if (role == Qt::ToolTipRole) {
        // Render the lastModified as ISO‑8601 for readability
        QDateTime dt = QDateTime::fromMSecsSinceEpoch(c.lastModified, Qt::UTC);
        return dt.toString(Qt::ISODate);
    } else if (role == TimestampRole) {
        // Custom role: return the raw epoch value
        return c.lastModified;
    } else if (role == ConversationIdRole) {
        return c.id;
    }

    return {};
}

bool ConversationsModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role == Qt::EditRole) {
        if (index.column() == 0) {
            const Conversation &c = m_items.at(index.row());

            ChatManager::instance().renameConversation(c.id, value.toString());
            return true;
        }
    }
    return false;
}

QVariant ConversationsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (role != Qt::DisplayRole)
        return {};

    if (orientation == Qt::Horizontal) {
        if (section == 0)
            return Tr::tr("Name");
        else if (section == 1)
            return Tr::tr("Date");
        else if (section == 2)
            return Tr::tr("Conversation Id");
    }

    return {};
}

Qt::ItemFlags ConversationsModel::flags(const QModelIndex &index) const
{
    Qt::ItemFlags f = QAbstractTableModel::flags(index);

    if (!index.isValid())
        return Qt::NoItemFlags;
    else
        return f |= Qt::ItemIsEditable;

    // All items are read‑only in this simple model
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QHash<int, QByteArray> ConversationsModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[Qt::DisplayRole] = "name";
    roles[TimestampRole] = "lastModified";
    roles[ConversationIdRole] = "id";
    return roles;
}

void ConversationsModel::addConversation(const Conversation &c)
{
    beginInsertRows(QModelIndex(), rowCount(), rowCount());
    m_items.append(c);
    endInsertRows();
}

void ConversationsModel::clear()
{
    if (m_items.isEmpty())
        return;

    beginRemoveRows(QModelIndex(), 0, rowCount() - 1);
    m_items.clear();
    endRemoveRows();
}

QList<Conversation> ConversationsModel::allConversations() const
{
    return QList<Conversation>(m_items.begin(), m_items.end());
}

} // namespace LlamaCpp
