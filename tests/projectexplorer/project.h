// Test stub: mirrors the subset of ProjectExplorer::Project used by the
// tools and the settings code, without requiring a running Qt Creator.
#pragma once

#include <utils/filepath.h>
#include <utils/store.h>
#include <utils/storekey.h>

#include <QVariant>

namespace ProjectExplorer {

class Project
{
public:
    Utils::FilePath projectDirectory() const { return {}; }

    QVariant namedSettings(const Utils::Key &name) const { return {}; }
    void setNamedSettings(const Utils::Key &name, const QVariant &value)
    {
    }
};

} // namespace ProjectExplorer
