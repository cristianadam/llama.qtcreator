#include "create_directory_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp::Tools {

namespace {
// Register the tool with the global factory as soon as the translation unit
// is loaded.
const bool registered = [] {
    ToolFactory::instance().registerCreator(CreateDirectoryTool{}.name(), []() {
        return std::make_unique<CreateDirectoryTool>();
    });
    return true;
}();
} // namespace

QString CreateDirectoryTool::name() const
{
    return QStringLiteral("create_directory");
}

QString CreateDirectoryTool::toolDefinition() const
{
    // The JSON is wrapped in a raw string literal to keep the formatting
    // exactly as required by the LLM‑tool spec.
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "create_directory",
            "description": "Create a new directory at the given path. The path is interpreted relative to the current project directory, or to the generic projects directory if no project is opened.",
            "parameters": {
                "type": "object",
                "properties": {
                    "dir_path": {
                        "type": "string",
                        "description": "Relative path of the directory to create."
                    }
                },
                "required": [ "dir_path" ],
                "strict": true
            }
        }
    })raw";
}

QString CreateDirectoryTool::oneLineSummary(const QJsonObject &args) const
{
    const QString rel = args.value("dir_path").toString();
    return Tr::tr("create directory %1").arg(rel);
}

void CreateDirectoryTool::run(const QJsonObject &args,
                              std::function<void(const QString &, bool)> done) const
{
    FilePath base = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        base = p->projectDirectory();

    const QString relPath = args.value("dir_path").toString().trimmed();

    if (relPath.isEmpty()) {
        return done(Tr::tr("dir_path argument is empty."), false);
    }

    // Resolve the final target path
    FilePath target = base.pathAppended(relPath);

    // If the directory already exists we treat it as success (idempotent).
    if (target.exists())
        return done(Tr::tr("Directory \"%1\" already exists.").arg(target.toUserOutput()), true);

    const bool ok = target.createDir();
    if (!ok)
        return done(Tr::tr("Failed to create directory \"%1\".").arg(target.toUserOutput()), false);

    return done(Tr::tr("Successfully created directory \"%1\".").arg(target.toUserOutput()), true);
}

} // namespace LlamaCpp::Tools
