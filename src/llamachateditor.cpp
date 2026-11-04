#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/documentmodel.h>
#include <coreplugin/editormanager/ieditorfactory.h>
#include <coreplugin/find/findplugin.h>
#include <coreplugin/find/ifindsupport.h>
#include <coreplugin/findplaceholder.h>
#include <coreplugin/icore.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>

#include <utils/action.h>
#include <utils/aggregate.h>
#include <utils/fadingindicator.h>
#include <utils/fsengine/fileiconprovider.h>
#include <utils/icon.h>
#include <utils/theme/theme.h>
#include <utils/utilsicons.h>
#include <utils/widgets.h>

#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>

#include <QApplication>
#include <QCoreApplication>
#include <QAction>
#include <limits>
#include <QDir>
#include <QDialog>
#include <QFileDialog>
#include <QFile>
#include <QFormLayout>
#include <QFrame>
#include <QHash>
#include <QActionGroup>
#include <QHBoxLayout>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QKeySequence>
#include <QKeyEvent>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QRect>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextEdit>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

#include "llamachateditor.h"
#include "llamachatinput.h"
#include "llamachatmanager.h"
#include "llamachatmessage.h"
#include "llamamarkdownwidget.h"
#include "llamastorage.h"
#include "llamaconstants.h"
#include "llamaicons.h"
#include "llamasettings.h"
#include "llamatheme.h"
#include "llamatr.h"

#include "tools/tool_utils.h"

using namespace TextEditor;
using namespace Core;
using namespace Utils;

namespace LlamaCpp {

namespace {

// The generation speed (predicted_n / predicted_ms) arrives live with every
// SSE chunk and predicted_ms counts from the end of prompt processing: in the
// first chunks it covers only the (fastest) first tokens, so the ratio briefly
// shows hundreds or thousands of t/s before settling. Wait for a small sample
// before displaying it.
constexpr int kMinTokensForSpeed = 10;

// "Human Editor" documents are backed by a throw‑away file in the temp dir
// so the regular text editor treats them as ordinary, saveable files (no
// "Save As" dialog, which a file‑less document would trigger).  The file is
// removed when the editor closes; the real content lives in the chat
// (committed on save, see ChatManager).
QString humanEditorFilePath(const QString &uniqueId)
{
    static QString dirPath;
    if (dirPath.isEmpty()) {
        dirPath = QDir::tempPath() + QStringLiteral("/llama-cpp-chat");
        QDir().mkpath(dirPath);
    }
    const QString path = dirPath + QStringLiteral("/") + uniqueId + QStringLiteral(".md");
    // Create the (empty) file so the document sees a writable path; a
    // non‑existent file is treated as read‑only.  The real content is set via
    // setContents(); this file is only a throw‑away backing store.
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.close();
    return path;
}

// Escapes the HTML metacharacters so a value (a model id or a directory that
// may contain '&', '<' or '>') can be embedded in the welcome screen's
// rich‑text label without being interpreted as markup.
QString escapeHtml(const QString &text)
{
    QString result = text;
    result.replace(QLatin1String("&"), QLatin1String("&amp;"));
    result.replace(QLatin1String("<"), QLatin1String("&lt;"));
    result.replace(QLatin1String(">"), QLatin1String("&gt;"));
    result.replace(QLatin1Char('"'), QLatin1String("&quot;"));
    return result;
}

// The thinking levels and their display names, shared by the status bar
// dropdown menu and the button label.
const QList<QPair<QString, QString>> &thinkingLevels()
{
    static const QList<QPair<QString, QString>> levels{
        {QStringLiteral("default"), Tr::tr("Default")},
        {QStringLiteral("off"), Tr::tr("Off")},
        {QStringLiteral("low"), Tr::tr("Low")},
        {QStringLiteral("medium"), Tr::tr("Medium")},
        {QStringLiteral("high"), Tr::tr("High")},
        {QStringLiteral("max"), Tr::tr("Max")},
    };
    return levels;
}

QString thinkingLevelDisplay(const QString &level)
{
    for (const auto &entry : thinkingLevels())
        if (entry.first == level)
            return entry.second;
    return thinkingLevels().front().second;
}

} // namespace

// ---------------------------------------------------------------------------
// ChatEditorFindSupport
//
// The Core::IFindSupport that backs the standard in‑chat "Find" bar.  It
// searches across every ChatMessage's markdown label (a QTextBrowser), the
// same way ACP's MessageViewFindSupport searches across its MarkdownBrowser
// widgets.  It is aggregated with the editor widget (see the ChatEditor
// constructor) so that, when focus is anywhere in the editor, the
// CurrentDocumentFind of the Find plugin can locate it and the FindToolBar
// docks into the FindToolBarPlaceHolder instead of floating.
// ---------------------------------------------------------------------------

class ChatEditorFindSupport : public Core::IFindSupport
{
public:
    explicit ChatEditorFindSupport(ChatEditor *editor)
        : m_editor(editor)
    {}

    bool supportsReplace() const override { return false; }

    Utils::FindFlags supportedFindFlags() const override
    {
        return Utils::FindCaseSensitively | Utils::FindWholeWords
               | Utils::FindRegularExpression;
    }

    void resetIncrementalSearch() override { m_incrementalStart = {nullptr, {}}; }

    void highlightAll(const QString &txt, Utils::FindFlags findFlags) override
    {
        if (txt.isEmpty()) {
            clearHighlights();
            return;
        }
        m_active = true;
        m_lastFindText = txt;

        const QTextDocument::FindFlags docFlags = toDocFlags(findFlags);
        QTextCharFormat format;
        format.setBackground(Utils::creatorColor(
            Utils::Theme::OutputPanes_SearchResultBackgroundColor));
        for (ChatMessage *msg : messages()) {
            auto *label = msg->markdownLabel();
            if (!label)
                continue;
            QList<QTextEdit::ExtraSelection> selections;
            QTextCursor found = findInLabel(label, txt, 0, findFlags, docFlags);
            while (!found.isNull()) {
                selections.append({found, format});
                int pos = found.selectionEnd();
                if (found.position() == found.anchor())
                    ++pos; // avoid an infinite loop on a zero‑length match
                found = findInLabel(label, txt, pos, findFlags, docFlags);
            }
            label->setExtraSelections(selections);
        }
    }

    void clearHighlights() override
    {
        m_active = false;
        for (ChatMessage *msg : messages())
            if (auto *label = msg->markdownLabel())
                label->setExtraSelections({});
    }

    QString currentFindString() const override { return m_lastFindText; }
    QString completedFindString() const override { return {}; }

    Result findIncremental(const QString &txt, Utils::FindFlags findFlags) override
    {
        if (txt.isEmpty()) {
            clearHighlights();
            return NotFound;
        }

        // The first keystroke of a fresh search starts from the top of the
        // message that is currently visible, so the first hit is not scrolled
        // far away from where the user is reading.
        if (m_incrementalStart.first == nullptr) {
            if (ChatMessage *msg = topmostVisibleMessage()) {
                auto *label = msg->markdownLabel();
                if (label) {
                    QWidget *viewport = m_editor->scrollArea()->viewport();
                    const int labelTopInViewport = label->mapTo(viewport, QPoint(0, 0)).y();
                    const QPoint visibleTopInLabel(0, qMax(0, -labelTopInViewport));
                    m_incrementalStart = {msg, label->cursorForPosition(visibleTopInLabel)};
                }
            }
        }

        const Result result = findText(txt, findFlags, true);
        highlightAll(result == Found ? txt : QString(), findFlags);
        return result;
    }

    Result findStep(const QString &txt, Utils::FindFlags findFlags) override
    {
        if (txt.isEmpty())
            return NotFound;
        m_lastFindText = txt;
        return findText(txt, findFlags, false);
    }

    //! Re‑applies the current highlights.  The message widgets are rebuilt on
    //! every refresh (new message, font zoom, …), which drops the extra
    //! selections; call this after a rebuild to bring them back while a find
    //! is active.
    void reapplyHighlights()
    {
        if (m_active && !m_lastFindText.isEmpty())
            highlightAll(m_lastFindText, Core::Find::findFlags());
    }

private:
    QList<ChatMessage *> messages() const
    {
        return m_editor->messageWidgets().toList();
    }

    ChatMessage *topmostVisibleMessage()
    {
        QWidget *viewport = m_editor->scrollArea()->viewport();
        const QRect viewportRect = viewport->rect();
        ChatMessage *topmost = nullptr;
        int topmostY = std::numeric_limits<int>::max();
        for (ChatMessage *msg : messages()) {
            auto *label = msg->markdownLabel();
            if (!label)
                continue;
            const QRect labelRect(label->mapTo(viewport, QPoint(0, 0)), label->size());
            if (labelRect.intersects(viewportRect) && labelRect.top() < topmostY) {
                topmostY = labelRect.top();
                topmost = msg;
            }
        }
        return topmost;
    }

    static QRegularExpression searchRegex(const QString &txt, Utils::FindFlags findFlags)
    {
        return QRegularExpression(
            (findFlags & Utils::FindRegularExpression) ? txt : QRegularExpression::escape(txt),
            (findFlags & Utils::FindCaseSensitively) ? QRegularExpression::NoPatternOption
                                                     : QRegularExpression::CaseInsensitiveOption);
    }

    static QTextDocument::FindFlags toDocFlags(Utils::FindFlags findFlags)
    {
        QTextDocument::FindFlags flags;
        if (findFlags & Utils::FindWholeWords)
            flags |= QTextDocument::FindWholeWords;
        return flags;
    }

    QTextCursor findInLabel(const MarkdownLabel *label,
                            const QString &txt,
                            int startPos,
                            Utils::FindFlags findFlags,
                            QTextDocument::FindFlags docFlags)
    {
        if (startPos < 0)
            startPos = findFlags & Utils::FindBackward ? label->document()->characterCount() - 1
                                                       : 0;
        return label->document()->find(searchRegex(txt, findFlags), startPos, docFlags);
    }

    void scrollToMatch(const MarkdownLabel *label, const QTextCursor &cursor)
    {
        QWidget *outerViewport = m_editor->scrollArea()->viewport();
        const QRect selRect = label->cursorRect(cursor);
        const QPoint topLeftInOuter =
            label->viewport()->mapTo(outerViewport, selRect.topLeft());
        int targetY = topLeftInOuter.y() + selRect.height() / 2
                     - outerViewport->height() / 2; // centre vertically
        QScrollBar *vbar = m_editor->scrollArea()->verticalScrollBar();
        vbar->setValue(qBound(0, targetY, vbar->maximum()));
    }

    Result findText(const QString &txt, Utils::FindFlags findFlags, bool incremental)
    {
        const QList<ChatMessage *> allMsgs = messages();
        const int count = allMsgs.size();
        if (count == 0)
            return NotFound;

        const bool backward = findFlags & Utils::FindBackward;
        QTextDocument::FindFlags docFlags = toDocFlags(findFlags);
        if (backward)
            docFlags |= QTextDocument::FindBackward;

        int startIdx = -1;

        auto findIn = [&](int msgIdx, int startPos = -1) -> bool {
            ChatMessage *msg = allMsgs[msgIdx];
            auto *label = msg->markdownLabel();
            if (!label)
                return false;
            const QTextCursor found = findInLabel(label, txt, startPos, findFlags, docFlags);
            if (found.isNull())
                return false;
            m_currentResult = {msg, found};
            label->setTextCursor(found);
            scrollToMatch(label, found);
            return true;
        };

        if (incremental && m_incrementalStart.first != nullptr) {
            if (startIdx = allMsgs.indexOf(m_incrementalStart.first); startIdx >= 0) {
                int startPos = backward ? m_incrementalStart.second.selectionEnd()
                                        : m_incrementalStart.second.selectionStart();
                if (findIn(startIdx, startPos))
                    return Found;
                startIdx = (backward ? startIdx - 1 + count : startIdx + 1) % count;
            }
        } else if (!incremental && m_currentResult.first != nullptr) {
            if (startIdx = allMsgs.indexOf(m_currentResult.first); startIdx >= 0) {
                int startPos = backward ? m_currentResult.second.selectionStart()
                                        : m_currentResult.second.selectionEnd();
                if (findIn(startIdx, startPos))
                    return Found;
                startIdx = (backward ? startIdx - 1 + count : startIdx + 1) % count;
            }
        }

        if (startIdx < 0)
            startIdx = backward ? count - 1 : 0;

        // Search from the current position, then wrap around.
        for (int i = 0; i < count; ++i) {
            const int idx = backward ? (startIdx - i + count) % count : (startIdx + i) % count;
            if (findIn(idx))
                return Found;
        }

        m_currentResult = {nullptr, QTextCursor()};
        return NotFound;
    }

    ChatEditor *m_editor;
    QPair<ChatMessage *, QTextCursor> m_currentResult;
    QPair<ChatMessage *, QTextCursor> m_incrementalStart;
    QString m_lastFindText;
    bool m_active{false};
};

QSet<ChatEditor *> ChatEditor::s_instances;

ChatEditor::ChatEditor()
    : m_document(new TextDocument())
{
    s_instances.insert(this);

    // Coalesces rapid font-zoom changes (Ctrl+mouse-wheel notches) into a
    // single re-render; the timer is single-shot and (re)starts on every
    // zoom step, so the conversation only re-renders once the user pauses.
    m_zoomDebounceTimer = new QTimer(this);
    m_zoomDebounceTimer->setSingleShot(true);
    m_zoomDebounceTimer->setInterval(40);
    connect(m_zoomDebounceTimer, &QTimer::timeout, this, &ChatEditor::applyFontZoom);
    setContext(Context(Constants::LLAMACPP_VIEWER_ID));
    setDuplicateSupported(false);

    m_document->setId(Constants::LLAMACPP_VIEWER_ID);
    m_document->setMimeType(Constants::LLAMACPP_CHAT_MIME_TYPE);

    auto widget = new QWidget;

    m_scrollArea = new AutoScrollArea(widget);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    m_messageContainer = new QWidget(m_scrollArea);
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(0, 0, 0, 0);
    m_messageLayout->setSpacing(0);           // no gap between message rows
    m_messageLayout->setAlignment(Qt::AlignTop);
    m_scrollArea->setWidget(m_messageContainer);

    m_input = new ChatInput(widget);
    m_input->setMaximumHeight(80);

    auto layout = new QVBoxLayout;
    layout->setContentsMargins(10, 10, 10, 10);
    layout->addWidget(m_scrollArea);
    layout->addWidget(m_input, 0, Qt::AlignBottom);

    widget->setLayout(layout);
    setWidget(widget);
    // Catch Esc from any widget in the editor (the ChatInput handles it
    // itself when one of its children has focus).
    widget->installEventFilter(this);
    // Catch Ctrl+mouse-wheel over the conversation to zoom the font.  The
    // QScrollArea installs its own viewport event filter (for scrolling) at
    // construction, which runs before any filter we add later and would
    // consume a Ctrl+wheel on a scrollable view.  So we also filter the
    // message container: the message labels are read‑only text with no
    // internal scroll, so a plain wheel event propagates up to the container
    // (and is seen there) before it ever reaches the viewport.  The viewport
    // filter covers the non‑scrollable case, where the scroll filter does not
    // consume the event.  A Ctrl+wheel is consumed here so the view does not
    // scroll instead.
    m_scrollArea->viewport()->installEventFilter(this);
    m_messageContainer->installEventFilter(this);

    // The "You are connected to ..." line in the empty‑conversation welcome
    // screen is created once, so keep it in sync when the startup project
    // changes (the welcome widget itself is created once and not rebuilt).
    connect(ProjectExplorer::ProjectManager::instance(),
            &ProjectExplorer::ProjectManager::startupProjectChanged, this,
            [this](ProjectExplorer::Project *) { updateConnectionLabel(); });

    m_statusBar = new Utils::StyledBar;
    auto statusLayout = new QHBoxLayout(m_statusBar);
    statusLayout->setContentsMargins(0, 0, 0, 0);
    // The labels size tightly to their text, so the gap between them must
    // come from the layout spacing rather than per-widget margins.
    statusLayout->setSpacing(
        widget->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing));

    // Model selector: lists the models served by the llama.cpp server
    // (router mode) or the single loaded model as a one-entry combo.
    m_modelCombo = new QComboBox(m_statusBar);
    m_modelCombo->setToolTip(Tr::tr("LLM model"));
    statusLayout->addWidget(m_modelCombo);
    statusLayout->addStretch();

    m_speedLabel = new QLabel(m_statusBar);
    m_speedLabel->setVisible(false);
    m_speedLabel->setTextFormat(Qt::PlainText);
    statusLayout->addWidget(m_speedLabel);

    m_contextLabel = new QLabel(m_statusBar);
    m_contextLabel->setVisible(false);
    m_contextLabel->setTextFormat(Qt::PlainText);
    statusLayout->addWidget(m_contextLabel);

    // Mid‑run steering: user messages sent while a reply is being
    // generated are queued; the popup lists them and each entry can be
    // removed before it is handed to the model.
    m_steeringButton = new QToolButton(m_statusBar);
    m_steeringButton->setPopupMode(QToolButton::InstantPopup);
    m_steeringButton->setToolTip(Tr::tr("Queued messages - sent after the current step"));
    m_steeringButton->setVisible(false);
    m_steeringMenu = new QMenu(m_steeringButton);
    m_steeringButton->setMenu(m_steeringMenu);
    statusLayout->addWidget(m_steeringButton);

    // "Follow up" toggle: when checked, follow-up question suggestions are
    // generated after each complete assistant reply.
    m_followUpButton = new QToolButton(m_statusBar);
    m_followUpButton->setCheckable(true);
    m_followUpButton->setText(Tr::tr("Follow up"));
    m_followUpButton->setToolTip(Tr::tr("Generate follow-up questions after each reply"));
    m_followUpButton->setChecked(settings().followUpEnabled.value());
    connect(m_followUpButton, &QToolButton::toggled, this, [this](bool checked) {
        settings().followUpEnabled.setValue(checked);
        settings().writeSettings();
    });
    // Keep the button state in sync when the setting changes elsewhere.
    settings().followUpEnabled.addOnChanged(this, [this] {
        QSignalBlocker blocker(m_followUpButton);
        m_followUpButton->setChecked(settings().followUpEnabled.value());
    });
    statusLayout->addWidget(m_followUpButton);

    // "Title" toggle: when checked, a short conversation title is generated
    // by the model after the first assistant reply.
    m_titleButton = new QToolButton(m_statusBar);
    m_titleButton->setCheckable(true);
    m_titleButton->setText(Tr::tr("Title"));
    m_titleButton->setToolTip(Tr::tr("Generate a conversation title after the first reply"));
    m_titleButton->setChecked(settings().autoTitle.value());
    connect(m_titleButton, &QToolButton::toggled, this, [this](bool checked) {
        settings().autoTitle.setValue(checked);
        settings().writeSettings();
    });
    // Keep the button state in sync when the setting changes elsewhere.
    settings().autoTitle.addOnChanged(this, [this] {
        QSignalBlocker blocker(m_titleButton);
        m_titleButton->setChecked(settings().autoTitle.value());
    });
    statusLayout->addWidget(m_titleButton);

    // Thinking-level dropdown (only shown when the model's chat template
    // supports thinking/reasoning control, see onServerPropsUpdated()).
    m_thinkingButton = new QToolButton(m_statusBar);
    m_thinkingButton->setPopupMode(QToolButton::InstantPopup);
    m_thinkingButton->setToolTip(Tr::tr("Thinking level"));
    m_thinkingButton->setVisible(false);
    auto *menu = new QMenu(m_thinkingButton);
    m_thinkingMenu = menu;
    auto *actionGroup = new QActionGroup(m_thinkingButton);
    actionGroup->setExclusive(true);
    for (const auto &[key, display] : thinkingLevels()) {
        auto *action = menu->addAction(display);
        action->setCheckable(true);
        action->setData(key);
        action->setChecked(settings().thinkingLevel.value() == key);
        actionGroup->addAction(action);
        connect(action,
                &QAction::triggered,
                this,
                [this, level = key]() {
                    settings().thinkingLevel.setValue(level);
                    settings().writeSettings();
                });
    }
    m_thinkingButton->setMenu(menu);
    syncThinkingLevel();
    // Keep the menu check and the button label in sync when the level
    // changes elsewhere (another chat editor, the settings, …).
    settings().thinkingLevel.addOnChanged(this, [this] { syncThinkingLevel(); });
    statusLayout->addWidget(m_thinkingButton);

    updateModelCombo();

    // In‑chat search uses the standard Qt Creator "Find" bar instead of a
    // floating tool window.  The FindToolBar of the Find plugin locates the
    // editor through the focused widget: it walks up the parent chain looking
    // for a Core::IFindSupport (found by aggregating it with the editor
    // widget) and for a FindToolBarPlaceHolder owned by an ancestor, which it
    // then docks into.  The owner is the top‑level editor widget so the bar
    // works no matter whether focus is in the conversation or in the input.
    m_findSupport = new ChatEditorFindSupport(this);
    Utils::Aggregation::aggregate({widget, m_findSupport});
    m_findPlaceHolder = new Core::FindToolBarPlaceHolder(widget);
    layout->insertWidget(1, m_findPlaceHolder);

    ChatManager &chatManager = ChatManager::instance();
    connect(m_modelCombo, qOverload<int>(&QComboBox::activated), this, [this](int idx) {
        ChatManager::instance().selectModel(m_modelCombo->itemData(idx).toString());
        applyHumanEditorMode();
    });
    connect(&chatManager, &ChatManager::modelsUpdated, this, &ChatEditor::onModelsUpdated);
    connect(&chatManager,
            &ChatManager::humanEditorReplyReady,
            this,
            &ChatEditor::onHumanEditorReplyReady);
    connect(&chatManager,
            &ChatManager::humanEditorAborted,
            this,
            &ChatEditor::onHumanEditorAborted);
    connect(&Storage::instance(),
            &Storage::messageContentUpdated,
            this,
            &ChatEditor::onMessageContentUpdated);
    connect(&chatManager, &ChatManager::messageAppended, this, &ChatEditor::onMessageAppended);
    connect(&chatManager,
            &ChatManager::pendingMessageChanged,
            this,
            &ChatEditor::onPendingMessageChanged);
    connect(&chatManager, &ChatManager::serverPropsUpdated, this, &ChatEditor::onServerPropsUpdated);
    connect(&ChatManager::instance(),
            &ChatManager::conversationRenamed,
            this,
            [this](const QString &convId) {
                if (convId != m_viewingConvId)
                    return;

                ViewingChat chat = ChatManager::instance().getViewingChat(convId);
                m_document->setPreferredDisplayName(chat.conv.name);
                EditorManager::instance()->updateWindowTitles();
            });
    connect(&ChatManager::instance(),
            &ChatManager::conversationDeleted,
            this,
            [this](const QString &convId) {
                if (convId != m_viewingConvId)
                    return;

                QString name;
                IEditor *ed = EditorManager::openEditorWithContents(
                    Constants::LLAMACPP_VIEWER_ID,
                    &name,
                    convId.toUtf8(),
                    convId,
                    EditorManager::DoNotMakeVisible | EditorManager::DoNotSwitchToEditMode
                        | EditorManager::DoNotChangeCurrentEditor);

                EditorManager::closeEditors({ed}, false);
            });
    connect(&ChatManager::instance(),
            &ChatManager::followUpQuestionsReceived,
            this,
            &ChatEditor::createFollowUpWidget);
    connect(&chatManager,
            &ChatManager::messageExtraUpdated,
            this,
            &ChatEditor::onMessageExtraUpdated);
    connect(&ChatManager::instance(),
            &ChatManager::messageDeleted,
            this,
            [this](const QString &convId) {
                if (convId == m_viewingConvId) {
                    // Refresh the entire view to update the tree
                    ViewingChat chat = ChatManager::instance().getViewingChat(m_viewingConvId);
                    refreshMessages(chat.messages, chat.conv.currNode);
                }
            });

    connect(&ChatManager::instance(),
            &ChatManager::steeringQueueChanged,
            this,
            [this](const QString &convId, int) {
                if (convId == m_viewingConvId)
                    updateSteeringButton();
            });

    connect(&chatManager,
            &ChatManager::handoffProgress,
            this,
            &ChatEditor::onHandoffProgress);

    connect(m_input, &ChatInput::sendRequested, this, &ChatEditor::onSendRequested);
    connect(m_input, &ChatInput::stopRequested, this, [this] { onStopRequested(false); });
    connect(m_input, &ChatInput::stopKeyPressed, this, [this] { onStopRequested(true); });
    connect(m_input, &ChatInput::fileDropped, this, &ChatEditor::onFileDropped);
    connect(m_input, &ChatInput::editingCancelled, this, &ChatEditor::onEditingCancelled);

    // Connect to the document to get the conversation id
    connect(EditorManager::instance(),
            &EditorManager::currentEditorChanged,
            this,
            [this](Core::IEditor *editor) {
                if (editor != this)
                    return;

                m_viewingConvId = QString::fromUtf8(m_document->contents());
                ViewingChat chat = ChatManager::instance().getViewingChat(m_viewingConvId);

                // A new conversation has one root message
                if (chat.messages.size() > 1) {
                    refreshMessages(chat.messages, chat.conv.currNode);
                } else {
                    ChatManager::instance().refreshServerProps();
                    // refreshMessages() did not run, so sync the transient
                    // "creating continuation" note for the new conversation.
                    updateHandoffLabel();
                }

                ChatManager::instance().setCurrentConversation(m_viewingConvId);
                m_document->setPreferredDisplayName(chat.conv.name);

                EditorManager::instance()->updateWindowTitles();

                m_input->setFocus();
            });

    // Make sure we have the markdown content before saving
    connect(EditorManager::instance(),
            &EditorManager::aboutToSave,
            this,
            [this](const Core::IDocument *document) {
                if (document == static_cast<Core::IDocument *>(m_document.get())) {
                    // Tool calls are exported as <details> blocks
                    QByteArray content;

                    for (ChatMessage *chat : std::as_const(m_messageWidgets))
                        content.append(ChatManager::messageToMarkdown(chat->message()).toUtf8());

                    m_document->setContents(content);
                }
            });

    connect(EditorManager::instance(),
            &EditorManager::editorAboutToClose,
            this,
            [this](Core::IEditor *editor) {
                // Only this chat's own close matters: closing any other editor
                // must not stop the conversation shown here.
                if (editor != this)
                    return;

                // A task (sub‑agent) conversation that is still working keeps
                // running in the background when its editor is closed: the
                // sub‑agent finishes on its own and reports back to the parent
                // conversation.  The stop button and Escape still stop it, as
                // does deleting the conversation.  Task conversations never
                // have a title summary, follow‑ups or a "Human Editor"
                // session, so skipping the cancels below is safe.
                if (ChatManager::instance().isTaskConversationRunning(m_viewingConvId))
                    return;

                ChatManager::instance().stopGenerating(m_viewingConvId);
                ChatManager::instance().cancelTitleSummary(m_viewingConvId);
                ChatManager::instance().cancelFollowUp(m_viewingConvId);

                // A pending "Human Editor" reply of this chat is discarded by
                // closing its editor. Never close it from here: this runs
                // inside EditorManager::closeEditors(), and a nested call
                // could close (and delete) an editor that the outer call
                // ("Close All") is about to close as well. Defer it, and
                // re-check that the editor is still open by then.
                if (IEditor *humanEditor
                    = ChatManager::instance().humanEditorEditor(m_viewingConvId,
                                                                /*pendingOnly=*/true)) {
                    QPointer<IEditor> guard(humanEditor);
                    QMetaObject::invokeMethod(
                        EditorManager::instance(),
                        [guard] {
                            if (guard
                                && DocumentModel::editorsForOpenedDocuments().contains(
                                    guard.data())) {
                                EditorManager::closeEditors({guard.data()},
                                                            /*askAboutModifiedEditors=*/false);
                            }
                        },
                        Qt::QueuedConnection);
                }
            });

    // In‑chat search is provided by the standard Find/Replace bar of the Find
    // plugin (Ctrl+F, F3, Shift+F3).  The editor registers a
    // Core::IFindSupport and a FindToolBarPlaceHolder (see the constructor),
    // so no chat‑specific find actions are needed here – the global actions
    // pick the chat up through the focused widget automatically.

    // Global "Send to Llama Chat" action: commits the current "Human
    // Editor" document (the split text editor) as a chat message, the same
    // as saving it (Ctrl+S).  Registered once, against the long‑lived
    // ChatManager as context: an action registered against this editor
    // would be deregistered when the editor is destroyed.
    static bool sendActionRegistered = false;
    if (!sendActionRegistered) {
        sendActionRegistered = true;
        ActionBuilder sendAction(&ChatManager::instance(), Constants::LLAMACPP_SEND_ACTION);
        sendAction.setText(Tr::tr("Send to Llama Chat"));
        sendAction.setDefaultKeySequence(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Enter));
        sendAction.addOnTriggered(&ChatManager::instance(), [] {
            Core::IDocument *document = EditorManager::currentDocument();
            if (document && ChatManager::instance().isHumanEditorDocument(document))
                EditorManager::saveDocument(document);
        });
    }
}

ChatEditor::~ChatEditor()
{
    s_instances.remove(this);
    delete widget();
    // The editor tool bar only detaches the status bar (setParent(nullptr))
    // when the editor is removed from a view, it never deletes it. The guard
    // covers the case where it is still parented and was deleted with that
    // parent already.
    delete m_statusBar.data();
}

Core::IDocument *ChatEditor::document() const
{
    return m_document.data();
}

QWidget *ChatEditor::toolBar()
{
    return m_statusBar;
}

bool ChatEditor::isDesignModePreferred() const
{
    return true;
}

QWidget *ChatEditor::displayServerProps()
{
    // A single rich‑text label, laid out like Qt Creator's "Open a document"
    // placeholder (see Locator::updateEditorManagerPlaceholderText()): a
    // centred heading, a separator and a left‑aligned list.  The model name
    // and the directory in the list are links; the model name opens the model
    // details dialog and the directory a folder picker.  The label is centred
    // in the message area by the layout (see showWelcomeScreen()), the way
    // Qt Creator centres its placeholder when no editor is open.
    m_connectionLabel = new QLabel(widget());
    m_connectionLabel->setObjectName("ServerPropsStatus");
    m_connectionLabel->setTextFormat(Qt::RichText);
    m_connectionLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    m_connectionLabel->setOpenExternalLinks(false);
    connect(m_connectionLabel,
            &QLabel::linkActivated,
            this,
            [this](const QString &link) {
                if (link == QLatin1String("model"))
                    showModelDetails();
                else if (link == QLatin1String("dir"))
                    changeWorkingDirectory();
            });

    updateConnectionLabel();

    return m_connectionLabel;
}

QString ChatEditor::selectedModelDisplayName() const
{
    const ChatManager &cm = ChatManager::instance();
    if (cm.isHumanEditor())
        return Tr::tr("Human Editor");
    const QString selected = cm.selectedModel();
    if (!selected.isEmpty())
        return selected;
    // Single‑model server that does not expose /models: fall back to the
    // loaded model's file name from the server props.
    const QString name = FilePath::fromUserInput(cm.serverProps().model_path).fileName();
    return name.isEmpty() ? QStringLiteral("model") : name;
}

void ChatEditor::updateConnectionLabel()
{
    if (!m_connectionLabel)
        return;
    const QString model = selectedModelDisplayName();
    const QString dir = toolsWorkingDirectory().toUserOutput();
    // The model name and the directory are links (the model name opens the
    // model details dialog, the directory a folder picker).  The link markup
    // is not translatable, so it is built here and substituted into the
    // translatable sentence through its placeholders – a translation can put
    // the placeholders where they read best.  The values are escaped so a
    // file name with HTML metacharacters is not interpreted.
    const QString modelLink =
        QStringLiteral("<a href=\"model\" style=\"color:Token_Accent_Default\">%1</a>")
            .arg(escapeHtml(model));
    const QString dirLink =
        QStringLiteral("<a href=\"dir\" style=\"color:Token_Accent_Default\">%1</a>")
            .arg(escapeHtml(dir));
    const QString connectionLine =
        Tr::tr("You are connected to %1 using %2 as current directory").arg(modelLink, dirLink);
    // Laid out like Qt Creator's "Open a document" placeholder (see
    // Locator::updateEditorManagerPlaceholderText()): a centred heading, a
    // separator and a left‑aligned list.  Each piece of visible text is its
    // own translatable string (no markup in them); the theme tokens in the
    // structural HTML are resolved afterwards.
    m_connectionLabel->setText(replaceThemeColorNamesWithRGBNames(
        Tr::tr("<html><body style=\"color:Token_Text_Muted; font-size:14px\">"
               "<div align='center'>"
               "<div style=\"font-size:20px; font-weight:bold; "
               "color:Token_Text_Default\">%1</div>"
               "<table><tr><td>"
               "<hr/>"
               "<div style=\"margin-top: 5px\">%2</div>"
               "<div style=\"margin-top: 5px\">%3</div>"
               "</td></tr></table>"
               "</div>"
               "</body></html>")
            .arg(Tr::tr("Hello there"),
                 connectionLine,
                 Tr::tr("Type a message or upload files to get started"))));
    m_connectionLabel->setToolTip(Tr::tr(
        "Click the model name to see its details, or the directory to change "
        "where the tools run."));
}

void ChatEditor::removePropsWidget()
{
    if (m_propsWidget) {
        m_propsWidget->deleteLater();
        m_propsWidget = nullptr;
        // The connection label is a child of the welcome widget; drop the
        // reference now so a (deferred) deleteLater() cannot leave it
        // dangling while a project/model change tries to refresh it.
        m_connectionLabel = nullptr;
        // The welcome screen centred the message area; messages are top
        // aligned, so restore the layout's default alignment.
        m_messageLayout->setAlignment(Qt::AlignTop);
    }
}

void ChatEditor::showWelcomeScreen()
{
    removePropsWidget();
    if (m_propsWidget)
        return;
    m_propsWidget = displayServerProps();
    // The welcome screen is the only item in the message area, so centre it
    // (the messages themselves are top aligned, see removePropsWidget()).
    m_messageLayout->setAlignment(Qt::AlignCenter);
    m_messageLayout->insertWidget(0, m_propsWidget);
}

void ChatEditor::showModelDetails()
{
    const auto &sp = ChatManager::instance().serverProps();

    QDialog dialog(widget());
    dialog.setWindowTitle(Tr::tr("Model Details"));
    dialog.setMinimumWidth(420);

    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    auto addRow = [&](const QString &label, const QString &value) {
        auto *valueLabel = new QLabel(value.isEmpty() ? QStringLiteral("–") : value, &dialog);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        valueLabel->setWordWrap(true);
        form->addRow(label, valueLabel);
    };

    addRow(Tr::tr("Model:"), selectedModelDisplayName());
    addRow(Tr::tr("Model path:"), FilePath::fromUserInput(sp.model_path).toUserOutput());
    addRow(Tr::tr("Context:"), sp.n_ctx > 0 ? QString(" %L1").arg(sp.n_ctx) : QString());
    addRow(Tr::tr("Vision:"), sp.modalities.vision ? Tr::tr("yes") : Tr::tr("no"));
    addRow(Tr::tr("Audio:"), sp.modalities.audio ? Tr::tr("yes") : Tr::tr("no"));
    if (!sp.build_info.isEmpty())
        addRow(Tr::tr("Build:"), sp.build_info);

    layout->addLayout(form);
    layout->addStretch();

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *closeButton = new QPushButton(Tr::tr("Close"), &dialog);
    connect(closeButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    buttons->addWidget(closeButton);
    layout->addLayout(buttons);

    dialog.exec();
}

void ChatEditor::changeWorkingDirectory()
{
    const QString startDir = toolsWorkingDirectory().toUserOutput();
    const QString dir = QFileDialog::getExistingDirectory(widget(),
                                                          Tr::tr("Select Working Directory"),
                                                          startDir,
                                                          QFileDialog::ShowDirsOnly
                                                              | QFileDialog::DontUseNativeDialog);
    if (dir.isEmpty())
        return;

    settings().workingDirectory.setValue(dir);
    settings().writeSettings();
    updateConnectionLabel();
}

void ChatEditor::createFollowUpWidget(const QString &convId,
                                      qint64 leafNodeId,
                                      const QStringList &questions)
{
    if (convId != m_viewingConvId)
        return;

    if (m_followUpWidget) {
        m_followUpWidget->deleteLater();
        m_followUpWidget = nullptr;
    }

    if (questions.isEmpty())
        return; // nothing to show

    m_followUpWidget = new QWidget(widget());
    QVBoxLayout *lay = new QVBoxLayout(m_followUpWidget);
    lay->setContentsMargins(0, 0, 0, 0);

    QLabel *title = new QLabel(Tr::tr("Follow-up questions:"), m_followUpWidget);
    title->setObjectName("FollowUpLabel");
    lay->addWidget(title);

    for (const QString &q : questions) {
        QPushButton *btn = new QPushButton(q, m_followUpWidget);
        btn->setFlat(true);
        btn->setObjectName("FollowUpQuestion");

        // capture convId / leafNodeId / question in the lambda
        connect(btn, &QPushButton::clicked, this, [this, convId, leafNodeId, q]() {
            // The view jumps to the bottom when the user message is appended.
            ChatManager::instance().sendMessage(convId, leafNodeId, q, {}, [](qint64) {});
        });

        lay->addWidget(btn);
    }

    m_followUpWidget->setStyleSheet(replaceThemeColorNamesWithRGBNames(R"(
            QLabel#FollowUpLabel {
                color: Token_Text_Muted;
            }
            QPushButton#FollowUpQuestion {
                background: transparent;
                border: none;
                color: Token_Text_Muted;
                text-align: left;
                margin-left: 20px;
            }
            QPushButton#FollowUpQuestion:hover {
                color: Token_Text_Default;
                text-decoration:underline;
            }
        )"));

    lay->addStretch();
    // Append below the regular messages. No explicit scroll: the
    // AutoScrollArea follows on its own while the user is pinned to the
    // bottom.
    m_messageLayout->addWidget(m_followUpWidget);
}

void ChatEditor::refreshMessages(const QVector<Message> &messages,
                                 qint64 leafNodeId,
                                 bool pinToBottom)
{
    // Clean old widgets
    qDeleteAll(m_messageWidgets);
    m_messageWidgets.clear();

    // Delete old welcome screen if it exists
    removePropsWidget();

    if (m_followUpWidget) {
        m_followUpWidget->deleteLater();
        m_followUpWidget = nullptr;
    }

    // Filter the messages that belong to the requested leaf node
    const QVector<Message> currNodes = ChatManager::instance().filterByLeafNodeId(messages,
                                                                                  leafNodeId,
                                                                                  true);

    QMap<qint64, Message> map;
    for (const Message &m : messages) {
        map.insert(m.id, m);
    }

    for (const Message &msg : currNodes) {
        if (msg.parent < 0 || msg.type == "root")
            continue; // skip leaf nodes

        int siblingIdx = 0;
        QVector<qint64> siblings;
        const Message *parent = &map[msg.parent];
        for (qint64 cid : parent->children) {
            siblings.append(cid);
            if (msg.id == cid)
                siblingIdx = siblings.size();
        }

        // find the leaf of each child
        QVector<qint64> leafs;
        for (qint64 cid : siblings) {
            // simple traversal to leaf
            const Message *cur = &map[cid];
            while (!cur->children.isEmpty())
                cur = &map[cur->children.back()];
            leafs.append(cur->id);
        }

        ChatMessage *w = new ChatMessage(msg, leafs, siblingIdx, widget());
        connect(w, &ChatMessage::editRequested, this, &ChatEditor::onEditRequested);
        connect(w, &ChatMessage::regenerateRequested, this, &ChatEditor::onRegenerateRequested);
        connect(w, &ChatMessage::siblingChanged, this, &ChatEditor::onSiblingChanged);
        connect(w, &ChatMessage::deleteRequested, this, &ChatEditor::onDeleteMessageRequested);
        m_messageLayout->addWidget(w);
        m_messageWidgets.append(w);
    }

    // Update the status bar labels for the last assistant message
    if (m_messageWidgets.size() > 0 && !m_messageWidgets.last()->isUser()
        && !m_messageWidgets.last()->isTool()) {
        const Message &lastMsg = m_messageWidgets.last()->message();
        if (settings().showTokensPerSecond.value())
            updateSpeedLabel(lastMsg);
        updateContextLabel(lastMsg);
    }
    updateHandoffLabel();
    updateSteeringButton();

    // If there were no messages, show the welcome screen (centred in the
    // message area, like Qt Creator's "Welcome" page).
    if (m_messageWidgets.isEmpty() && !m_propsWidget)
        showWelcomeScreen();

    // The message widgets were rebuilt, which dropped any active find
    // highlights; bring them back if a find is in progress.
    if (m_findSupport)
        m_findSupport->reapplyHighlights();

    // Only pin the view to the bottom when asked; a font-zoom rebuild
    // (pinToBottom == false) must keep the user where they were reading.
    if (pinToBottom)
        scrollToBottom();

    // Let Qt's layout system run its first sizing pass before reading the
    // document heights (see MarkdownLabel::notifyGeometryChanged() for why
    // the HFW caches must be invalidated, and fixMessageHeights() for the
    // explicit-height workaround).  Only layout-related event types are
    // dispatched — a full processEvents() could re-enter refreshMessages()
    // through unrelated signals.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::Resize);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::Paint);
    fixMessageHeights();
}

// Explicitly set each message row's height to bypass Qt's heightForWidth
// caching (see MarkdownLabel::notifyGeometryChanged() for details).
void ChatEditor::fixMessageHeights()
{
    for (ChatMessage *msg : std::as_const(m_messageWidgets)) {
        MarkdownLabel *label = msg->markdownLabel();
        if (!label)
            continue;

        // Only set the message's fixed height.  The label sizes naturally
        // via its sizeHint (which uses the document's current size).  We
        // must NOT set fixed height on the label — it causes an infinite
        // layout loop because the document reflows asynchronously.
        msg->recomputeFixedHeight();
    }

    m_messageContainer->updateGeometry();
    m_messageLayout->activate();
}

// ---------------------------------------------------------------------------
// Font zoom
// ---------------------------------------------------------------------------

void ChatEditor::zoomFont(int direction, QWidget *indicatorWidget)
{
    // 10% per step, clamped to the aspect's range.  The value is persisted
    // immediately; the (expensive) re-render is debounced per editor so a
    // burst of Ctrl+mouse-wheel notches only re-renders once the user pauses.
    const int newZoom = qBound(50, settings().chatFontZoom.value() + direction * 10, 300);
    // Show the resulting size right away – also at the clamped ends, as
    // feedback – even though the re-render is still debounced.
    showZoomIndicator(indicatorWidget, newZoom);
    if (newZoom == settings().chatFontZoom.value())
        return;
    settings().chatFontZoom.setValue(newZoom);
    settings().apply();
    for (ChatEditor *editor : std::as_const(s_instances))
        if (editor->widget() && editor->widget()->isVisible())
            editor->m_zoomDebounceTimer->start();
}

void ChatEditor::resetFontZoom(QWidget *indicatorWidget)
{
    showZoomIndicator(indicatorWidget, 100);
    if (settings().chatFontZoom.value() == 100)
        return;
    settings().chatFontZoom.setValue(100);
    settings().apply();
    for (ChatEditor *editor : std::as_const(s_instances))
        if (editor->widget() && editor->widget()->isVisible())
            editor->m_zoomDebounceTimer->start();
}

void ChatEditor::showZoomIndicator(QWidget *widget, int zoom)
{
    // The indicator is centered on its parent, so parent it to the chat
    // editor the user is interacting with.  The keyboard commands leave
    // \a widget null; there the focused chat editor is the one being zoomed
    // (the commands are only bound to the chat context).
    if (!widget) {
        const QWidget *focusWidget = QApplication::focusWidget();
        for (ChatEditor *editor : std::as_const(s_instances)) {
            QWidget *w = editor->widget();
            if (w && (w == focusWidget || w->isAncestorOf(focusWidget))) {
                widget = w;
                break;
            }
        }
    }
    if (!widget)
        return;
    Utils::FadingIndicator::showText(widget, Tr::tr("Zoom: %1%").arg(zoom),
                                     Utils::FadingIndicator::SmallText);
}

void ChatEditor::applyFontZoom()
{
    if (m_viewingConvId.isEmpty())
        return;
    // Re-render every message at the new font size.  Rebuild the view the
    // same way refreshMessages() does, but keep the current scroll position
    // instead of jumping to the bottom (zooming should not yank the view).
    const int scrollValue = m_scrollArea->verticalScrollBar()->value();
    ViewingChat chat = ChatManager::instance().getViewingChat(m_viewingConvId);
    refreshMessages(chat.messages, chat.conv.currNode, /* pinToBottom */ false);
    m_scrollArea->verticalScrollBar()->setValue(scrollValue);
}

void ChatEditor::onMessageAppended(const Message &msg, qint64 pendingId)
{
    if (msg.convId != m_viewingConvId)
        return;

    ViewingChat chat = ChatManager::instance().getViewingChat(msg.convId);

    // A user/tool message that does not extend the currently shown leaf
    // switches the visible branch (e.g. the user edited an old message) –
    // fall back to a filtered rebuild for that case.
    if (pendingId < 0 && !m_messageWidgets.isEmpty()
        && m_messageWidgets.last()->message().id != msg.parent) {
        refreshMessages(chat.messages, msg.id);

        m_input->setIsGenerating(ChatManager::instance().isGenerating(msg.convId));
        scrollToBottom();
        return;
    }

    // Delete old welcome screen if it exists
    removePropsWidget();

    if (m_followUpWidget) {
        m_followUpWidget->deleteLater();
        m_followUpWidget = nullptr;
    }

    QMap<qint64, Message> map;
    for (const Message &m : chat.messages)
        map.insert(m.id, m);

    // Skip root / orphan messages
    if (msg.type == "root")
        return;

    int siblingIdx = 1;
    QVector<qint64> siblings;
    if (msg.parent >= 0) {
        const Message *parent = &map[msg.parent];
        for (qint64 cid : parent->children) {
            siblings.append(cid);
            if (msg.id == cid)
                siblingIdx = siblings.size(); // 1‑based index
        }
    }

    // Find the leaf of each sibling
    QVector<qint64> leafs;
    for (qint64 cid : siblings) {
        const Message *cur = &map[cid];
        while (!cur->children.isEmpty())
            cur = &map[cur->children.back()];
        leafs.append(cur->id);
    }

    ChatMessage *w{nullptr};
    auto it = std::find_if(m_messageWidgets.begin(),
                           m_messageWidgets.end(),
                           [this, msg, pendingId](ChatMessage *cm) {
                               return cm->message().id == (pendingId < 0 ? msg.id : pendingId);
                           });
    if (it == m_messageWidgets.end()) {
        w = new ChatMessage(msg, leafs, siblingIdx, widget());
        connect(w, &ChatMessage::editRequested, this, &ChatEditor::onEditRequested);
        connect(w, &ChatMessage::regenerateRequested, this, &ChatEditor::onRegenerateRequested);
        connect(w, &ChatMessage::siblingChanged, this, &ChatEditor::onSiblingChanged);
        connect(w, &ChatMessage::deleteRequested, this, &ChatEditor::onDeleteMessageRequested);

        preWrapDocument(w);

        m_messageLayout->addWidget(w);
        m_messageWidgets.append(w);
    } else {
        w = *it;
        w->message() = msg;

        w->setSiblingIdx(siblingIdx);
        w->setSiblingLeafIds(leafs);

        w->renderMarkdown(msg.content, true);
        w->messageCompleted(true);
    }

    // Only assistant messages carry timing data; for user/tool messages the
    // last assistant's values are kept.
    if (msg.role == "assistant") {
        updateSpeedLabel(msg);
        updateContextLabel(msg);
    }

    // Stay in the "generating" state while a tool is still executing
    // asynchronously (e.g. a web search in flight).
    m_input->setIsGenerating(ChatManager::instance().isGenerating(msg.convId));

    // Only force the view to the bottom when the user sent a message; for
    // assistant/tool appends the AutoScrollArea keeps following on its own
    // while the user is pinned to the bottom.
    if (msg.role == "user")
        scrollToBottom();

    // Settle layout and heights synchronously (the same technique
    // refreshMessages() uses).  This matters for the "Preparing ..." ->
    // tool‑bubble transition: the assistant bubble's shrink and the tool
    // bubble's insertion then land in a single paint instead of two
    // separate frames, and the freshly added bubble is measured at its
    // real width right away instead of the 300 px default of an unshown
    // QTextDocument (both previously caused a visible flicker/jump).
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::Resize);
    fixMessageHeights();
}

// Pre‑wrap the document of a freshly created message widget at the width
// its label will end up with.  An unshown QTextDocument wraps at its 300 px
// default, so without this the first layout pass reports a wrong height
// (usually too tall) – with the viewport pinned to the bottom that reads as
// the whole conversation jumping up and then back down.  Setting the real
// width before the widget is laid out makes the very first size hint already
// correct.
void ChatEditor::preWrapDocument(ChatMessage *w)
{
    int refWidth = 0;
    if (!m_messageWidgets.isEmpty())
        refWidth = m_messageWidgets.last()->width();
    if (refWidth <= 0)
        refWidth = m_messageContainer->width();
    if (refWidth > 0)
        // 20 px = the ChatMessage's left+right contents margins.
        w->markdownLabel()->document()->setTextWidth(qMax(refWidth - 20, 100));
}

void ChatEditor::onPendingMessageChanged(const Message &pm)
{
    if (pm.convId != m_viewingConvId)
        return;

    QString content = pm.content;
    if (!pm.toolCallInProgress.isEmpty()) {
        // Show the concrete operation ("Add src/main.cpp") once the partial
        // arguments reveal it, otherwise fall back to the bare tool name.
        const QString label = pm.toolCallPreview.isEmpty()
                                  ? QStringLiteral("Preparing %1 ...")
                                        .arg(pm.toolCallInProgress.toHtmlEscaped())
                                  : QStringLiteral("%1 ...").arg(pm.toolCallPreview.toHtmlEscaped());
        const QString preparing =
            QStringLiteral("<img src=\"spinner://tool\" style=\"vertical-align: middle;\"/> ") + label;
        if (!content.isEmpty())
            content += QLatin1String("\n\n");
        content += preparing;
    }

    ChatMessage *w = nullptr;

    auto it = std::find_if(m_messageWidgets.begin(),
                           m_messageWidgets.end(),
                           [this, pm](ChatMessage *cm) { return cm->message().id == pm.id; });
    if (it == m_messageWidgets.end() && content.trimmed().isEmpty()) {
        // The first SSE events (prompt progress, timings) arrive before any
        // content exists.  Do not create the bubble for those: an empty
        // bubble would push the pinned viewport up for a blank strip, and
        // the real content (thinking section, text, or the tool line)
        // arriving a moment later would push it up again – the visible
        // "up and then down" wobble after a tool call.  The status bar
        // still receives the progress/timing updates below.
        m_input->setIsGenerating(true);
        updateSpeedLabel(pm);
        updateContextLabel(pm);
        return;
    }
    if (it == m_messageWidgets.end()) {
        // Add a “loading” bubble
        Message msg;
        msg.id = pm.id;
        msg.role = "assistant";
        msg.content = content;
        msg.children.clear();

        // completed=false: the bubble is still streaming; the ctor must not
        // finish() the markdown label (see the ctor comment there).
        w = new ChatMessage(msg, {}, 0, widget(), /* completed */ false);
        preWrapDocument(w);

        m_messageLayout->addWidget(w);
        m_messageWidgets.append(w);
        connect(w, &ChatMessage::editRequested, this, &ChatEditor::onEditRequested);
        connect(w, &ChatMessage::regenerateRequested, this, &ChatEditor::onRegenerateRequested);
        connect(w, &ChatMessage::siblingChanged, this, &ChatEditor::onSiblingChanged);
        connect(w, &ChatMessage::deleteRequested, this, &ChatEditor::onDeleteMessageRequested);
    } else {
        w = *it;
        w->renderMarkdown(content);
        w->message().content = content;
    }
    w->messageCompleted(false);
    m_input->setIsGenerating(true);

    updateSpeedLabel(pm);
    updateContextLabel(pm);

    // No explicit scroll: while the user is pinned to the bottom the
    // AutoScrollArea follows the growing content, and we must not yank the
    // view down while the user is reading further up.
}

void ChatEditor::onSendRequested(const QString &text, const QList<QVariantMap> &extra)
{
    const Conversation conv = ChatManager::instance().currentConversation();

    if (m_editedMessage) {
        // The view jumps to the bottom when the new user message is appended.
        ChatManager::instance().replaceMessageAndGenerate(m_editedMessage->convId,
                                                          m_editedMessage->parent,
                                                          text,
                                                          extra,
                                                          [](qint64) {});
        m_editedMessage.reset();
    } else {
        ChatManager::instance().sendMessage(conv.id,
                                            conv.currNode,
                                            text,
                                            extra,
                                            [](qint64) {});
    }

    scrollToBottom();
}

void ChatEditor::onStopRequested(bool fromKey)
{
    // This editor's own conversation, not the globally "current" one: the
    // stop button and Escape act on the chat shown in this editor.
    const QString convId = m_viewingConvId;

    // Stopping a running task (sub‑agent) conversation fails the "task" tool
    // waiting in the parent conversation, so an Escape key (easy to press
    // by accident) asks for confirmation first; the stop button does not.
    if (fromKey && ChatManager::instance().isTaskConversationRunning(convId)
            && QMessageBox::question(widget(),
                                     Tr::tr("Stop Task"),
                                     Tr::tr("The task is still running. Do you really want "
                                            "to stop it?"))
            != QMessageBox::Yes) {
        return;
    }

    // A "Human Editor" reply in progress: closing its (unsaved) editor
    // discards the pending message.
    if (IEditor *editor = ChatManager::instance().humanEditorEditor(convId,
                                                                    /*pendingOnly=*/true)) {
        EditorManager::closeEditors({editor}, /*askAboutModifiedEditors=*/false);
        return;
    }

    ChatManager::instance().stopGenerating(convId);

    m_input->setIsGenerating(false);
}

bool ChatEditor::eventFilter(QObject *obj, QEvent *event)
{
    // Esc stops an in‑progress generation (LLM streaming or a running
    // tool) no matter which widget in the chat editor has focus.  The
    // ChatInput consumes the event first when one of its children is
    // focused, so this only fires for the message area, find bar, status
    // bar, ….
    if (obj == widget() && event->type() == QEvent::ShortcutOverride) {
        if (auto *keyEvent = static_cast<QKeyEvent *>(event);
            keyEvent->key() == Qt::Key_Escape) {
            if (m_findPlaceHolder->isVisible()) {
                // One Escape hides the in‑chat find bar and puts the focus
                // back on the input.  The standard Find bar does not do
                // this: its Escape (the global "Return to Editor"
                // shortcut) only moves the focus out of the bar, so a
                // second press would be needed to hide it.  Accepting the
                // shortcut override also keeps that global action (and the
                // Find bar's own "return to find support" context action)
                // from firing.
                Core::Find::hideFindToolBar();
                m_input->textEdit()->setFocus();
                return true;
            }
            if (m_input->isGenerating()) {
                onStopRequested(true);
                return true;
            }
        }
    }

    // Ctrl+mouse-wheel over the conversation zooms the font (in or out).
    // See the constructor for why both the viewport and the message container
    // are filtered: the container catches the event before the scroll area's
    // own scroll filter can consume it on a scrollable view, the viewport
    // covers the non‑scrollable case.
    if ((obj == m_scrollArea->viewport() || obj == m_messageContainer)
        && event->type() == QEvent::Wheel) {
        if (auto *wheelEvent = static_cast<QWheelEvent *>(event);
            (wheelEvent->modifiers() & Qt::ControlModifier) != 0
            && wheelEvent->angleDelta().y() != 0) {
            // The keyboard focus may be elsewhere, so show the indicator on
            // the editor the wheel actually happened on.
            zoomFont(wheelEvent->angleDelta().y() > 0 ? +1 : -1, widget());
            return true;
        }
    }

    return Core::IEditor::eventFilter(obj, event);
}

void ChatEditor::onFileDropped(const QStringList &files)
{
    // Pass to your context / backend
    QMessageBox::information(widget(), "Files dropped", files.join("\n"));
}

void ChatEditor::onEditRequested(const Message &sentMsg)
{
    // \a sentMsg is owned by the sending ChatMessage widget; opening the
    // "Human Editor" below can change the current editor and so refresh
    // (and delete) the message widgets.
    const Message msg = sentMsg;
    // "Human Editor" mode: assistant messages are edited in place in a
    // regular text editor (split next to the chat) – the whole message is
    // editable (the user can type anywhere), not just appended to and
    // re‑sent.  Saving the editor commits the change.
    if (msg.role == "assistant" && ChatManager::instance().isHumanEditor()) {
        openHumanEditorDocument(msg.convId, msg.id, msg.content);
        return;
    }

    m_editedMessage = msg;
    m_input->setEditingText(msg.content, msg.extra);
    m_speedLabel->setVisible(false);
}

void ChatEditor::onEditingCancelled()
{
    m_editedMessage.reset();
    m_input->setEditingText({}, {});
    m_speedLabel->setVisible(false);
}

void ChatEditor::onRegenerateRequested(const Message &msg)
{
    Message msgCopy = msg;
    ViewingChat chat = ChatManager::instance().getViewingChat(msg.convId);

    // refreshMessages will invalidate msg, which part of a ChatMessage object
    refreshMessages(chat.messages, msgCopy.parent);

    // The view jumps to the bottom via the refreshMessages call above.
    ChatManager::instance().replaceMessageAndGenerate(msgCopy.convId,
                                                      msgCopy.parent,
                                                      QString(),
                                                      msgCopy.extra,
                                                      [](qint64) {});
}

void ChatEditor::onSiblingChanged(qint64 siblingId)
{
    Conversation c = ChatManager::instance().currentConversation();
    ViewingChat chat = ChatManager::instance().getViewingChat(c.id);

    refreshMessages(chat.messages, siblingId);
}

void ChatEditor::syncThinkingLevel()
{
    const QString level = settings().thinkingLevel.value();
    const QString label = thinkingLevelDisplay(level);
    if (m_thinkingMenu) {
        const QList<QAction *> actions = m_thinkingMenu->actions();
        for (QAction *a : std::as_const(actions))
            a->setChecked(a->data().toString() == level);
    }
    m_thinkingButton->setText(QStringLiteral("T: ") + label);
    m_thinkingButton->setToolTip(
        Tr::tr("Thinking level: %1 (applies to new messages)").arg(label));
}

void ChatEditor::onModelsUpdated()
{
    updateModelCombo();
    applyHumanEditorMode();
    // The active model is part of the welcome screen's connection line.
    updateConnectionLabel();
}

void ChatEditor::applyHumanEditorMode()
{
    const bool on = ChatManager::instance().isHumanEditor();
    for (ChatMessage *w : std::as_const(m_messageWidgets))
        w->setHumanEditorMode(on);
}

void ChatEditor::updateModelCombo()
{
    const QList<ChatManager::ModelEntry> models = ChatManager::instance().models();
    const QString selected = ChatManager::instance().selectedModel();

    m_modelCombo->blockSignals(true);
    m_modelCombo->clear();
    // The "Human Editor" pseudo‑model: the "assistant" side of the
    // conversation is a human typing Markdown in a text editor split next
    // to the chat, so the chat doubles as a Markdown editor with live
    // preview.
    m_modelCombo->addItem(Tr::tr("Human Editor"),
                          QLatin1String(Constants::HUMAN_EDITOR_MODEL_ID));
    if (models.isEmpty()) {
        // Model list unavailable: show the current model from the server
        // props as a single entry.
        const QString name =
            FilePath::fromUserInput(ChatManager::instance().serverProps().model_path).fileName();
        m_modelCombo->addItem(name.isEmpty() ? QStringLiteral("model") : name);
    } else {
        for (const auto &m : models) {
            QString text = m.id;
            if (!m.status.isEmpty() && m.status != QLatin1String("loaded"))
                text += QStringLiteral(" (%1)").arg(m.status);
            m_modelCombo->addItem(text, m.id);
        }
    }
    const int idx = m_modelCombo->findData(selected);
    m_modelCombo->setCurrentIndex(idx >= 0 ? idx : 0);
    m_modelCombo->blockSignals(false);
}

void ChatEditor::onHumanEditorReplyReady(const QString &convId)
{
    if (convId != m_viewingConvId) {
        // Not the conversation we display: drop the pending reply so it
        // does not linger and the input is not left in the "generating"
        // state.
        ChatManager::instance().abortHumanEditorMessage(convId);
        return;
    }

    // Seed the editor with the text the user just sent: it is the start of
    // the document, not the final one.
    ViewingChat chat = ChatManager::instance().getViewingChat(convId);
    QString seed;
    for (const Message &m : std::as_const(chat.messages))
        if (m.id == chat.conv.currNode)
            seed = m.content;

    openHumanEditorDocument(convId, /*msgId=*/0, seed);
}

void ChatEditor::openHumanEditorDocument(const QString &convId, qint64 msgId, const QString &text)
{
    // Open the "Human Editor" document as a regular text editor in a new
    // split next to the chat: the user edits like in any editor and saves
    // (Ctrl+S, or the "Send to Llama Chat" action) to commit the message.
    const int viewId = EditorManager::viewIdForEditor(this);
    if (viewId > 0)
        EditorManager::splitView(viewId, Qt::Horizontal);

    const QString uniqueId = msgId > 0
        ? (QStringLiteral("llamacpp-message-") + QString::number(msgId))
        : (QStringLiteral("llamacpp-reply-") + QString::number(QDateTime::currentMSecsSinceEpoch()));
    // The (throw‑away) file path doubles as the document's identity; the
    // real content is passed separately and the tab shows a friendly name.
    const QString path = humanEditorFilePath(uniqueId);

    QString title = path;
    IEditor *editor = EditorManager::openEditorWithContents(
        Core::Constants::K_DEFAULT_TEXT_EDITOR_ID,
        &title,
        text.toUtf8(), // the actual document content
        uniqueId,
        EditorManager::OpenInOtherSplit);

    if (!editor) {
        // No editor could be opened: drop the pending reply so the input is
        // not left in the "generating" state.
        if (msgId <= 0)
            ChatManager::instance().abortHumanEditorMessage(convId);
        QFile::remove(path);
        return;
    }

    // openEditorWithContents() does not put the path on the document (it only
    // uses it to pick the factory); a file‑less document would trigger a
    // "Save As" dialog on save (isSaveAsNeeded() is true when filePath() is
    // empty), so point it at the throw‑away file explicitly.
    editor->document()->setFilePath(FilePath::fromString(path));
    editor->document()->setPreferredDisplayName(
        msgId > 0 ? Tr::tr("Edit Message") : Tr::tr("Human Editor"));

    ChatManager::instance().registerHumanEditorSession(editor->document(),
                                                       editor,
                                                       convId,
                                                       msgId,
                                                       path);
}

void ChatEditor::onHumanEditorAborted(const QString &convId)
{
    if (convId == m_viewingConvId)
        m_input->setIsGenerating(false);
}

void ChatEditor::onMessageContentUpdated(const Message &msg)
{
    if (msg.convId != m_viewingConvId)
        return;

    for (ChatMessage *w : std::as_const(m_messageWidgets)) {
        if (w->message().id != msg.id)
            continue;
        w->message().content = msg.content;
        w->renderMarkdown(msg.content, true);
        w->messageCompleted(true);
        w->recomputeFixedHeight();
        break;
    }
}

void ChatEditor::onServerPropsUpdated()
{
    // Show the thinking-level dropdown only for models whose chat template
    // supports thinking/reasoning control.
    m_thinkingButton->setVisible(ChatManager::instance().serverSupportsThinking());

    removePropsWidget();

    if (m_messageWidgets.isEmpty() && !m_propsWidget)
        showWelcomeScreen();

    // Now that n_ctx is known, refresh the context-usage label
    if (m_messageWidgets.size() > 0 && !m_messageWidgets.last()->isUser()
        && !m_messageWidgets.last()->isTool()) {
        updateContextLabel(m_messageWidgets.last()->message());
    }
}

void ChatEditor::onDeleteMessageRequested(const Message &sentMsg)
{
    // \a sentMsg is owned by the sending ChatMessage widget, which a view
    // refresh during the (modal) confirmation below would delete.
    const Message msg = sentMsg;
    auto [userMessages, assistantMessages] = ChatManager::instance().getBranchStats(msg.convId,
                                                                                    msg.id);
    int totalMessages = userMessages + assistantMessages;
    if (totalMessages > 1) {
        QString warningText = Tr::tr("This will delete %1 messages including: %2 user messages and "
                                     "%3 assistant responses ...")
                                  .arg(totalMessages)
                                  .arg(userMessages)
                                  .arg(assistantMessages);

        if (QMessageBox::question(widget(), Tr::tr("Confirm Branch Deletion"), warningText)
            != QMessageBox::Yes) {
            return;
        }
    } else {
        if (QMessageBox::question(widget(),
                                  Tr::tr("Delete Message"),
                                  Tr::tr("Are you sure you want to delete this message?"))
            != QMessageBox::Yes) {
            return;
        }
    }

    ChatManager::instance().deleteMessageBranch(msg.convId, msg.id);
}

void ChatEditor::onMessageExtraUpdated(const Message &msg, const QList<QVariantMap> &newExtra)
{
    if (msg.convId != m_viewingConvId)
        return;

    auto it = std::find_if(m_messageWidgets.begin(),
                           m_messageWidgets.end(),
                           [this, msg](ChatMessage *cm) { return cm->message().id == msg.id; });
    if (it != m_messageWidgets.end()) {
        ChatMessage *w = *it;
        w->message().extra = newExtra;
        w->messageCompleted(true);
        // Apply the (grown) height in the same frame instead of waiting for
        // the queued documentSizeChanged fixup – avoids a one‑frame jump
        // when the tool result appears.
        w->recomputeFixedHeight();
    }
}

void ChatEditor::updateSpeedLabel(const Message &msg)
{
    // Update the speed label (in the editor status bar) using the latest timings
    const bool enabled = settings().showTokensPerSecond.value();

    QString text;
    QString tooltip;
    bool hasData = false;

    if (enabled) {
        if (msg.content.isEmpty() && msg.promptProgress.total > 0) {
            // The server reports processed tokens *including* the cached
            // prefix (processed >= cache), so the cached part must be
            // excluded on both sides – like the llama.cpp web UI – or a
            // mostly-cached prompt (every turn after the first) shows ~100%
            // from the very first event and stays there while only the small
            // uncached delta is being processed.
            const int actualTotal = msg.promptProgress.total - msg.promptProgress.cache;
            const int actualDone = msg.promptProgress.processed - msg.promptProgress.cache;
            double percent = actualTotal > 0 ? (actualDone * 100.0) / actualTotal : 100.0;

            percent = qBound(0.0, percent, 100.0);

            text = Tr::tr("Processing: %1%").arg(percent, 0, 'f', 0);

            tooltip = Tr::tr("<b>Prompt Processing:</b><br>"
                             "Total Tokens: %1<br>"
                             "Processed: %2<br>"
                             "Cached: %3<br>"
                             "Time: %4 ms")
                          .arg(msg.promptProgress.total)
                          .arg(msg.promptProgress.processed)
                          .arg(msg.promptProgress.cache)
                          .arg(msg.promptProgress.time_ms);
            hasData = true;
        } else if (!msg.content.isEmpty()) {
            const auto &t = msg.timings;
            if (t.predicted_n >= kMinTokensForSpeed && t.predicted_ms > 0 && t.prompt_ms > 0) {
                const qreal genTokensPerSec = t.predicted_n * 1000.0 / t.predicted_ms;
                text = Tr::tr("Speed: %1 t/s").arg(genTokensPerSec, 0, 'f', 1);

                tooltip = Tr::tr(
                    "<b>Prompt:</b><br>Tokens: %1<br>Time: %2 ms<br>Speed: %3 t/s<br><br>"
                    "<b>Generation:</b><br>Tokens: %4<br>Time: %5 ms<br>Speed: %6 t/s")
                              .arg(t.prompt_n)
                              .arg(t.prompt_ms)
                              .arg(t.prompt_n * 1000.0 / t.prompt_ms, 0, 'f', 1)
                              .arg(t.predicted_n)
                              .arg(t.predicted_ms)
                              .arg(genTokensPerSec, 0, 'f', 1);
                hasData = true;
            }
        }
    }

    m_speedLabel->setVisible(enabled && hasData);
    if (hasData) {
        m_speedLabel->setText(text);
        m_speedLabel->setToolTip(tooltip);
    }
}

void ChatEditor::updateContextLabel(const Message &msg)
{
    const int maxCtx = ChatManager::instance().serverProps().n_ctx;

    // Tokens currently in the context window = cached prompt prefix +
    // newly processed prompt tokens + generated tokens. This grows with the
    // conversation. (prompt_n alone is only the uncached delta, so it stays
    // small across a long, mostly-cached conversation.)
    const auto &t = msg.timings;
    const double timingsTotal = t.cache_n + t.prompt_n + t.predicted_n;

    // Fall back to the live prompt-progress total during the prompt phase,
    // before any timing data has arrived.
    const int usedTokens = timingsTotal > 0
        ? static_cast<int>(timingsTotal)
        : msg.promptProgress.total;

    if (maxCtx <= 0 || usedTokens <= 0) {
        m_contextLabel->setVisible(false);
        return;
    }

    double percent = (usedTokens * 100.0) / maxCtx;
    percent = qBound(0.0, percent, 100.0);

    const QString percentStr = QString::number(percent, 'f', 0);

    m_contextLabel->setVisible(true);
    m_contextLabel->setText(Tr::tr("Context: %1% used").arg(percentStr));
    m_contextLabel->setToolTip(
        Tr::tr("Context: %1% used.<br>  %2 tokens from %3.")
            .arg(percentStr)
            .arg(QLocale().toString(usedTokens))
            .arg(QLocale().toString(maxCtx)));
}

void ChatEditor::updateSteeringButton()
{
    const auto &chatManager = ChatManager::instance();
    const QStringList queued = chatManager.steeringQueue(m_viewingConvId);
    m_steeringButton->setVisible(!queued.isEmpty());
    if (queued.isEmpty())
        return;

    m_steeringButton->setText(Tr::tr("%1 queued").arg(queued.size()));

    // The menu only changes when the queue does (steeringQueueChanged)
    // or the conversation switches (refreshMessages), so rebuilding it
    // here keeps it in sync without an about‑to‑show hook.
    m_steeringMenu->clear();
    for (int i = 0; i < queued.size(); ++i) {
        const QString text = queued.at(i);
        const QString firstLine = text.section(QLatin1Char('\n'), 0, 0).trimmed();
        const QString label = firstLine.length() > 60
                ? firstLine.left(60) + QLatin1String("…")
                : firstLine;
        auto *action = m_steeringMenu->addAction(
            Utils::Icon::fromTheme(QLatin1String("SP.TrashIcon")),
            Tr::tr("Remove \"%1\"").arg(label));
        action->setStatusTip(text);
        action->setToolTip(Tr::tr("Remove the queued message (it will not be sent)"));
        connect(action, &QAction::triggered, this, [this, i] {
            ChatManager::instance().removeSteeringMessage(m_viewingConvId, i);
        });
    }
}

void ChatEditor::onHandoffProgress(const QString &convId, bool active)
{
    Q_UNUSED(active); // the label is driven by the ChatManager's state
    if (convId != m_viewingConvId)
        return;
    updateHandoffLabel();
}

void ChatEditor::updateHandoffLabel()
{
    // Always rebuild: a view refresh (new message, branch switch) re‑adds
    // the message widgets after a surviving note, so the note must be
    // re‑inserted at the end of the list.
    if (m_handoffLabel) {
        m_messageLayout->removeWidget(m_handoffLabel);
        m_handoffLabel->deleteLater();
        m_handoffLabel = nullptr;
    }
    if (!ChatManager::instance().isHandingOff(m_viewingConvId))
        return;

    // Transient (never persisted) note at the end of the conversation: the
    // original conversation stays untouched and idle while its history is
    // being summarized into the seed message of the new one, and that can
    // take a while for a long conversation on a small model.
    m_handoffLabel = new MarkdownLabel(widget());
    m_handoffLabel->setMarkdown(
        QStringLiteral("<img src=\"spinner://tool\" style=\"vertical-align: middle;\"/> ")
        + Tr::tr("*Creating a new conversation to continue this one – summarizing "
                 "the conversation…"),
        true);
    m_messageLayout->addWidget(m_handoffLabel);
}

void ChatEditor::scrollToBottom()
{
    // Pin the viewport to the bottom and resume automatic following. Only
    // call this for user‑initiated actions (send, regenerate, sibling
    // switch); never for streamed data – the AutoScrollArea keeps the view
    // on the newest message on its own while the user is pinned to the
    // bottom and must not yank the view down while the user reads further up.
    m_scrollArea->followToBottom();
}

class ChatEditorFactory final : public IEditorFactory
{
public:
    ChatEditorFactory()
    {
        setId(Constants::LLAMACPP_VIEWER_ID);
        setDisplayName(Tr::tr("LlamaCpp Chat Editor"));
        setMimeTypes({Constants::LLAMACPP_CHAT_MIME_TYPE});
        setEditorCreator([] { return new ChatEditor; });
        // TODO: doesn't seem to work
        FileIconProvider::registerIconForMimeType(LLAMACPP_ICON.icon(),
                                                  QString(Constants::LLAMACPP_CHAT_MIME_TYPE));
    }
};

void setupChatEditor()
{
    static ChatEditorFactory theChatEditorFactory;
}

} // namespace LlamaCpp
