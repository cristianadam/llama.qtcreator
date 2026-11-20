#include "serversettingswidget.h"

#include "llamaoutputpane.h"
#include "llamasettings.h"
#include "llamaserver.h"
#include "llamaruntime.h"
#include "llamatr.h"
#include "modellogos.h"

#include <coreplugin/icore.h>
#include <utils/layoutbuilder.h>
#include <utils/qtcassert.h>

#include <QCheckBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QScreen>
#include <QPushButton>
#include <QProgressDialog>
#include <QTimer>
#include <QVBoxLayout>

namespace LlamaCpp {
using namespace Layouting;

static QString link(const char *url)
{
    return QString("<a href=\"%1\">%1</a>").arg(QLatin1String(url));
}

// Renders a glyph of the bundled lucide icon font as a pixmap (an icon-font
// character in a themed settings widget would show up verbatim).
static QIcon lucideGlyph(QChar glyph, const QColor &color, int pixelSize)
{
    const qreal dpr = QGuiApplication::primaryScreen()
                          ? QGuiApplication::primaryScreen()->devicePixelRatio()
                          : 1.0;
    QFont font(QStringLiteral("lucideicons_outline"));
    font.setPixelSize(pixelSize);
    const int physicalSize = int(pixelSize * dpr);
    QPixmap pixmap(physicalSize, physicalSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRectF(0, 0, pixelSize, pixelSize), Qt::AlignCenter, QString(glyph));
    return QIcon(pixmap);
}

static QIcon lucideButtonIcon(QChar glyph, const QPushButton *button)
{
    return lucideGlyph(glyph, button->palette().color(QPalette::WindowText), 16);
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

// One model of the Server page's list: logo, name, details, modality
// marks, and - while the server runs - the live status.
class ModelRow : public QWidget
{
public:
    ModelRow(const ServerModel &model, bool live, QWidget *parent)
        : QWidget(parent)
    {
        auto *layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 5, 6, 5);
        layout->setSpacing(8);

        auto *logo = new QLabel(this);
        logo->setFixedSize(28, 28);
        logo->setScaledContents(true);
        logo->setFrameShape(QFrame::StyledPanel);
        logo->setStyleSheet(QStringLiteral("border-radius: 6px;"));
        logo->setPixmap(QIcon(modelLogoResource(model.id)).pixmap(28, 28));
        layout->addWidget(logo, 0, Qt::AlignVCenter);

        auto *column = new QWidget(this);
        auto *columnLayout = new QVBoxLayout(column);
        columnLayout->setContentsMargins(0, 0, 0, 0);
        columnLayout->setSpacing(1);
        auto *name = new QLabel(model.id, column);
        name->setToolTip(model.path);
        QFont nameFont = name->font();
        nameFont.setWeight(QFont::DemiBold);
        name->setFont(nameFont);
        columnLayout->addWidget(name);
        const QString details = modelDetails(model);
        if (!details.isEmpty()) {
            auto *sub = new QLabel(details, column);
            sub->setStyleSheet(QStringLiteral("color: gray; font-size: 11px;"));
            columnLayout->addWidget(sub);
        }
        layout->addWidget(column, 1);

        // The modality marks (the lucide eye/ear/video glyphs); "text" is
        // not marked.
        for (const QString &modality : model.inputModalities) {
            const QChar glyph = modality == QLatin1String("image")
                                    ? QChar('2')
                                    : modality == QLatin1String("audio")
                                          ? QChar('6')
                                          : modality == QLatin1String("video")
                                                ? QChar('3')
                                                : QChar();
            if (glyph.isNull())
                continue;
            auto *mark = new QLabel(this);
            mark->setPixmap(lucideGlyph(glyph, Qt::gray, 13).pixmap(13, 13));
            mark->setToolTip(modality);
            layout->addWidget(mark, 0, Qt::AlignVCenter);
        }

        if (live) {
            const QChar glyph = statusGlyph(model.status);
            const QColor color = statusColor(model.status);
            auto *status = new QWidget(this);
            auto *statusLayout = new QHBoxLayout(status);
            statusLayout->setContentsMargins(0, 0, 0, 0);
            statusLayout->setSpacing(4);
            if (!glyph.isNull()) {
                auto *icon = new QLabel(status);
                icon->setPixmap(lucideGlyph(glyph, color, 13).pixmap(13, 13));
                statusLayout->addWidget(icon);
            }
            auto *text = new QLabel(statusText(model.status), status);
            text->setStyleSheet(QStringLiteral("color: %1; font-size: 11px;")
                                     .arg(color.name()));
            statusLayout->addWidget(text);
            layout->addWidget(status, 0, Qt::AlignVCenter);
        }
    }

private:
    static QChar statusGlyph(const QString &status)
    {
        if (status == QLatin1String("loaded") || status == QLatin1String("sleeping"))
            return QChar('Z'); // circle-check
        if (status == QLatin1String("loading") || status == QLatin1String("downloading")
            || status == QLatin1String("incomplete"))
            return QChar('0'); // loader-circle
        if (status == QLatin1String("failed"))
            return QChar('4'); // circle-alert
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
            return Tr::tr("loading…");
        if (status == QLatin1String("downloading") || status == QLatin1String("incomplete"))
            return Tr::tr("downloading…");
        if (status == QLatin1String("failed"))
            return Tr::tr("failed");
        if (status == QLatin1String("sleeping"))
            return Tr::tr("sleeping");
        if (status == QLatin1String("loaded"))
            return Tr::tr("loaded");
        return Tr::tr("not loaded");
    }
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
    m_installButton = new QPushButton(Tr::tr("Download llama.cpp…"), this);
    m_installButton->setIcon(lucideButtonIcon(QChar('1'), m_installButton)); // download
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
    m_modelRefreshButton = new QPushButton(Tr::tr("Refresh"), this);
    m_modelRefreshButton->setFlat(true);
    m_modelRefreshButton->setIcon(
        lucideButtonIcon(QChar('A'), m_modelRefreshButton)); // refresh-cw
    m_modelRefreshButton->setToolTip(Tr::tr(
        "Re-reads the model list. While the server runs, it also makes the "
        "server rescan the models directory, so new .gguf files show up "
        "without a restart."));
    m_modelList = new QListWidget(this);
    m_modelList->setMinimumHeight(120);
    m_modelList->setMaximumHeight(260);
    m_modelList->setSelectionMode(QAbstractItemView::NoSelection);
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
    m_showOutputButton = new QPushButton(Tr::tr("Show output"), this);
    m_showOutputButton->setToolTip(Tr::tr(
        "Shows the managed server's terminal output (stdout and stderr) in "
        "the \"llama.cpp\" pane of the Output window: the start command "
        "line, then the colored log; filterable and clearable."));

    m_chatHint = new QLabel(this);
    m_chatHint->setWordWrap(true);

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
                Row {m_modelHint, st, m_modelRefreshButton},
                m_modelList,
            },
        }.emerge(),
        Group {
            title(Tr::tr("Control")),
            Column {
                Row {m_startStopButton, m_stateLabel, st},
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
        m_installDialog = new QProgressDialog(Tr::tr("Installing llama.cpp…"),
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
        lucideButtonIcon(running ? QChar('I') : QChar('Y'), m_startStopButton));
    switch (srv.state()) {
    case LlamaServer::State::Stopped:
        m_startStopButton->setText(Tr::tr("Start server"));
        m_stateLabel->setText(Tr::tr("The server is stopped."));
        break;
    case LlamaServer::State::Starting:
        m_startStopButton->setText(Tr::tr("Stop server"));
        m_stateLabel->setText(Tr::tr("Starting the server…"));
        break;
    case LlamaServer::State::Running:
        m_startStopButton->setText(Tr::tr("Stop server"));
        m_stateLabel->setText(Tr::tr("The server is running on %1.").arg(url));
        break;
    case LlamaServer::State::Failed:
        m_startStopButton->setText(Tr::tr("Start server"));
        m_stateLabel->setText(srv.errorMessage().isEmpty()
                                  ? Tr::tr("The server failed to start. See its output.")
                                  : srv.errorMessage());
        break;
    }
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

    // The item widgets are children of the viewport, not of the items, so
    // deleting the items alone would leak them.
    while (m_modelList->count() > 0) {
        auto *item = m_modelList->item(0);
        auto *widget = m_modelList->itemWidget(item);
        m_modelList->takeItem(0);
        delete widget;
        delete item;
    }

    if (models.isEmpty()) {
        auto *item = new QListWidgetItem(m_modelList);
        auto *empty = new QWidget(m_modelList);
        auto *emptyLayout = new QHBoxLayout(empty);
        emptyLayout->setContentsMargins(6, 5, 6, 5);
        auto *mark = new QLabel(empty);
        mark->setPixmap(lucideGlyph(QChar('5'), Qt::gray, 14).pixmap(14, 14)); // bot
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
    } else {
        for (const ServerModel &model : models) {
            auto *item = new QListWidgetItem(m_modelList);
            auto *row = new ModelRow(model, live, m_modelList);
            item->setSizeHint(row->sizeHint());
            m_modelList->setItemWidget(item, row);
        }
    }

    m_modelHint->setText(live
                             ? Tr::tr("The models the server offers (from the models "
                                      "directory).")
                             : Tr::tr("The server is stopped: the models directory is "
                                      "listed as-is."));
    m_modelRefreshButton->setEnabled(live ? !server().isRefreshingModels() : true);
}

} // namespace LlamaCpp
