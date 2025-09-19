#pragma once

#include <QWidget>
#include <utils/filepath.h>

class QTabBar;
class QTextEdit;
class QToolButton;

namespace LlamaCpp {

class ChatInput : public QWidget
{
    Q_OBJECT
public:
    explicit ChatInput(QWidget *parent = nullptr);

    void setIsGenerating(bool newIsGenerating);
    bool isGenerating() const;
    void setEditingText(const QString &editingText, const QList<QVariantMap> &extra);

    void updateMaximumHeight();
    QList<QVariantMap> getExtraFromAttachedFiles();

signals:
    void sendRequested(const QString &text, const QList<QVariantMap> &extra);
    void editingCancelled();
    void stopRequested();
    void fileDropped(const QStringList &filePaths);
    void pasteLongText(const QString &text);
    void pasteFiles(const QStringList &filePaths);

private:
    void buildUI();
    void updateUI();
    void onSendClicked();
    void onStopClicked();
    void applyStyleSheet();
    void cleanUp();
    void addFilesFromLocalPaths(const QStringList &filePaths);

    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    QTextEdit *m_txt;
    QToolButton *m_sendStopButton;
    QToolButton *m_attachButton;
    QTabBar *m_attachedFilesBar;
    bool m_isGenerating{false};

    QList<QPair<QString, QByteArray>> m_attachedFiles;
};

} // namespace LlamaCpp
