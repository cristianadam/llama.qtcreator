#include "modeldownloaddialog.h"

#include "llamaicons.h"
#include "llamasettings.h"
#include "llamaserver.h"
#include "llamatr.h"
#include "modellogos.h"

#include <coreplugin/icore.h>

#include <QDialogButtonBox>
#include <QFont>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QScreen>

namespace LlamaCpp {

// The lucide mark of a modality ("text" is not marked).
static QChar modalityGlyph(const QString &modality)
{
    if (modality == QLatin1String("image"))
        return Lucide::Eye;
    if (modality == QLatin1String("audio"))
        return Lucide::Ear;
    if (modality == QLatin1String("video"))
        return Lucide::Video;
    return QChar();
}

// The modality marks as one pixmap (13 px gray, like the Server page's model list).
static QPixmap modalityMarksPixmap(const QStringList &modalities)
{
    QStringList marked;
    for (const QString &modality : modalities)
        if (!modalityGlyph(modality).isNull())
            marked << modality;
    if (marked.isEmpty())
        return {};
    const int size = 13;
    const int spacing = 4;
    const qreal dpr = QGuiApplication::primaryScreen()
                          ? QGuiApplication::primaryScreen()->devicePixelRatio()
                          : 1.0;
    const int width = marked.size() * size + (marked.size() - 1) * spacing;
    QPixmap pixmap(width * dpr, size * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    for (int i = 0; i < marked.size(); ++i) {
        const QPixmap glyph = lucideGlyph(modalityGlyph(marked[i]), Qt::gray, size)
                                  .pixmap(size, size);
        painter.drawPixmap(i * (size + spacing), 0, glyph);
    }
    return pixmap;
}

ModelDownloadDialog::ModelDownloadDialog(QWidget *parent)
    : QDialog(parent)
    , m_network(new QNetworkAccessManager(this))
{
    setWindowTitle(Tr::tr("Download models"));
    resize(760, 500);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);

    m_filter = new QLineEdit(this);
    m_filter->setClearButtonEnabled(true);
    m_filter->setPlaceholderText(Tr::tr("Filter the catalog..."));

    m_tree = new QTreeWidget(this);
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({Tr::tr("Model"), Tr::tr("Details"), Tr::tr("Status")});
    m_tree->setRootIsDecorated(true);
    m_tree->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_tree->setSelectionMode(QAbstractItemView::SingleSelection);
    m_tree->setAlternatingRowColors(true);
    m_tree->setUniformRowHeights(true);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->header()->setMinimumSectionSize(60);

    m_progress = new QProgressBar(this);
    m_progress->setVisible(false);

    m_downloadButton = new QPushButton(Tr::tr("Download"), this);
    m_downloadButton->setIcon(lucideButtonIcon(Lucide::Download, m_downloadButton));
    m_downloadButton->setToolTip(Tr::tr(
        "The running server downloads the model from Hugging Face into its "
        "model cache and lists it afterwards. The quant is pinned by the "
        "selected build."));
    m_stopButton = new QPushButton(Tr::tr("Stop"), this);
    m_stopButton->setIcon(lucideButtonIcon(Lucide::Square, m_stopButton));
    m_stopButton->setVisible(false);
    QPushButton *closeButton = new QPushButton(Tr::tr("Close"), this);
    closeButton->setDefault(true);

    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    buttons->addWidget(m_downloadButton);
    buttons->addWidget(m_stopButton);
    buttons->addWidget(closeButton);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(m_status);
    layout->addWidget(m_filter);
    layout->addWidget(m_tree, 1);
    layout->addWidget(m_progress);
    layout->addLayout(buttons);

    for (const ServerModel &model : server().models())
        m_installedIds.insert(model.id);

    connect(m_filter, &QLineEdit::textChanged, this, &ModelDownloadDialog::onFilterTextChanged);
    connect(m_tree, &QTreeWidget::currentItemChanged, this, &ModelDownloadDialog::onSelectionChanged);
    connect(m_downloadButton, &QPushButton::clicked, this, &ModelDownloadDialog::downloadSelected);
    connect(m_stopButton, &QPushButton::clicked, this, &ModelDownloadDialog::stopDownload);
    connect(closeButton, &QPushButton::clicked, this, &QDialog::accept);

    // The download runs in LlamaServer and outlives this dialog; an in-flight one is picked up on reopen.
    connect(&server(), &LlamaServer::downloadStarted, this, [this](const QString &modelId) {
        m_downloading = true;
        m_progress->setRange(0, 0);
        m_progress->setVisible(true);
        m_status->setText(Tr::tr("Downloading %1...").arg(modelId));
        setDownloadUi(true);
    });
    connect(&server(), &LlamaServer::downloadProgress, this, &ModelDownloadDialog::onDownloadProgress);
    connect(&server(), &LlamaServer::downloadFinished, this, &ModelDownloadDialog::onDownloadFinished);
    if (server().isDownloading()) {
        m_downloading = true;
        m_progress->setRange(0, 0);
        m_progress->setVisible(true);
        m_status->setText(Tr::tr("Downloading %1...").arg(server().downloadingModel()));
        setDownloadUi(true);
    }

    m_loading = true;
    m_status->setText(Tr::tr("Loading the llama.app model catalog..."));
    QNetworkRequest request(catalogUrl());
    m_catalogReply = m_network->get(request);
    connect(m_catalogReply, &QNetworkReply::finished, this, &ModelDownloadDialog::onCatalogReply);
}

void ModelDownloadDialog::onCatalogReply()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply *>(sender());
    m_catalogReply = nullptr;
    if (!reply)
        return;
    const QByteArray data = reply->readAll();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    m_loading = false;
    if (reply->error() != QNetworkReply::NoError || code < 200 || code >= 300) {
        m_status->setText(Tr::tr(
                             "The model catalog could not be loaded%1. Check the network "
                             "connection and try again.")
                             .arg(code > 0 ? QStringLiteral(" (HTTP %1)").arg(code) : QString()));
        return;
    }
    m_families = parseCatalog(data);
    if (m_families.isEmpty()) {
        m_status->setText(Tr::tr("The model catalog is empty or could not be parsed."));
        return;
    }
    populateTree();
    m_status->setText(Tr::tr(
                         "The models of the llama.app catalog. Downloads run through the "
                         "running server into its model cache."));
}

void ModelDownloadDialog::populateTree()
{
    m_tree->clear();
    for (const CatalogFamily &family : m_families) {
        auto *familyItem = new QTreeWidgetItem(m_tree);
        familyItem->setText(0, family.name);
        // The brand logo like the Server page's list; the family name is tried too (the brand may not carry a keyword).
        QString logoName = family.brand;
        if (modelLogoResource(logoName) == QLatin1String(":/images/llama.svg"))
            logoName = family.name;
        familyItem->setIcon(0, QIcon(modelLogoPixmap(logoName, 16,
                                                     m_tree->palette().color(QPalette::WindowText))));
        QString details = family.brand;
        if (!family.license.isEmpty())
            details += QStringLiteral(" \u00B7 ") + family.license;
        if (family.nonCommercial)
            details += QStringLiteral(" \u00B7 ") + Tr::tr("non-commercial");
        familyItem->setText(1, details);
        if (!family.description.isEmpty())
            familyItem->setToolTip(0, family.description);
        QFont bold = familyItem->font(0);
        bold.setBold(true);
        familyItem->setFont(0, bold);

        for (const CatalogSize &size : family.sizes) {
            auto *sizeItem = new QTreeWidgetItem(familyItem);
            sizeItem->setText(0, size.name);
            QStringList modalities;
            for (const QString &modality : size.inputModalities)
                if (modality != QLatin1String("text"))
                    modalities << modality;
            sizeItem->setText(1, size.params);
            const QPixmap marks = modalityMarksPixmap(size.inputModalities);
            if (!marks.isNull())
                // A QPixmap, not a QIcon: the icon role would clamp it to the 16 px icon size.
                sizeItem->setData(1, Qt::DecorationRole, marks);
            if (!modalities.isEmpty())
                sizeItem->setToolTip(1, modalities.join(QStringLiteral(", ")));

            for (const CatalogBuild &build : size.builds) {
                auto *buildItem = new QTreeWidgetItem(sizeItem);
                const QString id = build.serverModelId();
                buildItem->setText(0, build.quant);
                buildItem->setText(1, build.size);
                buildItem->setData(0, Qt::UserRole, id);
                if (build.goodFirstModel)
                    buildItem->setToolTip(0, Tr::tr(
                                                 "A good first model: small, fast, and "
                                                 "multimodal."));
                const bool installed = m_installedIds.contains(id)
                                       || m_installedIds.contains(build.repo);
                setTreeItemStatus(buildItem,
                                  installed ? Tr::tr("Installed") : Tr::tr("Download"));
            }
        }
        familyItem->setExpanded(true);
    }
    applyFilter(m_filter->text());
}

void ModelDownloadDialog::setTreeItemStatus(QTreeWidgetItem *item, const QString &text)
{
    item->setText(2, text);
}

void ModelDownloadDialog::applyFilter(const QString &text)
{
    const QString needle = text.trimmed().toLower();
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i) {
        auto *family = m_tree->topLevelItem(i);
        const bool familyMatch = needle.isEmpty()
                                 || family->text(0).contains(needle, Qt::CaseInsensitive)
                                 || family->text(1).contains(needle, Qt::CaseInsensitive);
        bool anySize = false;
        for (int j = 0; j < family->childCount(); ++j) {
            auto *size = family->child(j);
            const bool sizeMatch = familyMatch
                                   || size->text(0).contains(needle, Qt::CaseInsensitive);
            bool anyBuild = false;
            for (int k = 0; k < size->childCount(); ++k) {
                auto *build = size->child(k);
                const bool show = sizeMatch
                                  || build->text(0).contains(needle, Qt::CaseInsensitive);
                build->setHidden(!show);
                if (show)
                    anyBuild = true;
            }
            size->setHidden(!anyBuild);
            if (anyBuild)
                anySize = true;
        }
        family->setHidden(!anySize);
    }
}

void ModelDownloadDialog::onFilterTextChanged(const QString &text)
{
    if (!m_loading)
        applyFilter(text);
}

void ModelDownloadDialog::onSelectionChanged()
{
    if (m_downloading)
        return;
    auto *item = m_tree->currentItem();
    const QString id = item ? item->data(0, Qt::UserRole).toString() : QString();
    // Only build rows (the ones carrying a "repo:quant" id) are downloadable,
    // and a build the server already offers is not downloaded again.
    const bool installed = !id.isEmpty()
                           && (m_installedIds.contains(id)
                               || m_installedIds.contains(id.section(QLatin1Char(':'), 0, 0)));
    m_downloadButton->setEnabled(!id.isEmpty() && !installed);
}

void ModelDownloadDialog::setDownloadUi(bool downloading)
{
    m_downloadButton->setEnabled(!downloading);
    m_stopButton->setVisible(downloading);
}

void ModelDownloadDialog::downloadSelected()
{
    auto *item = m_tree->currentItem();
    const QString modelId = item->data(0, Qt::UserRole).toString();
    if (modelId.isEmpty())
        return;
    m_downloadItem = item;
    setTreeItemStatus(item, Tr::tr("Starting..."));
    server().downloadModel(modelId);
}

void ModelDownloadDialog::stopDownload()
{
    server().cancelDownload();
}

void ModelDownloadDialog::onDownloadProgress(qint64 done, qint64 total)
{
    if (!m_downloading)
        return;
    if (total > 0) {
        m_progress->setRange(0, 100);
        m_progress->setValue(int(done * 100 / total));
        m_status->setText(Tr::tr("Downloading: %1 of %2")
                              .arg(formatByteSize(done))
                              .arg(formatByteSize(total)));
    } else {
        m_progress->setRange(0, 0);
        m_status->setText(Tr::tr("Downloading: %1...").arg(formatByteSize(done)));
    }
}

void ModelDownloadDialog::onDownloadFinished(bool success, const QString &message)
{
    if (!m_downloading)
        return;
    m_downloading = false;
    m_progress->setVisible(false);
    m_status->setText(message);
    if (m_downloadItem) {
        if (success) {
            setTreeItemStatus(m_downloadItem, Tr::tr("Installed"));
            m_installedIds.insert(m_downloadItem->data(0, Qt::UserRole).toString());
            server().refreshModels();
        } else {
            setTreeItemStatus(m_downloadItem, Tr::tr("Failed"));
        }
        m_downloadItem = nullptr;
    }
    setDownloadUi(false);
    onSelectionChanged();
}

} // namespace LlamaCpp
