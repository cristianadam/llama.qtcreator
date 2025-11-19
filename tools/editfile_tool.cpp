#include "editfile_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

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
              "enum": ["replace", "insert_before", "insert_after", "delete", "create", "delete_file"],
              "description": "What should be done with the file.
                             - **replace** – replace a contiguous block of text.
                             - **insert_before** – insert new text before a line that matches a search pattern.
                             - **insert_after** – insert new text after a line that matches a search pattern.
                             - **delete** – delete a contiguous block of text.
                             - **create** – create a brand‑new file (the file must not exist).
                             - **delete_file** – delete the file entirely."
            },

            "search": {
              "type": "string",
              "description": "Exact text (including whitespace) that identifies the block to be replaced / deleted / the line before/after which to insert.
                              Required for *replace*, *insert_before*, *insert_after* and *delete*."
            },

            "replace": {
              "type": "string",
              "description": "The new text that will replace the *search* block (for **replace**) or the text that will be inserted (for **insert_before** / **insert_after**).
                              If the operation is **delete**, this field must be an empty string."
            },

            "new_file_content": {
              "type": "string",
              "description": "Full content of the file when **operation** is **create**. Ignored for all other operations."
            }
          },
          "required": ["file_path", "operation"],
          "allOf": [
            {
              "if": { "properties": { "operation": { "const": "replace" } } },
              "then": { "required": ["search", "replace"] }
            },
            {
              "if": { "properties": { "operation": { "enum": ["insert_before", "insert_after"] } } },
              "then": { "required": ["search", "replace"] }
            },
            {
              "if": { "properties": { "operation": { "const": "delete" } } },
              "then": { "required": ["search"], "properties": { "replace": { "const": "" } } }
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

    static const QMap<QString, QString> opMap{
        {"create", "create file"},
        {"delete_file", "delete file"},
        {"replace", "replace in"},
        {"delete", "delete from"},
        {"insert_before", "insert before in"},
        {"insert_after", "insert after in"},
    };
    const QString opDesc = opMap.value(op, op);
    return QStringLiteral("%1 %2").arg(opDesc, path);
}

QString createFile(const QString &relPath, const QString &content)
{
    // Resolve the working directory (project directory if any, otherwise generic workspace)
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    const FilePath targetFile = cwd.pathAppended(relPath);

    // Do not overwrite an existing file
    if (targetFile.exists())
        return Tr::tr("File \"%1\" already exists – cannot create a new file at this location.")
            .arg(relPath);

    // Ensure the parent directory exists; create it if necessary
    const FilePath parentDir = targetFile.parentDir();
    if (!parentDir.exists()) {
        const Result<> mkRes = parentDir.ensureWritableDir();
        if (!mkRes) {
            return Tr::tr("Failed to create parent directory \"%1\": %2")
                .arg(parentDir.toUserOutput(), mkRes.error());
        }
    }

    // Write the supplied content
    const Result<qint64> writeRes = targetFile.writeFileContents(content.toUtf8());
    if (!writeRes)
        return Tr::tr("Cannot create \"%1\": %2").arg(relPath, writeRes.error());

    const QString fileUrl
        = QString("<a href=\"file://%1\">%2</a>").arg(targetFile.toFSPathString()).arg(relPath);
    return Tr::tr("Created %1").arg(fileUrl);
}

QString deleteFile(const QString &relPath)
{
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    const FilePath targetFile = cwd.pathAppended(relPath);

    if (!targetFile.exists()) {
        return Tr::tr("File \"%1\" does not exist.").arg(relPath);
    }

    Result<> removeRes = targetFile.removeFile();
    if (!removeRes) {
        return Tr::tr("Failed to delete \"%1\": %2").arg(relPath, removeRes.error());
    }

    return Tr::tr("Deleted file \"%1\"").arg(relPath);
}

QString editFile(const QString &path,
                 const QString &operation,
                 const QString &search,
                 const QString &replace,
                 const QString &newContent)
{
    // Resolve the target file (same logic you already have)
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();
    const FilePath target = cwd.pathAppended(path);

    if (operation == "create") {
        return createFile(path, newContent);
    } else if (operation == "delete_file") {
        return deleteFile(path);
    } else {
        // All other ops need the file to exist
        if (!target.exists())
            return Tr::tr("File \"%1\" does not exist.").arg(path);

        // Load current content
        const Result<QByteArray> readRes = target.fileContents();
        if (!readRes)
            return Tr::tr("Failed to read \"%1\": %2").arg(path, readRes.error());

        QStringList fileLines = QString::fromUtf8(readRes.value()).split('\n', Qt::KeepEmptyParts);

        QStringList searchLines = search.split('\n', Qt::KeepEmptyParts);
        QStringList replaceLines = replace.split('\n', Qt::KeepEmptyParts);

        int matchStart = -1;
        for (int i = 0; i <= fileLines.size() - searchLines.size(); ++i) {
            bool ok = true;
            for (int j = 0; j < searchLines.size(); ++j) {
                if (fileLines[i + j] != searchLines[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                matchStart = i;
                break;
            }
        }
        if (matchStart == -1)
            return Tr::tr("Search text not found in \"%1\".").arg(path);

        QStringList newContentLines;

        if (operation == "replace") {
            newContentLines = fileLines.mid(0, matchStart)                      // before SEARCH
                              + replaceLines                                    // new block
                              + fileLines.mid(matchStart + searchLines.size()); // after SEARCH
        } else if (operation == "delete") {
            newContentLines = fileLines.mid(0, matchStart)
                              + fileLines.mid(matchStart + searchLines.size());
        } else if (operation == "insert_before") {
            newContentLines = fileLines.mid(0, matchStart) + replaceLines
                              + fileLines.mid(matchStart);
        } else if (operation == "insert_after") {
            newContentLines = fileLines.mid(0, matchStart + searchLines.size()) + replaceLines
                              + fileLines.mid(matchStart + searchLines.size());
        } else {
            // should never happen – the JSON schema guarantees the enum values
            return Tr::tr("Unsupported operation \"%1\".").arg(operation);
        }

        const Result<qint64> writeRes = target.writeFileContents(
            newContentLines.join('\n').toUtf8());

        if (!writeRes)
            return Tr::tr("Cannot write \"%1\": %2").arg(path, writeRes.error());

        return Tr::tr("Edited %1").arg(path);
    }
    return {};
}

QString diffForEditFile(const QString &filePath,
                        const QString &operation,
                        const QString &search,
                        const QString &replace,
                        const QString &newFileContent)
{
    QStringList oldLines;
    if (operation != "create") {
        FilePath cwd = Core::DocumentManager::projectsDirectory();
        if (const Project *p = ProjectManager::startupProject())
            cwd = p->projectDirectory();

        const FilePath target = cwd.pathAppended(filePath);
        const Result<QByteArray> readRes = target.fileContents();
        if (!readRes)
            return Tr::tr("Failed to read \"%1\": %2").arg(filePath, readRes.error());

        oldLines = QString::fromUtf8(readRes.value()).split('\n', Qt::KeepEmptyParts);
    }

    const QStringList searchLines = search.split('\n', Qt::KeepEmptyParts);
    const QStringList replaceLines = replace.split('\n', Qt::KeepEmptyParts);
    const QStringList newLines = newFileContent.split('\n', Qt::KeepEmptyParts);

    int matchStart = -1;
    if (!searchLines.isEmpty()) {
        for (int i = 0; i <= oldLines.size() - searchLines.size(); ++i) {
            bool ok = true;
            for (int j = 0; j < searchLines.size(); ++j) {
                if (oldLines[i + j] != searchLines[j]) {
                    ok = false;
                    break;
                }
            }
            if (ok) {
                matchStart = i;
                break;
            }
        }
        if (matchStart == -1 && operation != "create" && operation != "delete_file")
            return Tr::tr("Search block not found in \"%1\".").arg(filePath);
    }

    QString diff;
    QTextStream out(&diff);
    out << "--- a/" << filePath << "\n";
    out << "+++ b/" << filePath << "\n";

    if (operation == "create") {
        out << "@@ -0,0 +" << 1 << "," << newLines.size() << " @@\n";
        for (const QString &ln : newLines)
            out << "+" << ln << "\n";
        return diff;
    }

    if (operation == "delete_file") {
        out << "@@ -1," << oldLines.size() << " +0,0 @@\n";
        for (const QString &ln : oldLines)
            out << "-" << ln << "\n";
        return diff;
    }

    if (operation == "replace") {
        out << "@@ -" << matchStart + 1 << "," << searchLines.size() << " +" << matchStart + 1
            << "," << replaceLines.size() << " @@\n";
        for (const QString &ln : searchLines)
            out << "-" << ln << "\n";
        for (const QString &ln : replaceLines)
            out << "+" << ln << "\n";
        return diff;
    }

    if (operation == "delete") {
        out << "@@ -" << matchStart + 1 << "," << searchLines.size() << " +0,0 @@\n";
        for (const QString &ln : searchLines)
            out << "-" << ln << "\n";
        return diff;
    }

    if (operation == "insert_before") {
        // added lines appear **before** the search block
        out << "@@ -" << matchStart + 1 << ",0 +" << matchStart + 1 << "," << replaceLines.size()
            << " @@\n";
        for (const QString &ln : replaceLines)
            out << "+" << ln << "\n";
        // the search block itself becomes context
        for (const QString &ln : searchLines)
            out << " " << ln << "\n";
        return diff;
    }

    if (operation == "insert_after") {
        // insertion point is just after the search block
        const int afterLine = matchStart + searchLines.size(); // 0‑based
        out << "@@ -" << afterLine + 1 << ",0 +" << afterLine + 1 << "," << replaceLines.size()
            << " @@\n";
        for (const QString &ln : replaceLines)
            out << "+" << ln << "\n";
        // keep the search block as context (it appears before the hunk)
        for (const QString &ln : searchLines)
            out << " " << ln << "\n";
        return diff;
    }

    return Tr::tr("Unsupported operation \"%1\" in diff generator.").arg(operation);
}

QString EditFileTool::run(const QJsonObject &args) const
{
    // The actual work is already done by the old `editFile` free function.
    // We just forward the parameters and return the *result* that the
    // assistant already sent back (the JSON response contains the result,
    // not the diff).  Therefore the implementation simply returns an empty
    // string – the UI will later fill the diff using `diffForEditFile`.
    Q_UNUSED(args);
    return {}; // result will be filled later from the JSON payload
}

QString EditFileTool::detailsMarkdown(const QJsonObject &args, const QString &) const
{
    const QString filePath = args.value("file_path").toString();
    const QString op = args.value("operation").toString();
    const QString search = args.value("search").toString();
    const QString replace = args.value("replace").toString();
    const QString newFile = args.value("new_file_content").toString();

    const QString diff = diffForEditFile(filePath, op, search, replace, newFile);

    return QString("**Operation:** %1\n\n"
                   "**Diff:**\n"
                   "```diff\n%2\n```")
        .arg(op, diff);
}
} // namespace LlamaCpp
