#include <utils/filepath.h>

namespace Core {
class DocumentManager
{
public:
    inline static Utils::FilePath s_projectDirectory;

    static Utils::FilePath projectsDirectory() { return s_projectDirectory; }
    static void setProjectsDirectory(const Utils::FilePath &path) { s_projectDirectory = path; }
};
} // namespace Core
