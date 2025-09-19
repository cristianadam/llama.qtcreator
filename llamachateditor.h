#pragma once
#include <QVector>

#include <coreplugin/editormanager/ieditor.h>
#include <texteditor/textdocument.h>

#include "llamatypes.h"

class QLabel;
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

    bool eventFilter(QObject *obj, QEvent *event) override;

    void refreshMessages(const QVector<Message> &messages, qint64 leafNodeId);
    void scrollToBottom();
    QWidget *displayServerProps();
    void createFollowUpWidget(const QString &convId,
                              qint64 leafNodeId,
                              const QStringList &questions);

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
    void updateSpeedLabel(const Message &msg);

private:
    TextEditor::TextDocumentPtr m_document;
    QScrollArea *m_scrollArea{nullptr};
    QWidget *m_messageContainer{nullptr};
    ChatInput *m_input{nullptr};
    QVBoxLayout *m_messageLayout{nullptr};
    QVector<ChatMessage *> m_messageWidgets; // keep for cleanup
    std::optional<Message> m_editedMessage;
    QWidget *m_propsWidget{nullptr};
    QWidget *m_followUpWidget{nullptr};
    QLabel *m_speedLabel{nullptr};
    bool m_userInteracted{false};
};

void setupChatEditor();
} // namespace LlamaCpp
