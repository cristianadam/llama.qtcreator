#include "bash_tool.h"
#include "factory.h"
#include "llamasettings.h"
#include "llamatr.h"
#include "tool_utils.h"
#include "windows_sandbox.h"

#include <QtTaskTree/qprocesstask.h>
#include <QtTaskTree/qtasktree.h>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QTemporaryFile>
#include <QUuid>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/environment.h>
#include <utils/filepath.h>
#include <utils/temporaryfile.h>

using namespace ProjectExplorer;
using namespace Utils;
using namespace QtTaskTree;

namespace LlamaCpp::Tools {

Q_LOGGING_CATEGORY(bashToolLog, "llama.cpp.chat.bash", QtWarningMsg)

namespace {

constexpr int kDefaultTimeoutMs = 120 * 1000;
constexpr int kMaxTimeoutMs = 10 * 60 * 1000;
constexpr int kMaxOutputLines = 2000;
constexpr int kMaxOutputBytes = 50 * 1024; // 50 KB

struct BashSpec
{
    QString program;
    QString executeFlag;
    QString error; // non-empty when no usable bash was found
};

struct SandboxSpec
{
    QString program; // sandbox wrapper executable
    QStringList arguments; // wrapper arguments, the command follows them
    QString error; // non-empty when the sandbox is requested but unavailable
};

#if defined(Q_OS_WIN)

/*! Locates the bash shipped with Git for Windows, mirroring how the
    terminal plugin finds it: next to the git executable, then in the
    default Program Files installation directories. */
QString findGitBash()
{
    const FilePath git = FilePath::fromUserInput(QStandardPaths::findExecutable("git.exe"));
    if (git.exists()) {
        const FilePath gitBash = git.parentDir().parentDir().pathAppended(QStringLiteral("bin/bash.exe"));
        if (gitBash.exists())
            return gitBash.toUserOutput();
    }
    const QStringList programFiles = {qEnvironmentVariable("ProgramFiles"),
                                      qEnvironmentVariable("ProgramFiles(x86)")};
    for (const QString &dir : programFiles) {
        if (dir.isEmpty())
            continue;
        const FilePath gitBash =
            FilePath::fromString(dir + QStringLiteral("/Git/bin/bash.exe"));
        if (gitBash.exists())
            return gitBash.toUserOutput();
    }
    return {};
}

#endif

/*! Resolves a bash executable on all platforms so that commands behave
    identically for the model: /bin/bash on Unix, Git Bash on Windows.
    Never uses $SHELL or cmd.exe. */
BashSpec bashSpec()
{
#if defined(Q_OS_WIN)
    const FilePath onPath =
        FilePath::fromUserInput(QStandardPaths::findExecutable("bash.exe"));
    if (onPath.exists())
        return { onPath.toUserOutput(), QStringLiteral("-c"), {} };
    const QString gitBash = findGitBash();
    if (!gitBash.isEmpty())
        return { gitBash, QStringLiteral("-c"), {} };
    return { {}, {},
             Tr::tr("No bash shell found. Install Git for Windows "
                    "(https://git-scm.com/download/win) or add a bash to PATH.") };
#else
    const FilePath binBash = FilePath::fromString(QStringLiteral("/bin/bash"));
    if (binBash.exists())
        return { binBash.toUserOutput(), QStringLiteral("-c"), {} };
    const FilePath onPath =
        FilePath::fromUserInput(QStandardPaths::findExecutable("bash"));
    if (onPath.exists())
        return { onPath.toUserOutput(), QStringLiteral("-c"), {} };
    return { QStringLiteral("/bin/sh"), QStringLiteral("-c"), {} };
#endif
}

// Escapes a path for use inside a double-quoted sandbox-exec profile string.
QString profileString(const QString &path)
{
    QString escaped = path;
    escaped.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    escaped.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return escaped;
}

#if defined(Q_OS_MACOS)

// Default-allow profile that turns the system locations read-only. The
// working directory and the temporary locations stay writable. Two
// Seatbelt quirks shape the deny list (both verified empirically):
// - request paths are canonicalized by the kernel (/var -> /private/var),
//   while rule paths are matched as written, so rules must use the
//   canonical spelling to take effect
// - a matching deny beats a more specific allow, so a blanket deny of a
//   writable location's parent (e.g. /private/var covering $TMPDIR,
//   /Users covering the working directory) cannot be carved out with an
//   allow. Deny entries that would cover one of the writable locations
//   (e.g. the home directory when the project is inside it, or a project
//   on a volume under /Volumes) are skipped instead.
QString macSandboxProfile(const QString &cwd)
{
    const QString tmpDir = QDir::tempPath();
    const QString workDir = profileString(cwd);

    // Unreadable credential locations (skipped when the working directory
    // is inside one of them, the same way the write denies are).
    QStringList readDenies;
    for (const SecretReadPath &secret : secretReadPaths())
        if (!pathCovers(secret.path, cwd))
            readDenies << QStringLiteral("  (subpath \"") + profileString(secret.path)
                        + QStringLiteral("\")");

    QStringList denyPaths = {
        QStringLiteral("/bin"),
        QStringLiteral("/sbin"),
        QStringLiteral("/usr"),
        QStringLiteral("/System"),
        QStringLiteral("/Library"),
        QStringLiteral("/Applications"),
        QStringLiteral("/cores"),
        QStringLiteral("/opt"),
        QStringLiteral("/home"),
        QStringLiteral("/Volumes"),
        QStringLiteral("/etc"),
        QStringLiteral("/private/etc"),
        // Canonical spellings: the /var/... forms are dead rules, see above.
        QStringLiteral("/private/var/db"),
        QStringLiteral("/private/var/root"),
        QStringLiteral("/private/var/log"),
        QStringLiteral("/private/var/at"),
        QStringLiteral("/private/var/spool"),
        QStringLiteral("/private/var/mail"),
        QStringLiteral("/private/var/ldap"),
    };
    // The credential locations are not writable either: the read denies
    // above would be pointless if commands could plant or overwrite
    // credentials.
    for (const SecretReadPath &secret : secretReadPaths())
        denyPaths << secret.path;
    // Home directories under /Users (all users'; the entry covering the
    // working directory is skipped by the loop below).
    const QStringList userDirs =
        QDir(QStringLiteral("/Users")).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &userDir : userDirs)
        denyPaths << QStringLiteral("/Users/") + userDir;
    QStringList writeDenies;
    for (const QString &denyPath : denyPaths)
        if (!pathCovers(denyPath, cwd) && !pathCovers(denyPath, tmpDir))
            writeDenies << QStringLiteral("  (subpath \"") + profileString(denyPath)
                        + QStringLiteral("\")");

    // Note: the (allow file-write*) block below is belt-and-braces -
    // (allow default) already permits those writes, and the deny list skips
    // anything covering the working directory or the temporary locations, so
    // a matching deny can never beat the allow. It is kept to make the
    // writable locations explicit.
    QString profile = QStringLiteral(
                          "(version 1)\n"
                          "(allow default)\n"
                          "(allow file-write* (literal \"/dev/null\") "
                          "(literal \"/dev/zero\") "
                          "(literal \"/dev/urandom\") "
                          "(literal \"/dev/random\") "
                          "(literal \"/dev/tty\") "
                          "(subpath \"/private/tmp\") "
                          "(subpath \"")
        + profileString(tmpDir) + QStringLiteral("\") (subpath \"") + workDir
        + QStringLiteral("\"))\n");
    // Block devices are writable under (allow default), so a runaway command
    // could dd or mkfs a disk. Individual globs, as a subpath deny of /dev
    // would also beat the needed writes to /dev/null that shell redirections
    // do constantly.
    profile += QStringLiteral(
                   "(deny file-write* (literal \"/dev/disk*\") "
                   "(literal \"/dev/rdisk*\") "
                   "(literal \"/dev/bpf*\") (literal \"/dev/apple*\"))\n");
    // No network access. network* is the only valid spelling (there are no
    // individual network-out/in symbols) and it also covers Unix domain
    // sockets; the process stub connects to its control socket before
    // exec'ing the wrapper, so it is unaffected. Web access is available
    // through the webfetch/websearch tools, which run outside the sandbox.
    profile += QStringLiteral("(deny network*)\n");
    if (!writeDenies.isEmpty())
        profile += QStringLiteral("(deny file-write*\n") + writeDenies.join(QLatin1Char('\n'))
                 + QStringLiteral("\n)\n");
    if (!readDenies.isEmpty())
        profile += QStringLiteral("(deny file-read*\n") + readDenies.join(QLatin1Char('\n'))
                 + QStringLiteral("\n)\n");
    return profile;
}

#endif

// Builds the wrapper that confines the command to the working directory and
// the temporary locations. Linux uses bubblewrap (the whole file system is
// read-only except \a cwd and /tmp); macOS uses sandbox-exec; Windows uses
// srt-win (the @anthropic-ai/sandbox-runtime backend): the command runs as
// a dedicated sandbox user with no network egress and an isolated profile,
// the home directory is readable, and \a cwd plus the temporary directory
// are writable. \a env is the environment the (sandboxed) command gets.
SandboxSpec sandboxSpec(const QString &cwd, const QProcessEnvironment &env)
{
#if defined(Q_OS_WIN)
    const WindowsSandboxSpec windows = windowsSandboxSpec(cwd, env);
    return { windows.program, windows.arguments, windows.error };
#elif defined(Q_OS_MACOS)
    const FilePath exe =
        FilePath::fromUserInput(QStandardPaths::findExecutable("sandbox-exec"));
    if (!exe.exists())
        return { {}, {}, Tr::tr("The sandbox-exec executable was not found; "
                                "sandboxing is unavailable on this system.") };
    return { exe.toUserOutput(),
             { QStringLiteral("-p"), macSandboxProfile(cwd) },
             {} };
#else
    const FilePath exe =
        FilePath::fromUserInput(QStandardPaths::findExecutable("bwrap"));
    if (!exe.exists())
        return { {}, {}, Tr::tr("bubblewrap (bwrap) was not found. Install it "
                                "(e.g. 'apt install bubblewrap' or 'dnf install "
                                "bubblewrap') or uncheck 'Sandbox commands' "
                                "in the Llama settings to run commands without "
                                "a sandbox.") };
    QStringList args = { QStringLiteral("--ro-bind"), QStringLiteral("/"),
                         QStringLiteral("/"),
                         QStringLiteral("--bind"), cwd, cwd };
    // A fresh, empty /tmp for the command. Skipped when the working
    // directory is inside it, as the later mount would wipe out the
    // writable cwd bind.
    if (!pathCovers(QStringLiteral("/tmp"), cwd))
        args << QStringLiteral("--tmpfs") << QStringLiteral("/tmp");
    args << QStringLiteral("--dev") << QStringLiteral("/dev")
        << QStringLiteral("--proc") << QStringLiteral("/proc")
        << QStringLiteral("--unshare-pid")
        << QStringLiteral("--unshare-ipc")
        << QStringLiteral("--unshare-uts")
        << QStringLiteral("--unshare-net")
        << QStringLiteral("--die-with-parent");
    // Hide the credential locations - the read equivalent of the macOS
    // deny-read rules: empty tmpfses over the directories, /dev/null over
    // the files. Skipped when the working directory is inside one of them,
    // as the later mount would wipe out the writable cwd bind.
    for (const SecretReadPath &secret : secretReadPaths()) {
        if (pathCovers(secret.path, cwd))
            continue;
        if (QDir(secret.path).exists())
            args << QStringLiteral("--tmpfs") << secret.path;
        else if (QFileInfo(secret.path).isFile())
            args << QStringLiteral("--ro-bind") << QStringLiteral("/dev/null")
                 << secret.path;
    }
    return { exe.toUserOutput(), args, {} };
#endif
}

QProcessEnvironment shellEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (const Project *p = ProjectManager::startupProject()) {
        if (const Kit *kit = p->activeKit())
            env = kit->buildEnvironment().toProcessEnvironment();
    }
    return env;
}

/*! Locates Qt Creator's process stub, which wraps the shell like the
    terminal does, so that the inferior can be killed reliably and crashes
    are reported through the control socket. Returns an empty string when
    the stub is not available (plain shell fallback is used then). */
QString findProcessStub()
{
    QString exe = QStringLiteral("qtcreator_process_stub");
#if defined(Q_OS_WIN)
    exe += QStringLiteral(".exe");
#endif
    const QString overridePath = qEnvironmentVariable("LLAMA_SHELL_STUB");
    const FilePath stub = overridePath.isEmpty()
            ? FilePath::fromUserInput(QCoreApplication::applicationDirPath())
                      .pathAppended(QLatin1String(RELATIVE_LIBEXEC_PATH))
                      .pathAppended(exe)
            : FilePath::fromUserInput(overridePath);
    return stub.exists() ? stub.toFSPathString() : QString();
}

/*! Writes the environment as a NUL separated "KEY=VALUE" list file, the
    format the process stub reads with its -e option. Returns an empty
    string on failure. */
QString writeEnvironmentFile(const QProcessEnvironment &env, QObject *parent)
{
    QTemporaryFile *file = new QTemporaryFile(parent);
    if (!file->open()) {
        delete file;
        return {};
    }
    for (const QString &key : env.keys())
        file->write((key + QLatin1Char('=') + env.value(key) + QLatin1Char('\0')).toUtf8());
    file->flush();
    return file->fileName();
}

/*! Saves \a text to a uniquely named temporary file so that truncated output
    can be recovered. Returns an empty string when saving fails. */
QString saveFullOutput(const QString &text)
{
    const QString path
        = QDir::tempPath() + QStringLiteral("/llama-bash-")
          + QUuid::createUuid().toString(QUuid::WithoutBraces) + QStringLiteral(".log");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return {};
    file.write(text.toUtf8());
    file.close();
    return path;
}

struct TruncatedOutput
{
    QString content;
    QString note; // empty unless the output was truncated
};

/*! Keeps only the tail of \a text, bounded by kMaxOutputLines lines and
    kMaxOutputBytes (whichever limit is hit first). The full output of
    truncated results is saved to a temporary file and referenced in \c note. */
TruncatedOutput truncateOutput(const QString &text)
{
    TruncatedOutput result{text, {}};
    if (text.isEmpty())
        return result;

    const int totalLines
        = text.count(QLatin1Char('\n')) + (text.endsWith(QLatin1Char('\n')) ? 0 : 1);
    QString keep = text;
    bool truncatedByLines = false;

    if (totalLines > kMaxOutputLines) {
        int from = 0;
        int found = 0;
        const int target = totalLines - kMaxOutputLines;
        for (int pos = keep.indexOf('\n'); pos != -1 && found < target;
             pos = keep.indexOf('\n', pos + 1)) {
            from = pos + 1;
            ++found;
        }
        keep = keep.mid(from);
        truncatedByLines = true;
    }

    QByteArray utf8 = keep.toUtf8();
    bool truncatedByBytes = false;
    if (utf8.size() > kMaxOutputBytes) {
        int pos = utf8.size() - kMaxOutputBytes;
        const int nl = utf8.indexOf('\n', pos);
        if (nl != -1)
            pos = nl + 1;
        keep = QString::fromUtf8(utf8.mid(pos));
        truncatedByBytes = true;
    }

    if (!truncatedByLines && !truncatedByBytes)
        return result;

    result.content = keep;
    QString note = Tr::tr("[Output truncated: %1]")
                       .arg(truncatedByLines
                                ? Tr::tr("showing last %1 of %2 lines")
                                      .arg(kMaxOutputLines)
                                      .arg(totalLines)
                                : Tr::tr("showing last %1 KB").arg(kMaxOutputBytes / 1024));
    const QString fullPath = saveFullOutput(text);
    if (!fullPath.isEmpty())
        note += QStringLiteral(" ") + Tr::tr("Full output saved to: %1").arg(fullPath);
    result.note = note;
    return result;
}

// State shared between the task tree callbacks. All access happens on the
// main thread, in this order: timeout handler, process done handler,
// tree done handler. Owns the stub's control server and temp files, which
// are released when the state is destroyed in the tree done handler.
class BashState : public QObject
{
public:
    explicit BashState() = default;

    QString stubPath;
    QString socketName;
    // Temp directory (0700) holding the stub control socket on non-Windows
    // systems; auto-removed when the state is destroyed.
    std::unique_ptr<Utils::TemporaryFilePath> socketDir;
    QString envFilePath;
    QLocalSocket *controlSocket = nullptr; // owned by the control server

    bool usedStub = false;
    bool crashed = false;
    bool timedOut = false;
    QProcess::ProcessError processError = QProcess::UnknownError;
    QString errorString;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    int exitCode = -1;
    QString output;
};

} // namespace

const bool registered = [] {
    ToolFactory::instance().registerCreator(BashTool{}.name(),
                                            []() { return std::make_unique<BashTool>(); });
    return true;
}();

QString BashTool::name() const
{
    return QStringLiteral("bash");
}

QString BashTool::toolDefinition() const
{
    QString description = QStringLiteral(
        "Executes a command in a bash shell and returns its combined output "
        "(stdout and stderr). Commands use bash/POSIX syntax on all platforms "
        "(Windows uses Git Bash). Use this for terminal operations like git, "
        "npm, docker, running builds or tests; do not use it for reading, "
        "writing, editing or searching files - use the dedicated tools for "
        "that. Output is limited to the last 2000 lines or 50 KB; when "
        "truncated, the full output is saved to a temporary file and its "
        "path is reported. Non-zero exit codes, crashes and timeouts are "
        "reported as failures, including the output produced so far.");
    if (sandboxEnabled(ProjectManager::startupProject()))
        description += QStringLiteral(
            " Commands run in a sandbox: only the working directory and "
            "temporary locations are writable, credential locations "
            "(~/.ssh, ~/.aws, ~/.gnupg, ~/.kube, ~/.netrc) are not "
            "readable, and there is no network access - use the webfetch "
            "and websearch tools for that. The workdir must be inside the "
            "project directory or a temporary location; on Linux /tmp is "
            "fresh and empty for each command, and on Windows the command "
            "runs as a dedicated sandbox user with its own empty profile "
            "and temporary directory.");
    return QString::fromUtf8(R"raw(
    {
        "type": "function",
        "function": {
            "name": "bash",
            "description": "%1",
            "parameters": {
                "type": "object",
                "properties": {
                    "command": {
                        "type": "string",
                        "description": "The command to execute."
                    },
                    "workdir": {
                        "type": "string",
                        "description": "Working directory. Defaults to the project directory; use this instead of 'cd'."
                    },
                    "timeout": {
                        "type": "integer",
                        "description": "Timeout in milliseconds (default 120000, max 600000)."
                    }
                },
                "required": ["command"],
                "strict": true
            }
        }
    })raw").arg(description);
}

QString BashTool::oneLineSummary(const QJsonObject &arguments) const
{
    const QString command = arguments.value("command").toString();
    // Only the first line goes into the summary; multi‑line commands
    // (heredocs, etc.) are signalled with an ellipsis.
    QString firstLine = command.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (command.contains(QLatin1Char('\n')))
        firstLine += QStringLiteral(" …");
    if (firstLine.isEmpty())
        return {};
    // Render the command as a code span (the summary is markdown); use a
    // double backtick when the command contains backticks itself.
    const QString code = firstLine.contains(QLatin1Char('`'))
            ? QStringLiteral("``%1``").arg(firstLine)
            : QStringLiteral("`%1`").arg(firstLine);
    return Tr::tr("running %1").arg(code);
}

void BashTool::run(const QJsonObject &arguments,
                   std::function<void(const QString &, bool)> done) const
{
    runCommand(arguments, std::move(done), OutputHandler{});
}

void BashTool::runLive(const QJsonObject &arguments,
                       const OutputHandler &onOutput,
                       std::function<void(const QString &, bool)> done) const
{
    runCommand(arguments, std::move(done), onOutput); // copy – see runCommand()
}

void BashTool::runCommand(const QJsonObject &arguments,
                          std::function<void(const QString &, bool)> done,
                          OutputHandler onOutput) const
{
    const QString command = arguments.value("command").toString().trimmed();
    if (command.isEmpty()) {
        done(Tr::tr("Error: the command must not be empty."), false);
        return;
    }

    Project *p = ProjectManager::startupProject();
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (p)
        cwd = p->projectDirectory();

    const QString workdir = arguments.value("workdir").toString();
    if (!workdir.isEmpty()) {
        const FilePath wd = FilePath::fromUserInput(workdir);
        cwd = wd.isAbsolutePath() ? wd : cwd.pathAppended(workdir);
    }

    const QString cwdString = cwd.toFSPathString();
    if (!QFileInfo::exists(cwdString) || !QFileInfo(cwdString).isDir()) {
        done(Tr::tr("Error: working directory does not exist: %1").arg(cwdString), false);
        return;
    }

    // The sandbox makes the working directory writable, so a model-supplied
    // workdir must stay inside the locations the sandbox allows writes to,
    // or the command would define its own sandbox boundary.
    if (const QString error = sandboxAccessError(cwd, /*isWrite=*/true);
            !error.isEmpty()) {
        done(Tr::tr("Error: %1").arg(error), false);
        return;
    }

    int timeoutMs = arguments.value("timeout").toInt(kDefaultTimeoutMs);
    timeoutMs = qBound(1, timeoutMs, kMaxTimeoutMs);

    const BashSpec spec = bashSpec();
    if (!spec.error.isEmpty()) {
        done(Tr::tr("Error: %1").arg(spec.error), false);
        return;
    }

    // The environment is needed both for the process and to build the
    // sandbox wrapper (srt-win passes it as the --env overlay for the
    // sandboxed child, which starts with the sandbox user's own profile
    // environment only).
    const QProcessEnvironment env = shellEnvironment();

    // Optionally confine the command to a sandbox (bubblewrap on Linux,
    // sandbox-exec on macOS, srt-win on Windows). The wrapper becomes the
    // program to start; the shell and the command are its arguments.
    const SandboxSpec sandbox = sandboxEnabled(p)
            ? sandboxSpec(cwdString, env)
            : SandboxSpec{};
    if (!sandbox.error.isEmpty()) {
        done(Tr::tr("Error: %1").arg(sandbox.error), false);
        return;
    }

    auto *state = new BashState;

    // Run the command through Qt Creator's process stub when available, so
    // the shell (the inferior) can be killed over the control socket and
    // crashes are reported by the stub itself. The control server and the
    // environment file are owned by the state and released with it.
    state->stubPath = findProcessStub();
    if (!state->stubPath.isEmpty()) {
        QString socketName;
#if defined(Q_OS_WIN)
        socketName = QStringLiteral("llama-bash-%1")
                         .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
#else
        // Keep the socket in a private 0700 directory, as some systems do
        // not check socket file permissions. The full path must fit into
        // sockaddr_un::sun_path (104 bytes on macOS).
        // Use QDir::tempPath() rather than Utils::TemporaryDirectory's
        // master directory, which is only initialised by Qt Creator core.
        const Utils::FilePath templatePath =
            Utils::FilePath::fromString(
                QDir::tempPath() + QStringLiteral("/llama-bash-s-XXXXXX"));
        auto dirResult = Utils::TemporaryFilePath::create(templatePath, /* directory = */ true);
        if (dirResult)
            socketName = (*dirResult)->filePath().pathAppended(QStringLiteral("t")).toFSPathString();
        state->socketDir = dirResult ? std::move(*dirResult) : nullptr;
#endif
        if (!socketName.isEmpty()) {
            auto *server = new QLocalServer(state);
            if (server->listen(socketName)) {
                state->usedStub = true;
                state->socketName = socketName;
                state->envFilePath = writeEnvironmentFile(env, state);
                QObject::connect(server,
                                 &QLocalServer::newConnection,
                                 state,
                                 [state, server] {
                                     QLocalSocket *socket = server->nextPendingConnection();
                                     if (!socket)
                                         return;
                                     state->controlSocket = socket;
                                     QObject::connect(socket,
                                                      &QIODevice::readyRead,
                                                      state,
                                                      [state, socket] {
                                                          while (socket->canReadLine()) {
                                                              const QByteArray line
                                                                  = socket->readLine().trimmed();
                                                              if (line.startsWith("crash "))
                                                                  state->crashed = true;
                                                          }
                                                      });
                                 });
            } else {
                qCWarning(bashToolLog)
                    << "Cannot create stub control socket:" << server->errorString();
                delete server;
            }
        }
    }

    // Captures a copy of the output handler (std::function copies are
    // cheap); the setup handler itself must stay const-callable for
    // QProcessTask.
    const auto onSetup = [state, cwdString, env, spec, command,
                          sandbox, onOutput](QProcess &process) {
        // The program to start and its arguments: the shell and the command,
        // optionally wrapped in the sandbox. The stub and the plain QProcess
        // path must agree on the inferior, or the sandbox would be silently
        // bypassed.
        QString program = spec.program;
        QStringList arguments = {spec.executeFlag, command};
        if (!sandbox.program.isEmpty()) {
            program = sandbox.program;
            arguments =
                sandbox.arguments + QStringList{spec.program, spec.executeFlag, command};
        }

        process.setWorkingDirectory(cwdString);
        process.setProcessChannelMode(QProcess::MergedChannels);
        if (state->usedStub) {
            // The stub needs a plain environment; the inferior gets the
            // environment from the file passed with -e.
            process.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
            process.setProgram(state->stubPath);
            // --wait must be passed explicitly (even empty): the option has
            // a default value, and a non-empty value makes the stub wait for
            // a key press before exiting.
            QStringList stubArguments = {QStringLiteral("-s"),
                                         state->socketName,
                                         QStringLiteral("-w"),
                                         cwdString,
                                         QStringLiteral("-e"),
                                         state->envFilePath,
                                         QStringLiteral("--wait"),
                                         QString(),
                                         QStringLiteral("--")};
            // The stub execs the first argument after "--" as the program,
            // so the program (shell, or sandbox wrapper) must lead.
            stubArguments += QStringList{program} + arguments;
            process.setArguments(stubArguments);
        } else {
            process.setProcessEnvironment(env);
            process.setProgram(program);
            process.setArguments(arguments);
        }

        // Live output: drain the merged channel as it arrives and report
        // the (truncated) tail.  The done handler appends whatever is left.
        if (onOutput) {
            // The process outlives the connection (the task tree owns it,
            // and the context object, state, is released with the tree).
            // The connection owns its own copy of the handler.
            QObject::connect(&process,
                             &QProcess::readyRead,
                             state,
                             [state, handler = onOutput, &process] {
                                 state->output +=
                                         QString::fromUtf8(process.readAllStandardOutput());
                                 handler(truncateOutput(state->output).content);
                             });
        }
    };

    // QCustomTask's done handler takes the task as const reference, but the
    // process is still alive and owned by the task tree at this point, so
    // casting away const to drain the output buffer is safe.
    const auto onProcessDone = [state](const QProcess &process, DoneWith) {
        QProcess *mutableProcess = const_cast<QProcess *>(&process);
        state->output += QString::fromUtf8(mutableProcess->readAllStandardOutput());
        state->processError = process.error();
        state->errorString = process.errorString();
        state->exitStatus = process.exitStatus();
        state->exitCode = process.exitCode();
    };

    const auto onTimeout = [state] {
        state->timedOut = true;
        // Kill the inferior (the shell) over the control socket; the stub
        // exits once the inferior is gone.
        if (state->controlSocket && state->controlSocket->isWritable()) {
            state->controlSocket->write("k", 1);
            state->controlSocket->flush();
        }
    };

    QTaskTree *tree = new QTaskTree;
    tree->setRecipe(Group{QProcessTask(onSetup, onProcessDone)
                              .withTimeout(std::chrono::milliseconds(timeoutMs), onTimeout)});

    // Builds the model-facing result from whatever the process managed to
    // produce, then reports it and cleans up.
    auto finish = [state, done = std::move(done), timeoutMs](DoneWith) mutable {
        QStringList notes;
        bool ok = true;

        if (state->timedOut) {
            ok = false;
            notes << Tr::tr("Command timed out after %1 ms. Retry with a larger "
                            "timeout if the command is expected to take longer.")
                          .arg(timeoutMs);
        } else if (!state->usedStub && state->processError == QProcess::FailedToStart) {
            ok = false;
            notes << Tr::tr("Failed to start the command: %1")
                          .arg(state->errorString.isEmpty()
                                   ? Tr::tr("unknown error")
                                   : state->errorString);
        } else if (state->crashed || state->exitStatus == QProcess::CrashExit) {
            ok = false;
            notes << Tr::tr("Command terminated abnormally (crashed).");
        } else if (state->exitCode != 0) {
            ok = false;
            notes << Tr::tr("Command exited with code %1.").arg(state->exitCode);
        }

        QString text;
        if (state->output.isEmpty()) {
            text = QStringLiteral("(no output)");
        } else {
            const TruncatedOutput truncated = truncateOutput(state->output);
            text = truncated.content;
            if (!truncated.note.isEmpty())
                notes.prepend(truncated.note);
        }

        if (!notes.isEmpty())
            text += QLatin1Char('\n') + QLatin1Char('\n') + notes.join(QLatin1Char('\n'));

        const QString result = text;
        const bool success = ok;
        state->deleteLater();

        done(result, success);
    };

    // Keep the tree alive until the recipe finished, then release it
    // together with the state.
    QObject::connect(tree,
                     &QTaskTree::done,
                     state,
                     [tree, finish](DoneWith result) mutable {
                         finish(result);
                         tree->deleteLater();
                     });
    tree->start();
}

QString BashTool::detailsMarkdown(const QJsonObject &arguments, const QString &result, bool ok) const
{
    Q_UNUSED(ok);
    const QString command = arguments.value("command").toString();
    // The command and its output go into a single code block with a "$ "
    // prompt in front of the command, so the expanded details read like a
    // terminal transcript.  Failures are visible in the output itself and
    // via the ✗ icon in the summary, so no separate error header.
    QString block = QStringLiteral("$ ") + command;
    if (!block.endsWith(QLatin1Char('\n')))
        block += QLatin1Char('\n');
    block += result;

    QString md;
    const QString workdir = arguments.value("workdir").toString();
    if (!workdir.isEmpty())
        md = Tr::tr("Working directory: %1\n\n").arg(workdir);
    md += codeFence(block, QStringLiteral("bash"));
    return md;
}

QString BashTool::summaryPreview(const QJsonObject &arguments, const QString &result, bool ok) const
{
    Q_UNUSED(arguments);
    Q_UNUSED(ok);
    if (result.isEmpty())
        return {};

    // Show the *tail* of the output: the last lines usually carry the
    // outcome of a command (summary lines, errors, final status).
    constexpr int kMaxLines = 3;
    const QStringList lines = result.split(QLatin1Char('\n'));
    QString tail = result;
    if (lines.size() > kMaxLines)
        // The … marks the earlier, omitted lines; the expand icon marks
        // that more details are available.
        tail = QStringLiteral("…\n") + lines.mid(lines.size() - kMaxLines + 1).join(QLatin1Char('\n'));
    if (tail.size() > 240)
        tail = tail.left(237);
    return codeFence(tail);
}

} // namespace LlamaCpp::Tools
