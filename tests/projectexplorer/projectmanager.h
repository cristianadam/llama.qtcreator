// Test stub: by default no projects exist in the unit test environment.
// Tests may install a stub project via setStartupProject to exercise code
// paths that depend on ProjectManager::startupProject().
#pragma once

#include <projectexplorer/project.h>

namespace ProjectExplorer {

class ProjectManager
{
public:
    static Project *startupProject()
    {
        return s_startupProject;
    }

    //! Returns a stub project reporting \a directory as its project
    //! directory.  The pointer is owned by ProjectManager and stays valid
    //! until the next setStartupProject or resetStartupProject call.
    static Project *setStartupProject(const QString &projectDirectory)
    {
        // One stub instance for the whole test run – it is never touched
        // after being unregistered, so keeping it is harmless.
        static Project stub;
        stub.setProjectDirectoryForTest(projectDirectory);
        s_startupProject = &stub;
        return s_startupProject;
    }

    static void resetStartupProject()
    {
        s_startupProject = nullptr;
    }

private:
    inline static Project *s_startupProject = nullptr;
};

} // namespace ProjectExplorer
