#include "readfile_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "tool_utils.h"

#include <utils/filepath.h>

#include <cstring>

using namespace Utils;

namespace LlamaCpp::Tools {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(ReadFileTool{}.name(),
                                            []() { return std::make_unique<ReadFileTool>(); });
    return true;
}();

//! Magic‑byte image detection, the same formats pi's read tool supports.
//! Returns the MIME type, or an empty string for non‑images.
QString imageMimeType(const QByteArray &data)
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(data.constData());
    if (data.size() >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF)
        return QStringLiteral("image/jpeg");
    if (data.size() >= 8 && memcmp(p, "\x89PNG\r\n\x1a\n", 8) == 0)
        return QStringLiteral("image/png");
    if (data.size() >= 6
        && (memcmp(p, "GIF87a", 6) == 0 || memcmp(p, "GIF89a", 6) == 0))
        return QStringLiteral("image/gif");
    if (data.size() >= 12 && memcmp(p, "RIFF", 4) == 0 && memcmp(p + 8, "WEBP", 4) == 0)
        return QStringLiteral("image/webp");
    if (data.size() >= 2 && data[0] == 'B' && data[1] == 'M')
        return QStringLiteral("image/bmp");
    return {};
}

// A base64‑encoded image in the prompt is pure token cost; beyond this
// size a vision model cannot use it meaningfully anyway.
constexpr int kMaxImageBytes = 10 * 1024 * 1024; // 10 MB
} // namespace

QString ReadFileTool::name() const
{
    return QStringLiteral("read_file");
}

QString ReadFileTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "read_file",
            "description": "Read the contents of a file from first_line to last_line_inclusive (at most 250 lines per call), or most of the file when should_read_entire_file is true. Output is capped (2000 lines, 50 KB); when cut short, the result ends with a hint for the next first_line - follow it. Image files (jpeg, png, gif, webp, bmp) are returned as an image attachment; the line arguments are ignored for images.",
            "parameters": {
                "type": "object",
                "properties": {
                    "first_line": { "type": "integer", "description": "First line to read (1-based)." },
                    "last_line_inclusive": { "type": "integer", "description": "Last line to read (1-based)." },
                    "should_read_entire_file": { "type": "boolean", "description": "Read the entire file (subject to the size caps). Defaults to false." },
                    "file_path": { "type": "string", "description": "Path of the file to read, absolute or relative to the project directory." }
                },
                "required": [ "first_line", "last_line_inclusive", "file_path" ],
                "strict": true
            }
        }
    })raw";
}

QString ReadFileTool::oneLineSummary(const QJsonObject &args) const
{
    const QString file = args.value("file_path").toString();
    const bool all = args.value("should_read_entire_file").toBool(false);
    QString target = file;
    if (!all) {
        int first = args.value("first_line").toInt(1);
        int last = args.value("last_line_inclusive").toInt(first);
        target = QStringLiteral("%1:%2\u2011%3").arg(file).arg(first).arg(last);
    }
    return all ? Tr::tr("read whole file %1").arg(codeSpan(target))
               : Tr::tr("read %1").arg(codeSpan(target));
}

QString ReadFileTool::detailsMarkdown(const QJsonObject &args, const QString &result, bool ok) const
{
    // Failures show the raw error; the ✗ icon in the summary already marks
    // the call as failed.
    if (!ok || result.isEmpty())
        return result;

    const QString path = args.value("file_path").toString();
    // The truncation hint ("[Showing lines …]") is appended by run() and is
    // not part of the file; keep it outside the code fence so it is not
    // highlighted as file content.
    const int hintPos = result.lastIndexOf(QStringLiteral("\n\n[Showing lines "));
    const QString content = hintPos == -1 ? result : result.left(hintPos);
    QString md = codeFence(content, codeLanguageFor(path));
    if (hintPos != -1)
        // codeFence() ends with the closing fence and no trailing newline;
        // without this the hint is glued to it, which is not a valid closing
        // fence and would leave the code block open.
        md += QLatin1Char('\n') + result.mid(hintPos + 2);
    return md;
}

// Whole‑file reads are capped so a huge file (minified JS, generated
// data, …) cannot blow the local model's context window.  The caps match
// the bash tool's output limits.
constexpr int kMaxWholeFileLines = 2000;
constexpr int kMaxOutputBytes = 50 * 1024; // 50 KB

void ReadFileTool::run(const QJsonObject &args,
                       std::function<void(const QString &, bool)> done) const
{
    const FilePath filePath = FilePath::fromUserInput(args.value("file_path").toString());
    int firstLine = args.value("first_line").toInt(1);
    int lastLineIncl = args.value("last_line_inclusive").toInt(firstLine);
    bool readAll = args.value("should_read_entire_file").toBool(false);

    const FilePath targetFile = absoluteProjectPath(filePath);
    if (const QString error = sandboxAccessError(targetFile, /*isWrite=*/false);
            !error.isEmpty())
        return done(error, false);
    if (!targetFile.exists()) {
        return done(Tr::tr("File \"%1\" does not exist.").arg(targetFile.toUserOutput()), false);
    }

    const Result<QByteArray> readRes = targetFile.fileContents();
    if (!readRes) {
        return done(Tr::tr("Failed to read \"%1\": %2")
                        .arg(targetFile.toUserOutput(), readRes.error()),
                    false);
    }

    // Image files are attached to the result instead of being dumped as
    // (mojibake) text: ChatManager sends them to the server as content
    // parts (text + image_url) for vision models.
    const QString mimeType = imageMimeType(readRes.value());
    if (!mimeType.isEmpty()) {
        if (readRes.value().size() > kMaxImageBytes) {
            return done(Tr::tr("Image \"%1\" is %2 MB large and exceeds the %3 MB read "
                               "limit; resize it first (e.g. with the bash tool).")
                            .arg(targetFile.toUserOutput())
                            .arg(readRes.value().size() / (1024 * 1024))
                            .arg(kMaxImageBytes / (1024 * 1024)),
                    false);
        }
        const QString dataUrl = QStringLiteral("data:%1;base64,").arg(mimeType)
                + QString::fromLatin1(readRes.value().toBase64());
        const QString text = Tr::tr("Image file \"%1\" (%2, %3 bytes). Its content is "
                                    "attached as an image.")
                                  .arg(targetFile.toUserOutput())
                                  .arg(mimeType)
                                  .arg(readRes.value().size());
        return done(toolResultWithImage(text, dataUrl), true);
    }

    const QString fileText = QString::fromUtf8(readRes.value());
    if (fileText.isEmpty())
        return done({}, true); // empty file – nothing to read
    const QStringList allLines = fileText.split('\n', Qt::KeepEmptyParts);
    const int totalLines = allLines.size();

    int startLine = 1; // 1‑based, inclusive
    int endLine = totalLines;
    if (!readAll) {
        if (firstLine < 1) {
            return done(Tr::tr("first_line must be >= 1."), false);
        }
        if (lastLineIncl < firstLine) {
            return done(Tr::tr("last_line_inclusive must be >= first_line."), false);
        }

        startLine = firstLine;
        endLine = qMin(lastLineIncl, totalLines);

        // Enforce the 250‑line hard limit per call.
        if (endLine - startLine + 1 > 250)
            endLine = startLine + 249;
    } else if (totalLines > kMaxWholeFileLines) {
        endLine = kMaxWholeFileLines;
    }

    if (startLine > totalLines) {
        return done(Tr::tr("first_line (%1) exceeds the number of lines in \"%2\" (%3).")
                        .arg(startLine)
                        .arg(filePath.toUserOutput())
                        .arg(totalLines),
                    false);
    }

    // Join the slice, cutting at a line boundary when the byte cap is hit.
    // The byte count is accumulated (one toUtf8() per line) instead of
    // re‑encoding the growing text on every iteration.
    QString text;
    qint64 byteSize = 0;
    int shownLastLine = endLine;
    bool byteTruncated = false;
    for (int line = startLine; line <= endLine; ++line) {
        const QByteArray lineUtf8 = allLines.at(line - 1).toUtf8();
        const qint64 added = lineUtf8.size() + (text.isEmpty() ? 0 : 1);
        if (byteSize + added > kMaxOutputBytes) {
            byteTruncated = true;
            shownLastLine = line - 1;
            break;
        }
        if (!text.isEmpty())
            text += QLatin1Char('\n');
        text += QString::fromUtf8(lineUtf8);
        byteSize += added;
    }
    if (text.isEmpty() && byteTruncated) {
        // The very first line alone exceeds the byte cap.  (A slice of only
        // empty lines is a legitimate empty result, not an error.)
        return done(Tr::tr("Line %1 of \"%2\" is larger than the %3 KB read limit; "
                           "the file cannot be read with this tool.")
                        .arg(startLine)
                        .arg(filePath.toUserOutput())
                        .arg(kMaxOutputBytes / 1024),
                    false);
    }

    // Actionable continuation hint, like the bash tool's truncation note:
    // the model should not guess where to continue.
    if (shownLastLine < totalLines) {
        if (!text.isEmpty())
            text += QStringLiteral("\n\n");
        text += QStringLiteral("[Showing lines %1-%2 of %3.")
                      .arg(startLine)
                      .arg(shownLastLine)
                      .arg(totalLines);
        if (byteTruncated)
            text += Tr::tr(" %1 KB limit reached.").arg(kMaxOutputBytes / 1024);
        text += Tr::tr(" Continue with first_line=%1, last_line_inclusive=%2.")
                      .arg(shownLastLine + 1)
                      .arg(qMin(shownLastLine + 250, totalLines))
                      + QLatin1Char(']');
    }

    return done(text, true);
}
} // namespace LlamaCpp::Tools
