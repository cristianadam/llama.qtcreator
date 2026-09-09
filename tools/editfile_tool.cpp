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
        "description": "Edit a file by finding and replacing exact text. The search text must match the file content exactly (including whitespace). Use multiple lines of context to ensure uniqueness. The tool will fail if the search text is not found or appears multiple times.",
        "parameters": {
          "type": "object",
          "properties": {
            "file_path": {
              "type": "string",
              "description": "Path of the target file, relative to the workspace root (or absolute). The file must already exist."
            },
            "old_string": {
              "type": "string",
              "description": "The exact text to find. Must match the file content exactly, including whitespace and indentation. Include 3-5 lines of surrounding context for uniqueness."
            },
            "new_string": {
              "type": "string",
              "description": "The replacement text. Can be empty to delete the old_string."
            }
          },
          "required": ["file_path", "old_string", "new_string"],
          "additionalProperties": false
        }
      }
    })raw";
}

QString EditFileTool::oneLineSummary(const QJsonObject &args) const
{
    const QString path = args.value("file_path").toString();
    return Tr::tr("edit file %1").arg(path);
}

// Normalize whitespace for fuzzy matching: collapse multiple spaces/tabs to single space
static QString normalizeWhitespace(const QString &s)
{
    QString result;
    bool lastWasSpace = false;
    for (const QChar &c : s) {
        if (c.isSpace()) {
            if (!lastWasSpace && !result.isEmpty()) {
                result += ' ';
                lastWasSpace = true;
            }
        } else {
            result += c;
            lastWasSpace = false;
        }
    }
    return result;
}

// Find the position and length of search text in content using exact match
// Returns {-1, 0} if not found
static std::pair<int, int> findExact(const QString &content, const QString &search)
{
    int idx = content.indexOf(search);
    if (idx >= 0)
        return {idx, search.size()};
    return {-1, 0};
}

// Find using whitespace-normalized matching (less strict)
static std::pair<int, int> findNormalized(const QString &content, const QString &search)
{
    QString normalizedContent = normalizeWhitespace(content);
    QString normalizedSearch = normalizeWhitespace(search);
    
    int idx = normalizedContent.indexOf(normalizedSearch);
    if (idx < 0)
        return {-1, 0};
    
    // Map back to original content position by scanning
    int contentIdx = 0;
    int normIdx = 0;
    while (contentIdx < content.size() && normIdx < idx) {
        if (content[contentIdx].isSpace()) {
            // Skip whitespace in content until we find non-whitespace
            while (contentIdx < content.size() && content[contentIdx].isSpace())
                ++contentIdx;
            if (normIdx < idx)
                ++normIdx;
        } else {
            ++contentIdx;
            ++normIdx;
        }
    }
    
    // Now find the end position similarly
    int endNormIdx = idx + normalizedSearch.size();
    int startContentIdx = contentIdx;
    while (contentIdx < content.size() && normIdx < endNormIdx) {
        if (content[contentIdx].isSpace()) {
            while (contentIdx < content.size() && content[contentIdx].isSpace())
                ++contentIdx;
            if (normIdx < endNormIdx)
                ++normIdx;
        } else {
            ++contentIdx;
            ++normIdx;
        }
    }
    
    return {startContentIdx, contentIdx - startContentIdx};
}

// Apply a replacement at the given position
static QString applyReplacement(const QString &content, int pos, int len, const QString &replacement)
{
    return content.left(pos) + replacement + content.mid(pos + len);
}

// Generate a unified diff for the edit
static QString generateDiff(const QString &filePath, const QString &oldContent, const QString &newContent)
{
    QString diff;
    QTextStream out(&diff);
    out << "--- a/" << filePath << "\n";
    out << "+++ b/" << filePath << "\n";
    
    // Simple line-based diff: show removed and added lines
    QStringList oldLines = oldContent.split('\n', Qt::KeepEmptyParts);
    QStringList newLines = newContent.split('\n', Qt::KeepEmptyParts);
    
    // Find common prefix/suffix for context
    int prefixLines = 0;
    while (prefixLines < oldLines.size() && prefixLines < newLines.size() 
           && oldLines[prefixLines] == newLines[prefixLines]) {
        ++prefixLines;
    }
    
    int suffixLines = 0;
    int oldSuffix = oldLines.size() - 1;
    int newSuffix = newLines.size() - 1;
    while (suffixLines < qMin(prefixLines, qMin(oldLines.size() - prefixLines, newLines.size() - prefixLines))
           && oldSuffix >= prefixLines && newSuffix >= prefixLines
           && oldLines[oldSuffix] == newLines[newSuffix]) {
        --oldSuffix;
        --newSuffix;
        ++suffixLines;
    }
    
    // Hunk header
    int oldStart = qMax(0, prefixLines > 0 ? prefixLines - 1 : 0);
    int newStart = qMax(0, prefixLines > 0 ? prefixLines - 1 : 0);
    int oldCount = oldLines.size() - prefixLines - suffixLines;
    int newCount = newLines.size() - prefixLines - suffixLines;
    if (oldCount < 0) oldCount = 0;
    if (newCount < 0) newCount = 0;
    
    out << "@@" << " -" << (prefixLines + 1) << "," << oldCount << " +" 
        << (prefixLines + 1) << "," << newCount << " @@\n";
    
    // Context lines (prefix)
    for (int i = 0; i < prefixLines; ++i) {
        out << " " << oldLines[i] << "\n";
    }
    
    // Removed lines
    for (int i = prefixLines; i <= oldSuffix; ++i) {
        if (i >= 0 && i < oldLines.size())
            out << "-" << oldLines[i] << "\n";
    }
    
    // Added lines
    for (int i = prefixLines; i <= newSuffix; ++i) {
        if (i >= 0 && i < newLines.size())
            out << "+" << newLines[i] << "\n";
    }
    
    // Context lines (suffix)
    for (int i = oldSuffix + 1; i < oldLines.size() - suffixLines + (oldLines.size() - newLines.size()); ++i) {
        if (i >= 0 && i < oldLines.size())
            out << " " << oldLines[i] << "\n";
    }
    
    return diff;
}

QString EditFileTool::detailsMarkdown(const QJsonObject &args, const QString &result) const
{
    const QString filePath = args.value("file_path").toString();
    const QString oldString = args.value("old_string").toString();
    const QString newString = args.value("new_string").toString();
    
    if (result.contains("failed") || result.contains("Error") || result.contains("error"))
        return result;
    
    QString diff;
    QTextStream out(&diff);
    out << "--- a/" << filePath << "\n";
    out << "+++ b/" << filePath << "\n";
    
    QStringList oldLines = oldString.split('\n');
    QStringList newLines = newString.split('\n');
    
    out << "@@ -1," << oldLines.size() << " +1," << newLines.size() << " @@\n";
    for (const QString &line : oldLines)
        out << "-" << line << "\n";
    for (const QString &line : newLines)
        out << "+" << line << "\n";
    
    return QString("**%1** `%2`\n\n```diff\n%3\n```")
        .arg(Tr::tr("edited"), filePath, diff);
}

void EditFileTool::run(const QJsonObject &args,
                       std::function<void(const QString &, bool)> done) const
{
    const QString filePathStr = args.value("file_path").toString();
    const QString oldString = args.value("old_string").toString();
    const QString newString = args.value("new_string").toString();
    
    if (filePathStr.isEmpty()) {
        return done(Tr::tr("Tool error: \"file_path\" must be a non-empty string."), false);
    }
    if (oldString.isEmpty()) {
        return done(Tr::tr("Tool error: \"old_string\" must be a non-empty string."), false);
    }
    
    const FilePath filePath = FilePath::fromUserInput(filePathStr);
    const FilePath targetFile = absoluteProjectPath(filePath);
    
    if (!targetFile.exists()) {
        return done(Tr::tr("File \"%1\" does not exist.").arg(targetFile.toUserOutput()), false);
    }
    
    // Read the file
    const Result<QByteArray> readRes = targetFile.fileContents();
    if (!readRes) {
        return done(Tr::tr("Failed to read \"%1\": %2")
                        .arg(targetFile.toUserOutput(), readRes.error()),
                    false);
    }
    
    QString content = QString::fromUtf8(readRes.value());
    QString oldContentForDiff = content; // Save for diff generation
    
    // Try exact match first
    auto [pos, len] = findExact(content, oldString);
    
    if (pos < 0) {
        // Try whitespace-normalized match as fallback
        auto [normPos, normLen] = findNormalized(content, oldString);
        if (normPos >= 0) {
            pos = normPos;
            len = normLen;
        }
    }
    
    if (pos < 0) {
        return done(Tr::tr("Edit failed: search text not found in \"%1\".\n"
                           "Make sure the text matches exactly, including whitespace and indentation.")
                        .arg(targetFile.toUserOutput()),
                    false);
    }
    
    // Check for multiple occurrences (exact match only)
    QString contentAfterReplace = applyReplacement(content, pos, len, newString);
    int occurrences = 0;
    int searchStart = 0;
    while ((pos = content.indexOf(oldString, searchStart)) >= 0) {
        ++occurrences;
        searchStart = pos + 1;
    }
    
    if (occurrences > 1) {
        return done(Tr::tr("Edit failed: search text appears %1 times in \"%2\".\n"
                           "Include more surrounding context to make it unique.")
                        .arg(occurrences)
                        .arg(targetFile.toUserOutput()),
                    false);
    }
    
    // Apply the replacement
    content = applyReplacement(content, pos, len, newString);
    
    // Write back
    const Result<qint64> writeRes = targetFile.writeFileContents(content.toUtf8());
    if (!writeRes) {
        return done(Tr::tr("Cannot write \"%1\": %2").arg(targetFile.toUserOutput(), writeRes.error()),
                    false);
    }
    
    // Generate diff for display
    QString diff = generateDiff(filePathStr, oldContentForDiff, content);
    
    return done(Tr::tr("Edited %1").arg(targetFile.toUserOutput()), true);
}

} // namespace LlamaCpp
