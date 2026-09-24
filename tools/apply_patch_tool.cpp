#include "apply_patch_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "patch.h"
#include "tool_utils.h"

#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QPair>
#include <QJsonDocument>
#include <QJsonObject>

#include <utils/filepath.h>

using namespace Utils;

namespace LlamaCpp {

namespace {

const bool registered = [] {
    ToolFactory::instance().registerCreator(ApplyPatchTool{}.name(),
                                            []() { return std::make_unique<ApplyPatchTool>(); });
    return true;
}();

QString readPatchedLines(const QString &path)
{
    const FilePath target = absoluteProjectPath(FilePath::fromUserInput(path));
    if (!target.isFile())
        return {};
    const Result<QByteArray> readRes = target.fileContents();
    if (!readRes)
        return {};
    return QString::fromUtf8(readRes.value());
}

/**
 * Renders the update hunks as a unified diff. The line numbers in the
 * @@ headers are recovered by locating each hunk's new lines in the
 * (already patched) file; if that fails the header has no numbers.
 */
QString unifiedDiffFor(const Patch::Hunk &hunk)
{
    QStringList newLines;
    const QString patched = readPatchedLines(hunk.path);
    if (!patched.isEmpty()) {
        newLines = patched.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        if (!newLines.isEmpty() && newLines.last().isEmpty())
            newLines.removeLast();
    }

    QString diff;
    int cursor = 0;     // 0-based search position in newLines
    int prevOldStart = -1; // 1-based
    int prevOldCount = 0;
    int prevFound = -1; // 0-based
    int prevNewCount = 0;

    for (const Patch::UpdateChunk &chunk : std::as_const(hunk.chunks)) {
        const int oldCount = chunk.oldLines.size();
        const int newCount = chunk.newLines.size();

        int found = -1;
        if (!newLines.isEmpty()) {
            if (newCount == 0)
                found = cursor;
            else
                found = Patch::locateLines(newLines, chunk.newLines, cursor, false);
        }

        QString header = QStringLiteral("@@");
        if (found != -1) {
            const int newStart = found + 1;
            const int gap = prevFound == -1 ? 0 : qMax(0, found - (prevFound + prevNewCount));
            const int oldStart = prevOldStart == -1 ? newStart : prevOldStart + prevOldCount + gap;
            header += QStringLiteral(" -%1,%2 +%3,%4 @@").arg(oldStart)
                                .arg(oldCount)
                                .arg(newStart)
                                .arg(newCount);
            if (!chunk.changeContext.isEmpty())
                header += QLatin1Char(' ') + chunk.changeContext;

            cursor = qMax(cursor, found + newCount);
            prevOldStart = oldStart;
            prevOldCount = oldCount;
            prevFound = found;
            prevNewCount = newCount;
        } else if (!chunk.changeContext.isEmpty()) {
            header += QLatin1Char(' ') + chunk.changeContext;
        }

        diff += header + QLatin1Char('\n');
        for (const QString &line : std::as_const(chunk.rawLines))
            diff += line + QLatin1Char('\n');
    }

    return diff.trimmed();
}

} // namespace

QString ApplyPatchTool::name() const
{
    return QStringLiteral("apply_patch");
}

QString ApplyPatchTool::toolDefinition() const
{
    const QString description = R"desc(
Apply a patch that creates, updates and/or deletes files.
The patch is wrapped in *** Begin Patch / *** End Patch markers and contains one section per file.

*** Add File: <path>
Create a new file, or fully replace an existing file.
Every following line is the file content, prefixed with +.

*** Update File: <path>
Modify an existing file with one or more hunks.
Optionally followed by *** Move to: <new path> to rename the file.
Each hunk starts with @@, optionally followed by a context line used to disambiguate the location (e.g. @@ def main():).
Inside a hunk: a line starting with a space is context (unchanged), - removes a line, + adds a line.

*** Delete File: <path>
Delete an existing file. Nothing follows this line.

Example:

*** Begin Patch
*** Add File: hello.txt
+Hello world
*** Update File: src/app.py
@@ def greet():
 def greet():
-    print("Hi")
+    print("Hello, world!")
 def farewell():
     pass
*** Delete File: obsolete.txt
*** End Patch

Guidelines for reliable patches:
- Always include the *** Begin Patch and *** End Patch markers.
- Always include a section header (Add File / Update File / Delete File) for every file.
- Prefix new lines with +, even when creating a new file. A missing + prefix in an Add File section is tolerated (the line is kept as content), but always prefix the lines anyway.
- Include at least 3 context lines above and below each change so the location is unambiguous.
- Keep hunks structurally coherent: when changing a function or a block, rewrite the whole unit instead of scattered line edits.
- If the change affects most of a file, or the file is small, prefer *** Add File: with the complete new content over many hunks.
- Hunks are located top to bottom: each hunk is searched for starting right after where the previous hunk matched. To change a line that occurs several times, emit one hunk per occurrence in file order; to change one specific occurrence, add context lines or an @@ context line.
- An Update section that leaves the file unchanged (removed and added lines identical) is rejected.
- A *** End of File line anchors a hunk to the end of the file; a hunk without removed lines is inserted at the end of the file.
- Multiple sections that target the same file (for example two Update sections) are applied in order, each on top of the result of the previous one.
- Inside an Update File hunk, a line that does not start with a space, - or + is rejected; the patch is never applied with silently dropped lines.
- The whole patch is validated before any file is modified. When a hunk cannot be located, the error tells where the closest match is or whether the hunks are out of file order - adjust the patch accordingly.
)desc";

    QJsonObject patchTextProperty;
    patchTextProperty[QStringLiteral("type")] = QStringLiteral("string");
    patchTextProperty[QStringLiteral("description")] = QStringLiteral(
        "The full patch text that describes all changes to be made. "
        "File paths are relative to the workspace root (or absolute).");

    QJsonObject properties;
    properties[QStringLiteral("patchText")] = patchTextProperty;

    QJsonObject parameters;
    parameters[QStringLiteral("type")] = QStringLiteral("object");
    parameters[QStringLiteral("properties")] = properties;
    parameters[QStringLiteral("required")] = QJsonArray{QStringLiteral("patchText")};
    parameters[QStringLiteral("additionalProperties")] = false;

    QJsonObject function;
    function[QStringLiteral("name")] = QStringLiteral("apply_patch");
    function[QStringLiteral("description")] = description.trimmed();
    function[QStringLiteral("parameters")] = parameters;

    QJsonObject root;
    root[QStringLiteral("type")] = QStringLiteral("function");
    root[QStringLiteral("function")] = function;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString hunkSummary(Patch::HunkType type, const QString &path, const QString &movePath)
{
    switch (type) {
    case Patch::HunkType::Add:
        return Tr::tr("Add %1").arg(path);
    case Patch::HunkType::Delete:
        return Tr::tr("Delete %1").arg(path);
    case Patch::HunkType::Update:
        if (movePath.isEmpty())
            return Tr::tr("Edit %1").arg(path);
        return Tr::tr("Move %1 to %2").arg(path, movePath);
    }
    return Tr::tr("Apply patch");
}

QString ApplyPatchTool::oneLineSummary(const QJsonObject &args) const
{
    QVector<Patch::Hunk> hunks;
    if (!Patch::parse(args.value("patchText").toString(), hunks).isEmpty() || hunks.isEmpty())
        return Tr::tr("Apply patch");

    if (hunks.size() == 1)
        return hunkSummary(hunks.first().type, hunks.first().path, hunks.first().movePath);

    return Tr::tr("Apply patch to %1 files").arg(hunks.size());
}

QString ApplyPatchTool::streamingSummary(const QString &partialArgs) const
{
    // Extracts the first file section from the raw (JSON‑escaped) argument
    // text as it is being streamed, so the UI can show "Add src/main.cpp"
    // long before the patch is complete.
    auto findSection = [&partialArgs](const QString &marker) -> QPair<int, QString> {
        const int idx = partialArgs.indexOf(marker);
        if (idx == -1)
            return { -1, QString() };
        int start = idx + marker.size();
        int end = start;
        while (end < partialArgs.size()) {
            const QChar c = partialArgs.at(end);
            if (c == QLatin1Char('\\') || c == QLatin1Char('"'))
                break;
            ++end;
        }
        return { idx, partialArgs.mid(start, end - start).trimmed() };
    };

    const auto add = findSection(QStringLiteral("*** Add File:"));
    const auto update = findSection(QStringLiteral("*** Update File:"));
    const auto del = findSection(QStringLiteral("*** Delete File:"));

    int pos = -1;
    Patch::HunkType type = Patch::HunkType::Add;
    QString path;
    QString movePath;

    if (add.first != -1 && !add.second.isEmpty()) {
        pos = add.first;
        type = Patch::HunkType::Add;
        path = add.second;
    }
    if (update.first != -1 && !update.second.isEmpty()
        && (pos == -1 || update.first < pos)) {
        pos = update.first;
        type = Patch::HunkType::Update;
        path = update.second;
        // A rename is only in effect when it appears before the first hunk
        const int moveIdx = partialArgs.indexOf(QStringLiteral("*** Move to:"), update.first);
        const int hunkIdx = partialArgs.indexOf(QLatin1Char('@'), update.first);
        if (moveIdx != -1 && (hunkIdx == -1 || moveIdx < hunkIdx)) {
            const auto move = findSection(QStringLiteral("*** Move to:"));
            if (!move.second.isEmpty())
                movePath = move.second;
        }
    }
    if (del.first != -1 && !del.second.isEmpty() && (pos == -1 || del.first < pos)) {
        pos = del.first;
        type = Patch::HunkType::Delete;
        path = del.second;
    }

    if (pos == -1)
        return {};
    return hunkSummary(type, path, movePath);
}

QString ApplyPatchTool::detailsMarkdown(const QJsonObject &args, const QString &result, bool ok) const
{
    if (!ok)
        return result;

    QVector<Patch::Hunk> hunks;
    if (!Patch::parse(args.value("patchText").toString(), hunks).isEmpty())
        return result;

    QString md;
    // The summary already names the file for single-file patches, so headers
    // are only needed to tell apart the sections of a multi-file patch.
    const bool showHeaders = hunks.size() > 1;
    for (const Patch::Hunk &hunk : std::as_const(hunks)) {
        switch (hunk.type) {
        case Patch::HunkType::Add:
            if (showHeaders)
                md += QString("**%1** `%2`\n\n").arg(Tr::tr("created"), hunk.path);
            md += codeFence(hunk.contents, codeLanguageFor(hunk.path)) + QStringLiteral("\n\n");
            break;
        case Patch::HunkType::Update: {
            if (showHeaders) {
                const QString title = hunk.movePath.isEmpty()
                        ? QString("**%1** `%2`").arg(Tr::tr("edited"), hunk.path)
                        : QString("**%1** `%2` \u2192 `%3`")
                              .arg(Tr::tr("moved"), hunk.path, hunk.movePath);
                md += title + QLatin1String("\n\n");
            }
            md += codeFence(unifiedDiffFor(hunk), QStringLiteral("diff")) + QLatin1String("\n\n");
            break;
        }
        case Patch::HunkType::Delete:
            // A deletion has no content to show – the line is the only
            // trace of it, so it always carries the file name.
            md += QString("**%1** `%2`\n\n").arg(Tr::tr("deleted"), hunk.path);
            break;
        }
    }

    return md.trimmed();
}

QString ApplyPatchTool::summaryPreview(const QJsonObject &args, const QString &result, bool ok) const
{
    // The per‑file diff is the interesting part; give it a couple of extra
    // lines.  On failure the ✗ icon suffices in the collapsed view; the
    // error text is only shown in the expanded details.
    return ok ? truncatedPreview(detailsMarkdown(args, result, ok), 8) : QString();
}

void ApplyPatchTool::run(const QJsonObject &args,
                         std::function<void(const QString &, bool)> done) const
{
    const QString patchText = args.value("patchText").toString().trimmed();
    if (patchText.isEmpty())
        return done(QStringLiteral("Tool error: \"patchText\" must be a non-empty string."), false);

    QVector<Patch::Hunk> hunks;
    const QString parseError = Patch::parse(patchText, hunks);
    if (!parseError.isEmpty())
        return done(QStringLiteral("apply_patch verification failed: %1").arg(parseError), false);

    struct Op
    {
        Patch::HunkType type;
        FilePath source; // file affected (update: modified, delete: removed)
        FilePath target; // where content is written (add, update, move)
        QString contents; // add and update only
        QString display;  // path shown in the output
    };
    QVector<Op> ops;
    // Content computed so far for paths that occur in several sections of the
    // same patch (e.g. two "*** Update File:" sections for one file): each
    // section is applied on top of the previous one instead of both being
    // computed from the original file (the second write would then silently
    // discard the first section's changes).
    QHash<QString, QString> pendingContents;

    // Verification phase: resolve everything and compute all new contents
    // before touching the file system, so a failed patch leaves no side effects.
    for (const Patch::Hunk &hunk : std::as_const(hunks)) {
        Op op;
        op.type = hunk.type;
        op.source = absoluteProjectPath(FilePath::fromUserInput(hunk.path));
        op.display = hunk.path;

        switch (hunk.type) {
        case Patch::HunkType::Add: {
            // The file does not exist yet; resolve without an existence probe
            // so it lands in the project directory, not the general one.
            op.target = absoluteProjectPath(FilePath::fromUserInput(hunk.path), /*mustExist=*/false);
            op.source = op.target;
            op.contents = hunk.contents;
            if (!op.contents.isEmpty() && !op.contents.endsWith(QLatin1Char('\n')))
                op.contents += QLatin1Char('\n');
            pendingContents.insert(hunk.path, op.contents);
            break;
        }
        case Patch::HunkType::Delete: {
            if (!pendingContents.contains(hunk.path) && !op.source.isFile())
                return done(QStringLiteral("apply_patch verification failed: "
                                           "file to delete does not exist: %1")
                                .arg(op.display),
                            false);
            pendingContents.remove(hunk.path);
            break;
        }
        case Patch::HunkType::Update: {
            QString baseContents;
            if (pendingContents.contains(hunk.path)) {
                baseContents = pendingContents.value(hunk.path);
            } else {
                if (!op.source.isFile())
                    return done(QStringLiteral("apply_patch verification failed: "
                                               "Failed to read file to update: %1")
                                    .arg(op.display),
                                false);

                const Result<QByteArray> readRes = op.source.fileContents();
                if (!readRes)
                    return done(QStringLiteral("apply_patch verification failed: "
                                               "Failed to read file to update: %1 (%2)")
                                    .arg(op.display, readRes.error()),
                                false);

                baseContents = QString::fromUtf8(readRes.value());
            }

            const QString error = Patch::applyUpdateChunks(baseContents, hunk.chunks, op.display, op.contents);
            if (!error.isEmpty())
                return done(QStringLiteral("apply_patch verification failed: %1").arg(error),
                            false);

            if (op.contents == baseContents)
                return done(QStringLiteral("apply_patch verification failed: no changes made to %1: "
                                           "the update leaves the file unchanged (the removed and "
                                           "added lines are identical).")
                                .arg(op.display),
                            false);

            pendingContents.insert(hunk.path, op.contents);

            if (!hunk.movePath.isEmpty()) {
                op.target =
                        absoluteProjectPath(FilePath::fromUserInput(hunk.movePath), /*mustExist=*/false);
                op.display = hunk.movePath;
            } else {
                op.target = op.source;
            }
            break;
        }
        }

        ops.append(op);
    }

    // Write phase.  The contents are fully verified, but the writes are not
    // atomic across files – if one fails, report what was already applied.
    QStringList applied;
    auto fail = [&applied, done](const QString &message) {
        const QString prefix = applied.isEmpty()
                ? QString()
                : QStringLiteral("Patch partially applied before failing (applied: %1). ")
                      .arg(applied.join(", "));
        done(prefix + message, false);
    };

    for (const Op &op : std::as_const(ops)) {
        switch (op.type) {
        case Patch::HunkType::Add:
        case Patch::HunkType::Update: {
            const FilePath parentDir = op.target.parentDir();
            if (!parentDir.exists()) {
                const Result<> mkRes = parentDir.ensureWritableDir();
                if (!mkRes)
                    return fail(QStringLiteral("Failed to create parent directory \"%1\": %2")
                                    .arg(parentDir.toUserOutput(), mkRes.error()));
            }

            const Result<qint64> writeRes = op.target.writeFileContents(op.contents.toUtf8());
            if (!writeRes)
                return fail(QStringLiteral("Cannot write \"%1\": %2")
                                .arg(op.target.toUserOutput(), writeRes.error()));

            if (op.type == Patch::HunkType::Update && op.target != op.source) {
                const Result<> removeRes = op.source.removeFile();
                if (!removeRes)
                    return fail(QStringLiteral("Failed to delete \"%1\" after move: %2")
                                    .arg(op.source.toUserOutput(), removeRes.error()));
            }
            break;
        }
        case Patch::HunkType::Delete: {
            const Result<> removeRes = op.source.removeFile();
            if (!removeRes)
                return fail(QStringLiteral("Failed to delete \"%1\": %2")
                                .arg(op.source.toUserOutput(), removeRes.error()));
            break;
        }
        }

        const QChar letter = op.type == Patch::HunkType::Add ? QLatin1Char('A')
                          : op.type == Patch::HunkType::Delete ? QLatin1Char('D')
                                                               : QLatin1Char('M');
        applied << QStringLiteral("%1 %2").arg(letter, op.display);
    }

    QString output = Tr::tr("Success. Updated the following files:");
    for (const Op &op : std::as_const(ops)) {
        const QChar letter = op.type == Patch::HunkType::Add ? QLatin1Char('A')
                          : op.type == Patch::HunkType::Delete ? QLatin1Char('D')
                                                               : QLatin1Char('M');
        output += QStringLiteral("\n%1 %2").arg(letter, op.display);
    }
    return done(output, true);
}

} // namespace LlamaCpp
