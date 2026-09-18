#include "task_tool.h"

#include "factory.h"
#include "llamachatmanager.h"
#include "llamathinkingsectionparser.h"
#include "llamatr.h"

#include <QMetaObject>
#include <memory>

namespace LlamaCpp::Tools {

namespace {

const char kSubagentExplore[] = "explore";
const char kSubagentGeneral[] = "general";

// Tools the "explore" subagent may use (read‑only, non‑interactive).
// "project_find_files" is provided by the Qt Creator MCP server; it is only
// picked up when the server is actually connected.
const QStringList kExploreTools = {
    "read_file",
    "list_directory",
    "project_find_files",
    "webfetch",
    "websearch",
};

} // namespace

QString taskSystemPromptFor(const QString &type)
{
    if (type == QLatin1String(kSubagentExplore))
        return Tr::tr("You are the \"explore\" subagent, running in a separate conversation "
                      "that is isolated from the main one. Explore the codebase and answer "
                      "the task with the read‑only tools available to you. Do not modify "
                      "any file and do not run commands that change anything. Work "
                      "autonomously; you cannot ask the user questions. When you are done, "
                      "reply with a single concise report containing the key findings, "
                      "relevant file paths with line numbers, and everything the main "
                      "conversation needs to continue the work.");
    return Tr::tr("You are the \"general\" subagent, running in a separate conversation "
                  "that is isolated from the main one. Work autonomously with the available "
                  "tools until the task is complete; you cannot ask the user questions. "
                  "When you are done, reply with a single concise report of what you did "
                  "or found, including relevant file paths with line numbers, so the main "
                  "conversation can continue the work.");
}

QStringList taskToolsFor(const QString &type, const QStringList &availableTools)
{
    if (type == QLatin1String(kSubagentExplore)) {
        QStringList res;
        for (const QString &tool : kExploreTools)
            if (availableTools.contains(tool))
                res << tool;
        return res;
    }

    // "general": every tool that exists, except the ones that must never run
    // inside a sub‑conversation (no task recursion, no user dialog).
    QStringList res;
    for (const QString &tool : availableTools)
        if (tool != QLatin1String("task") && tool != QLatin1String("ask_user"))
            res << tool;
    return res;
}

// The stored assistant content may carry a thinking section in front of the
// actual answer; only the text after it is the subagent's report.
QString taskFinalReport(const QString &content)
{
    return LlamaCpp::ThinkingSectionParser::parseThinkingSection(content).second.trimmed();
}

const bool registered = [] {
    ToolFactory::instance().registerCreator(TaskTool{}.name(),
                                            []() { return std::make_unique<TaskTool>(); });
    return true;
}();

QString TaskTool::name() const
{
    return QStringLiteral("task");
}

QString TaskTool::streamingSummary(const QString &partialArgs) const
{
    // "description" is the first field, so a usable summary appears almost
    // immediately while the arguments are still streaming in.
    int idx = partialArgs.indexOf(QStringLiteral("\"description\""));
    if (idx == -1)
        return {};
    idx = partialArgs.indexOf(QLatin1Char(':'), idx);
    if (idx == -1)
        return {};
    const int start = partialArgs.indexOf(QLatin1Char('"'), idx + 1);
    if (start == -1)
        return {};
    int end = start + 1;
    while (end < partialArgs.size()) {
        const QChar c = partialArgs.at(end);
        if (c == QLatin1Char('"') || c == QLatin1Char('\\'))
            break;
        ++end;
    }
    const QString description = partialArgs.mid(start + 1, end - start - 1);
    if (description.isEmpty())
        return {};
    return Tr::tr("task: %1").arg(description);
}

QString TaskTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "task",
            "description": "Launches a new agent that handles a self-contained subtask in a SEPARATE conversation, isolated from the current one. The subagent cannot see this conversation, so the prompt must be fully self-contained (include all relevant context). While it works, the current conversation is paused; when the subagent finishes, its final report is returned here as the tool result. The sub-conversation stays in the conversation list, so its full working history (file reads, searches, edits) can be inspected or continued later. Use this for exploratory or long-running work that would clutter the current conversation, e.g. exploring an unfamiliar codebase.",
            "parameters": {
                "type": "object",
                "properties": {
                    "description": { "type": "string", "description": "A short (3-5 word) summary of the subtask. Used as the title of the sub-conversation, e.g. \"Explore build system\"." },
                    "prompt": { "type": "string", "description": "The full, self-contained task for the subagent. The subagent cannot see this conversation, so include every relevant detail." },
                    "subagent_type": { "type": "string", "enum": ["explore", "general"], "description": "\"explore\": read-only subagent for exploring code and answering questions (default). \"general\": full tool access for tasks that need to modify files or run commands." }
                },
                "required": ["description", "prompt"],
                "strict": true
            }
        }
    })raw";
}

QString TaskTool::oneLineSummary(const QJsonObject &arguments) const
{
    return Tr::tr("task: %1").arg(arguments.value("description").toString());
}

QString TaskTool::detailsMarkdown(const QJsonObject &arguments, const QString &result) const
{
    QString md;
    const QString description = arguments.value("description").toString();
    if (!description.isEmpty())
        md += QStringLiteral("### %1\n\n").arg(description);

    const QString prompt = arguments.value("prompt").toString();
    if (!prompt.isEmpty())
        md += QStringLiteral("```\n%1\n```\n\n").arg(prompt);

    if (!result.isEmpty())
        md += QStringLiteral("### Result\n\n%1").arg(result);
    return md;
}

void TaskTool::run(const QJsonObject &args,
                   std::function<void(const QString &, bool)> done) const
{
    const QString description = args.value("description").toString().trimmed();
    const QString prompt = args.value("prompt").toString().trimmed();
    if (description.isEmpty() || prompt.isEmpty())
        return done(Tr::tr("Tool error: both \"description\" and \"prompt\" are required."),
                    false);

    QString type = args.value("subagent_type").toString().trimmed();
    if (type.isEmpty())
        type = QLatin1String(kSubagentExplore);
    if (type != QLatin1String(kSubagentExplore) && type != QLatin1String(kSubagentGeneral))
        return done(
            Tr::tr("Tool error: unknown subagent_type \"%1\" (expected \"explore\" or \"general\")")
                .arg(type),
            false);

    auto &chat = ChatManager::instance();
    const QString convName = Tr::tr("Task: %1").arg(description);
    const Conversation conv = chat.createTaskConversation(convName);
    const QStringList availableTools = ToolFactory::instance().creatorsList();
    chat.configureTaskConversation(conv.id,
                                   taskSystemPromptFor(type),
                                   taskToolsFor(type, availableTools));

    // The tool finishes when the sub‑conversation reaches its final answer
    // (or is stopped / deleted, which also emits the signal with ok=false).
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = QObject::connect(&chat,
                                   &ChatManager::taskConversationFinished,
                                   [connection, convId = conv.id, convName, done](
                                           const QString &id, const QString &content, bool ok) {
                                       if (id != convId)
                                           return;
                                       QObject::disconnect(*connection);
                                       const QString report = taskFinalReport(content);
                                       if (!ok || report.isEmpty())
                                           return done(
                                               Tr::tr("Task failed: the sub‑conversation \"%1\" ended "
                                                      "without a final answer (it may have been "
                                                      "stopped or deleted).")
                                                   .arg(convName),
                                               false);
                                       return done(
                                           Tr::tr("Task completed in the sub‑conversation \"%1\" "
                                                  "(id: %2). The conversation is kept in the "
                                                  "conversation list and can be opened for "
                                                  "further work. Final report from the subagent:\n\n%3")
                                               .arg(convName, convId, report),
                                           true);
                                   });

    chat.sendMessage(conv.id, conv.currNode, prompt, {}, [](qint64) {});
}

} // namespace LlamaCpp::Tools
