#pragma once
#include <QMap>
#include <QString>
#include <memory>

#include "tool.h"

namespace LlamaCpp {

class ToolFactory
{
public:
    static ToolFactory &instance()
    {
        static ToolFactory s;
        return s;
    }

    /** Returns a freshly allocated tool (owned by the caller) or nullptr
        if the name is unknown. */
    std::unique_ptr<Tool> create(const QString &name) const
    {
        const auto it = m_creators.find(name);
        if (it == m_creators.end())
            return nullptr;
        return it.value()(); // invoke the stored lambda
    }

    /** Register a creator lambda.  Called from each concrete *.cpp* file. */
    void registerCreator(const QString &name, std::function<std::unique_ptr<Tool>()> creator)
    {
        m_creators.insert(name, std::move(creator));
    }

    /** Returns the names of all registered creators. */
    QStringList creatorsList() const
    {
        return m_creators.keys();
    }

private:
    ToolFactory() = default;
    QMap<QString, std::function<std::unique_ptr<Tool>()>> m_creators;
};

} // namespace LlamaCpp
