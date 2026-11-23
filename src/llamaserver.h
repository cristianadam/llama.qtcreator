#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QProcess>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkReply;
class QTimer;
class QProcessEnvironment;

namespace LlamaCpp {

// A model of the managed server (its GET /models answer) plus the details
// of the local file behind it.
struct ServerModel
{
    QString id;
    QString status; // unloaded|loading|loaded|sleeping|downloading|incomplete|failed
    int contextLength = 0;
    QStringList inputModalities;
    QString source;
    bool canRemove = false;
    QString path;
    qint64 sizeBytes = 0;
};

// The quantization label of a .gguf file name ("" when none is recognized;
// a "Q" only counts when followed by digits, so "Qwen2.5" is not a quant).
QString quantFromFileName(const QString &fileName);

// The "data" array of a GET /models answer.
QList<ServerModel> parseModelsResponse(const QByteArray &json);

// The .gguf models of a --models-dir, the way the server lists them
// (top-level files by stem, subdirectories by name).
QList<ServerModel> scanModelsDirectory(const QString &dir);

QString formatByteSize(qint64 bytes);

// The address to connect to when the server listens on \a host (a wildcard
// bind is reached through localhost).
QString serverConnectAddress(const QString &host);

// Shell-like split: whitespace separated, double quotes group.
QStringList tokenizeServeArguments(const QString &arguments);

// The serve command line. A "llama-server" runs with the flags directly;
// any other binary gets the "serve" subcommand. The extra arguments are
// appended last, so they win on repeated flags.
QStringList buildServeArguments(const QString &binaryName,
                                const QString &host,
                                int port,
                                const QString &modelsDir,
                                const QString &extraArguments);

// The "b<build>" tag of a --version output ("" when unrecognizable).
QString parseVersionTag(const QString &versionOutput);

// One event of the router's /models/sse stream.
struct ModelSseEvent
{
    QString event; // model_status|status_change|download_progress|download_finished|...
    QString model; // the model id ("*" for models_reload)
    QJsonObject data; // the event's "data" field ({} when absent)
};

// The complete events of a /models/sse byte chunk; \a buffer carries the partial block between chunks.
QList<ModelSseEvent> parseModelSseChunk(QString &buffer, const QByteArray &chunk);

// The done/total bytes of a download_progress event, summed over the urls of its "progress" map.
void downloadProgressTotals(const QJsonObject &data, qint64 *done, qint64 *total);

// The managed server's process environment: the system environment, the
// chat API key (when set), and LLAMA_ARG_LOG_COLORS=on: the server only
// colorizes its log when it detects a terminal (isatty), and it runs with
// piped output, so the "auto" default would print uncolored text.
QProcessEnvironment serverProcessEnvironment(const QString &apiKey);

// The llama-server process the plugin manages: it runs the binary found by
// LlamaRuntime, watches /health until the server answers, and keeps a
// rolling tail of the server's output. The whole output (the start command
// line first, then the server's own log) is emitted to the "llama.cpp"
// output pane, which decodes the ANSI color sequences (forced on via
// LLAMA_ARG_LOG_COLORS). A set chat API key is passed as LLAMA_API_KEY, so
// the server requires it.
class LlamaServer : public QObject
{
    Q_OBJECT
public:
    enum class State
    {
        Stopped,
        Starting, // process is up, waiting for the first /health answer
        Running,
        Failed
    };

    explicit LlamaServer(QObject *parent = nullptr);

    State state() const { return m_state; }
    QString errorMessage() const { return m_errorMessage; }
    QString binaryPath() const { return m_binaryPath; }

    // The models the running server reports (empty while stopped).
    QList<ServerModel> models() const { return m_models; }
    bool isRefreshingModels() const { return m_modelReply != nullptr; }
    // Re-reads the model list; \a reload also makes the server rescan the
    // models directory.
    void refreshModels(bool reload = false);

    // Model operations against the running server; failures report modelActionFailed().
    void loadModel(const QString &modelId);
    void unloadModel(const QString &modelId);
    void removeModel(const QString &modelId);

    // Downloads \a modelId ("repo:quant") through the server; the progress comes over /models/sse. One download at a time.
    void downloadModel(const QString &modelId);
    void cancelDownload();
    bool isDownloading() const { return m_downloading; }
    QString downloadingModel() const { return m_downloadingModel; }

    void start();
    void stop();
    void restart();

    bool launchSettingsChanged() const;

    QString outputTail() const { return m_outputTail; }

signals:
    void stateChanged(LlamaServer::State state);
    void outputAppended(const QString &text);
    void outputReset();
    void modelsUpdated(const QList<ServerModel> &models);
    // A load/unload/remove request failed (transport or server error).
    void modelActionFailed(const QString &modelId, const QString &error);
    // The download of downloadingModel(): started, byte progress (total 0 when unknown), terminal report.
    void downloadStarted(const QString &modelId);
    void downloadProgress(qint64 done, qint64 total);
    void downloadFinished(bool success, const QString &message);

private:
    struct LaunchConfig
    {
        QString binaryPath;
        QString host;
        int port = 0;
        QString modelsDir;
        QString extraArguments;
        QString apiKey;

        bool operator==(const LaunchConfig &other) const
        {
            return binaryPath == other.binaryPath && host == other.host
                   && port == other.port && modelsDir == other.modelsDir
                   && extraArguments == other.extraArguments
                   && apiKey == other.apiKey;
        }
    };
    LaunchConfig currentConfig() const;

    void setState(State state, const QString &errorMessage = {});
    void startHealthTimer();
    void stopHealthTimer();
    void probeHealth();
    void openSseStream();
    void closeSseStream();
    void onSseData(const QByteArray &chunk);
    void handleSseEvent(const ModelSseEvent &event);
    void finishDownload(bool success, const QString &message);
    void sendModelRequest(const QString &modelId, QNetworkAccessManager::Operation op,
                          const QUrl &url, const QByteArray &body);
    void checkDownloadStatus(); // the /models poll when /models/sse is unavailable
    void inspectDownloadStatus(); // against the fresh listing
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void appendOutput(const QString &text);
    void resetOutput();

    State m_state = State::Stopped;
    QString m_errorMessage;
    QString m_binaryPath;
    LaunchConfig m_activeConfig;
    QString m_outputTail;

    QProcess *m_process = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_healthReply = nullptr;
    QNetworkReply *m_modelReply = nullptr;
    QNetworkReply *m_actionReply = nullptr;
    QNetworkReply *m_sseReply = nullptr;
    QTimer *m_healthTimer = nullptr;
    QTimer *m_startTimeoutTimer = nullptr;
    QTimer *m_downloadWatchTimer = nullptr;
    bool m_stopping = false;
    QList<ServerModel> m_models;
    bool m_downloading = false;
    QString m_downloadingModel;
    QString m_sseBuffer;
    bool m_sseAvailable = true; // the last /models/sse open succeeded
};

LlamaServer &server();

} // namespace LlamaCpp
