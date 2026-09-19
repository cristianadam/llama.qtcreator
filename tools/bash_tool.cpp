#include "bash_tool.h"
#include "factory.h"
#include "llamatr.h"

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
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "bash",
            "description": "Executes a command in a bash shell and returns its combined output (stdout and stderr). Commands use bash/POSIX syntax on all platforms (Windows uses Git Bash). Use this for terminal operations like git, npm, docker, running builds or tests. Do not use it for reading, writing, editing or searching files - use the dedicated tools for that instead. Output is limited to the last 2000 lines or 50 KB; if it is truncated, the full output is saved to a temporary file and its path is reported. Non-zero exit codes, crashes and timeouts are reported as failures together with the output produced so far.",
            "parameters": {
                "type": "object",
                "properties": {
                    "command": {
                        "type": "string",
                        "description": "The command to execute."
                    },
                    "workdir": {
                        "type": "string",
                        "description": "The working directory to run the command in. Defaults to the current project directory. Use this instead of 'cd' commands."
                    },
                    "timeout": {
                        "type": "integer",
                        "description": "Optional timeout in milliseconds. Defaults to 120000 ms and must not exceed 600000 ms."
                    }
                },
                "required": ["command"],
                "strict": true
            }
        }
    })raw";
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
    const QString command = arguments.value("command").toString().trimmed();
    if (command.isEmpty()) {
        done(Tr::tr("Error: the command must not be empty."), false);
        return;
    }

    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
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

    int timeoutMs = arguments.value("timeout").toInt(kDefaultTimeoutMs);
    timeoutMs = qBound(1, timeoutMs, kMaxTimeoutMs);

    const BashSpec spec = bashSpec();
    if (!spec.error.isEmpty()) {
        done(Tr::tr("Error: %1").arg(spec.error), false);
        return;
    }

    const QProcessEnvironment env = shellEnvironment();

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

    const auto onSetup = [state, cwdString, env, spec, command](QProcess &process) {
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
            process.setArguments({QStringLiteral("-s"),
                                  state->socketName,
                                  QStringLiteral("-w"),
                                  cwdString,
                                  QStringLiteral("-e"),
                                  state->envFilePath,
                                  QStringLiteral("--wait"),
                                  QString(),
                                  QStringLiteral("--"),
                                  spec.program,
                                  spec.executeFlag,
                                  command});
        } else {
            process.setProcessEnvironment(env);
            process.setProgram(spec.program);
            process.setArguments({spec.executeFlag, command});
        }
    };

    // QCustomTask's done handler takes the task as const reference, but the
    // process is still alive and owned by the task tree at this point, so
    // casting away const to drain the output buffer is safe.
    const auto onProcessDone = [state](const QProcess &process, DoneWith) {
        QProcess *mutableProcess = const_cast<QProcess *>(&process);
        state->output = QString::fromUtf8(mutableProcess->readAllStandardOutput());
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

QString BashTool::detailsMarkdown(const QJsonObject &arguments, const QString &result) const
{
    const QString command = arguments.value("command").toString();
    QString md = QStringLiteral("```bash\n%1\n```\n").arg(command);
    const QString workdir = arguments.value("workdir").toString();
    if (!workdir.isEmpty())
        md += Tr::tr("Working directory: %1\n").arg(workdir);
    md += QStringLiteral("\n```\n%1\n```").arg(result);
    return md;
}

} // namespace LlamaCpp::Tools
