#include "list_issues_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <projectexplorer/task.h>
#include <projectexplorer/taskhub.h>

#include <utils/filepath.h>
#include <utils/qtcassert.h>

#include <QSet>
#include <QVector>
#include <algorithm>

using namespace ProjectExplorer;
using namespace Utils;

namespace LlamaCpp {

namespace {

/* -------------------------------------------------------------------------
 *  IssueCollector – a tiny singleton that mirrors the current state of
 *  TaskHub.  Qt Creator does not expose a direct accessor, therefore we keep
 *  our own copy that is kept up‑to‑date via the TaskHub signals.
 * ----------------------------------------------------------------------- */
class IssueCollector : public QObject
{
    Q_OBJECT
public:
    static IssueCollector &instance()
    {
        static IssueCollector theInstance;
        return theInstance;
    }

    const QVector<Task> &tasks() const { return m_tasks; }

private:
    IssueCollector()
    {
        TaskHub *hub = &taskHub();

        connect(hub, &TaskHub::taskAdded, this, [this](const Task &t) { m_tasks.append(t); });

        connect(hub, &TaskHub::taskRemoved, this, [this](const Task &t) {
            const auto it = std::find_if(m_tasks.begin(), m_tasks.end(), [&t](const Task &x) {
                return x.id() == t.id();
            });
            if (it != m_tasks.end())
                m_tasks.erase(it);
        });

        connect(hub, &TaskHub::tasksCleared, this, [this](Utils::Id cat) {
            if (!cat.isValid()) {
                m_tasks.clear();
                return;
            }
            // Remove only tasks belonging to the given category
            m_tasks.erase(std::remove_if(m_tasks.begin(),
                                         m_tasks.end(),
                                         [&cat](const Task &t) { return t.category() == cat; }),
                          m_tasks.end());
        });

        connect(hub, &TaskHub::taskFilePathUpdated, this, [this](const Task &t, const FilePath &) {
            replaceTask(t);
        });
        connect(hub, &TaskHub::taskLineNumberUpdated, this, [this](const Task &t, int) {
            replaceTask(t);
        });
    }

    void replaceTask(const Task &newTask)
    {
        for (int i = 0; i < m_tasks.size(); ++i) {
            if (m_tasks[i].id() == newTask.id()) {
                m_tasks[i] = newTask;
                break;
            }
        }
    }

    QVector<Task> m_tasks;
};

} // anonymous namespace

static const bool registered = [] {
    ToolFactory::instance().registerCreator(ListIssuesTool{}.name(),
                                            []() { return std::make_unique<ListIssuesTool>(); });
    return true;
}();

ListIssuesTool::ListIssuesTool()
{
    const IssueCollector &collector = IssueCollector::instance();
    Q_UNUSED(collector);
}

QString ListIssuesTool::name() const
{
    return QStringLiteral("list_issues");
}

QString ListIssuesTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "list_issues",
            "description": "Return the current list of build issues (errors and warnings) known to Qt Creator. The output format is `file_path:line_number: [TYPE] description`. By default only the first 100 issues are returned; set `provide_all_results` to true to get the complete list.",
            "parameters": {
                "type": "object",
                "properties": {
                    "include_warnings": {
                        "type": "boolean",
                        "description": "Whether warning tasks should be included.",
                        "default": true
                    },
                    "include_errors": {
                        "type": "boolean",
                        "description": "Whether error tasks should be included.",
                        "default": true
                    },
                    "max_results": {
                        "type": "integer",
                        "description": "Maximum number of issues to return when `provide_all_results` is false.",
                        "default": 100,
                        "minimum": 1
                    },
                    "provide_all_results": {
                        "type": "boolean",
                        "description": "If true, return all matching issues regardless of `max_results`.",
                        "default": false
                    }
                },
                "required": [],
                "additionalProperties": false,
                "strict": true
            }
        }
    })raw";
}

QString ListIssuesTool::oneLineSummary(const QJsonObject &args) const
{
    const bool incWarn = args.value("include_warnings").toBool(true);
    const bool incErr = args.value("include_errors").toBool(true);
    const bool all = args.value("provide_all_results").toBool(false);
    const int maxRes = args.value("max_results").toInt(100);

    QStringList parts;
    if (incErr)
        parts << "errors";
    if (incWarn)
        parts << "warnings";
    if (parts.isEmpty())
        parts << "no types";

    QString summary = QStringLiteral("list %1 (%2)").arg(parts.join(QLatin1String(" + ")),
                                                    all ? QStringLiteral("all")
                                                        : QString::number(maxRes));
    return summary;
}

void ListIssuesTool::run(const QJsonObject &args,
                         std::function<void(const QString &, bool)> done) const
{
    const bool includeWarnings = args.value("include_warnings").toBool(true);
    const bool includeErrors = args.value("include_errors").toBool(true);
    const bool provideAll = args.value("provide_all_results").toBool(false);
    const int maxResultsUser = args.value("max_results").toInt(100);
    const int maxResults = provideAll ? std::numeric_limits<int>::max()
                                      : std::max(1, maxResultsUser);

    const QVector<Task> allTasks = IssueCollector::instance().tasks();

    QStringList lines;
    int emitted = 0;

    for (const Task &t : allTasks) {
        if (t.isWarning() && !includeWarnings)
            continue;
        if (t.isError() && !includeErrors)
            continue;
        if (!t.isWarning() && !t.isError())
            continue; // we only expose warnings / errors

        const QString filePath = t.hasFile() ? t.file().toUserOutput()
                                             : QStringLiteral("<no file>");
        const int lineNumber = t.line() > 0 ? t.line() : 0;
        const QString typeStr = t.isError() ? QStringLiteral("Error") : QStringLiteral("Warning");

        lines << QString("%1:%2: [%3] %4")
                     .arg(filePath,
                          QString::number(lineNumber),
                          typeStr,
                          t.description(Task::WithSummary));

        ++emitted;
        if (emitted >= maxResults)
            break;
    }

    if (!provideAll && emitted < allTasks.size()) {
        lines << Tr::tr("[output truncated to first %1 results; set "
                        "`provide_all_results` to true for more]")
                     .arg(maxResults);
    }

    if (lines.isEmpty()) {
        done(Tr::tr("No issues match the requested criteria."), false);
    } else {
        done(lines.join('\n'), true);
    }
}

} // namespace LlamaCpp

#include "list_issues_tool.moc"
