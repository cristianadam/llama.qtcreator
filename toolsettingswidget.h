#pragma once

#include <utils/aspects.h>
#include <utils/layoutbuilder.h>
#include <utils/treemodel.h>

#include <QCheckBox>
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

    // UI
    QTreeView *m_view = nullptr;
    Utils::TreeModel<> *m_model = nullptr;
    QTextEdit *m_detailEdit = nullptr;

    // Helper item that holds a tool name and a check‑state
    class ToolItem : public Utils::TreeItem
    {
    public:
        explicit ToolItem(const QString &toolName,
                          const QString &description,
                          const QString &tooltip);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        bool setData(int column, const QVariant &value, int role) override;
        QString name() const { return m_name; }

    private:
        QString m_name;
        QString m_description;
        QString m_tooltip;
        Qt::CheckState m_checkState = Qt::Unchecked;
    };
};

} // namespace LlamaCpp
