#pragma once
#include <QMap>
#include <QString>
#include <memory>

#include "tool.h"

namespace LlamaCpp {

/*! Supplies tools that are not implemented inside this plugin but served
    remotely, e.g. the tools exposed by the Qt Creator MCP server. */
class RemoteToolProvider
{
public:
    virtual ~RemoteToolProvider() = default;

    /*! Names of the tools currently available from the remote side. */
    virtual QStringList toolNames() const = 0;

    /*! Returns a freshly allocated tool or nullptr when the name is
        unknown to the provider. */
    virtual std::unique_ptr<Tool> createTool(const QString &name) const = 0;
};

class ToolFactory
{
public:
    static ToolFactory &instance()
    {
        static ToolFactory s;
        return s;
    }

    /** Returns a freshly allocated tool (owned by the caller) or nullptr
        if the name is unknown. Falls back to the remote tool provider
        (the Qt Creator MCP server) for unknown names. */
    std::unique_ptr<Tool> create(const QString &name) const
    {
        const auto it = m_creators.find(name);
        if (it != m_creators.end())
            return it.value()(); // invoke the stored lambda
        if (m_remoteProvider)
            return m_remoteProvider->createTool(name);
        return nullptr;
    }

    /** Register a creator lambda.  Called from each concrete *.cpp* file. */
    void registerCreator(const QString &name, std::function<std::unique_ptr<Tool>()> creator)
    {
        m_creators.insert(name, std::move(creator));
    }

    /** Returns the names of all registered creators, including the tools
        currently served by the remote provider. */
    QStringList creatorsList() const
    {
        QStringList names = m_creators.keys();
        if (m_remoteProvider) {
            const QStringList remoteNames = m_remoteProvider->toolNames();
            for (const QString &remoteName : remoteNames)
                if (!names.contains(remoteName))
                    names << remoteName;
        }
        return names;
    }

    /** Registers the provider of remote (MCP) tools. Not owned. */
    void setRemoteToolProvider(RemoteToolProvider *provider) { m_remoteProvider = provider; }

private:
    ToolFactory() = default;
    QMap<QString, std::function<std::unique_ptr<Tool>()>> m_creators;
    RemoteToolProvider *m_remoteProvider = nullptr;
};

} // namespace LlamaCpp
