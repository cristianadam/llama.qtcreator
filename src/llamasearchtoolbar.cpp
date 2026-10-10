#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QWidget>

#include "llamasearchtoolbar.h"
#include "llamatheme.h"
#include "llamatr.h"

namespace LlamaCpp {

SearchToolbar::SearchToolbar(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::Tool | Qt::WindowCloseButtonHint | Qt::WindowStaysOnTopHint
                   | Qt::WindowTitleHint | Qt::CustomizeWindowHint);
    setAttribute(Qt::WA_DeleteOnClose, false);

    setObjectName("SearchToolbar");
    setWindowTitle(Tr::tr("Search"));

    auto toolbarLayout = new QHBoxLayout(this);
    toolbarLayout->setContentsMargins(5, 5, 5, 5);
    toolbarLayout->setSpacing(4);

    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText(Tr::tr("Find ..."));
    m_searchEdit->setObjectName("SearchEdit");
    m_prevSearchBtn = new QPushButton("M", this);
    m_prevSearchBtn->setToolTip(Tr::tr("Previous result"));
    m_nextSearchBtn = new QPushButton("N", this);
    m_nextSearchBtn->setToolTip(Tr::tr("Next result"));

    m_searchEdit->installEventFilter(this);

    m_indexLabel = new QLabel(this);
    m_indexLabel->setText("0/0");

    toolbarLayout->addWidget(m_searchEdit);
    toolbarLayout->addWidget(m_indexLabel);
    toolbarLayout->addWidget(m_prevSearchBtn);
    toolbarLayout->addWidget(m_nextSearchBtn);

    hide();

    connect(m_prevSearchBtn, &QPushButton::clicked, this, &SearchToolbar::onPrevSearchClicked);
    connect(m_nextSearchBtn, &QPushButton::clicked, this, &SearchToolbar::onNextSearchClicked);
    connect(m_searchEdit, &QLineEdit::textChanged, this, &SearchToolbar::onSearchTextChanged);

    applyStyleSheet();
}

void SearchToolbar::setIndexLabel(const QString &label)
{
    m_indexLabel->setText(label);
}

void SearchToolbar::closeEvent(QCloseEvent *event)
{
    m_searchEdit->clear();
    hide();

    event->ignore();
    emit onCloseEvent();
}

bool SearchToolbar::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            m_searchEdit->clear();
            hide();

            return true;
        }
    }

    if (obj == this && event->type() == QEvent::FocusIn) {
        m_searchEdit->setFocus();
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

void SearchToolbar::applyStyleSheet()
{
    setAttribute(Qt::WA_StyledBackground, true);
    setStyleSheet(replaceThemeColorNamesWithRGBNames(R"(
        QWidget#SearchEdit {
            background-color: Token_Background_Muted;
            border: 1px solid Token_Foreground_Muted;
            border-radius: 8px;
            padding: 4px;
        }
        QWidget#SearchEdit:hover {
            border: 1px solid Token_Foreground_Default;
        }
        QTextEdit {
            background-color: Token_Background_Muted;
            border: 8px
        }

        QLabel {
           color: Token_Text_Muted;
        }

        QPushButton {
            border: 1px solid Token_Foreground_Muted;
            font-family: heroicons_outline;
            font-size: 14px;
            border-radius: 6px;
            padding: 6px 2px;
        }

        QPushButton:hover {
            background-color: Token_Foreground_Muted;
        }
    )"));
}

} // namespace LlamaCpp
