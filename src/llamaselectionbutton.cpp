#include "llamaselectionbutton.h"

#include "llamaicons.h"
#include "llamalocatorfilter.h"
#include "llamasettings.h"
#include "llamatr.h"

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/locator/ilocatorfilter.h>
#include <coreplugin/locator/locatormanager.h>
#include <texteditor/texteditor.h>

#include <QScrollBar>
#include <QTextCursor>

using namespace TextEditor;

namespace LlamaCpp {

SelectionPromptButton::SelectionPromptButton(TextEditorWidget *editor)
    : QToolButton(editor->viewport())
    , m_editor(editor)
{
    setAutoRaise(true);
    setFocusPolicy(Qt::NoFocus); // never steal the keyboard focus from the editor
    setCursor(Qt::PointingHandCursor);
    setIcon(LLAMACPP_ICON.icon());
    setToolTip(Tr::tr("Prompt llama.cpp about the selection (\"ll\" locator)"));

    connect(this, &QToolButton::clicked, this, [this] {
        if (Core::ILocatorFilter *filter = locatorFilter())
            Core::LocatorManager::showFilter(filter);
    });

    // Keep the button at the bottom of the selection as it moves.
    connect(m_editor, &TextEditorWidget::selectionChanged, this, &SelectionPromptButton::updateState);
    connect(m_editor, &TextEditorWidget::cursorPositionChanged, this, &SelectionPromptButton::updateState);
    connect(m_editor, &TextEditorWidget::textChanged, this, &SelectionPromptButton::updateState);
    if (QScrollBar *vbar = m_editor->verticalScrollBar())
        connect(vbar, &QScrollBar::valueChanged, this, &SelectionPromptButton::updateState);
    if (QScrollBar *hbar = m_editor->horizontalScrollBar())
        connect(hbar, &QScrollBar::valueChanged, this, &SelectionPromptButton::updateState);
}

SelectionPromptButton::~SelectionPromptButton() = default;

void SelectionPromptButton::updateState()
{
    if (!m_editor) {
        hide();
        return;
    }

    // Only the editor the locator will actually read the selection from gets
    // the button: the "ll" prompts use the current text editor's selection.
    const bool enabled = settings().enableLlamaCpp() && settings().selectionPromptButton();
    const bool isCurrent = m_editor == TextEditorWidget::currentTextEditorWidget();
    const QTextCursor cursor = m_editor->textCursor();
    if (!enabled || !isCurrent || !cursor.hasSelection()) {
        hide();
        return;
    }

    // Anchor the button to the end of the selection.
    QTextCursor endCursor = cursor;
    endCursor.setPosition(cursor.selectionEnd());
    const QRect rect = m_editor->cursorRect(endCursor);
    if (rect.isNull()) {
        hide();
        return;
    }

    const QSize sz = sizeHint();
    const int vw = m_editor->viewport()->width();
    const int vh = m_editor->viewport()->height();

    // Just below the end of the selection, hugging its right edge; clamp to
    // the viewport so the button never overflows.
    int x = rect.right() + 2 - sz.width();
    x = qBound(1, x, qMax(1, vw - sz.width() - 1));
    int y = rect.bottom() + 2;
    y = qBound(1, y, qMax(1, vh - sz.height() - 1));

    move(x, y);
    if (!isVisible())
        show();
    raise();
}

SelectionPromptButtonManager &SelectionPromptButtonManager::instance()
{
    static SelectionPromptButtonManager theManager;
    return theManager;
}

SelectionPromptButtonManager::SelectionPromptButtonManager()
    : QObject()
{
    connect(Core::EditorManager::instance(),
            &Core::EditorManager::currentEditorChanged,
            this,
            &SelectionPromptButtonManager::handleCurrentEditorChanged);
    connect(Core::EditorManager::instance(),
            &Core::EditorManager::editorAboutToClose,
            this,
            &SelectionPromptButtonManager::handleEditorAboutToClose);
    settings().selectionPromptButton.addOnChanged(this, [this] { handleSettingsChanged(); });
    settings().enableLlamaCpp.addOnChanged(this, [this] { handleSettingsChanged(); });

    // An editor may already be current by the time we connect (e.g. session
    // restore finished first): handle it so it gets its button too.
    if (Core::IEditor *editor = Core::EditorManager::currentEditor())
        handleCurrentEditorChanged(editor);
}

void SelectionPromptButtonManager::handleCurrentEditorChanged(Core::IEditor *editor)
{
    if (TextEditorWidget *widget = TextEditorWidget::fromEditor(editor))
        buttonFor(widget); // make sure the current editor has a button
    updateAll();
}

void SelectionPromptButtonManager::handleEditorAboutToClose(Core::IEditor *editor)
{
    if (TextEditorWidget *widget = TextEditorWidget::fromEditor(editor))
        m_buttons.remove(widget);
}

void SelectionPromptButtonManager::handleSettingsChanged()
{
    updateAll();
}

void SelectionPromptButtonManager::updateAll()
{
    for (const QPointer<SelectionPromptButton> &button : std::as_const(m_buttons))
        if (button)
            button->updateState();
}

SelectionPromptButton *SelectionPromptButtonManager::buttonFor(TextEditorWidget *editor)
{
    auto it = m_buttons.find(editor);
    if (it != m_buttons.end() && it.value())
        return it.value();
    auto *button = new SelectionPromptButton(editor);
    m_buttons.insert(editor, button);
    return button;
}

void setupSelectionPromptButton()
{
    SelectionPromptButtonManager::instance();
}

} // namespace LlamaCpp
