#include <QCheckBox>
#include <QClipboard>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include <coreplugin/icore.h>
#include <projectexplorer/buildconfiguration.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>

#include "llamachatmessage.h"
#include "llamamarkdownwidget.h"
#include "llamatheme.h"
#include "llamatr.h"

using namespace ProjectExplorer;
using namespace Utils;

namespace LlamaCpp {

static const QString thinkingToken("<|channel|>analysis<|message|>");
static const QString endToken("<|end|>");
static qsizetype notfound = -1;

ChatMessage::ChatMessage(const Message &msg,
                         const QVector<qint64> &siblingLeafIds,
                         int siblingIdx,
                         QWidget *parent)
    : QWidget(parent)
    , m_msg(msg)
    , m_siblingLeafIds(siblingLeafIds)
    , m_siblingIdx(siblingIdx)
    , m_isUser(msg.role == "user")
{
    setObjectName("ChatMessage");

    buildUI();
}

void ChatMessage::buildUI()
{
    m_mainLayout = new QVBoxLayout(this);
    m_mainLayout->setContentsMargins(10, 10, 10, 10);

    m_bubble = new QLabel(this);

    m_markdownLabel = new MarkdownLabel(this);
    m_markdownLabel->setWordWrap(true);
    connect(m_markdownLabel, &MarkdownLabel::copyToClipboard, this, &ChatMessage::onCopyToClipboard);
    connect(m_markdownLabel, &MarkdownLabel::saveToFile, this, &ChatMessage::onSaveToDisk);

    if (m_msg.content.indexOf(thinkingToken) != notfound) {
        m_thoughtToggle = new QPushButton(this);
        m_thoughtToggle->setText(Tr::tr("Thought Process"));
        m_thoughtToggle->setCheckable(true);

        connect(m_thoughtToggle, &QPushButton::toggled, this, &ChatMessage::onThoughtToggle);
        onThoughtToggle(false);
    } else {
        renderMarkdown(m_msg.content);
    }

    m_markdownLabel->setObjectName(m_isUser ? "BubbleUser" : "BubbleAssistant");
    m_markdownLabel->setContentsMargins(m_isUser ? QMargins(10, 10, 10, 10) : QMargins(0, 0, 0, 0));
    m_markdownLabel->installEventFilter(this);

    QVBoxLayout *bubbleLayout = new QVBoxLayout;
    bubbleLayout->setContentsMargins(0, 0, 0, 0);
    if (m_thoughtToggle) {
        QHBoxLayout *thoughtLayout = new QHBoxLayout;
        thoughtLayout->setContentsMargins(0, 0, 0, 0);
        thoughtLayout->addWidget(m_thoughtToggle);
        thoughtLayout->addStretch();
        bubbleLayout->addLayout(thoughtLayout);
    }
    QHBoxLayout *labelLayout = new QHBoxLayout;
    labelLayout->setContentsMargins(0, 0, 0, 0);
    if (m_isUser) {
        labelLayout->addStretch();
        labelLayout->addWidget(m_markdownLabel);
    } else {
        labelLayout->addWidget(m_markdownLabel);
        labelLayout->addStretch();
    }
    bubbleLayout->addLayout(labelLayout);
    m_bubble->setLayout(bubbleLayout);

    QHBoxLayout *actionLayout = new QHBoxLayout;
    actionLayout->setAlignment(m_isUser ? Qt::AlignRight : Qt::AlignLeft);
    actionLayout->setContentsMargins(0, 0, 0, 0);

    if (m_siblingLeafIds.size() > 1) {
        m_prevButton = new QToolButton(this);
        m_prevButton->setIcon(QIcon::fromTheme("go-previous"));
        m_prevButton->setEnabled(m_siblingIdx > 0);
        connect(m_prevButton, &QToolButton::clicked, this, &ChatMessage::onPrevSiblingClicked);

        m_nextButton = new QToolButton(this);
        m_nextButton->setIcon(QIcon::fromTheme("go-next"));
        m_nextButton->setEnabled(m_siblingIdx < m_siblingLeafIds.size() - 1);
        connect(m_nextButton, &QToolButton::clicked, this, &ChatMessage::onNextSiblingClicked);

        actionLayout->addWidget(m_prevButton);
        actionLayout->addWidget(m_nextButton);
    }

    if (m_isUser) {
        m_editButton = new QToolButton(this);
        m_editButton->setIcon(QIcon::fromTheme("edit-undo"));
        connect(m_editButton, &QToolButton::clicked, this, &ChatMessage::onEditClicked);
        actionLayout->addWidget(m_editButton);
    } else {
        m_regenButton = new QToolButton(this);
        m_regenButton->setIcon(QIcon::fromTheme("edit-redo"));
        connect(m_regenButton, &QToolButton::clicked, this, &ChatMessage::onRegenerateClicked);
        actionLayout->addWidget(m_regenButton);
    }

    m_copyButton = new QToolButton(this);
    m_copyButton->setIcon(QIcon::fromTheme("edit-copy"));
    connect(m_copyButton, &QToolButton::clicked, this, &ChatMessage::onCopyClicked);
    actionLayout->addWidget(m_copyButton);

    m_mainLayout->addWidget(m_bubble);
    m_mainLayout->addLayout(actionLayout);

    applyStyleSheet();
}

void ChatMessage::resizeEvent(QResizeEvent *ev)
{
    m_markdownLabel->setMinimumWidth(qMin(m_markdownLabel->minimumWidth(), int(width() * 0.9)));
}

bool ChatMessage::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress) {
        QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->modifiers() == Qt::ControlModifier && (keyEvent->key() == Qt::Key_C)) {
            QLabel *label = qobject_cast<QLabel *>(obj);
            if (label) {
                QGuiApplication::clipboard()->setText(label->selectedText());
                return true;
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

void ChatMessage::renderMarkdown(const QString &text)
{
    if (m_thoughtToggle) {
        if (m_thoughtToggle->isChecked()) {
            QString message = text;
            message.replace(thinkingToken, ">");
            auto endIdx = message.indexOf(endToken);
            if (endIdx != notfound) {
                auto newLineIdx = message.indexOf("\n");
                while (newLineIdx < endIdx && newLineIdx != notfound) {
                    message.insert(newLineIdx + 1, ">");
                    newLineIdx = message.indexOf("\n", newLineIdx + 2);
                }
            } else {
                message.replace("\n", "\n>");
            }
            message.replace(endToken, "\n\n");
            m_markdownLabel->setMarkdown(message);
        } else {
            auto endIdx = text.indexOf(endToken);
            if (endIdx != notfound) {
                m_markdownLabel->setMarkdown(text.mid(endIdx + endToken.size()));
            }
        }
    } else {
        m_markdownLabel->setMarkdown(text);
    }
}

void ChatMessage::messageCompleted(bool completed)
{
    // For assistant messages show regenerate and copy buttons
    if (!m_isUser) {
        m_regenButton->setVisible(completed);
        m_copyButton->setVisible(completed);

        renderMarkdown(m_msg.content);
    }
}

bool ChatMessage::isUser() const
{
    return m_isUser;
}

void ChatMessage::applyStyleSheet()
{
    setAttribute(Qt::WA_StyledBackground, true);

    setStyleSheet(replaceThemeColorNamesWithRGBNames(R"(
        QLabel#BubbleUser {
            background: Token_Background_Muted;
            border-radius: 8px;
        }
        QLabel#BubbleAssistant {
        }

        QToolButton {
            border: 1px solid Token_Foreground_Muted;
            border-radius: 6px;
            padding: 2px 2px;
        }
        QToolButton:hover {
            background-color: Token_Foreground_Muted;
        }

        QPushButton {
           border: 1px solid Token_Foreground_Muted;
           border-radius: 6px;
           padding: 4px 4px;
        }

        QPushButton:hover {
            background-color: Token_Foreground_Muted;
        }
    )"));
}

void ChatMessage::onCopyClicked()
{
    QGuiApplication::clipboard()->setText(m_msg.content);
}

void ChatMessage::onEditClicked()
{
    emit editRequested(m_msg);
}

void ChatMessage::onRegenerateClicked()
{
    emit regenerateRequested(m_msg);
}

void ChatMessage::onPrevSiblingClicked()
{
    if (m_siblingIdx > 0)
        emit siblingChanged(m_siblingLeafIds[m_siblingIdx - 1]);
}

void ChatMessage::onNextSiblingClicked()
{
    if (m_siblingIdx < m_siblingLeafIds.size() - 1)
        emit siblingChanged(m_siblingLeafIds[m_siblingIdx + 1]);
}

void ChatMessage::onThoughtToggle(bool /*checked*/)
{
    renderMarkdown(m_msg.content);
}

void ChatMessage::onCopyToClipboard(const QString &verbatimCode, const QString &highlightedCode)
{
    QMimeData *md = new QMimeData;
    md->setText(verbatimCode);
    md->setHtml("<pre><code>" + highlightedCode + "</code></pre>");
    QGuiApplication::clipboard()->setMimeData(md);
}

void ChatMessage::onSaveToDisk(const QString &fileName, const QString &verbatimCode)
{
    FilePath sourceFile;

    auto askOverwrite = [this](const FilePath &filePath) -> bool {
        if (!filePath.exists())
            return true;

        QMessageBox::StandardButton result
            = QMessageBox::question(window(),
                                    Tr::tr("Overwrite File?"),
                                    Tr::tr("The file \"%1\" already exists.\n\n"
                                           "Do you want to overwrite it?")
                                        .arg(filePath.fileName()),
                                    QMessageBox::Yes | QMessageBox::No,
                                    QMessageBox::No);

        return (result == QMessageBox::Yes);
    };

    const Project *project = ProjectManager::startupProject();
    if (project && !fileName.isEmpty()) {
        FilePath projDir = project->projectDirectory();
        sourceFile = projDir.pathAppended(fileName);

        if (!askOverwrite(sourceFile))
            return;
    } else {
        // Below the operating system file save dialog will ask if you want to overwrite
        // an existing file. No need to ask twice.
        QString fileNameWithPath
            = QFileDialog::getSaveFileName(this,
                                           Tr::tr("Save File"),
                                           project ? project->projectDirectory().toFSPathString()
                                                   : fileName,
                                           Tr::tr("All Files (*)"));
        sourceFile = FilePath::fromUserInput(fileNameWithPath);
    }

    sourceFile.writeFileContents(verbatimCode.toUtf8());
}
} // namespace LlamaCpp
