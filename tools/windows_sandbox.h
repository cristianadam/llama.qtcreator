// Windows sandbox backend for the chat tools, built on srt-win, the
// launcher binary of @anthropic-ai/sandbox-runtime (the same backend the
// pi coding agent's sandbox extension uses). See AGENTS.md, "Sandbox
// architecture", for the design and the platform quirks.
#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace LlamaCpp::Tools {

/// Result of building the Windows sandbox wrapper.
struct WindowsSandboxSpec
{
    QString program; // srt-win executable; empty when \a error is set
    QStringList arguments; // wrapper arguments; the shell and the command follow
    QString error; // actionable, non-empty on failure
};

/// Builds the `srt-win exec` wrapper that confines a command run in \a cwd.
/// The command runs as the dedicated sandbox user: no network egress (WFP
/// fence keyed on the user's SID), an isolated profile and temporary
/// directory, the home directory readable, and \a cwd plus the temporary
/// directory writable (granted for the plugin session, released on exit).
/// The credential locations are denied for the single exec. \a env is
/// passed to the sandboxed command as the `--env` overlay (the child
/// starts with the sandbox user's own profile environment only).
///
/// Portable on purpose (no Win32 calls; all the security work happens
/// inside srt-win) so the wrapper construction is unit-testable on any
/// platform.
WindowsSandboxSpec windowsSandboxSpec(const QString &cwd, const QProcessEnvironment &env);

} // namespace LlamaCpp::Tools
