#pragma once
#include <QVector>

#include <coreplugin/editormanager/ieditor.h>
#include <texteditor/textdocument.h>

#include "llamasearchtoolbar.h"
#include "llamatypes.h"

class QLabel;
class QLineEdit;
class QPushButton;
class QVBoxLayout;
class QScrollArea;

namespace TextEditor {
class IDocument;
}

namespace LlamaCpp {

class ChatInput;
class ChatMessage;
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

    bool eventFilter(QObject *obj, QEvent *event) override;

    void refreshMessages(const QVector<Message> &messages, qint64 leafNodeId);
    void scrollToBottom();
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
    void onStopRequested();
    void onFileDropped(const QStringList &files);
    void onEditRequested(const LlamaCpp::Message &msg);
    void onEditingCancelled();
    void onRegenerateRequested(const LlamaCpp::Message &msg);
    void onSiblingChanged(qint64 siblingId);
    void onServerPropsUpdated();

    void startSearch();
    void nextSearchResult();
    void prevSearchResult();
    void clearSearch();
    void onMessageExtraUpdated(const LlamaCpp::Message &msg, const QList<QVariantMap> &newExtra);

private:
    void updateSpeedLabel(const Message &msg);
    void performSearch(const QString &query);
    void jumpToResult(int idx, bool selected = true);

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

    QVector<SearchResult> m_searchResults; // all matches of the current query
    int m_currentResult{0};                // index into m_searchResults
    QString m_searchQuery;                 // the string that is active
    bool m_searchActive{false};

    SearchToolbar *m_searchToolbar{nullptr};
    QString m_viewingConvId;
};

void setupChatEditor();
} // namespace LlamaCpp
