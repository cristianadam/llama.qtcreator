#pragma once

#include <QString>

// Warns (General Messages and the Issues pane) when "Sandbox commands" is
// enabled but the sandbox backend it needs (bubblewrap on Linux,
// sandbox-exec on macOS, the Microsoft MXC runtime on Windows) is missing,
// as the sandboxed bash tool would then refuse every command.
namespace LlamaCpp::SandboxStatus {

// Empty when the sandbox backend of this host is usable, otherwise why not.
QString unavailableReason();

// Registers the Issues category and re-checks whenever the setting, the
// startup project or its project settings change.
void setup();

// Re-evaluates and updates the warning; does nothing when unchanged.
void check();

} // namespace LlamaCpp::SandboxStatus
