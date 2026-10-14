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

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

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
// The canary content the live isolation probe hides in the home directory.
constexpr const char *kCanaryContent = "llama-mxc-canary";

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
    if (slash > 0)
        return path.left(slash);
    // A path without a directory (a shell at the drive root, C:\bash.exe):
    // the drive root itself, with the separator (a bare "C:" is a
    // drive-relative path, not a root).
    if (path.size() == 2 && path.at(1) == QLatin1Char(':'))
        return path + QLatin1Char('\\');
    return path;
}

#if defined(Q_OS_WIN)
// A drive root (C:\) or a UNC share root (\\server\share).
bool isFileSystemRoot(const QString &path)
{
    const QString folded = normalizedPath(path);
    if (folded.size() <= 3 && folded.size() >= 2 && folded.at(1) == QLatin1Char(':'))
        return true;
    return folded.startsWith(QLatin1String("//"))
           && folded.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts).size() <= 2;
}
#endif

QJsonObject buildConfig(const QString &cwd,
                        const QProcessEnvironment &env,
                        const QString &commandLine,
                        int timeoutMs,
                        const QString &shellRoot,
                        const QStringList &extraDenied = {})
{
    const QString canonicalCwd = canonicalGrantPath(cwd);
    // Writable: the working directory and the temporary location (the same
    // coverage as the other platforms), plus the extra writable directories
    // from the Sandbox settings page.
    // Note: unlike bubblewrap and Seatbelt, the protected locations inside
    // the working directory (.git, .qtcreator, *.user) stay writable here:
    // the MXC configuration has no documented semantics for a read-only or
    // denied path nested in a read-write grant (and deniedPaths would hide
    // .git from git entirely). The file tools still refuse those writes.
    QStringList readwrite = { canonicalCwd, canonicalGrantPath(QDir::tempPath()) };
    for (const QString &dir : sandboxWritableDirs())
        readwrite << canonicalGrantPath(dir);
    readwrite = withoutNested(readwrite);

    // Read-only: the shell's own tree (Git for Windows: bash.dll,
    // msys-2.0.dll, the usr/ hierarchy), the Windows system directory and
    // the extra read-only directories from the Sandbox settings page.
    // Everything else does not exist in the container - in particular the
    // credential locations need no explicit deny, unlike the deny-list
    // profiles of the other platforms.
    QStringList readonly;
    if (!shellRoot.isEmpty())
        readonly << canonicalGrantPath(shellRoot);
    const QString systemRoot = env.value(QStringLiteral("SystemRoot"),
                                         env.value(QStringLiteral("WINDIR")));
    if (!systemRoot.isEmpty())
        readonly << canonicalGrantPath(systemRoot);
    for (const QString &dir : sandboxReadOnlyDirs())
        readonly << canonicalGrantPath(dir);
    readonly = withoutNested(readonly);

    // Denied: the user-configured deny-read directories (Sandbox settings
    // page) plus the paths the caller denies explicitly (the live probe's
    // canary directory, so its negative controls keep working even when a
    // user writable grant covers the canary location). Only meaningful for a
    // location the container would otherwise see (inside a grant); a path
    // outside all grants does not exist in the container anyway, so denying
    // it is harmless.
    QStringList denied;
    for (const QString &dir : sandboxDenyReadDirs())
        denied << canonicalGrantPath(dir);
    for (const QString &dir : extraDenied)
        denied << canonicalGrantPath(dir);

    // The container starts with no host environment; pass the caller's
    // environment explicitly. MXC's validator requires LOCALAPPDATA; point
    // it at the workdir when the caller has none.
    // Credentials never reach the container (the caller strips them
    // already; this keeps the probe and any other caller honest).
    QProcessEnvironment workloadEnv = withoutSecretVariables(env);
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
    filesystem[QStringLiteral("deniedPaths")] = QJsonArray::fromStringList(denied);
    config[QStringLiteral("filesystem")] = filesystem;
    // No network access by default, like (deny network*) on macOS: nothing
    // is allowed and no network capability is granted. Web access stays
    // available through the webfetch/websearch tools, which run outside the
    // sandbox. The Sandbox settings page can lift the restriction.
    QJsonObject network;
    if (sandboxAllowsNetwork()) {
        network[QStringLiteral("defaultPolicy")] = QStringLiteral("allow");
        network[QStringLiteral("allowLocalNetwork")] = true;
    } else {
        network[QStringLiteral("defaultPolicy")] = QStringLiteral("deny");
        network[QStringLiteral("allowLocalNetwork")] = false;
    }
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

// The base64 configuration is the single argument of wxc-exec; both the
// live probe and the command refuse it past the command line bound.
QString encodedConfig(const QJsonObject &config)
{
    return QString::fromLatin1(
        QJsonDocument(config).toJson(QJsonDocument::Compact).toBase64());
}

enum class LiveProbeState
{
    Running, // the probe command is running (a fresh entry starts here)
    Ok,
    Failed,
};

struct LiveProbe
{
    QProcess *process = nullptr;
    LiveProbeState state = LiveProbeState::Running;
    QString failure; // a complete, actionable message when the state is Failed
};

// Keyed by the executable, as LLAMA_WXC_EXEC may point elsewhere.
QHash<QString, LiveProbe> &liveProbes()
{
    static QHash<QString, LiveProbe> probes;
    return probes;
}

#if defined(Q_OS_WIN)
quint32 windowsBuildNumber()
{
    // QOperatingSystemVersion has no build number accessor; RtlGetVersion
    // is the documented replacement for the sandboxed GetVersionExW.
    OSVERSIONINFOEXW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    using RtlGetVersionFn = LONG(WINAPI *)(POSVERSIONINFOEXW);
    static RtlGetVersionFn rtlGetVersion = [] {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        return ntdll ? reinterpret_cast<RtlGetVersionFn>(
                           GetProcAddress(ntdll, "RtlGetVersion"))
                     : nullptr;
    }();
    if (rtlGetVersion && rtlGetVersion(&info) == 0)
        return info.dwBuildNumber;
    return 0;
}
bool windowsBuildOk()
{
    return windowsBuildNumber() >= kMinWindowsBuild;
}

// The positive and negative controls of the live probe: a write inside the
// workdir must have worked, the canary must not have leaked, and the write
// outside the granted paths must have failed.
bool liveProbePassed(QProcess &process,
                     const QString &workdir,
                     const QString &outsideWrite)
{
    const QString output
        = QString::fromLocal8Bit(process.readAllStandardOutput())
        + QString::fromLocal8Bit(process.readAllStandardError());
    // The `> canary-leak` redirect creates the file even when cat fails, so
    // only content is a leak.
    QFile leakFile(workdir + QStringLiteral("/canary-leak"));
    const bool leaked
        = leakFile.open(QIODevice::ReadOnly) && leakFile.readAll().contains(kCanaryContent);
    return process.exitStatus() == QProcess::NormalExit
        && process.exitCode() == 0
        && output.contains(QStringLiteral("LLAMA_MXC_LIVE_PROBE_OK"))
        && QFileInfo::exists(workdir + QStringLiteral("/canary-ok"))
        && !leaked
        && !QFileInfo::exists(outsideWrite);
}

/*! Checks that the container really isolates, with one real sandboxed
    command and positive and negative controls: a write inside the workdir
    must work, a read of and a write to a canary file in the home directory
    must fail (the canary's directory is denied in the probe configuration,
    so this holds even when a user writable grant covers it; a temp location
    would be granted and cannot be used). The probe runs asynchronously - the first call starts it and reports
    Running, so the command that arrived is refused with a retry hint and
    the next ones pay nothing. The successful result is cached for the
    session (keyed by the executable); a failure re-probes on the next
    call, like probeWxc(). The probe uses the same environment the command
    gets, so an oversized environment fails it with the same "too long"
    reason the command would get. */
LiveProbeState liveProbeState(const QString &exe,
                              const QProcessEnvironment &env,
                              const QString &shellPath,
                              const QString &shellRoot)
{
    LiveProbe &probe = liveProbes()[exe];
    if (probe.state != LiveProbeState::Failed)
        return probe.state;
    probe = LiveProbe{}; // a failed probe is retried on the next call

    QTemporaryDir *workdir = new QTemporaryDir;
    QDir homeProbe(QDir::homePath() + QStringLiteral("/.llama-mxc-probe-")
                         + QString::number(QCoreApplication::applicationPid()));
    // Leftovers of a probe interrupted by a crash; the probe re-creates
    // everything it needs.
    homeProbe.removeRecursively();

    const auto cleanupAndFail = [&probe, &workdir, &homeProbe](const QString &message) {
        delete workdir;
        homeProbe.removeRecursively();
        probe.state = LiveProbeState::Failed;
        probe.failure = message;
    };

    if (!workdir->isValid() || shellPath.isEmpty()
        || !homeProbe.mkpath(QStringLiteral("outside"))) {
        cleanupAndFail(Tr::tr("The Windows sandbox could not set up its live isolation "
                              "probe (temporary directory or home canary). %1")
                          .arg(installHint()));
        return probe.state;
    }
    const QString canary = homeProbe.path() + QStringLiteral("/outside/secret.txt");
    // The probe directory is denied in the probe configuration: a user
    // writable grant covering the home directory would otherwise make the
    // negative controls (the canary read and the outside write) succeed and
    // fail the probe, and with it every sandboxed command.
    const QStringList probeDenied = { homeProbe.path() };
    {
        QFile file(canary);
        // The negative read control needs the canary to exist; without it
        // the probe would pass without verifying anything.
        if (!file.open(QIODevice::WriteOnly)
            || file.write(kCanaryContent) < 0) {
            cleanupAndFail(Tr::tr("The Windows sandbox could not create the canary file "
                                  "of its live isolation probe at %1. %2")
                              .arg(canary)
                              .arg(installHint()));
            return probe.state;
        }
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
    const QString encoded
        = encodedConfig(buildConfig(workdir->path(),
                                    env,
                                    commandLine,
                                    kLiveProbeTimeoutMs,
                                    shellRoot,
                                    probeDenied));
    if (encoded.size() > kMaxConfigBase64Chars) {
        cleanupAndFail(Tr::tr("The Windows sandbox configuration is too long (%1 of %2 "
                              "characters); the process environment is probably oversized. "
                              "Uncheck 'Sandbox commands' in the Llama settings to run "
                              "commands without a sandbox.")
                          .arg(encoded.size())
                          .arg(kMaxConfigBase64Chars));
        return probe.state;
    }

    const QString workdirPath = workdir->path();
    const QString homeProbePath = homeProbe.path();
    QProcess *process = new QProcess;
    probe.process = process;
    const auto onFinished = [exe, process, workdir, workdirPath, homeProbePath,
                             outsideWrite] {
        const bool ok = liveProbePassed(*process, workdirPath, outsideWrite);
        process->deleteLater();
        delete workdir;
        QDir(homeProbePath).removeRecursively();
        LiveProbe &probe = liveProbes()[exe];
        probe.process = nullptr;
        if (ok) {
            probe.state = LiveProbeState::Ok;
            probe.failure.clear();
        } else {
            probe.state = LiveProbeState::Failed;
            probe.failure = Tr::tr("The Windows sandbox (Microsoft MXC) failed its live "
                                   "isolation check: a sandboxed command could not be "
                                   "verified to be confined. %1")
                              .arg(installHint());
        }
    };
    QObject::connect(process, &QProcess::finished, nullptr, onFinished);
    process->start(exe, { QStringLiteral("--config-base64"), encoded });
    return probe.state;
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
LiveProbeState liveProbeState(const QString &,
                              const QProcessEnvironment &,
                              const QString &,
                              const QString &)
{
    return LiveProbeState::Ok;
}
#endif

} // namespace

ShellInstallRoot windowsShellInstallRoot(const QString &shellPath)
{
    QString shell = shellPath;
    shell.replace(QLatin1Char('/'), QLatin1Char('\\'));
    const QString unrecognized
        = Tr::tr("The bash at \"%1\" is not part of a recognized Git for Windows or "
                 "MSYS2 installation, so the Windows sandbox cannot grant it a narrow "
                 "read-only location. Install Git for Windows "
                 "(https://git-scm.com/download/win) and put its bash first on PATH, "
                 "or uncheck 'Sandbox commands' in the Llama settings.")
              .arg(shellPath);

    // <root>\usr\bin\bash.exe (MSYS2 and Git for Windows) or
    // <root>\bin\bash.exe (the Git for Windows launcher).
    const QString bashDir = windowsDirectoryOf(shell);
    const QString foldedDir = normalizedPath(bashDir);
    QString root;
    if (foldedDir.endsWith(QLatin1String("/usr/bin")))
        root = windowsDirectoryOf(windowsDirectoryOf(bashDir));
    else if (foldedDir.endsWith(QLatin1String("/bin")))
        root = windowsDirectoryOf(bashDir);
    if (root.isEmpty() || root == bashDir)
        return { {}, unrecognized };

#if defined(Q_OS_WIN)
    // The grant must stay narrow: never a drive root, the Windows directory
    // (WSL's System32\bash.exe), or anything covering the user profile.
    const QString systemRoot = qEnvironmentVariable("SystemRoot",
                                                    qEnvironmentVariable("WINDIR"));
    const QString home = QDir::homePath();
    if (isFileSystemRoot(root)
        || (!systemRoot.isEmpty()
            && (pathCoversInsensitive(systemRoot, root)
                || pathCoversInsensitive(root, systemRoot)))
        || (!home.isEmpty() && pathCoversInsensitive(root, home))) {
        return { {}, unrecognized };
    }
    const QDir rootDir(root);
    const bool hasUsrBash = QFileInfo(rootDir.filePath(QStringLiteral("usr/bin/bash.exe"))).isFile();
    const bool hasMarker
        = QFileInfo::exists(rootDir.filePath(QStringLiteral("git-cmd.exe")))
          || QFileInfo::exists(rootDir.filePath(QStringLiteral("cmd/git.exe")))
          || QFileInfo::exists(rootDir.filePath(QStringLiteral("msys2.exe")))
          || QFileInfo::exists(rootDir.filePath(QStringLiteral("msys2_shell.cmd")));
    if (!hasUsrBash || !hasMarker)
        return { {}, unrecognized };
#endif
    return { root, {} };
}

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

    // Only a recognized shell installation is granted (read-only); the
    // grandparent of an arbitrary bash.exe could be a whole drive or the
    // user profile.
    QString shellRoot;
    if (!shellPath.isEmpty()) {
        const ShellInstallRoot root = windowsShellInstallRoot(shellPath);
        if (!root.error.isEmpty()) {
            spec.error = root.error;
            return spec;
        }
        shellRoot = root.root;
    }

    const LiveProbeState liveState = liveProbeState(exe, env, shellPath, shellRoot);
    if (liveState == LiveProbeState::Running) {
        spec.error = Tr::tr("The Windows sandbox (Microsoft MXC) is verifying its "
                            "isolation for the first time; run the command again in a "
                            "few seconds.");
        return spec;
    }
    if (liveState == LiveProbeState::Failed) {
        spec.error = liveProbes().value(exe).failure;
        return spec;
    }

    const QString encoded = encodedConfig(
        buildConfig(cwd, env, commandLine, timeoutMs, shellRoot));
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
    spec.arguments = { QStringLiteral("--config-base64"), encoded };
    return spec;
}

} // namespace LlamaCpp::Tools
