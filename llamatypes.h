#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVariant>
#include <QVector>

namespace LlamaCpp {

struct TimingReport
{
    int prompt_n{0};
    int prompt_ms{0};
    int predicted_n{0};
    int predicted_ms{0};
};

/**
 * What is conversation "branching"? It is a feature that allows the user to edit an old message
 * in the history, while still keeping the conversation flow.
 * Inspired by ChatGPT / Claude / Hugging Chat where you edit a message, a new branch of the
 * conversation is created, and the old message is still visible.
 *
 * We use the same node-based structure like other chat UIs, where each message has a parent
 * and children. A "root" message is the first message in a conversation, which will not be
 * displayed in the UI.
 *
 * root
 *  ├── message 1
 *  │      └── message 2
 *  │             └── message 3
 *  └── message 4
 *        └── message 5
 *
 * In the above example, assuming that user wants to edit message 2, a new branch will be created:
 *
 *          ├── message 2
 *          │   └── message 3
 *          └── message 6
 *
 * Message 2 and 6 are siblings, and message 6 is the new branch.
 *
 * We only need to know the last node (aka leaf) to get the current branch. In the above example,
 * message 5 is the leaf of branch containing message 4 and 5.
 *
 */

struct Message
{
    qint64 id; // 64‑bit is plenty
    QString convId;
    QString type;     // "text" | "root"
    qint64 timestamp; // from QDateTime::currentMSecsSinceEpoch()
    QString role;     // "user" | "assistant" | "system"
    QString content;
    TimingReport timings;
    QList<QVariantMap> extra; // array of MessageExtra

    // Node relations – stored in the DB, not serialised directly
    qint64 parent;
    QList<qint64> children;
};

struct MessageExtraTextFile
{
    QString type = "textFile";
    QString name;
    QString content;
};

struct MessageExtraImageFile
{
    QString type = "imageFile";
    QString name;
    QString base64Url;
};

struct MessageExtraAudioFile
{
    QString type = "audioFile";
    QString name;
    QString base64Data;
    QString mimeType;
};

struct MessageExtraContext
{
    QString type = "context";
    QString name;
    QString content;
};

using APIMessageContentPart = QJsonObject; // will contain type, text, image_url, etc.

struct APIMessage
{
    QString role;
    QVariant content; // QString or QList<APIMessageContentPart>
};

struct Conversation
{
    QString id; // e.g. “conv-1234567890”
    qint64 lastModified{-1};
    qint64 currNode{-1}; // id of the node currently shown
    QString name;
};

struct ViewingChat
{
    Conversation conv;
    QVector<Message> messages;
};

struct LlamaCppServerProps
{
    QString build_info;
    QString model_path;
    int n_ctx = 0;

    struct Modalities
    {
        bool vision = false;
        bool audio = false;
    } modalities;
};

} // namespace LLamaCpp
