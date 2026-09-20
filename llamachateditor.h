#pragma once
#include <QVector>

#include <coreplugin/editormanager/ieditor.h>
#include <texteditor/textdocument.h>

#include "autoscrollarea.h"
#include "llamasearchtoolbar.h"
#include "llamatypes.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;

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

    void refreshMessages(const QVector<Message> &messages, qint64 leafNodeId);
    // Explicitly set message row heights to bypass Qt's heightForWidth caching
    // which causes long messages to be clamped at wrong heights.
    void fixMessageHeights();
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
    void onModelsUpdated();
    void onDeleteMessageRequested(const LlamaCpp::Message &msg);

    void startSearch();
    void nextSearchResult();
    void prevSearchResult();
    void clearSearch();
    void onMessageExtraUpdated(const LlamaCpp::Message &msg, const QList<QVariantMap> &newExtra);

private:
    void updateSpeedLabel(const Message &msg);
    void updateContextLabel(const Message &msg);
    void updateThinkingButtonLabel();
    void updateModelCombo();
    void performSearch(const QString &query);
    void jumpToResult(int idx, bool selected = true);

private:
    TextEditor::TextDocumentPtr m_document;
    AutoScrollArea *m_scrollArea{nullptr};
    QWidget *m_messageContainer{nullptr};
    ChatInput *m_input{nullptr};
    QVBoxLayout *m_messageLayout{nullptr};
    QVector<ChatMessage *> m_messageWidgets; // keep for cleanup
    std::optional<Message> m_editedMessage;
    QWidget *m_propsWidget{nullptr};
    QWidget *m_followUpWidget{nullptr};
    QWidget *m_statusBar{nullptr};
    QLabel *m_contextLabel{nullptr};
    QLabel *m_speedLabel{nullptr};
    QToolButton *m_thinkingButton{nullptr};
    QComboBox *m_modelCombo{nullptr};

    QVector<SearchResult> m_searchResults; // all matches of the current query
    int m_currentResult{0};                // index into m_searchResults
    QString m_searchQuery;                 // the string that is active
    bool m_searchActive{false};

    SearchToolbar *m_searchToolbar{nullptr};
    QString m_viewingConvId;
};

void setupChatEditor();
} // namespace LlamaCpp
