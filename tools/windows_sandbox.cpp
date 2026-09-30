#include "windows_sandbox.h"

#include "llamatr.h"
#include "tool_utils.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>

namespace LlamaCpp::Tools {
namespace {

constexpr int kProbeTimeoutMs = 15'000;
constexpr int kGrantTimeoutMs = 30'000;
// The aboutToQuit revoke is best effort; do not keep the editor's
// shutdown waiting on a hung srt-win for the full grant timeout.
constexpr int kRevokeOnExitTimeoutMs = 5'000;
// CreateProcessW's lpCommandLine is capped at 32 767 WCHARs; leave
// headroom for the quote overhead the process launch adds.
constexpr int kMaxCommandLineChars = 30'000;

struct RunResult
{
    int exitCode = -1;
    QString stdoutText;
    QString stderrText;
};

/*! Locates the srt-win executable: the LLAMA_SRT_WIN environment
    variable (mirroring LLAMA_SHELL_STUB), then the PATH. */
QString findSrtWin()
{
    const QString overridePath = qEnvironmentVariable("LLAMA_SRT_WIN");
    if (!overridePath.isEmpty() && QFile::exists(overridePath))
        return overridePath;
    return QStandardPaths::findExecutable(QStringLiteral("srt-win"));
}

RunResult runSrtWin(const QString &exe,
                    const QStringList &args,
                    const QByteArray &stdinData,
                    int timeoutMs)
{
    RunResult result;
    QProcess process;
    process.setProgram(exe);
    process.setArguments(args);
    process.start();
    if (!stdinData.isEmpty()) {
        process.write(stdinData);
        process.closeWriteChannel();
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        return result;
    }
    if (process.exitStatus() == QProcess::NormalExit)
        result.exitCode = process.exitCode();
    result.stdoutText = QString::fromLocal8Bit(process.readAllStandardOutput());
    result.stderrText = QString::fromLocal8Bit(process.readAllStandardError());
    return result;
}

struct SrtWinStatus
{
    bool available = false; // binary found and the sandbox user provisioned
    QString sid;
    QString error;
};

QString installHint()
{
    return Tr::tr(
        "Install it once with 'npx @anthropic-ai/sandbox-runtime windows-install' "
        "(one UAC prompt); no logout is needed, the network fence keys on the "
        "dedicated sandbox user's SID. Alternatively uncheck 'Sandbox commands' "
        "in the Llama settings to run commands without a sandbox.");
}

/*! Checks that the sandbox user is provisioned and reads its SID. The
    successful result is cached for the session (the SID is stable); a
    failure re-probes on every call, so an install performed while the
    editor is running takes effect on the next command. */
SrtWinStatus probeStatus(const QString &exe)
{
    // Keyed by the executable: the SID belongs to whichever srt-win
    // answered, and LLAMA_SRT_WIN may point elsewhere.
    static QHash<QString, QString> s_cachedSid;
    if (const QString &sid = s_cachedSid.value(exe); !sid.isEmpty())
        return { true, sid, {} };

    SrtWinStatus status;
    const RunResult r = runSrtWin(exe,
                                  { QStringLiteral("user"), QStringLiteral("status") },
                                  {},
                                  kProbeTimeoutMs);
    if (r.exitCode != 0) {
        status.error = Tr::tr("The Windows sandbox backend (srt-win) could not be "
                              "queried (exit %1): %2 %3")
                            .arg(r.exitCode)
                            .arg(r.stderrText.trimmed())
                            .arg(installHint());
        return status;
    }
    const QJsonObject root = QJsonDocument::fromJson(r.stdoutText.toUtf8()).object();
    const QJsonObject user = root.value("user").toObject();
    if (!user.value("exists").toBool() || !root.value("cred_present").toBool()) {
        status.error = Tr::tr("The Windows sandbox user is not provisioned. %1")
                            .arg(installHint());
        return status;
    }
    status.sid = user.value("sid").toString();
    if (status.sid.isEmpty()) {
        status.error = Tr::tr("The Windows sandbox backend (srt-win) did not report "
                              "the sandbox user's SID. %1")
                            .arg(installHint());
        return status;
    }
    s_cachedSid.insert(exe, status.sid);
    status.available = true;
    return status;
}

// Paths that carry a sandbox-user ACE for this session, as "read:<path>"
// / "write:<path>" entries, keyed by the srt-win executable. Grants are
// idempotent and refcounted inside srt-win, so the cache only avoids
// re-spawning; a miss re-grants the full (growing) set.
QSet<QString> &grantedPaths(const QString &exe)
{
    static QHash<QString, QSet<QString>> s_granted;
    return s_granted[exe];
}

void revokeAll(const QString &exe, const QString &sid, int timeoutMs)
{
    if (exe.isEmpty() || sid.isEmpty())
        return;
    runSrtWin(exe,
              { QStringLiteral("acl"),
                QStringLiteral("revoke"),
                QStringLiteral("--holder-pid"),
                QString::number(QCoreApplication::applicationPid()),
                QStringLiteral("--sandbox-user-sid"),
                sid,
                QStringLiteral("--json") },
              {},
              timeoutMs);
    grantedPaths(exe).clear();
}

/*! Registers a one-time hook (aboutToQuit) that releases this session's
    ACL grants (best effort; srt-win's crash recovery also prunes dead
    holders). */
void registerRevokeOnExit(const QString &exe, const QString &sid)
{
    static bool s_registered = false;
    static QString s_exe;
    static QString s_sid;
    if (!s_registered) {
        s_registered = true;
        s_exe = exe;
        s_sid = sid;
        QObject::connect(qApp,
                         &QCoreApplication::aboutToQuit,
                         [] { revokeAll(s_exe, s_sid, kRevokeOnExitTimeoutMs); });
    }
}

/*! Ensures the sandbox user can read the home directory and write \a cwd
    and the temporary directory (an additive (OI)(CI) ALLOW ACE per path,
    released on exit). On failure whatever was granted is revoked again. */
bool ensureGrants(const QString &exe, const QString &sid, const QString &cwd, QString *error)
{
    const QStringList read = { QDir::homePath() };
    const QStringList write = { cwd, QDir::tempPath() };
    QSet<QString> wanted;
    for (const QString &path : read)
        wanted.insert(QStringLiteral("read:") + path);
    for (const QString &path : write)
        wanted.insert(QStringLiteral("write:") + path);
    // Subset check (QSet::contains(QSet)): all wanted paths already
    // granted in this session.
    if (grantedPaths(exe).contains(wanted))
        return true;

    registerRevokeOnExit(exe, sid);
    QJsonObject payload;
    payload["read"] = QJsonArray::fromStringList(read);
    payload["write"] = QJsonArray::fromStringList(write);
    const RunResult r = runSrtWin(
        exe,
        { QStringLiteral("acl"),
          QStringLiteral("grant"),
          QStringLiteral("--holder-pid"),
          QString::number(QCoreApplication::applicationPid()),
          QStringLiteral("--sandbox-user-sid"),
          sid },
        QJsonDocument(payload).toJson(QJsonDocument::Compact),
        kGrantTimeoutMs);
    if (r.exitCode != 0) {
        // Release whatever WAS granted so a failed command does not
        // leave new ACEs behind.
        revokeAll(exe, sid, kGrantTimeoutMs);
        *error = Tr::tr("The Windows sandbox could not grant the sandbox user "
                        "access to the working directory (exit %1): %2")
                      .arg(r.exitCode)
                      .arg(r.stderrText.trimmed());
        return false;
    }
    grantedPaths(exe) += wanted;
    return true;
}

} // namespace

WindowsSandboxSpec windowsSandboxSpec(const QString &cwd, const QProcessEnvironment &env)
{
    WindowsSandboxSpec spec;

    const QString exe = findSrtWin();
    if (exe.isEmpty()) {
        spec.error = Tr::tr("srt-win (the @anthropic-ai/sandbox-runtime Windows "
                            "sandbox backend) was not found on the PATH. %1 "
                            "LLAMA_SRT_WIN can point to the executable.")
                          .arg(installHint());
        return spec;
    }

    const SrtWinStatus status = probeStatus(exe);
    if (!status.available) {
        spec.error = status.error;
        return spec;
    }

    QString grantError;
    if (!ensureGrants(exe, status.sid, cwd, &grantError)) {
        spec.error = grantError;
        return spec;
    }

    QStringList args = { QStringLiteral("exec"), QStringLiteral("--quiet") };
    // Deny the credential locations for this single exec, both for
    // reading and for writing (the same coverage as the macOS profile).
    // Skipped when the working directory is inside one of them, the
    // same way the other platforms do. A trailing backslash marks a
    // directory target: srt-win materializes a missing deny target as
    // an empty *directory* placeholder only for directory targets.
    for (const SecretReadPath &secret : secretReadPaths()) {
        if (pathCovers(secret.path, cwd))
            continue;
        const QString target =
            secret.isFile ? secret.path : secret.path + QLatin1Char('\\');
        args << QStringLiteral("--deny-read") << target
             << QStringLiteral("--deny-write") << target;
    }
    // The sandboxed child starts with the sandbox user's own profile
    // environment; overlay the caller's environment explicitly (PATH,
    // PATHEXT, toolchain variables, ...).
    for (const QString &key : env.keys())
        args << QStringLiteral("--env") << key + QLatin1Char('=') + env.value(key);
    args << QStringLiteral("--");

    // Approximation: the CreateProcessW limit is counted in WCHARs, so
    // non-BMP characters (surrogate pairs) take two units per QString
    // char; the headroom below the 32 767 limit absorbs that.
    int length = 0;
    for (const QString &arg : args)
        length += arg.size() + 3;
    if (length > kMaxCommandLineChars) {
        spec.error = Tr::tr("The Windows sandbox command line is too long (%1 of "
                            "%2 characters); the process environment is probably "
                            "oversized. Uncheck 'Sandbox commands' in the Llama "
                            "settings to run commands without a sandbox.")
                          .arg(length)
                          .arg(kMaxCommandLineChars);
        return spec;
    }

    spec.program = exe;
    spec.arguments = std::move(args);
    return spec;
}

} // namespace LlamaCpp::Tools
