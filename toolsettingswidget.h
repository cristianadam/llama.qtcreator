#pragma once

#include "llamasyntaxhighlighter.h"
#include "tools/mcpserverconfig.h"

#include <utils/aspects.h>
#include <utils/layoutbuilder.h>
#include <utils/treemodel.h>

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QSortFilterProxyModel>
#include <QSyntaxHighlighter>
#include <QTextEdit>
#include <QTreeView>
#include <QWidget>

#include <coreplugin/dialogs/ioptionspage.h>

namespace LlamaCpp {

//! Highlights the tool definitions shown on the Tools settings page, using
//! the editor's "JSON" syntax definition and the global editor color scheme
//! (the same engine the rendered chat Markdown uses for its code blocks).
class ToolsJsonHighlighter : public QSyntaxHighlighter
{
    Q_OBJECT
public:
    explicit ToolsJsonHighlighter(QTextDocument *document);

protected:
    void highlightBlock(const QString &text) override;

private:
    SyntaxHighlighter m_engine;
    QTextCharFormat m_defaultFormat;
};

//! Dialog to add / edit a configured MCP server: a display name, the
//! streamable-HTTP endpoint URL and the request headers (one
//! "Name: value" per line, e.g. an Authorization header for a
//! token-protected server). OK is disabled until the entry is valid.
class McpServerDialog : public QDialog
{
    Q_OBJECT
public:
    explicit McpServerDialog(const Tools::McpServerConfig &server, QWidget *parent = nullptr);
    Tools::McpServerConfig server() const;

private:
    void updateValidation();

    QLineEdit *m_nameEdit = nullptr;
    QLineEdit *m_urlEdit = nullptr;
    QPlainTextEdit *m_headersEdit = nullptr;
    QDialogButtonBox *m_buttons = nullptr;
};

class ToolsSettingsWidget : public Core::IOptionsPageWidget
{
    Q_OBJECT
public:
    explicit ToolsSettingsWidget();
    void apply() override;
    void cancel() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void fillModel();
    void updateEnabledToolsFromModel();
    void updateModelFromEnabledTools();
    void showToolDefinition(const QModelIndex &current, const QModelIndex & /*previous*/);
    void syncGroupStates();
    void updateRipgrepStatus();

    // MCP server management: every group row of the tools tree except
    // "Internal" is an MCP server; Add / Edit / Remove (like the skill
    // directory buttons on the Skills page) act on the selected one. The
    // builtin "Qt Creator" server cannot be edited or removed.
    void addServer();
    void editServer();
    void removeServer();
    void selectServerRow(const QString &serverName);
    // The current selection, when it is a configurable (i.e. user-added)
    // MCP server group row; empty otherwise. Doubles as the Edit / Remove
    // action guard (the buttons are only enabled in that case).
    static QString configurableServerOf(const QAbstractItemModel *model, const QModelIndex &index);

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
    ToolsJsonHighlighter *m_jsonHighlighter = nullptr;
    QLabel *m_ripgrepLabel = nullptr;
    QPushButton *m_ripgrepButton = nullptr;
    QCheckBox *m_sandboxCheck = nullptr;
    QCheckBox *m_loadInstructionsCheck = nullptr;
    // Runaway tool-loop guard: the maximum number of consecutive tool-only
    // turns before the calls are no longer executed (0 = no limit).
    QSpinBox *m_maxToolTurnsSpin = nullptr;
    // MCP server management buttons, in a column to the right of the tools
    // tree (like the skill directory buttons on the Skills page); they act
    // on the selected server group row.
    QPushButton *m_addServerButton = nullptr;
    QPushButton *m_editServerButton = nullptr;
    QPushButton *m_removeServerButton = nullptr;
    bool m_synchronizing = false; // re-entrancy guard for check-box propagation

    // Top-level group row ("Internal" or an MCP server). Checkable:
    // toggling it checks / unchecks all of its children. A group whose
    // server name is non-empty is an MCP server group; configurable ones
    // (all except the builtin "Qt Creator" server) can be edited / removed
    // via the buttons next to the tree.
    class GroupItem : public Utils::TreeItem
    {
    public:
        static constexpr int ServerNameRole = int(Qt::UserRole) + 1;   // empty for "Internal"
        static constexpr int ConfigurableRole = int(Qt::UserRole) + 2; // false for "Internal" and the builtin server

        explicit GroupItem(const QString &groupName,
                           const QString &serverName = QString(),
                           bool connected = false,
                           bool configurable = false);
        QVariant data(int column, int role) const override;
        Qt::ItemFlags flags(int column) const override;
        bool setData(int column, const QVariant &value, int role) override;
        QString name() const { return m_name; }

    private:
        QString m_name;
        QString m_serverName;
        bool m_connected = false;
        bool m_configurable = false;
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
