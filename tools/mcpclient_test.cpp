// Plugin tests for the MCP client, run inside Qt Creator itself:
//
//   Qt Creator -pluginpath <this build dir> -test llamacpp[,McpClientTest]
//
// Unlike the standalone unit tests, these exercise the *real* usage path:
// the built-in Qt Creator MCP server (the "mcpserver" plugin), the
// McpBridge that connects to it, and the ToolFactory that publishes its
// tools to the chat. They are compiled only with -DWITH_TESTS=ON and
// registered by LlamaPlugin via addTestCreator().
//
// Note on timing: the bridge starts its handshake when the plugin's
// extensionsInitialized() runs. The handshake is a network round-trip that
// only completes once the event loop spins (after all plugins, including the
// ones that register MCP tools, are fully up). So the tests wait for the
// bridge to report a non-empty tool list rather than polling isConnected()
// and immediately reading the (possibly not-yet-populated) list.

#ifdef WITH_TESTS

#include "factory.h"
#include "mcpbridge.h"
#include "mcpclient.h"
#include "tool.h"

#include <coreplugin/mcp/mcpmanager.h>

#include <QtTest/QtTest>

#include <QJsonObject>
#include <QUrl>

#include <variant>

namespace LlamaCpp {

namespace {

// ID under which the builtin MCP server plugin registers itself with
// Core::McpManager (same constant McpBridge uses).
const char kQtCreatorMcpServerId[] = "QTCREATOR.BUILTIN.MCP.SERVER";

bool builtInServerRegistered()
{
    for (const Core::McpManager::ServerInfo &info : Core::McpManager::mcpServers()) {
        if (info.id == QLatin1String(kQtCreatorMcpServerId)
            && std::holds_alternative<QUrl>(info.launchInfo))
            return true;
    }
    return false;
}

// Wait until the bridge has connected *and* populated its tool list. A
// non-empty list implies a completed handshake (state Ready).
void waitForBridgeTools()
{
    QTRY_VERIFY_WITH_TIMEOUT(!McpBridge::instance().toolNames().isEmpty(), 30000);
}

} // namespace

class McpClientTest : public QObject
{
    Q_OBJECT

private slots:
    // The handshake (initialize -> notifications/initialized -> tools/list)
    // completes and the server's tools are published by the bridge.
    void testToolsListed()
    {
        if (!builtInServerRegistered())
            QSKIP("The Qt Creator builtin MCP server is not running; "
                  "enable the \"mcpserver\" plugin and re-run.");

        waitForBridgeTools();

        const QStringList names = McpBridge::instance().toolNames();
        QVERIFY2(!names.isEmpty(), "The MCP server reported no tools");
        // A stable part of the builtin server's tool set.
        QVERIFY(names.contains(QStringLiteral("project_list")));
        QVERIFY(names.contains(QStringLiteral("build_project")));
    }

    // The exact path that used to crash (heap-use-after-free in the
    // tools/call response handler): run a tool through the ToolFactory,
    // like ChatManager::executeToolAndSendResult does, and wait for the
    // result callback.
    void testCallTool()
    {
        if (!builtInServerRegistered())
            QSKIP("The Qt Creator builtin MCP server is not running; "
                  "enable the \"mcpserver\" plugin and re-run.");

        waitForBridgeTools();
        QVERIFY(McpBridge::instance().isMcpTool(QStringLiteral("project_list")));

        auto tool = ToolFactory::instance().create(QStringLiteral("project_list"));
        QVERIFY2(tool != nullptr, "ToolFactory does not provide project_list");

        bool done = false;
        QString output;
        bool ok = false;
        tool->run(QJsonObject{},
                  [&done, &output, &ok](const QString &out, bool success) {
                      output = out;
                      ok = success;
                      done = true;
                  });

        QTRY_VERIFY_WITH_TIMEOUT(done, 30000);
        QVERIFY2(ok, qPrintable(output));
    }
};

namespace Internal {

QObject *createMcpClientTest()
{
    return new McpClientTest;
}

} // namespace Internal

} // namespace LlamaCpp

#include "mcpclient_test.moc"

#endif // WITH_TESTS
