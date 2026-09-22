// Test stub: mirrors the subset of ProjectExplorer::Project used by the
// tools and the settings code, without requiring a running Qt Creator.
#pragma once

#include <utils/filepath.h>
#include <utils/store.h>
#include <utils/storekey.h>

#include <QVariant>

namespace ProjectExplorer {

class Kit;

class Project
{
public:
    Utils::FilePath projectDirectory() const { return m_projectDirectory; }

    // Test-only: set the directory the stub project reports.
    void setProjectDirectoryForTest(const QString &path)
    {
        m_projectDirectory = Utils::FilePath::fromString(path);
    }

    // No kit exists in the unit test environment.
    Kit *activeKit() const { return nullptr; }

    QVariant namedSettings(const Utils::Key &name) const { return {}; }
    void setNamedSettings(const Utils::Key &name, const QVariant &value)
    {
    }

private:
    Utils::FilePath m_projectDirectory;
};

} // namespace ProjectExplorer
