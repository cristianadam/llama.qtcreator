#pragma once

#include <QWidget>

class QTextEdit;
class QToolButton;

namespace LlamaCpp {

class ChatInput : public QWidget
{
    Q_OBJECT
public:
    explicit ChatInput(QWidget *parent = nullptr);

signals:
    void sendRequested(const QString &text);
    void stopRequested();
    void fileDropped(const QStringList &filePaths);
    void pasteLongText(const QString &text);
    void pasteFiles(const QStringList &filePaths);

private:
    void buildUI();
    void onSendClicked();
    void onStopClicked();

    void applyStyleSheet();

    void dragEnterEvent(QDragEnterEvent *e) override;
    void dropEvent(QDropEvent *e) override;

    bool eventFilter(QObject *obj, QEvent *event) override;

    QTextEdit *m_txt;
    QToolButton *m_sendStopButton;
    QToolButton *m_attachButton;
    bool m_isGenerating{false};
};
} // namespace LlamaCpp
