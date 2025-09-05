#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMimeData>
#include <QTextEdit>
#include <QToolButton>

#include "llamachatinput.h"
#include "llamatheme.h"
#include "llamatr.h"

namespace LlamaCpp {

ChatInput::ChatInput(QWidget *parent)
    : QWidget(parent)
{
    setObjectName("ChatInput");
    setAcceptDrops(true);
    buildUI();
    applyStyleSheet();
}

void ChatInput::buildUI()
{
    auto main = new QHBoxLayout(this);
    main->setContentsMargins(10, 10, 10, 10);

    m_txt = new QTextEdit(this);
    m_txt->setPlaceholderText(Tr::tr("Type a message (Shift+Enter for new line)"));
    m_txt->setAcceptRichText(false);

    m_txt->installEventFilter(this);
    installEventFilter(this);

    QHBoxLayout *btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(0, 0, 0, 0);

    m_attachButton = new QToolButton(this);
    m_attachButton->setIcon(QIcon::fromTheme("mail-attachment"));
    m_attachButton->setToolTip(Tr::tr("Attach file"));
    connect(m_attachButton, &QToolButton::clicked, [this]() {
        QStringList files = QFileDialog::getOpenFileNames(this);
        if (!files.isEmpty())
            emit fileDropped(files);
    });

    m_sendStopButton = new QToolButton(this);
    connect(m_sendStopButton, &QToolButton::clicked, this, [this] {
        if (!m_isGenerating)
            onSendClicked();
        else
            onStopClicked();
    });

    btnLayout->addWidget(m_attachButton);
    btnLayout->addWidget(m_sendStopButton);

    main->addWidget(m_txt);
    main->addLayout(btnLayout);

    updateUI();
}

void ChatInput::updateUI()
{
    if (m_isGenerating) {
        m_sendStopButton->setToolTip(Tr::tr("Stop assistant answer generation"));
        m_sendStopButton->setIcon(QIcon::fromTheme("media-playback-stop"));
    } else {
        m_sendStopButton->setToolTip(Tr::tr("Send message to assistant"));
        m_sendStopButton->setIcon(QIcon::fromTheme("mail-send"));
    }
}

void ChatInput::applyStyleSheet()
{
    setAttribute(Qt::WA_StyledBackground, true);

    setStyleSheet(replaceThemeColorNamesWithRGBNames(R"(
        QWidget#ChatInput {
            background-color: Token_Background_Muted;
            border: 1px solid Token_Foreground_Muted;
            border-radius: 8px;
        }
        QWidget#ChatInput:hover {
            border: 1px solid Token_Foreground_Default;
        }
        QTextEdit {
            background-color: Token_Background_Muted;
            border: 0px
        }

        QToolButton {
            border: 1px solid Token_Foreground_Muted;
            border-radius: 6px;
            padding: 4px 4px;
        }
        QToolButton:hover {
            background-color: Token_Foreground_Muted;
        }
    )"));
}

void ChatInput::onSendClicked()
{
    QString message = m_txt->toPlainText().trimmed();
    if (!message.isEmpty())
        emit sendRequested(message);
    m_txt->clear();
}

void ChatInput::onStopClicked()
{
    emit stopRequested();
}

void ChatInput::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void ChatInput::dropEvent(QDropEvent *e)
{
    QStringList fileList;
    for (const QUrl &url : e->mimeData()->urls())
        fileList << url.toLocalFile();
    emit fileDropped(fileList);
}

bool ChatInput::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::ShortcutOverride) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            emit editingCancelled();
            return true;
        }
    }

    if (obj == m_txt && event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->modifiers() == Qt::NoModifier
            && (keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter)) {
            onSendClicked();
            return true;
        }
    }
    if (obj == this && event->type() == QEvent::FocusIn) {
        m_txt->setFocus();
        return true;
    }

    return QWidget::eventFilter(obj, event);
}

void ChatInput::setIsGenerating(bool newIsGenerating)
{
    m_isGenerating = newIsGenerating;
    updateUI();
}

void ChatInput::setEditingText(const QString &editingText)
{
    m_txt->setText(editingText);
    m_txt->setFocus();
}

} // namespace LlamaCpp
