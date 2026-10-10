#include "promptssettingswidget.h"

#include "llamasettings.h"
#include "llamatr.h"

#include <texteditor/fontsettings.h>

#include <QHeaderView>

using namespace Utils;

namespace LlamaCpp {

namespace {

// The tree shows only the first line of a prompt (that is also what the
// "ll" locator displays in its menu); the full text is sent to the model.
QString promptDisplayName(const QString &text)
{
    const QString top = text.section(QLatin1Char('\n'), 0, 0).trimmed();
    return top.isEmpty() ? Tr::tr("(empty)") : top;
}

QString chatGroupId() { return QLatin1String("chat"); }
QString locatorGroupId() { return QLatin1String("locator"); }

} // namespace

PromptsSettingsWidget::PromptsSettingsWidget()
{
    // Add/Remove buttons, in a column to the right of the tree (like the
    // Skills settings page). They manage the locator prompts, which are
    // the children of the "Locator" group row; the chat prompts are fixed.
    m_addButton = new QPushButton(Tr::tr("Add…"), this);
    m_addButton->setFocusPolicy(Qt::ClickFocus);
    m_addButton->setEnabled(false); // only enabled on the locator group and its prompts
    connect(m_addButton, &QPushButton::clicked, this, &PromptsSettingsWidget::addLocatorPrompt);

    m_removeButton = new QPushButton(Tr::tr("Remove"), this);
    m_removeButton->setFocusPolicy(Qt::ClickFocus);
    m_removeButton->setEnabled(false); // only enabled on a locator prompt row
    connect(m_removeButton,
            &QPushButton::clicked,
            this,
            &PromptsSettingsWidget::removeCurrentPrompt);

    m_view = new QTreeView(this);
    m_view->setUniformRowHeights(true);
    m_view->setHeaderHidden(false);

    m_editor = new QPlainTextEdit(this);
    m_editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_editor->setFont(TextEditor::globalFontSettings().data().font());
    m_editor->setPlaceholderText(Tr::tr("Select a prompt to edit it"));

    m_resetButton = new QPushButton(Tr::tr("Reset to Default"), this);
    m_resetButton->setFocusPolicy(Qt::ClickFocus);
    m_resetButton->setEnabled(false); // only enabled when the prompt has a default
    connect(m_resetButton, &QPushButton::clicked, this, [this] {
        PromptItem *item = currentPromptItem();
        if (!item)
            return;
        m_synchronizing = true;
        m_editor->setPlainText(item->defaultText());
        m_synchronizing = false;
        item->setText(item->defaultText());
        item->update(); // refresh the tree label (first line)
        writeSettingsFromModel();
    });

    m_hintLabel = new QLabel(
            Tr::tr("Only the first line of a prompt is shown in the locator menu; the "
                   "full text is sent to the model. \"{selection}\" is replaced "
                   "with the selected text."),
            this);
    m_hintLabel->setVisible(false); // only shown for locator prompts

    // model
    m_model = new TreeModel<>(m_view);
    m_model->setHeader({Tr::tr("Prompt")});
    m_view->setModel(m_model);
    m_view->header()->setSectionResizeMode(0, QHeaderView::Stretch);

    // Layout: the tree with the Add/Remove buttons in a narrow column to
    // its right (grouped with the tree, like the Skills settings page),
    // the editor and the hint label below, spanning the full width.
    using namespace Layouting;
    Column{
        new QLabel(Tr::tr("Prompts:")),
        Row{
            m_view,
            Column{m_addButton, m_removeButton, st},
        },
        m_editor,
        Row{m_resetButton, m_hintLabel, st},
    }.attachTo(this);

    connect(m_view->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            &PromptsSettingsWidget::showPromptEditor);
    connect(m_editor, &QPlainTextEdit::textChanged, this, &PromptsSettingsWidget::editorTextChanged);

    rebuild();
}

void PromptsSettingsWidget::rebuild()
{
    auto &s = settings();

    // Don't let the editor updates below re-enter editorTextChanged()
    // (and write transient settings values).
    m_synchronizing = true;

    m_model->clear();

    auto *chatGroup = new GroupItem(Tr::tr("Chat"), chatGroupId());
    chatGroup->appendChild(
            new PromptItem(Tr::tr("Title"), s.titlePrompt.value(), defaultTitlePrompt(), false));
    chatGroup->appendChild(new PromptItem(Tr::tr("Follow-Up Questions"),
                                          s.followUpPrompt.value(),
                                          defaultFollowUpPrompt(),
                                          false));
    chatGroup->appendChild(new PromptItem(Tr::tr("Compaction"),
                                          s.compactPrompt.value(),
                                          defaultCompactPrompt(),
                                          false));
    m_model->rootItem()->appendChild(chatGroup);

    auto *locatorGroup = new GroupItem(Tr::tr("Locator (\"ll\")"), locatorGroupId());
    const QStringList prompts = s.locatorPrompts.value();
    const QStringList defaults = defaultLocatorPrompts();
    for (int i = 0; i < prompts.size(); ++i) {
        const QString def = i < defaults.size() ? defaults.at(i) : QString();
        locatorGroup->appendChild(new PromptItem(promptDisplayName(prompts.at(i)),
                                                 prompts.at(i),
                                                 def,
                                                 /*nameFromText=*/true));
    }
    m_model->rootItem()->appendChild(locatorGroup);

    m_editor->clear();
    m_resetButton->setEnabled(false);
    m_hintLabel->setVisible(false);
    m_addButton->setEnabled(false);
    m_removeButton->setEnabled(false);
    m_view->expandAll();
    m_synchronizing = false;

    // Select the first prompt so the editor is not empty on opening.
    const int rowCount = m_model->rowCount();
    if (rowCount > 0 && m_model->rowCount(m_model->index(0, 0)) > 0)
        m_view->setCurrentIndex(m_model->index(0, 0, m_model->index(0, 0)));
}

void PromptsSettingsWidget::addLocatorPrompt()
{
    GroupItem *group = groupItem(locatorGroupId());
    if (!group)
        return;
    auto *item = new PromptItem(promptDisplayName(QString()),
                                QString(),
                                /*defaultText=*/QString(),
                                /*nameFromText=*/true);
    group->appendChild(item);
    writeSettingsFromModel();
    m_view->setCurrentIndex(item->index());
}

void PromptsSettingsWidget::removeCurrentPrompt()
{
    const QModelIndex current = m_view->currentIndex();
    if (!isLocatorPromptRow(current))
        return;
    PromptItem *item = currentPromptItem();
    m_view->setCurrentIndex(QModelIndex());
    if (item)
        m_model->destroyItem(item);
    writeSettingsFromModel();
}

void PromptsSettingsWidget::showPromptEditor(const QModelIndex &current,
                                             const QModelIndex & /*previous*/)
{
    // Add/Remove only apply to the locator prompts: Add on the group row
    // and its children, Remove on a child row only.
    bool inLocatorGroup = false;
    if (current.isValid()) {
        const QModelIndex group = current.parent().isValid() ? current.parent() : current;
        inLocatorGroup = m_model->data(group, GroupItem::GroupIdRole).toString()
                == locatorGroupId();
    }
    m_addButton->setEnabled(inLocatorGroup);
    m_removeButton->setEnabled(isLocatorPromptRow(current));

    PromptItem *item = currentPromptItem();
    if (!item) {
        m_synchronizing = true;
        m_editor->clear();
        m_synchronizing = false;
        m_resetButton->setEnabled(false);
        m_hintLabel->setVisible(false);
        return;
    }

    m_synchronizing = true;
    m_editor->setPlainText(item->text());
    m_synchronizing = false;
    m_resetButton->setEnabled(!item->defaultText().isEmpty());
    m_hintLabel->setVisible(isLocatorPromptRow(current));
}

void PromptsSettingsWidget::editorTextChanged()
{
    if (m_synchronizing)
        return;
    PromptItem *item = currentPromptItem();
    if (!item)
        return;
    const QString text = m_editor->toPlainText();
    if (item->text() == text)
        return;
    item->setText(text);
    item->update(); // refresh the tree label (first line)
    writeSettingsFromModel();
}

void PromptsSettingsWidget::writeSettingsFromModel()
{
    auto &s = settings();
    QStringList locator;
    const int groupCount = m_model->rowCount();
    for (int row = 0; row < groupCount; ++row) {
        const QModelIndex groupIdx = m_model->index(row, 0);
        GroupItem *group = static_cast<GroupItem *>(m_model->itemForIndexAtLevel<1>(groupIdx));
        if (!group)
            continue;
        const int childCount = m_model->rowCount(groupIdx);
        for (int c = 0; c < childCount; ++c) {
            PromptItem *prompt = static_cast<PromptItem *>(
                    m_model->nonRootItemForIndex(m_model->index(c, 0, groupIdx)));
            if (!prompt)
                continue;
            if (group->groupId() == chatGroupId()) {
                // The chat prompts are fixed: the first child is the title
                // prompt, the second the follow-up prompt, the third the
                // compaction prompt.
                if (c == 0)
                    s.titlePrompt.setValue(prompt->text());
                else if (c == 1)
                    s.followUpPrompt.setValue(prompt->text());
                else
                    s.compactPrompt.setValue(prompt->text());
            } else {
                locator << prompt->text();
            }
        }
    }
    s.locatorPrompts.setValue(locator);
}

PromptsSettingsWidget::GroupItem *PromptsSettingsWidget::groupItem(const QString &groupId) const
{
    const int groupCount = m_model->rowCount();
    for (int row = 0; row < groupCount; ++row) {
        GroupItem *group = static_cast<GroupItem *>(m_model->itemForIndexAtLevel<1>(m_model->index(row, 0)));
        if (group && group->groupId() == groupId)
            return group;
    }
    return nullptr;
}

PromptsSettingsWidget::PromptItem *PromptsSettingsWidget::currentPromptItem() const
{
    const QModelIndex index = m_view->currentIndex();
    if (!index.isValid() || !index.parent().isValid())
        return nullptr;
    return static_cast<PromptItem *>(m_model->nonRootItemForIndex(index));
}

bool PromptsSettingsWidget::isLocatorPromptRow(const QModelIndex &index) const
{
    if (!index.isValid() || !index.parent().isValid())
        return false;
    return m_model->data(index.parent(), GroupItem::GroupIdRole).toString() == locatorGroupId();
}

void PromptsSettingsWidget::apply()
{
    // Ensure the latest UI state is persisted.
    writeSettingsFromModel();
    // The settings object already knows its values, we just need to write
    // them to disk.
    settings().writeSettings();
}

void PromptsSettingsWidget::cancel()
{
    // Re‑load the stored values – this discards any UI changes.
    settings().readSettings();
    rebuild();
}

/* --------------------------------------------------------------- GroupItem */

PromptsSettingsWidget::GroupItem::GroupItem(const QString &displayName, const QString &groupId)
    : m_displayName(displayName)
    , m_groupId(groupId)
{
}

QVariant PromptsSettingsWidget::GroupItem::data(int column, int role) const
{
    if (column == 0) {
        if (role == Qt::DisplayRole)
            return m_displayName;
        if (role == GroupIdRole)
            return m_groupId;
    }
    return QVariant();
}

Qt::ItemFlags PromptsSettingsWidget::GroupItem::flags(int /*column*/) const
{
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

/* --------------------------------------------------------------- PromptItem */

PromptsSettingsWidget::PromptItem::PromptItem(const QString &name,
                                              const QString &text,
                                              const QString &defaultText,
                                              bool nameFromText)
    : m_name(name)
    , m_text(text)
    , m_defaultText(defaultText)
    , m_nameFromText(nameFromText)
{
}

QVariant PromptsSettingsWidget::PromptItem::data(int column, int role) const
{
    if (column == 0) {
        if (role == Qt::DisplayRole)
            return m_name;
        if (role == Qt::ToolTipRole)
            return m_text;
        if (role == TextRole)
            return m_text;
        if (role == DefaultTextRole)
            return m_defaultText;
    }
    return QVariant();
}

Qt::ItemFlags PromptsSettingsWidget::PromptItem::flags(int /*column*/) const
{
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

void PromptsSettingsWidget::PromptItem::setText(const QString &text)
{
    if (m_text == text)
        return;
    m_text = text;
    if (m_nameFromText)
        m_name = promptDisplayName(text);
}

} // namespace LlamaCpp
