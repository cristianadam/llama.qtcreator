#include "llamatools.h"
#include "llamatr.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <texteditor/basefilefind.h>
#include <utils/filepath.h>
#include <utils/searchresultitem.h>

using namespace ProjectExplorer;
using namespace Utils;

namespace LlamaCpp::Tools {

QStringList getTools()
{
    QStringList tools;
    tools << R"raw(
    {
        "type": "function",
        "function": {
            "name": "python",
            "description": "Runs code in an Python interpreter and returns the result of the execution.",
            "parameters": {
                "type": "object",
                "properties": {
                    "code": {
                        "type": "string",
                        "description": "The code to run in the Python interpreter."
                    }
                },
                "required": ["code"],
                "strict": true
            }
        }
    })raw";

    tools << R"raw(
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

    tools << R"raw(
    {
        "type": "function",
        "function": {
            "name": "list_directory",
            "description": "List the contents of a directory. The quick tool to understand the file structure and explore the codebase.",
            "parameters": {
                "type": "object",
                "properties": {
                    "directory_path": { "type": "string", "description": "Absolute or relative workspace path" }
                },
                "required": [ "directory_path" ],
                "strict": true
            }
        }
    })raw";

    tools << R"raw(
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
                    "file_path": { "type": "string", "description": "The path of the file to read" }
                },
                "required": [ "first_line", "last_line_inclusive", "file_path" ],
                "strict": true
            }
        }
    })raw";

    tools << R"raw(
    {
        "type": "function",
        "function": {
            "name": "regex_search",
            "description": "Fast text-based regex search in the code base (prefer it for finding exact function names or expressions) that finds exact pattern matches with file names and line numbers within files or directories. If there is no exclude_pattern - provide an empty string. Returns matches in format file_name:line_number: line_content",
            "parameters": {
                "type": "object",
                "properties": {
                    "include_pattern": { "type": "string", "description": "Comma separated glob patterns for files to include (specify file extensions only if you are absolutely sure)" },
                    "exclude_pattern": { "type": "string", "description": "Comma separated glob patterns for files to exclude" },
                    "regex": { "type": "string", "description": "A string for constructing a regular expression pattern to search for. Escape special regex characters when needed." }
                },
                "required": [ "include_pattern", "regex" ],
                "strict": true
            }
        }
    })raw";

    tools << R"raw(
    {
        "type": "function",
        "function": {
            "name": "ask_user",
            "description": "Use this tool to ask the user for clarifications if something is unclear.",
            "parameters": {
                "type": "object",
                "properties": {
                    "question": { "type": "string", "description": "The question to the user." }
                },
                "required": [ "question" ],
                "strict": true
            }
        }
    })raw";

    return tools;
}

QString runPython(const QString &code)
{
    QProcess proc;
    proc.setProgram("python3");
    proc.setArguments({"-u", "-c", code});
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();
    proc.setWorkingDirectory(cwd.toFSPathString());
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    proc.waitForFinished();
    return QString::fromUtf8(proc.readAllStandardOutput());
}

QString listDirectory(const QString &rawPath)
{
    // Resolve the directory path – same strategy as other tools.
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    FilePath dirPath = rawPath.isEmpty() ? cwd : cwd.pathAppended(rawPath);

    if (!dirPath.exists())
        return Tr::tr("Directory \"%1\" does not exist.").arg(dirPath.toUserOutput());

    if (!dirPath.isDir())
        return Tr::tr("\"%1\" is not a directory.").arg(dirPath.toUserOutput());

    const FileFilter filter(QStringList(),
                            QDir::AllEntries | QDir::NoDotAndDotDot,
                            QDirIterator::NoIteratorFlags);

    const FilePaths entries = dirPath.dirEntries(filter, QDir::Name | QDir::DirsFirst);
    QStringList lines;

    lines << Tr::tr("Directory listing for %1:").arg(dirPath.toUserOutput());
    lines << "";

    for (const FilePath &entry : entries) {
        if (entry.isDir()) {
            lines << QStringLiteral("[DIR] %1/").arg(entry.fileName());
        } else {
            // Human‑readable size
            qint64 size = entry.fileSize();
            QString sizeStr;

            if (size < 1024)
                sizeStr = QString::number(size) + " B";
            else if (size < 1024 * 1024)
                sizeStr = QString::number(size / 1024.0, 'f', 1) + " KiB";
            else if (size < 1024LL * 1024 * 1024)
                sizeStr = QString::number(size / (1024.0 * 1024), 'f', 1) + " MiB";
            else
                sizeStr = QString::number(size / (1024.0 * 1024 * 1024), 'f', 1) + " GiB";

            const QString mod = entry.lastModified().toString(Qt::ISODate);
            lines << QStringLiteral("%1 (%2, %3)").arg(entry.fileName(), sizeStr, mod);
        }
    }

    return lines.join('\n');
}

QString readFile(const QString &relPath, int firstLine, int lastLineIncl, bool readAll)
{
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    const FilePath targetFile = cwd.pathAppended(relPath);

    if (!targetFile.exists()) {
        return Tr::tr("File \"%1\" does not exist.").arg(relPath);
    }

    const Result<QByteArray> readRes = targetFile.fileContents();
    if (!readRes) {
        return Tr::tr("Failed to read \"%1\": %2").arg(relPath, readRes.error());
    }

    const QString fileText = QString::fromUtf8(readRes.value());
    const QStringList allLines = fileText.split('\n', Qt::KeepEmptyParts);

    if (readAll) {
        return fileText;
    }

    if (firstLine < 1) {
        return Tr::tr("first_line must be >= 1.");
    }
    if (lastLineIncl < firstLine) {
        return Tr::tr("last_line_inclusive must be >= first_line.");
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
        return Tr::tr("first_line (%1) exceeds the number of lines in \"%2\" (%3).")
            .arg(firstLine)
            .arg(relPath)
            .arg(allLines.size());
    }

    const QStringList slice = allLines.mid(startIdx, endIdx - startIdx);
    return slice.join('\n');
}

class RegexFileFind : public TextEditor::BaseFileFind
{
public:
    using TextEditor::BaseFileFind::currentSearchEngine;
    using TextEditor::BaseFileFind::searchDir;
    using TextEditor::BaseFileFind::setSearchDir;

    QString id() const { return {}; }
    QString displayName() const { return {}; }
    QString label() const { return {}; }
    QString toolTip() const { return {}; }

    TextEditor::FileContainerProvider fileContainerProvider() const
    {
        return [this] {
            return SubDirFileContainer(FilePaths{searchDir()},
                                       fileFindParameters.nameFilters,
                                       fileFindParameters.exclusionFilters);
        };
    }

    TextEditor::FileFindParameters fileFindParameters;
};

QString regexSearch(const QString &includePattern,
                    const QString &excludePattern,
                    const QString &regex)
{
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    RegexFileFind finder;
    finder.setSearchDir(cwd);

    TextEditor::FileFindParameters &params = finder.fileFindParameters;
    params.text = regex;
    params.flags = FindRegularExpression;
    params.nameFilters = includePattern.isEmpty() ? QStringList()
                                                  : splitFilterUiText(includePattern);
    params.exclusionFilters = excludePattern.isEmpty() ? QStringList()
                                                       : splitFilterUiText(excludePattern);
    params.searchDir = cwd;
    params.fileContainerProvider = finder.fileContainerProvider();

    TextEditor::SearchEngine *engine = finder.currentSearchEngine();
    QTC_ASSERT(engine, return Tr::tr("No search engine available."));

    params.searchExecutor = engine->searchExecutor();
    QFuture<SearchResultItems> future = params.searchExecutor(params);
    future.waitForFinished();
    const SearchResultItems items = future.result();

    if (items.isEmpty())
        return Tr::tr("No matches found for pattern \"%1\".").arg(regex);

    QStringList lines;
    for (const SearchResultItem &it : items) {
        // `it.path()` returns a `FilePath` – make it relative to the cwd
        const QString relPath
            = FilePath::fromString(it.path().first()).relativeChildPath(cwd).toUserOutput();
        const int lineNumber = it.mainRange().begin.line;
        const QString lineText = it.lineText();
        lines << QString("%1:%2: %3").arg(relPath, QString::number(lineNumber), lineText);
    }

    return lines.join('\n');
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

} // namespace LlamaCpp::Tools
