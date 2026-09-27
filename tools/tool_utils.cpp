#include "llamasettings.h"
#include "llamatr.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>

using namespace LlamaCpp;
using namespace ProjectExplorer;
using namespace Utils;

bool sandboxEnabled(Project *project)
{
#if defined(Q_OS_WIN)
    Q_UNUSED(project);
    return false;
#else
    if (project)
        return LlamaProjectSettings(project).isSandboxEnabled();
    return settings().sandboxCommands();
#endif
}

bool pathCovers(const QString &prefix, const QString &path)
{
    return path == prefix
            || path.startsWith(prefix.endsWith(QLatin1Char('/'))
                                       ? prefix
                                       : prefix + QLatin1Char('/'));
}

// Credential locations that sandboxed tools must not read, mirroring the
// denyRead defaults of the pi sandbox extension.
QStringList secretReadPaths()
{
    const QString home = QDir::homePath();
    return { home + QStringLiteral("/.ssh"),
             home + QStringLiteral("/.aws"),
             home + QStringLiteral("/.gnupg"),
             home + QStringLiteral("/.kube"),
             home + QStringLiteral("/.netrc") };
}

QString sandboxAccessError(const FilePath &path, bool isWrite)
{
    if (!sandboxEnabled(ProjectManager::startupProject()))
        return {};

    const QString p = path.toFSPathString();
    if (isWrite) {
        FilePath cwd = Core::DocumentManager::projectsDirectory();
        if (const Project *project = ProjectManager::startupProject())
            cwd = project->projectDirectory();
        if (pathCovers(cwd.toFSPathString(), p) || pathCovers(QDir::tempPath(), p))
            return {};
        return Tr::tr(
                   "Writing to \"%1\" is not allowed: the sandbox only permits "
                   "writes inside the project directory and temporary "
                   "locations.")
                .arg(p);
    }

    for (const QString &secret : secretReadPaths())
        if (pathCovers(secret, p))
            return Tr::tr(
                       "Reading \"%1\" is not allowed: credential locations "
                       "are not readable inside the sandbox.")
                    .arg(p);
    return {};
}

FilePath absoluteProjectPath(const FilePath &relPath, bool mustExist)
{
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    const FilePath generalFilePath = cwd.pathAppended(relPath.path());

    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();
    const FilePath projectFilePath = cwd.pathAppended(relPath.path());

    if (relPath.isAbsolutePath())
        return relPath;
    if (!mustExist)
        // The target may not exist yet (a file about to be created): resolve
        // against the project directory without an existence probe.
        return projectFilePath;

    return projectFilePath.exists() ? projectFilePath : generalFilePath;
}

QString codeLanguageFor(const QString &filePath)
{
    const QString fileName = QFileInfo(filePath).fileName().toLower();
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (fileName == "cmakelists.txt" || suffix == "cmake")
        return QStringLiteral("cmake");
    if (suffix == "cpp" || suffix == "cc" || suffix == "cxx" || suffix == "c++")
        return QStringLiteral("cpp");
    if (suffix == "h" || suffix == "hpp" || suffix == "hxx")
        return QStringLiteral("cpp");
    if (suffix == "py")
        return QStringLiteral("python");
    if (suffix == "js" || suffix == "jsx")
        return QStringLiteral("javascript");
    if (suffix == "ts" || suffix == "tsx")
        return QStringLiteral("typescript");
    if (suffix == "json")
        return QStringLiteral("json");
    if (suffix == "xml")
        return QStringLiteral("xml");
    // SVG has no dedicated syntax definition; XML highlighting reads fine.
    if (suffix == "svg")
        return QStringLiteral("xml");
    if (suffix == "yaml" || suffix == "yml")
        return QStringLiteral("yaml");
    if (suffix == "md")
        return QStringLiteral("markdown");
    if (suffix == "html")
        return QStringLiteral("html");
    if (suffix == "css")
        return QStringLiteral("css");
    if (suffix == "sh" || suffix == "bash")
        return QStringLiteral("bash");
    if (suffix == "pro")
        return QStringLiteral("text");
    if (suffix == "qml")
        return QStringLiteral("javascript");
    if (suffix == "sql")
        return QStringLiteral("sql");
    if (suffix == "rs")
        return QStringLiteral("rust");
    if (suffix == "go")
        return QStringLiteral("go");
    if (suffix == "java")
        return QStringLiteral("java");
    if (suffix == "rb")
        return QStringLiteral("ruby");
    if (suffix == "php")
        return QStringLiteral("php");
    if (suffix == "swift")
        return QStringLiteral("swift");
    if (suffix == "kt" || suffix == "kts")
        return QStringLiteral("kotlin");
    return QStringLiteral("text");
}
