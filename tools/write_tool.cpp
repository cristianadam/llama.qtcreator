#include "write_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "tool_utils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <utils/filepath.h>

using namespace Utils;

namespace LlamaCpp::Tools {

namespace {

const bool registered = [] {
    ToolFactory::instance().registerCreator(WriteTool{}.name(),
                                            []() { return std::make_unique<WriteTool>(); });
    return true;
}();

} // namespace

QString WriteTool::name() const
{
    return QStringLiteral("write");
}

QString WriteTool::toolDefinition() const
{
    const QString description = R"desc(
Create a new file or completely overwrite an existing one with the given content.

- Use write for new files and for full rewrites. For small, targeted changes to an existing file use edit_file instead (and read_file it first).
- Parent directories are created automatically.
- Overwriting an existing file replaces its entire content – read it first if you need to keep parts of it.
)desc";

    QJsonObject pathProperty;
    pathProperty[QStringLiteral("type")] = QStringLiteral("string");
    pathProperty[QStringLiteral("description")] =
        QStringLiteral("The file to create or overwrite. Relative to the project directory, "
                       "or absolute.");

    QJsonObject contentProperty;
    contentProperty[QStringLiteral("type")] = QStringLiteral("string");
    contentProperty[QStringLiteral("description")] =
        QStringLiteral("The full content to write to the file.");

    QJsonObject properties;
    properties[QStringLiteral("path")] = pathProperty;
    properties[QStringLiteral("content")] = contentProperty;

    QJsonObject parameters;
    parameters[QStringLiteral("type")] = QStringLiteral("object");
    parameters[QStringLiteral("properties")] = properties;
    parameters[QStringLiteral("required")] = QJsonArray{QStringLiteral("path"),
                                                         QStringLiteral("content")};
    parameters[QStringLiteral("additionalProperties")] = false;

    QJsonObject function;
    function[QStringLiteral("name")] = QStringLiteral("write");
    function[QStringLiteral("description")] = description.trimmed();
    function[QStringLiteral("parameters")] = parameters;

    QJsonObject root;
    root[QStringLiteral("type")] = QStringLiteral("function");
    root[QStringLiteral("function")] = function;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString WriteTool::oneLineSummary(const QJsonObject &args) const
{
    const QString path = args.value("path").toString();
    if (path.isEmpty())
        return Tr::tr("Write file");
    return Tr::tr("write %1").arg(path);
}

QString WriteTool::streamingSummary(const QString &partialArgs) const
{
    // "path" is the first field, so a usable summary appears almost
    // immediately while the arguments are still streaming in.
    int idx = partialArgs.indexOf(QStringLiteral("\"path\""));
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
    const QString path = partialArgs.mid(start + 1, end - start - 1);
    if (path.isEmpty())
        return {};
    return Tr::tr("write %1").arg(path);
}

QString WriteTool::detailsMarkdown(const QJsonObject &args, const QString &result, bool ok) const
{
    if (!ok)
        return result;

    // No header: the summary already says "write <path>".
    const QString content = args.value("content").toString();
    if (content.isEmpty())
        return {};
    return codeFence(content);
}

void WriteTool::run(const QJsonObject &args,
                    std::function<void(const QString &, bool)> done) const
{
    const QString path = args.value("path").toString().trimmed();
    if (path.isEmpty())
        return done(Tr::tr("Tool error: \"path\" must be a non-empty string."), false);
    if (!args.contains("content"))
        return done(Tr::tr("Tool error: \"content\" is required."), false);
    const QString content = args.value("content").toString();

    const FilePath target = absoluteProjectPath(FilePath::fromUserInput(path), /*mustExist=*/false);
    if (target.parentDir().isFile())
        return done(Tr::tr("Cannot write \"%1\": a parent path is a file.").arg(path), false);

    const FilePath parentDir = target.parentDir();
    if (!parentDir.exists()) {
        const Result<> mkRes = parentDir.ensureWritableDir();
        if (!mkRes)
            return done(Tr::tr("Cannot write \"%1\": cannot create directory: %2")
                            .arg(path, mkRes.error()),
                        false);
    }

    const Result<qint64> writeRes = target.writeFileContents(content.toUtf8());
    if (!writeRes)
        return done(Tr::tr("Cannot write \"%1\": %2").arg(path, writeRes.error()), false);

    return done(Tr::tr("Successfully wrote %1 bytes to %2.")
                    .arg(writeRes.value())
                    .arg(path),
                true);
}

} // namespace LlamaCpp::Tools
