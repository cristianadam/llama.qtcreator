#pragma once
#include <QPoint>
#include <QPointer>
#include <QSet>
#include <QVector>

#include <coreplugin/editormanager/ieditor.h>
#include <texteditor/textdocument.h>

#include "autoscrollarea.h"
#include "llamatypes.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QActionGroup;
class QMenu;
class QPushButton;
class QTimer;
class QToolButton;
class QVBoxLayout;

namespace TextEditor {
class IDocument;
}

namespace Core {
class FindToolBarPlaceHolder;
}

namespace LlamaCpp {

class ChatEditorFindSupport;
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
    // in sync; \a direction is +1 (zoom in) or -1 (zoom out).  A fading
    // "Zoom: NN%" indicator (the same one the text editor shows) is displayed
    // on \a indicatorWidget, or on the focused chat editor when it is null
    // (the keyboard commands).
    static void zoomFont(int direction, QWidget *indicatorWidget = nullptr);
    static void resetFontZoom(QWidget *indicatorWidget = nullptr);
    QWidget *displayServerProps();
    void createFollowUpWidget(const QString &convId,
                              qint64 leafNodeId,
                              const QStringList &questions);

    // Accessors used by ChatEditorFindSupport (the Core::IFindSupport that
    // backs the in‑chat "Find" bar, see llamachateditor.cpp).
    const QVector<ChatMessage *> &messageWidgets() const { return m_messageWidgets; }
    AutoScrollArea *scrollArea() const { return m_scrollArea; }

public slots:
    void onMessageAppended(const LlamaCpp::Message &msg, qint64 pendingId);
    void onPendingMessageChanged(const LlamaCpp::Message &pm);
    void onSendRequested(const QString &text, const QList<QVariantMap> &extra);
    // \a fromKey is true when the stop was triggered by the Escape key
    // rather than the stop button; stopping a running task (sub‑agent)
    // conversation then asks for confirmation first.
    void onStopRequested(bool fromKey);
    void onEditRequested(const LlamaCpp::Message &msg);
    void onEditingCancelled();
    void onRegenerateRequested(const LlamaCpp::Message &msg);
    void onSiblingChanged(qint64 siblingId);
    void onServerPropsUpdated();
    void onModelsUpdated();
    void onDeleteMessageRequested(const LlamaCpp::Message &msg);

    void onMessageExtraUpdated(const LlamaCpp::Message &msg, const QList<QVariantMap> &newExtra);
    void onHumanEditorReplyReady(const QString &convId);
    void onHumanEditorAborted(const QString &convId);
    void onMessageContentUpdated(const LlamaCpp::Message &msg);

private:
    void updateSpeedLabel(const Message &msg);
    void updateContextLabel(const Message &msg);
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
    // Shows the fading "Zoom: NN%" indicator on \a widget (or the focused
    // chat editor when \a widget is null), like the text editor and the
    // markdown browser do.
    static void showZoomIndicator(QWidget *widget, int zoom);
    void syncThinkingLevel();
    // Rebuilds the thinking-level menu from the levels the server reports for
    // the active model's chat template (chat_template_caps.
    // reasoning_effort_levels) and enables the button only when the server
    // reports at least one.  Called from onServerPropsUpdated() and once
    // from the constructor.
    void updateThinkingMenu();
    void updateModelCombo();
    // Rebuilds the "You are connected to <model> in the <directory>" line of
    // the welcome screen (the model name and the directory are links; see
    // displayServerProps()).  Called from displayServerProps() and whenever
    // the startup project or the working-directory override changes.
    void updateConnectionLabel();
    // Removes the welcome screen (m_propsWidget) if it is showing and clears
    // the member references (the label is a child of the widget, and
    // deleteLater() is asynchronous).  Restores the message layout's top
    // alignment.  Call before adding a message.
    void removePropsWidget();
    // Creates the welcome screen and shows it centred in the message area
    // (the layout is aligned to the centre while it is the only item).
    void showWelcomeScreen();
    // The display name of the active model (router id, the loaded model's
    // file name in single-model mode, or "Human Editor").
    QString selectedModelDisplayName() const;
    // Shows the model details dialog (the llama.cpp web‑UI model info: path,
    // type, context, modalities, tokens, build, and the Jinja chat template
    // with syntax highlighting).
    void showModelDetails();
    // Opens a folder picker to change the working directory the tools run in.
    void changeWorkingDirectory();
    // The context menu of the working directory link in the welcome screen
    // (a regular click opens the folder picker): reset the directory to the
    // (startup) project's, or to the default projects directory (the setting
    // new projects are created in).
    void showWorkingDirectoryResetMenu(const QPoint &globalPos);
    // Toggles the in‑place edit button on all assistant message widgets
    // after the "Human Editor" endpoint was (de)selected.
    void applyHumanEditorMode();
    // Opens a "Human Editor" document as a regular text editor in a new
    // split next to the chat.  \a msgId > 0: in‑place edit of that stored
    // message; otherwise a new pending reply for \a convId.
    void openHumanEditorDocument(const QString &convId, qint64 msgId, const QString &text);
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
    QLabel *m_connectionLabel{nullptr}; // "You are connected to …" line in m_propsWidget
    bool m_workdirLinkHovered{false}; // the welcome screen's dir link is under the cursor
    QWidget *m_followUpWidget{nullptr};
    QPointer<QWidget> m_statusBar; // returned by toolBar(); owned by this editor
    QLabel *m_contextLabel{nullptr};
    QLabel *m_speedLabel{nullptr};
    QToolButton *m_thinkingButton{nullptr};
    QMenu *m_thinkingMenu{nullptr};
    QActionGroup *m_thinkingActions{nullptr};
    QToolButton *m_steeringButton{nullptr};
    QMenu *m_steeringMenu{nullptr};
    QToolButton *m_followUpButton{nullptr};
    QToolButton *m_titleButton{nullptr};
    QComboBox *m_modelCombo{nullptr};

    // Core::IFindSupport backing the standard in‑chat "Find" bar.  It is
    // aggregated with the editor widget so the FindToolBar of the current
    // document can locate it (and the FindToolBarPlaceHolder it docks into).
    ChatEditorFindSupport *m_findSupport{nullptr};
    // The placeholder the Find bar docks into; its visibility tells whether
    // the bar is currently shown in this editor (Escape closes it, see
    // eventFilter()).
    Core::FindToolBarPlaceHolder *m_findPlaceHolder{nullptr};
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
