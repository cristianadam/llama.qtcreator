#pragma once

#include <QAction>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPoint>
#include <QSettings>
#include <QTimer>

#include <extensionsystem/iplugin.h>
#include <utils/filepath.h>

#include <tuple>
#include "fimcache.h"
#include "llamatr.h"

namespace Core {
class IEditor;
class IDocument;
} // namespace Core

namespace TextEditor {
class TextMark;
class TextEditorWidget;
} // namespace TextEditor

namespace LlamaCpp {

class LlamaPlugin : public ExtensionSystem::IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QtCreatorPlugin" FILE "LlamaCpp.json")

public:
    LlamaPlugin();
    ~LlamaPlugin();

    void initialize() override;
    void extensionsInitialized() override;
    ShutdownFlag aboutToShutdown() override;
    bool delayedInitialize() override;

private slots:
    void handleCurrentEditorChanged(Core::IEditor *editor);
    void handleEditorAboutToClose(Core::IEditor *editor);
    void handleCursorPositionChanged();
    void handleCursorPositionChangedDelayed();
    void handleDocumentSaved(Core::IDocument *document);

private:
    void settingsUpdated();

    // Completion handling
    void fim(int pos_x, int pos_y, bool isAuto = false, const QStringList& prev = {});
    void fim_on_response(int pos_x,
                         int pos_y,
                         const QByteArrayList &hashes,
                         const QByteArray &response);
    void fim_try_hint(int pos_x, int pos_y);
    void fim_render(TextEditor::TextEditorWidget *editor,
                    int pos_x,
                    int pos_y,
                    const QList<QByteArray> &responses,
                    int selected);
    void fim_cycle(int direction);
    void hideCompletionHint();
    bool isValid(TextEditor::TextEditorWidget *editor);

    // Server status
    void checkServerStatus();

    Utils::FilePath getTranslationFilePath(const QString &translationFile);

    // Context management
    struct FimContext
    {
        QString prefix;
        QString middle;
        QString suffix;
        QString line_cur;
        QString line_cur_prefix;
        QString line_cur_suffix;
        int indent = 0;
    };
    FimContext fim_ctx_local(TextEditor::TextEditorWidget *editor,
                             int pos_x,
                             int pos_y,
                             const QStringList &prev = {});
    void pick_chunk(const QStringList &text, bool noModifiedState, bool doEviction);
    void ring_update();
    void pick_chunk_at_cursor(TextEditor::TextEditorWidget *editor);

    using ThreeQStrings = std::tuple<QString, QString, QString>;
    ThreeQStrings getShowInfoStats(const QJsonObject &response);
    QStringList getlines(TextEditor::TextEditorWidget *editor, int startLine, int endLine);
    QString getline(TextEditor::TextEditorWidget *editor, int line);

    // each key maps to a ring buffer of up to nCmpl individual completion responses,
    // used to avoid re-computing the same completions and to allow cycling through
    // multiple completions generated for the same context
    Fim::ResultCache m_cacheData;
    QHash<Utils::FilePath, int> m_lastEditLineHash;

    // Context chunks
    struct Chunk
    {
        QStringList data;
        QString str;
        QDateTime time;
        Utils::FilePath filename;
    };
    QList<Chunk> m_ringChunks;
    QList<Chunk> m_ringQueued;
    int m_ringNEvict = 0;

    // State tracking
    QPoint m_lastPos;
    QTimer *m_ringUpdateTimer;
    QTimer *m_positionChangedTimer;
    QNetworkAccessManager *m_networkManager;
    std::unique_ptr<QNetworkReply> m_fimReply;
    QElapsedTimer m_lastUserActivity;

    // the currently displayed suggestion (for cycling through completions)
    QList<QByteArray> m_suggestionResponses;
    int m_selectedCompletion = 0;
    QPoint m_suggestionPos;

    // indentation of the last accepted line, reused for speculative completions (n_indent)
    int m_indentLast = 0;

    // endpoint/api key/model changes invalidate the completion cache
    QString m_lastEndpoint;
    QString m_lastApiKey;
    QString m_lastModel;

    // Editor tracking
    std::unique_ptr<TextEditor::TextMark> m_textMark;
    QStringList m_suggestionContent;
    static QRegularExpression s_whitespace_regex;
    static QRegularExpression s_indent_regex;

    QAction m_newConversation{Tr::tr("New Conversation")};
    QAction m_toggleAction{Tr::tr("Toggle enable/disable llama.cpp")};
    QAction m_requestAction{Tr::tr("Request llama.cpp Suggestion")};
    QAction m_toogleAutoFimAction{Tr::tr("Toggle Auto FIM")};
    QAction m_nextCompletionAction{Tr::tr("Next Completion")};
    QAction m_prevCompletionAction{Tr::tr("Previous Completion")};
    QAction m_statusAction{Tr::tr("Show Server Status")};
};

} // namespace LlamaCpp
