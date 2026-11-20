#pragma once

#include <QObject>
#include <QUrl>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace LlamaCpp {

enum class InstallScriptVerdict
{
    Ok,
    HttpError,
    Redirected,
    WrongUri,
    TooLarge
};

QString installScriptUrl();

// The integrity gate of the install script download (pinned URL, direct 200,
// at most 1 MiB).
InstallScriptVerdict verifyInstallScript(const QUrl &finalUrl,
                                         int statusCode,
                                         qint64 contentLength);

QString describeInstallScriptVerdict(InstallScriptVerdict verdict);

// The llama.cpp runtime: binary discovery, version probe, install, update
// check.
class LlamaRuntime : public QObject
{
    Q_OBJECT
public:
    explicit LlamaRuntime(QObject *parent = nullptr);

    static QString managedDirectory();

    // The usable binary: the configured one, then the managed install, then
    // the PATH; empty when none was found.
    QString resolvedPath() const { return m_binaryPath; }
    bool isManaged() const { return m_managed; }
    QString versionTag() const { return m_versionTag; }

    void refresh();
    void install();
    bool isInstalling() const { return m_installing; }
    void checkForUpdates();
    bool isCheckingUpdates() const { return m_checkingUpdates; }

signals:
    void resolvedPathChanged(const QString &path);
    void versionChanged(const QString &versionTag);
    void installStarted();
    void installProgress(const QString &status);
    void installFinished(bool success, const QString &message);
    void updateCheckFinished(bool hasUpdate, const QString &message);

private:
    void runVersionProbe();
    void onInstallOutput(const QString &output);

    QString m_binaryPath;
    bool m_managed = false;
    QString m_versionTag;
    bool m_installing = false;
    bool m_checkingUpdates = false;

    QProcess *m_versionProcess = nullptr;
    QProcess *m_installProcess = nullptr;
    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_updateReply = nullptr;
};

LlamaRuntime &runtime();

} // namespace LlamaCpp
