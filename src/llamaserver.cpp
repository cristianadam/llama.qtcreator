#include "llamaserver.h"

#include "llamasettings.h"
#include "llamatr.h"
#include "llamaruntime.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QTimer>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(llamaServerLog, "llama.cpp.server", QtInfoMsg)

namespace LlamaCpp {

QString serverConnectAddress(const QString &host)
{
    if (host == QLatin1String("0.0.0.0") || host == QLatin1String("::"))
        return QStringLiteral("127.0.0.1");
    return host;
}

QStringList tokenizeServeArguments(const QString &arguments)
{
    QStringList tokens;
    QString current;
    bool inQuotes = false;
    bool hasToken = false;
    for (const QChar ch : arguments) {
        if (ch == QLatin1Char('"')) {
            inQuotes = !inQuotes;
            hasToken = true;
        } else if (!inQuotes && ch.isSpace()) {
            if (hasToken) {
                tokens << current;
                current.clear();
                hasToken = false;
            }
        } else {
            current += ch;
            hasToken = true;
        }
    }
    if (hasToken)
        tokens << current;
    return tokens;
}

QStringList buildServeArguments(const QString &binaryName,
                                const QString &host,
                                int port,
                                const QString &modelsDir,
                                const QString &extraArguments)
{
    QStringList arguments;
    if (!binaryName.startsWith(QLatin1String("llama-server")))
        arguments << QLatin1String("serve");
    arguments << QStringLiteral("--host") << host
              << QStringLiteral("--port") << QString::number(port)
              << QStringLiteral("--jinja");
    if (!modelsDir.isEmpty())
        arguments << QStringLiteral("--models-dir") << modelsDir;
    arguments += tokenizeServeArguments(extraArguments);
    return arguments;
}

QString parseVersionTag(const QString &versionOutput)
{
    // "build 12110" or "b9553".
    static const QRegularExpression reBuildExplicit(QStringLiteral("build:?\\s*(\\d{2,7})"));
    static const QRegularExpression reBuild(QStringLiteral("\\bb(\\d{2,7})\\b"));
    auto match = reBuildExplicit.match(versionOutput);
    if (match.hasMatch())
        return QStringLiteral("b") + match.captured(1);
    if ((match = reBuild.match(versionOutput)).hasMatch())
        return QStringLiteral("b") + match.captured(1);
    return {};
}

QList<ModelSseEvent> parseModelSseChunk(QString &buffer, const QByteArray &chunk)
{
    QList<ModelSseEvent> events;
    buffer += QString::fromUtf8(chunk);
    // An event is a "data: <json>" block terminated by a blank line.
    int end;
    while ((end = buffer.indexOf(QLatin1String("\n\n"))) >= 0) {
        const QString block = buffer.left(end);
        buffer.remove(0, end + 2);
        ModelSseEvent event;
        for (const QString &line : block.split(QLatin1Char('\n'))) {
            if (!line.startsWith(QLatin1String("data:")))
                continue; // comment / keep-alive lines carry no payload
            QJsonParseError parseError;
            const QJsonDocument doc = QJsonDocument::fromJson(line.mid(5).trimmed().toUtf8(),
                                                              &parseError);
            if (parseError.error != QJsonParseError::NoError)
                continue;
            const QJsonObject obj = doc.object();
            event.event = obj.value("event").toString();
            event.model = obj.value("model").toString();
            event.data = obj.value("data").toObject();
            break;
        }
        if (!event.event.isEmpty())
            events.append(event);
    }
    return events;
}

void downloadProgressTotals(const QJsonObject &data, qint64 *done, qint64 *total)
{
    *done = 0;
    *total = 0;
    const QJsonObject progress = data.value("progress").toObject();
    for (auto it = progress.constBegin(); it != progress.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        *done += entry.value("done").toVariant().toLongLong();
        *total += entry.value("total").toVariant().toLongLong();
    }
}

QProcessEnvironment serverProcessEnvironment(const QString &apiKey)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    if (!apiKey.isEmpty())
        environment.insert(QStringLiteral("LLAMA_API_KEY"), apiKey);
    // The server only colorizes its log when it detects a terminal
    // (tty_can_use_colors() is an isatty check; CLICOLOR_FORCE & co. are not
    // consulted), and it runs with piped output: force the colors on, the
    // output pane decodes the ANSI sequences.
    environment.insert(QStringLiteral("LLAMA_ARG_LOG_COLORS"), QStringLiteral("on"));
    return environment;
}

QString quantFromFileName(const QString &fileName)
{
    // A "UD-" prefix belongs to the next part (UD-Q4_K_XL), so it is matched
    // across the split.
    static const QRegularExpression reQuant(
        QStringLiteral("^(UD-)?((Q|IQ)\\d{1,2}(_[A-Z0-9]+)*|(F|BF)16|F32|mxfp\\d)$"));
    const QString base = QFileInfo(fileName).completeBaseName();
    const QStringList parts = base.split(QLatin1Char('-'), Qt::SkipEmptyParts);
    for (int i = 0; i < parts.size(); ++i) {
        QString part = parts.at(i);
        if (part == QLatin1String("UD") && i + 1 < parts.size()) {
            part = QLatin1String("UD-") + parts.at(i + 1);
            ++i;
        }
        if (reQuant.match(part).hasMatch())
            return part;
    }
    return {};
}

QList<ServerModel> parseModelsResponse(const QByteArray &json)
{
    QList<ServerModel> out;
    const QJsonArray arr = QJsonDocument::fromJson(json).object().value("data").toArray();
    for (const auto &value : arr) {
        const QJsonObject obj = value.toObject();
        ServerModel model;
        model.id = obj.value("id").toString();
        if (model.id.isEmpty())
            continue;
        model.status = obj.value("status").toObject().value("value").toString();
        model.contextLength = obj.value("context_length").toInt();
        const QJsonArray modalities = obj.value("architecture")
                                          .toObject()
                                          .value("input_modalities")
                                          .toArray();
        for (const auto &modality : modalities)
            model.inputModalities.append(modality.toString());
        model.source = obj.value("source").toString();
        model.canRemove = obj.value("can_remove").toBool();
        out.append(model);
    }
    return out;
}

namespace {
// A companion file (projector, calibration, draft model), not a model.
bool isModelSidecar(const QString &fileName)
{
    const QString lower = fileName.toLower();
    return lower.contains(QLatin1String("mmproj"))
           || lower.contains(QLatin1String("imatrix"))
           || lower.startsWith(QLatin1String("mtp-"))
           || lower.startsWith(QLatin1String("dflash-"))
           || lower.startsWith(QLatin1String("dspark-"))
           || lower.startsWith(QLatin1String("eagle3-"));
}
} // namespace

QList<ServerModel> scanModelsDirectory(const QString &dir)
{
    QList<ServerModel> out;
    QDir root(dir);
    if (!root.exists())
        return out;
    const QFileInfoList entries = root.entryInfoList(
        QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : entries) {
        ServerModel model;
        if (info.isDir()) {
            // A subdirectory is one model: the main .gguf (the first shard
            // when sharded) plus sidecars.
            model.id = info.fileName();
            QString main;
            const QFileInfoList files = QDir(info.absoluteFilePath())
                                           .entryInfoList(QDir::Files, QDir::Name);
            for (const QFileInfo &file : files) {
                const QString name = file.fileName();
                if (!name.endsWith(QLatin1String(".gguf"), Qt::CaseInsensitive)
                    || isModelSidecar(name))
                    continue;
                model.sizeBytes += file.size();
                if (name.contains(QLatin1String("-00001-of-")))
                    main = name;
                else if (main.isEmpty())
                    main = name;
            }
            if (main.isEmpty())
                continue; // no model file in the directory
            model.path = QDir(info.absoluteFilePath()).filePath(main);
        } else if (info.fileName().endsWith(QLatin1String(".gguf"), Qt::CaseInsensitive)
                   && !isModelSidecar(info.fileName())) {
            model.id = info.completeBaseName();
            model.path = info.absoluteFilePath();
            model.sizeBytes = info.size();
        } else {
            continue;
        }
        out.append(model);
    }
    return out;
}

QString formatByteSize(qint64 bytes)
{
    if (bytes <= 0)
        return QStringLiteral("?");
    const double gb = bytes / 1024.0 / 1024.0 / 1024.0;
    if (gb >= 1) {
        QString text = QStringLiteral("%1 GB").arg(gb, 0, 'f', gb >= 10 ? 0 : 1);
        if (text.endsWith(QLatin1String(".0 GB")))
            text.remove(1, 2); // "2.0 GB" reads better as "2 GB"
        return text;
    }
    const double mb = bytes / 1024.0 / 1024.0;
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 0);
}

LlamaServer::LlamaServer(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
    , m_process(new QProcess(this))
    , m_healthTimer(new QTimer(this))
    , m_startTimeoutTimer(new QTimer(this))
{
    m_healthTimer->setInterval(2000);
    m_healthTimer->setSingleShot(false);
    connect(m_healthTimer, &QTimer::timeout, this, &LlamaServer::probeHealth);

    // The download's completion watch when /models/sse is unavailable.
    m_downloadWatchTimer = new QTimer(this);
    m_downloadWatchTimer->setInterval(2000);
    connect(m_downloadWatchTimer, &QTimer::timeout, this, &LlamaServer::checkDownloadStatus);

    m_startTimeoutTimer->setInterval(120 * 1000);
    m_startTimeoutTimer->setSingleShot(true);
    connect(m_startTimeoutTimer, &QTimer::timeout, this, [this] {
        if (m_state == State::Starting)
            setState(State::Failed,
                     Tr::tr("The server did not answer within two minutes. See its output "
                            "for the reason."));
    });

    connect(m_process, &QProcess::started, this, [this] {
        // A stop() raced the start: kill the process as soon as it is up.
        if (m_stopping) {
            m_process->terminate();
            if (!m_process->waitForFinished(2000)) {
                m_process->kill();
                m_process->waitForFinished(1000);
            }
            return;
        }
        m_startTimeoutTimer->start();
        startHealthTimer();
    });
    connect(m_process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus exitStatus) {
                onProcessFinished(exitCode, exitStatus);
            });
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (m_state == State::Stopped)
            return;
        if (error == QProcess::FailedToStart) {
            const QString tail = m_outputTail.trimmed();
            const int keep = 300;
            setState(State::Failed,
                     Tr::tr("The server could not be started%1.").arg(
                         tail.isEmpty() ? QString()
                                        : (QLatin1Char(' ') + tail.right(keep))));
        }
    });
    connect(m_process, &QProcess::readyReadStandardOutput, this, [this] {
        appendOutput(QString::fromLocal8Bit(m_process->readAllStandardOutput()));
    });
    connect(m_process, &QProcess::readyReadStandardError, this, [this] {
        appendOutput(QString::fromLocal8Bit(m_process->readAllStandardError()));
    });
}

LlamaServer::LaunchConfig LlamaServer::currentConfig() const
{
    // The applied values (.value(), not the volatile ones the settings
    // dialog edits): an unapplied edit must not change the launch config.
    LaunchConfig config;
    config.binaryPath = runtime().resolvedPath();
    config.host = settings().serverHost.value();
    config.port = settings().serverPort.value();
    config.modelsDir = settings().serverModelsDir.value();
    config.extraArguments = settings().serverArguments.value();
    config.apiKey = settings().chatApiKey.value();
    return config;
}

bool LlamaServer::launchSettingsChanged() const
{
    if (m_state != State::Running && m_state != State::Starting)
        return false;
    return currentConfig() != m_activeConfig;
}

void LlamaServer::setState(State state, const QString &errorMessage)
{
    if (m_state == state && m_errorMessage == errorMessage)
        return;
    m_state = state;
    m_errorMessage = errorMessage;
    if (state != State::Starting && state != State::Running) {
        stopHealthTimer();
        m_startTimeoutTimer->stop();
    }
    qCInfo(llamaServerLog) << "server state:" << int(state) << errorMessage;
    emit stateChanged(state);
}

void LlamaServer::start()
{
    if (m_state == State::Starting || m_state == State::Running)
        return;
    stopHealthTimer();
    m_startTimeoutTimer->stop();
    if (m_healthReply) {
        m_healthReply->abort();
        m_healthReply = nullptr;
    }

    const LaunchConfig config = currentConfig();
    m_activeConfig = config;
    m_binaryPath = config.binaryPath;
    m_stopping = false;
    // A restarted server may be a different build: re-probe /models/sse.
    m_sseAvailable = true;
    resetOutput();
    if (!m_models.isEmpty()) {
        m_models.clear();
        emit modelsUpdated(m_models);
    }

    if (config.binaryPath.isEmpty()) {
        setState(State::Failed,
                 Tr::tr("No llama.cpp binary was found. Download it or set its path on this "
                        "settings page."));
        return;
    }
    // From here a start that never runs must report Failed, not Stopped.
    setState(State::Starting);
    if (!config.modelsDir.isEmpty())
        QDir().mkpath(config.modelsDir);

    const QStringList arguments = buildServeArguments(
        QFileInfo(config.binaryPath).fileName(),
        config.host, config.port, config.modelsDir, config.extraArguments);
    qCInfo(llamaServerLog) << "starting" << config.binaryPath << arguments;

    // The command line starts the output, so the pane shows exactly what
    // was run; it is not part of the tail (the error messages show the
    // server's own output there).
    emit outputAppended(QStringLiteral("$ %1 %2\n")
                            .arg(config.binaryPath)
                            .arg(arguments.join(QStringLiteral(" "))));

    m_process->setProcessEnvironment(serverProcessEnvironment(config.apiKey));
    m_process->setWorkingDirectory(QFileInfo(config.binaryPath).absolutePath());
    m_process->start(config.binaryPath, arguments);
}

void LlamaServer::stop()
{
    if (m_state == State::Stopped)
        return;
    m_stopping = true;
    stopHealthTimer();
    m_startTimeoutTimer->stop();
    if (m_healthReply) {
        m_healthReply->abort();
        m_healthReply = nullptr;
    }
    if (m_modelReply) {
        m_modelReply->abort();
        m_modelReply = nullptr;
    }
    if (m_actionReply) {
        m_actionReply->abort();
        m_actionReply = nullptr;
    }
    if (m_downloading)
        finishDownload(false, Tr::tr("The server was stopped."));
    else {
        m_downloadWatchTimer->stop();
        closeSseStream();
    }
    if (!m_models.isEmpty()) {
        m_models.clear();
        emit modelsUpdated(m_models);
    }
    if (m_process->state() == QProcess::Starting)
        m_process->waitForStarted(1000);
    if (m_process->state() != QProcess::NotRunning) {
        m_process->terminate();
        if (!m_process->waitForFinished(2000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    setState(State::Stopped);
}

void LlamaServer::restart()
{
    if (m_state == State::Starting || m_state == State::Running)
        stop();
    start();
}

void LlamaServer::startHealthTimer()
{
    if (!m_healthTimer->isActive()) {
        m_healthTimer->start();
        probeHealth();
    }
}

void LlamaServer::stopHealthTimer()
{
    m_healthTimer->stop();
}

void LlamaServer::probeHealth()
{
    if (m_state != State::Starting)
        return;
    if (m_healthReply)
        return; // a probe is in flight
    QNetworkRequest request(QUrl(
        QStringLiteral("http://%1:%2/health")
            .arg(serverConnectAddress(m_activeConfig.host))
            .arg(m_activeConfig.port)));
    if (!m_activeConfig.apiKey.isEmpty())
        request.setRawHeader("Authorization",
                             (QStringLiteral("Bearer ") + m_activeConfig.apiKey).toUtf8());
    m_healthReply = m_network->get(request);
    connect(m_healthReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
        m_healthReply = nullptr;
        if (!reply)
            return;
        reply->deleteLater();
        if (m_state != State::Starting)
            return;
        // Any HTTP answer (even a 401) means the server is up; a transport
        // error means nothing is listening yet.
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() == QNetworkReply::NoError || code > 0)
            setState(State::Running);
    });
}

void LlamaServer::refreshModels(bool reload)
{
    if (m_state != State::Running && m_state != State::Starting) {
        if (!m_models.isEmpty()) {
            m_models.clear();
            emit modelsUpdated(m_models);
        }
        return;
    }
    if (m_modelReply) {
        m_modelReply->abort();
        m_modelReply = nullptr;
    }
    // ?reload makes the server rescan the models directory.
    QUrl url(QStringLiteral("http://%1:%2/models%3")
                 .arg(serverConnectAddress(m_activeConfig.host))
                 .arg(m_activeConfig.port)
                 .arg(reload ? QStringLiteral("?reload") : QString()));
    QNetworkRequest request(url);
    if (!m_activeConfig.apiKey.isEmpty())
        request.setRawHeader("Authorization",
                             (QStringLiteral("Bearer ") + m_activeConfig.apiKey).toUtf8());
    m_modelReply = m_network->get(request);
    connect(m_modelReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
        m_modelReply = nullptr;
        if (!reply)
            return;
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
            return; // the server is going away; the state change clears the list
        m_models = parseModelsResponse(reply->readAll());
        // The listing carries no on-disk size; the directory scan has it.
        QHash<QString, ServerModel> local;
        if (!m_activeConfig.modelsDir.isEmpty())
            for (const ServerModel &m : scanModelsDirectory(m_activeConfig.modelsDir))
                local.insert(m.id, m);
        for (ServerModel &m : m_models) {
            const auto it = local.constFind(m.id);
            if (it != local.constEnd()) {
                m.path = it->path;
                m.sizeBytes = it->sizeBytes;
            }
        }
        emit modelsUpdated(m_models);
        // A download watched through the model list is checked against the fresh listing.
        if (m_downloading)
            inspectDownloadStatus();
    });
}

namespace {
// The "message" of a llama-server error body, or the body itself when unrecognizable.
QString serverErrorMessage(const QByteArray &body)
{
    const QJsonObject obj = QJsonDocument::fromJson(body).object();
    const QJsonValue error = obj.value("error");
    if (error.isObject()) {
        const QString message = error.toObject().value("message").toString();
        if (!message.isEmpty())
            return message;
    } else if (error.isString()) {
        return error.toString();
    }
    return QString::fromUtf8(body).trimmed();
}

// A model the server runs for requests (the unloadable states).
bool isModelRunning(const QString &status)
{
    return status == QLatin1String("loaded") || status == QLatin1String("loading")
           || status == QLatin1String("sleeping");
}
} // namespace

void LlamaServer::sendModelRequest(const QString &modelId,
                                   QNetworkAccessManager::Operation op,
                                   const QUrl &url,
                                   const QByteArray &body)
{
    if (m_state != State::Running) {
        emit modelActionFailed(modelId, Tr::tr("The server is not running."));
        return;
    }
    if (m_actionReply) {
        m_actionReply->abort();
        m_actionReply = nullptr;
    }
    QNetworkRequest request(url);
    if (!m_activeConfig.apiKey.isEmpty())
        request.setRawHeader("Authorization",
                             (QStringLiteral("Bearer ") + m_activeConfig.apiKey).toUtf8());
    QNetworkReply *reply = nullptr;
    if (op == QNetworkAccessManager::PostOperation)
        reply = m_network->post(request, body);
    else if (op == QNetworkAccessManager::DeleteOperation)
        reply = m_network->deleteResource(request);
    else
        reply = m_network->get(request);
    m_actionReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, modelId] {
        QNetworkReply *r = qobject_cast<QNetworkReply *>(sender());
        if (m_actionReply == r)
            m_actionReply = nullptr;
        if (!r)
            return;
        const int code = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError || code < 200 || code >= 300) {
            QString message;
            if (r->error() == QNetworkReply::OperationCanceledError) {
                // Aborted by a stop(); the state change reports it.
            } else if (code == 404 || code == 405) {
                message = Tr::tr(
                    "The server does not support this operation: the model "
                    "endpoints exist only in router mode (start it without a "
                    "fixed --model).");
            } else {
                message = Tr::tr("The server rejected the request: %1")
                              .arg(serverErrorMessage(r->readAll()));
            }
            if (!message.isEmpty())
                emit modelActionFailed(modelId, message);
        }
        r->deleteLater();
    });
}

void LlamaServer::loadModel(const QString &modelId)
{
    QUrl url(QStringLiteral("http://%1:%2/models/load")
                 .arg(serverConnectAddress(m_activeConfig.host))
                 .arg(m_activeConfig.port));
    sendModelRequest(modelId,
                     QNetworkAccessManager::PostOperation,
                     url,
                     QJsonDocument(QJsonObject{{"model", modelId}}).toJson());
}

void LlamaServer::unloadModel(const QString &modelId)
{
    QUrl url(QStringLiteral("http://%1:%2/models/unload")
                 .arg(serverConnectAddress(m_activeConfig.host))
                 .arg(m_activeConfig.port));
    sendModelRequest(modelId,
                     QNetworkAccessManager::PostOperation,
                     url,
                     QJsonDocument(QJsonObject{{"model", modelId}}).toJson());
}

void LlamaServer::removeModel(const QString &modelId)
{
    QUrl url(QStringLiteral("http://%1:%2/models?model=%3")
                 .arg(serverConnectAddress(m_activeConfig.host))
                 .arg(m_activeConfig.port)
                 .arg(modelId.toUtf8().toPercentEncoding()));
    sendModelRequest(modelId, QNetworkAccessManager::DeleteOperation, url, {});
}

void LlamaServer::downloadModel(const QString &modelId)
{
    if (m_downloading)
        return; // one download at a time
    if (m_state != State::Running) {
        emit downloadFinished(false, Tr::tr("The server is not running."));
        return;
    }
    m_downloading = true;
    m_downloadingModel = modelId;
    m_sseBuffer.clear();
    emit downloadStarted(modelId);

    // The SSE stream first, so no early progress event is missed; without it the model-list watch is the fallback.
    if (m_sseAvailable)
        openSseStream();
    else
        m_downloadWatchTimer->start();

    QUrl url(QStringLiteral("http://%1:%2/models")
                 .arg(serverConnectAddress(m_activeConfig.host))
                 .arg(m_activeConfig.port));
    QNetworkRequest request(url);
    if (!m_activeConfig.apiKey.isEmpty())
        request.setRawHeader("Authorization",
                             (QStringLiteral("Bearer ") + m_activeConfig.apiKey).toUtf8());
    QNetworkReply *reply = m_network->post(
        request, QJsonDocument(QJsonObject{{"model", modelId}}).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (m_downloading && reply->error() != QNetworkReply::NoError
            && code < 200) {
            // No HTTP answer at all: the server went away mid-download.
            if (m_state == State::Running)
                finishDownload(false, Tr::tr("The connection to the server was lost."));
        } else if (m_downloading && (code < 200 || code >= 300)) {
            finishDownload(false,
                           Tr::tr("The server rejected the download: %1")
                               .arg(serverErrorMessage(reply->readAll())));
        }
        reply->deleteLater();
    });
}

void LlamaServer::cancelDownload()
{
    if (!m_downloading)
        return;
    // The server stops the transfer; the SSE stream reports it (stopped: true), or the unload answer when there is no stream.
    const QString modelId = m_downloadingModel;
    QUrl url(QStringLiteral("http://%1:%2/models/unload")
                 .arg(serverConnectAddress(m_activeConfig.host))
                 .arg(m_activeConfig.port));
    QNetworkRequest request(url);
    if (!m_activeConfig.apiKey.isEmpty())
        request.setRawHeader("Authorization",
                             (QStringLiteral("Bearer ") + m_activeConfig.apiKey).toUtf8());
    QNetworkReply *reply = m_network->post(
        request, QJsonDocument(QJsonObject{{"model", modelId}}).toJson());
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (m_downloading && m_sseReply == nullptr
            && (reply->error() == QNetworkReply::NoError
                || (code >= 200 && code < 300))) {
            finishDownload(false, Tr::tr("The download was stopped."));
        }
        reply->deleteLater();
    });
}

void LlamaServer::openSseStream()
{
    if (m_sseReply)
        return;
    QUrl url(QStringLiteral("http://%1:%2/models/sse")
                 .arg(serverConnectAddress(m_activeConfig.host))
                 .arg(m_activeConfig.port));
    QNetworkRequest request(url);
    if (!m_activeConfig.apiKey.isEmpty())
        request.setRawHeader("Authorization",
                             (QStringLiteral("Bearer ") + m_activeConfig.apiKey).toUtf8());
    // An open-ended stream: no transfer timeout.
    request.setTransferTimeout(0);
    m_sseReply = m_network->get(request);
    connect(m_sseReply, &QNetworkReply::readyRead, this, [this] {
        if (m_sseReply)
            onSseData(m_sseReply->readAll());
    });
    connect(m_sseReply, &QNetworkReply::finished, this, [this] {
        QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
        if (m_sseReply == reply)
            m_sseReply = nullptr;
        if (!reply)
            return;
        // Only a server-side answer says anything about the endpoint (not our own abort).
        if (reply->error() != QNetworkReply::OperationCanceledError)
            m_sseAvailable = reply->error() == QNetworkReply::NoError
                             && reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        reply->deleteLater();
        // Without the stream the model-list watch is the completion signal.
        if (m_downloading && !m_downloadWatchTimer->isActive())
            m_downloadWatchTimer->start();
    });
}

void LlamaServer::closeSseStream()
{
    if (!m_sseReply)
        return;
    m_sseReply->abort();
    m_sseReply = nullptr;
}

void LlamaServer::onSseData(const QByteArray &chunk)
{
    for (const ModelSseEvent &event : parseModelSseChunk(m_sseBuffer, chunk))
        handleSseEvent(event);
}

void LlamaServer::handleSseEvent(const ModelSseEvent &event)
{
    if (!m_downloading || event.model != m_downloadingModel)
        return;
    if (event.event == QLatin1String("download_progress")) {
        qint64 done = 0;
        qint64 total = 0;
        downloadProgressTotals(event.data, &done, &total);
        emit downloadProgress(done, total);
    } else if (event.event == QLatin1String("download_finished")) {
        finishDownload(true, Tr::tr("The download is complete."));
    } else if (event.event == QLatin1String("download_failed")) {
        QString message;
        if (event.data.value("stopped").toBool())
            message = Tr::tr("The download was stopped.");
        else {
            QString detail = event.data.value("message").toString();
            if (detail.isEmpty())
                detail = event.data.value("error").toString();
            message = Tr::tr("The download failed%1.")
                          .arg(detail.isEmpty() ? QString()
                                                : (QLatin1Char(' ') + detail));
        }
        finishDownload(false, message);
    }
}

void LlamaServer::finishDownload(bool success, const QString &message)
{
    if (!m_downloading)
        return;
    m_downloading = false;
    m_downloadingModel.clear();
    m_downloadWatchTimer->stop();
    closeSseStream();
    emit downloadFinished(success, message);
    // The finished download resurfaces in the list (a failed one is resumable).
    refreshModels();
}

void LlamaServer::checkDownloadStatus()
{
    if (!m_downloading)
        return;
    // The fresh listing is inspected in the refreshModels reply (inspectDownloadStatus).
    refreshModels();
}

void LlamaServer::inspectDownloadStatus()
{
    if (!m_downloading)
        return;
    for (const ServerModel &model : m_models) {
        if (model.id != m_downloadingModel)
            continue;
        if (isModelRunning(model.status)
            || model.status == QLatin1String("downloaded")
            || model.status == QLatin1String("unloaded")) {
            finishDownload(true, Tr::tr("The download is complete."));
        } else if (model.status == QLatin1String("incomplete")) {
            finishDownload(false,
                           Tr::tr("The download failed; the partial files stay, so "
                                  "downloading again resumes it."));
        }
        return; // still downloading
    }
    // Not listed yet: still running, keep watching.
}

void LlamaServer::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus)
    if (m_stopping)
        return; // stop() reports Stopped itself
    if (m_state == State::Stopped)
        return;
    const QString tail = m_outputTail.trimmed();
    const int keep = 400;
    QString message = Tr::tr("The server exited unexpectedly (code %1)%2.")
                          .arg(exitCode)
                          .arg(tail.isEmpty() ? QString() : (QLatin1Char(' ') + tail.right(keep)));
    setState(State::Failed, message);
}

void LlamaServer::appendOutput(const QString &text)
{
    if (text.isEmpty())
        return;
    m_outputTail += text;
    // A long-running server produces megabytes of logs; keep only what the
    // error messages can show.
    constexpr int kMaxTail = 100 * 1024;
    if (m_outputTail.size() > kMaxTail)
        m_outputTail = m_outputTail.right(kMaxTail);
    emit outputAppended(text);
}

void LlamaServer::resetOutput()
{
    m_outputTail.clear();
    emit outputReset();
}

LlamaServer &server()
{
    static LlamaServer theServer;
    return theServer;
}

} // namespace LlamaCpp
