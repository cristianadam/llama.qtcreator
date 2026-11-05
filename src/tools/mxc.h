#pragma once

#include <QtTaskTree/QTaskTree>

#include <utils/filepath.h>

// The Microsoft MXC runtime (wxc-exec.exe) the Windows sandbox of the chat
// tools runs on. A pinned release is downloaded and checksum-verified, the
// same way the ripgrep binary is (tools/ripgrep.*).
namespace LlamaCpp::Tools::Mxc {

// MXC's process container tier exists only on Windows x86-64.
bool isSupportedPlatform();

QString version();
Utils::FilePath downloadDirectory();
bool isDownloaded();

// The wxc-exec.exe of the downloaded runtime (empty while it is not
// installed).
Utils::FilePath downloadedPath();

// A usable wxc-exec.exe: the one chosen on the Sandbox settings page, then
// the downloaded one, then empty.
Utils::FilePath resolvedPath();

// Fetches and unpacks the pinned release, after asking for the license.
QtTaskTree::GroupItem downloadRecipe();

QString dialogTitle();

} // namespace LlamaCpp::Tools::Mxc
