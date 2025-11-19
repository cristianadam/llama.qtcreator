#include "python_tool.h"
#include "factory.h"

#include <QProcess>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp::Tools {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(PythonTool{}.name(),
                                            []() { return std::make_unique<PythonTool>(); });
    return true;
}();
} // namespace

QString PythonTool::name() const
{
    return QStringLiteral("python");
}

QString PythonTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "python",
            "description": "Runs code in an Python interpreter and returns the result of the execution.",
            "parameters": {
                "type": "object",
                "properties": {
                    "code": {
                        "type": "string",
                        "description": "The code to run in the Python interpreter."
                    }
                },
                "required": ["code"],
                "strict": true
            }
        }
    })raw";
}

QString PythonTool::oneLineSummary(const QJsonObject &) const
{
    return QStringLiteral("running python");
}

QString PythonTool::run(const QJsonObject &arguments) const
{
    const QString code = arguments.value("code").toString();

    QProcess proc;
    proc.setProgram("python3");
    proc.setArguments({"-u", "-c", code});
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();
    proc.setWorkingDirectory(cwd.toFSPathString());
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start();
    proc.waitForFinished();
    return QString::fromUtf8(proc.readAllStandardOutput());
}

QString PythonTool::detailsMarkdown(const QJsonObject &arguments, const QString &result) const
{
    const QString code = arguments.value("code").toString();
    return QString("```python\n%1\n```\n\n"
                   "**Result:**\n"
                   "```\n%2\n```")
        .arg(code, result);
}
} // namespace LlamaCpp::Tools
