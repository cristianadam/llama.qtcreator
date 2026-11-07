#pragma once

#include <QtTaskTree/QTaskTree>

#include <utils/filepath.h>

// The niubash shell (https://github.com/unixwin/niubash) the bash tool runs
// on Windows. niubash is a native, MSYS2/Cygwin-free Bash, so - unlike Git
// Bash, whose msys-2.0.dll cannot create its session object directory in an
// AppContainer (microsoft/mxc#1061) - it initializes inside the Microsoft MXC
// process container. A published release never changes, so the checksum
// belongs to this version and no other.
namespace LlamaCpp::Tools::Niubash {

// niubash ships a Windows build for x86-64 and arm64 only.
bool isSupportedPlatform();

QString version();
Utils::FilePath downloadDirectory();
bool isDownloaded();

// The bash launcher (winuxcmd\bin\bash.exe) of the downloaded release (empty
// while it is not installed).
Utils::FilePath downloadedPath();

// A usable niubash bash: the one chosen on the Tools settings page, then the
// downloaded one, then empty.
Utils::FilePath resolvedPath();

// Fetches and unpacks the release for this platform, after asking for the
// license.
QtTaskTree::GroupItem downloadRecipe();

QString dialogTitle();

} // namespace LlamaCpp::Tools::Niubash
