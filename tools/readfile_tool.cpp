#include "readfile_tool.h"
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
const bool registered = [] {
    ToolFactory::instance().registerCreator(ReadFileTool{}.name(),
                                            []() { return std::make_unique<ReadFileTool>(); });
    return true;
}();
} // namespace

QString ReadFileTool::name() const
{
    return QStringLiteral("read_file");
}

QString ReadFileTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "read_file",
            "description": "Read the contents of a file from first_line to last_line_inclusive, at most 250 lines at a time or the entire file if parameter should_read_entire_file is true.",
            "parameters": {
                "type": "object",
                "properties": {
                    "first_line": { "type": "integer", "description": "The number of first line to read. Starts with 1." },
                    "last_line_inclusive": { "type": "integer", "description": "The number of last line to read. Line numbers start with 1" },
                    "should_read_entire_file": { "type": "boolean", "description": "Whether to read the entire file. Defaults to false." },
                    "file_path": { "type": "string", "description": "The path of the file to read. The path may be absolute or relative to the current project directory." }
                },
                "required": [ "first_line", "last_line_inclusive", "file_path" ],
                "strict": true
            }
        }
    })raw";
}

QString ReadFileTool::oneLineSummary(const QJsonObject &args) const
{
    const QString file = args.value("file_path").toString();
    const bool all = args.value("should_read_entire_file").toBool(false);
    if (all)
        return Tr::tr("read whole file %1").arg(file);

    int first = args.value("first_line").toInt(1);
    int last = args.value("last_line_inclusive").toInt(first);
    return Tr::tr("read %1:%2‑%3").arg(file).arg(first).arg(last);
}

void ReadFileTool::run(const QJsonObject &args,
                       std::function<void(const QString &, bool)> done) const
{
    const FilePath filePath = FilePath::fromUserInput(args.value("file_path").toString());
    int firstLine = args.value("first_line").toInt(1);
    int lastLineIncl = args.value("last_line_inclusive").toInt(firstLine);
    bool readAll = args.value("should_read_entire_file").toBool(false);

    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    const FilePath targetFile = filePath.isAbsolutePath() ? filePath
                                                          : cwd.pathAppended(filePath.path());

    if (!targetFile.exists()) {
        return done(Tr::tr("File \"%1\" does not exist.").arg(targetFile.toUserOutput()), false);
    }

    const Result<QByteArray> readRes = targetFile.fileContents();
    if (!readRes) {
        return done(Tr::tr("Failed to read \"%1\": %2")
                        .arg(targetFile.toUserOutput(), readRes.error()),
                    false);
    }

    const QString fileText = QString::fromUtf8(readRes.value());
    const QStringList allLines = fileText.split('\n', Qt::KeepEmptyParts);

    if (readAll) {
        return done(fileText, true);
    }

    if (firstLine < 1) {
        return done(Tr::tr("first_line must be >= 1."), false);
    }
    if (lastLineIncl < firstLine) {
        return done(Tr::tr("last_line_inclusive must be >= first_line."), false);
    }

    // Compute the number of lines requested
    int requestedLines = lastLineIncl - firstLine + 1;

    // Enforce the 250‑line hard limit
    if (requestedLines > 250) {
        requestedLines = 250;
    }

    const int startIdx = firstLine - 1; // zero‑based
    const int endIdx = qMin(startIdx + requestedLines, allLines.size());

    if (startIdx >= allLines.size()) {
        return done(Tr::tr("first_line (%1) exceeds the number of lines in \"%2\" (%3).")
                        .arg(firstLine)
                        .arg(filePath.toUserOutput())
                        .arg(allLines.size()),
                    false);
    }

    const QStringList slice = allLines.mid(startIdx, endIdx - startIdx);
    return done(slice.join('\n'), true);
}
} // namespace LlamaCpp::Tools
