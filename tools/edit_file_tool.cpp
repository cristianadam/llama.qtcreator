#include "edit_file_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "patch.h"
#include "tool_utils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utils/filepath.h>

#include <algorithm>

using namespace Utils;

namespace LlamaCpp::Tools {

namespace {

const bool registered = [] {
    ToolFactory::instance().registerCreator(EditFileTool{}.name(),
                                            []() { return std::make_unique<EditFileTool>(); });
    return true;
}();

//! "oldText" for a single edit, "edits[2].oldText" for several.
QString editRef(const char *field, int index, int total)
{
    if (total == 1)
        return QString::fromLatin1(field);
    return QStringLiteral("edits[%1].%2").arg(index).arg(QLatin1String(field));
}

struct Region
{
    int editIndex = 0;
    int start = 0;
    int length = 0;
    QString replacement;
};

} // namespace

QString applyTextEdits(const QString &content,
                       const QVector<QPair<QString, QString>> &edits,
                       bool replaceAll,
                       QString &newContentOut,
                       int *replacementsOut)
{
    if (edits.isEmpty())
        return QStringLiteral("no edits given");

    QString bom;
    QString text = content;
    if (text.startsWith(QChar(0xFEFF))) {
        bom = text.left(1);
        text.remove(0, 1);
    }

    // Work in LF space, restore the dominant line ending at the end.
    const bool crlf = text.contains(QStringLiteral("\r\n"));
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));

    QVector<Region> regions;
    const int total = edits.size();

    for (int i = 0; i < total; ++i) {
        const QString old = edits.at(i).first;
        const QString replacement = edits.at(i).second;
        if (old.trimmed().isEmpty())
            return QStringLiteral("%1 must not be empty").arg(editRef("oldText", i, total));
        if (old == replacement)
            return QStringLiteral("%1: oldText and newText are identical")
                        .arg(editRef("oldText", i, total));

        // Pass 1: exact match in the original (LF-normalized) content.
        if (text.indexOf(old) != -1) {
            int pos = 0;
            int count = 0;
            QVector<int> positions;
            while ((pos = text.indexOf(old, pos)) != -1) {
                positions.append(pos);
                pos += old.size();
                ++count;
            }
            if (count > 1 && !replaceAll)
                return QStringLiteral(
                           "Found %1 occurrences of %2 in the file. The text must be unique – "
                           "provide more surrounding context, or set replace_all to true.")
                            .arg(count)
                            .arg(editRef("oldText", i, total));
            for (const int start : std::as_const(positions))
                regions.append({i, start, static_cast<int>(old.size()), replacement});
            continue;
        }

        // Pass 2: fuzzy, line-based match (tolerates trailing whitespace and
        // unicode punctuation differences).  The old text must then consist
        // of complete file lines; only those lines are replaced, unchanged
        // lines keep their original bytes.
        QStringList pattern = old.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
        const QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

        auto findPositions = [&lines](const QStringList &pat) {
            QVector<int> positions;
            int cursor = 0;
            while (true) {
                const int found = Patch::locateLines(lines, pat, cursor, false);
                if (found == -1)
                    break;
                positions.append(found);
                cursor = found + pat.size();
            }
            return positions;
        };

        QVector<int> positions = findPositions(pattern);
        if (positions.isEmpty() && !pattern.isEmpty() && pattern.last().isEmpty()) {
            // Retry without a trailing empty line (tolerates a missing final newline).
            pattern.removeLast();
            positions = findPositions(pattern);
        }

        if (positions.isEmpty())
            return QStringLiteral(
                       "Could not find %1 in the file. The text must match exactly, "
                       "including whitespace and indentation.")
                        .arg(editRef("oldText", i, total));
        if (positions.size() > 1 && !replaceAll)
            return QStringLiteral(
                       "Found %1 occurrences of %2 in the file. The text must be unique – "
                       "provide more surrounding context, or set replace_all to true.")
                        .arg(positions.size())
                        .arg(editRef("oldText", i, total));

        const int patternSize = pattern.size();
        for (const int linePos : std::as_const(positions)) {
            int start = 0;
            for (int l = 0; l < linePos; ++l)
                start += lines.at(l).size() + 1;
            int length = 0;
            for (int l = linePos; l < linePos + patternSize; ++l)
                length += lines.at(l).size() + 1;
            length -= 1; // no newline after the last matched line
            regions.append({i, start, length, replacement});
        }
    }

    // Overlap check – all regions were matched against the original content.
    std::sort(regions.begin(), regions.end(),
              [](const Region &a, const Region &b) { return a.start < b.start; });
    for (int i = 1; i < regions.size(); ++i) {
        const Region &previous = regions.at(i - 1);
        const Region &current = regions.at(i);
        if (previous.editIndex != current.editIndex
            && previous.start + previous.length > current.start)
            return QStringLiteral("edits[%1] and edits[%2] overlap in the file. "
                                  "Merge them into one edit or target disjoint regions.")
                        .arg(previous.editIndex)
                        .arg(current.editIndex);
    }

    QString result = text;
    for (int i = regions.size() - 1; i >= 0; --i) {
        const Region &r = regions.at(i);
        result.replace(r.start, r.length, r.replacement);
    }

    if (result == text)
        return QStringLiteral(
            "No changes made: the replacements produced identical content. "
            "This can happen when the text differs only in special characters.");

    if (crlf)
        result.replace(QStringLiteral("\n"), QStringLiteral("\r\n"));

    newContentOut = bom + result;
    if (replacementsOut)
        *replacementsOut = regions.size();
    return {};
}

/* -------------------------------------------------------------- EditFileTool */

QString EditFileTool::name() const
{
    return QStringLiteral("edit_file");
}

QString EditFileTool::toolDefinition() const
{
    const QString description = R"desc(
Edit a file by replacing exact text. Use this for small, targeted changes to a single file; use apply_patch for multi-file changes, file renames and deletions.

- oldText must match the file exactly, including whitespace and indentation.
- Every oldText must occur exactly once in the file, unless replace_all is true (then every occurrence is replaced). When the text occurs several times, add surrounding context lines to make it unique.
- Multiple edits in one call are all matched against the original file (not incrementally) and must not overlap. If two changes touch the same block or nearby lines, merge them into one edit instead.

Matching falls back to a line-based fuzzy match when the exact match fails: it then tolerates trailing whitespace and unicode quote/dash differences, but oldText must consist of complete lines.

Guidelines:
- Read the file first (read_file) and copy the current text into oldText verbatim.
- Keep oldText as small as possible while still being unique in the file.
)desc";

    QJsonObject oldTextProperty;
    oldTextProperty[QStringLiteral("type")] = QStringLiteral("string");
    oldTextProperty[QStringLiteral("description")] =
        QStringLiteral("Exact text to replace. It must be unique in the original file "
                       "and must not overlap with any other edits[].oldText in the same call.");

    QJsonObject newTextProperty;
    newTextProperty[QStringLiteral("type")] = QStringLiteral("string");
    newTextProperty[QStringLiteral("description")] =
        QStringLiteral("Replacement text. Must differ from oldText.");

    QJsonObject editProperties;
    editProperties[QStringLiteral("oldText")] = oldTextProperty;
    editProperties[QStringLiteral("newText")] = newTextProperty;

    QJsonObject editItem;
    editItem[QStringLiteral("type")] = QStringLiteral("object");
    editItem[QStringLiteral("properties")] = editProperties;
    editItem[QStringLiteral("required")] = QJsonArray{QStringLiteral("oldText"),
                                                       QStringLiteral("newText")};
    editItem[QStringLiteral("additionalProperties")] = false;

    QJsonObject editsProperty;
    editsProperty[QStringLiteral("type")] = QStringLiteral("array");
    editsProperty[QStringLiteral("items")] = editItem;
    editsProperty[QStringLiteral("minItems")] = 1;
    editsProperty[QStringLiteral("description")] = QStringLiteral(
        "One or more targeted replacements. Each edit is matched against the original "
        "file, not after earlier edits are applied.");

    QJsonObject pathProperty;
    pathProperty[QStringLiteral("type")] = QStringLiteral("string");
    pathProperty[QStringLiteral("description")] =
        QStringLiteral("The file to edit. Relative to the project directory, or absolute.");

    QJsonObject replaceAllProperty;
    replaceAllProperty[QStringLiteral("type")] = QStringLiteral("boolean");
    replaceAllProperty[QStringLiteral("description")] =
        QStringLiteral("Replace every occurrence of each oldText instead of requiring a "
                       "unique match. Defaults to false.");

    QJsonObject properties;
    properties[QStringLiteral("path")] = pathProperty;
    properties[QStringLiteral("edits")] = editsProperty;
    properties[QStringLiteral("replace_all")] = replaceAllProperty;

    QJsonObject parameters;
    parameters[QStringLiteral("type")] = QStringLiteral("object");
    parameters[QStringLiteral("properties")] = properties;
    parameters[QStringLiteral("required")] = QJsonArray{QStringLiteral("path"),
                                                         QStringLiteral("edits")};
    parameters[QStringLiteral("additionalProperties")] = false;

    QJsonObject function;
    function[QStringLiteral("name")] = QStringLiteral("edit_file");
    function[QStringLiteral("description")] = description.trimmed();
    function[QStringLiteral("parameters")] = parameters;

    QJsonObject root;
    root[QStringLiteral("type")] = QStringLiteral("function");
    root[QStringLiteral("function")] = function;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString EditFileTool::oneLineSummary(const QJsonObject &args) const
{
    const QString path = args.value("path").toString();
    if (path.isEmpty())
        return Tr::tr("Edit file");
    const int count = args.value("edits").toArray().size();
    if (count > 1)
        return Tr::tr("edit %1 (%2 blocks)").arg(path).arg(count);
    return Tr::tr("edit %1").arg(path);
}

QString EditFileTool::streamingSummary(const QString &partialArgs) const
{
    // "path" is the first field, so a usable summary appears almost
    // immediately while the arguments are still streaming in.
    int idx = partialArgs.indexOf(QStringLiteral("\"path\""));
    if (idx == -1)
        return {};
    idx = partialArgs.indexOf(QLatin1Char(':'), idx);
    if (idx == -1)
        return {};
    const int start = partialArgs.indexOf(QLatin1Char('"'), idx + 1);
    if (start == -1)
        return {};
    int end = start + 1;
    while (end < partialArgs.size()) {
        const QChar c = partialArgs.at(end);
        if (c == QLatin1Char('"') || c == QLatin1Char('\\'))
            break;
        ++end;
    }
    const QString path = partialArgs.mid(start + 1, end - start - 1);
    if (path.isEmpty())
        return {};
    return Tr::tr("edit %1").arg(path);
}

QString EditFileTool::detailsMarkdown(const QJsonObject &args, const QString &result, bool ok) const
{
    if (!ok)
        return QStringLiteral("**%1**\n\n%2").arg(Tr::tr("Error"), result);

    const QString path = args.value("path").toString();
    QString md = QStringLiteral("**%1** `%2`\n\n").arg(Tr::tr("edited"), path);

    int n = 1;
    for (const QJsonValue &value : args.value("edits").toArray()) {
        const QJsonObject edit = value.toObject();
        QString diff;
        for (const QString &line : edit.value("oldText").toString().split(QLatin1Char('\n')))
            diff += QLatin1Char('-') + line + QLatin1Char('\n');
        for (const QString &line : edit.value("newText").toString().split(QLatin1Char('\n')))
            diff += QLatin1Char('+') + line + QLatin1Char('\n');
        md += QStringLiteral("**%1**\n\n").arg(n++) + codeFence(diff, QStringLiteral("diff")) + QStringLiteral("\n\n");
    }
    return md.trimmed();
}

QString EditFileTool::summaryPreview(const QJsonObject &args, const QString &result, bool ok) const
{
    // The diff is the interesting part; give it a couple of extra lines.
    return truncatedPreview(detailsMarkdown(args, result, ok), 8);
}

void EditFileTool::run(const QJsonObject &args,
                       std::function<void(const QString &, bool)> done) const
{
    const QString path = args.value("path").toString().trimmed();
    if (path.isEmpty())
        return done(Tr::tr("Tool error: \"path\" must be a non-empty string."), false);

    QVector<QPair<QString, QString>> edits;
    for (const QJsonValue &value : args.value("edits").toArray()) {
        const QJsonObject edit = value.toObject();
        if (!edit.contains("oldText") || !edit.contains("newText")) {
            return done(Tr::tr("Tool error: every edit needs an \"oldText\" and a \"newText\"."),
                        false);
        }
        edits.append({edit.value("oldText").toString(), edit.value("newText").toString()});
    }
    if (edits.isEmpty())
        return done(Tr::tr("Tool error: \"edits\" must contain at least one replacement."),
                    false);

    const bool replaceAll = args.value("replace_all").toBool();

    const FilePath target = absoluteProjectPath(FilePath::fromUserInput(path));
    if (!target.isFile())
        return done(Tr::tr("Cannot edit \"%1\": file not found. Use read_file to verify the "
                           "path, or write to create the file.")
                        .arg(path),
                    false);

    const Result<QByteArray> readRes = target.fileContents();
    if (!readRes)
        return done(Tr::tr("Cannot edit \"%1\": %2").arg(path, readRes.error()), false);

    QString newContent;
    int replacements = 0;
    const QString error =
        applyTextEdits(QString::fromUtf8(readRes.value()), edits, replaceAll, newContent, &replacements);
    if (!error.isEmpty())
        return done(Tr::tr("Cannot edit \"%1\": %2").arg(path, error), false);

    const Result<qint64> writeRes = target.writeFileContents(newContent.toUtf8());
    if (!writeRes)
        return done(Tr::tr("Cannot edit \"%1\": %2").arg(path, writeRes.error()), false);

    return done(Tr::tr("Successfully replaced %1 block(s) in %2.").arg(replacements).arg(path), true);
}

} // namespace LlamaCpp::Tools
