#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

using namespace ProjectExplorer;
using namespace Utils;

FilePath absoluteProjectPath(const FilePath &relPath)
{
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    const FilePath generalFilePath = cwd.pathAppended(relPath.path());

    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();
    const FilePath projectFilePath = cwd.pathAppended(relPath.path());

    const FilePath targetFile = relPath.isAbsolutePath()   ? relPath
                                : projectFilePath.exists() ? projectFilePath
                                                           : generalFilePath;

    return targetFile;
}
