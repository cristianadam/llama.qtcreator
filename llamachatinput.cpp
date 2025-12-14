#include <QDesktopServices>
#include <QDragEnterEvent>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMimeData>
#include <QTabBar>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolButton>

#include <projectexplorer/projectnodes.h>
#include <utils/algorithm.h>
#include <utils/dropsupport.h>
#include <utils/filepath.h>
#include <utils/fsengine/fileiconprovider.h>

#include "llamachatinput.h"
#include "llamasettings.h"
#include "llamatheme.h"
#include "llamatr.h"

using namespace ProjectExplorer;
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

    m_toolsButton = new QToolButton(this);
    m_toolsButton->setText("O");
    m_toolsButton->setCheckable(true);
    const bool toolsInitiallyEnabled = settings().toolsEnabled();
    m_toolsButton->setChecked(toolsInitiallyEnabled);
    m_toolsButton->setToolTip(toolsInitiallyEnabled
                                  ? Tr::tr("Disable Tools usage")
                                  : Tr::tr("Enable Tools usage"));
    connect(m_toolsButton, &QToolButton::clicked, this, [this](bool checked) {
        settings().toolsEnabled.setValue(checked);
        settings().writeSettings();

        m_toolsButton->setToolTip(checked ? Tr::tr("Disable Tools usage")
                                          : Tr::tr("Enable Tools usage"));
        emit toolsSupportEnabled(checked);
    });

    m_attachButton = new QToolButton(this);
    m_attachButton->setText("G");
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

    btnLayout->addWidget(m_toolsButton);
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
        m_sendStopButton->setText("I");
    } else {
        m_sendStopButton->setToolTip(Tr::tr("Send message to assistant"));
        m_sendStopButton->setText("B");
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
            font-family: heroicons_outline;
            font-size: 18px;
            border-radius: 6px;
            padding: 6px -2px;
        }

        QToolButton:hover {
            background-color: Token_Foreground_Muted;
        }

        QToolButton:checked {
            color: Token_Text_Accent;
        }

        QToolButton:unchecked {
            color: Token_Text_Default;
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
    const auto *dropData = dynamic_cast<const DropMimeData *>(e->mimeData());
    if (dropData)
        e->acceptProposedAction();

    if (e->mimeData()->hasUrls())
        e->acceptProposedAction();
}

static bool isSourceFileType(FileType type)
{
    return type == FileType::Header || type == FileType::Source || type == FileType::Form
           || type == FileType::QML || type == FileType::StateChart || type == FileType::Project;
}

static bool isInterestingFileNode(const FileNode *node)
{
    return node->listInProject() && !node->isGenerated() && isSourceFileType(node->fileType());
}

static void collectFilesFromFolderNode(const FolderNode *folderNode, QStringList &outPaths)
{
    if (!folderNode)
        return;

    // Skip the resource files for now.
    if (folderNode->displayName().endsWith(".qrc"))
        return;

    // Use the existing forEachFileNode method to iterate through all file nodes in this folder
    folderNode->forEachFileNode([&outPaths](FileNode *fileNode) {
        if (isInterestingFileNode(fileNode)) {
            FilePath fp = fileNode->filePath();
            if (fp.isLocal())
                outPaths.append(fp.toFSPathString());
        }
    });

    folderNode->forEachFolderNode([&outPaths](FolderNode *childFolderNode) {
        collectFilesFromFolderNode(childFolderNode, outPaths);
    });
}

static void collectFilesFromProjectNode(const ProjectNode *projectNode, QStringList &outPaths)
{
    if (!projectNode)
        return;

    projectNode->forEachFileNode([&outPaths](FileNode *fileNode) {
        if (isInterestingFileNode(fileNode)) {
            FilePath fp = fileNode->filePath();
            if (fp.isLocal())
                outPaths.append(fp.toFSPathString());
        }
    });

    projectNode->forEachFolderNode(
        [&outPaths](FolderNode *folderNode) { collectFilesFromFolderNode(folderNode, outPaths); });
}

void ChatInput::dropEvent(QDropEvent *e)
{
    QStringList localPaths;

    const auto *dropData = dynamic_cast<const DropMimeData *>(e->mimeData());
    if (dropData) {
        using NodesList = QList<const Node *>;
        NodesList nodes = transform<QList<const Node *>>(dropData->values(), [](const QVariant &v) {
            return v.value<Node *>();
        });

        // Filter to only file nodes (not directories or project nodes)
        NodesList fileNodes = filtered(nodes, [](const Node *n) {
            return n->asFileNode() && isInterestingFileNode(n->asFileNode());
        });

        for (const Node *node : fileNodes) {
            FilePath fp = node->filePath();
            if (fp.isLocal())
                localPaths.append(fp.toFSPathString());
        }

        for (const Node *node : nodes) {
            if (auto projectNode = node->asProjectNode())
                collectFilesFromProjectNode(projectNode, localPaths);
            else if (auto folderNode = node->asFolderNode())
                collectFilesFromFolderNode(folderNode, localPaths);
        }
    } else {
        // Fallback to URL-based handling
        for (const QUrl &url : e->mimeData()->urls()) {
            FilePath fp = FilePath::fromUrl(url);
            if (fp.isLocal())
                localPaths.append(fp.toFSPathString());
        }
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

bool ChatInput::isGenerating() const
{
    return m_isGenerating;
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
