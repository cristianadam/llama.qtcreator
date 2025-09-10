#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/ieditorfactory.h>
#include <coreplugin/icore.h>

#include <utils/action.h>
#include <utils/utilsicons.h>
#include <utils/fsengine/fileiconprovider.h>

#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>

#include <QApplication>
#include <QHBoxLayout>
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

    ChatManager &chatManager = ChatManager::instance();
    connect(&chatManager, &ChatManager::messageAppended, this, &ChatEditor::onMessageAppended);
    connect(&chatManager,
            &ChatManager::pendingMessageChanged,
            this,
            &ChatEditor::onPendingMessageChanged);

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
                    }
                    ChatManager::instance().setCurrentConversation(convId);

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

void ChatEditor::refreshMessages(const QVector<Message>& messages, qint64 leafNodeId)
{
    // Clean old widgets
    qDeleteAll(m_messageWidgets);
    m_messageWidgets.clear();

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
            continue;       // skip leaf nodes

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
    } else {
        w = *it;
        w->renderMarkdown(pm.content);
        w->message().content = pm.content;
    }
    w->messageCompleted(false);
    m_input->setIsGenerating(true);

    scrollToBottom();
}

void ChatEditor::onSendRequested(const QString &text)
{
    const Conversation conv = ChatManager::instance().currentConversation();

    if (m_editedMessage) {
        ChatManager::instance().replaceMessageAndGenerate(m_editedMessage->convId,
                                                          m_editedMessage->parent,
                                                          text,
                                                          m_editedMessage->extra,
                                                          [this](qint64 leafId) {
                                                              scrollToBottom();
                                                          });
        m_editedMessage.reset();
    } else {
        ChatManager::instance().sendMessage(conv.id,
                                            conv.currNode,
                                            text,
                                            {}, // extra context
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
    m_input->setEditingText(msg.content);
}

void ChatEditor::onEditingCancelled()
{
    m_editedMessage.reset();
    m_input->setEditingText({});
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
