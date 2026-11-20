#pragma once

#include <coreplugin/ioutputpane.h>

namespace Core {
class OutputWindow;
}

namespace LlamaCpp {

// The "llama.cpp" output pane (in the Output window, next to "General
// Messages"): the managed server's terminal output (stdout and stderr) -
// the start command line first, then the server's own log - filterable and
// clearable. ANSI color escape sequences are decoded by the output window
// (forced on for the server via LLAMA_ARG_LOG_COLORS).
class LlamaOutputPane : public Core::IOutputPane
{
    Q_OBJECT

public:
    explicit LlamaOutputPane(QObject *parent = nullptr);
    ~LlamaOutputPane() override;

private:
    QWidget *outputWidget(QWidget *parent) override;
    const QList<Core::OutputWindow *> outputWindows() const override;
    void clearContents() override;
    bool canFocus() const override;
    bool hasFocus() const override;
    void setFocus() override;
    bool canNext() const override;
    bool canPrevious() const override;
    void goToNext() override;
    void goToPrev() override;
    bool canNavigate() const override;
    bool hasFilterContext() const override;
    void updateFilter() override;

    Core::OutputWindow *m_widget = nullptr;
};

LlamaOutputPane &llamaOutputPane();

} // namespace LlamaCpp
