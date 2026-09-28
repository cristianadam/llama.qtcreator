#include "ls_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "tool_utils.h"

#include <utils/filepath.h>

#include <QDir>
#include <QFileInfo>

#include <algorithm>

using namespace Utils;

namespace LlamaCpp::Tools {

namespace {

constexpr int kDefaultLimit = 500;
constexpr int kMaxLimit = 5000;
constexpr int kMaxOutputBytes = 50 * 1024; // 50 KB

/*! Extracts a (possibly incomplete) string value for \a key from partially
    streamed tool argument JSON, mirroring what the find tool does for its
    pattern. Returns an empty string while the value is not visible yet. */
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

} // namespace

const bool registered = [] {
    ToolFactory::instance().registerCreator(LsTool{}.name(),
                                            []() { return std::make_unique<LsTool>(); });
    return true;
}();

QString LsTool::name() const
{
    return QStringLiteral("ls");
}

QString LsTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "ls",
            "description": "Lists the contents of a directory. Returns entry names sorted alphabetically (case-insensitive), one per line, with a '/' suffix for directories. Dotfiles are included. Use this to get an overview of a directory before reading or searching in it. Output is limited to the first 500 entries or 50 KB, whichever is hit first; the result then explains how to retrieve more.",
            "parameters": {
                "type": "object",
                "properties": {
                    "path": { "type": "string", "description": "Directory to list. Absolute, or relative to the current project directory. Defaults to the project directory." },
                    "limit": { "type": "integer", "description": "Maximum number of entries to return (1 to 5000). Defaults to 500." }
                },
                "required": [],
                "strict": true
            }
        }
    })raw";
}

QString LsTool::streamingSummary(const QString &partialArgs) const
{
    const QString path = extractPartialString(partialArgs, QStringLiteral("path"));
    return Tr::tr("list directory %1").arg(path.isEmpty() ? QStringLiteral(".") : path);
}

QString LsTool::oneLineSummary(const QJsonObject &args) const
{
    const QString path = args.value("path").toString();
    return Tr::tr("list directory %1").arg(path.isEmpty() ? QStringLiteral(".") : path);
}

QString LsTool::detailsMarkdown(const QJsonObject &arguments, const QString &result, bool ok) const
{
    Q_UNUSED(ok);
    const QString path = arguments.value("path").toString();
    QString md;
    if (!path.isEmpty())
        md = Tr::tr("Path: `%1`\n\n").arg(path);
    md += codeFence(result);
    return md;
}

void LsTool::run(const QJsonObject &args,
                 std::function<void(const QString &, bool)> done) const
{
    const FilePath dir = absoluteProjectPath(FilePath::fromUserInput(args.value("path").toString()));
    if (const QString error = sandboxAccessError(dir, /*isWrite=*/false); !error.isEmpty())
        return done(error, false);
    const QString dirString = dir.toUserOutput();
    const QFileInfo dirInfo(dirString);
    if (!dirInfo.exists() || !dirInfo.isDir()) {
        return done(Tr::tr("Error: path is not a directory: %1").arg(dirString), false);
    }

    const int limit = qBound(1, args.value("limit").toInt(kDefaultLimit), kMaxLimit);

    const QStringList entries = QDir(dirString).entryList(
        QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);

    // Case-insensitive alphabetical order, like pi's ls tool.
    QStringList sorted = entries;
    std::sort(sorted.begin(),
              sorted.end(),
              [](const QString &a, const QString &b) {
                  return a.compare(b, Qt::CaseInsensitive) < 0;
              });

    QStringList lines;
    bool limitReached = false;
    for (const QString &entry : sorted) {
        if (lines.size() >= limit) {
            limitReached = true;
            break;
        }
        const QFileInfo info(dirString, entry);
        lines << (info.isDir() ? entry + QStringLiteral("/") : entry);
    }

    QString result;
    QStringList notes;
    if (lines.isEmpty()) {
        result = Tr::tr("Directory is empty.");
    } else {
        result = lines.join(QLatin1Char('\n'));
        QByteArray utf8 = result.toUtf8();
        if (utf8.size() > kMaxOutputBytes) {
            int pos = kMaxOutputBytes;
            const int nl = utf8.lastIndexOf('\n', pos);
            if (nl != -1)
                pos = nl;
            QByteArray cut = utf8.left(pos);
            // Never split a multi‑byte UTF‑8 sequence (a single entry name
            // can exceed the whole cap): drop an incomplete trailing code
            // point – its continuation bytes plus the lead byte.
            int i = cut.size() - 1;
            if (i >= 0 && (static_cast<unsigned char>(cut.at(i)) & 0xC0) == 0x80) {
                while (i >= 0 && (static_cast<unsigned char>(cut.at(i)) & 0xC0) == 0x80)
                    --i;
                cut.truncate(i + 1);
            }
            result = QString::fromUtf8(cut);
            notes << Tr::tr("[Output truncated to %1 KB. List a subdirectory to see "
                            "the remaining entries.]")
                         .arg(kMaxOutputBytes / 1024);
        }
    }

    if (limitReached)
        notes << Tr::tr("[%1 entries limit reached. Use limit=%2 for more entries.]")
                     .arg(limit)
                     .arg(qMin(limit * 2, kMaxLimit));

    if (!notes.isEmpty())
        result += QLatin1Char('\n') + QLatin1Char('\n') + notes.join(QLatin1Char('\n'));

    done(result, true);
}

} // namespace LlamaCpp::Tools
