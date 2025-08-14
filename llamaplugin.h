#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPoint>
#include <QSettings>
#include <QTimer>

#include <extensionsystem/iplugin.h>
#include <utils/filepath.h>

#include <tuple>

namespace Core {
class IEditor;
class IDocument;
} // namespace Core

namespace TextEditor {
class TextMark;
class TextEditorWidget;
} // namespace TextEditor

namespace LlamaCpp::Internal {

class LlamaPlugin : public ExtensionSystem::IPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QtCreatorPlugin" FILE "LlamaCpp.json")

public:
    LlamaPlugin();
    ~LlamaPlugin();

    void initialize() override;
    ShutdownFlag aboutToShutdown() override;
    bool delayedInitialize() override;

private slots:
    void handleCurrentEditorChanged(Core::IEditor *editor);
    void handleEditorAboutToClose(Core::IEditor *editor);
    void handleCursorPositionChanged();
    void handleDocumentSaved(Core::IDocument *document);

private:
    void settingsUpdated();

    // Completion handling
    void fim(int pos_x, int pos_y, bool isAuto = false, const QStringList& prev = {});
    void fim_on_response(int pos_x, int pos_y, const QByteArray &hash, const QByteArray &response);
    void fim_try_hint(int pos_x, int pos_y);
    void fim_render(TextEditor::TextEditorWidget *editor,
                    int pos_x,
                    int pos_y,
                    const QByteArray &response);
    void hideCompletionHint();

    // Context management
    struct FimContext
    {
        QString prefix;
        QString middle;
        QString suffix;
        QString line_cur;
        QString line_cur_prefix;
        QString line_cur_suffix;
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

    QHash<QByteArray, QByteArray> m_cacheData;
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
    QNetworkAccessManager *m_networkManager;
    std::unique_ptr<QNetworkReply> m_fimReply;

    // Editor tracking
    std::unique_ptr<TextEditor::TextMark> m_textMark;
    QStringList m_suggestionContent;
    static QRegularExpression s_whitespace_regex;
};

} // namespace LlamaCpp::Internal
