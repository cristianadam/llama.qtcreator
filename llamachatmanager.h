#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QTimer>
#include <QVariantMap>

#include <memory>

#include "llamatypes.h"

namespace Core {
class IDocument;
class IEditor;
}

namespace LlamaCpp {

class Storage;
class Tool;

struct ToolCall
{
    QString name;      // e.g. "python"
    QString arguments; // the JSON string that is being streamed
    QString id;        // tool-call identifier
};

//! Tool calls aggregated while an assistant stream is in flight (per
//! conversation).  Slots are never removed mid-stream – removing one would
//! shift the indices the server keeps sending; the \a done set marks slots
//! whose arguments already parsed and were committed to the pending message.
struct StreamingToolCalls
{
    QVector<ToolCall> calls;
    QSet<int> done;
    int indexOffset = 0; // rebase for a new batch after interleaved text
    bool openBatch = false;
};

class ChatManager : public QObject
{
    Q_OBJECT
public:
    static ChatManager &instance(); // singleton

    void refreshServerProps();

    bool isGenerating(const QString &convId) const;
    ViewingChat getViewingChat(const QString &convId) const;
    //! Renders a single message as Markdown for the conversation export
    //! ("Save as Markdown" in the conversations view and the editor's save
    //! path).  Tool messages render their tool call and result as a
    //! collapsible <details> block.  Returns an empty string for messages
    //! that have nothing to show (e.g. a tool‑call‑only assistant wrapper,
    //! whose tool call is exported with the tool message instead).
    static QString messageToMarkdown(const Message &msg);

    //! Merges a batch of tool-call deltas (as streamed by llama-server) into
    //! \a state.  llama-server restarts the per-batch index at 0 after an
    //! interleaved text chunk, so a new batch is re-based onto the calls
    //! collected so far (\c finalizeToolCallBatch() marks the boundary, same
    //! scheme as the llama.cpp web UI); a delta without a usable index is
    //! appended at the end.  Calls whose arguments already form valid JSON
    //! are appended to \a committed (in call order) as single-entry
    //! "tool_calls" extras; a missing id gets the synthetic "tool_<index>"
    //! fallback so the tool result can always reference its call.
    static void mergeToolCallDeltas(StreamingToolCalls &state,
                                    const QJsonArray &deltas,
                                    QList<QVariantMap> *committed);
    //! Ends the open tool-call batch: the next batch's indices restart at 0.
    static void finalizeToolCallBatch(StreamingToolCalls &state);

    //! The tool results of one assistant batch in emission order: arranged
    //! according to \a callOrder (the assistant's tool_calls order); results
    //! whose call id is not in \a callOrder (should not happen) are appended
    //! in \a results' iteration order, so no tool result is ever lost.
    static QList<const Message *> orderedToolResults(
            const QHash<QString, const Message *> &results,
            const QStringList &callOrder);

    //! Persists a rendered diagram SVG (mermaid diagram, math) with the
    //! message that displayed it, in the message's extra field (a
    //! "diagram" entry).  A still-pending message (not committed to the
    //! database yet) gets the entry in memory, so it is stored with the
    //! commit; a committed message's stored extra is updated in place.
    void saveDiagramSvg(const LlamaCpp::Message &msg, const QVariantMap &entry);

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

    //! True when the "Human Editor" pseudo‑model is selected: the
    //! "assistant" side of the conversation is a human typing Markdown in
    //! a text editor split next to the chat, not a llama.cpp model (no
    //! server request is made).
    bool isHumanEditor() const;

    //! Commits the pending "Human Editor" assistant message with \a content
    //! (the split editor was saved).
    void commitHumanEditorMessage(const QString &convId, const QString &content);

    //! Discards the pending "Human Editor" assistant message (the split
    //! editor was closed without saving / the generation was stopped).
    void abortHumanEditorMessage(const QString &convId);

    //! In‑place edit of a stored message's content ("Human Editor" mode):
    //! updates the database and emits Storage::messageContentUpdated.
    void updateMessageContent(const QString &convId, qint64 msgId, const QString &content);

    // "Human Editor" documents: regular text editors opened in a split
    // next to the chat.  Saving the document commits its content as a chat
    // message, closing it without saving aborts a pending reply.

    //! Registers \a document (shown in \a editor) as a "Human Editor"
    //! session.  \a msgId > 0: in‑place edit of that stored message;
    //! otherwise a new pending reply for \a convId.  \a filePath is the
    //! throw‑away temp file backing the document, removed when the editor
    //! closes.
    void registerHumanEditorSession(Core::IDocument *document,
                                    Core::IEditor *editor,
                                    const QString &convId,
                                    qint64 msgId,
                                    const QString &filePath = {});
    bool isHumanEditorDocument(const Core::IDocument *document) const;
    //! The editor of a registered "Human Editor" session of \a convId (only
    //! pending replies when \a pendingOnly), or nullptr.
    Core::IEditor *humanEditorEditor(const QString &convId, bool pendingOnly = false) const;

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
                                  std::function<void(qint64)> onChunk,
                                  std::shared_ptr<int> batchRemaining = nullptr);

signals:
    void modelsUpdated();

    //! The "Human Editor" endpoint is ready for a reply: a pending
    //! assistant message exists for \a convId and the UI should open the
    //! split text editor (commit/abort it with
    //! commitHumanEditorMessage()/abortHumanEditorMessage()).
    void humanEditorReplyReady(const QString &convId);

    //! The pending "Human Editor" assistant message was discarded.
    void humanEditorAborted(const QString &convId);

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
    QHash<QString, StreamingToolCalls> m_streamingToolCalls;
    QHash<QString, std::shared_ptr<LlamaCpp::Tool>> m_streamingTools;

    //! Consecutive tool-call turns per conversation (runaway guard, see the
    //! finished handler in generateMessage()); reset by a user message,
    //! replaceMessageAndGenerate(), or a tool-free assistant turn.
    QHash<QString, int> m_consecutiveToolTurns;

    //! Number of tools currently executing (per conversation).  Tools run
    //! asynchronously, so a conversation stays "busy" until every in‑flight
    //! tool has reported back.
    QHash<QString, int> m_runningTools;

    //! The tool objects currently executing (per conversation), registered
    //! by executeToolAndSendResult().  stopGenerating() calls abort() on
    //! them when the user presses Escape while a tool is running.
    QHash<QString, QVector<std::shared_ptr<Tool>>> m_activeTools;

    //! Conversations where the user asked to stop while tools were running.
    //! When the last in‑flight tool of such a conversation reports back, the
    //! conversation ends there instead of the model getting another turn.
    QSet<QString> m_stopRequested;

    // Task‑conversation (sub‑agent) state
    struct TaskConversationConfig
    {
        QString systemPrompt;
        QStringList allowedTools; // empty = every enabled tool
    };
    QSet<QString> m_taskConversations;
    QHash<QString, TaskConversationConfig> m_taskConfigs;
    bool m_taskConvCreationPending{false};

    // "Human Editor" sessions, keyed by the editor's document.
    struct HumanEditorSession
    {
        QString convId;
        qint64 msgId{0}; // >0: in‑place edit of a stored message; 0: pending reply
        const Core::IDocument *document{nullptr};
        Core::IEditor *editor{nullptr};
        QString filePath; // throw‑away temp file, removed when the editor closes
        bool committed{false}; // content already saved to the chat
    };
    QHash<const Core::IDocument *, HumanEditorSession> m_humanEditorSessions;
};
} // namespace LlamaCpp
