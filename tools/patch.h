#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace LlamaCpp {
namespace Patch {

struct UpdateChunk
{
    QStringList oldLines; // lines as they appear in the file (context + removed)
    QStringList newLines; // lines the file should contain instead (context + added)
    QStringList rawLines; // change lines as received, with ' ' / '-' / '+' prefixes (diff display)
    QString changeContext; // optional text after "@@" used to disambiguate the match
    bool endOfFile = false; // anchor the chunk to the end of the file
};

enum class HunkType
{
    Add,
    Delete,
    Update
};

struct Hunk
{
    HunkType type = HunkType::Add;
    QString path;
    QString contents; // Add only
    QString movePath; // Update only, empty when there is no rename
    QVector<UpdateChunk> chunks; // Update only
};

/**
 * Parses a patch in the opencode apply_patch format:
 *
 *   *** Begin Patch
 *   *** Add File: <path>
 *   +line ...
 *   *** Update File: <path>
 *   *** Move to: <path>          (optional rename)
 *   @@ [context]
 *    context line
 *   -removed line
 *   +added line
 *   *** End of File              (optional, anchors the chunk to the file end)
 *   *** Delete File: <path>
 *   *** End Patch
 *
 * Returns an empty string on success, otherwise a human‑readable error
 * message. On success the parsed hunks are written to \a hunksOut.
 */
QString parse(const QString &patchText, QVector<Hunk> &hunksOut);

/**
 * Fuzzy‑locates \a pattern inside \a lines, searching forward from
 * \a startIndex. Tries four passes in order: exact, trailing‑whitespace,
 * whitespace and unicode‑punctuation insensitive.
 *
 * Returns the 0‑based index of the match, or -1 if not found.
 */
int locateLines(const QStringList &lines,
                const QStringList &pattern,
                int startIndex,
                bool eof = false);

/**
 * Applies the update chunks to \a oldContents and writes the new content to
 * \a newContentsOut.
 *
 * Each chunk's old lines are located in the file by a four‑pass fuzzy match
 * (exact, trailing‑whitespace‑insensitive, whitespace‑insensitive, unicode
 * punctuation normalized). Chunks must appear in file order; a chunk's
 * search starts right after the previous chunk's match.
 *
 * Returns an empty string on success, otherwise a human‑readable error
 * message.
 */
QString applyUpdateChunks(const QString &oldContents,
                          const QVector<UpdateChunk> &chunks,
                          const QString &filePath,
                          QString &newContentsOut);

} // namespace Patch
} // namespace LlamaCpp
