#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

#include <QFileInfo>

using namespace ProjectExplorer;
using namespace Utils;

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
