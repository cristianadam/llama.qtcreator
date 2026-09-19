#include "shell_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <QtTaskTree/qprocesstask.h>
#include <QtTaskTree/qtasktree.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QUuid>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/environment.h>
#include <utils/filepath.h>

using namespace ProjectExplorer;
using namespace Utils;
using namespace QtTaskTree;

namespace LlamaCpp::Tools {

namespace {

constexpr int kDefaultTimeoutMs = 120 * 1000;
constexpr int kMaxTimeoutMs = 10 * 60 * 1000;
constexpr int kMaxOutputLines = 2000;
constexpr int kMaxOutputBytes = 50 * 1024; // 50 KB

struct ShellSpec
{
    QString program;
    QString executeFlag;
};

ShellSpec shellSpec()
{
#if defined(Q_OS_WIN)
    const FilePath cmd = FilePath{}.findCmdExe();
    return { cmd.toUserOutput(), QStringLiteral("/c") };
#else
    const QString shell = qEnvironmentVariable("SHELL");
    return { shell.isEmpty() ? QStringLiteral("/bin/sh") : shell, QStringLiteral("-c") };
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

/*! Saves \a text to a uniquely named temporary file so that truncated output
    can be recovered. Returns an empty string when saving fails. */
QString saveFullOutput(const QString &text)
{
    const QString path
        = QDir::tempPath() + QStringLiteral("/llama-shell-")
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
// tree done handler.
struct ShellState
{
    bool timedOut = false;
    QProcess::ProcessError processError = QProcess::UnknownError;
    QString errorString;
    QProcess::ExitStatus exitStatus = QProcess::NormalExit;
    int exitCode = -1;
    QString output;
};

} // namespace

const bool registered = [] {
    ToolFactory::instance().registerCreator(ShellTool{}.name(),
                                            []() { return std::make_unique<ShellTool>(); });
    return true;
}();

QString ShellTool::name() const
{
    return QStringLiteral("shell");
}

QString ShellTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "shell",
            "description": "Executes a shell command and returns its combined output (stdout and stderr). Use this for terminal operations like git, npm, docker, running builds or tests. Do not use it for reading, writing, editing or searching files - use the dedicated tools for that instead. Output is limited to the last 2000 lines or 50 KB; if it is truncated, the full output is saved to a temporary file and its path is reported. Non-zero exit codes, crashes and timeouts are reported as failures together with the output produced so far.",
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

QString ShellTool::oneLineSummary(const QJsonObject &arguments) const
{
    return Tr::tr("running %1").arg(arguments.value("command").toString());
}

void ShellTool::run(const QJsonObject &arguments,
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

    const ShellSpec spec = shellSpec();
    const QProcessEnvironment env = shellEnvironment();

    auto state = std::make_shared<ShellState>();

    const auto onSetup = [cwdString, env, spec, command](QProcess &process) {
        process.setWorkingDirectory(cwdString);
        process.setProcessEnvironment(env);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.setProgram(spec.program);
        process.setArguments({spec.executeFlag, command});
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

    const auto onTimeout = [state] { state->timedOut = true; };

    QTaskTree *tree = new QTaskTree;
    tree->setRecipe(Group{QProcessTask(onSetup, onProcessDone)
                              .withTimeout(std::chrono::milliseconds(timeoutMs), onTimeout)});

    // Builds the model-facing result from whatever the process managed to
    // produce, then reports it and cleans up.
    const auto finish = [tree, state, done = std::move(done), timeoutMs](DoneWith) mutable {
        tree->deleteLater();

        QStringList notes;
        bool ok = true;

        if (state->timedOut) {
            ok = false;
            notes << Tr::tr("Command timed out after %1 ms. Retry with a larger "
                            "timeout if the command is expected to take longer.")
                          .arg(timeoutMs);
        } else if (state->processError == QProcess::FailedToStart) {
            ok = false;
            notes << Tr::tr("Failed to start the command: %1")
                          .arg(state->errorString.isEmpty()
                                   ? Tr::tr("unknown error")
                                   : state->errorString);
        } else if (state->exitStatus == QProcess::CrashExit) {
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

        done(text, ok);
    };

    // Keep 'tree' alive until the recipe finished; the connection is
    // destroyed together with the tree once it is deleted.
    QObject::connect(tree, &QTaskTree::done, tree, finish);
    tree->start();
}

QString ShellTool::detailsMarkdown(const QJsonObject &arguments, const QString &result) const
{
    const QString command = arguments.value("command").toString();
    QString md = QStringLiteral("```sh\n%1\n```\n").arg(command);
    const QString workdir = arguments.value("workdir").toString();
    if (!workdir.isEmpty())
        md += Tr::tr("Working directory: %1\n").arg(workdir);
    md += QStringLiteral("\n```\n%1\n```").arg(result);
    return md;
}

} // namespace LlamaCpp::Tools
