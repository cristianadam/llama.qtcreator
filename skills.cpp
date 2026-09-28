#include "skills.h"

#include "llamasettings.h"
#include "llamatr.h"

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

namespace LlamaCpp {

QString expandUserHome(const QString &path)
{
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/")))
        return QDir::homePath() + path.mid(1);
    return path;
}

namespace {

// Agent Skills spec limits.
constexpr int kMaxNameLength = 64;
constexpr int kMaxDescriptionLength = 1024;

QString escapeXml(QString text)
{
    text.replace(QLatin1String("&"), QLatin1String("&amp;"));
    text.replace(QLatin1String("<"), QLatin1String("&lt;"));
    text.replace(QLatin1String(">"), QLatin1String("&gt;"));
    text.replace(QLatin1String("\""), QLatin1String("&quot;"));
    text.replace(QLatin1String("'"), QLatin1String("&apos;"));
    return text;
}

bool isValidName(const QString &name)
{
    if (name.isEmpty() || name.length() > kMaxNameLength)
        return false;
    static const QRegularExpression re(QLatin1String("^[a-z0-9-]+$"));
    if (!re.match(name).hasMatch())
        return false;
    return name.at(0) != QLatin1Char('-') && name.at(name.size() - 1) != QLatin1Char('-')
            && !name.contains(QLatin1String("--"));
}

//! Splits a Markdown file into its YAML frontmatter (empty when the file
//! does not start with a "---" fence) and the body.
QPair<QString, QString> splitFrontmatter(const QString &content)
{
    QString normalized = content;
    normalized.replace(QLatin1String("\r\n"), QLatin1String("\n"))
        .replace(QLatin1Char('\r'), QLatin1Char('\n'));
    if (!normalized.startsWith(QLatin1String("---")))
        return {QString(), normalized};
    const int end = normalized.indexOf(QLatin1String("\n---"), 3);
    if (end == -1)
        return {QString(), normalized};
    return {normalized.mid(4, end - 3), normalized.mid(end + 4).trimmed()};
}

//! Reads and validates one skill file. Returns an empty Skill and sets
//! \a error when the file must be skipped; \a warning (when non-null) receives
//! a non-fatal problem – the skill is still loaded.
Skill loadSkillFromFile(const QString &filePath, bool isDeclaredSkill,
                        QString *error, QString *warning)
{
    Skill skill;

    QString raw;
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = Tr::tr("failed to read file");
            return {};
        }
        raw = QString::fromUtf8(file.readAll());
    }

    const auto [frontmatter, body] = splitFrontmatter(raw);

    YAML::Node fm;
    if (!frontmatter.isEmpty()) {
        try {
            fm = YAML::Load(frontmatter.toStdString());
        } catch (const YAML::Exception &e) {
            *error = Tr::tr("invalid YAML frontmatter: %1").arg(QString::fromUtf8(e.what()));
            return {};
        }
        if (!fm.IsMap()) {
            *error = Tr::tr("frontmatter is not a key/value mapping");
            return {};
        }
    }

    const QString fileBaseName = QFileInfo(filePath).completeBaseName();
    const QString parentDirName = QFileInfo(filePath).dir().dirName();

    QString name;
    if (fm["name"].IsScalar())
        name = QString::fromStdString(fm["name"].as<std::string>());
    name = name.trimmed();
    const bool explicitName = !name.isEmpty();
    if (name.isEmpty())
        name = isDeclaredSkill ? parentDirName : fileBaseName;

    QString description;
    if (fm["description"].IsScalar())
        description = QString::fromStdString(fm["description"].as<std::string>());
    description = description.trimmed();

    if (fm["disable-model-invocation"].IsScalar())
        skill.disableModelInvocation = fm["disable-model-invocation"].as<bool>();

    // A bare (non‑declared) Markdown file without a description is not a
    // skill – skip it silently, the way pi does.
    if (!isDeclaredSkill && description.isEmpty())
        return skill;

    if (description.isEmpty()) {
        *error = Tr::tr("description is required");
        return {};
    }
    if (description.length() > kMaxDescriptionLength)
        *error = Tr::tr("description exceeds %1 characters (%2)")
                     .arg(kMaxDescriptionLength)
                     .arg(description.length());
    if (!isValidName(name))
        *error = Tr::tr("invalid name \"%1\" (lowercase a‑z, 0‑9 and hyphens only, "
                        "max %2 characters, no leading/trailing/consecutive hyphens)")
                     .arg(name)
                     .arg(kMaxNameLength);
    if (error && !error->isEmpty())
        return {};

    // Like pi: a declared skill whose name does not match its directory is
    // kept, but flagged – it is the most common authoring mistake. (Bare
    // root files fall back to the *file* name, so no check applies there.)
    if (warning && isDeclaredSkill && explicitName && name != parentDirName)
        *warning = Tr::tr("name \"%1\" does not match its directory \"%2\"")
                       .arg(name, parentDirName);

    skill.name = name;
    skill.description = description;
    skill.filePath = QFileInfo(filePath).canonicalFilePath();
    skill.baseDir = QFileInfo(skill.filePath).dir().absolutePath();
    skill.content = body;
    return skill;
}

void scanDir(const QString &dir,
             bool includeRootFiles,
             SkillScanResult &out,
             QSet<QString> &seenFiles,
             QHash<QString, QString> &nameOwners)
{
    QDir d(dir);
    if (!d.exists())
        return;

    const QFileInfoList entries = d.entryInfoList(
            {QDir::Files, QDir::Dirs, QDir::NoDotAndDotDot}, QDir::Name);

    auto addSkill = [&](const Skill &skill, const QString &path) {
        if (seenFiles.contains(skill.filePath))
            return; // same file reached via a second directory/symlink
        const auto owner = nameOwners.constFind(skill.name);
        if (owner != nameOwners.constEnd()) {
            out.diagnostics.append({path,
                                    Tr::tr("name \"%1\" collides with %2 – keeping the "
                                           "first one")
                                            .arg(skill.name, owner.value())});
            return;
        }
        seenFiles.insert(skill.filePath);
        nameOwners.insert(skill.name, skill.filePath);
        out.skills.append(skill);
    };

    // A directory containing SKILL.md is a skill root – load it and stop.
    for (const QFileInfo &entry : entries) {
        if (entry.fileName() != QLatin1String("SKILL.md") || !entry.isFile())
            continue;
        QString error;
        QString warning;
        const Skill skill = loadSkillFromFile(entry.absoluteFilePath(), true, &error, &warning);
        if (error.isEmpty()) {
            if (!skill.name.isEmpty()) { // empty name: not a skill, skip silently
                if (!warning.isEmpty())
                    out.diagnostics.append({entry.absoluteFilePath(), warning});
                addSkill(skill, entry.absoluteFilePath());
            }
        } else {
            out.diagnostics.append({entry.absoluteFilePath(), error});
        }
        return;
    }

    for (const QFileInfo &entry : entries) {
        const QString absPath = entry.absoluteFilePath();
        if (entry.isDir()) {
            if (entry.fileName() == QLatin1String("node_modules"))
                continue;
            if (entry.isSymLink())
                continue; // don't follow links out of the configured root
            scanDir(absPath, false, out, seenFiles, nameOwners);
            continue;
        }
        if (includeRootFiles && entry.suffix().compare(QLatin1String("md"), Qt::CaseInsensitive)
                == 0) {
            QString error;
            QString warning;
            const Skill skill = loadSkillFromFile(absPath, false, &error, &warning);
            if (error.isEmpty()) {
                if (!skill.name.isEmpty()) { // no description: not a skill
                    if (!warning.isEmpty())
                        out.diagnostics.append({absPath, warning});
                    addSkill(skill, absPath);
                }
            } else {
                out.diagnostics.append({absPath, error});
            }
        }
    }
}

} // namespace

SkillScanResult Skills::scan(const QStringList &directories)
{
    SkillScanResult out;
    QSet<QString> seenFiles;
    QHash<QString, QString> nameOwners;
    for (const QString &dir : directories) {
        const QString expanded = expandUserHome(dir.trimmed());
        if (expanded.isEmpty())
            continue;
        scanDir(expanded, /* includeRootFiles = */ true, out, seenFiles, nameOwners);
    }
    return out;
}

QVector<Skill> Skills::allSkills()
{
    const QStringList dirs = settings().skillsDirectories();
    static QHash<QString, QVector<Skill>> cache;
    const QString key = dirs.join(QChar(0x1F));
    auto it = cache.constFind(key);
    if (it == cache.constEnd())
        it = cache.insert(key, scan(dirs).skills);
    return it.value();
}

void Skills::clearCache()
{
    static QHash<QString, QVector<Skill>> cache;
    cache.clear();
}

QVector<Skill> Skills::enabledSkills()
{
    const QVector<Skill> all = allSkills();

    QSet<QString> disabled;
    for (const QString &path : settings().disabledSkillsList())
        disabled.insert(path);
    QVector<Skill> res;
    res.reserve(all.size());
    for (const Skill &skill : all)
        if (!disabled.contains(skill.filePath) && !skill.disableModelInvocation)
            res.append(skill);
    return res;
}

QString Skills::formatForPrompt(const QVector<Skill> &skills)
{
    if (skills.isEmpty())
        return QString();

    // Model‑facing text, kept in English on purpose (not translated).
    // The skill tool (not read_file) loads a skill: it returns the
    // instructions, the base directory and the accompanying files, so the
    // list only needs name and description.
    QStringList lines;
    lines << QStringLiteral("Skills provide specialized instructions and workflows for "
                            "specific tasks.")
          << QStringLiteral("Use the skill tool to load a skill when the task matches its "
                            "description.")
          << QString();
    lines << QStringLiteral("<available_skills>");
    for (const Skill &skill : skills) {
        lines << QStringLiteral("  <skill>")
              << QStringLiteral("    <name>%1</name>").arg(escapeXml(skill.name))
              << QStringLiteral("    <description>%1</description>")
                     .arg(escapeXml(skill.description))
              << QStringLiteral("  </skill>");
    }
    lines << QStringLiteral("</available_skills>");
    return lines.join(QLatin1Char('\n'));
}

} // namespace LlamaCpp
