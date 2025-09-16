#pragma once
#include <QVector>

#include <coreplugin/editormanager/ieditor.h>
#include <texteditor/textdocument.h>

#include "llamatypes.h"

class QVBoxLayout;
class QScrollArea;

namespace TextEditor {
class IDocument;
}

namespace LlamaCpp {

class ChatInput;
class ChatMessage;

class ChatEditor : public Core::IEditor
{
    Q_OBJECT
public:
    ChatEditor();
    ~ChatEditor();

    Core::IDocument *document() const override;
    QWidget *toolBar() override;

    bool isDesignModePreferred() const override;

    void refreshMessages(const QVector<Message>& messages, qint64 leafNodeId);
    void scrollToBottom();
    QWidget *displayServerProps();

public slots:
    void onMessageAppended(const LlamaCpp::Message &msg);
    void onPendingMessageChanged(const LlamaCpp::Message &pm);
    void onSendRequested(const QString &text, const QList<QVariantMap> &extra);
    void onStopRequested();
    void onFileDropped(const QStringList &files);
    void onEditRequested(const LlamaCpp::Message &msg);
    void onEditingCancelled();
    void onRegenerateRequested(const LlamaCpp::Message &msg);
    void onSiblingChanged(qint64 siblingId);
    void onServerPropsUpdated();

private:
    TextEditor::TextDocumentPtr m_document;
    QScrollArea *m_scrollArea;
    QWidget *m_messageContainer;
    ChatInput *m_input;
    QVBoxLayout *m_messageLayout;
    QVector<ChatMessage *> m_messageWidgets; // keep for cleanup
    std::optional<Message> m_editedMessage;

    // Widget that displays server props when no messages exist.
    QWidget *m_propsWidget{nullptr};
};

void setupChatEditor();
} // namespace LlamaCpp
