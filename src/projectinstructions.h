#pragma once

#include <QString>
#include <QStringList>

#include <utils/filepath.h>

namespace ProjectExplorer {
class Project;
}

namespace LlamaCpp {

//! Candidate file names for the project instructions, in priority order
//! (the first existing file in a directory wins).
QStringList projectInstructionFileNames();

//! Finds the project instructions file of the project rooted at \a projectDir:
//! the candidate names are looked up in \a projectDir itself, then walked up
//! to the git repository root (the first ancestor containing a \c .git file
//! or directory), so a monorepo can keep a single instructions file for all
//! of its projects. When no git root is found only \a projectDir is searched
//! (walking further would pick up unrelated files such as
//! \c $HOME/AGENTS.md). Empty path when no candidate file exists.
Utils::FilePath findProjectInstructionsFile(const Utils::FilePath &projectDir);

//! Loads the project instructions of \a projectDir as the text that is
//! appended to the chat system message: a header naming the file followed by
//! its content (BOM stripped, content larger than 32 KB truncated). Empty
//! when no instructions file exists or it has no content.
QString loadProjectInstructions(const Utils::FilePath &projectDir);

//! True when the project instructions should be loaded for \a project: the
//! per‑project setting when a project (with its llama.cpp settings) is
//! given, the global one otherwise.
bool projectInstructionsEnabled(ProjectExplorer::Project *project);

} // namespace LlamaCpp
