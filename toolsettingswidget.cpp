#include "toolsettingswidget.h"
#include "llamasettings.h"
#include "llamatr.h"
#include "tools/factory.h"
#include "tools/mcpbridge.h"

#include <utils/qtcassert.h>

#include <QHeaderView>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

using namespace Utils;

namespace LlamaCpp {

namespace {

// The full tool descriptions can be very long (e.g. edit_file) and would blow
// up the list. Show a short summary in the tree; the full description and the
// complete JSON definition are available in the detail pane.
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

ToolsSettingsWidget::ToolsSettingsWidget()
{
    m_view = new QTreeView(this);
    m_view->setUniformRowHeights(true);
    m_view->setHeaderHidden(false);

    m_detailEdit = new QTextEdit(this);
    m_detailEdit->setReadOnly(true);
    m_detailEdit->setWordWrapMode(QTextOption::NoWrap);
    m_detailEdit->setPlaceholderText(Tr::tr("Select a tool to view its definition"));

    // model
    m_model = new TreeModel<>(m_view);
    m_model->setHeader({Tr::tr("Tool"), Tr::tr("Description")});

    m_view->setModel(m_model);
    m_view->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    fillModel();

    // layout
    using namespace Layouting;
    Column{m_view, m_detailEdit}.attachTo(this);

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
        updateEnabledToolsFromModel();
    });

    // The tools served by the Qt Creator MCP server change at runtime
    // (the server connects / disconnects) – refresh the list on that.
    connect(&McpBridge::instance(),
            &McpBridge::toolsChanged,
            this,
            [this] {
                fillModel();
                updateModelFromEnabledTools();
            });

    connect(m_view->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            &ToolsSettingsWidget::showToolDefinition);
}

void ToolsSettingsWidget::fillModel()
{
    m_model->clear();

    auto appendTool = [this](Utils::TreeItem *group, const QString &toolName) {
        std::unique_ptr<Tool> tmp = ToolFactory::instance().create(toolName);
        const QString json = tmp ? tmp->toolDefinition() : QString();

        QString description;
        if (!json.isEmpty()) {
            QJsonParseError err;
            const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
            if (!doc.isNull() && doc.isObject()) {
                // The schema we use is:
                // {
                //   "type": "function",
                //   "function": { "description": "...", ... }
                // }
                const QJsonObject functionObj = doc.object().value(QStringLiteral("function")).toObject();
                if (!functionObj.isEmpty())
                    description = functionObj.value(QStringLiteral("description")).toString();
            }
        }

        if (description.isEmpty())
            description = Tr::tr("No description");

        group->appendChild(new ToolItem(toolName, description, json));
    };

    QStringList internalTools;
    QStringList mcpTools;
    for (const QString &toolName : ToolFactory::instance().creatorsList()) {
        if (McpBridge::instance().isMcpTool(toolName))
            mcpTools << toolName;
        else
            internalTools << toolName;
    }

    // "Internal" – the tools implemented by this plugin
    if (!internalTools.isEmpty()) {
        auto *internalGroup = new GroupItem(Tr::tr("Internal"));
        m_model->rootItem()->appendChild(internalGroup);
        for (const QString &toolName : internalTools)
            appendTool(internalGroup, toolName);
    }

    // "Qt Creator MCP" – the tools served by the Qt Creator MCP server.
    // Only shown while the server is connected.
    if (!mcpTools.isEmpty()) {
        auto *mcpGroup = new GroupItem(Tr::tr("Qt Creator MCP"));
        m_model->rootItem()->appendChild(mcpGroup);
        for (const QString &toolName : mcpTools)
            appendTool(mcpGroup, toolName);
    }

    // Initialise the check-states from the stored settings
    updateModelFromEnabledTools();
    m_view->expandAll();
}

void ToolsSettingsWidget::showToolDefinition(const QModelIndex &current,
                                             const QModelIndex & /*previous*/)
{
    if (!current.isValid()) {
        m_detailEdit->clear();
        return;
    }

    const QString description = current.data(ToolItem::DescriptionRole).toString();
    const QString json = current.data(ToolItem::JsonRole).toString();

    QStringList parts;
    if (!description.isEmpty())
        parts << description;
    if (!json.isEmpty()) {
        if (!parts.isEmpty())
            parts << QString();
        parts << json;
    }
    m_detailEdit->setPlainText(parts.join(QLatin1Char('\n')));
}

/*
 * Recompute the check-state of every group row from the states of its
 * children (Checked / Unchecked / PartiallyChecked).
 */
void ToolsSettingsWidget::syncGroupStates()
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

void ToolsSettingsWidget::updateEnabledToolsFromModel()
{
    // Walk through all tools and collect the names whose check-state is Checked.
    QStringList enabled;
    QStringList disabledMcp;
    const int groupCount = m_model->rowCount();
    for (int groupRow = 0; groupRow < groupCount; ++groupRow) {
        const QModelIndex groupIdx = m_model->index(groupRow, 0);
        const int childCount = m_model->rowCount(groupIdx);
        for (int row = 0; row < childCount; ++row) {
            const QModelIndex idx = m_model->index(row, 0, groupIdx);
            const QString name = idx.data(Qt::DisplayRole).toString();
            const bool checked
                    = static_cast<Qt::CheckState>(idx.data(Qt::CheckStateRole).toInt()) == Qt::Checked;

            if (McpBridge::instance().isMcpTool(name)) {
                // MCP tools are enabled by default – only the unchecked ones are
                // stored (in the disabled list).
                if (!checked)
                    disabledMcp << name;
            } else if (checked) {
                enabled << name;
            }
        }
    }
    // Write back to the global settings object.
    settings().enabledToolsList.setValue(enabled);
    settings().disabledMcpToolsList.setValue(disabledMcp);
}

void ToolsSettingsWidget::updateModelFromEnabledTools()
{
    const QStringList enabled = settings().enabledToolsList();
    const QStringList disabledMcp = settings().disabledMcpToolsList();
    const int groupCount = m_model->rowCount();
    for (int groupRow = 0; groupRow < groupCount; ++groupRow) {
        const QModelIndex groupIdx = m_model->index(groupRow, 0);
        const int childCount = m_model->rowCount(groupIdx);
        for (int row = 0; row < childCount; ++row) {
            const QModelIndex idx = m_model->index(row, 0, groupIdx);
            const QString name = idx.data(Qt::DisplayRole).toString();

            // Local tools are checked when enabled; MCP tools are checked unless
            // the user explicitly disabled them.
            const bool checked = McpBridge::instance().isMcpTool(name)
                                     ? !disabledMcp.contains(name)
                                     : enabled.contains(name);
            m_model->setData(idx, checked ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
        }
    }
    syncGroupStates();
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

/* ---------------------------------------------------------------- GroupItem */

ToolsSettingsWidget::GroupItem::GroupItem(const QString &groupName)
    : m_name(groupName)
{
}

QVariant ToolsSettingsWidget::GroupItem::data(int column, int role) const
{
    if (column == 0) {
        if (role == Qt::DisplayRole)
            return m_name;
        if (role == Qt::CheckStateRole)
            return m_checkState;
        return QVariant();
    }

    if (column == 1 && role == Qt::DisplayRole) {
        const int count = childCount();
        return count == 1 ? QStringLiteral("1 tool")
                          : QString::number(count) + QStringLiteral(" tools");
    }

    return QVariant();
}

Qt::ItemFlags ToolsSettingsWidget::GroupItem::flags(int column) const
{
    if (column == 0)
        return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

/* The actual propagation to the children is done by the widget's
 * dataChanged handler – here we only track the requested state. */
bool ToolsSettingsWidget::GroupItem::setData(int column, const QVariant &value, int role)
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

/* --------------------------------------------------------------- ToolItem */

ToolsSettingsWidget::ToolItem::ToolItem(const QString &toolName,
                                        const QString &description,
                                        const QString &json)
    : m_name(toolName)
    , m_description(description)
    , m_json(json)
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
            return elideForList(m_description);
        return QVariant();
    }

    // Column 1 – description (elided, the full text is in the detail pane)
    if (column == 1 && role == Qt::DisplayRole)
        return elideForList(m_description);

    if (role == Qt::ToolTipRole)
        return elideForList(m_description);
    if (role == DescriptionRole)
        return m_description;
    if (role == JsonRole)
        return m_json;

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

} // namespace LlamaCpp
