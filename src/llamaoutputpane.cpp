#include "llamaoutputpane.h"

#include "llamaconstants.h"
#include "llamaserver.h"
#include "llamatr.h"

#include <coreplugin/outputwindow.h>
#include <texteditor/fontsettings.h>
#include <utils/shutdownguard.h>

namespace LlamaCpp {

LlamaOutputPane::LlamaOutputPane(QObject *parent)
    : Core::IOutputPane(parent)
{
    setId(Constants::LLAMACPP_OUTPUT_PANE_ID);
    setDisplayName(Tr::tr("llama.cpp"));
    setPriorityInStatusBar(-90); // right of "General Messages" (-100)

    m_widget = new Core::OutputWindow(
        Core::Context(Constants::LLAMACPP_OUTPUT_PANE_CONTEXT_ID),
        Utils::Key(Constants::LLAMACPP_OUTPUT_PANE_ZOOM_KEY));
    m_widget->setReadOnly(true);

    // Like the other output panes, use the editor's font.
    connect(&TextEditor::globalFontSettings(), &TextEditor::FontSettings::changed,
            m_widget, [this] {
                m_widget->setBaseFont(TextEditor::globalFontSettings().data().font());
            });
    m_widget->setBaseFont(TextEditor::globalFontSettings().data().font());

    connect(this, &Core::IOutputPane::zoomInRequested, m_widget, &Core::OutputWindow::zoomIn);
    connect(this, &Core::IOutputPane::zoomOutRequested, m_widget, &Core::OutputWindow::zoomOut);
    connect(this, &Core::IOutputPane::resetZoomRequested, m_widget, &Core::OutputWindow::resetZoom);
    connect(this, &Core::IOutputPane::fontChanged, m_widget, &Core::OutputWindow::setBaseFont);
    connect(this, &Core::IOutputPane::wheelZoomEnabledChanged,
            m_widget, &Core::OutputWindow::setWheelZoomEnabled);

    setupFilterUi(Constants::LLAMACPP_OUTPUT_PANE_FILTER_KEY,
                  QStringLiteral("LlamaCpp::LlamaOutputPane"));
    setFilteringEnabled(true);
    setupContext(Constants::LLAMACPP_OUTPUT_PANE_CONTEXT_ID, m_widget);

    // The managed server's terminal output: the output window decodes the
    // ANSI color escape sequences (16, 256 and true color).
    connect(&server(), &LlamaServer::outputAppended, m_widget, [this](const QString &text) {
        m_widget->appendMessage(text, Utils::GeneralMessageFormat);
    });
    connect(&server(), &LlamaServer::outputReset, m_widget, [this] {
        m_widget->clear();
    });
}

LlamaOutputPane::~LlamaOutputPane()
{
    delete m_widget;
}

QWidget *LlamaOutputPane::outputWidget(QWidget *parent)
{
    m_widget->setParent(parent);
    return m_widget;
}

const QList<Core::OutputWindow *> LlamaOutputPane::outputWindows() const
{
    return {m_widget};
}

void LlamaOutputPane::clearContents()
{
    m_widget->clear();
}

bool LlamaOutputPane::canFocus() const
{
    return true;
}

bool LlamaOutputPane::hasFocus() const
{
    return m_widget->window()->focusWidget() == m_widget;
}

void LlamaOutputPane::setFocus()
{
    m_widget->setFocus();
}

bool LlamaOutputPane::canNext() const
{
    return false;
}

bool LlamaOutputPane::canPrevious() const
{
    return false;
}

void LlamaOutputPane::goToNext()
{
}

void LlamaOutputPane::goToPrev()
{
}

bool LlamaOutputPane::canNavigate() const
{
    return false;
}

bool LlamaOutputPane::hasFilterContext() const
{
    return true;
}

void LlamaOutputPane::updateFilter()
{
    m_widget->updateFilterProperties(filterText(), filterCaseSensitivity(), filterUsesRegexp(),
                                     filterIsInverted(), beforeContext(), afterContext());
}

LlamaOutputPane &llamaOutputPane()
{
    static LlamaOutputPane *thePane = nullptr;
    if (!thePane)
        thePane = new LlamaOutputPane(Utils::shutdownGuard());
    return *thePane;
}

} // namespace LlamaCpp
