#include "open_project_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/buildsystem.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp::Tools {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(OpenProjectTool{}.name(),
                                            []() { return std::make_unique<OpenProjectTool>(); });
    return true;
}();
} // namespace

QString OpenProjectTool::name() const
{
    return QStringLiteral("open_project");
}

QString OpenProjectTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "open_project",
            "description": "Open an existing Qt Creator project file (.pro, .qmlproject, CMakeLists.txt). The path may be absolute or relative to the current project directory (or the generic projects directory if no project is opened).",
            "parameters": {
                "type": "object",
                "properties": {
                    "project_path": {
                        "type": "string",
                        "description": "Path to the project file to open."
                    }
                },
                "required": [ "project_path" ],
                "strict": true
            }
        }
    })raw";
}

QString OpenProjectTool::oneLineSummary(const QJsonObject &args) const
{
    const QString rel = args.value("project_path").toString();
    return Tr::tr("open project %1").arg(rel);
}

void OpenProjectTool::run(const QJsonObject &args,
                          std::function<void(const QString &, bool)> done) const
{
    // Resolve base directory the same way other tools do:
    FilePath base = Core::DocumentManager::projectsDirectory();

    const FilePath relPath = FilePath::fromUserInput(
        args.value("project_path").toString().trimmed());
    if (relPath.isEmpty())
        return done(Tr::tr("project_path argument is empty."), false);

    const FilePath target = relPath.isAbsolutePath()
                                ? relPath
                                : base.pathAppended(relPath.path()).absoluteFilePath();

    const OpenProjectResult result = ProjectExplorerPlugin::openProject(target);
    if (!result.alreadyOpen().isEmpty())
        return done(Tr::tr("Project is already opened \"%1\".").arg(target.toUserOutput()), true);

    Project *project = result.project();
    if (project) {
        if (project->needsConfiguration()) {
            auto parsingFinishedConn = new QMetaObject::Connection;
            *parsingFinishedConn = QObject::connect(
                ProjectManager::instance(),
                &ProjectManager::projectFinishedParsing,
                [done, target, parsingFinishedConn](Project *project) {
                    QObject::disconnect(*parsingFinishedConn);
                    delete parsingFinishedConn;

                    const bool success = project->activeBuildSystem()
                                             ? project->activeBuildSystem()->hasParsingData()
                                             : false;

                    if (success)
                        done(Tr::tr("Successfully opened project \"%1\".").arg(target.toUserOutput()),
                             true);
                    else
                        return done(Tr::tr(
                                        "Failed to open project \"%1\". No build system or failed "
                                        "to parse project.")
                                        .arg(target.toUserOutput()),
                                    false);
                });
        } else {
            done(Tr::tr("Successfully opened project \"%1\".").arg(target.toUserOutput()), true);
        }
    } else {
        return done(Tr::tr("Failed to open project \"%1\". No project found.")
                        .arg(target.toUserOutput()),
                    false);
    }

    if (!result) {
        return done(Tr::tr("Failed to open project \"%1\": %2")
                        .arg(target.toUserOutput(), result.errorMessage()),
                    false);
    }
}

} // namespace LlamaCpp::Tools
