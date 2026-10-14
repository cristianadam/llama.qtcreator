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
#include <QProcessEnvironment>
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

static Qt::CaseSensitivity pathCaseSensitivity()
{
    // NTFS and the default APFS/HFS+ volumes are case-insensitive: ~/.SSH is
    // ~/.ssh there, so the path checks must not be fooled by the spelling.
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return Qt::CaseInsensitive;
#else
    return Qt::CaseSensitive;
#endif
}

QString cleanedPath(const QString &path)
{
    if (path.isEmpty())
        return {};
    const QString absolute = QDir::isAbsolutePath(path)
            ? path
            : QDir(toolsWorkingDirectory().toFSPathString()).absoluteFilePath(path);
    // cleanPath also turns native separators into '/'.
    QString cleaned = QDir::cleanPath(absolute);
    // cleanPath keeps ".." segments that climb above the root, while the
    // file system treats them as the root itself ("/../etc" is "/etc").
    qsizetype rootLength = 0;
    if (cleaned.startsWith(QLatin1Char('/')) && !cleaned.startsWith(QLatin1String("//")))
        rootLength = 1;
    else if (cleaned.size() >= 3 && cleaned.at(1) == QLatin1Char(':')
             && cleaned.at(2) == QLatin1Char('/'))
        rootLength = 3;
    if (rootLength > 0) {
        while (true) {
            const QStringView rest = QStringView(cleaned).mid(rootLength);
            if (rest == u"..")
                cleaned.remove(rootLength, 2);
            else if (rest.startsWith(u"../"))
                cleaned.remove(rootLength, 3);
            else
                break;
        }
    }
    return cleaned;
}

namespace {

// Parent of a cleaned absolute path; empty for a root ("/", "C:/").
QString parentOfCleaned(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    if (slash < 0 || slash == path.size() - 1)
        return {}; // a root, or not an absolute path
    // Keep the separator of a root parent ("/x" -> "/", "C:/x" -> "C:/").
    if (slash == 0 || (slash == 2 && path.at(1) == QLatin1Char(':')))
        return path.left(slash + 1);
    return path.left(slash);
}

QString joinedPath(const QString &base, const QStringList &tail)
{
    if (tail.isEmpty())
        return base;
    return (base.endsWith(QLatin1Char('/')) ? base : base + QLatin1Char('/'))
         + tail.join(QLatin1Char('/'));
}

QString resolvedPathImpl(const QString &path, int depth)
{
    // Bounds symlink chains (and loops) - fail closed past it.
    constexpr int kMaxLinkDepth = 32;
    if (depth > kMaxLinkDepth)
        return {};

    QString existing = cleanedPath(path);
    QStringList tail; // the components below the deepest existing ancestor
    while (!existing.isEmpty()) {
        const QFileInfo info(existing);
        if (info.exists()) {
            const QString canonical = info.canonicalFilePath();
            if (canonical.isEmpty())
                return {};
            return joinedPath(canonical, tail);
        }
        // A dangling link: a write through it creates its target, so
        // continue with the target (relative targets are resolved by
        // QFileInfo against the link's directory).
        QString target;
        if (info.isSymbolicLink())
            target = info.symLinkTarget();
#if defined(Q_OS_WIN)
        else if (info.isJunction())
            target = info.junctionTarget();
#endif
        if (!target.isEmpty())
            return resolvedPathImpl(joinedPath(QDir::cleanPath(target), tail), depth + 1);

        const QString parent = parentOfCleaned(existing);
        if (parent.isEmpty())
            return joinedPath(existing, tail); // nothing exists, not even the root
        tail.prepend(existing.mid(parent.size() + (parent.endsWith(QLatin1Char('/')) ? 0 : 1)));
        existing = parent;
    }
    return {};
}

// Both spellings of a location: lexically cleaned and with links resolved
// (when resolvable), without duplicates.
QStringList pathSpellings(const QString &path)
{
    QStringList spellings;
    const QString lexical = cleanedPath(path);
    if (!lexical.isEmpty())
        spellings << lexical;
    const QString resolved = resolvedPathImpl(path, 0);
    if (!resolved.isEmpty() && resolved.compare(lexical, pathCaseSensitivity()) != 0)
        spellings << resolved;
    return spellings;
}

bool coveredByAny(const QStringList &prefixes, const QString &path)
{
    if (path.isEmpty())
        return false;
    for (const QString &prefix : prefixes)
        if (pathCovers(prefix, path))
            return true;
    return false;
}

// A directory entry from the Sandbox settings page, cleaned: trimmed, '~'
// expanded, made absolute and cleaned to the platform separator. Empty
// entries, relative paths (ambiguous without a project) and file system
// roots (granting one would make the whole drive writable) are dropped.
// The result has no duplicates.
QStringList cleanSandboxDirs(const QStringList &raw)
{
    QStringList result;
    const QString home = QDir::homePath();
    for (const QString &entry : raw) {
        QString dir = entry.trimmed();
        if (dir.isEmpty())
            continue;
        if (dir == QLatin1String("~"))
            dir = home;
        else if (dir.startsWith(QLatin1String("~/")))
            dir = home + dir.mid(1);
        if (!QDir::isAbsolutePath(dir))
            continue;
        dir = QDir::cleanPath(dir);
        if (QFileInfo(dir).isRoot())
            continue;
        if (!result.contains(dir))
            result << dir;
    }
    return result;
}

// A path component as the file system sees it: on Windows, trailing dots
// and spaces are dropped (".git." opens ".git") and so is an NTFS stream
// suffix (".git::$INDEX_ALLOCATION" is the .git directory itself).
QString effectiveComponent(const QString &component)
{
#if defined(Q_OS_WIN)
    QString effective = component;
    if (const int colon = effective.indexOf(QLatin1Char(':')); colon >= 0)
        effective.truncate(colon);
    while (effective.endsWith(QLatin1Char('.')) || effective.endsWith(QLatin1Char(' ')))
        effective.chop(1);
    return effective;
#else
    return component;
#endif
}

#if defined(Q_OS_WIN)
// Device paths (\\?\C:\..., \\.\...) and administrative shares
// (\\localhost\C$\...) reach local files under spellings the path checks
// cannot relate to the local ones.
bool isDeviceOrAdminSharePath(const QString &cleaned)
{
    if (cleaned.startsWith(QLatin1String("//?/")) || cleaned.startsWith(QLatin1String("//./")))
        return true;
    if (!cleaned.startsWith(QLatin1String("//")))
        return false;
    const QStringList parts = cleaned.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return parts.size() >= 2 && parts.at(1).endsWith(QLatin1Char('$'));
}
#endif

} // namespace

QString resolvedPath(const QString &path)
{
    return resolvedPathImpl(path, 0);
}

bool pathCovers(const QString &prefix, const QString &path)
{
    if (prefix.isEmpty() || path.isEmpty())
        return false;
    const QString cleanPrefix = QDir::cleanPath(prefix);
    const QString cleanPath = QDir::cleanPath(path);
    const Qt::CaseSensitivity cs = pathCaseSensitivity();
    return cleanPath.compare(cleanPrefix, cs) == 0
            || cleanPath.startsWith(cleanPrefix.endsWith(QLatin1Char('/'))
                                            ? cleanPrefix
                                            : cleanPrefix + QLatin1Char('/'),
                                    cs);
}

// Credential locations that sandboxed tools must not read, mirroring (and
// extending) the denyRead defaults of the pi sandbox extension.
QList<SecretReadPath> secretReadPaths()
{
    const QString home = QDir::homePath();
    QList<SecretReadPath> paths = {
        { home + QStringLiteral("/.ssh"), false },
        { home + QStringLiteral("/.aws"), false },
        { home + QStringLiteral("/.gnupg"), false },
        { home + QStringLiteral("/.kube"), false },
        { home + QStringLiteral("/.netrc"), true },
        { home + QStringLiteral("/.git-credentials"), true },
        { home + QStringLiteral("/.config/gh"), false },
        { home + QStringLiteral("/.config/gcloud"), false },
        { home + QStringLiteral("/.docker/config.json"), true },
        { home + QStringLiteral("/.npmrc"), true },
        { home + QStringLiteral("/.pypirc"), true },
        { home + QStringLiteral("/.azure"), false },
        { home + QStringLiteral("/.password-store"), false },
        { home + QStringLiteral("/.local/share/keyrings"), false },
        { home + QStringLiteral("/.cargo/credentials.toml"), true },
        { home + QStringLiteral("/.gem/credentials"), true },
    };
#if defined(Q_OS_WIN)
    // DPAPI master keys and the Credential Manager stores.
    const QString appData = qEnvironmentVariable("APPDATA");
    if (!appData.isEmpty()) {
        paths << SecretReadPath{ QDir::fromNativeSeparators(appData)
                                     + QStringLiteral("/Microsoft/Credentials"),
                                 false }
              << SecretReadPath{ QDir::fromNativeSeparators(appData)
                                     + QStringLiteral("/Microsoft/Protect"),
                                 false };
    }
    const QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
    if (!localAppData.isEmpty())
        paths << SecretReadPath{ QDir::fromNativeSeparators(localAppData)
                                     + QStringLiteral("/Microsoft/Credentials"),
                                 false };
#endif
    return paths;
}

QStringList sandboxDenyReadDirs()
{
    return cleanSandboxDirs(settings().sandboxDenyReadDirs());
}

QList<SecretReadPath> denyReadPaths()
{
    QList<SecretReadPath> paths = secretReadPaths();
    for (const QString &dir : sandboxDenyReadDirs())
        paths << SecretReadPath{ dir, /* isFile = */ false };
    return paths;
}

QStringList sandboxWritableDirs()
{
    return cleanSandboxDirs(settings().sandboxWritableDirs());
}

QStringList sandboxReadOnlyDirs()
{
    return cleanSandboxDirs(settings().sandboxReadOnlyDirs());
}

bool sandboxAllowsNetwork()
{
    return settings().sandboxAllowNetwork();
}

QStringList secretPathSpellings()
{
    QStringList spellings;
    // The user-configured deny-read directories are protected like the
    // built-in credential locations, so the file tools honour them here.
    for (const SecretReadPath &secret : denyReadPaths())
        spellings << pathSpellings(secret.path);
    return spellings;
}

bool isSecretPath(const QString &path, const QStringList &secretSpellings)
{
    const QString lexical = cleanedPath(path);
    if (coveredByAny(secretSpellings, lexical))
        return true;
    // A link into a credential location, or a path through one.
    const QString resolved = resolvedPath(lexical);
    return resolved.isEmpty() ? false : coveredByAny(secretSpellings, resolved);
}

bool isSecretPath(const QString &path)
{
    return isSecretPath(path, secretPathSpellings());
}

QStringList secretPathsInside(const QString &root)
{
    QStringList relative;
    const QStringList rootSpellings = pathSpellings(root);
    for (const QString &secret : secretPathSpellings()) {
        for (const QString &base : rootSpellings) {
            if (!pathCovers(base, secret) || pathCovers(secret, base))
                continue;
            const QString rel = secret.mid(base.size()
                                           + (base.endsWith(QLatin1Char('/')) ? 0 : 1));
            if (!rel.isEmpty() && !relative.contains(rel))
                relative << rel;
        }
    }
    return relative;
}

bool isProtectedWritePath(const QString &path)
{
    const QStringList components
        = cleanedPath(path).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (const QString &component : components) {
        const QString effective = effectiveComponent(component);
        if (effective.compare(QLatin1String(".git"), Qt::CaseInsensitive) == 0
            || effective.compare(QLatin1String(".qtcreator"), Qt::CaseInsensitive) == 0)
            return true;
    }
    if (components.isEmpty())
        return false;
    // Qt Creator's per-project settings (CMakeLists.txt.user,
    // foo.pro.user.4.8-pre1, ...).
    const QString name = effectiveComponent(components.last()).toLower();
    return name.endsWith(QLatin1String(".user")) || name.contains(QLatin1String(".user."));
}

QStringList protectedSubpaths(const QString &dir)
{
    QStringList result;
    for (const QString &name : { QStringLiteral(".git"), QStringLiteral(".qtcreator") }) {
        const QString path = QDir::cleanPath(dir + QLatin1Char('/') + name);
        if (QFileInfo::exists(path))
            result << path;
    }
    const QStringList userFiles = QDir(dir).entryList(
        { QStringLiteral("*.user"), QStringLiteral("*.user.*") },
        QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &name : userFiles)
        result << QDir::cleanPath(dir + QLatin1Char('/') + name);
    return result;
}

bool isSecretEnvironmentVariable(const QString &name)
{
    const QString upper = name.toUpper();
    static const QStringList exact = {
        QStringLiteral("GITHUB_TOKEN"), QStringLiteral("GH_TOKEN"),
        QStringLiteral("HF_TOKEN"), QStringLiteral("NPM_TOKEN"),
        QStringLiteral("SSH_AUTH_SOCK"), QStringLiteral("GPG_AGENT_INFO"),
        QStringLiteral("DBUS_SESSION_BUS_ADDRESS"),
    };
    static const QStringList prefixes = {
        QStringLiteral("AWS_"), QStringLiteral("AZURE_"), QStringLiteral("OPENAI"),
        QStringLiteral("ANTHROPIC"), QStringLiteral("GOOGLE_API"),
        QStringLiteral("GEMINI"), QStringLiteral("HUGGING"),
    };
    static const QStringList suffixes = {
        QStringLiteral("_KEY"), QStringLiteral("_TOKEN"), QStringLiteral("_APIKEY"),
        QStringLiteral("_PAT"), QStringLiteral("_CREDENTIALS"),
    };
    static const QStringList fragments = {
        QStringLiteral("SECRET"), QStringLiteral("PASSWORD"), QStringLiteral("PASSWD"),
        QStringLiteral("API_KEY"), QStringLiteral("ACCESS_KEY"), QStringLiteral("AUTH_TOKEN"),
    };
    if (exact.contains(upper))
        return true;
    for (const QString &prefix : prefixes)
        if (upper.startsWith(prefix))
            return true;
    for (const QString &suffix : suffixes)
        if (upper.endsWith(suffix))
            return true;
    for (const QString &fragment : fragments)
        if (upper.contains(fragment))
            return true;
    return false;
}

QProcessEnvironment withoutSecretVariables(const QProcessEnvironment &env)
{
    QProcessEnvironment result = env;
    for (const QString &key : env.keys())
        if (isSecretEnvironmentVariable(key))
            result.remove(key);
    return result;
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
    Project *project = ProjectManager::startupProject();
    if (!sandboxEnabled(project))
        return {};

    // Both the lexical and the link-resolved spelling are checked: ".."
    // segments must not walk out of an allowed location, and neither may a
    // symlink or junction (e.g. one planted in the project by a sandboxed
    // command).
    const QString p = path.toFSPathString();
    const QString lexical = cleanedPath(p);
    const QString resolved = resolvedPath(lexical);

#if defined(Q_OS_WIN)
    if (isDeviceOrAdminSharePath(lexical) || isDeviceOrAdminSharePath(resolved))
        return Tr::tr(
                   "Accessing \"%1\" is not allowed: device paths and administrative "
                   "shares are not accessible inside the sandbox.")
                .arg(p);
#endif

    if (isWrite) {
        // Without a project the projects directory (often the home
        // directory) would become writable; only the temporary locations
        // are then.
        QStringList roots;
        if (project)
            roots << pathSpellings(project->projectDirectory().toFSPathString());
        roots << pathSpellings(QDir::tempPath());
        // Extra writable directories from the Sandbox settings page.
        for (const QString &dir : sandboxWritableDirs())
            roots << pathSpellings(dir);
        // An unresolvable path (a link loop) fails closed.
        if (!coveredByAny(roots, lexical) || !coveredByAny(roots, resolved)) {
            if (!project)
                return Tr::tr(
                           "Writing to \"%1\" is not allowed: no project is open, "
                           "so the sandbox only permits writes inside temporary "
                           "locations and the extra writable directories from "
                           "the Sandbox settings. Open a project to work on its "
                           "files.")
                        .arg(p);
            return Tr::tr(
                       "Writing to \"%1\" is not allowed: the sandbox only permits "
                       "writes inside the project directory, temporary locations "
                       "and the extra writable directories from the Sandbox "
                       "settings.")
                    .arg(p);
        }
        // Locations whose content is executed or loaded outside the sandbox
        // (git hooks and config, Qt Creator's per-project settings, which
        // also hold the sandbox setting itself).
        if (isProtectedWritePath(lexical) || isProtectedWritePath(resolved))
            return Tr::tr(
                       "Writing to \"%1\" is not allowed: .git, .qtcreator and "
                       "*.user files are protected inside the sandbox.")
                    .arg(p);
        const QStringList secrets = secretPathSpellings();
        if (coveredByAny(secrets, lexical) || coveredByAny(secrets, resolved))
            return Tr::tr(
                       "Writing to \"%1\" is not allowed: credential locations "
                       "are protected inside the sandbox.")
                    .arg(p);
        return {};
    }

    const QStringList secrets = secretPathSpellings();
    if (coveredByAny(secrets, lexical) || coveredByAny(secrets, resolved))
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
    // Cleaned, so ".." segments are resolved here and not by the file
    // system after the sandbox checks looked at the path.
    const auto cleaned = [](const FilePath &path) {
        return path.isLocal() && !path.isEmpty()
                   ? FilePath::fromString(cleanedPath(path.toFSPathString()))
                   : path.cleanPath();
    };
    const FilePath projectFilePath = cleaned(toolsWorkingDirectory().pathAppended(relPath.path()));
    const FilePath generalFilePath
        = cleaned(Core::DocumentManager::projectsDirectory().pathAppended(relPath.path()));

    if (relPath.isAbsolutePath())
        return cleaned(relPath);
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
