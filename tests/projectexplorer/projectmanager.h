// Test stub: no projects exist in the unit test environment.
#pragma once

#include <projectexplorer/project.h>

namespace ProjectExplorer {

class ProjectManager
{
public:
    static Project *startupProject()
    {
        return nullptr;
    }
};

} // namespace ProjectExplorer
