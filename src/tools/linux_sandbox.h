// Linux sandbox backend for the chat tools, built on bubblewrap (bwrap,
// https://github.com/containers/bwrap): the command runs against a
// read-only view of the whole file system (the working directory and the
// temporary locations stay writable), in fresh pid/ipc/uts namespaces,
// with the credential locations hidden and - by default - no network
// access. The Linux counterpart of windows_sandbox.h.
#pragma once

#include <QString>
#include <QStringList>

namespace LlamaCpp::Tools {

/// Result of building the Linux sandbox wrapper.
struct LinuxSandboxSpec
{
    QString program; // bwrap executable; empty when \a error is set
    QStringList arguments; // wrapper arguments; the shell and the command
    // follow them
    QString error; // actionable, non-empty on failure
};

/// The bubblewrap executable: the LLAMA_BWRAP override (the tests point it
/// at a fake bwrap, a developer at a build of their own), then bwrap on the
/// PATH. Empty when there is none.
QString findBwrap();

/// Builds the `bwrap` wrapper that confines a command run in \a cwd: the
/// whole file system is read-only except the working directory and the
/// temporary locations, the credential locations are hidden, and the
/// command gets fresh pid/ipc/uts namespaces and (unless the Sandbox
/// settings page allows it) no network access.
///
/// Before building the wrapper, the bwrap executable is verified with a
/// one-shot functional probe (a minimal sandbox running a no-op): finding
/// bwrap is not enough, as the kernel can still refuse the sandbox setup -
/// in particular Ubuntu's AppArmor restriction of unprivileged user
/// namespaces, which lets bwrap create its namespaces but denies setting
/// them up. The successful result is cached for the session; a failure
/// re-probes on every call, so a fix applied while the editor is running
/// (e.g. 'sudo sysctl -w
/// kernel.apparmor_restrict_unprivileged_userns=0') takes effect with the
/// next command. When the probe fails, \a error is set and the known
/// failure signatures are turned into actionable messages.
///
/// Portable on purpose (no Linux-specific calls) so the wrapper
/// construction and the probe handling are unit-testable on any platform.
LinuxSandboxSpec linuxSandboxSpec(const QString &cwd);

} // namespace LlamaCpp::Tools
