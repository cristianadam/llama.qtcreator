#pragma once

#include <utils/layoutbuilder.h>
#include <utils/treemodel.h>

#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTreeView>
#include <QWidget>

#include <coreplugin/dialogs/ioptionspage.h>

namespace LlamaCpp {

//! The "Prompts" settings page, laid out like the Skills settings page: a
//! tree whose two top-level rows are the special prompt groups ("Chat" and
//! the "ll" locator) and whose children are the individual prompts, with
//! Add/Remove buttons grouped to the right of the tree that manage the
//! locator prompts. Selecting a prompt shows its full text in the editor
//! below; only the first line of a locator prompt is shown in the tree (and
//! in the "ll" locator menu), the full text is what is sent to the model.
class PromptsSettingsWidget : public Core::IOptionsPageWidget
{
    Q_OBJECT
public:
    explicit PromptsSettingsWidget();
    void apply() override;
    void cancel() override;

private:
    class GroupItem;
    class PromptItem;

    void rebuild();
    void addLocatorPrompt();
    void removeCurrentPrompt();
    void showPromptEditor(const QModelIndex &current, const QModelIndex & /*previous*/);
    void editorTextChanged();
    void writeSettingsFromModel();
    GroupItem *groupItem(const QString &groupId) const;
    PromptItem *currentPromptItem() const;
    bool isLocatorPromptRow(const QModelIndex &index) const;

    // UI
    QTreeView *m_view = nullptr;
    Utils::TreeModel<> *m_model = nullptr;
    QPlainTextEdit *m_editor = nullptr;
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QPushButton *m_resetButton = nullptr;
    QLabel *m_hintLabel = nullptr;
    bool m_synchronizing = false; // re-entrancy guard for model <-> editor sync

    // Top-level group row: the "Chat" prompts or the "ll" locator prompts.
    class GroupItem : public Utils::TreeItem
    {
    public:
        static constexpr int GroupIdRole = int(Qt::UserRole) + 1; // "chat" / "locator"

        GroupItem(const QString &displayName, const QString &groupId);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        QString groupId() const { return m_groupId; }

    private:
        QString m_displayName;
        QString m_groupId;
    };

    // A single editable prompt. Locator prompts derive their tree name from
    // the first line of their text; the chat prompts have a fixed name.
    class PromptItem : public Utils::TreeItem
    {
    public:
        static constexpr int TextRole = int(Qt::UserRole) + 1;
        static constexpr int DefaultTextRole = int(Qt::UserRole) + 2;

        PromptItem(const QString &name, const QString &text, const QString &defaultText,
                   bool nameFromText);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        QString text() const { return m_text; }
        QString defaultText() const { return m_defaultText; }
        void setText(const QString &text);

    private:
        QString m_name;
        QString m_text;
        QString m_defaultText;
        bool m_nameFromText;
    };
};

} // namespace LlamaCpp
