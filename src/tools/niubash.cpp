#include "niubash.h"

#include "llamasettings.h"
#include "llamatr.h"

#include <coreplugin/icore.h>

#include <QtTaskTree/QNetworkReplyWrapper>

#include <utils/async.h>
#include <utils/hostosinfo.h>
#include <utils/networkaccessmanager.h>
#include <utils/qtcassert.h>
#include <utils/temporarydirectory.h>
#include <utils/unarchiver.h>
#include <utils/widgets.h>

#include <QCryptographicHash>
#include <QFile>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStandardPaths>
#include <QSysInfo>

using namespace QtTaskTree;
using namespace Utils;

namespace LlamaCpp::Tools::Niubash {

// The niubash release the bash tool is pinned to. A published release never
// changes, so the checksum belongs to this version and no other.
const char packageVersion[] = "1.5.0";
const char packageProject[] = "https://github.com/unixwin/niubash";
const char packageLicense[] = "https://github.com/unixwin/niubash/blob/master/LICENSE";

// The portable package is a 7z archive (the smallest of the published
// formats, and hardlink-native). One build per Windows architecture, each
// pinned to the SHA-256 published in the release's SHA256SUMS.
struct Asset
{
    const char *target; // nullptr for unsupported platforms
    const char *sha256;
};

static const Asset &asset()
{
    static const Asset none{nullptr, nullptr};
    static const Asset winX86_64{"win-x64",
                                 "46f4781038bea9bbf62ebffd1ddc21ccd96a6db0b6e96db658776432e37667c0"};
    static const Asset winAarch64{"win-arm64",
                                  "1faeee6f8da437ed3b0d13a26fb46c3d23341efb7a4698e8205d99135de6559b"};

    const QString architecture = QSysInfo::buildCpuArchitecture();
    const bool isAarch64 = (architecture == QLatin1String("arm64")
                            || architecture == QLatin1String("aarch64"));
    const bool isX86_64 = architecture == QLatin1String("x86_64");
    if (!isAarch64 && !isX86_64)
        return none;
    if (!HostOsInfo::isWindowsHost())
        return none;
    return isAarch64 ? winAarch64 : winX86_64;
}

static QString archiveName()
{
    return QStringLiteral("niubash-v%1-%2.7z")
            .arg(QLatin1String(packageVersion), QLatin1String(asset().target));
}

// The top-level directory the archive unpacks into.
static QString topDirectory()
{
    return QStringLiteral("niubash-v%1-%2")
            .arg(QLatin1String(packageVersion), QLatin1String(asset().target));
}

QString dialogTitle()
{
    return Tr::tr("Download niubash");
}

QString version()
{
    return QLatin1String(packageVersion);
}

bool isSupportedPlatform()
{
    return asset().target != nullptr;
}

FilePath downloadDirectory()
{
    // The release is installed with its contents directly under this
    // (versioned) directory: niu.exe and the winuxcmd tree.
    return Core::ICore::userResourcePath("niubash") / QLatin1String(packageVersion);
}

static bool holdsShell(const FilePath &root)
{
    // Both the launcher and the engine it execs must be present: the bash
    // shim resolves ../../niu.exe relative to itself.
    return (root / QStringLiteral("winuxcmd") / QStringLiteral("bin")
            / QStringLiteral("bash.exe"))
                   .isFile()
           && (root / QStringLiteral("niu.exe")).isFile();
}

bool isDownloaded()
{
    return holdsShell(downloadDirectory());
}

FilePath downloadedPath()
{
    return isDownloaded()
           ? downloadDirectory() / QStringLiteral("winuxcmd") / QStringLiteral("bin")
                         / QStringLiteral("bash.exe")
           : FilePath();
}

FilePath resolvedPath()
{
    // The "bash" setting (Tools settings page) when it points at a niubash:
    // the downloaded release is pinned there by the Download... button and
    // by "Reset to niubash".
    const FilePath configured = settings().bashPath();
    if (!configured.isEmpty() && configured.exists())
        return configured;

    return downloadedPath();
}

static void warn(const QString &error)
{
    QMessageBox::warning(Core::ICore::dialogParent(), dialogTitle(), error);
}

static QString link(const char *url)
{
    return QString("<a href=\"%1\">%1</a>").arg(QLatin1String(url));
}

static bool acceptLicense()
{
    const QString text
        = "<p>" + Tr::tr("Download niubash %1 from GitHub?").arg(QLatin1String(packageVersion))
          + "</p><p>"
          + Tr::tr("The bash tool on Windows normally runs Git Bash, but its MSYS2 "
                   "runtime cannot initialize inside the Windows sandbox "
                   "(microsoft/mxc#1061). niubash is a native, MSYS2-free Bash that "
                   "can, so it can be downloaded here to run the bash tool in the "
                   "sandbox. niubash is published under the MIT license.")
          + "</p><p>" + Tr::tr("Project: %1").arg(link(packageProject)) + "<br/>"
          + Tr::tr("License: %1").arg(link(packageLicense)) + "</p>";

    QMessageBox box(QMessageBox::Question,
                    dialogTitle(),
                    text,
                    QMessageBox::Cancel,
                    Core::ICore::dialogParent());
    box.setTextFormat(Qt::RichText);
    box.addButton(Tr::tr("Download"), QMessageBox::AcceptRole);

    return box.exec() != QMessageBox::Cancel;
}

static void verifyChecksum(QPromise<void> &promise, const FilePath &package)
{
    const Result<QByteArray> contents = package.fileContents();
    if (contents) {
        QCryptographicHash hash(QCryptographicHash::Sha256);
        hash.addData(*contents);
        if (hash.result().toHex() == QByteArray(asset().sha256))
            return;
    }
    promise.future().cancel();
}

// Verifies the unpacked release and copies it where the bash tool looks for
// it. The final (versioned) directory is only touched once the extraction
// has been verified, so an interrupted download leaves a previous install
// intact.
static Result<> install(const FilePath &unpackedTopDir)
{
    if (!holdsShell(unpackedTopDir))
        return ResultError(Tr::tr("The package holds no niubash shell."));

    const FilePath directory = downloadDirectory();
    if (directory.exists()) {
        const Result<> removed = directory.removeRecursively();
        if (!removed)
            return removed;
    }
    if (const Result<> result = directory.ensureWritableDir(); !result)
        return result;

    // Copy the release contents (niu.exe, winuxcmd, ...) into the directory.
    if (const Result<> result = unpackedTopDir.copyRecursively(directory); !result)
        return result;

    // Whatever is left of the versions downloaded before is of no use to
    // anyone: the directory is versioned and the pinned release replaces
    // them.
    const FilePaths others = directory.parentDir().dirEntries(
        DirFilterFlag::Dirs | DirFilterFlag::NoDotAndDotDot);
    for (const FilePath &other : others)
        if (other != directory)
            other.removeRecursively();

    return ResultOk;
}

GroupItem downloadRecipe()
{
    struct StorageStruct
    {
        std::unique_ptr<QProgressDialog> progressDialog;
        std::unique_ptr<TemporaryDirectory> temporaryDirectory;
        FilePath package;
    };

    const Storage<StorageStruct> storage;

    const auto onSetup = [storage] {
        if (!isSupportedPlatform()) {
            warn(Tr::tr("There is no niubash for this platform."));
            return SetupResult::StopWithError;
        }
        if (!acceptLicense())
            return SetupResult::StopWithError;

        storage->temporaryDirectory = std::make_unique<TemporaryDirectory>("llama-niubash-XXXXXX");
        storage->package = storage->temporaryDirectory->filePath(archiveName());
        storage->progressDialog.reset(
            createProgressDialog(100, dialogTitle(), Tr::tr("Downloading niubash...")));
        return SetupResult::Continue;
    };

    const auto onQuerySetup = [storage](QNetworkReplyWrapper &query) {
        const QString url = QStringLiteral(
                                "https://github.com/unixwin/niubash/releases/download/v%1/%2")
                                .arg(QLatin1String(packageVersion), archiveName());
        query.setRequest(QNetworkRequest(QUrl(url)));
        query.setNetworkAccessManager(NetworkAccessManager::instance());

        QProgressDialog *progressDialog = storage->progressDialog.get();
        QObject::connect(&query,
                         &QNetworkReplyWrapper::downloadProgress,
                         progressDialog,
                         [progressDialog](qint64 received, qint64 max) {
                             progressDialog->setRange(0, max);
                             progressDialog->setValue(received);
                         });
    };
    const auto onQueryDone = [storage](const QNetworkReplyWrapper &query, DoneWith result) {
        if (result == DoneWith::Cancel)
            return;

        QNetworkReply *reply = query.reply();
        QTC_ASSERT(reply, return);
        if (result != DoneWith::Success) {
            warn(Tr::tr("Downloading niubash failed: %1").arg(reply->errorString()));
            storage->package.clear();
            return;
        }

        const Result<qint64> written = storage->package.writeFileContents(reply->readAll());
        if (!written) {
            warn(written.error());
            storage->package.clear();
        }
    };

    const auto onVerifySetup = [storage](Async<void> &async) {
        if (storage->package.isEmpty())
            return SetupResult::StopWithError;

        async.setConcurrentCallData(verifyChecksum, storage->package);
        storage->progressDialog->setRange(0, 0);
        storage->progressDialog->setLabelText(Tr::tr("Verifying package integrity..."));
        return SetupResult::Continue;
    };
    const auto onVerifyDone = [](DoneWith result) {
        if (result == DoneWith::Error)
            warn(Tr::tr("The downloaded package is not the one that was expected."));
    };

    const auto onUnarchiveSetup = [storage](Unarchiver &task) {
        storage->progressDialog->setLabelText(Tr::tr("Unpacking niubash..."));
        task.setArchive(storage->package);
        task.setDestination(storage->temporaryDirectory->path());
    };
    const auto onUnarchiveDone = [storage](const Unarchiver &task) {
        const Result<> unpacked = task.result();
        if (!unpacked) {
            warn(Tr::tr("Unpacking niubash failed: %1").arg(unpacked.error()));
            return DoneResult::Error;
        }

        const FilePath topDir = storage->temporaryDirectory->path() / topDirectory();
        const Result<> installed = install(topDir);
        if (!installed) {
            warn(installed.error());
            return DoneResult::Error;
        }

        return DoneResult::Success;
    };

    const auto onCancelSetup = [storage] {
        return makeObjectSignal(storage->progressDialog.get(), &QProgressDialog::canceled);
    };

    return Group {
        storage,
        Group {
            onGroupSetup(onSetup),
            QNetworkReplyWrapperTask(onQuerySetup, onQueryDone),
            AsyncTask<void>(onVerifySetup, onVerifyDone),
            UnarchiverTask(onUnarchiveSetup, onUnarchiveDone)
        }.withCancel(onCancelSetup)
    };
}

} // namespace LlamaCpp::Tools::Niubash
