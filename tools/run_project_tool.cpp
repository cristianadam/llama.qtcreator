#include "run_project_tool.h"

#include "factory.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/buildmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectexplorerconstants.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/runconfiguration.h>
#include <projectexplorer/runcontrol.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp::Tools {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(RunProjectTool{}.name(),
                                            []() { return std::make_unique<RunProjectTool>(); });
    return true;
}();
} // namespace

QString RunProjectTool::name() const
{
    return QStringLiteral("run_project");
}

QString RunProjectTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "run_project",
            "description": "Run a Qt Creator project. Captures the program's standard output and returns it.",
            "parameters": {
                "type": "object",
                "properties": {
                    "project_path": {
                        "type": "string",
                        "description": "Path to the project file (.pro, .qmlproject, CMakeLists.txt)."
                    }
                },
                "required": [ "project_path" ],
                "additionalProperties": false,
                "strict": true
            }
        }
    })raw";
}

QString RunProjectTool::oneLineSummary(const QJsonObject &args) const
{
    const QString proj = args.value("project_path").toString();
    return Tr::tr("run project %1").arg(proj);
}

void RunProjectTool::run(const QJsonObject &args,
                         std::function<void(const QString &, bool)> done) const
{
    const FilePath base = Core::DocumentManager::projectsDirectory();
    const FilePath rel = FilePath::fromUserInput(args.value("project_path").toString().trimmed());

    if (rel.isEmpty())
        return done(Tr::tr("project_path argument is empty."), false);

    const FilePath projectFile = rel.isAbsolutePath()
                                     ? rel
                                     : base.pathAppended(rel.path()).absoluteFilePath();

    Project *project = ProjectManager::projectForFile(projectFile);
    if (!project)
        return done(Tr::tr("Failed to open project \"%1\"").arg(projectFile.toUserOutput()), false);

    RunConfiguration *runConfig = activeRunConfig(project);
    if (!runConfig) {
        return done(Tr::tr("No run configuration found for project \"%1\"")
                        .arg(project->displayName()),
                    false);
    }

    RunControl *runControl = new RunControl(Constants::NORMAL_RUN_MODE);
    runControl->copyDataFromRunConfiguration(runConfig);

    QObject::connect(runControl, &RunControl::stopped, [runControl]() {
        runControl->deleteLater();
    });

    struct Data
    {
        QString stdOutBuf;
        QString stdErrBuf;

        QMetaObject::Connection procDoneConn;
        QMetaObject::Connection procStdOutConn;
        QMetaObject::Connection procStdErrConn;
    };
    const Tasking::Storage<Data> outputStorage;

    auto modifier =
        [outputStorage, projectFile, done](Utils::Process &proc) -> Tasking::SetupResult {
        Data *d = outputStorage.activeStorage();

        d->procStdOutConn = QObject::connect(&proc, &Process::readyReadStandardOutput, [d, &proc]() {
            d->stdOutBuf.append(proc.readAllStandardOutput());
        });

        d->procStdErrConn = QObject::connect(&proc, &Process::readyReadStandardError, [d, &proc]() {
            d->stdErrBuf.append(proc.readAllStandardError());
        });

        d->procDoneConn = QObject::connect(&proc, &Process::done, [d, &proc, projectFile, done]() {
            QObject::disconnect(d->procDoneConn);
            QObject::disconnect(d->procStdOutConn);
            QObject::disconnect(d->procStdErrConn);

            const bool ok = proc.exitCode() == 0;
            QString result;
            if (ok) {
                result = Tr::tr("Run succeeded for \"%1\".\n--- stdout ---\n%2")
                             .arg(projectFile.toUserOutput(), d->stdOutBuf);
                if (!d->stdErrBuf.isEmpty()) {
                    result += Tr::tr("\n--- stderr ---\n%1").arg(d->stdErrBuf);
                }
            } else {
                result = Tr::tr("Run failed for \"%1\".\n--- stdout ---\n%2")
                             .arg(projectFile.toUserOutput(), d->stdOutBuf);
                if (!d->stdErrBuf.isEmpty())
                    result += Tr::tr("\n--- stderr ---\n%1").arg(d->stdErrBuf);
            }
            done(result, ok);
        });

        return Tasking::SetupResult::Continue;
    };

    const Tasking::Group recipe = Tasking::Group{outputStorage, runControl->processTask(modifier)};
    runControl->setRunRecipe(recipe);

    runControl->initiateStart();
}

} // namespace LlamaCpp::Tools
