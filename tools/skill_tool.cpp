#include "skill_tool.h"
#include "factory.h"
#include "llamatr.h"
#include "llamasettings.h"
#include "skills.h"

#include <QDir>
#include <QFileInfo>

namespace LlamaCpp::Tools {

namespace {
const bool registered = [] {
    ToolFactory::instance().registerCreator(SkillTool{}.name(),
                                            []() { return std::make_unique<SkillTool>(); });
    return true;
}();

// The skill_files list is a sample, not exhaustive (like opencode's).
constexpr int kMaxListedFiles = 10;

// A skill's instructions are injected into the conversation verbatim; cap
// them like project instructions so one oversized SKILL.md cannot blow up
// the context.
constexpr int kMaxSkillContentChars = 32 * 1024;

//! Collects the files under \a dir (recursively, dot directories and
//! node_modules skipped, SKILL.md excluded) as sorted relative paths, at
//! most kMaxListedFiles of them.
void collectSkillFiles(const QDir &dir, const QString &prefix, QStringList &out)
{
    if (out.size() >= kMaxListedFiles)
        return;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
                                                    QDir::Name);
    for (const QFileInfo &entry : std::as_const(entries)) {
        const QString rel = prefix.isEmpty() ? entry.fileName()
                                             : prefix + QLatin1Char('/') + entry.fileName();
        if (entry.isDir()) {
            if (entry.fileName() == QLatin1String("node_modules"))
                continue;
            collectSkillFiles(QDir(entry.absoluteFilePath()), rel, out);
        } else if (entry.fileName() != QLatin1String("SKILL.md")) {
            out << rel;
            if (out.size() >= kMaxListedFiles)
                return;
        }
    }
}

//! The model-facing result: the skill's instructions, its base directory
//! (relative paths in the skill resolve against it) and a sampled list of
//! the accompanying files.
QString skillToolOutput(const Skill &skill)
{
    QString content = skill.content.trimmed();
    if (content.length() > kMaxSkillContentChars)
        content = content.left(kMaxSkillContentChars) + QStringLiteral("\n\n[... truncated ...]");

    QStringList lines;
    lines << QStringLiteral("<skill_content name=\"%1\">").arg(skill.name)
          << QStringLiteral("# Skill: %1").arg(skill.name)
          << QString()
          << content
          << QString()
          << QStringLiteral("Base directory for this skill: %1").arg(skill.baseDir)
          << QStringLiteral("Relative paths in this skill (e.g., scripts/, references/) are "
                            "relative to this base directory.");

    if (QFileInfo(skill.filePath).fileName() == QLatin1String("SKILL.md")) {
        QStringList files;
        collectSkillFiles(QDir(skill.baseDir), QString(), files);
        if (!files.isEmpty()) {
            lines << QStringLiteral("Note: file list is sampled (at most %1 files).")
                          .arg(kMaxListedFiles)
                  << QString()
                  << QStringLiteral("<skill_files>");
            for (const QString &file : files)
                lines << QStringLiteral("<file>%1</file>").arg(file);
            lines << QStringLiteral("</skill_files>");
        }
    }
    lines << QStringLiteral("</skill_content>");
    return lines.join(QLatin1Char('\n'));
}
} // namespace

QString SkillTool::name() const
{
    return QStringLiteral("skill");
}

QString SkillTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "skill",
            "description": "Load a specialized skill when the task matches one of the available skills in the system context. Returns the skill's instructions, its base directory and a sample of the files that come with it; relative paths are relative to that base directory.",
            "parameters": {
                "type": "object",
                "properties": {
                    "name": { "type": "string", "description": "The name of the skill from the available skills list." }
                },
                "required": [ "name" ],
                "strict": true
            }
        }
    })raw";
}

QString SkillTool::oneLineSummary(const QJsonObject &args) const
{
    return Tr::tr("load skill %1").arg(args.value("name").toString());
}

void SkillTool::run(const QJsonObject &args,
                    std::function<void(const QString &, bool)> done) const
{
    const QString name = args.value("name").toString().trimmed();
    if (name.isEmpty())
        return done(Tr::tr("The \"name\" argument is required."), false);

    // Shared cached scan (refreshed at the start of every model turn), so
    // this always agrees with what the system message advertised. An
    // explicit invocation may also load skills that are not advertised
    // (unchecked on the settings page) – like pi's explicit /skill:
    // invocation.
    const QVector<Skill> skills = Skills::allSkills();
    const Skill *skill = nullptr;
    for (const Skill &candidate : skills) {
        if (candidate.name == name) {
            skill = &candidate;
            break;
        }
    }
    if (!skill) {
        QStringList names;
        for (const Skill &candidate : skills)
            names << candidate.name;
        return done(Tr::tr("Skill \"%1\" not found. Available skills: %2")
                        .arg(name,
                             names.isEmpty() ? Tr::tr("(none)")
                                             : names.join(QLatin1Char(', '))),
                    false);
    }

    return done(skillToolOutput(*skill), true);
}

} // namespace LlamaCpp::Tools
