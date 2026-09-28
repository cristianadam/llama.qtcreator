#pragma once

#include "llamasyntaxhighlighter.h"

#include <utils/layoutbuilder.h>
#include <utils/treemodel.h>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QSyntaxHighlighter>
#include <QTextEdit>
#include <QTreeView>
#include <QWidget>

#include <coreplugin/dialogs/ioptionspage.h>

namespace LlamaCpp {

//! Highlights the Markdown source shown on the Skills settings page, using
//! the editor's "Markdown" syntax definition and the global editor color
//! scheme (the same engine the rendered chat Markdown uses for its code
//! blocks).
class SkillsMarkdownHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit SkillsMarkdownHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;

private:
    SyntaxHighlighter m_engine;
    QTextCharFormat m_defaultFormat;
};

//! The "Skills" settings page, laid out like the CMake tools page: a tree
//! whose top-level rows are the skill directories and whose children are
//! the discovered skills (a check-box, the name and a one-line description
//! each), with Add/Remove buttons grouped to the right of the tree that
//! manage the directories. Selecting a skill expands its details below.
//! Unchecked skills are not advertised to the model in the chat system
//! message.
class SkillsSettingsWidget : public Core::IOptionsPageWidget
{
    Q_OBJECT
public:
    explicit SkillsSettingsWidget();
    void apply();
    void cancel();

private:
    void rescan();
    void removeCurrentDirectory();
    void updateDisabledFromModel();
    void updateModelFromDisabled();
    void syncGroupStates();
    void showSkillDetails(const QModelIndex &current, const QModelIndex & /*previous*/);
    bool isDirectoryRow(const QModelIndex &index) const;

    // Filter model, mirroring the one used by the tools settings page: a
    // skill row matches when its name, its full description or the name of
    // its group (the directory) matches; a group row matches when its own
    // path matches or when at least one of its children matches.
    class SkillsFilterModel : public QSortFilterProxyModel
    {
    public:
        explicit SkillsFilterModel(QObject *parent = nullptr);

    protected:
        bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;
    };

    // UI
    QPushButton *m_addButton = nullptr;
    QPushButton *m_removeButton = nullptr;
    QTreeView *m_view = nullptr;
    Utils::TreeModel<> *m_model = nullptr;
    SkillsFilterModel *m_filterModel = nullptr;
    QTextEdit *m_detailEdit = nullptr;
    SkillsMarkdownHighlighter *m_markdownHighlighter = nullptr;
    QLabel *m_diagnosticsLabel = nullptr;
    bool m_synchronizing = false; // re-entrancy guard for check-box propagation

    // Top-level group row (one per skill directory). Checkable: toggling it
    // checks / unchecks all of its children.
    class GroupItem : public Utils::TreeItem
    {
    public:
        static constexpr int PathRole = int(Qt::UserRole) + 1; // the directory path

        explicit GroupItem(const QString &directoryPath);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        bool setData(int column, const QVariant &value, int role) override;
        QString path() const { return m_path; }

    private:
        QString m_path;
        Qt::CheckState m_checkState = Qt::Unchecked;
    };

    // Helper item that holds a skill and a check-state.
    class SkillItem : public Utils::TreeItem
    {
    public:
        static constexpr int PathRole = int(Qt::UserRole) + 1;     // SKILL.md path
        static constexpr int ContentRole = int(Qt::UserRole) + 2;  // Markdown body
        static constexpr int DescriptionRole = int(Qt::UserRole) + 3;

        SkillItem(const QString &name, const QString &description,
                  const QString &path, const QString &content);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        bool setData(int column, const QVariant &value, int role) override;
        QString name() const { return m_name; }

    private:
        QString m_name;
        QString m_description;
        QString m_path;
        QString m_content;
        Qt::CheckState m_checkState = Qt::Unchecked;
    };
};

} // namespace LlamaCpp
