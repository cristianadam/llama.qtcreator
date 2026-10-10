#include "projectinstructions.h"

#include "llamasettings.h"

#include <QFile>
#include <QDir>

#include <projectexplorer/project.h>

using namespace Utils;

namespace LlamaCpp {

namespace {

//! Instructions files larger than this are truncated: they are ambient
//! context for every chat request, not a document to be read in full.
constexpr int kMaxInstructionsBytes = 32 * 1024;

bool isInside(const FilePath &parent, const FilePath &child)
{
    const QString p = parent.path();
    const QString c = child.path();
    return c == p || c.startsWith(p.endsWith(QLatin1Char('/')) ? p : p + QLatin1Char('/'));
}

FilePath firstExistingCandidate(const FilePath &dir)
{
    for (const QString &name : projectInstructionFileNames()) {
        const FilePath candidate = dir.pathAppended(name);
        if (candidate.isFile())
            return candidate;
    }
    return {};
}

//! The git root of \a dir: the first ancestor (including \a dir itself)
//! containing a \c .git file or directory; empty when there is none.
FilePath gitRootOf(const FilePath &dir)
{
    FilePath current = dir;
    while (!current.isEmpty()) {
        if (current.pathAppended(QStringLiteral(".git")).exists())
            return current;
        const FilePath parent = current.parentDir();
        if (parent == current) // reached the file system root
            return {};
        current = parent;
    }
    return {};
}

//! Truncates \a data to at most \a maxBytes on a UTF‑8 character boundary.
void truncateUtf8(QByteArray &data, int maxBytes)
{
    if (data.size() <= maxBytes)
        return;
    data.truncate(maxBytes);
    // A cut inside a multi‑byte sequence leaves a dangling leading byte
    // followed by continuation bytes (10xxxxxx); drop the whole sequence.
    // A trailing continuation byte can only belong to a complete sequence
    // when the cut landed exactly on a character boundary, so the lead byte
    // is only chopped when it is really a lead byte (110xxxxx / 1110xxxx).
    while (!data.isEmpty() && (static_cast<uchar>(data.at(data.size() - 1)) & 0xC0) == 0x80)
        data.chop(1);
    if (!data.isEmpty() && (static_cast<uchar>(data.at(data.size() - 1)) & 0xC0) == 0xC0)
        data.chop(1);
}

} // namespace

QStringList projectInstructionFileNames()
{
    return {QStringLiteral("AGENTS.md"), QStringLiteral("CLAUDE.md")};
}

FilePath findProjectInstructionsFile(const FilePath &projectDir)
{
    if (!projectDir.isDir())
        return {};

    const FilePath file = firstExistingCandidate(projectDir);
    if (!file.isEmpty())
        return file;

    const FilePath gitRoot = gitRootOf(projectDir);
    if (gitRoot.isEmpty())
        return {};

    FilePath current = projectDir.parentDir();
    while (isInside(gitRoot, current)) {
        const FilePath candidate = firstExistingCandidate(current);
        if (!candidate.isEmpty())
            return candidate;
        current = current.parentDir();
    }
    return {};
}

QString loadProjectInstructions(const FilePath &projectDir)
{
    const FilePath file = findProjectInstructionsFile(projectDir);
    if (file.isEmpty())
        return {};

    QFile f(file.path());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    const qint64 fileSize = f.size();
    // Read at most the cap: the boundary walk in truncateUtf8 only ever
    // looks backwards from the cut, so nothing beyond it is needed.
    QByteArray data = f.read(kMaxInstructionsBytes);
    f.close();

    truncateUtf8(data, kMaxInstructionsBytes);
    const bool truncated = data.size() < fileSize;

    QString text = QString::fromUtf8(data);
    if (text.startsWith(QChar(0xFEFF)))
        text.remove(0, 1);
    if (text.trimmed().isEmpty())
        return {};

    if (truncated)
        text += QStringLiteral("\n\n[... truncated ...]");

    return QStringLiteral("Project instructions from %1:\n%2").arg(file.path(), text);
}

bool projectInstructionsEnabled(ProjectExplorer::Project *project)
{
    if (project)
        return LlamaProjectSettings(project).isLoadProjectInstructionsEnabled();
    return settings().loadProjectInstructions();
}

} // namespace LlamaCpp
