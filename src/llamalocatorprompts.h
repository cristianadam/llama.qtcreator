#pragma once

#include <QList>
#include <QString>

namespace LlamaCpp {

//! Values for the placeholders of the "ll" locator prompts.
struct LocatorPlaceholders
{
    QString selection; //!< fenced block of the current editor selection (empty without one)
    QString file; //!< path of the current file (empty without one)
    QString line; //!< current cursor line, 1-based (empty without a text editor)
    QString diagnostics; //!< errors/warnings reported in the current file
    QString gitDiff; //!< uncommitted changes of the current file's git repository
};

//! A single error/warning mark of a document (input of formatDiagnostics()).
struct LocatorDiagnostic
{
    int line = 0; //!< 1-based line number
    QString text; //!< e.g. "Clangd: no matching function for call to ..."
};

//! Replaces {selection}, {file}, {line}, {diagnostics} and {git_diff} in
//! \a prompt with the corresponding values; placeholders without a value
//! become an empty string. The replacement values are not scanned again for
//! placeholders, so a selection containing e.g. "{file}" is safe.
QString applyLocatorPlaceholders(const QString &prompt, const LocatorPlaceholders &values);

//! Assembles the {diagnostics} placeholder value from the diagnostic marks of
//! a document, sorted by line and capped (with a "… and N more" note).
//! Returns a "(no errors or warnings reported for this file)" hint when
//! \a diagnostics is empty.
QString formatDiagnostics(const QList<LocatorDiagnostic> &diagnostics);

//! Assembles the {git_diff} placeholder value from the outputs of
//! `git status --porcelain` (\a status) and `git diff` (\a diff). Returns a
//! "(no uncommitted changes)" hint for a clean repository, an error hint when
//! \a gitFailed is true, and appends the list of untracked files (which a
//! `git diff` does not show), capped with a "… and N more" note. Oversized
//! diffs are truncated at a line boundary with a note.
QString formatGitDiff(const QString &status, const QString &diff, bool gitFailed);

} // namespace LlamaCpp
