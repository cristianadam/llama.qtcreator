#pragma once
#include "tool.h"

#include <utils/filepath.h>

namespace LlamaCpp::Tools {

// The Git for Windows bash the bash tool defaults to on Windows: next to the
// git executable on the PATH, then in the default Program Files installation
// directories. Empty when Git for Windows is not installed.
Utils::FilePath findGitBash();

// The bash the bash tool uses when the "bash" setting (Tools settings
// page) is empty: Git Bash (a recognized bash on the PATH, then the one
// shipped with Git for Windows) on Windows, /bin/bash on Unix.
Utils::FilePath defaultGitBash();

class BashState;

class BashTool : public Tool
{
public:
    QString name() const override;
    QString toolDefinition() const override;
    QString oneLineSummary(const QJsonObject &) const override;
    void run(const QJsonObject &arguments,
             std::function<void(const QString &output, bool ok)> done) const override;
    bool supportsLiveOutput() const override { return true; }
    void runLive(const QJsonObject &arguments,
                 const OutputHandler &onOutput,
                 std::function<void(const QString &output, bool ok)> done) const override;
    void abort() override;
    QString detailsMarkdown(const QJsonObject &arguments,
                                    const QString &result,
                                    bool ok) const override;
    QString summaryPreview(const QJsonObject &arguments,
                           const QString &result,
                           bool ok) const override;

private:
    //! Shared implementation of run()/runLive(); @p onOutput (may be
    //! empty) receives the output tail while the command runs.  Passed by
    //! value: the command runs asynchronously, long after the caller of
    //! runLive() has returned, so the tool must own its copy of the
    //! handler (a pointer to the caller's local would dangle).
    void runCommand(const QJsonObject &arguments,
                    std::function<void(const QString &output, bool ok)> done,
                    OutputHandler onOutput) const;

    //! The state of the currently running command (nullptr when idle), so
    //! abort() can kill the shell.
    mutable BashState *m_state = nullptr;
};

} // namespace LlamaCpp::Tools
