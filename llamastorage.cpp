#include <QJsonArray>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

#include <coreplugin/icore.h>

#include "llamastorage.h"
#include "llamatypes.h"

Q_LOGGING_CATEGORY(llamaStorage, "llama.cpp.storage", QtWarningMsg)

namespace LlamaCpp {

static QString serialize(const QList<QVariantMap> &list)
{
    QJsonArray arr;
    for (const QVariantMap &m : list)
        arr.append(QJsonObject::fromVariantMap(m));
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact));
}

static QList<QVariantMap> deserializeExtra(const QString &json)
{
    QList<QVariantMap> list;
    QJsonDocument d = QJsonDocument::fromJson(json.toUtf8());
    if (!d.isArray())
        return list;
    for (const QJsonValue &v : d.array()) {
        if (v.isObject())
            list.append(v.toObject().toVariantMap());
    }
    return list;
}

static QString serialize(const TimingReport &tr)
{
    QJsonObject obj;
    obj["prompt_n"] = tr.prompt_n;
    obj["prompt_ms"] = tr.prompt_ms;
    obj["predicted_n"] = tr.predicted_n;
    obj["predicted_ms"] = tr.predicted_ms;

    QJsonDocument doc(obj);
    return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
}

static TimingReport deserializeTimingsReport(const QString &json)
{
    TimingReport result;

    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject())
        return result;

    QJsonObject obj = doc.object();
    auto getDouble = [&](const QString &key, double &field) {
        if (obj.contains(key))
            field = obj[key].toDouble();
    };

    getDouble("prompt_n", result.prompt_n);
    getDouble("prompt_ms", result.prompt_ms);
    getDouble("predicted_n", result.predicted_n);
    getDouble("predicted_ms", result.predicted_ms);

    return result;
}

Storage &Storage::instance()
{
    static Storage inst;
    return inst;
}

Storage::Storage()
{
    const Utils::FilePath cacheResourceDir = Core::ICore::cacheResourcePath(".");
    cacheResourceDir.ensureWritableDir();

    const QString databasePath = Core::ICore::cacheResourcePath("llamacpp.db").path();
    qCDebug(llamaStorage) << "Storage path:" << databasePath;
    db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName(databasePath);
    if (!db.open()) {
        qFatal("Failed to open database: %s", qPrintable(db.lastError().text()));
    }
    // create tables if not exist
    QSqlQuery q(db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS conversations "
                "(id TEXT PRIMARY KEY, lastModified INTEGER, currNode INTEGER, name TEXT)"))
        qCCritical(llamaStorage) << "Failed to create table \"conversations\"" << q.lastError();

    if (!q.exec("CREATE TABLE IF NOT EXISTS messages "
                "(id INTEGER PRIMARY KEY AUTOINCREMENT, convId TEXT, type TEXT, timestamp INTEGER, "
                "role TEXT, "
                "content TEXT, "
                "timings TEXT, "
                "extra TEXT, "
                "parent INTEGER, "
                "children TEXT, "
                "FOREIGN KEY(convId) REFERENCES conversations(id))"))
        qCCritical(llamaStorage) << "Failed to create table \"messages\"" << q.lastError();

    // create indexes for quick lookups
    if (!q.exec("CREATE INDEX IF NOT EXISTS idx_messages_convId ON messages(convId)"))
        qCCritical(llamaStorage) << "Failed to create table \"idx_messages_convId\""
                                 << q.lastError();
}

QList<Conversation> Storage::getAllConversations()
{
    QVector<Conversation> res;
    QSqlQuery q(db);
    q.prepare("SELECT * FROM conversations ORDER BY lastModified DESC");
    if (!q.exec())
        qCWarning(llamaStorage) << "getAllConversations" << q.lastError();

    while (q.next()) {
        Conversation c;
        c.id = q.value("id").toString();
        c.lastModified = q.value("lastModified").toLongLong();
        c.currNode = q.value("currNode").toLongLong();
        c.name = q.value("name").toString();
        res.append(c);
    }
    return res;
}

Conversation Storage::getOneConversation(const QString &convId)
{
    QSqlQuery q(db);
    q.prepare("SELECT * FROM conversations WHERE  id = (:id)");
    q.bindValue(":id", convId);
    if (!q.exec()) {
        qCWarning(llamaStorage) << "getOneConversation" << convId << q.lastError();
        return {};
    }
    if (!q.next())
        return {};

    Conversation c;
    c.id = q.value("id").toString();
    c.lastModified = q.value("lastModified").toLongLong();
    c.currNode = q.value("currNode").toLongLong();
    c.name = q.value("name").toString();
    return c;
}

Conversation Storage::createConversation(const QString &name)
{
    qint64 now = QDateTime::currentMSecsSinceEpoch() - 1;

    QSqlQuery q(db);
    q.prepare("INSERT INTO conversations (id,lastModified,currNode,name) "
              "VALUES (:id,:lm,:curr,:name)");
    QString convId = QString("conv-%1").arg(now);
    q.bindValue(":id", convId);
    q.bindValue(":lm", now);
    q.bindValue(":curr", -1); // Will be updated after root message is created
    q.bindValue(":name", name);
    if (!q.exec())
        qCWarning(llamaStorage) << "createConversation insert into conversations" << q.lastError();

    // create root node
    q.prepare("INSERT INTO messages "
              "(convId,type,timestamp,role,content,timings,extra,parent,children) "
              "VALUES (:conv,:type,:ts,:role,:content,:timings,:extra,:parent,:children)");
    q.bindValue(":conv", convId);
    q.bindValue(":type", "root");
    q.bindValue(":ts", now);
    q.bindValue(":role", "system");
    q.bindValue(":content", "");
    q.bindValue(":timings", "");
    q.bindValue(":extra", "[]");
    q.bindValue(":parent", -1);
    q.bindValue(":children", "[]");
    if (!q.exec())
        qCWarning(llamaStorage) << "createConversation insert into messages" << q.lastError();

    // Get the auto-generated root message ID
    qint64 rootMsgId = q.lastInsertId().toLongLong();

    // Update conversation with actual root message ID
    q.prepare("UPDATE conversations SET currNode = (:curr) WHERE id = (:id)");
    q.bindValue(":curr", rootMsgId);
    q.bindValue(":id", convId);
    if (!q.exec())
        qCWarning(llamaStorage) << "createConversation update currNode" << q.lastError();

    Conversation c;
    c.id = convId;
    c.lastModified = now;
    c.currNode = rootMsgId;
    c.name = name;

    emit conversationCreated(convId);

    return c;
}

void Storage::renameConversation(const QString &convId, const QString &name)
{
    QSqlQuery q(db);
    q.prepare("UPDATE conversations SET name = (:name), lastModified = (:lm) WHERE id = (:id)");
    q.bindValue(":name", name);
    q.bindValue(":lm", QDateTime::currentMSecsSinceEpoch());
    q.bindValue(":id", convId);
    if (!q.exec())
        qCWarning(llamaStorage) << "updateConversationName" << q.lastError();

    emit conversationRenamed(convId);
}

void Storage::deleteConversation(const QString &convId)
{
    QSqlQuery q(db);
    q.prepare("DELETE FROM conversations WHERE id = (:id)");
    q.bindValue(":id", convId);
    if (!q.exec())
        qCWarning(llamaStorage) << "removeConversation from conversations" << q.lastError();

    q.prepare("DELETE FROM messages WHERE convId = (:id)");
    q.bindValue(":id", convId);
    if (!q.exec())
        qCWarning(llamaStorage) << "removeConversation from messages" << q.lastError();

    emit conversationDeleted(convId);
}

QVector<Message> Storage::getMessages(const QString &convId)
{
    QVector<Message> res;
    QSqlQuery q(db);
    q.prepare("SELECT * FROM messages WHERE convId = (:id) ORDER BY timestamp ASC");
    q.bindValue(":id", convId);
    if (!q.exec()) {
        qCWarning(llamaStorage) << "getMessages select from message" << convId << q.lastError();
        return res;
    }
    while (q.next()) {
        Message m;
        m.id = q.value("id").toLongLong();
        m.convId = q.value("convId").toString();
        m.type = q.value("type").toString();
        m.timestamp = q.value("timestamp").toLongLong();
        m.role = q.value("role").toString();
        m.content = q.value("content").toString();
        m.timings = deserializeTimingsReport(q.value("timings").toString());
        m.extra = deserializeExtra(q.value("extra").toString());
        m.parent = q.value("parent").toLongLong();
        QJsonArray arr = QJsonDocument::fromJson(q.value("children").toString().toUtf8()).array();
        for (const QJsonValue &v : std::as_const(arr))
            m.children.append(v.toInteger());
        res.append(m);
    }
    return res;
}

void Storage::appendMsg(Message &msg, qint64 parentNodeId)
{
    QSqlQuery q(db);
    db.transaction();

    // insert new message - note: we don't specify the id column, let SQLite auto-generate it
    q.prepare("INSERT INTO messages "
              "(convId,type,timestamp,role,content,timings,extra,parent,children) "
              "VALUES (:conv,:type,:ts,:role,:content,:timings,:extra,:parent,:children)");
    q.bindValue(":conv", msg.convId);
    q.bindValue(":type", msg.type);
    q.bindValue(":ts", msg.timestamp);
    q.bindValue(":role", msg.role);
    q.bindValue(":content", msg.content);
    q.bindValue(":timings", serialize(msg.timings));
    q.bindValue(":extra", serialize(msg.extra));
    q.bindValue(":parent", parentNodeId);
    q.bindValue(":children", "[]");
    if (!q.exec())
        qCWarning(llamaStorage) << "appendMsg: Failed to insert messages" << parentNodeId
                                << q.lastError();

    // Get the auto-generated ID
    qint64 pendingId = msg.role == "assistant" ? msg.id : -1;
    msg.id = q.lastInsertId().toLongLong();

    // update parent children
    q.prepare("SELECT children FROM messages WHERE  id = (:pid)");
    q.bindValue(":pid", parentNodeId);
    if (!q.exec()) {
        qCWarning(llamaStorage) << "appendMsg: Failed to select children for parent node"
                                << parentNodeId << q.lastError();
        return;
    }
    if (!q.next()) {
        qCWarning(llamaStorage) << "appendMsg: no children found for" << parentNodeId;
        return;
    }
    QJsonArray arr = QJsonDocument::fromJson(q.value(0).toString().toUtf8()).array();
    arr.append(msg.id);
    q.prepare("UPDATE messages SET children = (:arr) WHERE id = (:pid)");
    q.bindValue(":arr", QJsonDocument(arr).toJson(QJsonDocument::Compact));
    q.bindValue(":pid", parentNodeId);
    if (!q.exec())
        qCWarning(llamaStorage) << "appendMsg: Failed update children messages" << q.lastError();

    // update conversation lastModified & currNode
    q.prepare(
        "UPDATE conversations SET lastModified = (:lm), currNode = (:node) WHERE id = (:conv)");
    q.bindValue(":lm", QDateTime::currentMSecsSinceEpoch());
    q.bindValue(":node", msg.id);
    q.bindValue(":conv", msg.convId);
    if (!q.exec())
        qCWarning(llamaStorage) << "appendMsg: Failed to update conversations" << q.lastError();

    db.commit();

    emit messageAppended(msg, pendingId);
}

QVector<Message> Storage::filterByLeafNodeId(const QVector<Message> &msgs,
                                             qint64 leafNodeId,
                                             bool includeRoot)
{
    QHash<qint64, Message> map;
    for (const Message &m : msgs)
        map.insert(m.id, m);

    QVector<Message> res;
    Message *curr = map.contains(leafNodeId) ? &map[leafNodeId] : nullptr;
    if (!curr) {
        // no exact match – pick the latest by timestamp
        qint64 latest = -1;
        for (auto it = map.constBegin(); it != map.constEnd(); ++it)
            if (it.value().timestamp > latest) {
                curr = const_cast<Message *>(&it.value());
                latest = it.value().timestamp;
            }
    }
    while (curr) {
        if (curr->type != "root" || (curr->type == "root" && includeRoot))
            res.append(*curr);
        curr = map.contains(curr->parent) ? &map[curr->parent] : nullptr;
    }
    std::sort(res.begin(), res.end(), [](const Message &a, const Message &b) {
        return a.timestamp < b.timestamp;
    });
    return res;
}

bool Storage::updateMessageExtra(const Message &msg, const QList<QVariantMap> &extra)
{
    // Serialize the list to the same JSON format we use everywhere else.
    const QString json = serialize(extra);

    QSqlQuery q(db);
    // We use an explicit transaction – if anything fails we roll back.
    if (!db.transaction()) {
        qCWarning(llamaStorage) << "updateMessageExtra: cannot start transaction" << db.lastError();
        return false;
    }

    q.prepare("UPDATE messages "
              "SET extra = :extra "
              "WHERE id = :id");
    q.bindValue(":extra", json);
    q.bindValue(":id", msg.id);

    if (!q.exec()) {
        qCWarning(llamaStorage) << "updateMessageExtra: UPDATE failed for id" << msg.id
                                << q.lastError();
        db.rollback();
        return false;
    }

    // Commit the transaction – this will also release any locks.
    if (!db.commit()) {
        qCWarning(llamaStorage) << "updateMessageExtra: commit failed" << db.lastError();
        db.rollback();
        return false;
    }

    // Let everybody know the extra field changed.
    emit messageExtraUpdated(msg, extra);
    return true;
}

} // namespace LlamaCpp
