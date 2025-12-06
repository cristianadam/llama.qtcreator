#include "askuser_tool.h"
#include "factory.h"

namespace LlamaCpp {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(AskUserTool{}.name(),
                                            []() { return std::make_unique<AskUserTool>(); });
    return true;
}();
} // namespace

QString AskUserTool::name() const
{
    return QStringLiteral("ask_user");
}

QString AskUserTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "ask_user",
            "description": "Use this tool to ask the user for clarifications if something is unclear.",
            "parameters": {
                "type": "object",
                "properties": {
                    "question": { "type": "string", "description": "The question to the user." }
                },
                "required": [ "question" ],
                "strict": true
            }
        }
    })raw";
}

QString AskUserTool::oneLineSummary(const QJsonObject &args) const
{
    Q_UNUSED(args);
    return QStringLiteral("ask user");
}

void AskUserTool::run(const QJsonObject &arguments,
                      std::function<void(const QString &, bool)> done) const
{
    return done({}, true); // no result, the UI will handle the question.
}
} // namespace LlamaCpp
