#pragma once

#include "llamacatalog.h"

#include <QDialog>
#include <QSet>

class QNetworkAccessManager;
class QNetworkReply;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace LlamaCpp {

//! Browses the llama.app catalog and downloads a picked build through the running server (the server does the transfer);
//! a download outlives the dialog.
class ModelDownloadDialog : public QDialog
{
    Q_OBJECT
public:
    explicit ModelDownloadDialog(QWidget *parent = nullptr);

private slots:
    void onCatalogReply();
    void onFilterTextChanged(const QString &text);
    void onSelectionChanged();
    void downloadSelected();
    void stopDownload();
    void onDownloadProgress(qint64 done, qint64 total);
    void onDownloadFinished(bool success, const QString &message);

private:
    void populateTree();
    void applyFilter(const QString &text);
    void setTreeItemStatus(QTreeWidgetItem *item, const QString &text);
    void setDownloadUi(bool downloading);

    QNetworkAccessManager *m_network = nullptr;
    QNetworkReply *m_catalogReply = nullptr;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QLabel *m_status = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_downloadButton = nullptr;
    QPushButton *m_stopButton = nullptr;

    QList<CatalogFamily> m_families;
    // The model ids the server already offers (marked installed).
    QSet<QString> m_installedIds;
    bool m_loading = false;
    bool m_downloading = false;
    QTreeWidgetItem *m_downloadItem = nullptr;
};

} // namespace LlamaCpp
