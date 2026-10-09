#include "windows_sandbox.h"

#include "llamatr.h"
#include "mxc.h"
#include "tool_utils.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QOperatingSystemVersion>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>

namespace LlamaCpp::Tools {
namespace {

constexpr int kProbeTimeoutMs = 15'000;
constexpr int kLiveProbeTimeoutMs = 30'000;
// The base64 configuration is the single argument of wxc-exec; keep it
// well below CreateProcessW's 32 767 WCHAR command line limit (the same
// bound the MXC SDK itself applies).
constexpr int kMaxConfigBase64Chars = 24'000;
// MXC's process container tier needs the container infrastructure of
// Windows 11 24H2; older builds have no backend to run on.
constexpr quint32 kMinWindowsBuild = 26100;

/*! Locates the wxc-exec executable: the LLAMA_WXC_EXEC environment
    variable (mirroring LLAMA_SHELL_STUB), then the pinned release
    downloaded from the tools settings page. */
QString findWxcExec()
{
    const Utils::FilePath resolved = Mxc::resolvedPath();
    return resolved.exists() ? resolved.toUserOutput() : QString();
}

QString installHint()
{
    return Tr::tr(
        "Download it from the Llama.cpp Chat tools settings page (a pinned, "
        "checksum-verified Microsoft MXC release); LLAMA_WXC_EXEC can point "
        "to a wxc-exec.exe. Alternatively uncheck 'Sandbox commands' in the "
        "Llama settings to run commands without a sandbox.");
}

struct WxcStatus
{
    bool available = false;
    QString error;
};

/*! Checks that the MXC container can run on this host. The successful
    result is cached for the session (keyed by the executable, as
    LLAMA_WXC_EXEC may point elsewhere); a failure re-probes on every call,
    so a runtime installed while the editor is running takes effect on the
    next command. */
WxcStatus probeWxc(const QString &exe)
{
    static QHash<QString, bool> s_probed;
    if (s_probed.value(exe))
        return { true, {} };

    QProcess process;
    process.start(exe, { QStringLiteral("--probe") });
    if (!process.waitForFinished(kProbeTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        return { false,
                 Tr::tr("The Windows sandbox backend (wxc-exec) did not answer its "
                        "capability probe in time. %1")
                         .arg(installHint()) };
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return { false,
                 Tr::tr("The Windows sandbox backend (wxc-exec) reported the MXC "
                        "container as unavailable (exit %1): %2 %3")
                         .arg(process.exitCode())
                         .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed())
                         .arg(installHint()) };
    }
    s_probed.insert(exe, true);
    return { true, {} };
}

/*! Case- and separator-insensitive spelling of a path for the nested-root
    checks (deterministic on every host, the paths are Windows paths). */
QString normalizedPath(const QString &path)
{
    return QDir::cleanPath(path).replace(QLatin1Char('\\'), QLatin1Char('/')).toLower();
}

bool pathCoversInsensitive(const QString &prefix, const QString &path)
{
    const QString folded = normalizedPath(path);
    const QString base = normalizedPath(prefix);
    return folded == base || folded.startsWith(base + QLatin1Char('/'));
}

/*! Drops every path that is covered by another one (a granted root covers
    its subpaths, so the inner grant is redundant). */
QStringList withoutNested(const QStringList &paths)
{
    QStringList result;
    for (int i = 0; i < paths.size(); ++i) {
        bool covered = false;
        for (int j = 0; j < paths.size(); ++j) {
            if (i != j && pathCoversInsensitive(paths.at(j), paths.at(i))) {
                covered = true;
                break;
            }
        }
        if (!covered)
            result << paths.at(i);
    }
    return result;
}

/*! Canonical spelling of a path for the configuration (symlinks resolved,
    the same rule the Seatbelt profile paths follow; a missing path is only
    cleaned). */
QString canonicalGrantPath(const QString &path)
{
#if defined(Q_OS_WIN)
    const QString canonical = QFileInfo(path).canonicalFilePath();
    return canonical.isEmpty() ? QDir::cleanPath(path) : canonical;
#else
    // The grant paths are Windows paths; only the real container host
    // canonicalizes them (this keeps the wrapper construction testable on
    // any platform with the Windows spellings intact).
    return path;
#endif
}

/*! Directory part of a Windows path (the shell is always a Windows path,
    so this is a pure string operation - QFileInfo would apply the host
    platform's path rules and break the cross-platform tests). */
QString windowsDirectoryOf(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('\\'));
    return slash > 0 ? path.left(slash) : path;
}

QJsonObject buildConfig(const QString &cwd,
                        const QProcessEnvironment &env,
                        const QString &commandLine,
                        int timeoutMs,
                        const QString &shellPath)
{
    const QString canonicalCwd = canonicalGrantPath(cwd);
    // Writable: the working directory and the temporary location (the same
    // coverage as the other platforms).
    QStringList readwrite = { canonicalCwd, canonicalGrantPath(QDir::tempPath()) };
    readwrite = withoutNested(readwrite);

    // Read-only: the shell's own tree (Git for Windows: bash.dll,
    // msys-2.0.dll, the usr/ hierarchy) and the Windows system directory.
    // Everything else does not exist in the container - in particular the
    // credential locations need no explicit deny, unlike the deny-list
    // profiles of the other platforms.
    QStringList readonly;
    if (!shellPath.isEmpty()) {
        // Grant the shell's installation root (the parent of the directory
        // holding the executable, e.g. C:\git for C:\git\bin\bash.exe) so
        // the whole tree the shell may load from is visible.
        const QString shellDir = canonicalGrantPath(
            windowsDirectoryOf(windowsDirectoryOf(shellPath)));
        readonly << shellDir;
    }
    const QString systemRoot = env.value(QStringLiteral("SystemRoot"),
                                         env.value(QStringLiteral("WINDIR")));
    if (!systemRoot.isEmpty())
        readonly << canonicalGrantPath(systemRoot);
    readonly = withoutNested(readonly);

    // The container starts with no host environment; pass the caller's
    // environment explicitly. MXC's validator requires LOCALAPPDATA; point
    // it at the workdir when the caller has none.
    QProcessEnvironment workloadEnv = env;
    if (!workloadEnv.contains(QStringLiteral("LOCALAPPDATA")))
        workloadEnv.insert(QStringLiteral("LOCALAPPDATA"), canonicalCwd);
    QStringList envList; // keys() is sorted
    for (const QString &key : workloadEnv.keys())
        envList << key + QLatin1Char('=') + workloadEnv.value(key);

    QJsonObject config;
    config[QStringLiteral("version")] = QStringLiteral("0.8.0-alpha");
    const QString containerId =
        QString::fromLatin1("llama-cpp-")
        + QUuid::createUuid().toString(QUuid::WithoutBraces);
    config[QStringLiteral("containerId")] = containerId;
    config[QStringLiteral("containment")] = QStringLiteral("processcontainer");
    QJsonObject lifecycle;
    lifecycle[QStringLiteral("destroyOnExit")] = true;
    lifecycle[QStringLiteral("preservePolicy")] = false;
    config[QStringLiteral("lifecycle")] = lifecycle;
    QJsonObject process;
    process[QStringLiteral("commandLine")] = commandLine;
    process[QStringLiteral("cwd")] = canonicalCwd;
    process[QStringLiteral("env")] = QJsonArray::fromStringList(envList);
    process[QStringLiteral("timeout")] = timeoutMs;
    config[QStringLiteral("process")] = process;
    QJsonObject filesystem;
    filesystem[QStringLiteral("readwritePaths")] = QJsonArray::fromStringList(readwrite);
    filesystem[QStringLiteral("readonlyPaths")] = QJsonArray::fromStringList(readonly);
    filesystem[QStringLiteral("deniedPaths")] = QJsonArray();
    config[QStringLiteral("filesystem")] = filesystem;
    // No network access, like (deny network*) on macOS: nothing is allowed
    // and no network capability is granted. Web access stays available
    // through the webfetch/websearch tools, which run outside the sandbox.
    QJsonObject network;
    network[QStringLiteral("defaultPolicy")] = QStringLiteral("deny");
    network[QStringLiteral("allowLocalNetwork")] = false;
    network[QStringLiteral("allowedHosts")] = QJsonArray();
    network[QStringLiteral("blockedHosts")] = QJsonArray();
    network[QStringLiteral("enforcementMode")] = QStringLiteral("capabilities");
    config[QStringLiteral("network")] = network;
    QJsonObject containerUi;
    containerUi[QStringLiteral("isolation")] = QStringLiteral("container");
    containerUi[QStringLiteral("desktopSystemControl")] = false;
    containerUi[QStringLiteral("systemSettings")] = QStringLiteral("none");
    containerUi[QStringLiteral("ime")] = false;
    QJsonObject processContainer;
    processContainer[QStringLiteral("leastPrivilege")] = false;
    processContainer[QStringLiteral("capabilities")] = QJsonArray();
    processContainer[QStringLiteral("ui")] = containerUi;
    config[QStringLiteral("processContainer")] = processContainer;
    QJsonObject ui;
    ui[QStringLiteral("disable")] = false;
    ui[QStringLiteral("clipboard")] = QStringLiteral("none");
    ui[QStringLiteral("injection")] = false;
    config[QStringLiteral("ui")] = ui;
    // The DACL-mutation fallback tier (older Windows) needs an elevated
    // one-time host preparation; it is deliberately not used.
    QJsonObject fallback;
    fallback[QStringLiteral("allowDaclMutation")] = false;
    config[QStringLiteral("fallback")] = fallback;
    return config;
}

#if defined(Q_OS_WIN)
bool windowsBuildOk()
{
    return QOperatingSystemVersion::current().buildNumber() >= kMinWindowsBuild;
}
quint32 windowsBuildNumber()
{
    return QOperatingSystemVersion::current().buildNumber();
}

// Runs one real sandboxed command and checks the isolation with positive
// and negative controls: a write inside the workdir must work, a read of
// and a write to a file outside the granted paths must fail. The canary
// lives in the home directory, which the container cannot see (a temp
// location would be granted). The successful result is cached for the
// session; a failure re-probes on the next call, like probeWxc(). The
// probe is synchronous (bounded by the timeouts) and runs on the caller's
// thread - the first sandboxed command pays for it.
bool liveProbeOk(const QString &exe, const QString &shellPath)
{
    static QHash<QString, bool> s_result;
    if (s_result.value(exe))
        return true;

    bool ok = false;
    QTemporaryDir workdir;
    QDir homeProbe(QDir::homePath() + QStringLiteral("/.llama-mxc-probe-")
                             + QString::number(QCoreApplication::applicationPid()));
    // Leftovers of a probe interrupted by a crash; the probe re-creates
    // everything it needs.
    homeProbe.removeRecursively();
    if (workdir.isValid() && !shellPath.isEmpty() && homeProbe.mkpath(QStringLiteral("outside"))) {
        const QString canary = homeProbe.path() + QStringLiteral("/outside/secret.txt");
        {
            QFile file(canary);
            if (file.open(QIODevice::WriteOnly))
                file.write("llama-mxc-canary");
        }
        const QString outsideWrite
            = homeProbe.path() + QStringLiteral("/outside/outside-write.txt");
        const QString probeCommand = QStringLiteral("touch canary-ok; ")
                                   + QStringLiteral("cat '") + canary
                                   + QStringLiteral("' > canary-leak 2>/dev/null; ")
                                   + QStringLiteral("echo bad > '") + outsideWrite
                                   + QStringLiteral("' 2>/dev/null; ")
                                   + QStringLiteral("echo LLAMA_MXC_LIVE_PROBE_OK");
        const QString commandLine
            = quoteWindowsArgument(shellPath) + QLatin1Char(' ')
            + QStringLiteral("-c ") + quoteWindowsArgument(probeCommand);
        const QJsonObject config = buildConfig(workdir.path(),
                                               QProcessEnvironment::systemEnvironment(),
                                               commandLine,
                                               kLiveProbeTimeoutMs,
                                               shellPath);
        const QByteArray encoded
            = QJsonDocument(config).toJson(QJsonDocument::Compact).toBase64();
        if (encoded.size() <= kMaxConfigBase64Chars) {
            QProcess process;
            process.start(exe,
                          { QStringLiteral("--config-base64"),
                            QString::fromLatin1(encoded) });
            if (process.waitForFinished(kLiveProbeTimeoutMs + 5000)) {
                const QString output
                    = QString::fromLocal8Bit(process.readAllStandardOutput())
                    + QString::fromLocal8Bit(process.readAllStandardError());
                // The `> canary-leak` redirect creates the file even when
                // cat fails, so only content is a leak.
                QFile leakFile(workdir.path() + QStringLiteral("/canary-leak"));
                const bool leaked
                    = leakFile.open(QIODevice::ReadOnly)
                    && leakFile.readAll().contains("llama-mxc-canary");
                ok = process.exitStatus() == QProcess::NormalExit
                   && process.exitCode() == 0
                   && output.contains(QStringLiteral("LLAMA_MXC_LIVE_PROBE_OK"))
                   && QFileInfo::exists(workdir.path() + QStringLiteral("/canary-ok"))
                   && !leaked
                   && !QFileInfo::exists(outsideWrite);
            }
        }
        homeProbe.removeRecursively();
    }
    if (ok)
        s_result.insert(exe, true);
    return ok;
}
#else
// The build check and the live probe only make sense against the real
// container; the wrapper construction stays testable on any platform.
bool windowsBuildOk()
{
    return true;
}
quint32 windowsBuildNumber()
{
    return 0;
}
bool liveProbeOk(const QString &, const QString &)
{
    return true;
}
#endif

} // namespace

QString quoteWindowsArgument(const QString &argument)
{
    // No quoting needed for an argument without whitespace or quotes.
    bool needsQuoting = argument.isEmpty();
    for (const QChar ch : argument) {
        if (ch.isSpace() || ch == QLatin1Char('"')) {
            needsQuoting = true;
            break;
        }
    }
    if (!needsQuoting)
        return argument;

    // MSVCRT rules: wrap in quotes; backslashes not followed by a quote
    // are literal, a run before a quote is doubled (an odd run before the
    // closing quote would escape it) and an embedded quote gets one extra
    // backslash.
    QString result(1, QLatin1Char('"'));
    int backslashes = 0;
    for (const QChar ch : argument) {
        if (ch == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (ch == QLatin1Char('"')) {
            result.append(QString(backslashes * 2 + 1, QLatin1Char('\\')));
            result.append(QLatin1Char('"'));
            backslashes = 0;
            continue;
        }
        result.append(QString(backslashes, QLatin1Char('\\')));
        backslashes = 0;
        result.append(ch);
    }
    result.append(QString(backslashes * 2, QLatin1Char('\\')));
    result.append(QLatin1Char('"'));
    return result;
}

WindowsSandboxSpec windowsSandboxSpec(const QString &cwd,
                                      const QProcessEnvironment &env,
                                      const QString &commandLine,
                                      int timeoutMs,
                                      const QString &shellPath)
{
    WindowsSandboxSpec spec;

    if (!windowsBuildOk()) {
        spec.error = Tr::tr("The Windows sandbox (Microsoft MXC) needs Windows 11 "
                            "24H2 (build %1) or newer; this system is build %2. Uncheck "
                            "'Sandbox commands' in the Llama settings to run commands "
                            "without a sandbox.")
                          .arg(kMinWindowsBuild)
                          .arg(windowsBuildNumber());
        return spec;
    }

    const QString exe = findWxcExec();
    if (exe.isEmpty()) {
        spec.error = Tr::tr("wxc-exec.exe (the Microsoft MXC Windows sandbox runtime) was "
                            "not found. %1")
                          .arg(installHint());
        return spec;
    }

    const WxcStatus status = probeWxc(exe);
    if (!status.available) {
        spec.error = status.error;
        return spec;
    }

    if (!liveProbeOk(exe, shellPath)) {
        spec.error = Tr::tr("The Windows sandbox (Microsoft MXC) failed its live "
                            "isolation check: a sandboxed command could not be verified "
                            "to be confined. %1")
                          .arg(installHint());
        return spec;
    }

    const QJsonObject config = buildConfig(cwd, env, commandLine, timeoutMs, shellPath);
    const QByteArray encoded
        = QJsonDocument(config).toJson(QJsonDocument::Compact).toBase64();
    if (encoded.size() > kMaxConfigBase64Chars) {
        spec.error = Tr::tr("The Windows sandbox command line is too long (%1 of %2 "
                            "characters); the process environment is probably oversized. "
                            "Uncheck 'Sandbox commands' in the Llama settings to run "
                            "commands without a sandbox.")
                          .arg(encoded.size())
                          .arg(kMaxConfigBase64Chars);
        return spec;
    }

    spec.program = exe;
    spec.arguments = { QStringLiteral("--config-base64"), QString::fromLatin1(encoded) };
    return spec;
}

} // namespace LlamaCpp::Tools
