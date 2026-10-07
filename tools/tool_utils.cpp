#include "tool_utils.h"

#include "factory.h"
#include "llamasettings.h"
#include "llamatr.h"
#include "mcpbridge.h"

#include <coreplugin/documentmanager.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QStandardPaths>

using namespace LlamaCpp;
using namespace ProjectExplorer;
using namespace Utils;

bool sandboxEnabled(Project *project)
{
    if (project)
        return LlamaProjectSettings(project).isSandboxEnabled();
    return settings().sandboxCommands();
}

bool pathCovers(const QString &prefix, const QString &path)
{
    return path == prefix
            || path.startsWith(prefix.endsWith(QLatin1Char('/'))
                                       ? prefix
                                       : prefix + QLatin1Char('/'));
}

// Credential locations that sandboxed tools must not read, mirroring the
// denyRead defaults of the pi sandbox extension.
QList<SecretReadPath> secretReadPaths()
{
    const QString home = QDir::homePath();
    return { { home + QStringLiteral("/.ssh"), false },
             { home + QStringLiteral("/.aws"), false },
             { home + QStringLiteral("/.gnupg"), false },
             { home + QStringLiteral("/.kube"), false },
             { home + QStringLiteral("/.netrc"), true } };
}

FilePath toolsWorkingDirectory()
{
    FilePath cwd = Core::DocumentManager::projectsDirectory();
    if (const Project *project = ProjectManager::startupProject())
        cwd = project->projectDirectory();
    return cwd;
}

QString sandboxAccessError(const FilePath &path, bool isWrite)
{
    if (!sandboxEnabled(ProjectManager::startupProject()))
        return {};

    const QString p = path.toFSPathString();
    if (isWrite) {
        const FilePath cwd = toolsWorkingDirectory();
        if (pathCovers(cwd.toFSPathString(), p) || pathCovers(QDir::tempPath(), p))
            return {};
        return Tr::tr(
                   "Writing to \"%1\" is not allowed: the sandbox only permits "
                   "writes inside the project directory and temporary "
                   "locations.")
                .arg(p);
    }

    for (const SecretReadPath &secret : secretReadPaths())
        if (pathCovers(secret.path, p))
            return Tr::tr(
                       "Reading \"%1\" is not allowed: credential locations "
                       "are not readable inside the sandbox.")
                    .arg(p);
    return {};
}

QStringList effectiveEnabledTools()
{
    QStringList enabled = settings().enabledToolsList();
    for (const QString &name : LlamaCpp::ToolFactory::instance().creatorsList()) {
        if (LlamaCpp::McpBridge::instance().isMcpTool(name))
            continue; // remote tools have their own opt-in list
        if (!enabled.contains(name))
            enabled << name;
    }
    return enabled;
}

FilePath absoluteProjectPath(const FilePath &relPath, bool mustExist)
{
    const FilePath projectFilePath = toolsWorkingDirectory().pathAppended(relPath.path());
    const FilePath generalFilePath = Core::DocumentManager::projectsDirectory()
                                        .pathAppended(relPath.path());

    if (relPath.isAbsolutePath())
        return relPath;
    if (!mustExist)
        // The target may not exist yet (a file about to be created): resolve
        // against the project directory without an existence probe.
        return projectFilePath;

    return projectFilePath.exists() ? projectFilePath : generalFilePath;
}

QString codeLanguageFor(const QString &filePath)
{
    const QString fileName = QFileInfo(filePath).fileName().toLower();
    const QString suffix = QFileInfo(filePath).suffix().toLower();
    if (fileName == "cmakelists.txt" || suffix == "cmake")
        return QStringLiteral("cmake");
    if (suffix == "cpp" || suffix == "cc" || suffix == "cxx" || suffix == "c++")
        return QStringLiteral("cpp");
    if (suffix == "h" || suffix == "hpp" || suffix == "hxx")
        return QStringLiteral("cpp");
    if (suffix == "py")
        return QStringLiteral("python");
    if (suffix == "js" || suffix == "jsx")
        return QStringLiteral("javascript");
    if (suffix == "ts" || suffix == "tsx")
        return QStringLiteral("typescript");
    if (suffix == "json")
        return QStringLiteral("json");
    if (suffix == "xml")
        return QStringLiteral("xml");
    // SVG has no dedicated syntax definition; XML highlighting reads fine.
    if (suffix == "svg")
        return QStringLiteral("xml");
    if (suffix == "yaml" || suffix == "yml")
        return QStringLiteral("yaml");
    if (suffix == "md")
        return QStringLiteral("markdown");
    if (suffix == "html")
        return QStringLiteral("html");
    if (suffix == "css")
        return QStringLiteral("css");
    if (suffix == "sh" || suffix == "bash")
        return QStringLiteral("bash");
    if (suffix == "pro")
        return QStringLiteral("text");
    if (suffix == "qml")
        return QStringLiteral("javascript");
    if (suffix == "sql")
        return QStringLiteral("sql");
    if (suffix == "rs")
        return QStringLiteral("rust");
    if (suffix == "go")
        return QStringLiteral("go");
    if (suffix == "java")
        return QStringLiteral("java");
    if (suffix == "rb")
        return QStringLiteral("ruby");
    if (suffix == "php")
        return QStringLiteral("php");
    if (suffix == "swift")
        return QStringLiteral("swift");
    if (suffix == "kt" || suffix == "kts")
        return QStringLiteral("kotlin");
    return QStringLiteral("text");
}

namespace {

const QSet<QChar> kValidJsonEscapes{'"', '\\', '/', 'b', 'f', 'n', 'r', 't', 'u'};

bool isJsonControlCharacter(QChar c)
{
    return c.unicode() >= 0x00 && c.unicode() <= 0x1f;
}

QString escapeJsonControlCharacter(QChar c)
{
    switch (c.unicode()) {
    case '\b':
        return QStringLiteral("\\b");
    case '\f':
        return QStringLiteral("\\f");
    case '\n':
        return QStringLiteral("\\n");
    case '\r':
        return QStringLiteral("\\r");
    case '\t':
        return QStringLiteral("\\t");
    default:
        return QStringLiteral("\\u%1").arg(static_cast<int>(c.unicode()), 4, 16, QChar('0'));
    }
}

} // namespace

QString repairJson(const QString &json)
{
    QString repaired;
    repaired.reserve(json.size());
    bool inString = false;
    for (int i = 0; i < json.size(); ++i) {
        const QChar ch = json.at(i);

        if (!inString) {
            repaired.append(ch);
            if (ch == QLatin1Char('"'))
                inString = true;
            continue;
        }

        if (ch == QLatin1Char('"')) {
            repaired.append(ch);
            inString = false;
            continue;
        }

        if (ch == QLatin1Char('\\')) {
            const QChar next = (i + 1 < json.size()) ? json.at(i + 1) : QChar();
            if (next.isNull()) {
                repaired.append(QLatin1String("\\\\")); // dangling backslash
                continue;
            }
            if (next == QLatin1Char('u')) {
                const QString digits = json.mid(i + 2, 4);
                bool isHex = digits.size() == 4;
                for (const QChar d : digits) {
                    if (!QLatin1String("0123456789abcdefABCDEF").contains(d)) {
                        isHex = false;
                        break;
                    }
                }
                if (isHex) {
                    repaired.append(QStringLiteral("\\u"));
                    repaired.append(digits);
                    i += 5;
                    continue;
                }
            }
            if (kValidJsonEscapes.contains(next)) {
                repaired.append(QLatin1Char('\\'));
                repaired.append(next);
                ++i;
                continue;
            }
            repaired.append(QLatin1String("\\\\")); // invalid escape: keep literal backslash
            continue;
        }

        repaired.append(isJsonControlCharacter(ch) ? escapeJsonControlCharacter(ch) : ch);
    }
    return repaired;
}
