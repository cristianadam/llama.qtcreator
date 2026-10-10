#include "toolsettingswidget.h"
#include "llamasettings.h"
#include "llamatr.h"
#include "llamasyntaxhighlighter.h"
#include "tools/factory.h"
#include "tools/mcpbridge.h"
#include "tools/mxc.h"
#include "tools/ripgrep.h"
#include "tools/tool_utils.h"

#include <QtTaskTree/QTaskTree>
#include <QtTaskTree/qtasktreerunner.h>

#include <texteditor/fontsettings.h>

#include <utils/fancylineedit.h>
#include <utils/hostosinfo.h>
#include <utils/qtcassert.h>

#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>

#include <algorithm>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMessageBox>
#include <QMouseEvent>
#include <QRegularExpression>

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
    // Filter line edit, same pattern as the MIME types settings page.
    auto filterLineEdit = new FancyLineEdit(this);
    filterLineEdit->setObjectName(QStringLiteral("filterLineEdit"));
    filterLineEdit->setFiltering(true);

    m_view = new QTreeView(this);
    m_view->setUniformRowHeights(true);
    m_view->setHeaderHidden(false);
    m_view->viewport()->installEventFilter(this);

    m_detailEdit = new QTextEdit(this);
    m_detailEdit->setReadOnly(true);
    m_detailEdit->setWordWrapMode(QTextOption::NoWrap);
    m_detailEdit->setPlaceholderText(Tr::tr("Select a tool to view its definition"));

    // Show the tool definitions in the editor's fixed font, with JSON
    // syntax highlighting (the same "JSON" definition and color scheme the
    // Qt Creator editor uses).
    m_detailEdit->setFont(TextEditor::globalFontSettings().data().font());
    m_jsonHighlighter = new ToolsJsonHighlighter(m_detailEdit->document());

    // model
    m_model = new TreeModel<>(m_view);
    m_model->setHeader({Tr::tr("Tool"), Tr::tr("Description")});

    m_filterModel = new ToolsFilterModel(m_view);
    m_filterModel->setSourceModel(m_model);
    m_filterModel->setFilterRole(Qt::DisplayRole);
    m_filterModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

    m_view->setModel(m_filterModel);
    m_view->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_view->header()->setSectionResizeMode(1, QHeaderView::Stretch);

    fillModel();

    // The search and find tools run ripgrep, which is not always installed.
    // Offer to download a pinned release, the way the terminal plugin does for
    // its console host.
    m_ripgrepLabel = new QLabel(this);
    m_ripgrepButton = new QPushButton(
        Tr::tr("Download ripgrep %1").arg(Tools::Ripgrep::version()), this);
    m_ripgrepButton->setVisible(false);
    const auto downloader = std::make_shared<QtTaskTree::QSingleTaskTreeRunner>();
    connect(m_ripgrepButton,
            &QPushButton::clicked,
            this,
            [this, downloader] {
                if (downloader->isRunning())
                    return;
                m_ripgrepButton->setEnabled(false);
                downloader->start({Tools::Ripgrep::downloadRecipe()}, {},
                                  [this, downloader](QtTaskTree::DoneWith) {
                                      m_ripgrepButton->setEnabled(true);
                                      updateRipgrepStatus();
                                  });
            });
    auto *ripgrepRow = new QWidget(this);
    auto *ripgrepLayout = new QHBoxLayout(ripgrepRow);
    ripgrepLayout->setContentsMargins(0, 0, 0, 0);
    ripgrepLayout->addWidget(m_ripgrepLabel);
    ripgrepLayout->addStretch();
    ripgrepLayout->addWidget(m_ripgrepButton);
    updateRipgrepStatus();

    // The Windows sandbox runs the command in a Microsoft MXC process
    // container; the pinned wxc-exec.exe runtime can be downloaded here,
    // like ripgrep above (the row is shown on Windows hosts only).
    m_mxcLabel = new QLabel(this);
    m_mxcButton = new QPushButton(
        Tr::tr("Download MXC %1").arg(Tools::Mxc::version()), this);
    m_mxcButton->setVisible(false);
    const auto mxcDownloader = std::make_shared<QtTaskTree::QSingleTaskTreeRunner>();
    connect(m_mxcButton,
            &QPushButton::clicked,
            this,
            [this, mxcDownloader] {
                if (mxcDownloader->isRunning())
                    return;
                m_mxcButton->setEnabled(false);
                mxcDownloader->start({Tools::Mxc::downloadRecipe()}, {},
                                     [this, mxcDownloader](QtTaskTree::DoneWith) {
                                         m_mxcButton->setEnabled(true);
                                         updateMxcStatus();
                                     });
            });
    auto *mxcRow = new QWidget(this);
    auto *mxcLayout = new QHBoxLayout(mxcRow);
    mxcLayout->setContentsMargins(0, 0, 0, 0);
    mxcLayout->addWidget(m_mxcLabel);
    mxcLayout->addStretch();
    mxcLayout->addWidget(m_mxcButton);
    updateMxcStatus();

    // Sandbox for the chat tools (bubblewrap on Linux, sandbox-exec on
    // macOS, a Microsoft MXC process container on Windows).
    m_sandboxCheck = new QCheckBox(settings().sandboxCommands.displayName(), this);
    m_sandboxCheck->setToolTip(settings().sandboxCommands.toolTip());
    m_sandboxCheck->setChecked(settings().sandboxCommands());
    connect(m_sandboxCheck,
            &QCheckBox::toggled,
            this,
            [](bool checked) { settings().sandboxCommands.setValue(checked); });

    // Project instructions (AGENTS.md / CLAUDE.md) appended to the chat
    // system message.
    m_loadInstructionsCheck = new QCheckBox(settings().loadProjectInstructions.displayName(), this);
    m_loadInstructionsCheck->setToolTip(settings().loadProjectInstructions.toolTip());
    m_loadInstructionsCheck->setChecked(settings().loadProjectInstructions());
    connect(m_loadInstructionsCheck,
            &QCheckBox::toggled,
            this,
            [](bool checked) { settings().loadProjectInstructions.setValue(checked); });

    // Runaway tool-loop guard: the maximum number of consecutive tool-only
    // turns (0 = no limit), mirroring the web UI's maxTurns cap.
    auto *maxToolTurnsLabel = new QLabel(settings().maxToolTurns.labelText(), this);
    m_maxToolTurnsSpin = new QSpinBox(this);
    m_maxToolTurnsSpin->setRange(0, 100000);
    m_maxToolTurnsSpin->setValue(settings().maxToolTurns());
    m_maxToolTurnsSpin->setToolTip(settings().maxToolTurns.toolTip());
    maxToolTurnsLabel->setBuddy(m_maxToolTurnsSpin);
    connect(m_maxToolTurnsSpin,
            QOverload<int>::of(&QSpinBox::valueChanged),
            this,
            [](int value) { settings().maxToolTurns.setValue(value); });
    auto *maxToolTurnsRow = new QWidget(this);
    auto *maxToolTurnsLayout = new QHBoxLayout(maxToolTurnsRow);
    maxToolTurnsLayout->setContentsMargins(0, 0, 0, 0);
    maxToolTurnsLayout->addWidget(maxToolTurnsLabel);
    maxToolTurnsLayout->addWidget(m_maxToolTurnsSpin);
    maxToolTurnsLayout->addStretch();

    // MCP server management, laid out like the skill directories on the
    // Skills page: buttons in a column to the right of the tree, acting on
    // the selected server group row (every group except "Internal"; the
    // builtin "Qt Creator" server cannot be edited or removed). Tool names
    // must be unique across servers: the Qt Creator server and servers
    // listed earlier take priority.
    m_addServerButton = new QPushButton(Tr::tr("Add…"), this);
    m_editServerButton = new QPushButton(Tr::tr("Edit"), this);
    m_removeServerButton = new QPushButton(Tr::tr("Remove"), this);
    m_editServerButton->setEnabled(false);
    m_removeServerButton->setEnabled(false);

    connect(m_addServerButton, &QPushButton::clicked, this, &ToolsSettingsWidget::addServer);
    connect(m_editServerButton, &QPushButton::clicked, this, &ToolsSettingsWidget::editServer);
    connect(m_removeServerButton,
            &QPushButton::clicked,
            this,
            &ToolsSettingsWidget::removeServer);

    // layout
    using namespace Layouting;
    Column{
        Row{
            Column{filterLineEdit, m_view},
            Column{m_addServerButton, m_editServerButton, m_removeServerButton, st},
        },
         m_detailEdit,
         ripgrepRow,
         mxcRow,
         m_sandboxCheck,
         m_loadInstructionsCheck,
         maxToolTurnsRow,
     }
        .attachTo(this);

    connect(filterLineEdit,
            &FancyLineEdit::textChanged,
            m_filterModel,
            &ToolsFilterModel::setFilterWildcard);

    // Filtering resets the expanded state of the group rows – re-expand them so
    // the matching tools are directly visible.
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
        updateEnabledToolsFromModel();
    });

    // The tools served by the MCP servers change at runtime (a server
    // connects / disconnects) – refresh the tree on that.
    connect(&McpBridge::instance(),
            &McpBridge::toolsChanged,
            this,
            [this] {
                fillModel();
                updateModelFromEnabledTools();
            });

    // The connection state of the servers changes at runtime (a server
    // starts or stops) – refresh the tree's status on that.
    connect(&McpBridge::instance(), &McpBridge::connectionChanged, this, &ToolsSettingsWidget::fillModel);

    connect(m_view->selectionModel(),
            &QItemSelectionModel::currentChanged,
            this,
            &ToolsSettingsWidget::showToolDefinition);

    // Double-clicking a configurable server group edits it.
    connect(m_view, &QTreeView::doubleClicked, this, [this](const QModelIndex &) {
        if (!configurableServerOf(m_view->model(), m_view->currentIndex()).isEmpty())
            editServer();
    });
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
    QMap<QString, QStringList> mcpToolsByServer;
    for (const QString &toolName : ToolFactory::instance().creatorsList()) {
        const QString server = McpBridge::instance().serverForTool(toolName);
        if (server.isEmpty())
            internalTools << toolName;
        else
            mcpToolsByServer[server] << toolName;
    }

    // "Internal" – the tools implemented by this plugin
    if (!internalTools.isEmpty()) {
        auto *internalGroup = new GroupItem(Tr::tr("Internal"));
        m_model->rootItem()->appendChild(internalGroup);
        for (const QString &toolName : internalTools)
            appendTool(internalGroup, toolName);
    }

    // One group per MCP server, named after the server: the builtin
    // "Qt Creator" server first, then the configured servers in their
    // stored order. Configured servers are shown even while not connected,
    // so a dead server can still be edited / removed from the tree; a
    // server's tools only appear while it is connected.
    QHash<QString, bool> connectedByServer;
    for (const McpBridge::ServerStatus &status : McpBridge::instance().serverStatuses())
        connectedByServer.insert(status.name, status.connected);
    QSet<QString> addedServers;
    const auto appendServerGroup = [&](const QString &serverName, bool configurable) {
        if (addedServers.contains(serverName))
            return;
        addedServers.insert(serverName);
        auto *serverGroup = new GroupItem(serverName,
                                          serverName,
                                          connectedByServer.value(serverName),
                                          configurable);
        m_model->rootItem()->appendChild(serverGroup);
        if (const auto tools = mcpToolsByServer.constFind(serverName); tools != mcpToolsByServer.constEnd())
            for (const QString &toolName : tools.value())
                appendTool(serverGroup, toolName);
    };
    // The builtin server first (not part of the configured list, not
    // editable); shown only while available.
    if (connectedByServer.contains(McpBridge::builtInServerName()))
        appendServerGroup(McpBridge::builtInServerName(), /*configurable=*/false);
    for (const Tools::McpServerConfig &config :
             Tools::McpServerConfig::fromJson(settings().mcpServersJson()))
        appendServerGroup(config.name, /*configurable=*/true);

    // Initialise the check-states from the stored settings
    updateModelFromEnabledTools();
    m_view->expandAll();
}

void ToolsSettingsWidget::updateRipgrepStatus()
{
    const Utils::FilePath rg = Tools::Ripgrep::resolvedPath();
    if (rg.isEmpty()) {
        m_ripgrepLabel->setText(Tr::tr("The search and find tools use ripgrep, which is not "
                                       "installed on this system."));
        m_ripgrepButton->setVisible(true);
    } else {
        m_ripgrepLabel->setText(Tr::tr("ripgrep: %1").arg(rg.toUserOutput()));
        m_ripgrepButton->setVisible(false);
    }
}

void ToolsSettingsWidget::updateMxcStatus()
{
    if (!HostOsInfo::isWindowsHost()) {
        m_mxcLabel->setVisible(false);
        m_mxcButton->setVisible(false);
        return;
    }
    m_mxcLabel->setVisible(true);
    const Utils::FilePath mxc = Tools::Mxc::resolvedPath();
    if (mxc.isEmpty()) {
        m_mxcLabel->setText(Tr::tr("The sandboxed bash tool on Windows uses the Microsoft "
                                   "MXC runtime (wxc-exec), which is not installed on this "
                                   "system."));
        m_mxcButton->setVisible(Tools::Mxc::isSupportedPlatform());
    } else {
        m_mxcLabel->setText(Tr::tr("Windows sandbox runtime: %1").arg(mxc.toUserOutput()));
        m_mxcButton->setVisible(false);
    }
}

void ToolsSettingsWidget::showToolDefinition(const QModelIndex &current,
                                             const QModelIndex & /*previous*/)
{
    // The Edit / Remove buttons only act on a configurable server group row.
    m_editServerButton->setEnabled(false);
    m_removeServerButton->setEnabled(false);

    if (!current.isValid()) {
        m_detailEdit->clear();
        return;
    }

    // A top-level row is a group row; the MCP server groups show the
    // server's connection details.
    if (!current.parent().isValid()) {
        const QString serverName = current.data(GroupItem::ServerNameRole).toString();
        if (serverName.isEmpty()) {
            m_detailEdit->clear(); // "Internal" – nothing to show
            return;
        }

        const bool configurable = current.data(GroupItem::ConfigurableRole).toBool();
        m_editServerButton->setEnabled(configurable);
        m_removeServerButton->setEnabled(configurable);

        QStringList parts;
        parts << Tr::tr("Name: %1").arg(serverName);

        const Tools::McpServerConfig *config = nullptr;
        const QVector<Tools::McpServerConfig> servers =
                Tools::McpServerConfig::fromJson(settings().mcpServersJson());
        for (const Tools::McpServerConfig &candidate : servers)
            if (candidate.name == serverName) {
                config = &candidate;
                break;
            }

        if (config) {
            parts << Tr::tr("URL: %1").arg(config->url.toString());
            parts << Tr::tr("Headers: %1")
                       .arg(config->headers.isEmpty() ? Tr::tr("(none)")
                                                      : config->headers.join(", "));
            parts << QString();
            parts << Tr::tr("The tools served by this server are listed below; check a "
                            "tool to enable it for the chat.");
        } else {
            // The builtin server (not part of the configured list).
            for (const McpBridge::ServerStatus &status : McpBridge::instance().serverStatuses())
                if (status.name == serverName && status.url.isValid())
                    parts << Tr::tr("URL: %1").arg(status.url.toString());
            parts << QString();
            parts << Tr::tr("The builtin Qt Creator MCP server, managed by Qt Creator "
                            "itself (Tools → MCP in the Qt Creator settings).");
        }

        m_detailEdit->setPlainText(parts.join(QLatin1Char('\n')));
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
    QStringList enabledMcp;
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
                // MCP tools are disabled by default – only the checked ones are
                // stored (in the enabled list).
                if (checked)
                    enabledMcp << name;
            } else if (checked) {
                enabled << name;
            }
        }
    }
    // Write back to the global settings object.
    settings().enabledToolsList.setValue(enabled);
    settings().enabledMcpToolsList.setValue(enabledMcp);
}

void ToolsSettingsWidget::updateModelFromEnabledTools()
{
    // effectiveEnabledTools(): tools added by a newer plugin version show
    // up checked, like isToolEnabled() resolves them at run time.
    const QStringList enabled = effectiveEnabledTools();
    const QStringList enabledMcp = settings().enabledMcpToolsList();
    const int groupCount = m_model->rowCount();
    for (int groupRow = 0; groupRow < groupCount; ++groupRow) {
        const QModelIndex groupIdx = m_model->index(groupRow, 0);
        const int childCount = m_model->rowCount(groupIdx);
        for (int row = 0; row < childCount; ++row) {
            const QModelIndex idx = m_model->index(row, 0, groupIdx);
            const QString name = idx.data(Qt::DisplayRole).toString();

            // Local tools are checked when enabled; MCP tools are only checked
            // when the user explicitly enabled them.
            const bool checked = McpBridge::instance().isMcpTool(name)
                                     ? enabledMcp.contains(name)
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

// Column 0 carries the enable check-boxes: a click on such a cell toggles
// the check state and does not reliably change the current index (or emit
// clicked), so the detail pane would only update on the second click. Watch
// the viewport's mouse release directly so the details show on the first
// click in any column.
bool ToolsSettingsWidget::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view->viewport() && event->type() == QEvent::MouseButtonRelease) {
        const auto *mouseEvent = static_cast<QMouseEvent *>(event);
        if (mouseEvent->button() == Qt::LeftButton) {
            const QModelIndex index = m_view->indexAt(mouseEvent->position().toPoint());
            if (index.isValid())
                showToolDefinition(index, QModelIndex());
        }
        return false;
    }
    return Core::IOptionsPageWidget::eventFilter(watched, event);
}

void ToolsSettingsWidget::cancel()
{
    // Re‑load the stored value – this discards any UI changes.
    settings().readSettings(); // reload from .ini
    m_sandboxCheck->setChecked(settings().sandboxCommands());
    m_loadInstructionsCheck->setChecked(settings().loadProjectInstructions());
    m_maxToolTurnsSpin->setValue(settings().maxToolTurns());
    fillModel(); // also drop server groups that were added but not applied
    updateModelFromEnabledTools(); // reflect the stored state in the UI
}

/* ----------------------------------------------- MCP servers management */

QString ToolsSettingsWidget::configurableServerOf(const QAbstractItemModel *model,
                                                  const QModelIndex &index)
{
    if (!index.isValid() || index.parent().isValid())
        return {};
    if (index.data(GroupItem::ConfigurableRole).toBool())
        return index.data(GroupItem::ServerNameRole).toString();
    return {};
}

void ToolsSettingsWidget::addServer()
{
    McpServerDialog dialog(Tools::McpServerConfig{}, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    Tools::McpServerConfig server = dialog.server();

    // The builtin server's name is reserved.
    if (server.name == McpBridge::builtInServerName()) {
        QMessageBox::warning(this,
                             Tr::tr("MCP Server"),
                             Tr::tr("The name \"%1\" is reserved for the builtin Qt "
                                    "Creator MCP server.")
                                     .arg(McpBridge::builtInServerName()));
        return;
    }

    QVector<Tools::McpServerConfig> servers =
            Tools::McpServerConfig::fromJson(settings().mcpServersJson());

    // Adding a name that is already configured replaces that entry.
    int replaceAt = -1;
    for (int i = 0; i < servers.size(); ++i)
        if (servers[i].name == server.name) {
            replaceAt = i;
            break;
        }
    if (replaceAt >= 0)
        servers[replaceAt] = server;
    else
        servers.append(server);

    settings().mcpServersJson.setValue(Tools::McpServerConfig::toJson(servers));
    fillModel();
    selectServerRow(server.name);
}

void ToolsSettingsWidget::editServer()
{
    const QString name =
            configurableServerOf(m_view->model(), m_view->currentIndex());
    if (name.isEmpty())
        return;

    Tools::McpServerConfig server;
    for (const Tools::McpServerConfig &candidate :
             Tools::McpServerConfig::fromJson(settings().mcpServersJson()))
        if (candidate.name == name) {
            server = candidate;
            break;
        }

    McpServerDialog dialog(server, this);
    if (dialog.exec() != QDialog::Accepted)
        return;

    Tools::McpServerConfig edited = dialog.server();

    // The builtin server's name is reserved.
    if (edited.name == McpBridge::builtInServerName()) {
        QMessageBox::warning(this,
                             Tr::tr("MCP Server"),
                             Tr::tr("The name \"%1\" is reserved for the builtin Qt "
                                    "Creator MCP server.")
                                     .arg(McpBridge::builtInServerName()));
        return;
    }

    // Replaced by the (old) row name – editing may also rename the server.
    QVector<Tools::McpServerConfig> updated =
            Tools::McpServerConfig::fromJson(settings().mcpServersJson());
    for (int i = 0; i < updated.size(); ++i)
        if (updated[i].name == name) {
            updated[i] = edited;
            break;
        }

    settings().mcpServersJson.setValue(Tools::McpServerConfig::toJson(updated));
    fillModel();
    selectServerRow(edited.name);
}

void ToolsSettingsWidget::removeServer()
{
    const QString name =
            configurableServerOf(m_view->model(), m_view->currentIndex());
    if (name.isEmpty())
        return;

    QVector<Tools::McpServerConfig> servers =
            Tools::McpServerConfig::fromJson(settings().mcpServersJson());
    servers.erase(std::remove_if(servers.begin(),
                                 servers.end(),
                                 [&name](const Tools::McpServerConfig &s) { return s.name == name; }),
                  servers.end());
    settings().mcpServersJson.setValue(Tools::McpServerConfig::toJson(servers));
    fillModel();
}

void ToolsSettingsWidget::selectServerRow(const QString &serverName)
{
    const QAbstractItemModel *model = m_view->model();
    for (int row = 0; row < model->rowCount(); ++row) {
        const QModelIndex index = model->index(row, 0);
        if (index.data(GroupItem::ServerNameRole).toString() == serverName) {
            m_view->setCurrentIndex(index);
            return;
        }
    }
}

/* ---------------------------------------------------------------- GroupItem */

ToolsSettingsWidget::GroupItem::GroupItem(const QString &groupName,
                                          const QString &serverName,
                                          bool connected,
                                          bool configurable)
    : m_name(groupName)
    , m_serverName(serverName)
    , m_connected(connected)
    , m_configurable(configurable)
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
        if (!m_serverName.isEmpty() && !m_connected)
            return Tr::tr("not connected");
        const int count = childCount();
        if (count == 0)
            return m_serverName.isEmpty() ? QVariant()
                                          : Tr::tr("no tools");
        return count == 1 ? QStringLiteral("1 tool")
                          : QString::number(count) + QStringLiteral(" tools");
    }

    if (role == ServerNameRole)
        return m_serverName;
    if (role == ConfigurableRole)
        return m_configurable;

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

/* ------------------------------------------------------ ToolsFilterModel */

ToolsSettingsWidget::ToolsFilterModel::ToolsFilterModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{
    // All columns are checked; the actual matching is done in
    // filterAcceptsRow() (name, full description and group name are all
    // taken into account there).
    setFilterKeyColumn(-1);
    setFilterCaseSensitivity(Qt::CaseInsensitive);
}

bool ToolsSettingsWidget::ToolsFilterModel::filterAcceptsRow(int source_row,
                                                             const QModelIndex &source_parent) const
{
    const QRegularExpression regex = filterRegularExpression();
    const QModelIndex index = sourceModel()->index(source_row, 0, source_parent);

    if (sourceModel()->hasChildren(index)) {
        // Group row: visible when the group name matches or when at least one
        // of the children does.
        if (regex.match(sourceModel()->data(index, Qt::DisplayRole).toString()).hasMatch())
            return true;
        const int childCount = sourceModel()->rowCount(index);
        for (int row = 0; row < childCount; ++row)
            if (filterAcceptsRow(row, index))
                return true;
        return false;
    }

    // Tool row: match against the name, the full (unelided) description or
    // the name of the group the tool belongs to.
    if (regex.match(sourceModel()->data(index, Qt::DisplayRole).toString()).hasMatch())
        return true;
    QString description = sourceModel()->data(index, ToolItem::DescriptionRole).toString();
    if (description.isEmpty())
        description = sourceModel()->data(index.siblingAtColumn(1), Qt::DisplayRole).toString();
    if (regex.match(description).hasMatch())
        return true;
    if (source_parent.isValid()
        && regex.match(sourceModel()->data(source_parent, Qt::DisplayRole).toString()).hasMatch())
        return true;
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

/* ------------------------------------------ ToolsJsonHighlighter */

ToolsJsonHighlighter::ToolsJsonHighlighter(QTextDocument *document)
    : QSyntaxHighlighter(document)
{
    m_engine.setDefinition(syntaxDefinitionForName(QStringLiteral("JSON")));
    m_defaultFormat =
            TextEditor::globalFontSettings().data().toTextCharFormat(TextEditor::C_TEXT);
}

void ToolsJsonHighlighter::highlightBlock(const QString &text)
{
    // The engine carries inter-line state (e.g. an unterminated string).
    // Reset it at the start of the document so re-highlighting fresh
    // content does not continue a state from the previous content.
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

/* ----------------------------------------------------------- McpServerDialog */

McpServerDialog::McpServerDialog(const Tools::McpServerConfig &server, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(Tr::tr("MCP Server"));

    m_nameEdit = new QLineEdit(server.name, this);
    m_nameEdit->setPlaceholderText(Tr::tr("Display name, e.g. My MCP server"));

    m_urlEdit = new QLineEdit(server.url.toString(), this);
    m_urlEdit->setPlaceholderText(Tr::tr("https://host:port/mcp"));

    m_headersEdit = new QPlainTextEdit(this);
    m_headersEdit->setPlainText(server.headers.join(QLatin1Char('\n')));
    m_headersEdit->setPlaceholderText(Tr::tr("One \"Name: value\" header per line, e.g.\n"
                                             "Authorization: Bearer <token>"));
    m_headersEdit->setMaximumHeight(80);

    m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    // A Form initializer list is flushed as a *single* row (first item =
    // label, the rest in an HBox), so each field gets its own addRow().
    using namespace Layouting;
    Form form;
    form.addRow({new QLabel(Tr::tr("Name:")), m_nameEdit});
    form.addRow({new QLabel(Tr::tr("URL:")), m_urlEdit});
    form.addRow({new QLabel(Tr::tr("Headers:")), m_headersEdit});

    Column{form, m_buttons}.attachTo(this);

    connect(m_nameEdit, &QLineEdit::textChanged, this, &McpServerDialog::updateValidation);
    connect(m_urlEdit, &QLineEdit::textChanged, this, &McpServerDialog::updateValidation);
    updateValidation();
}

Tools::McpServerConfig McpServerDialog::server() const
{
    Tools::McpServerConfig config;
    config.name = m_nameEdit->text().trimmed();
    config.url = QUrl(m_urlEdit->text().trimmed());
    const QStringList lines =
            m_headersEdit->toPlainText().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString &line : lines)
        if (!line.trimmed().isEmpty())
            config.headers << line.trimmed();
    return config;
}

void McpServerDialog::updateValidation()
{
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(server().isValid());
}

} // namespace LlamaCpp
