#include "patch.h"

#include <QRegularExpression>

#include <algorithm>
#include <functional>

namespace LlamaCpp {
namespace Patch {

namespace {

QString stripHeredoc(const QString &input)
{
    // Tolerate patches wrapped in a heredoc: cat <<'EOF'\n...\nEOF or <<EOF\n...\nEOF
    static const QRegularExpression re(
        QStringLiteral(R"(^(?:cat\s+)?<<['"]?(\w+)['"]?\s*\n([\s\S]*?)\n\1\s*$)"));
    const auto match = re.match(input);
    if (match.hasMatch())
        return match.captured(2);
    return input;
}

QString rightTrim(const QString &s)
{
    int end = s.size();
    while (end > 0 && s.at(end - 1).isSpace())
        --end;
    return s.left(end);
}

QString normalizeUnicode(QString s)
{
    static const QRegularExpression singleQuote(QStringLiteral("[\u2018\u2019\u201A\u201B]"));
    static const QRegularExpression doubleQuote(QStringLiteral("[\u201C\u201D\u201E\u201F]"));
    static const QRegularExpression dash(QStringLiteral("[\u2010\u2011\u2012\u2013\u2014\u2015]"));
    static const QRegularExpression ellipsis(QStringLiteral("\u2026"));
    static const QRegularExpression noBreakSpace(QStringLiteral("\u00A0"));

    s.replace(singleQuote, QStringLiteral("'"));
    s.replace(doubleQuote, QStringLiteral("\""));
    s.replace(dash, QStringLiteral("-"));
    s.replace(ellipsis, QStringLiteral("..."));
    s.replace(noBreakSpace, QStringLiteral(" "));
    return s;
}

bool lineEquals(int pass, const QString &a, const QString &b)
{
    switch (pass) {
    case 0: // exact
        return a == b;
    case 1: // trailing whitespace insensitive
        return rightTrim(a) == rightTrim(b);
    case 2: // whitespace insensitive
        return a.trimmed() == b.trimmed();
    default: // unicode punctuation normalized
        return normalizeUnicode(a.trimmed()) == normalizeUnicode(b.trimmed());
    }
}

int tryMatch(const QStringList &lines,
             const QStringList &pattern,
             int startIndex,
             int pass,
             bool eof)
{
    const int n = lines.size();
    const int p = pattern.size();
    if (p == 0 || n < p)
        return -1;

    auto matchesAt = [pass, &lines, &pattern](int pos) {
        for (int j = 0; j < pattern.size(); ++j) {
            if (!lineEquals(pass, lines.at(pos + j), pattern.at(j)))
                return false;
        }
        return true;
    };

    // With an EOF anchor, try matching at the very end of the file first
    if (eof) {
        const int fromEnd = n - p;
        if (fromEnd >= 0 && matchesAt(fromEnd))
            return fromEnd;
    }

    // Forward search from the cursor
    for (int pos = qMax(0, startIndex); pos + p <= n; ++pos) {
        if (matchesAt(pos))
            return pos;
    }

    return -1;
}

} // namespace

int locateLines(const QStringList &lines,
                const QStringList &pattern,
                int startIndex,
                bool eof)
{
    if (pattern.isEmpty())
        return -1;

    for (int pass = 0; pass < 4; ++pass) {
        const int found = tryMatch(lines, pattern, startIndex, pass, eof);
        if (found != -1)
            return found;
    }
    return -1;
}

QString parse(const QString &patchText, QVector<Hunk> &hunksOut)
{
    const QString text = stripHeredoc(patchText.trimmed());
    QStringList lines = text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);

    bool hasBegin = false;
    bool hasEnd = false;
    bool hasSection = false;
    for (const QString &line : std::as_const(lines)) {
        const QString trimmed = line.trimmed();
        if (trimmed == QStringLiteral("*** Begin Patch"))
            hasBegin = true;
        else if (trimmed == QStringLiteral("*** End Patch"))
            hasEnd = true;
        else if (trimmed.startsWith(QStringLiteral("*** Add File:"))
                 || trimmed.startsWith(QStringLiteral("*** Update File:"))
                 || trimmed.startsWith(QStringLiteral("*** Delete File:")))
            hasSection = true;
    }

    // Tolerate models that forgot the envelope markers
    if (hasSection && (!hasBegin || !hasEnd)) {
        QStringList wrapped;
        if (!hasBegin)
            wrapped << QStringLiteral("*** Begin Patch");
        for (const QString &line : std::as_const(lines))
            wrapped << line;
        if (!hasEnd)
            wrapped << QStringLiteral("*** End Patch");
        lines = wrapped;
    }

    int beginIdx = -1;
    int endIdx = -1;
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (trimmed == QStringLiteral("*** Begin Patch"))
            beginIdx = i;
        else if (trimmed == QStringLiteral("*** End Patch"))
            endIdx = i;
    }

    if (beginIdx == -1 || endIdx == -1 || beginIdx >= endIdx)
        return QStringLiteral("Invalid patch format: missing Begin/End markers");

    int i = beginIdx + 1;
    while (i < endIdx) {
        const QString line = lines.at(i);

        if (line.startsWith(QStringLiteral("*** Add File:"))) {
            const QString path = line.mid(QStringLiteral("*** Add File:").size()).trimmed();
            if (path.isEmpty())
                return QStringLiteral("Invalid add file path");

            Hunk hunk;
            hunk.type = HunkType::Add;
            hunk.path = path;

            ++i;
            while (i < endIdx && !lines.at(i).startsWith(QLatin1Char('*'))) {
                if (lines.at(i).startsWith(QLatin1Char('+')))
                    hunk.contents += lines.at(i).mid(1) + QLatin1Char('\n');
                ++i;
            }
            if (hunk.contents.endsWith(QLatin1Char('\n')))
                hunk.contents.chop(1);

            hunksOut.append(hunk);
        } else if (line.startsWith(QStringLiteral("*** Delete File:"))) {
            const QString path = line.mid(QStringLiteral("*** Delete File:").size()).trimmed();
            if (path.isEmpty())
                return QStringLiteral("Invalid delete file path");

            Hunk hunk;
            hunk.type = HunkType::Delete;
            hunk.path = path;
            hunksOut.append(hunk);

            ++i;
        } else if (line.startsWith(QStringLiteral("*** Update File:"))) {
            const QString path = line.mid(QStringLiteral("*** Update File:").size()).trimmed();
            if (path.isEmpty())
                return QStringLiteral("Invalid update file path");

            Hunk hunk;
            hunk.type = HunkType::Update;
            hunk.path = path;

            ++i;
            if (i < endIdx && lines.at(i).startsWith(QStringLiteral("*** Move to:"))) {
                hunk.movePath = lines.at(i).mid(QStringLiteral("*** Move to:").size()).trimmed();
                if (hunk.movePath.isEmpty())
                    return QStringLiteral("Invalid move file path");
                ++i;
            }

            while (i < endIdx && !lines.at(i).startsWith(QLatin1Char('*'))) {
                if (!lines.at(i).startsWith(QLatin1Char('@'))) {
                    ++i;
                    continue;
                }

                UpdateChunk chunk;
                chunk.changeContext = lines.at(i).mid(2).trimmed();
                ++i;

                while (i < endIdx && !lines.at(i).startsWith(QLatin1Char('@'))) {
                    const QString changeLine = lines.at(i);
                    if (changeLine == QStringLiteral("*** End of File")) {
                        chunk.endOfFile = true;
                        ++i;
                        break;
                    }
                    if (changeLine.startsWith(QLatin1Char('*')))
                        break;
                    if (changeLine.startsWith(QLatin1Char(' '))) {
                        chunk.oldLines << changeLine.mid(1);
                        chunk.newLines << changeLine.mid(1);
                        chunk.rawLines << changeLine;
                    } else if (changeLine.startsWith(QLatin1Char('-'))) {
                        chunk.oldLines << changeLine.mid(1);
                        chunk.rawLines << changeLine;
                    } else if (changeLine.startsWith(QLatin1Char('+'))) {
                        chunk.newLines << changeLine.mid(1);
                        chunk.rawLines << changeLine;
                    }
                    ++i;
                }

                hunk.chunks.append(chunk);
            }

            if (hunk.chunks.isEmpty())
                return QStringLiteral("Invalid update hunk for %1: expected at least one @@ chunk")
                            .arg(path);

            hunksOut.append(hunk);
        } else {
            ++i;
        }
    }

    if (hunksOut.isEmpty())
        return QStringLiteral("no hunks found");

    return {};
}

QString applyUpdateChunks(const QString &oldContents,
                          const QVector<UpdateChunk> &chunks,
                          const QString &filePath,
                          QString &newContentsOut)
{
    QString bom;
    QString body = oldContents;
    if (body.startsWith(QChar(0xFEFF))) {
        bom = body.left(1);
        body.remove(0, 1);
    }

    QStringList lines = body.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    // Drop the trailing empty element for consistent line counting
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    struct Replacement
    {
        int start;
        int count;
        QStringList newLines;
    };
    QVector<Replacement> replacements;
    int lineIndex = 0;

    for (const UpdateChunk &chunk : std::as_const(chunks)) {
        if (!chunk.changeContext.isEmpty()) {
            const int contextIdx = locateLines(lines, {chunk.changeContext}, lineIndex, false);
            if (contextIdx == -1)
                return QStringLiteral("Failed to find context '%1' in %2")
                            .arg(chunk.changeContext, filePath);
            lineIndex = contextIdx + 1;
        }

        if (chunk.oldLines.isEmpty()) {
            const int insertionIdx = (!lines.isEmpty() && lines.last().isEmpty()) ? lines.size() - 1
                                                                                  : lines.size();
            replacements.append({insertionIdx, 0, chunk.newLines});
            continue;
        }

        QStringList pattern = chunk.oldLines;
        QStringList newSlice = chunk.newLines;
        int found = locateLines(lines, pattern, lineIndex, chunk.endOfFile);

        // Retry without a trailing empty line (tolerates a missing final newline)
        if (found == -1 && !pattern.isEmpty() && pattern.last().isEmpty()) {
            pattern.removeLast();
            if (!newSlice.isEmpty() && newSlice.last().isEmpty())
                newSlice.removeLast();
            found = locateLines(lines, pattern, lineIndex, chunk.endOfFile);
        }

        if (found == -1)
            return QStringLiteral("Failed to find expected lines in %1:\n%2")
                        .arg(filePath, chunk.oldLines.join(QLatin1Char('\n')));

        replacements.append({found, static_cast<int>(pattern.size()), newSlice});
        lineIndex = found + static_cast<int>(pattern.size());
    }

    std::sort(replacements.begin(),
              replacements.end(),
              [](const Replacement &a, const Replacement &b) { return a.start < b.start; });

    // Apply in reverse order so earlier indices stay valid
    for (int i = replacements.size() - 1; i >= 0; --i) {
        const Replacement &r = replacements.at(i);
        lines.erase(lines.begin() + r.start, lines.begin() + r.start + r.count);
        for (int j = r.newLines.size() - 1; j >= 0; --j)
            lines.insert(r.start, r.newLines.at(j));
    }

    QString result = lines.join(QLatin1Char('\n'));
    if (!result.isEmpty() && !result.endsWith(QLatin1Char('\n')))
        result += QLatin1Char('\n');

    newContentsOut = bom + result;
    return {};
}

} // namespace Patch
} // namespace LlamaCpp
