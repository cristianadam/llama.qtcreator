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

/// Result of locating the installation root of the sandboxed shell.
struct ShellInstallRoot
{
    QString root; // the directory granted read-only; empty when \a error is set
    QString error; // actionable, non-empty when the shell is not recognized
};

/// The installation root of a Git for Windows or MSYS2 bash (\a shellPath
/// is <root>\usr\bin\bash.exe or <root>\bin\bash.exe). On Windows the root
/// must also look like such an installation (usr\bin\bash.exe plus
/// git-cmd.exe, cmd\git.exe, msys2.exe or msys2_shell.cmd) and must not be
/// a drive root, the Windows directory or cover the user profile; anything
/// else fails closed. Elsewhere only the path layout is checked (so the
/// wrapper construction stays testable).
ShellInstallRoot windowsShellInstallRoot(const QString &shellPath);

/// Quotes a single argument for a Windows command line (the MSVCRT
/// CreateProcessW rules): an argument with whitespace or quotes is wrapped
/// in quotes, a run of backslashes before a quote (or the end of the
/// argument) is doubled (an odd run before the closing quote would escape
/// it), and embedded quotes are backslash-escaped.
QString quoteWindowsArgument(const QString &argument);

/// Builds the `wxc-exec --config-base64` wrapper that confines
/// \a commandLine (the full shell command line, already quoted) run in \a
/// cwd: the working directory and the temporary location are writable, the
/// shell's own tree (\a shellPath, e.g. Git Bash) and the Windows system
/// directory are readable, nothing else exists in the container, and there
/// is no network access. \a env is the environment the sandboxed command
/// gets (the container starts with no host environment; credential
/// variables are dropped, see withoutSecretVariables()); \a timeoutMs is the
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
