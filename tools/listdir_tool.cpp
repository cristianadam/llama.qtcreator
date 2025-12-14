#include "listdir_tool.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp {

QString ListDirTool::name() const
{
    return QStringLiteral("list_directory");
}

QString ListDirTool::toolDefinition() const
{
    return R"raw(
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
}

QString ListDirTool::oneLineSummary(const QJsonObject &args) const
{
    const QString dir = args.value("directory_path").toString();
    return Tr::tr("list directory %1").arg(dir.isEmpty() ? "." : dir);
}

void ListDirTool::run(const QJsonObject &args, std::function<void(const QString &, bool)> done) const
{
    const FilePath rawPath = FilePath::fromUserInput(args.value("directory_path").toString());

    // Resolve the directory path – same strategy as other tools.
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    const FilePath dirPath = rawPath.isAbsolutePath()
                                 ? rawPath
                                 : cwd.pathAppended(rawPath.path()).absoluteFilePath();

    if (!dirPath.exists())
        return done(Tr::tr("Directory \"%1\" does not exist.").arg(dirPath.toUserOutput()), false);

    if (!dirPath.isDir())
        return done(Tr::tr("\"%1\" is not a directory.").arg(dirPath.toUserOutput()), false);

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

    return done(lines.join('\n'), true);
}

} // namespace LlamaCpp
