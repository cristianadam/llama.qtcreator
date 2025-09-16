#include <coreplugin/actionmanager/command.h>
#include <coreplugin/documentmanager.h>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/inavigationwidgetfactory.h>

#include <utils/fancylineedit.h>
#include <utils/navigationtreeview.h>
#include <utils/stylehelper.h>
#include <utils/utilsicons.h>

#include <QKeySequence>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QToolButton>
#include <QTreeView>
#include <QVBoxLayout>

#include "llamachatmanager.h"
#include "llamaconstants.h"
#include "llamaconversationsmodel.h"
#include "llamaconversationsview.h"
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

        const QModelIndex idx = sourceModel()->index(sourceRow, 0, sourceParent);
        const QString txt = sourceModel()->data(idx).toString();
        if (txt.contains(filterRegularExpression()))
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
    void expandAndResize();
    QModelIndex selectedIndex();

    bool deleteConversation();
    bool renameConversation();
    bool summarizeConversation();

    QAction *m_addAction{nullptr};
    QAction *m_refreshAction{nullptr};

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
                                                                convId.toUtf8());
                }
            });
    connect(m_conversationsView,
            &QWidget::customContextMenuRequested,
            this,
            &ConversationsView::contextMenuAtPoint);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ConversationsView::expandAndResize);

    m_conversationsView->selectionModel()->clear();
}

void ConversationsView::refresh()
{
    m_model->clear();
    for (const Conversation &c : ChatManager::instance().allConversations()) {
        m_model->addConversation(c);
    }
}

void ConversationsView::newConversation()
{
    QString title("llama.cpp coversation");
    Core::EditorManager::openEditorWithContents(Constants::LLAMACPP_VIEWER_ID, &title);
}

void ConversationsView::showEvent(QShowEvent *)
{
    refresh();
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

    contextMenu.exec(m_conversationsView->viewport()->mapToGlobal(point));
}

void ConversationsView::expandAndResize()
{
    m_conversationsView->expandAll();
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
    const QModelIndex selected = selectedIndex();
    if (!selected.isValid())
        return false;

    const QString name = selected.data(Qt::DisplayRole).toString();
    const QString convId = selected.data(ConversationsModel::ConversationIdRole).toString();

    QMessageBox *messageBox
        = new QMessageBox(QMessageBox::Warning,
                          Tr::tr("Delete Conversation"),
                          Tr::tr("Are you sure you want to delete the conversation:\n%1").arg(name),
                          QMessageBox::Discard | QMessageBox::Cancel,
                          window());

    // Change the text and role of the discard button
    auto deleteButton = static_cast<QPushButton *>(messageBox->button(QMessageBox::Discard));
    deleteButton->setText(Tr::tr("Delete"));
    messageBox->addButton(deleteButton, QMessageBox::AcceptRole);
    messageBox->setDefaultButton(deleteButton);

    connect(messageBox, &QDialog::accepted, this, [this, convId]() {
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
            QString shortTitle = title;
            const QString endToken = "<|end|>";
            auto endIdx = title.indexOf(endToken);
            if (endIdx != -1) {
                shortTitle = title.mid(endIdx + endToken.size());
            }
            ChatManager::instance().renameConversation(convId, shortTitle);
        });

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
