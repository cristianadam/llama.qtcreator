#include "llamaruntime.h"

#include "llamasettings.h"
#include "llamatr.h"
#include "llamaserver.h" // parseVersionTag

#include <utils/hostosinfo.h>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <QLoggingCategory>

Q_LOGGING_CATEGORY(llamaRuntimeLog, "llama.cpp.runtime", QtInfoMsg)

using namespace Utils;

namespace LlamaCpp {

QString installScriptUrl()
{
    return HostOsInfo::isWindowsHost()
               ? QStringLiteral("https://llama.app/install.ps1")
               : QStringLiteral("https://llama.app/install.sh");
}

InstallScriptVerdict verifyInstallScript(const QUrl &finalUrl,
                                         int statusCode,
                                         qint64 contentLength)
{
    if (statusCode >= 300 && statusCode < 400)
        return InstallScriptVerdict::Redirected;
    if (statusCode != 200)
        return InstallScriptVerdict::HttpError;
    if (finalUrl != QUrl(installScriptUrl()))
        return InstallScriptVerdict::WrongUri;
    if (contentLength > 1024 * 1024)
        return InstallScriptVerdict::TooLarge;
    return InstallScriptVerdict::Ok;
}

QString describeInstallScriptVerdict(InstallScriptVerdict verdict)
{
    switch (verdict) {
    case InstallScriptVerdict::HttpError:
        return Tr::tr("The install script server returned a non-200 response.");
    case InstallScriptVerdict::Redirected:
        return Tr::tr("The install script download was redirected; redirects are refused.");
    case InstallScriptVerdict::WrongUri:
        return Tr::tr("The install script did not come from %1.").arg(installScriptUrl());
    case InstallScriptVerdict::TooLarge:
        return Tr::tr("The install script is larger than expected; the download was refused.");
    case InstallScriptVerdict::Ok:
        break;
    }
    return {};
}

QString LlamaRuntime::managedDirectory()
{
    if (HostOsInfo::isWindowsHost())
    {
        // install.ps1 moves the binary into the WindowsApps directory (and
        // removes the %LOCALAPPDATA%\llama-app scratch dir it downloads to).
        // The script uses $env:LOCALAPPDATA itself, so do the same (the
        // QStandardPaths variant appends the organization and app name).
        QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
        if (localAppData.isEmpty())
            localAppData = QDir(QDir::homePath()).filePath(QStringLiteral("AppData/Local"));
        return QDir(localAppData).filePath(QStringLiteral("Microsoft/WindowsApps"));
    }
    // install.sh keeps the binary in ~/.llama-app (and copies it to
    // ~/.local/bin).
    return QDir(QDir::homePath()).filePath(QStringLiteral(".llama-app"));
}

static QString binaryName()
{
    return HostOsInfo::isWindowsHost() ? QStringLiteral("llama.exe") : QStringLiteral("llama");
}

LlamaRuntime::LlamaRuntime(QObject *parent)
    : QObject(parent)
    , m_network(new QNetworkAccessManager(this))
{
    refresh();
}

void LlamaRuntime::refresh()
{
    const QString oldPath = m_binaryPath;
    m_binaryPath.clear();
    m_managed = false;

    const QString configured = settings().llamaPath().isEmpty()
                                   ? QString()
                                   : settings().llamaPath().toUserOutput();
    if (!configured.isEmpty() && QFileInfo::exists(configured))
        m_binaryPath = configured;

    if (m_binaryPath.isEmpty()) {
        const QString managed = QDir(managedDirectory()).filePath(binaryName());
        if (QFile::exists(managed)) {
            m_binaryPath = managed;
            m_managed = true;
        }
    }

    if (m_binaryPath.isEmpty()) {
        const QString found = QStandardPaths::findExecutable(QStringLiteral("llama"));
        if (!found.isEmpty())
            m_binaryPath = found;
        else
            m_binaryPath = QStandardPaths::findExecutable(QStringLiteral("llama-server"));
    }

    if (m_binaryPath != oldPath)
        emit resolvedPathChanged(m_binaryPath);
    runVersionProbe();
}

void LlamaRuntime::runVersionProbe()
{
    if (m_versionProcess) {
        m_versionProcess->deleteLater();
        m_versionProcess = nullptr;
    }
    const QString oldTag = m_versionTag;
    m_versionTag.clear();
    if (m_binaryPath.isEmpty()) {
        emit versionChanged(m_versionTag);
        return;
    }
    auto *process = new QProcess(this);
    m_versionProcess = process;
    connect(process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, process, oldTag](int, QProcess::ExitStatus) {
                if (process != m_versionProcess)
                    return; // a newer probe superseded this one
                m_versionProcess = nullptr;
                const QString output = QString::fromLocal8Bit(
                    process->readAllStandardError() + process->readAllStandardOutput());
                m_versionTag = parseVersionTag(output);
                if (m_versionTag != oldTag)
                    emit versionChanged(m_versionTag);
            });
    process->start(m_binaryPath, {QStringLiteral("--version")});
}

void LlamaRuntime::install()
{
    if (m_installing)
        return;
    m_installing = true;
    emit installStarted();
    emit installProgress(Tr::tr("Downloading the install script..."));

    const QUrl url(installScriptUrl());
    QNetworkRequest request(url);
    // Safe redirects only; the integrity gate checks the final URL.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        const int statusCode
            = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const qint64 contentLength = reply->rawHeader("Content-Length").toLongLong();
        const InstallScriptVerdict verdict
            = verifyInstallScript(reply->url(), statusCode, contentLength);
        if (verdict != InstallScriptVerdict::Ok) {
            m_installing = false;
            qCWarning(llamaRuntimeLog)
                << "install script refused:" << describeInstallScriptVerdict(verdict);
            emit installFinished(false, describeInstallScriptVerdict(verdict));
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.size() > 1024 * 1024) {
            m_installing = false;
            emit installFinished(false,
                                 describeInstallScriptVerdict(InstallScriptVerdict::TooLarge));
            return;
        }
        // The script runs arbitrary code, so record what actually ran.
        qCInfo(llamaRuntimeLog)
            << "install script sha256:"
            << QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex();

        const bool isWindows = HostOsInfo::isWindowsHost();
        const QString scriptPath = QDir(
                                       QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                       .filePath(isWindows ? QStringLiteral("llama-install.ps1")
                                                           : QStringLiteral("llama-install.sh"));
        QFile file(scriptPath);
        if (!file.open(QFile::WriteOnly) || file.write(body) != body.size()) {
            m_installing = false;
            emit installFinished(
                false,
                Tr::tr("The install script could not be written to a temporary file."));
            return;
        }
        file.close();

        emit installProgress(Tr::tr("Installing llama.cpp (this can take a minute)..."));
        if (m_installProcess)
            m_installProcess->deleteLater();
        auto *process = new QProcess(this);
        m_installProcess = process;
        connect(process,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                [this, process, scriptPath](int code, QProcess::ExitStatus) {
                    if (process != m_installProcess)
                        return;
                    m_installProcess = nullptr;
                    QFile::remove(scriptPath);
                    m_installing = false;
                    if (code == 0) {
                        refresh();
                        emit installFinished(true, Tr::tr("llama.cpp installed."));
                    } else {
                        const QString output = QString::fromLocal8Bit(
                            process->readAllStandardError()
                            + process->readAllStandardOutput());
                        qCWarning(llamaRuntimeLog) << "install script failed:" << output;
                        emit installFinished(
                            false,
                            Tr::tr("The install script failed (exit code %1).").arg(code)
                                + QLatin1Char(' ') + output.trimmed().right(300));
                    }
                });
        connect(process, &QProcess::errorOccurred, this,
                [this, process, scriptPath](QProcess::ProcessError) {
                    if (process != m_installProcess)
                        return;
                    m_installProcess = nullptr;
                    QFile::remove(scriptPath);
                    m_installing = false;
                    emit installFinished(false, Tr::tr("The install script could not be run."));
                });
        connect(process, &QProcess::readyReadStandardOutput, this, [this, process] {
            if (process == m_installProcess)
                onInstallOutput(QString::fromLocal8Bit(process->readAllStandardOutput()));
        });
        connect(process, &QProcess::readyReadStandardError, this, [this, process] {
            if (process == m_installProcess)
                onInstallOutput(QString::fromLocal8Bit(process->readAllStandardError()));
        });
        if (isWindows)
            process->start(
                QStringLiteral("powershell.exe"),
                {QStringLiteral("-NoProfile"),
                 QStringLiteral("-ExecutionPolicy"),
                 QStringLiteral("Bypass"),
                 QStringLiteral("-File"),
                 scriptPath});
        else
            process->start(QStringLiteral("sh"), {scriptPath});
    });
}

void LlamaRuntime::onInstallOutput(const QString &output)
{
    const QStringList lines = output.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (!line.isEmpty()) {
            emit installProgress(line.left(160));
            return;
        }
    }
}

void LlamaRuntime::checkForUpdates()
{
    if (m_checkingUpdates || m_binaryPath.isEmpty())
        return;
    m_checkingUpdates = true;
    // The install channel's "latest" marker is what the install scripts use,
    // so it is the only meaningful update baseline.
    QNetworkRequest request(QUrl(QStringLiteral(
        "https://huggingface.co/buckets/ggml-org/install.sh/resolve/latest")));
    QNetworkReply *reply = m_network->get(request);
    m_updateReply = reply;
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        m_updateReply = nullptr;
        m_checkingUpdates = false;
        if (reply->error() != QNetworkReply::NoError) {
            emit updateCheckFinished(false, Tr::tr("The update check failed (network)."));
            return;
        }
        const QString text = QString::fromUtf8(reply->readAll()).trimmed();
        static const QRegularExpression re(QStringLiteral("b(\\d+)"));
        const auto match = re.match(text);
        if (!match.hasMatch()) {
            emit updateCheckFinished(
                false, Tr::tr("The latest build could not be determined."));
            return;
        }
        const int latest = match.captured(1).toInt();
        const int installed
            = m_versionTag.startsWith(QLatin1Char('b')) ? m_versionTag.mid(1).toInt() : 0;
        if (latest > installed)
            emit updateCheckFinished(true,
                                     Tr::tr("A newer llama.cpp is available (b%1 → b%2).")
                                         .arg(installed)
                                         .arg(latest));
        else
            emit updateCheckFinished(false,
                                     Tr::tr("You have the latest llama.cpp (b%1).").arg(installed));
    });
}

LlamaRuntime &runtime()
{
    static LlamaRuntime theRuntime;
    return theRuntime;
}

} // namespace LlamaCpp
