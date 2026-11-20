#pragma once

#include <coreplugin/dialogs/ioptionspage.h>

#include <QLabel>
#include <QPushButton>
#include <QWidget>

class QProgressDialog;
class QListWidget;
class QTimer;

namespace LlamaCpp {

//! The "Server" settings page: runtime (discovery, download, update check),
//! serve settings, the model list, and the server control. The aspect rows
//! are wired by the settings container; the rest are live views of
//! LlamaRuntime and LlamaServer. The server's output goes to the
//! "llama.cpp" output pane; the page's "Show output" button pops it up.
class ServerSettingsWidget : public Core::IOptionsPageWidget
{
    Q_OBJECT
public:
    explicit ServerSettingsWidget();
    void apply() override;
    void cancel() override;

private:
    void updateRuntimeStatus();
    void updateServerState();
    void updateChatHint();
    void updateModelList();

    QLabel *m_runtimeStatus = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_updateButton = nullptr;
    QLabel *m_modelHint = nullptr;
    QPushButton *m_modelRefreshButton = nullptr;
    QListWidget *m_modelList = nullptr;
    QTimer *m_modelPollTimer = nullptr;
    QPushButton *m_startStopButton = nullptr;
    QLabel *m_stateLabel = nullptr;
    QPushButton *m_showOutputButton = nullptr;
    QLabel *m_chatHint = nullptr;
    // Busy dialog while the install script runs (the install continues in
    // LlamaRuntime).
    QProgressDialog *m_installDialog = nullptr;
};

} // namespace LlamaCpp
