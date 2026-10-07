#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/ieditorfactory.h>
#include <coreplugin/find/textfindconstants.h>
#include <coreplugin/icore.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>

#include <utils/action.h>
#include <utils/fsengine/fileiconprovider.h>
#include <utils/icon.h>
#include <utils/utilsicons.h>
#include <utils/widgets.h>

#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>

#include <QCoreApplication>
#include <QAction>
#include <QDir>
#include <QFile>
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
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

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

ChatEditor::ChatEditor()
    : m_document(new TextDocument())
{
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

    // The "Working directory" label in the empty-conversation props widget
    // is created once, so keep it in sync when the startup project changes.
    connect(ProjectExplorer::ProjectManager::instance(),
            &ProjectExplorer::ProjectManager::startupProjectChanged, this,
            [this](ProjectExplorer::Project *) {
                if (m_workingDirLabel)
                    m_workingDirLabel->setText(Tr::tr("Working directory: %1")
                                                   .arg(toolsWorkingDirectory().toUserOutput()));
            });

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
    m_steeringButton->setToolTip(Tr::tr("Queued messages – sent after the current step"));
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

    m_searchToolbar = new SearchToolbar(widget);

    connect(m_searchToolbar,
            &SearchToolbar::onPrevSearchClicked,
            this,
            &ChatEditor::prevSearchResult);
    connect(m_searchToolbar,
            &SearchToolbar::onNextSearchClicked,
            this,
            &ChatEditor::nextSearchResult);
    connect(m_searchToolbar, &SearchToolbar::onCloseEvent, this, [this]() { clearSearch(); });
    connect(m_searchToolbar, &SearchToolbar::onSearchTextChanged, this, [this](const QString &text) {
        if (text.isEmpty()) {
            clearSearch();
            return;
        }
        m_searchQuery = text;
        performSearch(text);
        m_searchActive = true;
        m_currentResult = 0;
        jumpToResult(m_currentResult);
    });

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

    connect(m_input, &ChatInput::sendRequested, this, &ChatEditor::onSendRequested);
    connect(m_input, &ChatInput::stopRequested, this, &ChatEditor::onStopRequested);
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
                if (editor == this) {
                    ChatManager::instance().stopGenerating(m_viewingConvId);
                    ChatManager::instance().cancelTitleSummary(m_viewingConvId);
                    ChatManager::instance().cancelFollowUp(m_viewingConvId);
                }

                onStopRequested();
            });

    // Search in chat  (Ctrl+F)
    ActionBuilder startSearchAction(this, Core::Constants::FIND_IN_DOCUMENT);
    startSearchAction.setText(Tr::tr("Search in chat"));
    startSearchAction.setContext(Context(Constants::LLAMACPP_VIEWER_ID));
    startSearchAction.addOnTriggered(this, [this] { startSearch(); });

    // Next search result (F3)
    ActionBuilder nextSearchAction(this, Core::Constants::FIND_NEXT);
    nextSearchAction.setText(Tr::tr("Next search result"));
    nextSearchAction.setContext(Context(Constants::LLAMACPP_VIEWER_ID));
    nextSearchAction.addOnTriggered(this, [this] { nextSearchResult(); });

    // Previous search result (Shift+F3)
    ActionBuilder prevSearchAction(this, Core::Constants::FIND_PREVIOUS);
    prevSearchAction.setText(Tr::tr("Previous search result"));
    prevSearchAction.setContext(Context(Constants::LLAMACPP_VIEWER_ID));
    prevSearchAction.addOnTriggered(this, [this] { prevSearchResult(); });

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
    clearSearch();
    delete widget();
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
    QWidget *w = new QWidget(widget());
    w->setObjectName("ServerProps");
    auto lay = new QVBoxLayout(w);
    lay->setContentsMargins(8, 8, 8, 8);
    lay->setAlignment(Qt::AlignTop);

    const auto &sp = ChatManager::instance().serverProps();

    auto addLabel = [&](const QString &label) -> QLabel * {
        QLabel *l = new QLabel(label, w);
        l->setObjectName("ServerPropsLabel");
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lay->addWidget(l);
        return l;
    };

    addLabel(Tr::tr("Model Path: %1").arg(FilePath::fromUserInput(sp.model_path).fileName()));
    addLabel(Tr::tr("Context: %L1").arg(sp.n_ctx));
    addLabel(Tr::tr("Vision: %1").arg(sp.modalities.vision ? Tr::tr("yes") : Tr::tr("no")));

    // The directory the tools operate in: the startup project when one is
    // open, otherwise the default projects directory. Kept in a member so
    // it can be refreshed when the startup project changes (the props
    // widget itself is created once and not rebuilt).
    m_workingDirLabel = addLabel(Tr::tr("Working directory: %1")
                                     .arg(toolsWorkingDirectory().toUserOutput()));

    w->setStyleSheet(replaceThemeColorNamesWithRGBNames(R"(
        QWidget#ServerProps {
            border: 1px solid Token_Foreground_Muted;
            border-radius: 8px;
        }

        QLabel#ServerPropsLabel {
            color: Token_Text_Subtle;
        }
    )"));

    return w;
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

    QLabel *title = new QLabel(Tr::tr("Follow‑up questions:"), m_followUpWidget);
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

void ChatEditor::refreshMessages(const QVector<Message> &messages, qint64 leafNodeId)
{
    // Clean old widgets
    qDeleteAll(m_messageWidgets);
    m_messageWidgets.clear();

    // Delete old server‑props widget if it exists
    if (m_propsWidget) {
        m_propsWidget->deleteLater();
        m_propsWidget = nullptr;
    }

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
    updateSteeringButton();

    // If there were no messages, show the server props
    if (m_messageWidgets.isEmpty() && !m_propsWidget) {
        m_propsWidget = displayServerProps();
        // Place it at the top of the layout
        m_messageLayout->insertWidget(0, m_propsWidget);
    }

    if (m_searchActive)
        performSearch(m_searchQuery);

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

    // Delete old server‑props widget if it exists
    if (m_propsWidget) {
        m_propsWidget->deleteLater();
        m_propsWidget = nullptr;
    }

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

void ChatEditor::onStopRequested()
{
    const Conversation conv = ChatManager::instance().currentConversation();

    // A "Human Editor" reply in progress: closing its (unsaved) editor
    // discards the pending message.
    if (IEditor *editor = ChatManager::instance().humanEditorEditor(conv.id,
                                                                    /*pendingOnly=*/true)) {
        EditorManager::closeEditors({editor}, /*askAboutModifiedEditors=*/false);
        return;
    }

    ChatManager::instance().stopGenerating(conv.id);

    m_input->setIsGenerating(false);
}

bool ChatEditor::eventFilter(QObject *obj, QEvent *event)
{
    // Esc stops an in‑progress generation (LLM streaming or a running
    // tool) no matter which widget in the chat editor has focus.  The
    // ChatInput consumes the event first when one of its children is
    // focused, so this only fires for the message area, status bar, ….
    if (obj == widget() && event->type() == QEvent::ShortcutOverride) {
        if (auto *keyEvent = static_cast<QKeyEvent *>(event);
            keyEvent->key() == Qt::Key_Escape && m_input->isGenerating()) {
            onStopRequested();
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

void ChatEditor::onEditRequested(const Message &msg)
{
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

    if (m_propsWidget) {
        m_propsWidget->deleteLater();
        m_propsWidget = nullptr;
    }

    if (m_messageWidgets.isEmpty() && !m_propsWidget) {
        m_propsWidget = displayServerProps();
        m_messageLayout->insertWidget(0, m_propsWidget);
    }

    // Now that n_ctx is known, refresh the context-usage label
    if (m_messageWidgets.size() > 0 && !m_messageWidgets.last()->isUser()
        && !m_messageWidgets.last()->isTool()) {
        updateContextLabel(m_messageWidgets.last()->message());
    }
}

void ChatEditor::onDeleteMessageRequested(const Message &msg)
{
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

void ChatEditor::startSearch()
{
    // Show the floating search toolbar
    m_searchToolbar->show();
    m_searchToolbar->raise();
    m_searchToolbar->activateWindow();

    // If we already have a query, run a search to highlight it
    if (!m_searchQuery.isEmpty()) {
        performSearch(m_searchQuery);
        m_searchActive = true;
        m_currentResult = 0;
        jumpToResult(m_currentResult);
    } else {
        clearSearch(); // clear any old highlights
    }
}

void ChatEditor::nextSearchResult()
{
    if (!m_searchActive || m_searchResults.isEmpty())
        return;

    jumpToResult(m_currentResult, false);

    m_currentResult = (m_currentResult + 1) % m_searchResults.size();
    jumpToResult(m_currentResult);
}

void ChatEditor::prevSearchResult()
{
    if (!m_searchActive || m_searchResults.isEmpty())
        return;

    jumpToResult(m_currentResult, false);

    m_currentResult = (m_currentResult - 1 + m_searchResults.size()) % m_searchResults.size();
    jumpToResult(m_currentResult);
}

void ChatEditor::clearSearch()
{
    for (ChatMessage *w : std::as_const(m_messageWidgets))
        w->clearHighlight();

    m_searchResults.clear();
    m_currentResult = 0;
    m_searchActive = false;
    m_searchQuery.clear();
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

void ChatEditor::performSearch(const QString &query)
{
    clearSearch(); // wipe old highlights

    QRegularExpression re(query, QRegularExpression::CaseInsensitiveOption);
    for (ChatMessage *w : std::as_const(m_messageWidgets)) {
        const QString txt = w->plainText();
        QRegularExpressionMatchIterator it = re.globalMatch(txt);
        while (it.hasNext()) {
            QRegularExpressionMatch m = it.next();
            SearchResult r;
            r.widget = w;
            r.start = m.capturedStart();
            r.length = m.capturedLength();
            m_searchResults.append(r);
        }
        // highlight all matches inside the widget
        w->highlightAllMatches(query);
    }
}

void ChatEditor::jumpToResult(int idx, bool selected)
{
    if (m_searchResults.isEmpty())
        m_searchToolbar->setIndexLabel(QString("0/0"));
    else
        m_searchToolbar->setIndexLabel(QString("%1/%2").arg(idx + 1).arg(m_searchResults.size()));

    if (idx < 0 || idx >= m_searchResults.size())
        return;

    const SearchResult &r = m_searchResults[idx];
    if (!r.widget)
        return;

    // Find the QTextEdit inside the widget (MarkdownLabel is a QTextBrowser)
    QTextEdit *te = r.widget->findChild<QTextEdit *>();
    if (!te)
        return;

    QTextCursor cursor(te->document());
    cursor.setPosition(r.start);
    cursor.setPosition(r.start + r.length, QTextCursor::KeepAnchor);

    QRect selRect = te->cursorRect(cursor);

    // Map that rectangle to the outer viewport coordinates We must map from the viewport,
    // otherwise the widget's own scroll position is ignored and the coordinates are wrong.
    QPoint topLeftInOuter = te->viewport()->mapTo(m_scrollArea->viewport(), selRect.topLeft());

    // Centre the rectangle vertically in the outer viewport ---
    int viewportHeight = m_scrollArea->viewport()->height();

    int targetY = topLeftInOuter.y()     // top of the selection
                  + selRect.height() / 2 // centre of the selection
                  - viewportHeight / 2;  // centre of the viewport

    // Clamp to the valid scroll‑bar range
    QScrollBar *vbar = m_scrollArea->verticalScrollBar();
    targetY = qBound(0, targetY, vbar->maximum());

    vbar->setValue(targetY);

    // Highlight only the current hit inside the widget
    r.widget->highlightMatch(r.start, r.length, selected);
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
            double processed = msg.promptProgress.processed + msg.promptProgress.cache;
            double percent = (processed / msg.promptProgress.total) * 100.0;

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
            if (t.predicted_ms > 0 && t.prompt_ms > 0) {
                qreal tokensPerSec = (t.predicted_n + t.prompt_n) * 1000.0
                                     / (t.predicted_ms + t.prompt_ms);
                text = Tr::tr("Speed: %1 t/s").arg(tokensPerSec, 0, 'f', 1);

                tooltip = Tr::tr(
                    "<b>Prompt:</b><br>Tokens: %1<br>Time: %2 ms<br>Speed: %3 t/s<br><br>"
                    "<b>Generation:</b><br>Tokens: %4<br>Time: %5 ms<br>Speed: %6 t/s")
                              .arg(t.prompt_n)
                              .arg(t.prompt_ms)
                              .arg(t.prompt_n * 1000.0 / t.prompt_ms, 0, 'f', 1)
                              .arg(t.predicted_n)
                              .arg(t.predicted_ms)
                              .arg(t.predicted_n * 1000.0 / t.predicted_ms, 0, 'f', 1);
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
            Tr::tr("Remove “%1”").arg(label));
        action->setStatusTip(text);
        action->setToolTip(Tr::tr("Remove the queued message (it will not be sent)"));
        connect(action, &QAction::triggered, this, [this, i] {
            ChatManager::instance().removeSteeringMessage(m_viewingConvId, i);
        });
    }
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
