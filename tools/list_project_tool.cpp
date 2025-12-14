#include "list_project_tool.h"

#include "factory.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectexplorer.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp::Tools {

namespace {
/* Register the tool with the global factory when the library is loaded */
const bool registered = [] {
    ToolFactory::instance().registerCreator(ListProjectFilesTool{}.name(), []() {
        return std::make_unique<ListProjectFilesTool>();
    });
    return true;
}();
} // namespace

QString ListProjectFilesTool::name() const
{
    return QStringLiteral("list_project_files");
}

QString ListProjectFilesTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "list_project_files",
            "description": "List all files that belong to a Qt Creator project (.pro, .qmlproject, CMakeLists.txt). The path may be absolute or relative to the default projects directory.",
            "parameters": {
                "type": "object",
                "properties": {
                    "project_path": {
                        "type": "string",
                        "description": "Path to the project file to inspect."
                    }
                },
                "required": [ "project_path" ],
                "strict": true
            }
        }
    })raw";
}

QString ListProjectFilesTool::oneLineSummary(const QJsonObject &args) const
{
    const QString rel = args.value("project_path").toString();
    return QStringLiteral("list files of project %1").arg(rel);
}

void ListProjectFilesTool::run(const QJsonObject &args,
                               std::function<void(const QString &, bool)> done) const
{
    const FilePath base = Core::DocumentManager::projectsDirectory();

    const FilePath relPath = FilePath::fromUserInput(
        args.value("project_path").toString().trimmed());

    if (relPath.isEmpty())
        return done(Tr::tr("project_path argument is empty."), false);

    const FilePath projectFileName = relPath.isAbsolutePath()
                                         ? relPath
                                         : base.pathAppended(relPath.path()).absoluteFilePath();

    Project *project = ProjectManager::projectForFile(projectFileName);
    if (!project) {
        const OpenProjectResult openResult = ProjectExplorerPlugin::openProject(projectFileName);
        if (!openResult) {
            return done(Tr::tr("Failed to open project \"%1\": %2")
                            .arg(projectFileName.toUserOutput(), openResult.errorMessage()),
                        false);
        }
        project = openResult.project(); // usually the only project in the result
    }

    if (!project) {
        return done(Tr::tr("Could not determine a project to list files for \"%1\".")
                        .arg(projectFileName.toUserOutput()),
                    false);
    }

    const FilePaths files = project->files(Project::AllFiles);
    QStringList textualFiles;
    textualFiles.reserve(files.size());
    for (const FilePath &fp : files)
        textualFiles << fp.toUserOutput();

    const QString output = textualFiles.join(QLatin1Char('\n'));
    done(output, true);
}

} // namespace LlamaCpp::Tools
