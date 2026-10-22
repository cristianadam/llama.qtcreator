#pragma once

#include <QEvent>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QToolButton>

namespace Core {
class IEditor;
}

namespace TextEditor {
class TextEditorWidget;
}

namespace LlamaCpp {

//! A small floating button that appears at the bottom of a non-empty text
//! selection in a text editor. Clicking it opens the llama.cpp locator so the
//! selection can be sent to llama.cpp with one of the prompts.
class SelectionPromptButton : public QToolButton
{
    Q_OBJECT
public:
    explicit SelectionPromptButton(TextEditor::TextEditorWidget *editor);
    ~SelectionPromptButton() override;

    //! Recompute whether the button should be visible and, if so, where.
    void updateState();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    TextEditor::TextEditorWidget *m_editor;
};

//! Tracks the open editors and shows a SelectionPromptButton in the current
//! text editor while it has a non-empty selection.
class SelectionPromptButtonManager : public QObject
{
    Q_OBJECT
public:
    static SelectionPromptButtonManager &instance();

private:
    SelectionPromptButtonManager();
    void handleCurrentEditorChanged(Core::IEditor *editor);
    void handleEditorAboutToClose(Core::IEditor *editor);
    void handleSettingsChanged();
    void updateAll();
    SelectionPromptButton *buttonFor(TextEditor::TextEditorWidget *editor);

    QHash<TextEditor::TextEditorWidget *, QPointer<SelectionPromptButton>> m_buttons;
};

//! Connects the manager to the editor manager and settings.
void setupSelectionPromptButton();

} // namespace LlamaCpp
