#pragma once

#include <coreplugin/dialogs/ioptionspage.h>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>

namespace LlamaCpp {

// A single editable prompt. Only the top line of the prompt is shown in the
// collapsed state (that is also what the "ll" locator displays in its menu);
// expanding the entry reveals the full text that is sent to the model.
class PromptEntry : public QWidget
{
    Q_OBJECT
public:
    PromptEntry(const QString &defaultText, const QString &text, QWidget *parent = nullptr);

    QString text() const;
    void setText(const QString &text);
    void setDefaultText(const QString &defaultText);

signals:
    void textEdited(const QString &text);

private:
    void updateHeader();
    void setExpanded(bool expanded);

    QString m_defaultText;
    QToolButton *m_toggle{nullptr};
    QLabel *m_header{nullptr};
    QPlainTextEdit *m_editor{nullptr};
    QPushButton *m_resetButton{nullptr};
    bool m_expanded{false};
};

// The "Prompts" settings page: user-editable built-in prompts for the
// conversation title / follow-up generation and for the "ll" locator.
class PromptsSettingsWidget : public Core::IOptionsPageWidget
{
    Q_OBJECT
public:
    explicit PromptsSettingsWidget();
    void apply() override;
    void cancel() override;

private:
    struct LocatorEntry
    {
        PromptEntry *prompt{nullptr};
        QWidget *row{nullptr};
    };

    void syncFromSettings();
    void clearLocatorEntries();
    PromptEntry *createLocatorEntry(const QString &text, const QString &defaultText);
    void removeLocatorEntry(PromptEntry *entry);
    void syncLocatorPromptsToSettings();

    PromptEntry *m_titleEntry{nullptr};
    PromptEntry *m_followUpEntry{nullptr};
    QVBoxLayout *m_locatorLayout{nullptr};
    QList<LocatorEntry> m_locatorEntries;
};

} // namespace LlamaCpp
