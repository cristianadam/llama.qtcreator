#include "getinfo_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "tool_utils.h"

#include <utils/filepath.h>

#include <QSysInfo>

namespace LlamaCpp::Tools {

const bool registered = [] {
    ToolFactory::instance().registerCreator(GetInfoTool{}.name(),
                                            []() { return std::make_unique<GetInfoTool>(); });
    return true;
}();

QString GetInfoTool::name() const
{
    return QStringLiteral("get_info");
}

QString GetInfoTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "get_info",
            "description": "Returns runtime info: the OS name/version and the current working directory (the directory relative tool paths and bash commands use). Use it before running platform-specific commands, or when the user asks about the environment.",
            "parameters": {
                "type": "object",
                "properties": {},
                "required": [],
                "strict": true
            }
        }
    })raw";
}

QString GetInfoTool::oneLineSummary(const QJsonObject &args) const
{
    Q_UNUSED(args);
    return Tr::tr("get runtime info");
}

void GetInfoTool::run(const QJsonObject &args,
                      std::function<void(const QString &, bool)> done) const
{
    Q_UNUSED(args);
    // In-process equivalent of llama.cpp's server tool: QSysInfo gives the
    // OS name/version without spawning uname/ver, and the tools' working
    // directory is the "current directory" from the settings (the startup
    // project's directory unless overridden).
    const QString os = QSysInfo::prettyProductName();
    const QString cwd = toolsWorkingDirectory().toUserOutput();
    done(Tr::tr("OS: %1\nWorking directory: %2").arg(os, cwd), true);
}

} // namespace LlamaCpp::Tools
