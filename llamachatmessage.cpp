#include <QCheckBox>
#include <QClipboard>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QToolButton>
#include <QVBoxLayout>

#include "llamachatmessage.h"
#include "llamamarkdownwidget.h"

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
    m_mainLayout->setContentsMargins(0, 0, 0, 0);

    // 1. Bubble container
    m_bubble = new QLabel(this);
    //m_bubble->setFrameStyle(QFrame::NoFrame);
    m_bubble->setObjectName(m_isUser ? "BubbleUser" : "BubbleAssistant");

    m_bubble->setStyleSheet(m_isUser ? "background:#d0f0ff; border-radius:8px;"
                                     : "background:#f0f0f0; border-radius:8px;");

    // 2. Markdown rendering using qlitehtml
    m_markdownLabel = new MarkdownLabel(this);
    m_markdownLabel->setWordWrap(true);
    m_markdownLabel->setAlignment(m_isUser ? Qt::AlignRight : Qt::AlignLeft);
    renderMarkdown(m_msg.content);

    // 3. Thought / extra context
    if (!m_msg.extra.isEmpty()) {
        // Render the extra context (simple key/value)
        QString ctx;
        for (const auto &kv : m_msg.extra) {
            ctx += kv.value("name").toString() + ": " + kv.value("content").toString() + "\n";
        }
        showThought(ctx, false); // not "thinking" – just a static block
    }

    // 4. Assemble bubble
    QVBoxLayout *bubbleLayout = new QVBoxLayout;
    bubbleLayout->setContentsMargins(10, 10, 10, 10);
    if (m_toughtLabel)
        bubbleLayout->addWidget(m_toughtLabel);
    bubbleLayout->addWidget(m_markdownLabel);
    m_bubble->setLayout(bubbleLayout);

    // 5. Actions bar
    QHBoxLayout *actionLayout = new QHBoxLayout;
    actionLayout->setAlignment(m_isUser ? Qt::AlignRight : Qt::AlignLeft);
    actionLayout->setContentsMargins(0, 0, 0, 0);

    const QString toolButtonStyle = R"(
        QToolButton {
            background-color: #f5f5f5;
            border: 1px solid #b0b0b0;
            border-radius: 6px;
            padding: 4px 4px;
        }
        QToolButton:hover {
            background-color: #d0d0d0;
        })";

    // Prev / Next sibling
    if (m_siblingLeafIds.size() > 1) {
        m_prevButton = new QToolButton(this);
        m_prevButton->setIcon(QIcon::fromTheme("go-previous"));
        m_prevButton->setStyleSheet(toolButtonStyle);
        m_prevButton->setEnabled(m_siblingIdx > 0);
        connect(m_prevButton, &QToolButton::clicked, this, &ChatMessage::onPrevSiblingClicked);

        m_nextButton = new QToolButton(this);
        m_nextButton->setIcon(QIcon::fromTheme("go-next"));
        m_nextButton->setStyleSheet(toolButtonStyle);
        m_nextButton->setEnabled(m_siblingIdx < m_siblingLeafIds.size() - 1);
        connect(m_nextButton, &QToolButton::clicked, this, &ChatMessage::onNextSiblingClicked);

        actionLayout->addWidget(m_prevButton);
        actionLayout->addWidget(m_nextButton);
    }

    // Edit / Regenerate
    if (m_isUser) {
        m_editButton = new QToolButton(this);
        m_editButton->setIcon(QIcon::fromTheme("edit-undo"));
        m_editButton->setStyleSheet(toolButtonStyle);
        connect(m_editButton, &QToolButton::clicked, this, &ChatMessage::onEditClicked);
        actionLayout->addWidget(m_editButton);
    } else { // assistant
        m_regenButton = new QToolButton(this);
        m_regenButton->setIcon(QIcon::fromTheme("edit-redo"));
        m_regenButton->setStyleSheet(toolButtonStyle);
        connect(m_regenButton, &QToolButton::clicked, this, &ChatMessage::onRegenerateClicked);
        actionLayout->addWidget(m_regenButton);
    }

    // Copy button
    m_copyButton = new QToolButton(this);
    m_copyButton->setIcon(QIcon::fromTheme("edit-copy"));
    m_copyButton->setStyleSheet(toolButtonStyle);
    connect(m_copyButton, &QToolButton::clicked, this, &ChatMessage::onCopyClicked);
    actionLayout->addWidget(m_copyButton);

    m_mainLayout->addWidget(m_bubble);
    m_mainLayout->addLayout(actionLayout);

    //applyStyleSheet();
}

void ChatMessage::renderMarkdown(const QString &text)
{
    m_markdownLabel->setMarkdown(text);
}

void ChatMessage::showThought(const QString &content, bool isThinking)
{
    if (!m_toughtLabel) {
        m_toughtLabel = new QLabel(this);
        m_toughtLabel->setWordWrap(true);
        m_toughtLabel->setStyleSheet("color: gray; font-style: italic;");
        m_bubble->layout()->addWidget(m_toughtLabel);
    }
    if (isThinking)
        m_toughtLabel->setText("[Thinking...]");
    else
        m_toughtLabel->setText(content);
}

void ChatMessage::messageCompleted(bool completed)
{
    // For assistant messages show regenerate and copy buttons
    if (!m_isUser) {
        m_regenButton->setVisible(completed);
        m_copyButton->setVisible(completed);
    }
}

bool ChatMessage::isUser() const
{
    return m_isUser;
}

void ChatMessage::applyStyleSheet()
{
    setAttribute(Qt::WA_StyledBackground, true);

    //background-color: #ffffff;

    setStyleSheet(R"(
        QWidget#ChatMessage {
            border: 1px solid #b0b0b0;
        }
        QLabel#BubbleUser {
            background: #d0f0ff;
            border: 1px solid #b0b0b0;
            border-radius: 8px;
        }
        QLabel#BubbleAssistant {
            background: #f0f0f0;
            border: 1px solid #b0b0b0;
            border-radius: 8px;
        }

        QToolButton {
            background-color: #f5f5f5;
            border: 1px solid #b0b0b0;
            border-radius: 6px;
            padding: 2px 2px;
        }
        QToolButton:hover {
            background-color: #d0d0d0;
        }
    )");
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
    if (m_siblingIdx > 0)
        emit siblingChanged(m_siblingLeafIds[m_siblingIdx - 1]);
}

void ChatMessage::onNextSiblingClicked()
{
    if (m_siblingIdx < m_siblingLeafIds.size() - 1)
        emit siblingChanged(m_siblingLeafIds[m_siblingIdx + 1]);
}
} // namespace LlamaCpp
