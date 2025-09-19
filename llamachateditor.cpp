#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/ieditorfactory.h>
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
            &ChatManager::followUpQuestionsReceived,
            this,
            &ChatEditor::createFollowUpWidget);

    connect(m_input, &ChatInput::sendRequested, this, &ChatEditor::onSendRequested);
    connect(m_input, &ChatInput::stopRequested, this, &ChatEditor::onStopRequested);
    connect(m_input, &ChatInput::fileDropped, this, &ChatEditor::onFileDropped);
    connect(m_input, &ChatInput::editingCancelled, this, &ChatEditor::onEditingCancelled);

    // Connect to the document to get the conversation id
    connect(EditorManager::instance(),
            &EditorManager::currentEditorChanged,
            this,
            [this](Core::IEditor *editor) {
                if (editor == this) {
                    const QString convId = QString::fromUtf8(m_document->contents());
                    if (!convId.isEmpty()) {
                        auto chat = ChatManager::instance().getViewingChat(convId);
                        refreshMessages(chat.messages, chat.conv.currNode);
                    } else {
                        ChatManager::instance().refreshServerProps();
                    }
                    ChatManager::instance().setCurrentConversation(convId);

                    ViewingChat chat = ChatManager::instance().getViewingChat(convId);
                    if (chat.conv.name.isEmpty())
                        m_document->setPreferredDisplayName(Tr::tr("llama.cpp coversation"));
                    else
                        m_document->setPreferredDisplayName(chat.conv.name);

                    EditorManager::instance()->updateWindowTitles();

                    m_input->setFocus();
                }
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
}

ChatEditor::~ChatEditor()
{
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
        && settings().showTokensPerSecond.value()) {
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

    scrollToBottom();
}

void ChatEditor::onMessageAppended(const Message &msg)
{
    Conversation c = ChatManager::instance().currentConversation();
    ViewingChat chat = ChatManager::instance().getViewingChat(c.id);
    refreshMessages(chat.messages, msg.id);

    m_input->setIsGenerating(false);

    scrollToBottom();
}

void ChatEditor::onPendingMessageChanged(const Message &pm)
{
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
    scrollToBottom();
}

void ChatEditor::onStopRequested()
{
    const Conversation conv = ChatManager::instance().currentConversation();
    ChatManager::instance().stopGenerating(conv.id);

    m_input->setIsGenerating(false);
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
    ViewingChat chat = ChatManager::instance().getViewingChat(msg.convId);

    ChatManager::instance().replaceMessageAndGenerate(chat.conv.id,
                                                      msg.parent,
                                                      QString(),
                                                      msg.extra,
                                                      [this](qint64 leafId) { scrollToBottom(); });
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
    QScrollBar *sb = m_scrollArea->verticalScrollBar();
    sb->setValue(sb->maximum());
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
