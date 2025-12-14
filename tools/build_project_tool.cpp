#include "build_project_tool.h"

#include "factory.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/buildmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/target.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp::Tools {

namespace {
/* Register the tool with the global factory when the library is loaded */
const bool registered = [] {
    ToolFactory::instance().registerCreator(BuildProjectTool{}.name(),
                                            []() { return std::make_unique<BuildProjectTool>(); });
    return true;
}();
} // namespace

QString BuildProjectTool::name() const
{
    return QStringLiteral("build_project");
}

QString BuildProjectTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "build_project",
            "description": "Build an existing Qt Creator project file (.pro, .qmlproject, CMakeLists.txt). The path may be absolute or relative to the current project directory.",
            "parameters": {
                "type": "object",
                "properties": {
                    "project_path": {
                        "type": "string",
                        "description": "Path to the project file to build."
                    }
                },
                "required": [ "project_path" ],
                "strict": true
            }
        }
    })raw";
}

QString BuildProjectTool::oneLineSummary(const QJsonObject &args) const
{
    const QString rel = args.value("project_path").toString();
    return Tr::tr("build project %1").arg(rel);
}

void BuildProjectTool::run(const QJsonObject &args,
                           std::function<void(const QString &, bool)> done) const
{
    FilePath base = Core::DocumentManager::projectsDirectory();

    const FilePath relPath = FilePath::fromUserInput(
        args.value("project_path").toString().trimmed());

    if (relPath.isEmpty())
        return done(Tr::tr("project_path argument is empty."), false);

    const FilePath target = relPath.isAbsolutePath()
                                ? relPath
                                : base.pathAppended(relPath.path()).absoluteFilePath();

    Project *project = nullptr;

    for (Project *p : ProjectManager::projects()) {
        if (p && p->projectFilePath() == target) {
            project = p;
            break;
        }
    }

    if (!project) {
        const OpenProjectResult openResult = ProjectExplorerPlugin::openProject(target);
        if (!openResult) {
            return done(Tr::tr("Failed to open project \"%1\": %2")
                            .arg(target.toUserOutput(), openResult.errorMessage()),
                        false);
        }
        project = openResult.project(); // usually the only project in the result
    }

    if (!project) {
        return done(Tr::tr("Could not determine a project to build for \"%1\".")
                        .arg(target.toUserOutput()),
                    false);
    }

    QMetaObject::Connection conn;
    conn = QObject::connect(BuildManager::instance(),
                            &BuildManager::buildQueueFinished,
                            [done, conn, target](bool success) mutable {
                                // Disconnect immediately – we only care about the first emission.
                                QObject::disconnect(conn);

                                const QString msg = success ? Tr::tr("Build succeeded for \"%1\".")
                                                                  .arg(target.toUserOutput())
                                                            : Tr::tr("Build failed for \"%1\".")
                                                                  .arg(target.toUserOutput());

                                done(msg, success);
                            });

    BuildManager::buildProjectWithoutDependencies(project);
}
} // namespace LlamaCpp::Tools
