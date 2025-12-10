#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/ieditorfactory.h>
#include <coreplugin/find/textfindconstants.h>
#include <coreplugin/icore.h>

#include <utils/action.h>
#include <utils/fsengine/fileiconprovider.h>
#include <utils/utilsicons.h>

#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>

#include "llamachateditor.h"
#include "llamachatinput.h"
#include "llamachatmanager.h"
#include "llamachatmessage.h"
#include "llamaconstants.h"
#include "llamaicons.h"
#include "llamasettings.h"
#include "llamatheme.h"
#include "llamatr.h"

using namespace TextEditor;
using namespace Core;
using namespace Utils;

namespace LlamaCpp {

ChatEditor::ChatEditor()
    : m_document(new TextDocument())
{
    setContext(Context(Constants::LLAMACPP_VIEWER_ID));
    setDuplicateSupported(false);

    m_document->setId(Constants::LLAMACPP_VIEWER_ID);
    m_document->setMimeType(Constants::LLAMACPP_CHAT_MIME_TYPE);

    auto widget = new QWidget;

    m_scrollArea = new QScrollArea(widget);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scrollArea->installEventFilter(this);
    m_scrollArea->verticalScrollBar()->installEventFilter(this);

    m_messageContainer = new QWidget(m_scrollArea);
    m_messageLayout = new QVBoxLayout(m_messageContainer);
    m_messageLayout->setContentsMargins(0, 0, 0, 0);
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

    m_speedLabel = new QLabel(widget);
    m_speedLabel->setObjectName("SpeedLabel");
    m_speedLabel->setVisible(false);
    m_speedLabel->setTextFormat(Qt::PlainText);
    widget->setStyleSheet(replaceThemeColorNamesWithRGBNames(R"(
        QLabel#SpeedLabel {
           color: Token_Text_Muted;
           margin-left: 10px;
           font-style: italic;
        }
        )"));

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
                ViewingChat chat = ChatManager::instance().getViewingChat(convId);
                m_document->setPreferredDisplayName(chat.conv.name);
                EditorManager::instance()->updateWindowTitles();
            });
    connect(&ChatManager::instance(),
            &ChatManager::conversationDeleted,
            this,
            [this](const QString &convId) {
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
    connect(&chatManager, &ChatManager::messageExtraUpdated, this, &ChatEditor::onMessageExtraUpdated);

    connect(m_input, &ChatInput::sendRequested, this, &ChatEditor::onSendRequested);
    connect(m_input, &ChatInput::stopRequested, this, &ChatEditor::onStopRequested);
    connect(m_input, &ChatInput::fileDropped, this, &ChatEditor::onFileDropped);
    connect(m_input, &ChatInput::editingCancelled, this, &ChatEditor::onEditingCancelled);
    connect(m_input,
            &ChatInput::toolsSupportEnabled,
            &ChatManager::instance(),
            &ChatManager::onToolsSupportEnabled);

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
                    QByteArray content;

                    for (ChatMessage *chat : std::as_const(m_messageWidgets)) {
                        if (chat->isUser()) {
                            content.append("### User\n\n");
                            content.append(chat->message().content.toUtf8());
                            content.append("\n\n");
                        } else {
                            content.append("### Assistant\n\n");
                            content.append(chat->message().content.toUtf8());
                            content.append("\n\n");
                        }
                    }

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
    return nullptr;
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

    auto addLabel = [&](const QString &label) {
        QLabel *l = new QLabel(label, w);
        l->setObjectName("ServerPropsLabel");
        l->setWordWrap(true);
        l->setTextInteractionFlags(Qt::TextSelectableByMouse);
        lay->addWidget(l);
    };

    addLabel(Tr::tr("Model Path: %1").arg(FilePath::fromUserInput(sp.model_path).fileName()));
    addLabel(Tr::tr("Context: %L1").arg(sp.n_ctx));
    addLabel(Tr::tr("Vision: %1").arg(sp.modalities.vision ? Tr::tr("yes") : Tr::tr("no")));

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
            ChatManager::instance().sendMessage(convId, leafNodeId, q, {}, [this](qint64) {
                m_userInteracted = false;
                scrollToBottom();
            });
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
    // Append below the regular messages
    m_messageLayout->addWidget(m_followUpWidget);

    scrollToBottom();
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
        m_messageLayout->addWidget(w);
        m_messageWidgets.append(w);
    }

    // Insert speed label after the assistant widget
    if (m_messageWidgets.size() > 0 && !m_messageWidgets.last()->isUser()
        && !m_messageWidgets.last()->isTool() && settings().showTokensPerSecond.value()) {
        m_messageLayout->addWidget(m_speedLabel);
        m_speedLabel->setVisible(true);

        updateSpeedLabel(m_messageWidgets.last()->message());
    }

    // If there were no messages, show the server props
    if (m_messageWidgets.isEmpty() && !m_propsWidget) {
        m_propsWidget = displayServerProps();
        // Place it at the top of the layout
        m_messageLayout->insertWidget(0, m_propsWidget);
    }

    if (m_searchActive)
        performSearch(m_searchQuery);

    scrollToBottom();
}

void ChatEditor::onMessageAppended(const Message &msg, qint64 pendingId)
{
    if (msg.convId != m_viewingConvId)
        return;

    ViewingChat chat = ChatManager::instance().getViewingChat(msg.convId);
    if (pendingId < 0) {
        refreshMessages(chat.messages, msg.id);

        m_input->setIsGenerating(false);
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

        m_messageLayout->addWidget(w);
        m_messageWidgets.append(w);

        // Speed‑label handling for assistant messages
        if (settings().showTokensPerSecond.value() && !w->isUser()) {
            m_messageLayout->addWidget(m_speedLabel);
            m_speedLabel->setVisible(true);
        }
    } else {
        w = *it;
        w->message() = msg;

        w->setSiblingIdx(siblingIdx);
        w->setSiblingLeafIds(siblings);

        w->renderMarkdown(msg.content, true);
        w->messageCompleted(true);
    }

    updateSpeedLabel(msg);

    m_input->setIsGenerating(false);
    scrollToBottom();
}

void ChatEditor::onPendingMessageChanged(const Message &pm)
{
    if (pm.convId != m_viewingConvId)
        return;

    ChatMessage *w = nullptr;

    auto it = std::find_if(m_messageWidgets.begin(),
                           m_messageWidgets.end(),
                           [this, pm](ChatMessage *cm) { return cm->message().id == pm.id; });
    if (it == m_messageWidgets.end()) {
        // Add a “loading” bubble
        Message msg;
        msg.id = pm.id;
        msg.role = "assistant";
        msg.content = pm.content;
        msg.children.clear();

        w = new ChatMessage(msg, {}, 0, widget());
        m_messageLayout->addWidget(w);
        m_messageWidgets.append(w);
        connect(w, &ChatMessage::editRequested, this, &ChatEditor::onEditRequested);
        connect(w, &ChatMessage::regenerateRequested, this, &ChatEditor::onRegenerateRequested);
        connect(w, &ChatMessage::siblingChanged, this, &ChatEditor::onSiblingChanged);

        // Insert speed label after the assistant widget
        if (settings().showTokensPerSecond.value()) {
            m_messageLayout->addWidget(m_speedLabel);
            m_speedLabel->setVisible(true);
        }
    } else {
        w = *it;
        w->renderMarkdown(pm.content);
        w->message().content = pm.content;
    }
    w->messageCompleted(false);
    m_input->setIsGenerating(true);

    updateSpeedLabel(pm);

    scrollToBottom();
}

void ChatEditor::onSendRequested(const QString &text, const QList<QVariantMap> &extra)
{
    const Conversation conv = ChatManager::instance().currentConversation();

    if (m_editedMessage) {
        ChatManager::instance().replaceMessageAndGenerate(m_editedMessage->convId,
                                                          m_editedMessage->parent,
                                                          text,
                                                          extra,
                                                          [this](qint64 leafId) {
                                                              scrollToBottom();
                                                          });
        m_editedMessage.reset();
    } else {
        ChatManager::instance().sendMessage(conv.id,
                                            conv.currNode,
                                            text,
                                            extra,
                                            [this](qint64 leafId) { scrollToBottom(); });
    }

    m_userInteracted = false;
    scrollToBottom();
}

void ChatEditor::onStopRequested()
{
    const Conversation conv = ChatManager::instance().currentConversation();
    ChatManager::instance().stopGenerating(conv.id);

    m_input->setIsGenerating(false);
    m_userInteracted = false;
}

void ChatEditor::onFileDropped(const QStringList &files)
{
    // Pass to your context / backend
    QMessageBox::information(widget(), "Files dropped", files.join("\n"));
}

void ChatEditor::onEditRequested(const Message &msg)
{
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

    ChatManager::instance().replaceMessageAndGenerate(msgCopy.convId,
                                                      msgCopy.parent,
                                                      QString(),
                                                      msgCopy.extra,
                                                      [this, msgCopy](qint64 leafId) {
                                                          scrollToBottom();
                                                      });

    m_userInteracted = false;
}

void ChatEditor::onSiblingChanged(qint64 siblingId)
{
    Conversation c = ChatManager::instance().currentConversation();
    ViewingChat chat = ChatManager::instance().getViewingChat(c.id);

    refreshMessages(chat.messages, siblingId);
}

void ChatEditor::onServerPropsUpdated()
{
    if (m_propsWidget) {
        m_propsWidget->deleteLater();
        m_propsWidget = nullptr;
    }

    if (m_messageWidgets.isEmpty() && !m_propsWidget) {
        m_propsWidget = displayServerProps();
        m_messageLayout->insertWidget(0, m_propsWidget);
    }
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
    // Update the speed label using the latest timings
    if (settings().showTokensPerSecond.value()) {
        const auto &t = msg.timings;
        if (t.predicted_ms > 0 && t.prompt_ms > 0) {
            qreal tokensPerSec = (t.predicted_n + t.prompt_n) * 1000.0
                                 / (t.predicted_ms + t.prompt_ms);
            m_speedLabel->setText(Tr::tr("Speed: %1 t/s").arg(tokensPerSec, 0, 'f', 1));

            QString labelTooltip(
                Tr::tr("<b>Prompt:</b><br>Tokens: %1<br>Time: %2 ms<br>Speed: %3 t/s<br><br>"
                       "<b>Generation:</b><br>Tokens: %4<br>Time: %5 ms<br>Speed: %6 t/s")
                    .arg(t.prompt_n)
                    .arg(t.prompt_ms)
                    .arg(t.prompt_n * 1000.0 / t.prompt_ms, 0, 'f', 1)
                    .arg(t.predicted_n)
                    .arg(t.predicted_ms)
                    .arg(t.predicted_n * 1000.0 / t.predicted_ms, 0, 'f', 1));
            m_speedLabel->setToolTip(labelTooltip);
        }
    }
}

void ChatEditor::scrollToBottom()
{
    if (m_userInteracted)
        return;

    // Scroll to bottom after the layout has finished
    QTimer::singleShot(100, this, [this] {
        QScrollBar *sb = m_scrollArea->verticalScrollBar();
        sb->setValue(sb->maximum());
    });
}

bool ChatEditor::eventFilter(QObject *obj, QEvent *event)
{
    if (obj == m_scrollArea || obj == m_scrollArea->verticalScrollBar()) {
        // Mouse wheel, mouse press, key press – any user interaction
        if (event->type() == QEvent::Wheel || event->type() == QEvent::MouseButtonPress
            || event->type() == QEvent::MouseButtonRelease || event->type() == QEvent::KeyPress) {
            m_userInteracted = true;
        }
    }
    return QObject::eventFilter(obj, event);
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
