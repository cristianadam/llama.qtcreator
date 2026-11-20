#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

class QNetworkAccessManager;
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
    QTimer *m_healthTimer = nullptr;
    QTimer *m_startTimeoutTimer = nullptr;
    bool m_stopping = false;
    QList<ServerModel> m_models;
};

LlamaServer &server();

} // namespace LlamaCpp
