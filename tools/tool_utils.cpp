#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

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
