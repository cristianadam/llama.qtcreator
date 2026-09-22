#include "mcptool.h"

#include "mcpbridge.h"

#include <QJsonDocument>
#include <QRegularExpression>

namespace LlamaCpp::Tools {

McpTool::McpTool(const QString &name)
    : m_name(name)
{}

QString McpTool::toolDefinition() const
{
    const McpToolInfo info = McpBridge::instance().toolInfo(m_name);

    QJsonObject function;
    function["name"] = m_name;
    function["description"] = info.description.isEmpty()
                                  ? QStringLiteral("Tool provided by the Qt Creator MCP server.")
                                  : info.description;

    QJsonObject parameters = info.inputSchema;
    if (parameters.isEmpty()) {
        parameters["type"] = QStringLiteral("object");
        parameters["properties"] = QJsonObject();
    }
    function["parameters"] = parameters;

    QJsonObject root;
    root["type"] = QStringLiteral("function");
    root["function"] = function;

    return QString::fromUtf8(QJsonDocument(root).toJson(QJsonDocument::Indented));
}

QString McpTool::oneLineSummary(const QJsonObject &arguments) const
{
    // Show the first string argument to give the user an idea of what is
    // being done, e.g. "build_project ~/Projects/foo/CMakeLists.txt".
    const auto it = arguments.constBegin();
    if (it == arguments.constEnd())
        return m_name;

    QString value;
    if (it.value().isString())
        value = it.value().toString();
    else
        // non-string first argument - show the whole arguments object
        // (note: QJsonDocument::fromVariant() returns an empty document for
        // primitive values, so the variant route cannot be used here)
        value = QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Compact));

    if (value.size() > 80)
        value = value.left(77) + QStringLiteral("...");

    return m_name + QLatin1Char(' ') + value;
}

QString McpTool::streamingSummary(const QString &partialArguments) const
{
    // Mid-stream the arguments are a truncated JSON text – pick up the
    // first string value that has arrived so far; the closing quote is
    // optional since it has not arrived yet while streaming.
    static const QRegularExpression re(R"rx("([a-zA-Z_][a-zA-Z0-9_\-]*)"\s*:\s*"((?:[^"\\]|\\.)*)"?)rx");
    const QRegularExpressionMatch match = re.match(partialArguments);
    if (!match.hasMatch() || match.captured(2).isEmpty())
        return {};

    QString value = match.captured(2);
    value.replace(QStringLiteral("\\n"), QStringLiteral(" "));
    if (value.size() > 60)
        value = value.left(57) + QStringLiteral("...");

    return m_name + QLatin1Char(' ') + value;
}

QString McpTool::detailsMarkdown(const QJsonObject &arguments, const QString &result, bool ok) const
{
    Q_UNUSED(ok);
    QString md;
    if (!arguments.isEmpty()) {
        md += QStringLiteral("**Arguments**\n\n```json\n")
              + QString::fromUtf8(QJsonDocument(arguments).toJson(QJsonDocument::Indented))
              + QStringLiteral("\n```");
    }
    if (!result.isEmpty()) {
        if (!md.isEmpty())
            md += QLatin1Char('\n');
        md += QStringLiteral("\n```\n") + result + QLatin1Char('\n') + QStringLiteral("```");
    }
    return md;
}

void McpTool::run(const QJsonObject &arguments,
                  std::function<void(const QString &output, bool ok)> done) const
{
    McpBridge::instance().callTool(m_name, arguments, std::move(done));
}

} // namespace LlamaCpp::Tools
