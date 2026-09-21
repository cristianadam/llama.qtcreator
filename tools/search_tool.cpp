#include "search_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "ripgrep.h"
#include "tool_utils.h"

#include <utils/filepath.h>

#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

using namespace Utils;

namespace LlamaCpp::Tools {

namespace {

constexpr int kDefaultLimit = 100;
constexpr int kMaxLimit = 1000;
constexpr int kMaxContext = 20;
constexpr int kMaxLineLength = 2000;
constexpr int kMaxOutputBytes = 50 * 1024; // 50 KB
constexpr int kTimeoutMs = 30 * 1000;

/*! Renders \a absolutePath relative to the search root so the model sees
    short, stable paths. Falls back to the file name when the path lies
    outside the search root or the root itself is a file. */
QString relativizePath(const QString &absolutePath, const QString &searchRoot, bool rootIsDir)
{
    QString path = absolutePath;
    path.replace('\\', '/');
    if (!rootIsDir)
        return QFileInfo(path).fileName();
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

struct SearchState : public QObject
{
    explicit SearchState() = default;

    QProcess *process = nullptr;
    QString searchRoot;
    bool rootIsDir = true;
    int limit = kDefaultLimit;
    int matchCount = 0;
    bool limitReached = false;
    bool linesTruncated = false;
    bool timedOut = false;
    QProcess::ProcessError processError = QProcess::UnknownError;
    QString errorString;
    QString stderrText;
    QStringList outputLines;
    QSet<QString> emittedLines; // "file\x0c<line>" of already printed lines
    QByteArray pending;         // incomplete output line
};

/*! Parses one ripgrep --json event and appends the formatted line to the
    state. Context lines that were already printed (overlapping context
    blocks around nearby matches) are skipped. */
void handleRgEvent(SearchState *state, const QByteArray &line)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return;
    const QJsonObject event = doc.object();
    const QString type = event.value("type").toString();
    if (type != QLatin1String("match") && type != QLatin1String("context"))
        return;
    const QJsonObject data = event.value("data").toObject();
    const QString filePath = data.value("path").toObject().value("text").toString();
    const int lineNumber = data.value("line_number").toInt();
    const QJsonObject lines = data.value("lines").toObject();
    if (!lines.contains("text"))
        return; // match in a binary file; nothing printable to show
    QString text = lines.value("text").toString();
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    text.remove(QLatin1Char('\r'));
    if (text.endsWith(QLatin1Char('\n')))
        text.chop(1);

    if (type == QLatin1String("match"))
        state->matchCount++;

    const QString key = filePath + QLatin1Char('\f') + QString::number(lineNumber);
    if (state->emittedLines.contains(key))
        return;
    state->emittedLines.insert(key);

    if (text.size() > kMaxLineLength) {
        text = text.left(kMaxLineLength) + QStringLiteral("…");
        state->linesTruncated = true;
    }

    const QString rel = relativizePath(filePath, state->searchRoot, state->rootIsDir);
    if (type == QLatin1String("match"))
        state->outputLines << QStringLiteral("%1:%2: %3").arg(rel, QString::number(lineNumber), text);
    else
        state->outputLines << QStringLiteral("%1-%2- %3").arg(rel, QString::number(lineNumber), text);
}

} // namespace

const bool registered = [] {
    ToolFactory::instance().registerCreator(SearchTool{}.name(),
                                            []() { return std::make_unique<SearchTool>(); });
    return true;
}();

QString SearchTool::name() const
{
    return QStringLiteral("search");
}

QString SearchTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "search",
            "description": "Searches file contents for a pattern (regular expression or literal string) using ripgrep. Returns matching lines as 'path:line: text' with paths relative to the searched directory, respecting .gitignore. Hidden files are searched but the .git directory is never searched. Use this instead of bash with grep. Output is limited to the first 100 matches or 50 KB, whichever is hit first; the result then explains how to retrieve more.",
            "parameters": {
                "type": "object",
                "properties": {
                    "pattern": { "type": "string", "description": "Search pattern (regular expression, or literal string when literal is true)." },
                    "path": { "type": "string", "description": "Directory or file to search. Absolute, or relative to the current project directory. Defaults to the project directory." },
                    "glob": { "type": "string", "description": "Only search files matching this glob pattern, e.g. '*.cpp' or '**/*.ts'." },
                    "ignore_case": { "type": "boolean", "description": "Case-insensitive search. Defaults to false." },
                    "literal": { "type": "boolean", "description": "Treat the pattern as a literal string instead of a regular expression. Defaults to false." },
                    "context": { "type": "integer", "description": "Number of lines to show before and after each match (0 to 20). Defaults to 0." },
                    "limit": { "type": "integer", "description": "Maximum number of matches to return (1 to 1000). Defaults to 100." }
                },
                "required": ["pattern"],
                "strict": true
            }
        }
    })raw";
}

QString SearchTool::streamingSummary(const QString &partialArgs) const
{
    const QString pattern = extractPartialString(partialArgs, QStringLiteral("pattern"));
    if (pattern.isEmpty())
        return {};
    const QString code = pattern.contains(QLatin1Char('`'))
            ? QStringLiteral("``%1``").arg(pattern)
            : QStringLiteral("`%1`").arg(pattern);
    return Tr::tr("search for %1").arg(code);
}

QString SearchTool::oneLineSummary(const QJsonObject &args) const
{
    const QString pattern = args.value("pattern").toString();
    if (pattern.isEmpty())
        return {};
    const QString code = pattern.contains(QLatin1Char('`'))
            ? QStringLiteral("``%1``").arg(pattern)
            : QStringLiteral("`%1`").arg(pattern);
    return Tr::tr("search for %1").arg(code);
}

QString SearchTool::detailsMarkdown(const QJsonObject &arguments, const QString &result) const
{
    const QString pattern = arguments.value("pattern").toString();
    const QString glob = arguments.value("glob").toString();
    QString md = Tr::tr("Pattern: `%1`").arg(pattern);
    if (!glob.isEmpty())
        md += Tr::tr("  Glob: `%1`").arg(glob);
    const QString path = arguments.value("path").toString();
    if (!path.isEmpty())
        md += Tr::tr("  Path: %1").arg(path);
    md += QStringLiteral("\n\n```\n") + result + QStringLiteral("\n```");
    return md;
}

void SearchTool::run(const QJsonObject &args,
                     std::function<void(const QString &, bool)> done) const
{
    const FilePath rgPath = Ripgrep::resolvedPath();
    if (rgPath.isEmpty()) {
        return done(Tr::tr("Error: ripgrep (rg) was not found. It can be downloaded from the "
                           "Llama.cpp Chat tools settings page, or installed manually from "
                           "https://github.com/BurntSushi/ripgrep, to use the search tool."),
                    false);
    }

    const QString pattern = args.value("pattern").toString();
    if (pattern.isEmpty()) {
        return done(Tr::tr("Error: the pattern must not be empty."), false);
    }

    const FilePath root = absoluteProjectPath(FilePath::fromUserInput(args.value("path").toString()));
    const QString rootString = root.toUserOutput();
    if (!root.exists()) {
        return done(Tr::tr("Error: path does not exist: %1").arg(rootString), false);
    }
    const bool rootIsDir = QFileInfo(rootString).isDir();

    const int limit = qBound(1, args.value("limit").toInt(kDefaultLimit), kMaxLimit);
    const int context = qBound(0, args.value("context").toInt(0), kMaxContext);

    const QStringList rgArgs = [&] {
        QStringList a;
        a << QStringLiteral("--json")
          << QStringLiteral("--line-number")
          << QStringLiteral("--color=never")
          << QStringLiteral("--hidden");
        if (args.value("ignore_case").toBool())
            a << QStringLiteral("--ignore-case");
        if (args.value("literal").toBool())
            a << QStringLiteral("--fixed-strings");
        const QString glob = args.value("glob").toString();
        if (!glob.isEmpty())
            a << QStringLiteral("--glob") << glob;
        // Hidden files are searched (e.g. .github workflows), but never the
        // .git directory itself. The negation must come last: ripgrep applies
        // the last matching glob, so a broad user glob would otherwise
        // re-include .git.
        a << QStringLiteral("--glob") << QStringLiteral("!.git");
        if (context > 0)
            a << QStringLiteral("--context") << QString::number(context);
        a << QStringLiteral("--") << pattern << rootString;
        return a;
    }();

    auto *state = new SearchState;
    state->searchRoot = rootString;
    state->rootIsDir = rootIsDir;
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
                             handleRgEvent(state, line);
                             // Collect no more events once the limit is reached.
                             if (state->matchCount >= state->limit)
                                 break;
                         }
                         // Stop the search as soon as the limit is reached.
                         if (state->matchCount >= state->limit
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

            QString matches = state->outputLines.join(QLatin1Char('\n'));
            QStringList notes;
            bool ok = true;

            if (state->timedOut && !state->limitReached) {
                ok = false;
                notes << Tr::tr("[Search timed out after %1 ms. The results are "
                                "incomplete; narrow the path or refine the pattern.]")
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
                notes << Tr::tr("[%1 matches limit reached. Use limit=%2 for more results, "
                                "or refine the pattern.]")
                           .arg(state->limit)
                           .arg(state->limit * 2);
            if (state->linesTruncated)
                notes << Tr::tr("[Some lines truncated to %1 characters. Use read_file to "
                                "see full lines.]")
                           .arg(kMaxLineLength);

            QString result;
            if (matches.isEmpty()) {
                result = Tr::tr("No matches found.");
            } else {
                result = matches;
                QByteArray utf8 = matches.toUtf8();
                if (utf8.size() > kMaxOutputBytes) {
                    int pos = kMaxOutputBytes;
                    const int nl = utf8.lastIndexOf('\n', pos);
                    if (nl != -1)
                        pos = nl;
                    result = QString::fromUtf8(utf8.left(pos));
                    notes << Tr::tr("[Output truncated to %1 KB. Refine the pattern or "
                                    "reduce the context to see all matches.]")
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
