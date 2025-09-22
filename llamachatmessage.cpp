#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QFileDialog>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryFile>
#include <QToolButton>
#include <QVBoxLayout>

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/editormanager/ieditorfactory.h>
#include <coreplugin/icore.h>
#include <projectexplorer/buildconfiguration.h>
#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <utils/filepath.h>
#include <utils/fsengine/fileiconprovider.h>

#include "llamachatmessage.h"
#include "llamamarkdownwidget.h"
#include "llamatheme.h"
#include "llamathinkingsectionparser.h"
#include "llamatr.h"

using namespace Core;
using namespace ProjectExplorer;
using namespace Utils;

namespace LlamaCpp {

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
    connect(m_markdownLabel, &MarkdownLabel::copyToClipboard, this, &ChatMessage::onCopyToClipboard);
    connect(m_markdownLabel, &MarkdownLabel::saveToFile, this, &ChatMessage::onSaveToDisk);

    if (ThinkingSectionParser::hasThinkingSection(m_msg.content)) {
        m_thoughtToggle = new QPushButton(this);
        m_thoughtToggle->setText(Tr::tr("Thought Process"));
        m_thoughtToggle->setToolTip(Tr::tr("Click to expand / hide the thought process"));
        m_thoughtToggle->setCheckable(true);

        connect(m_thoughtToggle, &QPushButton::toggled, this, &ChatMessage::onThoughtToggle);
        onThoughtToggle(false);
    } else {
        renderMarkdown(m_msg.content, true);
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

    if (m_isUser && !m_msg.extra.isEmpty()) {
        m_attachedFiles = new QToolButton(this);
        m_attachedFiles->setText("G");
        m_attachedFiles->setToolTip(Tr::tr("Attached files"));
        actionLayout->addWidget(m_attachedFiles);

        QMenu *menu = new QMenu(m_attachedFiles);
        for (const QVariantMap &e : m_msg.extra) {
            if (e.value("type").toString() == "textFile") {
                const QString fileName = e.value("name").toString();
                const QByteArray content = e.value("content").toByteArray();

                FilePath file = FilePath::fromString(fileName);
                const QIcon icon = FileIconProvider::icon(file);
                auto action = menu->addAction(icon, fileName);

                connect(action, &QAction::triggered, this, [file, content](bool /*triggered*/) {
                    QString title = file.fileName();
                    EditorFactories factories = IEditorFactory::preferredEditorTypes(file);
                    if (factories.isEmpty())
                        return;
                    auto editor = EditorManager::openEditorWithContents(factories.first()->id(),
                                                                        &title,
                                                                        content);
                });
                action->setIconVisibleInMenu(true);
            } else if (e.value("type").toString() == "imageFile") {
                const QString fileName = e.value("name").toString();
                const QString base64Url = e.value("base64Url").toString();
                const QByteArray content = QByteArray::fromBase64(
                    base64Url.split(',').last().toUtf8());

                FilePath file = FilePath::fromString(fileName);
                const QIcon icon = FileIconProvider::icon(file);
                auto action = menu->addAction(icon, fileName);

                connect(action, &QAction::triggered, this, [content, fileName](bool /*triggered*/) {
                    QTemporaryFile tmpFile;
                    tmpFile.setFileName(QDir::tempPath() + "/" + fileName);
                    if (tmpFile.open()) {
                        tmpFile.write(content);
                        tmpFile.flush();
                        tmpFile.close();
                        QDesktopServices::openUrl(QUrl::fromLocalFile(tmpFile.fileName()));
                    }
                });
                action->setIconVisibleInMenu(true);
            } else if (e.value("type").toString() == "audioFile") {
                const QString fileName = e.value("name").toString();
                const QString base64Url = e.value("base64Url").toString();
                const QByteArray content = QByteArray::fromBase64(
                    base64Url.split(',').last().toUtf8());

                FilePath file = FilePath::fromString(fileName);
                const QIcon icon = FileIconProvider::icon(file);
                auto action = menu->addAction(icon, fileName);

                connect(action, &QAction::triggered, this, [content, fileName](bool /*triggered*/) {
                    QTemporaryFile tmpFile;
                    tmpFile.setFileName(QDir::tempPath() + "/" + fileName);
                    if (tmpFile.open()) {
                        tmpFile.write(content);
                        tmpFile.flush();
                        tmpFile.close();
                        QDesktopServices::openUrl(QUrl::fromLocalFile(tmpFile.fileName()));
                    }
                });
                action->setIconVisibleInMenu(true);
            }
        }
        m_attachedFiles->setMenu(menu);

        connect(m_attachedFiles, &QToolButton::clicked, m_attachedFiles, &QToolButton::showMenu);
    }

    m_prevButton = new QToolButton(this);
    m_prevButton->setText("C");
    m_prevButton->setToolTip(Tr::tr("Go to previous message"));
    connect(m_prevButton, &QToolButton::clicked, this, &ChatMessage::onPrevSiblingClicked);

    m_siblingLabel = new QLabel(this);

    m_nextButton = new QToolButton(this);
    m_nextButton->setText("D");
    m_nextButton->setToolTip(Tr::tr("Go to next message"));
    connect(m_nextButton, &QToolButton::clicked, this, &ChatMessage::onNextSiblingClicked);
    actionLayout->addWidget(m_prevButton);
    actionLayout->addWidget(m_siblingLabel);
    actionLayout->addWidget(m_nextButton);

    updateUI();

    if (m_isUser) {
        m_editButton = new QToolButton(this);
        m_editButton->setText("H");
        m_editButton->setToolTip(Tr::tr("Edit the message"));
        connect(m_editButton, &QToolButton::clicked, this, &ChatMessage::onEditClicked);
        actionLayout->addWidget(m_editButton);
    } else {
        m_regenButton = new QToolButton(this);
        m_regenButton->setText("A");
        m_regenButton->setToolTip(Tr::tr("Re-generate the answer"));
        connect(m_regenButton, &QToolButton::clicked, this, &ChatMessage::onRegenerateClicked);
        actionLayout->addWidget(m_regenButton);
    }

    m_copyButton = new QToolButton(this);
    m_copyButton->setText("E");
    m_copyButton->setToolTip(Tr::tr("Copy the message to clipboard"));
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
                QString selectedText = label->selectedText();
                // replace the unicode line separator with a newline
                selectedText.replace("\u2028", "\n");
                QGuiApplication::clipboard()->setText(selectedText);
                return true;
            }
        }
    }

    return QWidget::eventFilter(obj, event);
}

void ChatMessage::renderMarkdown(const QString &text, bool forceUpdate)
{
    if (forceUpdate)
        m_markdownLabel->invalidate();

    if (m_thoughtToggle) {
        auto [thinking, message] = ThinkingSectionParser::parseThinkingSection(text);
        if (m_thoughtToggle->isChecked()) {
            m_markdownLabel->setMarkdown(ThinkingSectionParser::formatThinkingContent(thinking)
                                         + "\n\n" + message);
        } else {
            m_markdownLabel->setMarkdown(message);
        }

        static QVector<QChar> chars{u'⠋', u'⠙', u'⠹', u'⠸', u'⠼', u'⠴', u'⠦', u'⠧', u'⠇', u'⠏'};
        // Dividing with 33ms results in 30fps
        m_thoughtToggle->setText(
            message.isEmpty()
                ? Tr::tr("Thinking %1")
                      .arg(chars[(QDateTime::currentMSecsSinceEpoch() / 33) % chars.size()])
                : Tr::tr("Thought Process"));

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

        renderMarkdown(m_msg.content, completed);
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
        QTextBrowser#BubbleUser {
            background: Token_Background_Muted;
            border-radius: 8px;
        }
        QTextBrowser#BubbleAssistant {
            background: Token_Background_Default;
            border-radius: 8px;
        }

        QToolButton {
            border: 1px solid Token_Foreground_Muted;
            font-family: heroicons_outline;
            font-size: 14px;
            border-radius: 6px;
            padding: 4px -2px;
        }
        QToolButton:hover {
            background-color: Token_Foreground_Muted;
        }
        QToolButton::menu-indicator {
            image: none;
        }

        QPushButton {
           border: 1px solid Token_Foreground_Muted;
           border-radius: 6px;
           padding: 4px 4px;
        }

        QPushButton:hover {
            background-color: Token_Foreground_Muted;
        }

        QMenu {
            background-color: Token_Background_Muted;
            border-radius: 8px;
            padding: 4px;
        }

        QMenu::item {
            padding: 2px 2px;
            border-radius: 6px;
        }

        QMenu::item:selected {
            background-color: Token_Foreground_Muted;
        }
    )"));
}

void ChatMessage::setSiblingIdx(int newSiblingIdx)
{
    m_siblingIdx = newSiblingIdx;
}

void ChatMessage::setSiblingLeafIds(const QVector<qint64> &newSiblingLeafIds)
{
    m_siblingLeafIds = newSiblingLeafIds;
    updateUI();
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
    int index = m_siblingIdx - 1; // 1 based
    if (index > 0)
        emit siblingChanged(m_siblingLeafIds[index - 1]);
}

void ChatMessage::onNextSiblingClicked()
{
    int index = m_siblingIdx - 1; // 1 based
    if (index < m_siblingLeafIds.size() - 1)
        emit siblingChanged(m_siblingLeafIds[index + 1]);
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

void ChatMessage::updateUI()
{
    m_siblingLabel->setText(QString("%1/%2").arg(m_siblingIdx).arg(m_siblingLeafIds.size()));
    m_prevButton->setEnabled(m_siblingIdx > 1);
    m_nextButton->setEnabled(m_siblingIdx < m_siblingLeafIds.size());

    m_prevButton->setVisible(m_siblingLeafIds.size() > 1);
    m_siblingLabel->setVisible(m_siblingLeafIds.size() > 1);
    m_nextButton->setVisible(m_siblingLeafIds.size() > 1);
}

} // namespace LlamaCpp
