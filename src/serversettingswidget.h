#pragma once

#include <coreplugin/dialogs/ioptionspage.h>

#include <QHash>
#include <QLabel>
#include <QPushButton>
#include <QWidget>

class QProgressDialog;
class QListWidget;
class QTimer;

namespace LlamaCpp {

struct ServerModel;

//! The "Server" settings page: runtime (discovery, download, update check),
//! serve settings, the model list, and the server control. The aspect rows
//! are wired by the settings container; the rest are live views of
//! LlamaRuntime and LlamaServer. The server's output goes to the
//! "llama.cpp" output pane; the page's "Show output" button pops it up.
//! The model list is updated in place (rows keyed by model id); a selected
//! model can be loaded/unloaded or removed, and new models are downloaded
//! through the server from the llama.app catalog.
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
    void updateModelButtons();
    void toggleModelLoad();
    void removeSelectedModel(const ServerModel &model);
    // The row of the model \a id (-1 when absent).
    int modelRow(const QString &id) const;
    // The model of row \a row (an empty model when the row is invalid).
    ServerModel modelAt(int row) const;
    ServerModel selectedModel() const;
    static bool isModelRunningStatus(const QString &status);

    QLabel *m_runtimeStatus = nullptr;
    QPushButton *m_installButton = nullptr;
    QPushButton *m_updateButton = nullptr;
    QLabel *m_modelHint = nullptr;
    QPushButton *m_modelDownloadButton = nullptr;
    QPushButton *m_modelRefreshButton = nullptr;
    QListWidget *m_modelList = nullptr;
    QPushButton *m_modelLoadButton = nullptr;
    QPushButton *m_modelRemoveButton = nullptr;
    QTimer *m_modelPollTimer = nullptr;
    QPushButton *m_startStopButton = nullptr;
    QLabel *m_stateLabel = nullptr;
    QPushButton *m_showOutputButton = nullptr;
    QLabel *m_chatHint = nullptr;
    // The current model list, keyed by id (the rows' data).
    QHash<QString, ServerModel> m_modelsById;
    // Busy dialog while the install script runs (the install continues in
    // LlamaRuntime).
    QProgressDialog *m_installDialog = nullptr;
};

} // namespace LlamaCpp
