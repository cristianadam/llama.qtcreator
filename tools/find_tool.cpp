#include "find_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "ripgrep.h"
#include "tool_utils.h"

#include <utils/filepath.h>

#include <QFileInfo>
#include <QProcess>
#include <QTimer>

using namespace Utils;

namespace LlamaCpp::Tools {

namespace {

constexpr int kDefaultLimit = 1000;
constexpr int kMaxLimit = 5000;
constexpr int kMaxOutputBytes = 50 * 1024; // 50 KB
constexpr int kTimeoutMs = 30 * 1000;

/*! Renders \a absolutePath relative to the search root so the model sees
    short, stable paths. Falls back to the full path when it lies outside
    the search root. */
QString relativizePath(const QString &absolutePath, const QString &searchRoot)
{
    QString path = absolutePath;
    path.replace('\\', '/');
    QString prefix = searchRoot;
    prefix.replace('\\', '/');
    if (!prefix.endsWith('/'))
        prefix += '/';
    if (path.startsWith(prefix))
        return path.mid(prefix.size());
    return path;
}

/*! Extracts a (possibly incomplete) string value for \a key from partially
    streamed tool argument JSON, mirroring what the task tool does for its
    description. Returns an empty string while the value is not visible yet. */
QString extractPartialString(const QString &partialArgs, const QString &key)
{
    int idx = partialArgs.indexOf(QStringLiteral("\"%1\"").arg(key));
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
        if (c == QLatin1Char('\\'))
            end += 2;
        else if (c == QLatin1Char('"'))
            break;
        else
            ++end;
    }
    if (end >= partialArgs.size())
        return {}; // closing quote not streamed yet
    return partialArgs.mid(start + 1, end - start - 1);
}

struct FindState : public QObject
{
    explicit FindState() = default;

    QProcess *process = nullptr;
    QString searchRoot;
    int limit = kDefaultLimit;
    int resultCount = 0;
    bool limitReached = false;
    bool timedOut = false;
    QProcess::ProcessError processError = QProcess::UnknownError;
    QString errorString;
    QString stderrText;
    QStringList outputLines;
    QByteArray pending; // incomplete output line
};

} // namespace

const bool registered = [] {
    ToolFactory::instance().registerCreator(FindTool{}.name(),
                                            []() { return std::make_unique<FindTool>(); });
    return true;
}();

QString FindTool::name() const
{
    return QStringLiteral("find");
}

QString FindTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "find",
            "description": "Finds files by glob pattern using ripgrep. Returns matching file paths relative to the searched directory, one per line, respecting .gitignore. Hidden files are listed but the .git directory is never listed. Use this instead of bash with find. Output is limited to the first 1000 results or 50 KB, whichever is hit first; the result then explains how to retrieve more.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pattern": { "type": "string", "description": "Glob pattern to match files against, e.g. '*.cpp', '**/*.json', or 'src/**/*test*'. Patterns match against the path relative to the searched directory." },
                    "path": { "type": "string", "description": "Directory to search. Absolute, or relative to the current project directory. Defaults to the project directory." },
                    "limit": { "type": "integer", "description": "Maximum number of results to return (1 to 5000). Defaults to 1000." }
                },
                "required": ["pattern"],
                "strict": true
            }
        }
    })raw";
}

QString FindTool::streamingSummary(const QString &partialArgs) const
{
    const QString pattern = extractPartialString(partialArgs, QStringLiteral("pattern"));
    if (pattern.isEmpty())
        return {};
    const QString code = pattern.contains(QLatin1Char('`'))
            ? QStringLiteral("``%1``").arg(pattern)
            : QStringLiteral("`%1`").arg(pattern);
    return Tr::tr("find files %1").arg(code);
}

QString FindTool::oneLineSummary(const QJsonObject &args) const
{
    const QString pattern = args.value("pattern").toString();
    if (pattern.isEmpty())
        return {};
    const QString code = pattern.contains(QLatin1Char('`'))
            ? QStringLiteral("``%1``").arg(pattern)
            : QStringLiteral("`%1`").arg(pattern);
    return Tr::tr("find files %1").arg(code);
}

QString FindTool::detailsMarkdown(const QJsonObject &arguments, const QString &result, bool ok) const
{
    Q_UNUSED(ok);
    const QString pattern = arguments.value("pattern").toString();
    const QString path = arguments.value("path").toString();
    QString md = Tr::tr("Pattern: `%1`").arg(pattern);
    if (!path.isEmpty())
        md += Tr::tr("  Path: %1").arg(path);
    md += QStringLiteral("\n\n") + codeFence(result);
    return md;
}

void FindTool::run(const QJsonObject &args,
                   std::function<void(const QString &, bool)> done) const
{
    const FilePath rgPath = Ripgrep::resolvedPath();
    if (rgPath.isEmpty()) {
        return done(Tr::tr("Error: ripgrep (rg) was not found. It can be downloaded from the "
                           "Llama.cpp Chat tools settings page, or installed manually from "
                           "https://github.com/BurntSushi/ripgrep, to use the find tool."),
                    false);
    }

    const QString pattern = args.value("pattern").toString();
    if (pattern.isEmpty()) {
        return done(Tr::tr("Error: the pattern must not be empty."), false);
    }

    const FilePath root = absoluteProjectPath(FilePath::fromUserInput(args.value("path").toString()));
    const QString rootString = root.toUserOutput();
    const QFileInfo rootInfo(rootString);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        return done(Tr::tr("Error: path is not a directory: %1").arg(rootString), false);
    }

    const int limit = qBound(1, args.value("limit").toInt(kDefaultLimit), kMaxLimit);

    const QStringList rgArgs{QStringLiteral("--files"),
                             QStringLiteral("--color=never"),
                             // Same hidden-file policy as the search tool: dotfiles
                             // are listed ...
                             QStringLiteral("--hidden"),
                             QStringLiteral("--glob"),
                             pattern,
                             // ... but never the .git directory itself. The negation
                             // must come last: ripgrep applies the last matching
                             // glob, so a broad pattern like "*" would otherwise
                             // re-include .git.
                             QStringLiteral("--glob"),
                             QStringLiteral("!.git"),
                             QStringLiteral("--"),
                             rootString};

    auto *state = new FindState;
    state->searchRoot = rootString;
    state->limit = limit;

    QProcess *process = new QProcess(state);
    state->process = process;
    process->setProcessChannelMode(QProcess::SeparateChannels);

    QTimer *timer = new QTimer(state);
    timer->setSingleShot(true);
    QObject::connect(timer,
                     &QTimer::timeout,
                     state,
                     [state] {
                         state->timedOut = true;
                         state->process->kill();
                     });

    QObject::connect(process,
                     &QProcess::readyReadStandardOutput,
                     state,
                     [state] {
                         state->pending += state->process->readAllStandardOutput();
                         int idx;
                         while ((idx = state->pending.indexOf('\n')) != -1) {
                             const QByteArray line = state->pending.left(idx).trimmed();
                             state->pending.remove(0, idx + 1);
                             if (line.isEmpty())
                                 continue;
                             state->outputLines
                                 << relativizePath(QString::fromUtf8(line), state->searchRoot);
                             state->resultCount++;
                             // Collect no more results once the limit is reached.
                             if (state->resultCount >= state->limit)
                                 break;
                         }
                         // Stop the search as soon as the limit is reached.
                         if (state->resultCount >= state->limit
                             && state->process->state() != QProcess::NotRunning) {
                             state->limitReached = true;
                             state->process->kill();
                         }
                     });

    QObject::connect(process,
                     &QProcess::readyReadStandardError,
                     state,
                     [state] { state->stderrText += QString::fromUtf8(state->process->readAllStandardError()); });

    QObject::connect(process,
                     &QProcess::errorOccurred,
                     state,
                     [state](QProcess::ProcessError error) {
                         state->processError = error;
                         state->errorString = state->process->errorString();
                     });

    QObject::connect(
        process,
        qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
        state,
        [state, done = std::move(done)](int exitCode, QProcess::ExitStatus) mutable {
            state->stderrText += QString::fromUtf8(state->process->readAllStandardError());

            QString results = state->outputLines.join(QLatin1Char('\n'));
            QStringList notes;
            bool ok = true;

            if (state->timedOut && !state->limitReached) {
                ok = false;
                notes << Tr::tr("[Search timed out after %1 ms. The results are "
                                "incomplete; use a more specific pattern.]")
                             .arg(kTimeoutMs);
            } else if (state->processError == QProcess::FailedToStart) {
                ok = false;
                notes << Tr::tr("[Failed to start ripgrep: %1]")
                             .arg(state->errorString.isEmpty()
                                      ? Tr::tr("unknown error")
                                      : state->errorString);
            } else if (exitCode > 1 && !state->limitReached) {
                ok = false;
                notes << Tr::tr("[ripgrep failed (exit code %1): %2]")
                             .arg(exitCode)
                             .arg(state->stderrText.trimmed().isEmpty()
                                      ? Tr::tr("unknown error")
                                      : state->stderrText.trimmed());
            }

            if (state->limitReached)
                notes << Tr::tr("[%1 results limit reached. Use limit=%2 for more results, "
                                "or use a more specific pattern.]")
                           .arg(state->limit)
                           .arg(state->limit * 2);

            QString result;
            if (results.isEmpty()) {
                result = Tr::tr("No files found.");
            } else {
                result = results;
                QByteArray utf8 = results.toUtf8();
                if (utf8.size() > kMaxOutputBytes) {
                    int pos = kMaxOutputBytes;
                    const int nl = utf8.lastIndexOf('\n', pos);
                    if (nl != -1)
                        pos = nl;
                    result = QString::fromUtf8(utf8.left(pos));
                    notes << Tr::tr("[Output truncated to %1 KB. Use a more specific "
                                    "pattern to see all results.]")
                               .arg(kMaxOutputBytes / 1024);
                }
            }

            if (!notes.isEmpty())
                result += QLatin1Char('\n') + QLatin1Char('\n') + notes.join(QLatin1Char('\n'));

            state->deleteLater();
            done(result, ok);
        });

    timer->start(kTimeoutMs);
    process->start(rgPath.toUserOutput(), rgArgs);
}

} // namespace LlamaCpp::Tools
