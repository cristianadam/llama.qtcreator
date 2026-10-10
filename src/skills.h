#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace LlamaCpp {

/*! A single discovered skill, following the Agent Skills specification
    (agentskills.io): a directory containing a SKILL.md file with YAML
    frontmatter, or – at the root of a skill directory – a bare Markdown
    file with a description in its frontmatter. */
struct Skill
{
    QString name; //!< frontmatter "name", falling back to the directory/file name
    QString description; //!< frontmatter "description" (required)
    QString filePath; //!< canonical absolute path of the skill file
    QString baseDir; //!< directory containing the skill file
    QString content; //!< the Markdown body (without frontmatter)
    bool disableModelInvocation = false; //!< frontmatter "disable-model-invocation"
};

//! A non-fatal problem found while scanning (malformed file, invalid
//! metadata, name collision, …). Invalid skills are skipped, never fatal.
struct SkillDiagnostic
{
    QString path;
    QString message;
};

struct SkillScanResult
{
    QVector<Skill> skills;
    QVector<SkillDiagnostic> diagnostics;
};

//! Expands a leading "~" to the user's home directory.
QString expandUserHome(const QString &path);

//! Discovers skills in the directories configured on the "Skills" settings
//! page and formats them for the chat system message.
class Skills
{
public:
    //! Recursively scans \a directories for skills. Missing directories are
    //! skipped. A directory containing SKILL.md is a skill root (no further
    //! recursion into it); dot directories, node_modules and symlinked
    //! directories (they could point outside the configured root) are
    //! skipped.
    static SkillScanResult scan(const QStringList &directories);

    //! All discovered skills, enabled or not. Cached per directory list;
    //! the chat manager clears the cache at the start of every model turn
    //! and the settings page after changes, so on-disk edits are picked up
    //! with at most a one-turn delay. The skill tool uses this (rather than
    //! a fresh scan) so it and the system message always agree on what
    //! exists.
    static QVector<Skill> allSkills();

    //! All discovered skills that are advertised to the model: the user did
    //! not disable them (i.e. their SKILL.md path is not in the
    //! DisabledSkillsList setting) and they do not set
    //! "disable-model-invocation" in their frontmatter. The disabled list is
    //! re-read on every call; only the scan is cached (see allSkills()).
    static QVector<Skill> enabledSkills();

    //! Drops the scan cache.
    static void clearCache();

    //! The <available_skills> block appended to the chat system message.
    //! Empty when \a skills is empty.
    static QString formatForPrompt(const QVector<Skill> &skills);
};

} // namespace LlamaCpp
