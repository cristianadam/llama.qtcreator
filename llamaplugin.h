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
}

namespace TextEditor {
    class TextMark;
    class TextEditorWidget;
}

class QTextDocument;

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
    void checkForCompletion();

private:
    void settingsUpdated();

    // Completion handling
    void requestCompletion(int pos_x, int pos_y, bool isAuto = false);
    void processCompletionResponse(const QString &response);
    void hideCompletionHint();

    // Context management
    using ThreeQStrings = std::tuple<QString, QString, QString>;

    ThreeQStrings getLocalContext(int pos_x, int pos_y, const QString &prev = QString());
    void pickChunk(const QStringList &text, bool noModifiedState, bool doEviction);
    void ringUpdate();
    void pickChunkAtCursor(const TextEditor::TextEditorWidget* editor);

    ThreeQStrings getShowInfoStats(const QJsonObject &response);
    QStringList getLines(const QTextDocument *document, int startLine, int endLine);

    QHash<QString, QString> m_cacheData;
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
    bool m_hintShown = false;
    QPoint m_lastPos;
    QTimer *m_ringUpdateTimer;
    QNetworkAccessManager *m_networkManager;
    QString m_currentRequestId;

    // Editor tracking
    std::unique_ptr<TextEditor::TextMark> m_textMark;
};

} // namespace LlamaCpp::Internal
