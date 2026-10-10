#include "ripgrep.h"

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

namespace LlamaCpp::Tools::Ripgrep {

// The release the search and find tools are pinned to. A published release
// never changes, so the checksum belongs to this version and no other.
const char packageVersion[] = "15.2.0";
const char packageProject[] = "https://github.com/BurntSushi/ripgrep";
const char packageLicense[] = "https://github.com/BurntSushi/ripgrep/blob/master/LICENSE-MIT";

struct Asset
{
    const char *target; // nullptr for unsupported platforms
    const char *sha256;
    bool isZip;
};

static const Asset &asset()
{
    static const Asset none{nullptr, nullptr, false};
    static const Asset macAarch64{"aarch64-apple-darwin",
                                  "3750b2e93f37e0c692657da574d7019a101c0084da05a790c83fd335bad973e4",
                                  false};
    static const Asset macX86_64{"x86_64-apple-darwin",
                                 "af7825fcc69a2afc7a7aea55fc9af90e26421d8f20fe59df32e233c0b8a231c1",
                                 false};
    static const Asset linuxAarch64{"aarch64-unknown-linux-musl",
                                    "800b1e7206afe799dfb5a6901f23147cfaabe0e52210538100f61e86e1740915",
                                    false};
    static const Asset linuxX86_64{"x86_64-unknown-linux-musl",
                                   "33e15bcf1624b25cdd2a55813a47a2f95dbe126268203e76aa6a585d1e7b149c",
                                   false};
    static const Asset winAarch64{"aarch64-pc-windows-msvc",
                                  "e4abca10c3a64ebea742667dd7009449d49403db5460dd6873e389fa2945360f",
                                  true};
    static const Asset winX86_64{"x86_64-pc-windows-msvc",
                                 "71b2fef860abe467217a538ff31de02f5258807c0129f771846f87bd029aafc5",
                                 true};

    const QString architecture = QSysInfo::buildCpuArchitecture();
    const bool isAarch64 = (architecture == QLatin1String("arm64")
                            || architecture == QLatin1String("aarch64"));
    const bool isX86_64 = architecture == QLatin1String("x86_64");
    if (!isAarch64 && !isX86_64)
        return none;
    if (HostOsInfo::isMacHost())
        return isAarch64 ? macAarch64 : macX86_64;
    if (HostOsInfo::isWindowsHost())
        return isAarch64 ? winAarch64 : winX86_64;
    if (HostOsInfo::isLinuxHost())
        return isAarch64 ? linuxAarch64 : linuxX86_64;
    return none;
}

static QString archiveName()
{
    return QStringLiteral("ripgrep-%1-%2.%3")
            .arg(QLatin1String(packageVersion),
                 QLatin1String(asset().target),
                 asset().isZip ? QStringLiteral("zip") : QStringLiteral("tar.gz"));
}

QString dialogTitle()
{
    return Tr::tr("Download ripgrep");
}

QString version()
{
    return QLatin1String(packageVersion);
}

static QString binaryName()
{
    return HostOsInfo::isWindowsHost() ? QStringLiteral("rg.exe") : QStringLiteral("rg");
}

bool isSupportedPlatform()
{
    return asset().target != nullptr;
}

FilePath downloadDirectory()
{
    return Core::ICore::userResourcePath("ripgrep") / QLatin1String(packageVersion);
}

static bool holdsBinary(const FilePath &directory)
{
    return (directory / binaryName()).isFile();
}

bool isDownloaded()
{
    return holdsBinary(downloadDirectory());
}

FilePath resolvedPath()
{
    const FilePath onPath = FilePath::fromUserInput(QStandardPaths::findExecutable("rg"));
    if (onPath.exists())
        return onPath;
    const FilePath downloaded = downloadDirectory() / binaryName();
    if (holdsBinary(downloadDirectory()))
        return downloaded;
    return {};
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
        = "<p>" + Tr::tr("Download ripgrep %1 from GitHub?").arg(QLatin1String(packageVersion))
          + "</p><p>"
          + Tr::tr("The search and find tools use ripgrep to search file contents and to "
                   "locate files by name. It is not installed on this system, so it can be "
                   "downloaded here instead. ripgrep is published under the MIT or the "
                   "Unlicense license.")
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

// Takes the binary out of the unpacked package and leaves it where the search
// and find tools look for it.
static Result<> install(const FilePath &unpacked)
{
    const FilePath binary
        = unpacked / (QStringLiteral("ripgrep-%1-%2")
                          .arg(QLatin1String(packageVersion), QLatin1String(asset().target)))
                      / binaryName();

    if (!binary.isFile())
        return ResultError(Tr::tr("The package holds no ripgrep binary."));

    const FilePath directory = downloadDirectory();
    if (const Result<> result = directory.ensureWritableDir(); !result)
        return result;

    const FilePath target = directory / binaryName();
    target.removeFile();
    if (const Result<> result = binary.copyFile(target); !result)
        return result;

#if !defined(Q_OS_WIN)
    // Extraction preserves the executable bit, but make sure of it.
    QFile file(target.toFSPathString());
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner
                        | QFile::ReadGroup | QFile::ExeGroup
                        | QFile::ReadOther | QFile::ExeOther);
#endif

    // Whatever is left of the versions downloaded before is of no use to anyone
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
            warn(Tr::tr("There is no ripgrep for this platform."));
            return SetupResult::StopWithError;
        }
        if (!acceptLicense())
            return SetupResult::StopWithError;

        storage->temporaryDirectory = std::make_unique<TemporaryDirectory>("llama-rg-XXXXXX");
        storage->package = storage->temporaryDirectory->filePath(archiveName());
        storage->progressDialog.reset(
            createProgressDialog(100, dialogTitle(), Tr::tr("Downloading ripgrep...")));
        return SetupResult::Continue;
    };

    const auto onQuerySetup = [storage](QNetworkReplyWrapper &query) {
        const QString url = QStringLiteral(
                                "https://github.com/BurntSushi/ripgrep/releases/download/%1/%2")
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
            warn(Tr::tr("Downloading ripgrep failed: %1").arg(reply->errorString()));
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
        storage->progressDialog->setLabelText(Tr::tr("Unpacking ripgrep..."));
        task.setArchive(storage->package);
        task.setDestination(storage->temporaryDirectory->path());
    };
    const auto onUnarchiveDone = [storage](const Unarchiver &task) {
        const Result<> unpacked = task.result();
        if (!unpacked) {
            warn(Tr::tr("Unpacking ripgrep failed: %1").arg(unpacked.error()));
            return DoneResult::Error;
        }

        const Result<> installed = install(storage->temporaryDirectory->path());
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

} // namespace LlamaCpp::Tools::Ripgrep
