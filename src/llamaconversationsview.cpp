#include <coreplugin/actionmanager/command.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/inavigationwidgetfactory.h>

#include <utils/fancylineedit.h>
#include <utils/navigationtreeview.h>
#include <utils/stylehelper.h>
#include <utils/utilsicons.h>

#include <QFileDialog>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QQueue>
#include <QPushButton>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QStandardPaths>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "llamachatmanager.h"
#include "llamaconstants.h"
#include "llamaconversationsmodel.h"
#include "llamaconversationsview.h"
#include "llamahtmlexporter.h"
#include "llamathinkingsectionparser.h"
#include "llamatr.h"

using namespace Core;
using namespace Utils;

namespace LlamaCpp {

class ConversationsFilterModel : public QSortFilterProxyModel
{
public:
    ConversationsFilterModel(QObject *parent)
        : QSortFilterProxyModel(parent)
    {}

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override
    {
        if (!filterRegularExpression().isValid()) // no filter -> accept all
            return true;

        return rowMatchesRecursively(sourceRow, sourceParent);
    }

private:
    // Accept a row when it matches the filter itself or when any of its
    // descendants matches (so a task conversation is shown even if only its
    // title, not its parent's, contains the search text).
    bool rowMatchesRecursively(int sourceRow, const QModelIndex &sourceParent) const
    {
        const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        const QString txt = sourceModel()->data(idx, Qt::DisplayRole).toString();
        if (txt.contains(filterRegularExpression()))
            return true;

        const int childCount = sourceModel()->rowCount(idx);
        for (int i = 0; i < childCount; ++i)
            if (rowMatchesRecursively(i, idx))
                return true;

        return false; // nothing matched
    }
};

class ConversationsView : public QWidget
{
public:
    explicit ConversationsView();

    void refresh();
    void newConversation();
    QList<QToolButton *> createToolButtons();

protected:
    void showEvent(QShowEvent *) override;

private:
    void resizeColumns();
    void contextMenuAtPoint(const QPoint &point);
    void restoreExpansionAndResize();
    void expansionChanged(const QModelIndex &idx, bool expanded);
    QModelIndex selectedIndex();

    bool eventFilter(QObject *obj, QEvent *event) override;

    bool deleteConversation();
    bool renameConversation();
    bool summarizeConversation();
    bool saveConversationAsMarkdown();
    bool saveConversationAsHtml();

    QAction *m_addAction{nullptr};
    QAction *m_refreshAction{nullptr};

    QSet<QString> m_expandedIds; // ids of the currently expanded branches
    bool m_expansionInitialized{false};

    Utils::NavigationTreeView *m_conversationsView{nullptr};
    ConversationsModel *m_model{nullptr};
    ConversationsFilterModel *m_filterModel{nullptr};
};

class ConversationsViewFactory : public Core::INavigationWidgetFactory
{
public:
    ConversationsViewFactory();

    ConversationsView *view() const;

private:
    Core::NavigationView createWidget() override;

    QPointer<ConversationsView> m_view;
};

ConversationsView::ConversationsView()
    : m_addAction(new QAction(this))
    , m_refreshAction(new QAction(this))
    , m_conversationsView(new NavigationTreeView(this))
    , m_model(new ConversationsModel(this))
    , m_filterModel(new ConversationsFilterModel(this))
{
    m_addAction->setIcon(Utils::Icons::PLUS_TOOLBAR.icon());
    m_addAction->setToolTip(Tr::tr("Creates a new llama.cpp conversation"));
    connect(m_addAction, &QAction::triggered, this, &ConversationsView::newConversation);

    m_refreshAction->setIcon(Utils::Icons::RELOAD_TOOLBAR.icon());
    m_refreshAction->setToolTip(Tr::tr("Refresh"));
    connect(m_refreshAction, &QAction::triggered, this, &ConversationsView::refresh);

    connect(&ChatManager::instance(),
            &ChatManager::conversationCreated,
            this,
            &ConversationsView::refresh);
    connect(&ChatManager::instance(),
            &ChatManager::conversationRenamed,
            this,
            &ConversationsView::refresh);
    connect(&ChatManager::instance(),
            &ChatManager::conversationDeleted,
            this,
            &ConversationsView::refresh);

    m_conversationsView->setHeaderHidden(true);
    setFocus();

    m_filterModel->setSourceModel(m_model);
    m_filterModel->setFilterRole(Qt::DisplayRole);
    m_filterModel->setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_conversationsView->setModel(m_filterModel);

    // Multiple conversations can be selected (Ctrl/Shift+click) so several
    // can be deleted at once; Delete/Backspace then removes them.
    m_conversationsView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_conversationsView->installEventFilter(this);

    auto filterEdit = new FancyLineEdit(this);
    filterEdit->setFiltering(true);
    connect(filterEdit,
            &FancyLineEdit::textChanged,
            m_filterModel,
            QOverload<const QString &>::of(&ConversationsFilterModel::setFilterRegularExpression));
    auto layout = new QVBoxLayout(this);
    layout->addWidget(filterEdit);
    layout->addWidget(m_conversationsView);
    layout->setContentsMargins(0, 2, 0, 0);
    setLayout(layout);

    m_conversationsView->setContextMenuPolicy(Qt::CustomContextMenu);
    m_conversationsView->setEditTriggers(QAbstractItemView::EditKeyPressed);
    connect(m_conversationsView,
            &QAbstractItemView::doubleClicked,
            this,
            [this](const QModelIndex &idx) {
                auto item = m_filterModel->mapToSource(idx);
                if (item.isValid()) {
                    QString name = item.data(Qt::DisplayRole).toString();
                    const QString convId = item.data(ConversationsModel::ConversationIdRole)
                                               .toString();

                    Core::EditorManager::openEditorWithContents(Constants::LLAMACPP_VIEWER_ID,
                                                                &name,
                                                                convId.toUtf8(),
                                                                convId);
                }
            });
    connect(m_conversationsView,
            &QWidget::customContextMenuRequested,
            this,
            &ConversationsView::contextMenuAtPoint);
    // Track the expanded branches so their state survives model resets.
    connect(m_conversationsView, &QTreeView::expanded, this, [this](const QModelIndex &idx) {
        expansionChanged(idx, true);
    });
    connect(m_conversationsView, &QTreeView::collapsed, this, [this](const QModelIndex &idx) {
        expansionChanged(idx, false);
    });
    connect(m_model, &QAbstractItemModel::modelReset, this, &ConversationsView::restoreExpansionAndResize);

    m_conversationsView->selectionModel()->clear();
}

void ConversationsView::refresh()
{
    m_model->setConversations(ChatManager::instance().allConversations());
}

void ConversationsView::newConversation()
{
    QString title("llama.cpp coversation");
    Conversation c = ChatManager::instance().createConversation(title);
    Core::EditorManager::openEditorWithContents(Constants::LLAMACPP_VIEWER_ID,
                                                &title,
                                                c.id.toUtf8(),
                                                c.id);
}

void ConversationsView::showEvent(QShowEvent *)
{
    refresh();
}

bool ConversationsView::eventFilter(QObject *obj, QEvent *event)
{
    // The key events reach the view itself (the viewport proxies its focus
    // to it), and QAbstractItemView ignores Delete/Backspace, so the filter
    // is the hook for the "delete the selection" keyboard mapping.  While
    // an inline editor (rename) has focus the events go to it instead.
    if (obj == m_conversationsView && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);
        if ((keyEvent->key() == Qt::Key_Delete || keyEvent->key() == Qt::Key_Backspace)
                && keyEvent->modifiers() == Qt::NoModifier
                && deleteConversation())
            return true;
    }
    return QWidget::eventFilter(obj, event);
}

QList<QToolButton *> ConversationsView::createToolButtons()
{
    auto addButton = new QToolButton;
    addButton->setDefaultAction(m_addAction);
    addButton->setProperty(StyleHelper::C_NO_ARROW, true);

    auto refreshButton = new QToolButton;
    refreshButton->setDefaultAction(m_refreshAction);
    refreshButton->setProperty(StyleHelper::C_NO_ARROW, true);

    return {addButton, refreshButton};
}

void ConversationsView::resizeColumns()
{
    for (int column = 0, total = m_model->columnCount(); column < total; ++column)
        m_conversationsView->resizeColumnToContents(column);
}

void ConversationsView::contextMenuAtPoint(const QPoint &point)
{
    const QModelIndex filteredIndex = m_conversationsView->indexAt(point);
    if (!filteredIndex.isValid())
        return;

    const QModelIndex index = m_filterModel->mapToSource(filteredIndex);

    QMenu contextMenu;
    contextMenu.addAction(Tr::tr("Rename..."), this, &ConversationsView::renameConversation);
    contextMenu.addAction(Tr::tr("Summarize"), this, &ConversationsView::summarizeConversation);
    contextMenu.addAction(Tr::tr("Delete"), this, &ConversationsView::deleteConversation);
    contextMenu.addAction(Tr::tr("Save as Markdown"),
                          this,
                          &ConversationsView::saveConversationAsMarkdown);
    contextMenu.addAction(Tr::tr("Save as HTML"), this, &ConversationsView::saveConversationAsHtml);

    contextMenu.exec(m_conversationsView->viewport()->mapToGlobal(point));
}

void ConversationsView::expansionChanged(const QModelIndex &idx, bool expanded)
{
    const QString id = idx.data(ConversationsModel::ConversationIdRole).toString();
    if (expanded)
        m_expandedIds.insert(id);
    else
        m_expandedIds.remove(id);
}

void ConversationsView::restoreExpansionAndResize()
{
    if (!m_expansionInitialized) {
        // First refresh: show everything expanded (the expanded() signals
        // fired by expandAll() populate m_expandedIds).
        m_conversationsView->expandAll();
        m_expansionInitialized = true;
    } else {
        // Restore the branches the user had open before the model reset, so a
        // background refresh does not re-expand the whole tree.
        QQueue<QModelIndex> pending;
        for (int row = 0; row < m_filterModel->rowCount(); ++row)
            pending.enqueue(m_filterModel->index(row, 0));
        while (!pending.isEmpty()) {
            const QModelIndex idx = pending.dequeue();
            if (m_expandedIds.contains(idx.data(ConversationsModel::ConversationIdRole).toString()))
                m_conversationsView->expand(idx);
            for (int child = 0; child < m_filterModel->rowCount(idx); ++child)
                pending.enqueue(m_filterModel->index(child, 0, idx));
        }
    }
    resizeColumns();
}

QModelIndex ConversationsView::selectedIndex()
{
    QModelIndexList selected = m_conversationsView->selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return {};
    return m_filterModel->mapToSource(selected.at(0));
}

bool ConversationsView::deleteConversation()
{
    // One source index per selected row (the selection may span several
    // levels of the tree, e.g. a conversation and its task conversations).
    const QModelIndexList selected = m_conversationsView->selectionModel()->selectedRows();
    if (selected.isEmpty())
        return false;

    QList<QModelIndex> rows;
    QSet<QString> ids;
    for (const QModelIndex &idx : selected) {
        const QModelIndex src = m_filterModel->mapToSource(idx);
        if (!src.isValid())
            continue;
        const QString id = src.data(ConversationsModel::ConversationIdRole).toString();
        if (!ids.contains(id)) {
            ids.insert(id);
            rows.append(src);
        }
    }
    if (rows.isEmpty())
        return false;

    // A selected conversation nested under another selected one is not
    // deleted: deleting the ancestor re-parents it to the top level, the
    // same as deleting it on its own.
    QStringList names;
    QStringList toDelete;
    for (int i = 0; i < rows.size(); ++i) {
        bool nested = false;
        for (QModelIndex anc = rows.at(i).parent(); anc.isValid() && !nested;
             anc = anc.parent())
            for (int j = 0; j < rows.size(); ++j)
                if (i != j && anc == rows.at(j)) {
                    nested = true;
                    break;
                }
        if (nested)
            continue;
        toDelete.append(rows.at(i).data(ConversationsModel::ConversationIdRole).toString());
        names.append(rows.at(i).data(Qt::DisplayRole).toString());
    }
    if (toDelete.isEmpty())
        return false;

    QString text;
    if (toDelete.size() == 1) {
        text = Tr::tr("Are you sure you want to delete the conversation:\n%1").arg(names.at(0));
    } else {
        // Cap the list so a huge selection does not fill the screen.
        QStringList listed;
        const int shown = qMin(names.size(), 10);
        for (int i = 0; i < shown; ++i)
            listed << names.at(i);
        if (names.size() > shown)
            listed << Tr::tr("... and %1 more").arg(names.size() - shown);
        text = Tr::tr("Are you sure you want to delete the %1 conversations:\n%2")
                   .arg(toDelete.size())
                   .arg(listed.join(QLatin1Char('\n')));
    }

    QMessageBox *messageBox = new QMessageBox(
        QMessageBox::Warning,
        toDelete.size() == 1 ? Tr::tr("Delete Conversation") : Tr::tr("Delete Conversations"),
        text,
        QMessageBox::Discard | QMessageBox::Cancel,
        window());

    // Change the text and role of the discard button
    auto deleteButton = static_cast<QPushButton *>(messageBox->button(QMessageBox::Discard));
    deleteButton->setText(Tr::tr("Delete"));
    messageBox->addButton(deleteButton, QMessageBox::AcceptRole);
    messageBox->setDefaultButton(deleteButton);

    connect(messageBox,
            &QDialog::accepted,
            this,
            [this, toDelete]() {
                for (const QString &convId : toDelete)
                    ChatManager::instance().deleteConversation(convId);
            });
    messageBox->setAttribute(Qt::WA_DeleteOnClose);
    messageBox->open();

    return true;
}

bool ConversationsView::renameConversation()
{
    QModelIndexList selected = m_conversationsView->selectionModel()->selectedIndexes();
    if (selected.isEmpty())
        return false;

    m_conversationsView->edit(selected.at(0));
    return true;
}

bool ConversationsView::summarizeConversation()
{
    const QModelIndex selected = selectedIndex();
    if (!selected.isValid())
        return false;

    const QString convId = selected.data(ConversationsModel::ConversationIdRole).toString();
    ViewingChat chat = ChatManager::instance().getViewingChat(convId);
    if (chat.messages.isEmpty())
        return false;

    ChatManager::instance()
        .summarizeConversationTitle(convId, chat.messages.last().id, [convId](const QString &title) {
            auto [thinking, shortTitle] = ThinkingSectionParser::parseThinkingSection(title);
            ChatManager::instance().renameConversation(convId, shortTitle);
        });

    return true;
}

bool ConversationsView::saveConversationAsMarkdown()
{
    const QModelIndex selected = selectedIndex();
    if (!selected.isValid())
        return false;

    const QString convId = selected.data(ConversationsModel::ConversationIdRole).toString();
    ViewingChat chat = ChatManager::instance().getViewingChat(convId);

    // Build markdown content (tool calls are exported as <details> blocks)
    QByteArray content;
    for (const Message &msg : chat.messages)
        content.append(ChatManager::messageToMarkdown(msg).toUtf8());

    // Default filename
    QString defaultFileName = QString("%1.md").arg(selected.data().toString());
    FilePath filePath = FilePath::fromUserInput(
        QFileDialog::getSaveFileName(this,
                                     Tr::tr("Save Conversation as Markdown"),
                                     QStandardPaths::writableLocation(
                                         QStandardPaths::DocumentsLocation)
                                         + QDir::separator() + defaultFileName,
                                     "Markdown Files (*.md)"));

    if (filePath.isEmpty())
        return false;

    auto result = filePath.writeFileContents(content);
    if (!result) {
        QMessageBox::warning(this,
                             Tr::tr("Error"),
                             Tr::tr("Cannot write file:\n%1").arg(result.error()));
        return false;
    }

    EditorManager::openEditor(filePath);

    return true;
}

bool ConversationsView::saveConversationAsHtml()
{
    const QModelIndex selected = selectedIndex();
    if (!selected.isValid())
        return false;

    const QString convId = selected.data(ConversationsModel::ConversationIdRole).toString();
    ViewingChat chat = ChatManager::instance().getViewingChat(convId);

    // Self-contained HTML document (styles in a <style> block), matching
    // the chat view's look.
    const QString title = selected.data().toString();
    const QByteArray content = HtmlExporter::conversationHtml(title, chat.messages).toUtf8();

    // Default filename
    QString defaultFileName = QString("%1.html").arg(title);
    FilePath filePath = FilePath::fromUserInput(
        QFileDialog::getSaveFileName(this,
                                     Tr::tr("Save Conversation as HTML"),
                                     QStandardPaths::writableLocation(
                                         QStandardPaths::DocumentsLocation)
                                         + QDir::separator() + defaultFileName,
                                     "HTML Files (*.html)"));

    if (filePath.isEmpty())
        return false;

    auto result = filePath.writeFileContents(content);
    if (!result) {
        QMessageBox::warning(this,
                             Tr::tr("Error"),
                             Tr::tr("Cannot write file:\n%1").arg(result.error()));
        return false;
    }

    EditorManager::openEditor(filePath);

    return true;
}

ConversationsViewFactory::ConversationsViewFactory()
{
    setDisplayName(Tr::tr("llama.cpp Conversations"));
    setPriority(900);
    setId(Constants::LLAMACPP_CONVERSATIONS_VIEW_ID);
}

NavigationView ConversationsViewFactory::createWidget()
{
    m_view = new ConversationsView;
    return {m_view, m_view->createToolButtons()};
}

ConversationsView *ConversationsViewFactory::view() const
{
    return m_view;
}

void setupConversationViewWidgetFactory()
{
    static ConversationsViewFactory theConversationsViewFactory;
}

} // namespace LlamaCpp
