#include "toolsettingswidget.h"
#include "llamasettings.h"
#include "llamatr.h"
#include "tools/factory.h"

#include <utils/qtcassert.h>

#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

using namespace Utils;

namespace LlamaCpp {

ToolsSettingsWidget::ToolsSettingsWidget()
{
    m_view = new QTreeView(this);
    m_view->setUniformRowHeights(true);
    m_view->setHeaderHidden(false);

    m_detailEdit = new QTextEdit(this);
    m_detailEdit->setReadOnly(true);
    m_detailEdit->setWordWrapMode(QTextOption::NoWrap);
    m_detailEdit->setPlaceholderText(Tr::tr("Select a tool to view its JSON definition"));

    // model
    m_model = new TreeModel<>(m_view);
    m_model->setHeader({Tr::tr("Tool"), Tr::tr("Description")});

    fillModel();
    m_view->setModel(m_model);
    m_view->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    // layout
    using namespace Layouting;
    Column{m_view, m_detailEdit}.attachTo(this);

    // keep model in sync when user toggles a check‑box
    connect(m_model, &TreeModel<>::dataChanged, this, [this] { updateEnabledToolsFromModel(); });

    connect(m_view->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            &ToolsSettingsWidget::showToolDefinition);
}

void ToolsSettingsWidget::fillModel()
{
    m_model->clear();

    const QStringList allTools = ToolFactory::instance().creatorsList();
    for (const QString &toolName : allTools) {
        std::unique_ptr<Tool> tmp = ToolFactory::instance().create(toolName);
        const QString json = tmp ? tmp->toolDefinition() : QString();

        QString description;
        QString tooltip = json; // default tooltip = whole JSON
        if (!json.isEmpty()) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
            if (!doc.isNull() && doc.isObject()) {
                const QJsonObject rootObj = doc.object();
                // The schema we use is:
                // {
                //   "type": "function",
                //   "function": { "description": "...", ... }
                // }
                const QJsonObject functionObj = rootObj.value(QStringLiteral("function")).toObject();
                if (!functionObj.isEmpty()) {
                    description = functionObj.value(QStringLiteral("description")).toString();
                }
            }
        }

        if (description.isEmpty())
            description = Tr::tr("No description");

        // Store both the short description and the full JSON (tooltip).
        m_model->rootItem()->appendChild(new ToolItem(toolName, description, tooltip));
    }

    // Initialise the check‑states from the stored settings
    updateModelFromEnabledTools();
}

void ToolsSettingsWidget::showToolDefinition(const QModelIndex &current,
                                             const QModelIndex & /*previous*/)
{
    if (!current.isValid()) {
        m_detailEdit->clear();
        return;
    }

    // The model stores the full JSON string in the ToolItem (as “tooltip”).
    // We can retrieve it via the custom role we already use – Qt::ToolTipRole.
    // This works even if the tooltip is not shown to the user.
    const QString json = current.data(Qt::ToolTipRole).toString();
    m_detailEdit->setPlainText(json);
}

ToolsSettingsWidget::ToolItem::ToolItem(const QString &toolName,
                                        const QString &description,
                                        const QString &tooltip)
    : m_name(toolName)
    , m_description(description)
    , m_tooltip(tooltip)
{
    // default to unchecked – the UI will later set the correct state
    m_checkState = Qt::Unchecked;
}

QVariant ToolsSettingsWidget::ToolItem::data(int column, int role) const
{
    // Column 0 – name + check‑box
    if (column == 0) {
        if (role == Qt::DisplayRole)
            return m_name;
        if (role == Qt::CheckStateRole)
            return m_checkState;
        if (role == Qt::ToolTipRole)
            return m_tooltip; // show the full JSON as tooltip
        return QVariant();
    }

    // Column 1 – description (read‑only)
    if (column == 1) {
        if (role == Qt::DisplayRole)
            return m_description;
        if (role == Qt::ToolTipRole)
            return m_tooltip; // also show tooltip on the description column
        return QVariant();
    }

    return QVariant();
}

/* flags() – only column 0 is user‑checkable */
Qt::ItemFlags ToolsSettingsWidget::ToolItem::flags(int column) const
{
    if (column == 0)
        return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

/* setData() – handle changes of the check‑state */
bool ToolsSettingsWidget::ToolItem::setData(int column, const QVariant &value, int role)
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

void ToolsSettingsWidget::updateEnabledToolsFromModel()
{
    // Walk through all rows and collect the names whose check‑state is Checked.
    QStringList enabled;
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        const Qt::CheckState cs = static_cast<Qt::CheckState>(idx.data(Qt::CheckStateRole).toInt());
        if (cs == Qt::Checked) {
            const QString name = idx.data(Qt::DisplayRole).toString();
            enabled << name;
        }
    }
    // Write back to the global settings object.
    settings().enabledToolsList.setValue(enabled);
}

void ToolsSettingsWidget::updateModelFromEnabledTools()
{
    const QStringList enabled = settings().enabledToolsList();
    for (int row = 0; row < m_model->rowCount(); ++row) {
        const QModelIndex idx = m_model->index(row, 0);
        const QString name = idx.data(Qt::DisplayRole).toString();
        const Qt::CheckState cs = enabled.contains(name) ? Qt::Checked : Qt::Unchecked;
        m_model->setData(idx, cs, Qt::CheckStateRole);
    }
}

void ToolsSettingsWidget::apply()
{
    // Ensure the latest UI state is persisted.
    updateEnabledToolsFromModel();
    // The settings object already knows its value, we just need to write it to disk.
    settings().writeSettings(); // writes all changed aspects, including enabledTools
}

void ToolsSettingsWidget::cancel()
{
    // Re‑load the stored value – this discards any UI changes.
    settings().readSettings();     // reload from .ini
    updateModelFromEnabledTools(); // reflect the stored state in the UI
}

} // namespace LlamaCpp
