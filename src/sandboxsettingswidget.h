#pragma once

#include <utils/layoutbuilder.h>

#include <QCheckBox>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QWidget>

#include <coreplugin/dialogs/ioptionspage.h>

namespace LlamaCpp {

//! The "Sandbox" settings page: the master "Sandbox commands" toggle plus
//! the details of the sandbox that the chat tools run in.
//!
//! Filesystem – directories the sandbox grants on top of the project
//! directory and the temporary locations:
//!   - writable: the tools may write there (all platforms);
//!   - read-only: readable inside the sandbox (only the Windows process
//!     container restricts reads, so this takes effect on Windows);
//!   - deny-read: no tool may read (or write to) there, on top of the
//!     built-in credential locations, which are always protected.
//!
//! Network – whether sandboxed bash commands may use the network (off by
//! default). The webfetch and websearch tools always run outside the
//! sandbox and are not affected.
//!
//! The detail controls are disabled while the master toggle is off, since
//! the details only apply to sandboxed tools.
class SandboxSettingsWidget : public Core::IOptionsPageWidget
{
    Q_OBJECT
public:
    explicit SandboxSettingsWidget();
    void apply() override;
    void cancel() override;

private:
    // One directory list with its caption and Add / Remove buttons. The
    // Add button opens a directory chooser; items are editable in place.
    struct DirList
    {
        QLabel *label = nullptr;
        QListWidget *list = nullptr;
        QPushButton *addButton = nullptr;
        QPushButton *removeButton = nullptr;
    };
    DirList makeDirList(const QString &caption, const QString &tooltip);
    // Read the items of a list back into the matching setting.
    void saveLists();
    // (Re)populate the lists and the network toggle from the settings.
    void loadFromSettings();
    // Enable / disable the detail controls from the master toggle state.
    void applyMasterState();

    // UI
    QCheckBox *m_masterCheck = nullptr;
    QCheckBox *m_networkCheck = nullptr;
    DirList m_writable;
    DirList m_readOnly;
    DirList m_denyRead;
    QWidget *m_detailsContainer = nullptr; // the groups gated by the master
};

} // namespace LlamaCpp
