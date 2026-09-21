#pragma once

#include <utils/aspects.h>
#include <utils/layoutbuilder.h>
#include <utils/treemodel.h>

#include <QCheckBox>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTextEdit>
#include <QTreeView>
#include <QWidget>

#include <coreplugin/dialogs/ioptionspage.h>

namespace LlamaCpp {

class ToolsSettingsWidget : public Core::IOptionsPageWidget
{
    Q_OBJECT
public:
    explicit ToolsSettingsWidget();
    void apply();
    void cancel();

private:
    void fillModel();
    void updateEnabledToolsFromModel();
    void updateModelFromEnabledTools();
    void showToolDefinition(const QModelIndex &current, const QModelIndex & /*previous*/);
    void syncGroupStates();
    void updateRipgrepStatus();

    // Filter model, mirroring the one used by the MIME types settings page.
    // A tool row matches when its name, its full description or the name of its
    // group matches; a group row matches when its own name matches or when at
    // least one of its children matches.
    class ToolsFilterModel : public QSortFilterProxyModel
    {
    public:
        explicit ToolsFilterModel(QObject *parent = nullptr);

    protected:
        bool filterAcceptsRow(int source_row, const QModelIndex &source_parent) const override;
    };

    // UI
    QTreeView *m_view = nullptr;
    Utils::TreeModel<> *m_model = nullptr;
    ToolsFilterModel *m_filterModel = nullptr;
    QTextEdit *m_detailEdit = nullptr;
    QLabel *m_ripgrepLabel = nullptr;
    QPushButton *m_ripgrepButton = nullptr;
    bool m_synchronizing = false; // re-entrancy guard for check-box propagation

    // Top-level group row (e.g. "Internal", "Qt Creator MCP"). Checkable:
    // toggling it checks / unchecks all of its children.
    class GroupItem : public Utils::TreeItem
    {
    public:
        explicit GroupItem(const QString &groupName);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        bool setData(int column, const QVariant &value, int role) override;
        QString name() const { return m_name; }

    private:
        QString m_name;
        Qt::CheckState m_checkState = Qt::Unchecked;
    };

    // Helper item that holds a tool name and a check-state
    class ToolItem : public Utils::TreeItem
    {
    public:
        static constexpr int JsonRole = int(Qt::UserRole) + 1;       // full JSON definition
        static constexpr int DescriptionRole = int(Qt::UserRole) + 2; // full (unelided) description

        explicit ToolItem(const QString &toolName,
                          const QString &description,
                          const QString &json);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        bool setData(int column, const QVariant &value, int role) override;
        QString name() const { return m_name; }

    private:
        QString m_name;
        QString m_description;
        QString m_json;
        Qt::CheckState m_checkState = Qt::Unchecked;
    };
};

} // namespace LlamaCpp
