#include "linux_sandbox.h"

#include "llamatr.h"
#include "tool_utils.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QLoggingCategory>
#include <QProcess>
#include <QStandardPaths>

#include <utils/filepath.h>

using namespace Utils;

namespace LlamaCpp::Tools {

Q_LOGGING_CATEGORY(linuxSandboxLog, "llama.cpp.chat.sandbox.linux", QtWarningMsg)

namespace {

constexpr int kProbeTimeoutMs = 10'000;

// The no-op payload the probe runs inside the minimal sandbox: the first
// of the standard locations that exists.
QString probePayload()
{
    const char *candidates[] = {"/bin/true", "/usr/bin/true",
                                "/bin/bash", "/usr/bin/bash"};
    for (const char *candidate : candidates)
        if (QFileInfo::exists(QString::fromLatin1(candidate)))
            return QString::fromLatin1(candidate);
    return {};
}

// The last non-empty line of \a text; bwrap's setup failures are single
// lines, and that line is what the error message quotes.
QString lastLine(const QString &text)
{
    const QStringList lines = text.split('\n');
    for (int i = lines.size() - 1; i >= 0; --i) {
        const QString line = lines.at(i).trimmed();
        if (!line.isEmpty())
            return line;
    }
    return {};
}

// Turns a raw bwrap failure into an actionable message. The signatures are
// the stable prefixes of bwrap's own error lines ("loopback:", "setting
// up uid map"/"gid map", "Failed to unshare"), so the classification keeps
// working across bwrap releases; anything unrecognized falls back to the
// raw output.
QString classifyProbeFailure(const QString &stderrText)
{
    const QString text = stderrText.toLower();
    const bool refused = text.contains("permitted") || text.contains("denied");
    const QString detail = lastLine(stderrText);
    if (refused && (text.contains("loopback") || text.contains("uid map")
                    || text.contains("gid map"))) {
        // The namespaces are created, but the kernel refuses to set them
        // up: the signature of a restricted unprivileged user namespace,
        // which Ubuntu 24.04 and newer enforce by default through AppArmor
        // (kernel.apparmor_restrict_unprivileged_userns=1).
        return Tr::tr("bubblewrap (bwrap) is installed, but the kernel refuses "
                      "to set up its sandbox: %1. On Ubuntu 24.04 and newer "
                      "this is usually AppArmor's restriction of "
                      "unprivileged user namespaces: run 'sudo sysctl -w "
                      "kernel.apparmor_restrict_unprivileged_userns=0' (add "
                      "it to /etc/sysctl.d/ to make it permanent), or give "
                      "bwrap an AppArmor profile with the 'userns' "
                      "attribute. Alternatively uncheck 'Sandbox commands' "
                      "in the Llama settings to run commands without a "
                      "sandbox.")
            .arg(detail.isEmpty() ? QStringLiteral("operation not permitted")
                                  : detail);
    }
    if (refused && text.contains("unshare")) {
        return Tr::tr("bubblewrap (bwrap) is installed, but unprivileged "
                      "user namespaces are disabled on this system: %1. On "
                      "Debian and its derivatives, enable them with 'sudo "
                      "sysctl -w kernel.unprivileged_userns_clone=1'. "
                      "Alternatively uncheck 'Sandbox commands' in the "
                      "Llama settings to run commands without a sandbox.")
            .arg(detail.isEmpty() ? QStringLiteral("operation not permitted")
                                  : detail);
    }
    return Tr::tr("bubblewrap (bwrap) failed to set up the sandbox: %1. "
                  "Check that it can run on this system, or uncheck "
                  "'Sandbox commands' in the Llama settings to run commands "
                  "without a sandbox.")
        .arg(detail.isEmpty() ? QStringLiteral("unknown error") : detail);
}

// Runs a minimal sandbox (the read-only root, fresh /dev and /proc, new
// pid/ipc/uts and network namespaces) with a no-op payload, verifying that
// bwrap can actually set up the sandbox on this host. The network
// namespace is always part of the probe: it is the superset of the setup
// work, so a passing probe covers both the network-off (default) and the
// network-on configuration.
//
// The successful result is cached for the session (keyed by the
// executable, as LLAMA_BWRAP may point elsewhere); a failure re-probes on
// every call, so a fix applied while the editor is running (e.g. the
// sysctl change in classifyProbeFailure()) takes effect with the next
// command.
QString probeBwrap(const QString &exe)
{
    static QHash<QString, bool> s_probed;
    if (s_probed.value(exe))
        return {};

    const QString payload = probePayload();
    if (payload.isEmpty()) {
        // Nothing to run inside the sandbox: the probe cannot be built, so
        // it is skipped (the first real command surfaces any setup failure
        // then).
        return {};
    }

    QProcess process;
    process.start(exe, {
        QStringLiteral("--ro-bind"), QStringLiteral("/"),
        QStringLiteral("/"),
        QStringLiteral("--dev"), QStringLiteral("/dev"),
        QStringLiteral("--proc"), QStringLiteral("/proc"),
        QStringLiteral("--unshare-pid"),
        QStringLiteral("--unshare-ipc"),
        QStringLiteral("--unshare-uts"),
        QStringLiteral("--unshare-net"),
        payload,
    });
    if (!process.waitForFinished(kProbeTimeoutMs)) {
        process.kill();
        process.waitForFinished(5000);
        qCWarning(linuxSandboxLog)
            << "bwrap capability probe timed out after" << kProbeTimeoutMs
            << "ms for" << exe;
        return Tr::tr("bubblewrap (bwrap) did not set up the sandbox in "
                      "time. Check that it can run on this system, or "
                      "uncheck 'Sandbox commands' in the Llama settings to "
                      "run commands without a sandbox.");
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString stderrText
            = QString::fromLocal8Bit(process.readAllStandardError()).trimmed();
        qCWarning(linuxSandboxLog)
            << "bwrap capability probe failed for" << exe << "(exit"
            << process.exitCode() << "):" << stderrText;
        return classifyProbeFailure(stderrText);
    }
    s_probed.insert(exe, true);
    qCDebug(linuxSandboxLog) << "bwrap capability probe passed for" << exe;
    return {};
}

} // namespace

QString findBwrap()
{
    // The LLAMA_BWRAP override: the tests point it at a fake bwrap (or the
    // real one), and a developer can point it at a build of their own
    // without touching the PATH.
    const QString overridePath = qEnvironmentVariable("LLAMA_BWRAP");
    if (!overridePath.isEmpty()) {
        const FilePath fromEnv = FilePath::fromUserInput(overridePath);
        if (fromEnv.exists())
            return fromEnv.toUserOutput();
    }
    return QStandardPaths::findExecutable("bwrap");
}

LinuxSandboxSpec linuxSandboxSpec(const QString &cwd)
{
    const QString exe = findBwrap();
    if (exe.isEmpty())
        return { {}, {}, Tr::tr("bubblewrap (bwrap) was not found. Install it "
                                "(e.g. 'apt install bubblewrap' or 'dnf install "
                                "bubblewrap') or uncheck 'Sandbox commands' "
                                "in the Llama settings to run commands without "
                                "a sandbox.") };
    if (const QString error = probeBwrap(exe); !error.isEmpty())
        return { {}, {}, error };

    QStringList args = { QStringLiteral("--ro-bind"), QStringLiteral("/"),
                         QStringLiteral("/"),
                         QStringLiteral("--bind"), cwd, cwd };
    // A fresh, empty /tmp for the command. Skipped when the working
    // directory is inside it, as the later mount would wipe out the
    // writable cwd bind.
    if (!pathCovers(QStringLiteral("/tmp"), cwd))
        args << QStringLiteral("--tmpfs") << QStringLiteral("/tmp");
    // The read-only root still exposes the host's Unix sockets (D-Bus
    // session bus, ssh-agent, gpg-agent, docker.sock, Wayland/X11), which
    // --unshare-net does not cover: mask the runtime directories with empty
    // tmpfses. /var/run is normally a symlink to /run and then follows the
    // mask. Skipped when the working directory is inside one of them (the
    // later mount would wipe out the writable cwd bind). Name resolution
    // (/etc/resolv.conf -> /run/...) breaks, so the masks are skipped when
    // the network is allowed (the command needs DNS then).
    if (!sandboxAllowsNetwork()) {
        QStringList runtimeDirs = { QStringLiteral("/run") };
        if (!QFileInfo(QStringLiteral("/var/run")).isSymLink())
            runtimeDirs << QStringLiteral("/var/run");
        const QString xdgRuntimeDir = qEnvironmentVariable("XDG_RUNTIME_DIR");
        if (!xdgRuntimeDir.isEmpty() && !pathCovers(QStringLiteral("/run"), xdgRuntimeDir))
            runtimeDirs << xdgRuntimeDir;
        for (const QString &runtimeDir : std::as_const(runtimeDirs)) {
            if (pathCovers(runtimeDir, cwd) || !QFileInfo(runtimeDir).isDir())
                continue;
            args << QStringLiteral("--tmpfs") << runtimeDir;
        }
    }
    // NixOS keeps the whole system profile (PATH) under /run/current-system.
    if (QFileInfo::exists(QStringLiteral("/run/current-system"))
        && !pathCovers(QStringLiteral("/run"), cwd))
        args << QStringLiteral("--ro-bind") << QStringLiteral("/run/current-system")
             << QStringLiteral("/run/current-system");
    // Extra writable directories from the Sandbox settings page, bound on
    // top of the read-only root (and after the fresh /tmp, so a writable
    // directory inside /tmp survives). Only existing directories can be
    // bound; the secret and protected mounts below still take precedence.
    for (const QString &dir : sandboxWritableDirs())
        if (QFileInfo(dir).isDir())
            args << QStringLiteral("--bind") << dir << dir;
    args << QStringLiteral("--dev") << QStringLiteral("/dev")
        << QStringLiteral("--proc") << QStringLiteral("/proc")
        << QStringLiteral("--unshare-pid")
        << QStringLiteral("--unshare-ipc")
        << QStringLiteral("--unshare-uts");
    // No network by default; the Sandbox settings page can lift it.
    if (!sandboxAllowsNetwork())
        args << QStringLiteral("--unshare-net");
    args
        // A new session: no TIOCSTI injection into the controlling terminal.
        << QStringLiteral("--new-session")
        << QStringLiteral("--die-with-parent");
    // Hide the credential locations plus the user-configured deny-read
    // directories - the read equivalent of the macOS deny-read rules: empty
    // tmpfses over the directories, /dev/null over the files. Skipped when
    // the working directory is inside one of them, as the later mount would
    // wipe out the writable cwd bind.
    for (const SecretReadPath &secret : denyReadPaths()) {
        if (pathCovers(secret.path, cwd))
            continue;
        if (QDir(secret.path).exists())
            args << QStringLiteral("--tmpfs") << secret.path;
        else if (QFileInfo(secret.path).isFile())
            args << QStringLiteral("--ro-bind") << QStringLiteral("/dev/null")
                 << secret.path;
    }
    // The protected locations in the working directory (git hooks and
    // config, Qt Creator's per-project settings) stay read-only: their
    // content runs outside the sandbox. Last, so that they sit on top of
    // the writable cwd bind.
    for (const QString &path : protectedSubpaths(cwd))
        args << QStringLiteral("--ro-bind") << path << path;
    return { exe, args, {} };
}

} // namespace LlamaCpp::Tools
