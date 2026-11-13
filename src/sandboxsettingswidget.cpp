#include "sandboxsettingswidget.h"

#include "llamasettings.h"
#include "llamatr.h"
#include "sandboxstatus.h"
#include "tools/mxc.h"

#include <QtTaskTree/QTaskTree>
#include <QtTaskTree/qtasktreerunner.h>

#include <QDir>
#include <QFileDialog>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>

#include <utils/infolabel.h>

using namespace Layouting;

namespace LlamaCpp {

SandboxSettingsWidget::SandboxSettingsWidget()
{
    // Master toggle: whether the chat tools run in a sandbox at all. The
    // details below only apply while it is on, so they are enabled /
    // disabled from its state.
    m_masterCheck = new QCheckBox(settings().sandboxCommands.displayName(), this);
    m_masterCheck->setToolTip(settings().sandboxCommands.toolTip());
    m_masterCheck->setChecked(settings().sandboxCommands());
    connect(m_masterCheck, &QCheckBox::toggled, this, [this](bool checked) {
        settings().sandboxCommands.setValue(checked);
        applyMasterState();
    });

    // Shown while the sandbox backend of this host (bubblewrap on Linux,
    // sandbox-exec on macOS, the Microsoft MXC runtime on Windows) is
    // missing, the way the code style selector of the C++ page warns
    // about a read-only configuration: the master toggle and the details
    // are disabled then, as the sandbox could not be used in the first
    // place.
    m_backendWarning = new Utils::InfoLabel(QString(), Utils::InfoLabelType::Warning, this);
    m_backendWarning->setVisible(false);

    m_writable = makeDirList(
        Tr::tr("Writable directories:"),
        Tr::tr("Directories the sandboxed tools may write to, in addition "
               "to the project directory and the temporary locations."));
    m_readOnly = makeDirList(
        Tr::tr("Read-only directories:"),
        Tr::tr("Additional directories readable inside the sandbox. Only "
               "the Windows process container restricts reads (the other "
               "platforms already expose the file system read-only), so "
               "this takes effect on Windows."));
    m_denyRead = makeDirList(
        Tr::tr("Deny-read directories:"),
        Tr::tr("Additional directories that no tool may read (or write to), "
               "on top of the built-in credential locations, which are "
               "always protected."));

    // Network policy of the sandboxed commands.
    m_networkCheck = new QCheckBox(settings().sandboxAllowNetwork.displayName(), this);
    m_networkCheck->setToolTip(settings().sandboxAllowNetwork.toolTip());
    m_networkCheck->setChecked(settings().sandboxAllowNetwork());
    connect(m_networkCheck, &QCheckBox::toggled, this, [](bool checked) {
        settings().sandboxAllowNetwork.setValue(checked);
    });
    auto *networkNote = new QLabel(Tr::tr(
        "The webfetch and websearch tools always run outside the sandbox "
        "and are not affected by this setting."),
        this);
    networkNote->setWordWrap(true);

    using namespace Layouting;

    // The Windows sandbox runs commands in a Microsoft MXC process
    // container; like the terminal plugin's console host, a pinned
    // wxc-exec.exe can be downloaded here, or a runtime of one's own can
    // be pointed to (the group is shown on supported platforms only).
    QWidget *mxcGroup = nullptr;
    if (Tools::Mxc::isSupportedPlatform()) {
        m_mxcStatus = new QLabel(this);
        m_mxcStatus->setWordWrap(true);
        m_mxcDownloadButton = new QPushButton(Tr::tr("Download..."), this);

        // The path chooser is for a runtime of one's own; when it is
        // empty the downloaded one is taken, and showing that as the
        // placeholder is what tells where it came from.
        const auto showMxc = [this] {
            const Utils::FilePath inUse = Tools::Mxc::resolvedPath();
            m_mxcStatus->setText(inUse.isEmpty()
                ? Tr::tr("The sandboxed bash tool on Windows uses the Microsoft "
                         "MXC runtime (wxc-exec), which is not installed on this "
                         "system.")
                : Tr::tr("The sandboxed bash tool on Windows runs in a Microsoft "
                         "MXC process container."));
            settings().mxcPath.setPlaceHolderText(inUse.toUserOutput());
            updateBackendWarning();
        };
        showMxc();
        connect(&settings().mxcPath, &Utils::BaseAspect::changed, this, showMxc);

        const auto mxcDownloader = std::make_shared<QtTaskTree::QSingleTaskTreeRunner>();
        connect(m_mxcDownloadButton,
                &QPushButton::clicked,
                m_mxcDownloadButton,
                [this, showMxc, mxcDownloader] {
                    m_mxcDownloadButton->setEnabled(false);
                    mxcDownloader->start({Tools::Mxc::downloadRecipe()}, {},
                                         [this, showMxc, mxcDownloader](
                                             QtTaskTree::DoneWith result) {
                                             m_mxcDownloadButton->setEnabled(true);
                                             if (result == QtTaskTree::DoneWith::Success)
                                                 settings().mxcPath.setValue(
                                                     Tools::Mxc::downloadedPath());
                                             showMxc();
                                             SandboxStatus::check();
                                         });
                });

        mxcGroup = Group{
            title(Tr::tr("Windows runtime")),
            Column{
                m_mxcStatus,
                settings().mxcPath,
                Row{m_mxcDownloadButton, st},
            },
        }.emerge();
    }

    // The detail groups are gated by the master toggle, so they live in a
    // single container that is enabled / disabled as a whole.
    m_detailsContainer = new QWidget(this);
    Group fsGroup {
        Column{
            m_writable.label,
            Row{
                m_writable.list,
                Column{m_writable.addButton, m_writable.removeButton, st},
            },
            br,
            m_readOnly.label,
            Row{
                m_readOnly.list,
                Column{m_readOnly.addButton, m_readOnly.removeButton, st},
            },
            br,
            m_denyRead.label,
            Row{
                m_denyRead.list,
                Column{m_denyRead.addButton, m_denyRead.removeButton, st},
            },
        },
    };
    fsGroup.setTitle(Tr::tr("Filesystem"));

    Group netGroup {
        Column{
            m_networkCheck,
            networkNote,
        },
    };
    netGroup.setTitle(Tr::tr("Network"));

    // The Windows runtime group leads the details (a null item on other
    // platforms, which the layout drops, the way the terminal plugin does
    // for its console host), then the filesystem and the network policy.
    Column{
        mxcGroup,
        fsGroup,
        br,
        netGroup,
    }.attachTo(m_detailsContainer);

    // Layout: the master toggle (and the warning shown while its backend
    // is missing), then the (gated) details.
    Column{
        m_masterCheck,
        m_backendWarning,
        m_detailsContainer,
        st,
    }.attachTo(this);

    loadFromSettings();
    applyMasterState();
    updateBackendWarning();
}

SandboxSettingsWidget::DirList SandboxSettingsWidget::makeDirList(const QString &caption,
                                                                  const QString &tooltip)
{
    DirList dir;
    dir.label = new QLabel(caption, this);
    dir.label->setToolTip(tooltip);

    dir.list = new QListWidget(this);
    dir.list->setToolTip(tooltip);
    dir.list->setFixedHeight(90);
    dir.list->setDragDropMode(QListWidget::NoDragDrop);
    // Items are editable in place (double-click or F2) so a path can be
    // corrected without removing and re-adding it.
    connect(dir.list, &QListWidget::itemChanged, this, [this, dir](QListWidgetItem *item) {
        Q_UNUSED(item);
        saveLists();
    });

    dir.addButton = new QPushButton(Tr::tr("Add..."), this);
    connect(dir.addButton, &QPushButton::clicked, this, [this, dir] {
        const QString path = QFileDialog::getExistingDirectory(
            this, Tr::tr("Choose a Directory"), QDir::homePath());
        if (path.isEmpty())
            return;
        if (!dir.list->findItems(path, Qt::MatchExactly).isEmpty())
            return; // already listed
        auto *item = new QListWidgetItem(path, dir.list);
        item->setFlags(item->flags() | Qt::ItemIsEditable);
        saveLists();
    });

    dir.removeButton = new QPushButton(Tr::tr("Remove"), this);
    dir.removeButton->setEnabled(false);
    connect(dir.list, &QListWidget::itemSelectionChanged, this, [dir] {
        dir.removeButton->setEnabled(!dir.list->selectedItems().isEmpty());
    });
    connect(dir.removeButton, &QPushButton::clicked, this, [this, dir] {
        const QList<QListWidgetItem *> selected = dir.list->selectedItems();
        for (QListWidgetItem *item : selected)
            delete dir.list->takeItem(dir.list->row(item));
        saveLists();
    });

    return dir;
}

void SandboxSettingsWidget::saveLists()
{
    auto store = [](QListWidget *list, Utils::StringListAspect &aspect) {
        QStringList paths;
        for (int i = 0; i < list->count(); ++i) {
            const QString text = list->item(i)->text().trimmed();
            if (!text.isEmpty())
                paths << text;
        }
        aspect.setValue(paths);
    };
    store(m_writable.list, settings().sandboxWritableDirs);
    store(m_readOnly.list, settings().sandboxReadOnlyDirs);
    store(m_denyRead.list, settings().sandboxDenyReadDirs);
}

void SandboxSettingsWidget::loadFromSettings()
{
    auto fill = [](QListWidget *list, const QStringList &paths) {
        list->clear();
        for (const QString &path : paths) {
            auto *item = new QListWidgetItem(path, list);
            item->setFlags(item->flags() | Qt::ItemIsEditable);
        }
    };
    fill(m_writable.list, settings().sandboxWritableDirs());
    fill(m_readOnly.list, settings().sandboxReadOnlyDirs());
    fill(m_denyRead.list, settings().sandboxDenyReadDirs());
    m_networkCheck->setChecked(settings().sandboxAllowNetwork());
}

void SandboxSettingsWidget::applyMasterState()
{
    // The details apply only while the master toggle is on and usable;
    // the toggle is disabled (and the warning shown) while the sandbox
    // backend of this host is missing.
    const bool enabled = m_masterCheck->isEnabled() && m_masterCheck->isChecked();
    m_detailsContainer->setEnabled(enabled);
}

void SandboxSettingsWidget::updateBackendWarning()
{
    // The reason is empty when the sandbox backend of this host is
    // usable, otherwise it says why not (bubblewrap or sandbox-exec not
    // installed, the MXC runtime not downloaded on Windows).
    const QString reason = SandboxStatus::unavailableReason();
    m_backendWarning->setVisible(!reason.isEmpty());
    if (!reason.isEmpty())
        m_backendWarning->setText(reason);
    m_masterCheck->setEnabled(reason.isEmpty());
    applyMasterState();
}

void SandboxSettingsWidget::apply()
{
    // Persist the latest list state (the toggles write on change already).
    saveLists();
}

void SandboxSettingsWidget::cancel()
{
    // Re-load the stored values - this discards any unapplied UI changes.
    settings().readSettings();
    m_masterCheck->setChecked(settings().sandboxCommands());
    loadFromSettings();
    applyMasterState();
}

} // namespace LlamaCpp
