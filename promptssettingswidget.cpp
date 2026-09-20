#include "promptssettingswidget.h"

#include "llamasettings.h"
#include "llamatr.h"

#include <QFontDatabase>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QVBoxLayout>

using namespace Utils;

namespace LlamaCpp {

namespace {

QString topLine(const QString &text)
{
    return text.section(QLatin1Char('\n'), 0, 0).trimmed();
}

} // namespace

/* ------------------------------------------------------------ PromptEntry */

PromptEntry::PromptEntry(const QString &defaultText, const QString &text, QWidget *parent)
    : QWidget(parent)
    , m_defaultText(defaultText)
{
    m_toggle = new QToolButton(this);
    m_toggle->setAutoRaise(true);
    m_toggle->setFocusPolicy(Qt::NoFocus);
    m_toggle->setCursor(Qt::PointingHandCursor);

    m_header = new QLabel(this);
    m_header->setTextInteractionFlags(Qt::TextSelectableByMouse);

    m_editor = new QPlainTextEdit(this);
    m_editor->setPlainText(text);
    m_editor->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_editor->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    m_resetButton = new QPushButton(Tr::tr("Reset to Default"), this);
    m_resetButton->setFocusPolicy(Qt::ClickFocus);

    auto *headerLayout = new QHBoxLayout();
    headerLayout->setContentsMargins(0, 0, 0, 0);
    headerLayout->setSpacing(4);
    headerLayout->addWidget(m_toggle);
    headerLayout->addWidget(m_header, 1);

    auto *resetLayout = new QHBoxLayout();
    resetLayout->setContentsMargins(16, 0, 0, 0);
    resetLayout->addStretch(1);
    resetLayout->addWidget(m_resetButton);

    auto *bodyLayout = new QVBoxLayout();
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(2);
    bodyLayout->addWidget(m_editor);
    bodyLayout->addLayout(resetLayout);

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(0, 0, 0, 0);
    outerLayout->setSpacing(2);
    outerLayout->addLayout(headerLayout);
    outerLayout->addLayout(bodyLayout);

    connect(m_toggle, &QToolButton::clicked, this, [this] { setExpanded(!m_expanded); });
    connect(m_editor, &QPlainTextEdit::textChanged, this, [this] {
        updateHeader();
        emit textEdited(m_editor->toPlainText());
    });
    connect(m_resetButton, &QPushButton::clicked, this, [this] {
        m_editor->setPlainText(m_defaultText);
    });

    setExpanded(false);
    updateHeader();
}

QString PromptEntry::text() const
{
    return m_editor ? m_editor->toPlainText() : QString();
}

void PromptEntry::setText(const QString &text)
{
    if (m_editor->toPlainText() == text)
        return;
    m_editor->setPlainText(text);
}

void PromptEntry::setDefaultText(const QString &defaultText)
{
    m_defaultText = defaultText;
}

void PromptEntry::updateHeader()
{
    const QString text = m_editor->toPlainText();
    const QString top = topLine(text);
    m_header->setText(top.isEmpty() ? QStringLiteral("…") : top);
    m_header->setToolTip(text);
}

void PromptEntry::setExpanded(bool expanded)
{
    m_expanded = expanded;
    m_editor->setVisible(expanded);
    m_resetButton->setVisible(expanded);
    m_toggle->setText(expanded ? QStringLiteral("▾") : QStringLiteral("▸"));
    m_toggle->setToolTip(expanded ? Tr::tr("Collapse the full prompt")
                                  : Tr::tr("Expand the full prompt"));
}

/* -------------------------------------------------------- PromptsSettingsWidget */

PromptsSettingsWidget::PromptsSettingsWidget()
{
    auto &s = settings();

    auto *mainLayout = new QVBoxLayout(this);
    mainLayout->setSpacing(12);

    auto *chatBox = new QGroupBox(Tr::tr("Chat"), this);
    auto *chatLayout = new QVBoxLayout(chatBox);
    chatLayout->setSpacing(4);

    m_titleEntry = new PromptEntry(defaultTitlePrompt(), s.titlePrompt.value(), chatBox);
    connect(m_titleEntry, &PromptEntry::textEdited, this, [this](const QString &text) {
        settings().titlePrompt.setValue(text);
    });
    chatLayout->addWidget(m_titleEntry);

    m_followUpEntry = new PromptEntry(defaultFollowUpPrompt(), s.followUpPrompt.value(), chatBox);
    connect(m_followUpEntry, &PromptEntry::textEdited, this, [this](const QString &text) {
        settings().followUpPrompt.setValue(text);
    });
    chatLayout->addWidget(m_followUpEntry);

    auto *locatorBox = new QGroupBox(Tr::tr("Locator (\u201cll\u201d)"), this);
    auto *locatorLayout = new QVBoxLayout(locatorBox);
    locatorLayout->setSpacing(4);
    m_locatorLayout = locatorLayout;

    auto *hint = new QLabel(
        Tr::tr("Only the first line of a prompt is shown in the locator menu; the "
               "full text is sent to the model. \u201c{selection}\u201d is replaced "
               "with the selected text."),
        this);
    hint->setWordWrap(true);
    locatorLayout->addWidget(hint);

    auto *addButton = new QPushButton(Tr::tr("Add Prompt"), this);
    addButton->setFocusPolicy(Qt::ClickFocus);
    locatorLayout->addWidget(addButton, 0, Qt::AlignRight);

    syncFromSettings();

    connect(addButton, &QPushButton::clicked, this, [this] {
        auto list = settings().locatorPrompts.value();
        list.append(QString());
        settings().locatorPrompts.setValue(list);
        syncFromSettings();
    });

    mainLayout->addWidget(chatBox);
    mainLayout->addWidget(locatorBox);
    mainLayout->addStretch(1);
}

void PromptsSettingsWidget::apply()
{
    settings().writeSettings();
}

void PromptsSettingsWidget::cancel()
{
    settings().readSettings();
    syncFromSettings();
}

void PromptsSettingsWidget::syncFromSettings()
{
    auto &s = settings();

    m_titleEntry->setText(s.titlePrompt.value());
    m_followUpEntry->setText(s.followUpPrompt.value());

    clearLocatorEntries();

    const QStringList prompts = s.locatorPrompts.value();
    const QStringList defaults = defaultLocatorPrompts();
    for (int i = 0; i < prompts.size(); ++i) {
        const QString def = i < defaults.size() ? defaults.at(i) : QString();
        createLocatorEntry(prompts.at(i), def);
    }
}

PromptEntry *PromptsSettingsWidget::createLocatorEntry(const QString &text,
                                                       const QString &defaultText)
{
    QWidget *row = new QWidget(this);
    auto *rowLayout = new QHBoxLayout(row);
    rowLayout->setContentsMargins(0, 0, 0, 0);
    rowLayout->setSpacing(4);

    PromptEntry *entry = new PromptEntry(defaultText, text, row);
    connect(entry, &PromptEntry::textEdited, this, [this](const QString &) {
        syncLocatorPromptsToSettings();
    });

    auto *removeButton = new QToolButton(row);
    removeButton->setText(QStringLiteral("\u00d7"));
    removeButton->setAutoRaise(true);
    removeButton->setFocusPolicy(Qt::NoFocus);
    removeButton->setCursor(Qt::PointingHandCursor);
    removeButton->setToolTip(Tr::tr("Remove this prompt"));
    // Capture the entry itself (not an index) so that removing other
    // entries does not invalidate this button.
    connect(removeButton, &QToolButton::clicked, this, [this, entry] {
        removeLocatorEntry(entry);
    });

    rowLayout->addWidget(entry, 1);
    rowLayout->addWidget(removeButton, 0, Qt::AlignTop);

    m_locatorLayout->insertWidget(m_locatorLayout->count() - 1, row);

    m_locatorEntries.append({entry, row});
    return entry;
}

void PromptsSettingsWidget::removeLocatorEntry(PromptEntry *entry)
{
    int index = -1;
    for (int i = 0; i < m_locatorEntries.size(); ++i) {
        if (m_locatorEntries[i].prompt == entry) {
            index = i;
            break;
        }
    }
    if (index == -1)
        return;

    auto &s = settings();
    auto list = s.locatorPrompts.value();
    if (index < list.size())
        list.removeAt(index);
    s.locatorPrompts.setValue(list);

    const LocatorEntry removed = m_locatorEntries.takeAt(index);
    removed.row->deleteLater();
}

void PromptsSettingsWidget::clearLocatorEntries()
{
    for (const LocatorEntry &record : m_locatorEntries)
        record.row->deleteLater();
    m_locatorEntries.clear();
}

void PromptsSettingsWidget::syncLocatorPromptsToSettings()
{
    auto &s = settings();
    auto list = s.locatorPrompts.value();
    while (list.size() < m_locatorEntries.size())
        list.append(QString());
    list.resize(m_locatorEntries.size());
    for (int i = 0; i < m_locatorEntries.size(); ++i)
        list[i] = m_locatorEntries[i].prompt->text();
    s.locatorPrompts.setValue(list);
}

} // namespace LlamaCpp
