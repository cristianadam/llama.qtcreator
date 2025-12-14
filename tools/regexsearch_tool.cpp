#include "regexsearch_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <QFutureWatcher>
#include <QRegularExpression>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>
#include <utils/filesearch.h>
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
            "description": "Fast text-based regex search in the code base (prefer it for finding exact function names or expressions). Returns matches in the format `file_name:line_number: line_content`. By default only the first 100 matches are returned; set `provide_all_results` to true to get all matches.",
            "parameters": {
                "type": "object",
                "properties": {
                    "include_pattern": { "type": "string", "description": "Comma‑separated glob patterns for files to include (specify file extensions only if you are absolutely sure)" },
                    "exclude_pattern": { "type": "string", "description": "Comma‑separated glob patterns for files to exclude" },
                    "regex": { "type": "string", "description": "A string for constructing a regular‑expression pattern to search for. Escape special regex characters when needed." },
                    "provide_all_results": {
                        "type": "boolean",
                        "description": "If true, return all matches. If omitted or false, the tool returns at most 100 matches.",
                        "default": false
                    }
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
    const bool all = args.value("provide_all_results").toBool(false);
    if (all)
        return QStringLiteral("regex search <tt>%1</tt> (all results)").arg(regex);
    return QStringLiteral("regex search <tt>%1</tt> (max 100)").arg(regex);
}

void RegexSearchTool::run(const QJsonObject &args,
                          std::function<void(const QString &, bool)> done) const
{
    const QString includePattern = args.value("include_pattern").toString();
    const QString excludePattern = args.value("exclude_pattern").toString();
    const QString regex = args.value("regex").toString();

    const bool provideAll = args.value("provide_all_results").toBool(false);

    const int kDefaultLimit = 100;
    int maxResults = provideAll ? std::numeric_limits<int>::max() : kDefaultLimit;

    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *p = ProjectManager::startupProject())
        cwd = p->projectDirectory();

    const QStringList includeFilters = includePattern.isEmpty() ? QStringList()
                                                                : splitFilterUiText(includePattern);
    const QStringList excludeFilters = excludePattern.isEmpty()
                                           ? splitFilterUiText("*/.git/*,*/.qtcreator/*,*/build/*")
                                           : splitFilterUiText(excludePattern);

    const SubDirFileContainer container(FilePaths{cwd}, includeFilters, excludeFilters);

    const FindFlags flags = FindRegularExpression | DontFindBinaryFiles;
    const QFuture<SearchResultItems> future = Utils::findInFiles(regex, container, flags, {});

    // Watch the whole asynchronous operation.
    auto *watcher = new QFutureWatcher<SearchResultItems>(nullptr);
    watcher->setFuture(future);

    QObject::connect(watcher,
                     &QFutureWatcher<SearchResultItems>::finished,
                     [watcher, cwd, regex, done, maxResults, provideAll]() mutable {
                         const QFuture<SearchResultItems> f = watcher->future();
                         SearchResultItems allItems;
                         for (const SearchResultItems &partial : f.results())
                             allItems << partial;

                         if (allItems.isEmpty()) {
                             done(Tr::tr("No matches found for pattern \"%1\".").arg(regex), false);
                         } else {
                             QStringList lines;
                             int resultCount = 0;

                             for (const SearchResultItem &it : std::as_const(allItems)) {
                                 if (resultCount >= maxResults)
                                     break; // respect the limit

                                 // Convert the absolute path to a path relative to the search root.
                                 const QString relPath = FilePath::fromString(it.path().first())
                                                             .relativeChildPath(cwd)
                                                             .toUserOutput();

                                 const int lineNumber = it.mainRange().begin.line;
                                 const QString lineText = it.lineText();

                                 lines << QString("%1:%2: %3")
                                              .arg(relPath, QString::number(lineNumber), lineText);
                                 ++resultCount;
                             }

                             // If we stopped because of the limit, tell the user/LLM.
                             if (!provideAll && allItems.size() > maxResults) {
                                 lines << Tr::tr("[output truncated to first %1 results; set "
                                                 "`provide_all_results` to true for more]")
                                              .arg(maxResults);
                             }

                             done(lines.join('\n'), true);
                         }

                         watcher->deleteLater(); // clean‑up
                     });
}
} // namespace LlamaCpp
