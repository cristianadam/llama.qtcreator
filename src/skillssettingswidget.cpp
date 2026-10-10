#include "skillssettingswidget.h"
#include "llamasettings.h"
#include "llamatr.h"
#include "llamasyntaxhighlighter.h"
#include "skills.h"

#include <texteditor/fontsettings.h>

#include <utils/fancylineedit.h>

#include <QDir>
#include <QFileDialog>
#include <QHeaderView>
#include <QRegularExpression>

#include <algorithm>

using namespace Utils;

namespace LlamaCpp {

namespace {

//! Canonical form of a configured directory: skills are stored with their
//! canonical file paths, so directory matching must canonicalize as well –
//! otherwise a stored path that only textually differs (trailing slash,
//! symlink, case) would group no skills under it.
QString canonicalDir(const QString &path)
{
    const QString expanded = expandUserHome(path.trimmed());
    const QString canonical = QFileInfo(expanded).canonicalFilePath();
    return canonical.isEmpty() ? expanded : canonical;
}

// The full skill descriptions can be long and would blow up the list. Show a
// short summary in the tree; the full description and the SKILL.md content
// are available in the detail pane.
QString elideForList(const QString &text, int maxLength = 100)
{
    QString result = text;
    while (result.contains(QStringLiteral("\n")))
        result.replace(QStringLiteral("\n"), QStringLiteral(" "));

    if (result.length() <= maxLength)
        return result;

    // Prefer a sentence boundary, then a word boundary, else a hard cut.
    int cut = result.lastIndexOf(QLatin1Char('.'), maxLength);
    if (cut < 20)
        cut = result.lastIndexOf(QLatin1Char(' '), maxLength);
    if (cut < 20)
        cut = maxLength;
    return result.left(cut).trimmed() + QStringLiteral("…");
}

} // namespace

SkillsSettingsWidget::SkillsSettingsWidget()
{
    // Add/Remove buttons, in a column to the right of the tree (like the
    // CMake tools settings page). They manage the skill directories, which
    // are the tree's top-level rows.
    m_addButton = new QPushButton(Tr::tr("Add…"), this);
    connect(m_addButton,
            &QPushButton::clicked,
            this,
            [this] {
                const QString dir = QFileDialog::getExistingDirectory(
                        this, Tr::tr("Choose a Skills Directory"));
                if (dir.isEmpty())
                    return;
                QStringList dirs = settings().skillsDirectories();
                if (!dirs.contains(dir)) {
                    dirs << dir;
                    settings().skillsDirectories.setValue(dirs);
                    rescan();
                }
            });

    m_removeButton = new QPushButton(Tr::tr("Remove"), this);
    m_removeButton->setEnabled(false); // only enabled on a directory row
    connect(m_removeButton, &QPushButton::clicked, this, &SkillsSettingsWidget::removeCurrentDirectory);

    m_view = new QTreeView(this);
    m_view->setUniformRowHeights(true);
    m_view->setHeaderHidden(false);

    m_detailEdit = new QTextEdit(this);
    m_detailEdit->setReadOnly(true);
    m_detailEdit->setWordWrapMode(QTextOption::NoWrap);
    m_detailEdit->setPlaceholderText(Tr::tr("Select a skill to view its content"));

    // Show the SKILL.md source in the editor's fixed font, with Markdown
    // syntax highlighting (the same "Markdown" definition and color scheme
    // the Qt Creator editor uses).
    m_detailEdit->setFont(TextEditor::globalFontSettings().data().font());
    m_markdownHighlighter = new SkillsMarkdownHighlighter(m_detailEdit->document());

    m_diagnosticsLabel = new QLabel(this);
    m_diagnosticsLabel->setVisible(false);

    // model
    m_model = new TreeModel<>(m_view);
    m_model->setHeader({Tr::tr("Skill"), Tr::tr("Description")});

    m_filterModel = new SkillsFilterModel(m_view);
    m_filterModel->setSourceModel(m_model);
    m_filterModel->setFilterRole(Qt::DisplayRole);

    m_view->setModel(m_filterModel);
    m_view->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    auto filterLineEdit = new FancyLineEdit(this);
    filterLineEdit->setObjectName(QStringLiteral("filterLineEdit"));
    filterLineEdit->setFiltering(true);

    // Layout: the tree with the Add/Remove buttons in a narrow column to
    // its right (grouped with the tree, like the CMake tools settings
    // page), the detail pane and the diagnostics label below, spanning the
    // full width.
    using namespace Layouting;
    Column{
        new QLabel(Tr::tr("Skill directories:")),
        Row{
            Column{filterLineEdit, m_view},
            Column{m_addButton, m_removeButton, st},
        },
        m_detailEdit,
        m_diagnosticsLabel,
    }.attachTo(this);

    connect(filterLineEdit,
            &FancyLineEdit::textChanged,
            m_filterModel,
            &SkillsFilterModel::setFilterWildcard);

    // Filtering resets the expanded state of the group rows – re-expand them
    // so the matching skills are directly visible.
    connect(filterLineEdit, &FancyLineEdit::textChanged, this, [this] { m_view->expandAll(); });

    // keep model in sync when the user toggles a check-box
    connect(m_model, &TreeModel<>::dataChanged, this, [this](const QModelIndex &top) {
        if (m_synchronizing)
            return;
        m_synchronizing = true;

        // If a group row was toggled, propagate the state to its children.
        if (m_model->rowCount(top) > 0) {
            const Qt::CheckState groupState =
                    static_cast<Qt::CheckState>(m_model->data(top, Qt::CheckStateRole).toInt());
            if (groupState != Qt::PartiallyChecked) {
                const int childCount = m_model->rowCount(top);
                for (int row = 0; row < childCount; ++row)
                    m_model->setData(m_model->index(row, 0, top), groupState, Qt::CheckStateRole);
            }
        }

        syncGroupStates();

        m_synchronizing = false;
        updateDisabledFromModel();
    });

    connect(m_view->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            &SkillsSettingsWidget::showSkillDetails);

    rescan();
}

// A directory row is a top-level row of the (proxy) model.
bool SkillsSettingsWidget::isDirectoryRow(const QModelIndex &index) const
{
    return index.isValid() && !index.parent().isValid();
}

void SkillsSettingsWidget::removeCurrentDirectory()
{
    const QModelIndex current = m_view->currentIndex();
    if (!isDirectoryRow(current))
        return;
    // The row holds the canonical path, the setting may not – compare
    // canonical forms when removing.
    const QString rowPath = canonicalDir(current.data(GroupItem::PathRole).toString());
    QStringList dirs = settings().skillsDirectories();
    dirs.erase(std::remove_if(dirs.begin(),
                              dirs.end(),
                              [&rowPath](const QString &dir) {
                                  return canonicalDir(dir) == rowPath;
                              }),
               dirs.end());
    settings().skillsDirectories.setValue(dirs);
    rescan();
}

void SkillsSettingsWidget::rescan()
{
    Skills::clearCache();
    const SkillScanResult result = Skills::scan(settings().skillsDirectories());

    m_model->clear();
    const QStringList dirs = settings().skillsDirectories();
    QSet<QString> grouped; // a skill belongs to the first matching dir only
    for (const QString &dir : dirs) {
        const QString canonical = canonicalDir(dir);
        const QString prefix = canonical + QLatin1Char('/');
        QVector<Skill> groupSkills;
        for (const Skill &skill : result.skills) {
            if (!skill.filePath.startsWith(prefix))
                continue;
            if (grouped.contains(skill.filePath))
                continue;
            grouped.insert(skill.filePath);
            groupSkills.append(skill);
        }
        // Every configured directory gets a row – also when it holds no
        // skills, so it can be selected and removed from the tree.
        auto *group = new GroupItem(canonical);
        m_model->rootItem()->appendChild(group);
        for (const Skill &skill : groupSkills)
            group->appendChild(new SkillItem(skill.name,
                                             skill.description,
                                             skill.filePath,
                                             skill.content));
    }

    // Don't let the per-row updates below re-enter the dataChanged handler
    // (and write transient settings values).
    m_synchronizing = true;
    updateModelFromDisabled();
    m_synchronizing = false;
    m_view->expandAll();

    if (result.diagnostics.isEmpty()) {
        m_diagnosticsLabel->setVisible(false);
    } else {
        QString text = Tr::tr("%1 skill file(s) were skipped: %2")
                               .arg(result.diagnostics.size())
                               .arg(result.diagnostics.first().message);
        QStringList toolTip;
        for (const SkillDiagnostic &d : result.diagnostics)
            toolTip << d.path + QStringLiteral(": ") + d.message;
        m_diagnosticsLabel->setToolTip(toolTip.join(QLatin1Char('\n')));
        m_diagnosticsLabel->setText(text);
        m_diagnosticsLabel->setVisible(true);
    }
}

void SkillsSettingsWidget::showSkillDetails(const QModelIndex &current,
                                            const QModelIndex & /*previous*/)
{
    // The Remove button only applies to directory rows.
    m_removeButton->setEnabled(isDirectoryRow(current));

    // Only skill rows have details; a directory row (or no selection) just
    // clears the pane. (Test the row kind, not a role value: group items
    // carry a PathRole too – their own directory path.)
    if (!current.isValid() || isDirectoryRow(current)) {
        m_detailEdit->clear();
        return;
    }

    const QString path = current.data(SkillItem::PathRole).toString();
    const QString name = current.data(Qt::DisplayRole).toString();
    const QString description = current.data(SkillItem::DescriptionRole).toString();
    const QString content = current.data(SkillItem::ContentRole).toString();

    QStringList parts;
    parts << Tr::tr("Name: %1").arg(name);
    if (!description.isEmpty())
        parts << Tr::tr("Description: %1").arg(description);
    if (!path.isEmpty())
        parts << Tr::tr("File: %1").arg(path);
    if (!content.isEmpty()) {
        parts << QString();
        parts << content;
    }
    m_detailEdit->setPlainText(parts.join(QLatin1Char('\n')));
}

/*
 * Recompute the check-state of every group row from the states of its
 * children (Checked / Unchecked / PartiallyChecked).
 */
void SkillsSettingsWidget::syncGroupStates()
{
    const int groupCount = m_model->rowCount();
    for (int row = 0; row < groupCount; ++row) {
        const QModelIndex groupIdx = m_model->index(row, 0);
        int checked = 0;
        int unchecked = 0;
        const int childCount = m_model->rowCount(groupIdx);
        for (int childRow = 0; childRow < childCount; ++childRow) {
            const Qt::CheckState state = static_cast<Qt::CheckState>(
                    m_model->data(m_model->index(childRow, 0, groupIdx), Qt::CheckStateRole).toInt());
            if (state == Qt::Checked)
                ++checked;
            else if (state == Qt::Unchecked)
                ++unchecked;
        }

        const Qt::CheckState state = checked == 0 ? Qt::Unchecked
                               : unchecked == 0 ? Qt::Checked
                                                : Qt::PartiallyChecked;
        m_model->setData(groupIdx, state, Qt::CheckStateRole);
    }
}

void SkillsSettingsWidget::updateDisabledFromModel()
{
    // Walk through all skills and collect the paths whose check-state is
    // Unchecked (skills are enabled by default).
    QStringList disabled;
    const int groupCount = m_model->rowCount();
    for (int groupRow = 0; groupRow < groupCount; ++groupRow) {
        const QModelIndex groupIdx = m_model->index(groupRow, 0);
        const int childCount = m_model->rowCount(groupIdx);
        for (int row = 0; row < childCount; ++row) {
            const QModelIndex idx = m_model->index(row, 0, groupIdx);
            const bool checked = static_cast<Qt::CheckState>(
                                             idx.data(Qt::CheckStateRole).toInt())
                    == Qt::Checked;
            if (!checked)
                disabled << idx.data(SkillItem::PathRole).toString();
        }
    }
    settings().disabledSkillsList.setValue(disabled);
}

void SkillsSettingsWidget::updateModelFromDisabled()
{
    const QStringList disabled = settings().disabledSkillsList();
    const int groupCount = m_model->rowCount();
    for (int groupRow = 0; groupRow < groupCount; ++groupRow) {
        const QModelIndex groupIdx = m_model->index(groupRow, 0);
        const int childCount = m_model->rowCount(groupIdx);
        for (int row = 0; row < childCount; ++row) {
            const QModelIndex idx = m_model->index(row, 0, groupIdx);
            const bool checked = !disabled.contains(idx.data(SkillItem::PathRole).toString());
            m_model->setData(idx, checked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        }
    }
    syncGroupStates();
}

void SkillsSettingsWidget::apply()
{
    // Ensure the latest UI state is persisted.
    updateDisabledFromModel();
    Skills::clearCache();
    // The settings object already knows its values, we just need to write
    // them to disk.
    settings().writeSettings();
}

void SkillsSettingsWidget::cancel()
{
    // Re‑load the stored values – this discards any UI changes.
    settings().readSettings();
    Skills::clearCache();
    rescan();
}

/* ------------------------------------------ SkillsMarkdownHighlighter */

SkillsMarkdownHighlighter::SkillsMarkdownHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    m_engine.setDefinition(syntaxDefinitionForName(QStringLiteral("Markdown")));
    m_defaultFormat =
            TextEditor::globalFontSettings().data().toTextCharFormat(TextEditor::C_TEXT);
}

void SkillsMarkdownHighlighter::highlightBlock(const QString &text)
{
    // The engine carries inter-line state (e.g. an open code fence). Reset
    // it at the start of the document so re-highlighting fresh content does
    // not continue a fence from the previous content.
    if (currentBlock().blockNumber() == 0)
        m_engine.resetState();

    QVector<HighlightFragment> fragments;
    m_engine.highlight(text, m_defaultFormat, fragments);
    int position = 0;
    for (const HighlightFragment &fragment : fragments) {
        setFormat(position, fragment.text.length(), fragment.format);
        position += fragment.text.length();
    }
}

/* ---------------------------------------------------------------- GroupItem */

SkillsSettingsWidget::GroupItem::GroupItem(const QString &directoryPath)
    : m_path(directoryPath)
{
}

QVariant SkillsSettingsWidget::GroupItem::data(int column, int role) const
{
    if (column == 0) {
        if (role == Qt::DisplayRole)
            return m_path;
        if (role == Qt::CheckStateRole)
            return m_checkState;
        if (role == Qt::ToolTipRole)
            return m_path;
        if (role == PathRole)
            return m_path;
        return QVariant();
    }

    if (column == 1 && role == Qt::DisplayRole) {
        const int count = childCount();
        return count == 1 ? Tr::tr("1 skill") : Tr::tr("%1 skills").arg(count);
    }

    return QVariant();
}

Qt::ItemFlags SkillsSettingsWidget::GroupItem::flags(int column) const
{
    if (column == 0)
        return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

/* The actual propagation to the children is done by the widget's
 * dataChanged handler – here we only track the requested state. */
bool SkillsSettingsWidget::GroupItem::setData(int column, const QVariant &value, int role)
{
    if (column == 0 && role == Qt::CheckStateRole) {
        const Qt::CheckState newState = static_cast<Qt::CheckState>(value.toInt());
        if (newState != m_checkState) {
            m_checkState = newState;
            // Returning true tells TreeModel<> to emit dataChanged for us.
            return true;
        }
    }
    return false;
}

/* ---------------------------------------------------- SkillsFilterModel */

SkillsSettingsWidget::SkillsFilterModel::SkillsFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    // All columns are checked; the actual matching is done in
    // filterAcceptsRow() (name, full description and group path are all
    // taken into account there).
    setFilterKeyColumn(-1);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
}

bool SkillsSettingsWidget::SkillsFilterModel::filterAcceptsRow(int source_row,
                                                               const QModelIndex &source_parent) const
{
    const QRegularExpression regex = filterRegularExpression();
    const QModelIndex index = sourceModel()->index(source_row, 0, source_parent);

    if (sourceModel()->hasChildren(index)) {
        // Group row: visible when the directory matches or when at least one
        // of the children does.
        if (regex.match(sourceModel()->data(index, Qt::DisplayRole).toString()).hasMatch())
            return true;
        const int childCount = sourceModel()->rowCount(index);
        for (int row = 0; row < childCount; ++row)
            if (filterAcceptsRow(row, index))
                return true;
        return false;
    }

    // Skill row: match against the name, the full (unelided) description or
    // the path of the group (directory) the skill belongs to.
    if (regex.match(sourceModel()->data(index, Qt::DisplayRole).toString()).hasMatch())
        return true;
    QString description = sourceModel()->data(index, SkillItem::DescriptionRole).toString();
    if (description.isEmpty())
        description = sourceModel()->data(index.siblingAtColumn(1), Qt::DisplayRole).toString();
    if (regex.match(description).hasMatch())
        return true;
    if (source_parent.isValid()
        && regex.match(sourceModel()->data(source_parent, Qt::DisplayRole).toString()).hasMatch())
        return true;
    return false;
}

/* --------------------------------------------------------------- SkillItem */

SkillsSettingsWidget::SkillItem::SkillItem(const QString &name,
                                           const QString &description,
                                           const QString &path,
                                           const QString &content)
    : m_name(name)
    , m_description(description)
    , m_path(path)
    , m_content(content)
{
    // default to unchecked – the UI will later set the correct state
    m_checkState = Qt::Unchecked;
}

QVariant SkillsSettingsWidget::SkillItem::data(int column, int role) const
{
    // Column 0 – name + check‑box
    if (column == 0) {
        if (role == Qt::DisplayRole)
            return m_name;
        if (role == Qt::CheckStateRole)
            return m_checkState;
        if (role == Qt::ToolTipRole)
            return m_path;
        return QVariant();
    }

    // Column 1 – description (elided, the full text is in the detail pane)
    if (column == 1 && role == Qt::DisplayRole)
        return elideForList(m_description);

    if (role == Qt::ToolTipRole)
        return elideForList(m_description);
    if (role == DescriptionRole)
        return m_description;
    if (role == PathRole)
        return m_path;
    if (role == ContentRole)
        return m_content;

    return QVariant();
}

/* flags() – only column 0 is user‑checkable */
Qt::ItemFlags SkillsSettingsWidget::SkillItem::flags(int column) const
{
    if (column == 0)
        return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

/* setData() – handle changes of the check‑state */
bool SkillsSettingsWidget::SkillItem::setData(int column, const QVariant &value, int role)
{
    Q_UNUSED(column);
    if (role == Qt::CheckStateRole) {
        const Qt::CheckState newState = static_cast<Qt::CheckState>(value.toInt());
        if (newState != m_checkState) {
            m_checkState = newState;
            // Returning true tells TreeModel<> to emit dataChanged for us.
            return true;
        }
    }
    return false;
}

} // namespace LlamaCpp
