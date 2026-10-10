#include "llamalocatorprompts.h"

#include <algorithm>

namespace LlamaCpp {

namespace {

// Private-Use-Area characters that never occur in user content; they shield
// the replacement values from a second round of placeholder matching.
const QString kTokenStart = QString(QChar(0xE000));
const QString kTokenEnd = QString(QChar(0xE001));

QString tokenFor(int index)
{
    return kTokenStart + QString::number(index) + kTokenEnd;
}

constexpr int kMaxDiagnostics = 25;
constexpr int kMaxDiffChars = 100000;
constexpr int kMaxUntrackedFiles = 50;

} // namespace

QString applyLocatorPlaceholders(const QString &prompt, const LocatorPlaceholders &values)
{
    const struct {
        const char *placeholder;
        const QString &value;
    } entries[] = {
        {"{selection}", values.selection},
        {"{file}", values.file},
        {"{line}", values.line},
        {"{diagnostics}", values.diagnostics},
        {"{git_diff}", values.gitDiff},
    };

    QString result = prompt;
    QStringList tokens;
    for (const auto &entry : entries) {
        const QString placeholder = QLatin1String(entry.placeholder);
        if (!result.contains(placeholder))
            continue;
        const QString token = tokenFor(tokens.size());
        result.replace(placeholder, token);
        tokens << entry.value;
    }
    for (int i = 0; i < tokens.size(); ++i)
        result.replace(tokenFor(i), tokens.at(i));
    return result;
}

QString formatDiagnostics(const QList<LocatorDiagnostic> &diagnostics)
{
    if (diagnostics.isEmpty())
        return QStringLiteral(
            "(no errors or warnings reported for this file)");

    QList<LocatorDiagnostic> sorted = diagnostics;
    std::stable_sort(sorted.begin(),
                     sorted.end(),
                     [](const LocatorDiagnostic &a, const LocatorDiagnostic &b) {
                         return a.line < b.line;
                     });

    const int shown = qMin(sorted.size(), kMaxDiagnostics);
    QStringList lines;
    lines.reserve(shown + 1);
    for (int i = 0; i < shown; ++i)
        lines << QStringLiteral("line %1: %2").arg(sorted.at(i).line).arg(sorted.at(i).text);
    if (sorted.size() > shown)
        lines << QStringLiteral("... and %1 more").arg(sorted.size() - shown);
    return lines.join(QLatin1Char('\n'));
}

QString formatGitDiff(const QString &status, const QString &diff, bool gitFailed)
{
    if (gitFailed)
        return QStringLiteral(
            "(could not read the git repository: the file is probably not inside "
            "a git repository, or git is not available)");

    QStringList untracked;
    bool hasChanges = false;
    for (const QString &rawLine : status.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line.isEmpty())
            continue;
        hasChanges = true;
        if (line.startsWith(QLatin1String("??")))
            untracked << line.mid(2).trimmed();
    }
    if (!hasChanges)
        return QStringLiteral("(no uncommitted changes)");

    QString out;
    QString diffText = diff;
    if (diffText.size() > kMaxDiffChars) {
        const qsizetype lastNewline = QStringView(diffText).left(kMaxDiffChars).lastIndexOf('\n');
        diffText.truncate(lastNewline >= 0 ? lastNewline + 1 : kMaxDiffChars);
        out += QStringLiteral("... diff truncated (%1 characters total) ...\n").arg(diff.size());
    }
    if (!diffText.trimmed().isEmpty())
        out += QStringLiteral("\n```diff\n%1\n```\n").arg(diffText);
    if (!untracked.isEmpty()) {
        // A repository full of untracked files (e.g. a build directory that
        // is not in .gitignore) must not blow up the prompt like an
        // oversized diff would.
        QStringList listed = untracked;
        if (listed.size() > kMaxUntrackedFiles) {
            const int more = listed.size() - kMaxUntrackedFiles;
            listed = listed.mid(0, kMaxUntrackedFiles);
            listed << QStringLiteral("... and %1 more").arg(more);
        }
        out += QStringLiteral("Untracked files (not part of the diff):\n%1\n")
                   .arg(listed.join(QLatin1Char('\n')));
    }
    return out;
}

} // namespace LlamaCpp
