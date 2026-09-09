#include "writefile_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "tool_utils.h"

#include <QFileInfo>
#include <utils/filepath.h>

using namespace Utils;

namespace LlamaCpp {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(WriteFileTool{}.name(),
                                            []() { return std::make_unique<WriteFileTool>(); });
    return true;
}();
} // namespace

QString WriteFileTool::name() const
{
    return QStringLiteral("write_file");
}

QString WriteFileTool::toolDefinition() const
{
    return R"raw(
    {
      "type": "function",
      "function": {
        "name": "write_file",
        "description": "Write content to a file, replacing the entire file. Creates the file if it does not exist, or overwrites if it does. Use this for creating new files or replacing entire file contents.",
        "parameters": {
          "type": "object",
          "properties": {
            "file_path": {
              "type": "string",
              "description": "Path of the target file, relative to the workspace root (or absolute)."
            },
            "content": {
              "type": "string",
              "description": "The complete content to write to the file."
            }
          },
          "required": ["file_path", "content"],
          "additionalProperties": false
        }
      }
    })raw";
}

QString WriteFileTool::oneLineSummary(const QJsonObject &args) const
{
    const QString path = args.value("file_path").toString();
    return Tr::tr("write file %1").arg(path);
}

QString WriteFileTool::detailsMarkdown(const QJsonObject &args, const QString &result) const
{
    const QString filePath = args.value("file_path").toString();
    const QString content = args.value("content").toString();
    
    if (result.contains("failed") || result.contains("Error") || result.contains("error"))
        return result;
    
    QString lang;
    const QString fileName = QFileInfo(filePath).fileName().toLower();
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (fileName == "cmakelists.txt" || suffix == "cmake")
        lang = "cmake";
    else if (suffix == "cpp" || suffix == "cc" || suffix == "cxx" || suffix == "c++")
        lang = "cpp";
    else if (suffix == "h" || suffix == "hpp" || suffix == "hxx")
        lang = "cpp";
    else if (suffix == "py")
        lang = "python";
    else if (suffix == "js" || suffix == "jsx")
        lang = "javascript";
    else if (suffix == "ts" || suffix == "tsx")
        lang = "typescript";
    else if (suffix == "json")
        lang = "json";
    else if (suffix == "xml")
        lang = "xml";
    else if (suffix == "yaml" || suffix == "yml")
        lang = "yaml";
    else if (suffix == "md")
        lang = "markdown";
    else if (suffix == "html")
        lang = "html";
    else if (suffix == "css")
        lang = "css";
    else if (suffix == "sh" || suffix == "bash")
        lang = "bash";
    else if (suffix == "cmake")
        lang = "cmake";
    else if (suffix == "pro")
        lang = "text";
    else if (suffix == "qml")
        lang = "javascript";
    else if (suffix == "sql")
        lang = "sql";
    else if (suffix == "rs")
        lang = "rust";
    else if (suffix == "go")
        lang = "go";
    else if (suffix == "java")
        lang = "java";
    else if (suffix == "rb")
        lang = "ruby";
    else if (suffix == "php")
        lang = "php";
    else if (suffix == "swift")
        lang = "swift";
    else if (suffix == "kt" || suffix == "kts")
        lang = "kotlin";
    
    return QString("**%1** `%2`\n\n```%3\n%4\n```")
        .arg(Tr::tr("wrote"), filePath, lang.isEmpty() ? "text" : lang, content);
}

void WriteFileTool::run(const QJsonObject &args,
                        std::function<void(const QString &, bool)> done) const
{
    const QString filePathStr = args.value("file_path").toString();
    const QString content = args.value("content").toString();

    if (filePathStr.isEmpty()) {
        return done(Tr::tr("Tool error: \"file_path\" must be a non-empty string."), false);
    }
    if (content.isEmpty()) {
        return done(Tr::tr("Tool error: \"content\" must be a non-empty string."), false);
    }

    const FilePath filePath = FilePath::fromUserInput(filePathStr);
    const FilePath targetFile = absoluteProjectPath(filePath);

    // Create parent directories if needed
    const FilePath parentDir = targetFile.parentDir();
    if (!parentDir.exists()) {
        const Result<> mkRes = parentDir.ensureWritableDir();
        if (!mkRes) {
            return done(Tr::tr("Failed to create parent directory \"%1\": %2")
                            .arg(parentDir.toUserOutput(), mkRes.error()),
                        false);
        }
    }

    // Write the file
    const Result<qint64> writeRes = targetFile.writeFileContents(content.toUtf8());
    if (!writeRes) {
        return done(Tr::tr("Cannot write \"%1\": %2").arg(targetFile.toUserOutput(), writeRes.error()),
                    false);
    }

    return done(Tr::tr("Created %1").arg(targetFile.toUserOutput()), true);
}

} // namespace LlamaCpp
