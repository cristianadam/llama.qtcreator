#include "sandboxstatus.h"

#include "llamasettings.h"
#include "llamatr.h"
#include "tools/mxc.h"

#include <coreplugin/messagemanager.h>

#include <projectexplorer/project.h>
#include <projectexplorer/projectmanager.h>
#include <projectexplorer/task.h>
#include <projectexplorer/taskhub.h>

#include <utils/filepath.h>
#include <utils/hostosinfo.h>

#include <QStandardPaths>

using namespace ProjectExplorer;
using namespace Utils;

namespace LlamaCpp::SandboxStatus {

const char kTaskCategory[] = "LlamaCpp.Sandbox";

QString unavailableReason()
{
    if (HostOsInfo::isWindowsHost()) {
        if (!Tools::Mxc::isSupportedPlatform())
            return Tr::tr("the Microsoft MXC runtime (wxc-exec) the Windows sandbox "
                          "uses is available for Windows x86-64 only");
        if (Tools::Mxc::resolvedPath().isEmpty())
            return Tr::tr("the Microsoft MXC runtime (wxc-exec) is not installed; "
                          "download it on the Llama Tools settings page");
        return {};
    }
    if (HostOsInfo::isMacHost()) {
        if (QStandardPaths::findExecutable("sandbox-exec").isEmpty())
            return Tr::tr("the sandbox-exec executable was not found");
        return {};
    }
    if (QStandardPaths::findExecutable("bwrap").isEmpty())
        return Tr::tr("bubblewrap (bwrap) was not found; install it (e.g. "
                      "'apt install bubblewrap' or 'dnf install bubblewrap')");
    return {};
}

static bool sandboxRequested()
{
    if (Project *project = ProjectManager::startupProject())
        return LlamaProjectSettings(project).isSandboxEnabled();
    return settings().sandboxCommands();
}

void check()
{
    static QString lastReason;

    const QString reason = sandboxRequested() ? unavailableReason() : QString();
    if (reason == lastReason)
        return;
    lastReason = reason;

    TaskHub::clearTasks(kTaskCategory);
    if (reason.isEmpty())
        return;

    const QString message = Tr::tr("\"Sandbox commands\" is enabled, but %1. The bash "
                                   "tool refuses to run commands until the sandbox is "
                                   "available or the setting is disabled.")
                                .arg(reason);
    Core::MessageManager::writeFlashing(Tr::tr("[llama.cpp] %1").arg(message));
    TaskHub::addTask(Task(Task::Warning, message, FilePath(), -1, kTaskCategory));
}

void setup()
{
    TaskHub::addCategory({kTaskCategory,
                          Tr::tr("Llama.cpp Sandbox"),
                          Tr::tr("Issues with the sandbox of the llama.cpp chat tools.")});

    settings().sandboxCommands.addOnChanged(ProjectManager::instance(), &check);
    QObject::connect(ProjectManager::instance(),
                     &ProjectManager::startupProjectChanged,
                     ProjectManager::instance(),
                     &check);
    check();
}

} // namespace LlamaCpp::SandboxStatus
