#pragma once

#include <QtTaskTree/QTaskTree>

#include <utils/filepath.h>

// The ripgrep binary the search and find tools run. A published release never
// changes, so the checksum belongs to this version and no other.
namespace LlamaCpp::Tools::Ripgrep {

bool isSupportedPlatform();

QString version();
Utils::FilePath downloadDirectory();
bool isDownloaded();

// A usable ripgrep: the one on PATH, then the downloaded one, then empty.
Utils::FilePath resolvedPath();

// Fetches and unpacks the release for this platform, after asking for the
// license.
QtTaskTree::GroupItem downloadRecipe();

QString dialogTitle();

} // namespace LlamaCpp::Tools::Ripgrep
