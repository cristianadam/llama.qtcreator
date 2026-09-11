// Test stub: keeps the tools unit test free of a full Qt Creator
// initialization.  Provides only what the tools call on DocumentManager.
#pragma once

#include <utils/filepath.h>

namespace Core {

class DocumentManager
{
public:
    static void setProjectsDirectory(const Utils::FilePath &path)
    {
        projectsDirectory() = path;
    }

    static Utils::FilePath &projectsDirectory()
    {
        static Utils::FilePath path;
        return path;
    }
};

} // namespace Core
