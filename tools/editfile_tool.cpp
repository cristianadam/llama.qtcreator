#include "editfile_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "tool_utils.h"

#include <utils/filepath.h>

using namespace Utils;

namespace LlamaCpp {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(EditFileTool{}.name(),
                                            []() { return std::make_unique<EditFileTool>(); });
    return true;
}();
} // namespace

QString EditFileTool::name() const
{
    return QStringLiteral("edit_file");
}

QString EditFileTool::toolDefinition() const
{
    return R"raw(
    {
      "type": "function",
      "function": {
        "name": "edit_file",
        "description": "Edit an existing file, create a new file or delete a file.",
        "parameters": {
          "type": "object",
          "properties": {
            "file_path": {
              "type": "string",
              "description": "Path of the target file, relative to the workspace root (or absolute)."
            },

            "operation": {
              "type": "string",
              "enum": ["replace", "insert", "delete", "create", "delete_file"],
              "description": "What should be done with the file.
                             - **replace** – replace the line(s) from `line` to `end_line` (inclusive) with `text`.
                             - **insert** – insert `text` **before** the line at `line`.
                             - **delete** – delete the line(s) from `line` to `end_line` (inclusive).
                             - **create** – create a brand‑new file (the file must not exist).
                             - **delete_file** – delete the file entirely."
            },

            "line": {
              "type": "integer",
              "minimum": 1,
              "description": "1‑based start line number the operation refers to. Required for replace, insert and delete."
            },

            "end_line": {
              "type": "integer",
              "minimum": 1,
              "description": "1‑based inclusive end line number. Optional – if omitted the operation works on a single line (`line`)."
            },

            "text": {
              "type": "string",
              "description": "The new line(s) (may contain new‑lines) for replace/insert operations."
            },

            "new_file_content": {
              "type": "string",
              "description": "Full content of the file when **operation** is **create**."
            }
          },
          "required": ["file_path", "operation"],
          "allOf": [
            {
              "if": { "properties": { "operation": { "const": "replace" } } },
              "then": { "required": ["line", "text"] }
            },
            {
              "if": { "properties": { "operation": { "const": "insert" } } },
              "then": { "required": ["line", "text"] }
            },
            {
              "if": { "properties": { "operation": { "const": "delete" } } },
              "then": { "required": ["line"] }
            },
            {
              "if": { "properties": { "operation": { "const": "create" } } },
              "then": { "required": ["new_file_content"] }
            }
          ],
          "additionalProperties": false
        }
      }
    })raw";
}

QString EditFileTool::oneLineSummary(const QJsonObject &args) const
{
    const QString path = args.value("file_path").toString();
    const QString op = args.value("operation").toString();
    const int line = args.value("line").toInt(-1);
    const int endLine = args.value("end_line").toInt(-1);
    static const QMap<QString, QString> opMap{
        {"create", Tr::tr("create file")},
        {"delete_file", Tr::tr("delete file")},
        {"replace", Tr::tr("replace line(s)")},
        {"insert", Tr::tr("insert at line")},
        {"delete", Tr::tr("delete line(s)")},
    };
    QString desc = opMap.value(op, op);
    if (line > 0 && (op == "replace" || op == "insert" || op == "delete")) {
        QString linePart = QString::number(line);
        if (endLine > line)
            linePart += QStringLiteral("‑%1").arg(endLine);
        desc = QString("%1 %2").arg(desc, linePart);
    } else {
        desc = QString("%1 %2").arg(desc, path);
    }
    return desc;
}

QString createFile(const FilePath &targetFile, const QString &content)
{
    if (targetFile.exists())
        return Tr::tr("File \"%1\" already exists – cannot create a new file at this location.")
            .arg(targetFile.toUserOutput());

    const FilePath parentDir = targetFile.parentDir();
    if (!parentDir.exists()) {
        const Result<> mkRes = parentDir.ensureWritableDir();
        if (!mkRes)
            return Tr::tr("Failed to create parent directory \"%1\": %2")
                .arg(parentDir.toUserOutput(), mkRes.error());
    }

    const Result<qint64> writeRes = targetFile.writeFileContents(content.toUtf8());
    if (!writeRes)
        return Tr::tr("Cannot create \"%1\": %2").arg(targetFile.toUserOutput(), writeRes.error());

    return Tr::tr("Created %1").arg(targetFile.toUserOutput());
}

QString deleteFile(const FilePath &targetFile)
{
    if (!targetFile.exists())
        return Tr::tr("File \"%1\" does not exist.").arg(targetFile.toUserOutput());

    Result<> removeRes = targetFile.removeFile();
    if (!removeRes)
        return Tr::tr("Failed to delete \"%1\": %2")
            .arg(targetFile.toUserOutput(), removeRes.error());

    return Tr::tr("Deleted file \"%1\"").arg(targetFile.toUserOutput());
}

QString editFile(const QString &path,
                 const QString &operation,
                 int line,            // 1‑based start line, -1 if N/A
                 int endLine,         // 1‑based inclusive end line, -1 if not supplied
                 const QString &text, // may be empty for delete/create/delete_file
                 const QString &newFileContent)
{
    const FilePath filePath = FilePath::fromUserInput(path);
    const FilePath targetFile = absoluteProjectPath(filePath);

    if (operation == "create")
        return createFile(targetFile, newFileContent);
    if (operation == "delete_file")
        return deleteFile(targetFile);

    if (!targetFile.exists())
        return Tr::tr("File \"%1\" does not exist.").arg(targetFile.toUserOutput());

    const Result<QByteArray> readRes = targetFile.fileContents();
    if (!readRes)
        return Tr::tr("Failed to read \"%1\": %2").arg(targetFile.toUserOutput(), readRes.error());

    // Split into lines – keep empty lines, no trimming
    QStringList fileLines = QString::fromUtf8(readRes.value()).split('\n', Qt::KeepEmptyParts);

    auto rangeFromArgs = [&](int start, int end) -> std::pair<int, int> {
        // If end is not supplied or <= 0, treat as a single line operation
        if (end < start || end < 1)
            return {start - 1, start - 1};
        return {start - 1, end - 1};
    };

    if (operation == "replace") {
        const auto [startIdx, endIdx] = rangeFromArgs(line, endLine);
        if (startIdx < 0 || startIdx >= fileLines.size())
            return Tr::tr("Start line %1 out of range (file has %2 lines).")
                .arg(line)
                .arg(fileLines.size());

        if (endIdx >= fileLines.size())
            return Tr::tr("End line %1 out of range (file has %2 lines).")
                .arg(endLine)
                .arg(fileLines.size());

        // Remove the old range
        for (int i = endIdx; i >= startIdx; --i)
            fileLines.removeAt(i);

        // Insert the new text (may be multi‑line)
        const QStringList newLines = text.split('\n', Qt::KeepEmptyParts);
        for (int i = 0; i < newLines.size(); ++i)
            fileLines.insert(startIdx + i, newLines[i]);

    } else if (operation == "insert") {
        // Insertion before the line at `line`. `end_line` is ignored.
        const int idx = line - 1; // 0‑based insertion point
        if (idx < 0 || idx > fileLines.size())
            return Tr::tr("Line %1 out of range for insertion (file has %2 lines).")
                .arg(line)
                .arg(fileLines.size());

        const QStringList newLines = text.split('\n', Qt::KeepEmptyParts);
        for (int i = 0; i < newLines.size(); ++i)
            fileLines.insert(idx + i, newLines[i]);

    } else if (operation == "delete") {
        const auto [startIdx, endIdx] = rangeFromArgs(line, endLine);
        if (startIdx < 0 || startIdx >= fileLines.size())
            return Tr::tr("Start line %1 out of range (file has %2 lines).")
                .arg(line)
                .arg(fileLines.size());

        if (endIdx >= fileLines.size())
            return Tr::tr("End line %1 out of range (file has %2 lines).")
                .arg(endLine)
                .arg(fileLines.size());

        for (int i = endIdx; i >= startIdx; --i)
            fileLines.removeAt(i);
    } else {
        return Tr::tr("Unsupported operation \"%1\".").arg(operation);
    }

    // Write back the modified content
    const Result<qint64> writeRes = targetFile.writeFileContents(fileLines.join('\n').toUtf8());
    if (!writeRes)
        return Tr::tr("Cannot write \"%1\": %2").arg(targetFile.toUserOutput(), writeRes.error());

    return Tr::tr("Edited %1").arg(targetFile.toUserOutput());
}

QString diffForEditFile(const QString &path,
                        const QString &operation,
                        int line,
                        int endLine,
                        const QString &text,
                        const QString &newFileContent)
{
    const FilePath filePath = FilePath::fromUserInput(path);
    const FilePath targetFile = absoluteProjectPath(filePath);

    QStringList oldLines;
    if (operation != "create") {
        const Result<QByteArray> readRes = targetFile.fileContents();
        if (!readRes)
            return Tr::tr("Failed to read \"%1\": %2")
                .arg(targetFile.toUserOutput(), readRes.error());

        oldLines = QString::fromUtf8(readRes.value()).split('\n', Qt::KeepEmptyParts);
    }

    const QStringList newLines = newFileContent.split('\n', Qt::KeepEmptyParts);
    const QStringList insertLines = text.split('\n', Qt::KeepEmptyParts);

    // Helper to format a diff header
    auto header = [&] {
        QString diff;
        QTextStream out(&diff);
        out << "--- a/" << filePath.toUserOutput() << "\n";
        out << "+++ b/" << filePath.toUserOutput() << "\n";
        return diff;
    };

    if (operation == "create") {
        QString diff = header();
        QTextStream out(&diff);
        out << "@@ -0,0 +" << 1 << "," << newLines.size() << " @@\n";
        for (const QString &ln : newLines)
            out << "+" << ln << "\n";
        return diff;
    }

    if (operation == "delete_file") {
        QString diff = header();
        QTextStream out(&diff);
        out << "@@ -1," << oldLines.size() << " +0,0 @@\n";
        for (const QString &ln : std::as_const(oldLines))
            out << "-" << ln << "\n";
        return diff;
    }

    if (line < 1)
        return Tr::tr("Invalid line number %1 (must be >= 1).").arg(line);
    const int startIdx = line - 1;
    const int endIdx = (endLine >= line) ? endLine - 1
                                         : startIdx; // if endLine missing, use startIdx

    if (operation == "replace") {
        if (startIdx >= oldLines.size())
            return Tr::tr("Start line %1 out of range for replace (file has %2 lines).")
                .arg(line)
                .arg(oldLines.size());
        if (endIdx >= oldLines.size())
            return Tr::tr("End line %1 out of range for replace (file has %2 lines).")
                .arg(endLine)
                .arg(oldLines.size());

        QString diff = header();
        QTextStream out(&diff);
        // Hunk header: show the range being replaced
        out << "@@ -" << line << "," << (endIdx - startIdx + 1) << " +" << line << ","
            << insertLines.size() << " @@\n";

        // Old lines (with '-')
        for (int i = startIdx; i <= endIdx; ++i)
            out << "-" << oldLines.at(i) << "\n";

        // New lines (with '+')
        for (const QString &ln : insertLines)
            out << "+" << ln << "\n";

        return diff;
    }

    if (operation == "insert") {
        // Insertion before line `line`; allowed to insert at EOF (idx == size)
        if (startIdx > oldLines.size())
            return Tr::tr("Line %1 out of range for insert (file has %2 lines).")
                .arg(line)
                .arg(oldLines.size());

        QString diff = header();
        QTextStream out(&diff);
        // Insertion hunk: 0 lines removed, N lines added
        out << "@@ -" << line << ",0 +" << line << "," << insertLines.size() << " @@\n";
        for (const QString &ln : insertLines)
            out << "+" << ln << "\n";

        // Optional context line (the line that follows the insertion)
        if (startIdx < oldLines.size())
            out << " " << oldLines.at(startIdx) << "\n";

        return diff;
    }

    if (operation == "delete") {
        if (startIdx >= oldLines.size())
            return Tr::tr("Start line %1 out of range for delete (file has %2 lines).")
                .arg(line)
                .arg(oldLines.size());
        if (endIdx >= oldLines.size())
            return Tr::tr("End line %1 out of range for delete (file has %2 lines).")
                .arg(endLine)
                .arg(oldLines.size());

        QString diff = header();
        QTextStream out(&diff);
        out << "@@ -" << line << "," << (endIdx - startIdx + 1) << " +0,0 @@\n";
        for (int i = startIdx; i <= endIdx; ++i)
            out << "-" << oldLines.at(i) << "\n";

        return diff;
    }

    return Tr::tr("Unsupported operation \"%1\" in diff generator.").arg(operation);
}

void EditFileTool::run(const QJsonObject &args,
                       std::function<void(const QString &, bool)> done) const
{
    const QString filePath = args.value("file_path").toString();
    const QString op = args.value("operation").toString();
    const int line = args.value("line").toInt(-1);
    const int endLine = args.value("end_line").toInt(-1);
    const QString text = args.value("text").toString();
    const QString newFile = args.value("new_file_content").toString();

    if (filePath.isEmpty())
        return done(Tr::tr(R"(Tool error: "file_path" must be a non‑empty string.)"), false);
    if (op.isEmpty())
        return done(Tr::tr(R"(Tool error: "operation" must be a non‑empty string.)"), false);

    static const QSet<QString> validOps{"replace", "insert", "delete", "create", "delete_file"};
    if (!validOps.contains(op))
        return done(Tr::tr(R"(Tool error: unknown operation "%1".)").arg(op), false);

    // Helper for missing required fields
    auto missing = [&](const QString &field) {
        return Tr::tr(R"(Tool error: "%1" operation requires the field "%2".)").arg(op, field);
    };

    if (op == "replace" || op == "insert") {
        if (line < 1)
            return done(missing("line"), false);
        if (text.isEmpty())
            return done(missing("text"), false);
        if (endLine != -1 && endLine < line)
            return done(Tr::tr(R"(Tool error: "end_line" must be >= "line".)"), false);
    } else if (op == "delete") {
        if (line < 1)
            return done(missing("line"), false);
        if (!text.isEmpty())
            return done(Tr::tr(R"(Tool error: "delete" operation must not contain "text".)"), false);
        if (endLine != -1 && endLine < line)
            return done(Tr::tr(R"(Tool error: "end_line" must be >= "line".)"), false);
    } else if (op == "create") {
        if (newFile.isEmpty())
            return done(missing("new_file_content"), false);
        if (line != -1 || !text.isEmpty() || endLine != -1)
            return done(Tr::tr(
                            R"(Tool error: "create" must not contain "line", "end_line" or "text".)"),
                        false);
    } else if (op == "delete_file") {
        if (line != -1 || !text.isEmpty() || !newFile.isEmpty() || endLine != -1)
            return done(Tr::tr(R"(Tool error: "delete_file" must not contain extra fields.)"),
                        false);
    }

    // Dispatch to the core implementation (now with endLine)
    const QString result = editFile(filePath, op, line, endLine, text, newFile);
    done(result, true);
}

QString EditFileTool::detailsMarkdown(const QJsonObject &args, const QString &result) const
{
    const QString filePath = args.value("file_path").toString();
    const QString op = args.value("operation").toString();
    const int line = args.value("line").toInt(-1);
    const int endLine = args.value("end_line").toInt(-1);
    const QString text = args.value("text").toString();
    const QString newFile = args.value("new_file_content").toString();

    const QString diff = diffForEditFile(filePath, op, line, endLine, text, newFile);

    return QString("**Operation:** %1\n\n"
                   "**Diff:**\n"
                   "```diff\n%2\n```\n"
                   "**Result:**\n```\n%3\n```")
        .arg(op, diff, result);
}
} // namespace LlamaCpp
