#include <coreplugin/actionmanager/actionmanager.h>
#include <coreplugin/coreconstants.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/editormanager/ieditorfactory.h>
#include <coreplugin/icore.h>

#include <utils/action.h>
#include <utils/utilsicons.h>

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

#include "llamachatinput.h"
#include "llamachatmanager.h"
#include "llamachatmessage.h"
#include "llamaconstants.h"
#include "llamatr.h"
#include "llamatypes.h"

using namespace TextEditor;
using namespace Core;
using namespace Utils;

namespace LlamaCpp {

class ChatEditor : public IEditor
{
    Q_OBJECT
public:
    ChatEditor()
        : m_document(new TextDocument(Constants::LLAMACPP_TEXT_CONTEXT))
    {
        setContext(Context(Constants::LLAMACPP_VIEWER_ID));
        setDuplicateSupported(false);

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
        m_widget->installEventFilter(this);

        ChatManager &chatManager = ChatManager::instance();
        connect(&chatManager, &ChatManager::messageAppended, this, &ChatEditor::onMessageAppended);
        connect(&chatManager,
                &ChatManager::pendingMessageChanged,
                this,
                &ChatEditor::onPendingMessageChanged);

        connect(m_input, &ChatInput::sendRequested, this, &ChatEditor::onSendRequested);
        connect(m_input, &ChatInput::stopRequested, this, &ChatEditor::onStopRequested);
        connect(m_input, &ChatInput::fileDropped, this, &ChatEditor::onFileDropped);

        // Connect to the document to get the conversation id
        connect(EditorManager::instance(),
                &EditorManager::currentEditorChanged,
                this,
                [this](Core::IEditor *editor) {
                    if (editor == this) {
                        const QString convId = QString::fromUtf8(m_document->contents());
                        if (!convId.isEmpty()) {
                            auto chat = ChatManager::instance().getViewingChat(convId);
                            refreshMessages(chat);
                        }
                        ChatManager::instance().setCurrentConversation(convId);

                        m_input->setFocus();
                    }
                });

        // Make sure we have the markdown content before saving
        connect(EditorManager::instance(),
                &EditorManager::aboutToSave,
                this,
                [this](const IDocument *document) {
                    if (document == static_cast<IDocument *>(m_document.get())) {
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
    ~ChatEditor() { delete widget(); }

    IDocument *document() const override { return m_document.data(); }
    QWidget *toolBar() override { return nullptr; }

    bool isDesignModePreferred() const override { return true; }

    bool eventFilter(QObject *obj, QEvent *ev) override
    {
        // TODO: this doesn't seem to work
        if (obj == m_widget && ev->type() == QEvent::FocusIn) {
            m_input->setFocus();
            return true;
        }
        return IEditor::eventFilter(obj, ev);
    }

    void refreshMessages(const ViewingChat &chat)
    {
        // Clean old widgets
        qDeleteAll(m_messageWidgets);
        m_messageWidgets.clear();

        QMap<qint64, Message> map;
        for (const Message &m : chat.messages) {
            map.insert(m.id, m);
        }

        for (const Message &msg : chat.messages) {
            if (msg.parent < 0)
                continue;       // skip leaf nodes
            int siblingIdx = 0; // you can compute the sibling index from the parent
            QVector<qint64> siblings;
            const Message *parent = &map[msg.parent];
            for (qint64 cid : parent->children)
                siblings.append(cid);
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

    void onMessageAppended(const Message &msg)
    {
        ChatMessage *w = nullptr;
        // Add just the new message (you could also just call refreshMessages())
        auto it = std::find_if(m_messageWidgets.begin(),
                               m_messageWidgets.end(),
                               [this, msg](ChatMessage *cm) { return cm->message().id == msg.id; });
        if (it == m_messageWidgets.end()) {
            w = new ChatMessage(msg, {}, 0, widget());
            m_messageLayout->addWidget(w);
            m_messageWidgets.append(w);
            connect(w, &ChatMessage::editRequested, this, &ChatEditor::onEditRequested);
            connect(w, &ChatMessage::regenerateRequested, this, &ChatEditor::onRegenerateRequested);
            connect(w, &ChatMessage::siblingChanged, this, &ChatEditor::onSiblingChanged);
        } else {
            w = *it;
            w->message().convId = msg.convId;
        }
        w->messageCompleted(true);

        scrollToBottom();
    }

    void onPendingMessageChanged(const Message &pm)
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

        scrollToBottom();
    }

    void onSendRequested(const QString &text)
    {
        const Conversation conv = ChatManager::instance().currentConversation();

        ChatManager::instance().sendMessage(conv.id,
                                            conv.currNode, // last message id (simplified)
                                            text,
                                            {}, // extra context
                                            [this](qint64 leafId) { scrollToBottom(); });
        scrollToBottom();
    }

    void onStopRequested()
    {
        const Conversation conv = ChatManager::instance().currentConversation();
        ChatManager::instance().stopGenerating(conv.id);
    }

    void onFileDropped(const QStringList &files)
    {
        // Pass to your context / backend
        QMessageBox::information(widget(), "Files dropped", files.join("\n"));
    }

    void onEditRequested(const Message &msg)
    {
        // Show a modal dialog with a QTextEdit to edit the message,
        // then call ChatManager::replaceMessageAndGenerate()
    }

    void onRegenerateRequested(const Message &msg)
    {
        ViewingChat chat = ChatManager::instance().getViewingChat(msg.convId);

        ChatManager::instance().replaceMessageAndGenerate(chat.conv.id,
                                                          msg.id,
                                                          nullptr, // new content = null → keep old
                                                          msg.extra,
                                                          [this](qint64 leafId) { /* onChunk */ });
    }

    void onSiblingChanged(qint64 siblingId)
    {
        // Update the UI to show the selected sibling
        // For simplicity you might just reload the whole conversation
        // or store a “current leaf” ID and rebuild the list.
    }

    void scrollToBottom()
    {
        QScrollBar *sb = m_scrollArea->verticalScrollBar();
        sb->setValue(sb->maximum());
    }

private:
    TextDocumentPtr m_document;
    QScrollArea *m_scrollArea;
    QWidget *m_messageContainer;
    ChatInput *m_input;
    QVBoxLayout *m_messageLayout;
    QVector<ChatMessage *> m_messageWidgets; // keep for cleanup
};

class ChatEditorFactory final : public IEditorFactory
{
public:
    ChatEditorFactory()
    {
        setId(Constants::LLAMACPP_VIEWER_ID);
        setDisplayName(Tr::tr("LlamaCpp Chat Editor"));
        addMimeType(Constants::LLAMACPP_CHAT_MIME_TYPE);
        setEditorCreator([] { return new ChatEditor; });
    }
};

void setupChatEditor()
{
    static ChatEditorFactory theChatEditorFactory;
}

} // namespace LlamaCpp

#include "llamachateditor.moc"
