#pragma once

#include <QString>

namespace Utils {
class FilePath;
}

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
