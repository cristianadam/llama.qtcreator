#pragma once

#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace Utils {
class FilePath;
}

namespace ProjectExplorer {
class Project;
}

//! True when the sandbox is enabled (global or per-project setting; same
//! resolution on all platforms, including Windows).
bool sandboxEnabled(ProjectExplorer::Project *project);

//! True when \a path equals \a prefix or is located inside it. Both are
//! lexically cleaned first; the comparison is case-insensitive on Windows
//! and macOS. An empty prefix covers nothing.
bool pathCovers(const QString &prefix, const QString &path);

//! Lexically normalized absolute spelling of \a path ('/' separators, "."
//! and ".." resolved); a relative path is taken relative to the tools
//! working directory. Links are not resolved, see resolvedPath().
QString cleanedPath(const QString &path);

//! \a path with symlinks and junctions resolved: the deepest existing
//! ancestor is canonicalized and the non-existing tail re-appended (a
//! dangling link is followed to its target, as a write through it would
//! create the target). Empty when the path cannot be resolved (link loop).
QString resolvedPath(const QString &path);

//! True for paths whose content is executed or loaded outside the sandbox
//! and which the sandbox therefore never lets tools write: any component
//! named .git or .qtcreator (case-insensitively) and Qt Creator's
//! per-project settings files (*.user, *.user.*).
bool isProtectedWritePath(const QString &path);

//! The existing protected locations directly inside \a dir (dir/.git,
//! dir/.qtcreator, dir/ *.user*) for the platform sandboxes to mount
//! read-only.
QStringList protectedSubpaths(const QString &dir);

//! A credential location that sandboxed tools must not read.
struct SecretReadPath
{
    QString path;
    bool isFile = false; //!< true for file locations (e.g. ~/.netrc),
                         //!< false for directories
};

//! Credential locations (directories and files) that sandboxed tools must
//! not read.
QList<SecretReadPath> secretReadPaths();

//! The user-configured deny-read directories (Sandbox settings page),
//! cleaned to absolute paths with the platform separator; empty entries,
//! relative entries and file system roots are dropped, '~' is expanded,
//! duplicates removed.
QStringList sandboxDenyReadDirs();

//! The full deny-read set: the built-in credential locations plus the
//! user-configured deny-read directories. Everything it covers is
//! unreadable and unwritable by the sandboxed tools.
QList<SecretReadPath> denyReadPaths();

//! The user-configured writable directories (Sandbox settings page): extra
//! locations the sandboxed tools may write to, on top of the project
//! directory and the temporary locations (cleaned, see sandboxDenyReadDirs).
QStringList sandboxWritableDirs();

//! The user-configured read-only directories (Sandbox settings page): extra
//! locations readable inside the sandbox. Only the Windows process container
//! restricts reads, so this takes effect on Windows (cleaned, see
//! sandboxDenyReadDirs).
QStringList sandboxReadOnlyDirs();

//! True when sandboxed bash commands may use the network (Sandbox settings
//! page); false by default, in which case the sandbox blocks all network
//! access.
bool sandboxAllowsNetwork();

//! The deny-read locations (the built-in credential locations plus the
//! user-configured deny-read directories) in both spellings (cleaned and
//! link-resolved), for repeated isSecretPath() checks.
QStringList secretPathSpellings();

//! True when \a path (in its cleaned or its link-resolved spelling) lies in
//! one of the deny-read locations \a secretSpellings.
bool isSecretPath(const QString &path, const QStringList &secretSpellings);
bool isSecretPath(const QString &path);

//! The credential locations below \a root, relative to it ('/'
//! separators), e.g. ".ssh" for the home directory; used to exclude them
//! from recursive searches.
QStringList secretPathsInside(const QString &root);

//! True for environment variable names that typically carry credentials
//! (*_KEY, *_TOKEN, *SECRET*, *PASSWORD*, AWS_*, OPENAI*, ...) or agent
//! sockets (SSH_AUTH_SOCK, DBUS_SESSION_BUS_ADDRESS, ...); case-insensitive.
bool isSecretEnvironmentVariable(const QString &name);

//! \a env without the variables isSecretEnvironmentVariable() matches; the
//! environment sandboxed commands get.
QProcessEnvironment withoutSecretVariables(const QProcessEnvironment &env);

//! The directory the tools operate in: the startup project directory when a
//! project is open, otherwise the default projects directory.
Utils::FilePath toolsWorkingDirectory();

//! Model-facing error when the (enabled) sandbox forbids \a isWrite access
//! to \a path; an empty string when the access is allowed or the sandbox is
//! disabled. Mirrors the bash sandbox: writes are restricted to the project
//! directory and the temporary locations (only the latter when no project
//! is open) minus the protected and credential locations, reads are denied
//! for the credential locations. Both the cleaned and the link-resolved
//! spelling of \a path are checked.
QString sandboxAccessError(const Utils::FilePath &path, bool isWrite);

//! The effectively enabled local tools: the stored EnabledToolsList plus
//! every registered tool the stored list does not know about yet, so tools
//! added by a newer plugin version are enabled by default.  A tool the user
//! explicitly disabled stays disabled – it is still present in the stored
//! list.  Remote (MCP) tools are not part of this list; they have their
//! own opt-in EnabledMcpToolsList.
QStringList effectiveEnabledTools();

/*! Resolves \a relPath against the startup project directory (or the project
    directory when no project is open).

    When \a mustExist is true (the default) a relative path is preferred in
    the project directory only if it exists there, and otherwise falls back
    to the general project directory; useful for tools that only touch
    files that already exist.  When it is false the path is resolved against
    the project directory unconditionally, which is what tools that create
    new files (write, apply_patch add) need: an existence probe would always
    fail for a fresh file and misplace it in the general project directory. */
Utils::FilePath absoluteProjectPath(const Utils::FilePath &relPath, bool mustExist = true);

//! Returns the syntax‑highlighting language name for a fenced code block
//! showing the contents of \a filePath, derived from the file suffix.
//! SVG maps to "xml" (there is no dedicated SVG definition, and XML
//! highlighting reads fine for it).
QString codeLanguageFor(const QString &filePath);

/*! Repairs the two JSON string‑literal malformations that small models
    produce most often, so that \c QJsonDocument::fromJson can parse the
    result: raw control characters inside strings (real newlines/tabs in a
    \c command or \c content value) are escaped, and backslashes before
    invalid escape characters are doubled.  The string is returned unchanged
    when it is already valid.  Structural problems (truncated objects,
    missing braces) are *not* repaired – only string literals.

    Ported from the pi coding agent (packages/ai/src/utils/json-parse.ts). */
QString repairJson(const QString &json);
