#pragma once

#include <QLabel>
#include <QVBoxLayout>
#include <QVector>
#include <QWidget>

#include "llamatypes.h"

class QToolButton;
class QCheckBox;

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
    void renderMarkdown(const QString &text);

    void showThought(const QString &content, bool isThinking);

    void messageCompleted(bool completed);
    bool isUser() const;
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
    //void onThoughtToggle(bool checked);

private:
    void buildUI();

    void applyStyleSheet();

    Message m_msg;
    QVector<qint64> m_siblingLeafIds;
    int m_siblingIdx;
    bool m_isUser;

    // UI
    QLabel *m_bubble{nullptr};
    QToolButton *m_copyButton{nullptr};
    QToolButton *m_editButton{nullptr};
    QToolButton *m_regenButton{nullptr};
    QToolButton *m_prevButton{nullptr};
    QToolButton *m_nextButton{nullptr};
    QCheckBox *m_thoughToggle{nullptr};
    QLabel *m_toughtLabel{nullptr};
    MarkdownLabel *m_markdownLabel{nullptr};
    QVBoxLayout *m_mainLayout{nullptr};
};
} // namespace LlamaCpp
