#pragma once

#include <QString>
#include <QStringList>

namespace Utils {
class FilePath;
}

namespace ProjectExplorer {
class Project;
}

//! True when the sandbox is enabled (global or per-project setting; always
//! false on Windows, where there is no per-command sandbox).
bool sandboxEnabled(ProjectExplorer::Project *project);

//! True when \a path equals \a prefix or is located inside it.
bool pathCovers(const QString &prefix, const QString &path);

//! Credential locations (directories and files) that sandboxed tools must
//! not read.
QStringList secretReadPaths();

//! Model-facing error when the (enabled) sandbox forbids \a isWrite access
//! to \a path; an empty string when the access is allowed or the sandbox is
//! disabled. Mirrors the bash sandbox: writes are restricted to the project
//! directory and the temporary locations, reads are denied for the
//! credential locations.
QString sandboxAccessError(const Utils::FilePath &path, bool isWrite);

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
