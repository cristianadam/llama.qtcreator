#include "todowrite_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace LlamaCpp::Tools {

namespace {

const bool registered = [] {
    ToolFactory::instance().registerCreator(TodoWriteTool{}.name(),
                                            []() { return std::make_unique<TodoWriteTool>(); });
    return true;
}();

constexpr const char *kPending = "pending";
constexpr const char *kInProgress = "in_progress";
constexpr const char *kCompleted = "completed";

bool isKnownStatus(const QString &status)
{
    return status == QLatin1String(kPending)
        || status == QLatin1String(kInProgress) || status == QLatin1String(kCompleted);
}

} // namespace

QString TodoWriteTool::name() const
{
    return QStringLiteral("todo_write");
}

QString TodoWriteTool::toolDefinition() const
{
    const QString description = R"desc(
Create or update the task list used to track progress on multi-step work.

- Always send the complete list of tasks; each call replaces the previous list.
- Use it proactively for non-trivial tasks (3 or more steps): plan the work first, then update the list as you go.
- Exactly one task may be in_progress at a time; set it to completed as soon as it is done.
- Task content is a short imperative phrase, e.g. "Add write tool to the tool factory".
)desc";

    QJsonObject contentProperty;
    contentProperty[QStringLiteral("type")] = QStringLiteral("string");
    contentProperty[QStringLiteral("description")] =
        QStringLiteral("Short description of the task.");

    QJsonObject statusProperty;
    statusProperty[QStringLiteral("type")] = QStringLiteral("string");
    statusProperty[QStringLiteral("enum")] = QJsonArray{QLatin1String(kPending),
                                                        QLatin1String(kInProgress),
                                                        QLatin1String(kCompleted)};
    statusProperty[QStringLiteral("description")] =
        QStringLiteral("Current state of the task: \"pending\", \"in_progress\" or "
                       "\"completed\".");

    QJsonObject todoProperties;
    todoProperties[QStringLiteral("content")] = contentProperty;
    todoProperties[QStringLiteral("status")] = statusProperty;

    QJsonObject todoItem;
    todoItem[QStringLiteral("type")] = QStringLiteral("object");
    todoItem[QStringLiteral("properties")] = todoProperties;
    todoItem[QStringLiteral("required")] = QJsonArray{QStringLiteral("content"),
                                                       QStringLiteral("status")};
    todoItem[QStringLiteral("additionalProperties")] = false;

    QJsonObject todosProperty;
    todosProperty[QStringLiteral("type")] = QStringLiteral("array");
    todosProperty[QStringLiteral("items")] = todoItem;
    todosProperty[QStringLiteral("minItems")] = 1;
    todosProperty[QStringLiteral("description")] =
        QStringLiteral("The complete, ordered list of tasks. Replaces the previous list.");

    QJsonObject properties;
    properties[QStringLiteral("todos")] = todosProperty;

    QJsonObject parameters;
    parameters[QStringLiteral("type")] = QStringLiteral("object");
    parameters[QStringLiteral("properties")] = properties;
    parameters[QStringLiteral("required")] = QJsonArray{QStringLiteral("todos")};
    parameters[QStringLiteral("additionalProperties")] = false;

    QJsonObject function;
    function[QStringLiteral("name")] = QStringLiteral("todo_write");
    function[QStringLiteral("description")] = description.trimmed();
    function[QStringLiteral("parameters")] = parameters;

    QJsonObject root;
    root[QStringLiteral("type")] = QStringLiteral("function");
    root[QStringLiteral("function")] = function;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString TodoWriteTool::oneLineSummary(const QJsonObject &args) const
{
    const int count = args.value("todos").toArray().size();
    if (count <= 1)
        return Tr::tr("update task list");
    return Tr::tr("update task list (%1 tasks)").arg(count);
}

QString TodoWriteTool::streamingSummary(const QString &partialArguments) const
{
    Q_UNUSED(partialArguments);
    // The list itself is what streams in; a constant summary is clearer.
    return Tr::tr("update task list");
}

QString TodoWriteTool::detailsMarkdown(const QJsonObject &args, const QString &result, bool ok) const
{
    // On failure show the error, not the rejected list.
    if (!ok)
        return result;
    QString md;
    for (const QJsonValue &value : args.value("todos").toArray()) {
        const QJsonObject todo = value.toObject();
        const QString status = todo.value("status").toString();
        const QString content = todo.value("content").toString();
        if (status == QLatin1String(kCompleted))
            md += QStringLiteral("- [x] %1\n").arg(content);
        else if (status == QLatin1String(kInProgress))
            md += QStringLiteral("- [~] %1\n").arg(content);
        else
            md += QStringLiteral("- [ ] %1\n").arg(content);
    }
    return md.trimmed();
}

void TodoWriteTool::run(const QJsonObject &args,
                        std::function<void(const QString &, bool)> done) const
{
    const QJsonArray todos = args.value("todos").toArray();
    if (todos.isEmpty())
        return done(Tr::tr("Tool error: \"todos\" must contain at least one task."), false);

    int inProgress = 0;
    int completed = 0;
    for (int i = 0; i < todos.size(); ++i) {
        const QJsonObject todo = todos.at(i).toObject();
        const QString content = todo.value("content").toString().trimmed();
        const QString status = todo.value("status").toString();
        if (content.isEmpty()) {
            return done(Tr::tr("Tool error: task %1 has an empty \"content\".").arg(i + 1),
                        false);
        }
        if (!isKnownStatus(status)) {
            return done(Tr::tr("Tool error: task %1 has unknown status \"%2\" (expected "
                               "\"pending\", \"in_progress\" or \"completed\").")
                            .arg(i + 1)
                            .arg(status),
                        false);
        }
        if (status == QLatin1String(kInProgress))
            ++inProgress;
        if (status == QLatin1String(kCompleted))
            ++completed;
    }
    if (inProgress > 1)
        return done(Tr::tr("Tool error: only one task may be \"in_progress\" at a time "
                           "(%1 given).")
                        .arg(inProgress),
                    false);

    const QString summary =
        Tr::tr("Task list updated: %1 of %2 completed.").arg(completed).arg(todos.size());
    return done(summary, true);
}

} // namespace LlamaCpp::Tools
