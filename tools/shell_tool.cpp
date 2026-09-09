#include "shell_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QTimer>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/kit.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/environment.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp::Tools {

namespace {
constexpr int kDefaultTimeoutMs = 120 * 1000;
constexpr int kMaxTimeoutMs = 10 * 60 * 1000;

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

void startShell(const QString &command, const FilePath &cwd, int timeoutMs,
                std::function<void(const QString &, bool)> done)
{
    QProcess *proc = new QProcess();
    proc->setWorkingDirectory(cwd.toFSPathString());
    proc->setProcessEnvironment(shellEnvironment());
    proc->setProcessChannelMode(QProcess::MergedChannels);

    QTimer *timer = new QTimer();
    QObject::connect(timer, &QTimer::timeout, proc, &QProcess::kill);

    QObject::connect(proc,
                     &QProcess::finished,
                     [proc, timer, done](int exitCode, QProcess::ExitStatus /*status*/) {
                         timer->deleteLater();
                         QString out = QString::fromUtf8(proc->readAllStandardOutput());

                         done(out, exitCode == 0);
                         proc->deleteLater();
                     });

    const ShellSpec spec = shellSpec();
    proc->start(spec.program, QStringList{spec.executeFlag, command});
    timer->start(timeoutMs);
}

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
            "description": "Executes a shell command and returns its combined output. Use this for terminal operations like git, npm, docker, running builds, etc. Do not use it for reading, writing, editing or searching files - use the dedicated tools for that instead.",
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
    const QString command = arguments.value("command").toString();

    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    const QString workdir = arguments.value("workdir").toString();
    if (!workdir.isEmpty()) {
        const FilePath wd = FilePath::fromUserInput(workdir);
        cwd = wd.isAbsolutePath() ? wd : cwd.pathAppended(workdir);
    }

    int timeoutMs = arguments.value("timeout").toInt(kDefaultTimeoutMs);
    timeoutMs = qBound(1, timeoutMs, kMaxTimeoutMs);

    startShell(command, cwd, timeoutMs, std::move(done));
}

QString ShellTool::detailsMarkdown(const QJsonObject &arguments, const QString &result) const
{
    const QString command = arguments.value("command").toString();
    return QString("```sh\n%1\n```\n\n```\n%2\n```")
        .arg(command, result);
}
} // namespace LlamaCpp::Tools
