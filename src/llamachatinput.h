#pragma once

#include <QWidget>
#include <utils/filepath.h>

class QMimeData;
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
    //! Attaches all local files referenced by \a mimeData (URLs, project
    //! tree file nodes, folder and project nodes).  Shared by the input's
    //! own drop handler and the conversation area's, so dropping a file
    //! anywhere in the chat attaches it the same way.
    void addFilesFromMimeData(const QMimeData *mimeData);

    //! The message text edit (the ChatEditor moves the focus back to it
    //! when the in‑chat find bar is closed with Escape).
    QTextEdit *textEdit() const { return m_txt; }

signals:
    void sendRequested(const QString &text, const QList<QVariantMap> &extra);
    void editingCancelled();
    void stopRequested();
    //! Escape was pressed while a reply was being generated (as opposed to
    //! the stop button being clicked); the receiver may ask for
    //! confirmation before actually stopping.
    void stopKeyPressed();
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

    void addToHistory(const QString &text);
    void navigateHistory(int direction);
    void exitHistoryBrowsing();
    void showContextMenu(const QPoint &pos);

    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    QTextEdit *m_txt;
    QToolButton *m_sendStopButton;
    QToolButton *m_attachButton;
    QToolButton *m_toolsButton;
    QTabBar *m_attachedFilesBar;
    bool m_isGenerating{false};

    QList<QPair<QString, QByteArray>> m_attachedFiles;

    // Input history: most recent first. m_historyIndex == -1 means "not
    // browsing"; m_historyDraft holds the text the user was composing when
    // browsing started, so it can be restored.
    QStringList m_history;
    int m_historyIndex{-1};
    QString m_historyDraft;
    bool m_applyingHistory{false};
};

} // namespace LlamaCpp
