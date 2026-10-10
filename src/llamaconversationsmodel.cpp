#include "llamaconversationsmodel.h"
#include "llamachatmanager.h"
#include "llamatr.h"

namespace LlamaCpp {

struct ConversationsModel::Node
{
    Conversation conv;
    QVector<Node *> children;
    Node *parent = nullptr;

    ~Node()
    {
        qDeleteAll(children);
    }
};

ConversationsModel::ConversationsModel(QObject *parent)
    : QAbstractItemModel(parent)
    , m_root(new Node)
{}

ConversationsModel::~ConversationsModel()
{
    delete m_root;
}

ConversationsModel::Node *ConversationsModel::nodeFromIndex(const QModelIndex &index) const
{
    if (index.isValid())
        return static_cast<Node *>(index.internalPointer());
    return m_root;
}

QModelIndex ConversationsModel::index(int row, int column, const QModelIndex &parent) const
{
    if (row < 0 || column != 0)
        return {};

    Node *parentNode = nodeFromIndex(parent);
    if (!parentNode || row >= parentNode->children.size())
        return {};

    Node *node = parentNode->children.at(row);
    if (node)
        return createIndex(row, column, node);
    return {};
}

QModelIndex ConversationsModel::parent(const QModelIndex &index) const
{
    if (!index.isValid())
        return {};

    Node *child = nodeFromIndex(index);
    if (!child)
        return {};

    Node *p = child->parent;
    if (!p || p == m_root)
        return {};

    return createIndex(p->parent->children.indexOf(p), 0, p);
}

int ConversationsModel::rowCount(const QModelIndex &parent) const
{
    Node *node = nodeFromIndex(parent);
    return node ? node->children.size() : 0;
}

int ConversationsModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 1;
}

QVariant ConversationsModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    Node *node = nodeFromIndex(index);
    if (!node)
        return {};

    const Conversation &c = node->conv;

    switch (role) {
    case Qt::DisplayRole:
    case Qt::EditRole:
        return c.name;
    case Qt::ToolTipRole:
        // Render the lastModified as ISO‑8601 for readability
        return QDateTime::fromMSecsSinceEpoch(c.lastModified, Qt::LocalTime).toString(Qt::ISODate);
    case TimestampRole:
        // Custom role: return the raw epoch value
        return c.lastModified;
    case ConversationIdRole:
        return c.id;
    default:
        break;
    }

    return {};
}

bool ConversationsModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid())
        return false;

    Node *node = nodeFromIndex(index);
    if (!node)
        return false;

    ChatManager::instance().renameConversation(node->conv.id, value.toString());
    return true;
}

Qt::ItemFlags ConversationsModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    return QAbstractItemModel::flags(index) | Qt::ItemIsEditable;
}

QHash<int, QByteArray> ConversationsModel::roleNames() const
{
    QHash<int, QByteArray> roles;
    roles[Qt::DisplayRole] = "name";
    roles[TimestampRole] = "lastModified";
    roles[ConversationIdRole] = "id";
    return roles;
}

void ConversationsModel::buildTree(const QList<Conversation> &conversations)
{
    m_root->children.clear();
    m_root->conv = {};

    // First pass: create a node for every conversation (not attached yet),
    // keeping the input order (lastModified DESC from the storage layer).
    QHash<QString, Node *> byId;
    byId.reserve(conversations.size());
    QVector<Node *> nodes;
    nodes.reserve(conversations.size());
    for (const Conversation &c : conversations) {
        Node *node = new Node;
        node->conv = c;
        byId.insert(c.id, node);
        nodes.append(node);
    }

    // Second pass: attach each node to its parent, so every node ends up in
    // exactly one children list (see ~Node's qDeleteAll).  A missing / self
    // parent leaves the node at the top level.
    for (Node *node : std::as_const(nodes)) {
        Node *parent = m_root;
        const QString &pid = node->conv.parentId;
        if (!pid.isEmpty() && pid != node->conv.id) {
            auto it = byId.find(pid);
            if (it != byId.end())
                parent = it.value();
        }
        node->parent = parent;
        parent->children.append(node);
    }
}

void ConversationsModel::setConversations(const QList<Conversation> &conversations)
{
    beginResetModel();
    buildTree(conversations);
    endResetModel();
}

void ConversationsModel::collectConversations(Node *node, QList<Conversation> &res)
{
    for (Node *child : std::as_const(node->children)) {
        res.append(child->conv);
        collectConversations(child, res);
    }
}

QList<Conversation> ConversationsModel::allConversations() const
{
    QList<Conversation> res;
    res.reserve(m_root->children.size());
    collectConversations(m_root, res);
    return res;
}

} // namespace LlamaCpp
