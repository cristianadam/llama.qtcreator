#include "regexsearch_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTextStream>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <texteditor/basefilefind.h>
#include <utils/filepath.h>
#include <utils/searchresultitem.h>

using namespace Utils;
using namespace ProjectExplorer;

namespace LlamaCpp {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(RegexSearchTool{}.name(),
                                            []() { return std::make_unique<RegexSearchTool>(); });
    return true;
}();
} // namespace

QString RegexSearchTool::name() const
{
    return QStringLiteral("regex_search");
}

QString RegexSearchTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "regex_search",
            "description": "Fast text-based regex search in the code base (prefer it for finding exact function names or expressions) that finds exact pattern matches with file names and line numbers within files or directories. If there is no exclude_pattern - provide an empty string. Returns matches in format file_name:line_number: line_content",
            "parameters": {
                "type": "object",
                "properties": {
                    "include_pattern": { "type": "string", "description": "Comma separated glob patterns for files to include (specify file extensions only if you are absolutely sure)" },
                    "exclude_pattern": { "type": "string", "description": "Comma separated glob patterns for files to exclude" },
                    "regex": { "type": "string", "description": "A string for constructing a regular expression pattern to search for. Escape special regex characters when needed." }
                },
                "required": [ "include_pattern", "regex" ],
                "strict": true
            }
        }
    })raw";
}

QString RegexSearchTool::oneLineSummary(const QJsonObject &args) const
{
    const QString regex = args.value("regex").toString();
    return QStringLiteral("regex search %1").arg(regex);
}

class RegexFileFind : public TextEditor::BaseFileFind
{
public:
    using TextEditor::BaseFileFind::currentSearchEngine;
    using TextEditor::BaseFileFind::searchDir;
    using TextEditor::BaseFileFind::setSearchDir;

    QString id() const { return {}; }
    QString displayName() const { return {}; }
    QString label() const { return {}; }
    QString toolTip() const { return {}; }

    TextEditor::FileContainerProvider fileContainerProvider() const
    {
        return [this] {
            return SubDirFileContainer(FilePaths{searchDir()},
                                       fileFindParameters.nameFilters,
                                       fileFindParameters.exclusionFilters);
        };
    }

    TextEditor::FileFindParameters fileFindParameters;
};

void RegexSearchTool::run(const QJsonObject &args,
                          std::function<void(const QString &, bool)> done) const
{
    const QString includePattern = args.value("include_pattern").toString();
    const QString excludePattern = args.value("exclude_pattern").toString();
    const QString regex = args.value("regex").toString();

    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    RegexFileFind finder;
    finder.setSearchDir(cwd);

    TextEditor::FileFindParameters &params = finder.fileFindParameters;
    params.text = regex;
    params.flags = FindRegularExpression;
    params.nameFilters = includePattern.isEmpty() ? QStringList()
                                                  : splitFilterUiText(includePattern);
    params.exclusionFilters = excludePattern.isEmpty() ? QStringList()
                                                       : splitFilterUiText(excludePattern);
    params.searchDir = cwd;
    params.fileContainerProvider = finder.fileContainerProvider();

    TextEditor::SearchEngine *engine = finder.currentSearchEngine();
    QTC_ASSERT(engine, return done(Tr::tr("No search engine available."), false));

    params.searchExecutor = engine->searchExecutor();
    QFuture<SearchResultItems> future = params.searchExecutor(params);
    future.waitForFinished();
    const SearchResultItems items = future.result();

    if (items.isEmpty())
        return done(Tr::tr("No matches found for pattern \"%1\".").arg(regex), false);

    QStringList lines;
    for (const SearchResultItem &it : items) {
        // `it.path()` returns a `FilePath` – make it relative to the cwd
        const QString relPath
            = FilePath::fromString(it.path().first()).relativeChildPath(cwd).toUserOutput();
        const int lineNumber = it.mainRange().begin.line;
        const QString lineText = it.lineText();
        lines << QString("%1:%2: %3").arg(relPath, QString::number(lineNumber), lineText);
    }

    return done(lines.join('\n'), true);
}
} // namespace LlamaCpp
