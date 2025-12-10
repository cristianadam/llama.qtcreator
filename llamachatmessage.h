#pragma once

#include <QLabel>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include "llamachatmanager.h"
#include "llamatypes.h"

class QToolButton;
class QPushButton;

namespace LlamaCpp {
class MarkdownLabel;

class ChatMessage : public QWidget
{
    Q_OBJECT
public:
    explicit ChatMessage(const Message &msg,
                         const QVector<qint64> &siblingLeafIds,
                         int siblingIdx,
                         QWidget *parent = nullptr);

    Message &message() { return m_msg; }
    void renderMarkdown(const QString &text, bool forceUpdate = false);

    void messageCompleted(bool completed);
    bool isUser() const;
    void setSiblingLeafIds(const QVector<qint64> &newSiblingLeafIds);

    void setSiblingIdx(int newSiblingIdx);

    QString plainText() const;
    void highlightAllMatches(const QString &query);
    void highlightMatch(int start, int length, bool selected = true);
    void clearHighlight();

    bool isTool() const;

signals:
    void regenerateRequested(const Message &msg);
    void editRequested(const Message &msg);
    void siblingChanged(qint64 siblingId);

private slots:
    void onCopyClicked();
    void onEditClicked();
    void onRegenerateClicked();
    void onPrevSiblingClicked();
    void onNextSiblingClicked();
    void onCopyToClipboard(const QString &verbatimCode, const QString &highlightedCode);
    void onSaveToDisk(const QString &fileName, const QString &verbatimCode);

private:
    void buildUI();
    void updateUI();
    void resizeEvent(QResizeEvent *ev) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

    void applyStyleSheet();
    QString getToolUsageAndResult() const;
    bool haveToolCalls() const;

    Message m_msg;
    QVector<qint64> m_siblingLeafIds;
    int m_siblingIdx{1};
    bool m_isUser;
    bool m_isTool = false;

    // UI
    QLabel *m_bubble{nullptr};
    QToolButton *m_copyButton{nullptr};
    QToolButton *m_editButton{nullptr};
    QToolButton *m_regenButton{nullptr};
    QToolButton *m_prevButton{nullptr};
    QToolButton *m_nextButton{nullptr};
    QToolButton *m_attachedFiles{nullptr};
    QLabel *m_siblingLabel{nullptr};
    MarkdownLabel *m_markdownLabel{nullptr};
    QVBoxLayout *m_mainLayout{nullptr};
    bool m_isToolCall{false};
    bool m_haveToolCalls{false};
};
} // namespace LlamaCpp
