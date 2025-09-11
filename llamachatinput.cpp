#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMimeData>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>

#include <utils/filepath.h>
#include <utils/fsengine/fileiconprovider.h>

#include "llamachatinput.h"
#include "llamatheme.h"
#include "llamatr.h"

using namespace Utils;

namespace LlamaCpp {

static bool isImageFile(const QString &fileName)
{
    QString ext = QFileInfo(fileName).suffix().toLower();
    return ext == "png" || ext == "jpg" || ext == "jpeg" || ext == "webp" || ext == "svg";
}

static bool isAudioFile(const QString &fileName)
{
    QString ext = QFileInfo(fileName).suffix().toLower();
    return ext == "wav" || ext == "mp3";
}

static QString mimeNameFromExtension(const QString &ext)
{
    if (ext == "png")
        return "image/png";
    if (ext == "jpg" || ext == "jpeg")
        return "image/jpeg";
    if (ext == "webp")
        return "image/webp";
    if (ext == "svg")
        return "image/svg+xml";
    if (ext == "wav")
        return "audio/wav";
    if (ext == "mp3")
        return "audio/mpeg";
    return "application/octet-stream";
}

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
    auto main = new QVBoxLayout(this);
    main->setContentsMargins(0, 0, 0, 0);

    auto textAndButtonLayout = new QHBoxLayout;
    textAndButtonLayout->setContentsMargins(10, 10, 10, 10);

    m_txt = new QTextEdit(this);
    m_txt->setPlaceholderText(Tr::tr("Type a message (Shift+Enter for new line)"));
    m_txt->setAcceptRichText(false);
    m_txt->setAcceptDrops(false);

    m_txt->installEventFilter(this);
    installEventFilter(this);

    QHBoxLayout *btnLayout = new QHBoxLayout;
    btnLayout->setContentsMargins(0, 0, 0, 0);

    m_attachButton = new QToolButton(this);
    m_attachButton->setIcon(QIcon::fromTheme("mail-attachment"));
    m_attachButton->setToolTip(Tr::tr("Attach file"));
    connect(m_attachButton, &QToolButton::clicked, [this]() {
        const QStringList files = QFileDialog::getOpenFileNames(this);
        if (!files.isEmpty())
            addFilesFromLocalPaths(files);
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

    textAndButtonLayout->addWidget(m_txt);
    textAndButtonLayout->addLayout(btnLayout);

    m_attachedFilesBar = new QTabBar(this);
    m_attachedFilesBar->setAcceptDrops(false);
    m_attachedFilesBar->setVisible(false);
    m_attachedFilesBar->setTabsClosable(true);
    m_attachedFilesBar->setDocumentMode(true);
    m_attachedFilesBar->setExpanding(false);
    m_attachedFilesBar->setDrawBase(false);
    m_attachedFilesBar->setElideMode(Qt::ElideMiddle);

    QSizePolicy sp(QSizePolicy::Preferred, QSizePolicy::Fixed);
    sp.setHorizontalStretch(1);
    sp.setVerticalStretch(0);
    sp.setHeightForWidth(sizePolicy().hasHeightForWidth());
    m_attachedFilesBar->setSizePolicy(sp);

    connect(m_attachedFilesBar, &QTabBar::tabCloseRequested, this, [this](int index) {
        m_attachedFilesBar->removeTab(index);
        m_attachedFiles.removeAt(index);
        if (m_attachedFilesBar->count() == 0) {
            m_attachedFilesBar->setVisible(false);
            updateMaximumHeight();
        }
    });

    main->addLayout(textAndButtonLayout);
    main->addWidget(m_attachedFilesBar);

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

        QTabBar {
            background-color: Token_Background_Muted;
            height: 22px;
            margin-left: 10px;
            margin-right: 10px;
            margin-bottom: 1px;
        }

        QTabBar::tear {
            width: 0px;
        }

        QTabBar::close-button {
            subcontrol-position: right;
        }

        QTabBar::tab {
            background-color: Token_Background_Muted;
            color: Token_Text_Default;

            border: 1px solid Token_Foreground_Muted;
            border-radius: 6px;
            height: 20px;

            min-width: 5ex;

            margin-top: 4px;
            margin-bottom: 4px;
            margin-left: 4px;
        }

        QTabBar::tab:hover {
            background-color: Token_Foreground_Muted;
        }

    )"));
}

void ChatInput::cleanUp()
{
    m_txt->clear();

    while (m_attachedFilesBar->count() > 0) {
        m_attachedFilesBar->removeTab(0);
    }
    m_attachedFilesBar->setVisible(false);

    m_attachedFiles.clear();

    updateMaximumHeight();
}

void ChatInput::onSendClicked()
{
    QString message = m_txt->toPlainText().trimmed();
    if (!message.isEmpty())
        emit sendRequested(message, getExtraFromAttachedFiles());
    cleanUp();
}

void ChatInput::onStopClicked()
{
    emit stopRequested();
}

void ChatInput::addFilesFromLocalPaths(const QStringList &filePaths)
{
    for (const QString &path : filePaths) {
        FilePath localFile = FilePath::fromString(path);
        if (!localFile.isLocal())
            continue; // skip non‑local files

        const QString fileName = localFile.fileName();

        // If a file with the same name already exists, just replace its
        // contents – we don't want duplicate tabs.
        auto existing = std::ranges::find_if(m_attachedFiles, [fileName](const auto &pair) {
            return pair.first == fileName;
        });
        if (existing != m_attachedFiles.end()) {
            existing->second = localFile.fileContents().value_or("");
            continue;
        }

        const QIcon icon = FileIconProvider::icon(localFile);
        m_attachedFilesBar->addTab(icon, localFile.fileName());

        m_attachedFiles.append({localFile.fileName(), localFile.fileContents().value_or("")});
    }

    if (!filePaths.isEmpty()) {
        m_attachedFilesBar->setVisible(true);
        updateMaximumHeight();
    }
}

void ChatInput::dragEnterEvent(QDragEnterEvent *e)
{
    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

void ChatInput::dropEvent(QDropEvent *e)
{
    QStringList localPaths;
    for (const QUrl &url : e->mimeData()->urls()) {
        FilePath fp = FilePath::fromUrl(url);
        if (fp.isLocal())
            localPaths.append(fp.toFSPathString());
    }

    addFilesFromLocalPaths(localPaths);

    e->acceptProposedAction();
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

void ChatInput::setEditingText(const QString &editingText, const QList<QVariantMap> &extra)
{
    cleanUp();

    for (const QVariantMap &e : extra) {
        if (e.value("type").toString() == "textFile") {
            const QString fileName = e.value("name").toString();
            m_attachedFiles.append({fileName, e.value("content").toByteArray()});

            const QIcon icon = FileIconProvider::icon(FilePath::fromString(fileName));
            m_attachedFilesBar->addTab(icon, fileName);
        } else if (e.value("type").toString() == "imageFile") {
            const QString fileName = e.value("name").toString();
            m_attachedFiles.append({fileName, e.value("content").toByteArray()});

            const QIcon icon = FileIconProvider::icon(FilePath::fromString(fileName));
            m_attachedFilesBar->addTab(icon, fileName);
        } else if (e.value("type").toString() == "audioFile") {
            const QString fileName = e.value("name").toString();
            m_attachedFiles.append({fileName, e.value("content").toByteArray()});

            const QIcon icon = FileIconProvider::icon(FilePath::fromString(fileName));
            m_attachedFilesBar->addTab(icon, fileName);
        }
    }

    m_attachedFilesBar->setVisible(m_attachedFilesBar->count() > 0);
    updateMaximumHeight();

    m_txt->setText(editingText);
    m_txt->setFocus();
}

void ChatInput::updateMaximumHeight()
{
    int maximumHeight = 80;
    if (m_attachedFilesBar->isVisible())
        maximumHeight += 20;
    setMaximumHeight(maximumHeight);
}

QList<QVariantMap> ChatInput::getExtraFromAttachedFiles()
{
    QList<QVariantMap> extraList;
    for (const auto &fileAndContent : std::as_const(m_attachedFiles)) {
        QVariantMap extra;
        const QString fileName = fileAndContent.first;
        const QByteArray content = fileAndContent.second;
        QString ext = QFileInfo(fileName).suffix().toLower();

        if (isImageFile(fileName)) {
            QString base64Url = "data:" + mimeNameFromExtension(ext) + ";base64,"
                                + content.toBase64();
            extra["type"] = "imageFile";
            extra["name"] = fileName;
            extra["base64Url"] = base64Url;
        } else if (isAudioFile(fileName)) {
            QString base64Url = "data:" + mimeNameFromExtension(ext) + ";base64,"
                                + content.toBase64();
            extra["type"] = "audioFile";
            extra["name"] = fileName;
            extra["base64Url"] = base64Url;
        } else {
            extra["type"] = "textFile";
            extra["name"] = fileName;
            extra["content"] = content;
        }
        extraList << extra;
    }
    return extraList;
}

} // namespace LlamaCpp
