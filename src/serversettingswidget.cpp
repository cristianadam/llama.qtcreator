#include "serversettingswidget.h"

#include "ansitext.h"
#include "llamaicons.h"
#include "llamaoutputpane.h"
#include "llamasettings.h"
#include "llamaserver.h"
#include "llamaruntime.h"
#include "llamatr.h"
#include "modeldownloaddialog.h"
#include "modellogos.h"

#include <coreplugin/icore.h>
#include <utils/layoutbuilder.h>
#include <utils/qtcassert.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QProgressDialog>
#include <QTimer>
#include <QVBoxLayout>

namespace LlamaCpp {
using namespace Layouting;

// The item data role carrying the model id of a row.
static constexpr int ModelIdRole = Qt::UserRole + 1;
// The id of the list's "no models" placeholder row.
static const QString kEmptyRowId = QStringLiteral("\0empty");

static QString link(const char *url)
{
    return QString("<a href=\"%1\">%1</a>").arg(QLatin1String(url));
}

static QString modelDetails(const ServerModel &model)
{
    QStringList parts;
    const QString quant = quantFromFileName(model.id);
    if (!quant.isEmpty())
        parts << quant;
    if (model.sizeBytes > 0)
        parts << formatByteSize(model.sizeBytes);
    if (model.contextLength > 0)
        parts << (model.contextLength >= 1024
                      ? QStringLiteral("%1k ctx").arg(model.contextLength / 1024)
                      : QStringLiteral("%1 ctx").arg(model.contextLength));
    if (!model.source.isEmpty() && model.source != QLatin1String("local"))
        parts << model.source;
    return parts.join(QStringLiteral(" \u00B7 "));
}

// One model of the Server page's list: logo, name, details, modality marks,
// and - while the server runs - the live status. Updated in place on
// refreshes, so the list's scroll position and selection survive.
class ModelRow : public QWidget
{
public:
    ModelRow(const ServerModel &model, bool live, QWidget *parent)
        : QWidget(parent)
        , m_modelId(model.id)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 5, 6, 5);
        layout->setSpacing(8);

        m_logo = new QLabel(this);
        m_logo->setFixedSize(28, 28);
        m_logo->setScaledContents(true);
        m_logo->setFrameShape(QFrame::StyledPanel);
        m_logo->setStyleSheet(QStringLiteral("border-radius: 6px;"));
        // The logo is a monochrome "currentColor" SVG painted in the palette
        // color; re-render it when the theme changes (see eventFilter()).
        m_logo->installEventFilter(this);
        m_logo->setPixmap(modelLogoPixmap(model.id, 28,
                                          m_logo->palette().color(QPalette::WindowText)));
        layout->addWidget(m_logo, 0, Qt::AlignVCenter);

        auto *column = new QWidget(this);
        auto *columnLayout = new QVBoxLayout(column);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(1);
        m_name = new QLabel(model.id, column);
        m_name->setToolTip(model.path);
        QFont nameFont = m_name->font();
        nameFont.setWeight(QFont::DemiBold);
        m_name->setFont(nameFont);
        columnLayout->addWidget(m_name);
        m_details = new QLabel(column);
        m_details->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
        columnLayout->addWidget(m_details);
        layout->addWidget(column, 1);

        // The modality marks (the lucide eye/ear/video glyphs); "text" is
        // not marked.
        m_marks = new QWidget(this);
        m_marksLayout = new QHBoxLayout(m_marks);
        m_marksLayout->setContentsMargins(0, 0, 0, 0);
        m_marksLayout->setSpacing(4);
        layout->addWidget(m_marks, 0, Qt::AlignVCenter);

        m_status = new QWidget(this);
        auto *statusLayout = new QHBoxLayout(m_status);
        statusLayout->setContentsMargins(0, 0, 0, 0);
        statusLayout->setSpacing(4);
        m_statusIcon = new QLabel(m_status);
        statusLayout->addWidget(m_statusIcon);
        m_statusText = new QLabel(m_status);
        m_statusText->setStyleSheet(QStringLiteral("font-size: 11px;"));
        statusLayout->addWidget(m_statusText);
        layout->addWidget(m_status, 0, Qt::AlignVCenter);

        updateModel(model, live);
    }

    void updateModel(const ServerModel &model, bool live)
    {
        m_name->setToolTip(model.path);
        const QString details = modelDetails(model);
        m_details->setText(details);
        m_details->setVisible(!details.isEmpty());
        setModalities(model.inputModalities);
        m_status->setVisible(live);
        if (live)
            setStatus(model.status);
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override
    {
        // The logo label's palette changed (theme switch): repaint the mark.
        if (obj == m_logo && event->type() == QEvent::PaletteChange)
            m_logo->setPixmap(modelLogoPixmap(m_modelId, 28,
                                              m_logo->palette().color(QPalette::WindowText)));
        return QWidget::eventFilter(obj, event);
    }

private:
    QLabel *m_logo = nullptr;
    QString m_modelId;
    void setModalities(const QStringList &modalities)
    {
        while (QLayoutItem *item = m_marksLayout->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        for (const QString &modality : modalities) {
            const QChar glyph = modality == QLatin1String("image")
                                    ? Lucide::Eye
                                    : modality == QLatin1String("audio")
                                          ? Lucide::Ear
                                          : modality == QLatin1String("video")
                                                ? Lucide::Video
                                                : QChar();
            if (glyph.isNull())
                continue;
            auto *mark = new QLabel(m_marks);
            mark->setPixmap(lucideGlyph(glyph, Qt::gray, 13).pixmap(13, 13));
            mark->setToolTip(modality);
            m_marksLayout->addWidget(mark);
        }
        m_marks->setVisible(m_marksLayout->count() > 0);
    }

    void setStatus(const QString &status)
    {
        const QChar glyph = statusGlyph(status);
        const QColor color = statusColor(status);
        m_statusIcon->setVisible(!glyph.isNull());
        if (!glyph.isNull())
            m_statusIcon->setPixmap(lucideGlyph(glyph, color, 13).pixmap(13, 13));
        m_statusText->setText(statusText(status));
        m_statusText->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                         .arg(color.name()));
    }

    static QChar statusGlyph(const QString &status)
    {
        if (status == QLatin1String("loaded") || status == QLatin1String("sleeping"))
            return Lucide::CircleCheck;
        if (status == QLatin1String("loading") || status == QLatin1String("downloading")
            || status == QLatin1String("incomplete"))
            return Lucide::LoaderCircle;
        if (status == QLatin1String("failed"))
            return Lucide::TriangleExclamationPoint;
        return QChar();
    }
    static QColor statusColor(const QString &status)
    {
        if (status == QLatin1String("failed"))
            return QColor(QStringLiteral("#d9534f"));
        if (status == QLatin1String("loaded") || status == QLatin1String("sleeping"))
            return QColor(QStringLiteral("#4caf50"));
        return Qt::gray;
    }
    static QString statusText(const QString &status)
    {
        if (status == QLatin1String("loading"))
            return Tr::tr("loading...");
        if (status == QLatin1String("downloading") || status == QLatin1String("incomplete"))
            return Tr::tr("downloading...");
        if (status == QLatin1String("failed"))
            return Tr::tr("failed");
        if (status == QLatin1String("sleeping"))
            return Tr::tr("sleeping");
        if (status == QLatin1String("loaded"))
            return Tr::tr("loaded");
        return Tr::tr("not loaded");
    }

    QLabel *m_name = nullptr;
    QLabel *m_details = nullptr;
    QWidget *m_marks = nullptr;
    QLayout *m_marksLayout = nullptr;
    QWidget *m_status = nullptr;
    QLabel *m_statusIcon = nullptr;
    QLabel *m_statusText = nullptr;
};

ServerSettingsWidget::ServerSettingsWidget()
{
    auto &s = settings();
    auto &rt = runtime();
    auto &srv = server();

    // llama.cpp runtime group: the resolved path is the path field's
    // placeholder (like ripgrep's on the Tools page).
    m_runtimeStatus = new QLabel(this);
    m_runtimeStatus->setWordWrap(true);
    m_installButton = new QPushButton(Tr::tr("Download llama.cpp..."), this);
    m_installButton->setIcon(lucideButtonIcon(Lucide::Download, m_installButton));
    m_installButton->setToolTip(Tr::tr(
        "Downloads and runs the official llama.cpp install script. The build is "
        "not pinned: the script installs whatever the install channel marks as "
        "latest."));
    m_updateButton = new QPushButton(Tr::tr("Check for updates"), this);
    m_updateButton->setToolTip(Tr::tr(
        "Checks the llama.cpp install channel for a newer build than the "
        "installed one."));

    // models group: /models while the server runs, the models directory
    // itself while it is stopped.
    m_modelHint = new QLabel(this);
    m_modelHint->setWordWrap(true);
    m_modelDownloadButton = new QPushButton(Tr::tr("Download models..."), this);
    m_modelDownloadButton->setIcon(lucideButtonIcon(Lucide::Download, m_modelDownloadButton));
    m_modelDownloadButton->setToolTip(Tr::tr(
        "Browses the llama.app model catalog and downloads a model through "
        "the running server into its model cache. The server must be running."));
    m_modelRefreshButton = new QPushButton(Tr::tr("Refresh"), this);
    m_modelRefreshButton->setIcon(
        lucideButtonIcon(Lucide::RefreshCw, m_modelRefreshButton));
    m_modelRefreshButton->setToolTip(Tr::tr(
        "Re-reads the model list. While the server runs, it also makes the "
        "server rescan the models directory, so new .gguf files show up "
        "without a restart."));
    m_modelList = new QListWidget(this);
    m_modelList->setMinimumHeight(120);
    m_modelList->setMaximumHeight(260);
    m_modelList->setSelectionMode(QAbstractItemView::SingleSelection);
    m_modelList->setContextMenuPolicy(Qt::CustomContextMenu);
    m_modelList->setToolTip(Tr::tr(
        "The models the server offers. Select one to load or unload it on the "
        "server, or right-click it for more actions."));
    m_modelLoadButton = new QPushButton(Tr::tr("Load model"), this);
    m_modelLoadButton->setToolTip(Tr::tr(
        "Loads the selected model on the server (starts its model process), or "
        "unloads it when it is running."));
    m_modelRemoveButton = new QPushButton(Tr::tr("Remove model"), this);
    m_modelRemoveButton->setIcon(lucideButtonIcon(Lucide::Trash, m_modelRemoveButton));
    m_modelRemoveButton->setToolTip(Tr::tr(
        "Removes the selected model from the server's model cache (a model "
        "that was downloaded by the server)."));
    m_modelPollTimer = new QTimer(this);
    m_modelPollTimer->setInterval(5000);
    connect(m_modelPollTimer, &QTimer::timeout, this, [this] {
        if (server().state() == LlamaServer::State::Running
            && !server().isRefreshingModels())
            server().refreshModels();
    });

    m_startStopButton = new QPushButton(Tr::tr("Start server"), this);
    m_stateLabel = new QLabel(this);
    m_stateLabel->setWordWrap(true);
    // Let the URL be selected and copied with the mouse.
    m_stateLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    // Expand over the row's free space itself, instead of a trailing stretch.
    m_stateLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_showOutputButton = new QPushButton(Tr::tr("Show output"), this);
    m_showOutputButton->setToolTip(Tr::tr(
        "Shows the managed server's terminal output (stdout and stderr) in "
        "the \"llama.cpp\" pane of the Output window: the start command "
        "line, then the colored log; filterable and clearable."));

    m_chatHint = new QLabel(this);
    m_chatHint->setWordWrap(true);
    m_chatHint->setTextInteractionFlags(Qt::TextSelectableByMouse);

    QCheckBox *autoStart = new QCheckBox(s.serverAutoStart.displayName(), this);
    autoStart->setToolTip(s.serverAutoStart.toolTip());
    autoStart->setChecked(s.serverAutoStart());
    connect(autoStart, &QCheckBox::toggled, this, [this](bool checked) {
        settings().serverAutoStart.setValue(checked);
    });
    // Keep the check in step when the aspect changes from outside; setValue
    // does not re-announce equal values, so this does not loop.
    connect(&s.serverAutoStart, &Utils::BaseAspect::changed, this, [this, autoStart] {
        autoStart->setChecked(settings().serverAutoStart());
    });

    // Before the layout is attached: the aspect applies the path field's
    // placeholder only when the field is created.
    updateRuntimeStatus();

    // clang-format off
    Column {
        Group {
            title(Tr::tr("llama.cpp runtime")),
            Column {
                m_runtimeStatus,
                Row {s.llamaPath, m_installButton, m_updateButton},
            },
        }.emerge(),
        Group {
            title(Tr::tr("Server")),
            Column {
                Row {s.serverHost, s.serverPort},
                s.serverModelsDir,
                Row {s.serverArguments},
                autoStart,
                m_chatHint,
            },
        }.emerge(),
        Group {
            title(Tr::tr("Models")),
            Column {
                Row {m_modelHint, st, m_modelDownloadButton, m_modelRefreshButton},
                m_modelList,
                Row {m_modelLoadButton, m_modelRemoveButton, st},
            },
        }.emerge(),
        Group {
            title(Tr::tr("Control")),
            Column {
                Row {m_startStopButton, m_stateLabel},
                Row {m_showOutputButton, st},
            },
        }.emerge(),
        st
    }.attachTo(this);
    // clang-format on

    connect(&rt, &LlamaRuntime::resolvedPathChanged, this, [this](const QString &) {
        updateRuntimeStatus();
    });
    connect(&rt, &LlamaRuntime::versionChanged, this, [this](const QString &) {
        updateRuntimeStatus();
    });
    // Re-discover the runtime when the path changes.
    connect(&s.llamaPath, &Utils::BaseAspect::changed, this, [this] {
        runtime().refresh();
        updateRuntimeStatus();
    });

    connect(m_installButton, &QPushButton::clicked, this, [this] {
        if (runtime().isInstalling())
            return;
        const QString text
            = "<p>" + Tr::tr("Download llama.cpp from llama.app?") + "</p><p>"
              + Tr::tr("The Server page runs the llama.cpp runtime, which is not "
                       "installed on this system, so it can be downloaded here "
                       "instead. The download fetches and runs llama.cpp's "
                       "official install script. llama.cpp is published under "
                       "the MIT license.")
              + "</p><p>"
              + Tr::tr("Project: %1").arg(link("https://github.com/ggml-org/llama.cpp"))
              + "<br/>"
              + Tr::tr("License: %1")
                    .arg(link("https://github.com/ggml-org/llama.cpp/blob/master/LICENSE"))
              + "</p>";
        QMessageBox box(QMessageBox::Question,
                        Tr::tr("Download llama.cpp"),
                        text,
                        QMessageBox::Cancel,
                        Core::ICore::dialogParent());
        box.setTextFormat(Qt::RichText);
        box.addButton(Tr::tr("Download"), QMessageBox::AcceptRole);
        if (box.exec() == QMessageBox::Cancel)
            return;
        runtime().install();
    });

    connect(m_updateButton, &QPushButton::clicked, this, [this] {
        if (runtime().isCheckingUpdates())
            return;
        runtime().checkForUpdates();
    });
    connect(&rt, &LlamaRuntime::updateCheckFinished, this,
            [this](bool hasUpdate, const QString &message) {
                updateRuntimeStatus();
                if (hasUpdate) {
                    const int answer = QMessageBox::question(
                        Core::ICore::dialogParent(),
                        Tr::tr("llama.cpp"),
                        message + QLatin1Char(' ') + Tr::tr("Install the update now?"),
                        QMessageBox::Yes | QMessageBox::No);
                    if (answer == QMessageBox::Yes)
                        runtime().install();
                } else {
                    QMessageBox::information(Core::ICore::dialogParent(),
                                             Tr::tr("llama.cpp"),
                                             message);
                }
            });

    connect(&rt, &LlamaRuntime::installStarted, this, [this] {
        m_installDialog = new QProgressDialog(Tr::tr("Installing llama.cpp..."),
                                              nullptr,
                                              0,
                                              0,
                                              this);
        m_installDialog->setWindowModality(Qt::ApplicationModal);
        m_installDialog->show();
        updateRuntimeStatus();
    });
    connect(&rt, &LlamaRuntime::installProgress, this, [this](const QString &status) {
        if (m_installDialog)
            m_installDialog->setLabelText(status);
    });
    connect(&rt, &LlamaRuntime::installFinished, this,
            [this](bool success, const QString &message) {
                if (m_installDialog) {
                    m_installDialog->close();
                    m_installDialog->deleteLater();
                    m_installDialog = nullptr;
                }
                if (!success) {
                    QMessageBox::warning(Core::ICore::dialogParent(),
                                         Tr::tr("llama.cpp"),
                                         message);
                } else if (settings().llamaPath().isEmpty()
                           && !runtime().resolvedPath().isEmpty()) {
                    // The freshly installed binary becomes the configured one,
                    // so the path field shows it.
                    settings().llamaPath.setValue(runtime().resolvedPath());
                }
                updateRuntimeStatus();
                updateServerState();
            });

    connect(m_startStopButton, &QPushButton::clicked, this, [this] {
        const auto state = server().state();
        if (state == LlamaServer::State::Starting || state == LlamaServer::State::Running)
            server().stop();
        else
            server().start();
    });
    connect(&srv, &LlamaServer::stateChanged, this, [this](LlamaServer::State state) {
        updateServerState();
        if (state == LlamaServer::State::Running || state == LlamaServer::State::Starting)
            server().refreshModels();
        updateModelList();
    });
    connect(m_showOutputButton, &QPushButton::clicked, this, [this] {
        llamaOutputPane().popup(Core::IOutputPane::WithFocus);
    });

    connect(&srv, &LlamaServer::modelsUpdated, this, [this](const QList<ServerModel> &) {
        updateModelList();
    });
    connect(&srv, &LlamaServer::modelActionFailed, this, [this](const QString &, const QString &error) {
        QMessageBox::warning(Core::ICore::dialogParent(),
                             Tr::tr("llama.cpp"),
                             error);
    });
    connect(&s.serverModelsDir, &Utils::BaseAspect::changed, this, [this] {
        updateModelList();
    });
    connect(m_modelRefreshButton, &QPushButton::clicked, this, [this] {
        const auto state = server().state();
        if (state == LlamaServer::State::Running || state == LlamaServer::State::Starting)
            server().refreshModels(/* reload */ true);
        else
            updateModelList();
    });
    connect(m_modelDownloadButton, &QPushButton::clicked, this, [this] {
        if (server().state() != LlamaServer::State::Running)
            return;
        auto *dialog = new ModelDownloadDialog(Core::ICore::dialogParent());
        dialog->setAttribute(Qt::WA_DeleteOnClose);
        dialog->open();
    });
    connect(m_modelList, &QListWidget::currentRowChanged, this, [this](int) {
        updateModelButtons();
    });
    connect(m_modelList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem *item) {
        if (item && item->data(ModelIdRole).toString() != kEmptyRowId)
            toggleModelLoad();
    });
    connect(m_modelList, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = m_modelList->indexAt(pos).row();
        if (row < 0)
            return;
        const ServerModel model = modelAt(row);
        if (model.id.isEmpty())
            return; // the "no models" placeholder row
        const bool running = server().state() == LlamaServer::State::Running;
        if (!running)
            return; // the model actions need the server
        // Select the right-clicked row (a right-click selects it on macOS, but not everywhere).
        m_modelList->setCurrentRow(row);
        QMenu menu(this);
        const bool modelRunning = isModelRunningStatus(model.status);
        auto *loadAction = menu.addAction(
            modelRunning ? Tr::tr("Unload model") : Tr::tr("Load model"));
        connect(loadAction, &QAction::triggered, this, [this] {
            toggleModelLoad();
        });
        if (model.canRemove) {
            auto *removeAction = menu.addAction(Tr::tr("Remove model..."));
            connect(removeAction, &QAction::triggered, this, [this, model] {
                removeSelectedModel(model);
            });
        }
        menu.exec(m_modelList->viewport()->mapToGlobal(pos));
    });
    connect(m_modelLoadButton, &QPushButton::clicked, this, [this] {
        toggleModelLoad();
    });
    connect(m_modelRemoveButton, &QPushButton::clicked, this, [this] {
        const ServerModel model = selectedModel();
        if (!model.id.isEmpty())
            removeSelectedModel(model);
    });

    connect(&s.serverHost, &Utils::BaseAspect::changed, this, [this] { updateChatHint(); });
    connect(&s.serverPort, &Utils::BaseAspect::changed, this, [this] { updateChatHint(); });

    updateRuntimeStatus();
    updateServerState();
    updateChatHint();
    m_modelPollTimer->start();
    const auto state = server().state();
    if (state == LlamaServer::State::Running || state == LlamaServer::State::Starting)
        server().refreshModels();
    updateModelList();
}

void ServerSettingsWidget::apply()
{
    // The page applies the whole shared settings container, like the other
    // llama.cpp pages.
    settings().apply();
    settings().writeSettings();
}

void ServerSettingsWidget::cancel()
{
    settings().cancel();
}

void ServerSettingsWidget::updateRuntimeStatus()
{
    const auto &rt = runtime();
    const QString path = rt.resolvedPath();
    if (path.isEmpty()) {
        m_runtimeStatus->setText(Tr::tr(
            "No llama.cpp binary was found. Download it, or point the path field "
            "below at one of yours (llama or llama-server)."));
    } else {
        m_runtimeStatus->setText(rt.versionTag().isEmpty()
                                     ? Tr::tr("llama.cpp (version unknown)")
                                     : Tr::tr("llama.cpp %1").arg(rt.versionTag()));
    }
    // The resolved path is the path field's placeholder (like ripgrep's on
    // the Tools page): what the plugin would run when the field is empty.
    settings().llamaPath.setPlaceHolderText(
        path.isEmpty() ? Tr::tr("Not found") : path);
    m_installButton->setEnabled(!rt.isInstalling());
    m_updateButton->setEnabled(!path.isEmpty() && !rt.isCheckingUpdates());
}

void ServerSettingsWidget::updateServerState()
{
    const auto &srv = server();
    const QString url = QStringLiteral("http://%1:%2")
                            .arg(serverConnectAddress(settings().serverHost()))
                            .arg(settings().serverPort());
    const bool running = srv.state() == LlamaServer::State::Starting
                         || srv.state() == LlamaServer::State::Running;
    m_startStopButton->setIcon(
        lucideButtonIcon(running ? Lucide::Square : Lucide::Play, m_startStopButton));
    switch (srv.state()) {
    case LlamaServer::State::Stopped:
        m_startStopButton->setText(Tr::tr("Start server"));
        m_stateLabel->setText(Tr::tr("The server is stopped."));
        break;
    case LlamaServer::State::Starting:
        m_startStopButton->setText(Tr::tr("Stop server"));
        m_stateLabel->setText(Tr::tr("Starting the server..."));
        break;
    case LlamaServer::State::Running:
        m_startStopButton->setText(Tr::tr("Stop server"));
        m_stateLabel->setText(Tr::tr("The server is running on %1.").arg(url));
        break;
    case LlamaServer::State::Failed:
        m_startStopButton->setText(Tr::tr("Start server"));
        const QString error = srv.errorMessage();
        if (error.isEmpty()) {
            m_stateLabel->setText(Tr::tr("The server failed to start. See its output."));
        } else if (containsAnsiSequences(error)) {
            // The tail of the server log carries the ANSI color codes it
            // emitted; decode them into spans, the plain label would show
            // the raw escape sequences.
            m_stateLabel->setText(ansiToHtml(error));
        } else {
            m_stateLabel->setText(error);
        }
        break;
    }
    updateModelButtons();
}

void ServerSettingsWidget::updateChatHint()
{
    m_chatHint->setText(Tr::tr(
                           "To use this server for the chat, point the chat endpoint "
                           "(Chat settings page) at %1.")
                           .arg(QStringLiteral("http://%1:%2")
                                    .arg(settings().serverHost())
                                    .arg(settings().serverPort())));
}

int ServerSettingsWidget::modelRow(const QString &id) const
{
    for (int i = 0; i < m_modelList->count(); ++i) {
        if (m_modelList->item(i)->data(ModelIdRole).toString() == id)
            return i;
    }
    return -1;
}

ServerModel ServerSettingsWidget::modelAt(int row) const
{
    if (row < 0 || row >= m_modelList->count())
        return {};
    const QString id = m_modelList->item(row)->data(ModelIdRole).toString();
    const auto it = m_modelsById.constFind(id);
    return it != m_modelsById.constEnd() ? *it : ServerModel {};
}

ServerModel ServerSettingsWidget::selectedModel() const
{
    return modelAt(m_modelList->currentRow());
}

bool ServerSettingsWidget::isModelRunningStatus(const QString &status)
{
    return status == QLatin1String("loaded") || status == QLatin1String("loading")
           || status == QLatin1String("sleeping");
}

void ServerSettingsWidget::toggleModelLoad()
{
    const ServerModel model = selectedModel();
    if (model.id.isEmpty())
        return;
    if (isModelRunningStatus(model.status))
        server().unloadModel(model.id);
    else
        server().loadModel(model.id);
}

void ServerSettingsWidget::removeSelectedModel(const ServerModel &model)
{
    if (!model.canRemove)
        return;
    const int answer = QMessageBox::question(
        Core::ICore::dialogParent(),
        Tr::tr("Remove model"),
        Tr::tr("Remove the model \"%1\" from the server's model cache? The model's "
               "files are deleted; downloading again restores it.")
            .arg(model.id),
        QMessageBox::Yes | QMessageBox::No);
    if (answer == QMessageBox::Yes)
        server().removeModel(model.id);
}

void ServerSettingsWidget::updateModelButtons()
{
    QTC_ASSERT(m_modelList, return);
    const bool running = server().state() == LlamaServer::State::Running;
    const ServerModel model = selectedModel();
    const bool hasModel = !model.id.isEmpty();
    const bool modelRunning = hasModel && isModelRunningStatus(model.status);
    m_modelDownloadButton->setEnabled(running);
    m_modelRefreshButton->setEnabled(running);
    m_modelLoadButton->setText(modelRunning ? Tr::tr("Unload model") : Tr::tr("Load model"));
    m_modelLoadButton->setIcon(lucideButtonIcon(modelRunning ? Lucide::Square : Lucide::Play,
                                                m_modelLoadButton));
    m_modelLoadButton->setEnabled(running && hasModel
                                  && model.status != QLatin1String("downloading"));
    m_modelRemoveButton->setEnabled(running && hasModel && model.canRemove);
}

void ServerSettingsWidget::updateModelList()
{
    QTC_ASSERT(m_modelList, return);
    const auto state = server().state();
    const bool live = state == LlamaServer::State::Running
                      || state == LlamaServer::State::Starting;

    QList<ServerModel> models;
    if (live)
        models = server().models();
    else
        models = scanModelsDirectory(settings().serverModelsDir().toUserOutput());

    // The rows are updated in place (never rebuilt), so the scroll position and selection survive; the selection is tracked by id.
    const QString selectedId = modelAt(m_modelList->currentRow()).id;

    m_modelsById.clear();
    for (const ServerModel &model : models)
        m_modelsById.insert(model.id, model);

    // Drop the rows of the models that are gone (and the empty-state placeholder).
    for (int i = m_modelList->count() - 1; i >= 0; --i) {
        auto *item = m_modelList->item(i);
        const QString id = item->data(ModelIdRole).toString();
        const bool keep = id == kEmptyRowId ? models.isEmpty() : m_modelsById.contains(id);
        if (!keep) {
            // The item widget is a child of the viewport, not of the item: deleting the item alone would leak it.
            delete m_modelList->itemWidget(item);
            m_modelList->takeItem(i);
            delete item;
        }
    }

    // Update the surviving rows and append the new ones (appending keeps the positions stable).
    for (const ServerModel &model : models) {
        const int existingRow = modelRow(model.id);
        if (existingRow >= 0) {
            static_cast<ModelRow *>(m_modelList->itemWidget(m_modelList->item(existingRow)))
                ->updateModel(model, live);
        } else {
            auto *item = new QListWidgetItem(m_modelList);
            item->setData(ModelIdRole, model.id);
            auto *row = new ModelRow(model, live, m_modelList);
            item->setSizeHint(row->sizeHint());
            m_modelList->addItem(item);
            m_modelList->setItemWidget(item, row);
        }
    }

    if (models.isEmpty() && modelRow(kEmptyRowId) < 0) {
        auto *item = new QListWidgetItem(m_modelList);
        item->setData(ModelIdRole, kEmptyRowId);
        auto *empty = new QWidget(m_modelList);
        auto *emptyLayout = new QHBoxLayout(empty);
        emptyLayout->setContentsMargins(6, 5, 6, 5);
        auto *mark = new QLabel(empty);
        mark->setPixmap(lucideGlyph(Lucide::Bot, Qt::gray, 14).pixmap(14, 14));
        auto *label = new QLabel(
            live ? Tr::tr("The server reports no models.")
                 : Tr::tr("No models in the models directory."),
            empty);
        label->setEnabled(false);
        emptyLayout->addWidget(mark);
        emptyLayout->addWidget(label);
        emptyLayout->addStretch();
        item->setSizeHint(empty->sizeHint());
        m_modelList->setItemWidget(item, empty);
    }

    if (!selectedId.isEmpty()) {
        const int row = modelRow(selectedId);
        if (row >= 0)
            m_modelList->setCurrentRow(row);
    }

    m_modelHint->setText(live
                             ? Tr::tr("The models the server offers (from the models "
                                      "directory).")
                             : Tr::tr("The server is stopped: the models directory is "
                                      "listed as-is."));
    m_modelRefreshButton->setEnabled(live ? !server().isRefreshingModels() : true);
    updateModelButtons();
}

} // namespace LlamaCpp
