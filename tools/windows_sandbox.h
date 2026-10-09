// Windows sandbox backend for the chat tools, built on Microsoft MXC
// (wxc-exec.exe, https://github.com/microsoft/mxc): the command runs in a
// process container that sees only the granted paths (the working directory
// and the temporary location writable, the shell's own tree and the system
// directory readable) and has no network access. See AGENTS.md, "Sandbox
// architecture", for the design and the platform quirks.
#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace LlamaCpp::Tools {

/// Result of building the Windows sandbox wrapper.
struct WindowsSandboxSpec
{
    QString program; // wxc-exec executable; empty when \a error is set
    QStringList arguments; // wrapper arguments; complete - the command line is
    // embedded in the sandbox configuration, nothing follows
    QString error; // actionable, non-empty on failure
};

/// Quotes a single argument for a Windows command line (the MSVCRT
/// CreateProcessW rules): an argument with whitespace or quotes is wrapped
/// in quotes, every backslash is doubled (an odd run before the closing
/// quote would escape it), and embedded quotes are backslash-escaped.
QString quoteWindowsArgument(const QString &argument);

/// Builds the `wxc-exec --config-base64` wrapper that confines
/// \a commandLine (the full shell command line, already quoted) run in \a
/// cwd: the working directory and the temporary location are writable, the
/// shell's own tree (\a shellPath, e.g. Git Bash) and the Windows system
/// directory are readable, nothing else exists in the container, and there
/// is no network access. \a env is the environment the sandboxed command
/// gets (the container starts with no host environment); \a timeoutMs is the
/// container's backstop timeout for the command.
///
/// Portable on purpose (no Win32 calls; all the security work happens inside
/// wxc-exec) so the wrapper construction is unit-testable on any platform.
WindowsSandboxSpec windowsSandboxSpec(const QString &cwd,
                                      const QProcessEnvironment &env,
                                      const QString &commandLine,
                                      int timeoutMs,
                                      const QString &shellPath);

} // namespace LlamaCpp::Tools
