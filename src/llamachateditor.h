#pragma once
#include <QPointer>
#include <QSet>
#include <QVector>

#include <coreplugin/editormanager/ieditor.h>
#include <texteditor/textdocument.h>

#include "autoscrollarea.h"
#include "llamasearchtoolbar.h"
#include "llamatypes.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace TextEditor {
class IDocument;
}

namespace LlamaCpp {

class ChatInput;
class ChatMessage;
class MarkdownLabel;
struct ToolCall;

class ChatEditor : public Core::IEditor
{
    Q_OBJECT
public:
    ChatEditor();
    ~ChatEditor();

    Core::IDocument *document() const override;
    QWidget *toolBar() override;

    bool isDesignModePreferred() const override;

    void refreshMessages(const QVector<Message> &messages,
                         qint64 leafNodeId,
                         bool pinToBottom = true);
    // Explicitly set message row heights to bypass Qt's heightForWidth caching
    // which causes long messages to be clamped at wrong heights.
    void fixMessageHeights();
    void scrollToBottom();

    // Font zoom for the conversation (see LlamaSettings::chatFontZoom).
    // These act on every visible chat editor so all open conversations stay
    // in sync; \a direction is +1 (zoom in) or -1 (zoom out).
    static void zoomFont(int direction);
    static void resetFontZoom();
    QWidget *displayServerProps();
    void createFollowUpWidget(const QString &convId,
                              qint64 leafNodeId,
                              const QStringList &questions);

    struct SearchResult
    {
        ChatMessage *widget{nullptr}; // which widget contains the hit
        int start{0};                 // offset inside the widget's plain text
        int length{0};                // length of the match
    };

public slots:
    void onMessageAppended(const LlamaCpp::Message &msg, qint64 pendingId);
    void onPendingMessageChanged(const LlamaCpp::Message &pm);
    void onSendRequested(const QString &text, const QList<QVariantMap> &extra);
    // \a fromKey is true when the stop was triggered by the Escape key
    // rather than the stop button; stopping a running task (sub‑agent)
    // conversation then asks for confirmation first.
    void onStopRequested(bool fromKey);
    void onFileDropped(const QStringList &files);
    void onEditRequested(const LlamaCpp::Message &msg);
    void onEditingCancelled();
    void onRegenerateRequested(const LlamaCpp::Message &msg);
    void onSiblingChanged(qint64 siblingId);
    void onServerPropsUpdated();
    void onModelsUpdated();
    void onDeleteMessageRequested(const LlamaCpp::Message &msg);

    void startSearch();
    void nextSearchResult();
    void prevSearchResult();
    void clearSearch();
    void onMessageExtraUpdated(const LlamaCpp::Message &msg, const QList<QVariantMap> &newExtra);
    void onHumanEditorReplyReady(const QString &convId);
    void onHumanEditorAborted(const QString &convId);
    void onMessageContentUpdated(const LlamaCpp::Message &msg);
    //! Shows/hides the transient "creating a continuation" note in the
    //! conversation being viewed (see ChatManager::handoffProgress()).
    void onHandoffProgress(const QString &convId, bool active);

private:
    void updateSpeedLabel(const Message &msg);
    void updateContextLabel(const Message &msg);
    //! (Re)adds the transient "creating a continuation" note at the end of
    //! the message list while a handoff summary runs for the conversation
    //! being viewed; the note is re-inserted on every view refresh, since
    //! the message widgets are rebuilt after it.
    void updateHandoffLabel();
    //! Shows/hides the "queued messages" popup button (mid‑run steering)
    //! and rebuilds its menu (one removable entry per queued message).
    void updateSteeringButton();
    // Pre‑wrap a freshly created message's document at its final label width
    // so its first size hint is already correct (call before adding the
    // widget to m_messageWidgets / the layout).
    void preWrapDocument(ChatMessage *w);
    // Re-renders this editor's conversation at the current font zoom, keeping
    // the scroll position (called by zoomFont()/resetFontZoom(), debounced).
    void applyFontZoom();
    void syncThinkingLevel();
    void updateModelCombo();
    // Toggles the in‑place edit button on all assistant message widgets
    // after the "Human Editor" endpoint was (de)selected.
    void applyHumanEditorMode();
    // Opens a "Human Editor" document as a regular text editor in a new
    // split next to the chat.  \a msgId > 0: in‑place edit of that stored
    // message; otherwise a new pending reply for \a convId.
    void openHumanEditorDocument(const QString &convId, qint64 msgId, const QString &text);
    void performSearch(const QString &query);
    void jumpToResult(int idx, bool selected = true);
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    TextEditor::TextDocumentPtr m_document;
    AutoScrollArea *m_scrollArea{nullptr};
    QWidget *m_messageContainer{nullptr};
    ChatInput *m_input{nullptr};
    QVBoxLayout *m_messageLayout{nullptr};
    QVector<ChatMessage *> m_messageWidgets; // keep for cleanup
    std::optional<Message> m_editedMessage;
    QWidget *m_propsWidget{nullptr};
    QLabel *m_workingDirLabel{nullptr}; // "Working directory" label in m_propsWidget
    QWidget *m_followUpWidget{nullptr};
    QPointer<MarkdownLabel> m_handoffLabel; // transient "creating continuation" note
    QPointer<QWidget> m_statusBar; // returned by toolBar(); owned by this editor
    QLabel *m_contextLabel{nullptr};
    QLabel *m_speedLabel{nullptr};
    QToolButton *m_thinkingButton{nullptr};
    QMenu *m_thinkingMenu{nullptr};
    QToolButton *m_steeringButton{nullptr};
    QMenu *m_steeringMenu{nullptr};
    QToolButton *m_followUpButton{nullptr};
    QToolButton *m_titleButton{nullptr};
    QComboBox *m_modelCombo{nullptr};

    QVector<SearchResult> m_searchResults; // all matches of the current query
    int m_currentResult{0};                // index into m_searchResults
    QString m_searchQuery;                 // the string that is active
    bool m_searchActive{false};

    SearchToolbar *m_searchToolbar{nullptr};
    QString m_viewingConvId;

    // All live chat editors; used so a font-zoom change can re-render every
    // visible conversation and so the zoom commands can tell whether the
    // active editor is a chat.
    static QSet<ChatEditor *> s_instances;
    // Coalesces rapid Ctrl+mouse-wheel notches into a single re-render.
    QTimer *m_zoomDebounceTimer{nullptr};
};

void setupChatEditor();
} // namespace LlamaCpp
