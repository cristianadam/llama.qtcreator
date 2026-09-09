#include "deletefile_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "tool_utils.h"

#include <utils/filepath.h>

using namespace Utils;

namespace LlamaCpp {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(DeleteFileTool{}.name(),
                                            []() { return std::make_unique<DeleteFileTool>(); });
    return true;
}();
} // namespace

QString DeleteFileTool::name() const
{
    return QStringLiteral("delete_file");
}

QString DeleteFileTool::toolDefinition() const
{
    return R"raw(
    {
      "type": "function",
      "function": {
        "name": "delete_file",
        "description": "Delete a file from the filesystem. The file must exist. This operation cannot be undone.",
        "parameters": {
          "type": "object",
          "properties": {
            "file_path": {
              "type": "string",
              "description": "Path of the file to delete, relative to the workspace root (or absolute)."
            }
          },
          "required": ["file_path"],
          "additionalProperties": false
        }
      }
    })raw";
}

QString DeleteFileTool::oneLineSummary(const QJsonObject &args) const
{
    const QString path = args.value("file_path").toString();
    return Tr::tr("delete file %1").arg(path);
}

QString DeleteFileTool::detailsMarkdown(const QJsonObject &args, const QString &result) const
{
    const QString filePath = args.value("file_path").toString();
    
    if (result.contains("failed") || result.contains("Error") || result.contains("error"))
        return result;
    
    return QString("**%1** `%2`").arg(Tr::tr("deleted"), filePath);
}

void DeleteFileTool::run(const QJsonObject &args,
                         std::function<void(const QString &, bool)> done) const
{
    const QString filePathStr = args.value("file_path").toString();

    if (filePathStr.isEmpty()) {
        return done(Tr::tr("Tool error: \"file_path\" must be a non-empty string."), false);
    }

    const FilePath filePath = FilePath::fromUserInput(filePathStr);
    const FilePath targetFile = absoluteProjectPath(filePath);

    if (!targetFile.exists()) {
        return done(Tr::tr("File \"%1\" does not exist.").arg(targetFile.toUserOutput()), false);
    }

    Result<> removeRes = targetFile.removeFile();
    if (!removeRes) {
        return done(Tr::tr("Failed to delete \"%1\": %2")
                        .arg(targetFile.toUserOutput(), removeRes.error()),
                    false);
    }

    return done(Tr::tr("Deleted file \"%1\"").arg(targetFile.toUserOutput()), true);
}

} // namespace LlamaCpp
