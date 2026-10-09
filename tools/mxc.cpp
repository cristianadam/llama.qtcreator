#include "mxc.h"

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
#include <QSysInfo>

using namespace QtTaskTree;
using namespace Utils;

namespace LlamaCpp::Tools::Mxc {

// The Microsoft MXC release the Windows sandbox is pinned to (the
// @microsoft/mxc-sdk npm package; its x64 binaries are byte-identical to
// the microsoft/mxc v0.8.0 release). A published release never changes, so
// the checksums belong to this version and no other.
const char packageVersion[] = "0.8.0";
const char packageProject[] = "https://github.com/microsoft/mxc";
const char packageLicense[] = "https://github.com/microsoft/mxc/blob/main/LICENSE";
const char packageUrl[] = "https://registry.npmjs.org/@microsoft/mxc-sdk/-/mxc-sdk-0.8.0.tgz";
const char packageSha256[] = "06bb2399d7e98ab1907acf851e12a4e44748dd467b79d3e53c2f2fbf569da14e";

// The launcher binary the sandbox wraps commands in, inside the package.
const char binaryMember[] = "package/bin/x64/wxc-exec.exe";
const quint64 binarySize = 9478968;
const char binarySha256[] = "6049c64723af1173c3739dc6cd6b2f33f6c021bb2832c4216233cba7f71aee9a";

static QString binaryName()
{
    return QStringLiteral("wxc-exec.exe");
}

bool isSupportedPlatform()
{
    if (!HostOsInfo::isWindowsHost())
        return false;
    // MXC v0.8.0 is published for x86-64 only.
    const QString architecture = QSysInfo::buildCpuArchitecture();
    return architecture == QLatin1String("x86_64");
}

QString version()
{
    return QLatin1String(packageVersion);
}

QString dialogTitle()
{
    return Tr::tr("Download the Windows sandbox runtime (MXC)");
}

FilePath downloadDirectory()
{
    return Core::ICore::userResourcePath("mxc") / QLatin1String(packageVersion);
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
    const QString overridePath = qEnvironmentVariable("LLAMA_WXC_EXEC");
    if (!overridePath.isEmpty()) {
        const FilePath fromEnv = FilePath::fromUserInput(overridePath);
        if (fromEnv.exists())
            return fromEnv;
    }
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
        = "<p>" + Tr::tr("Download the Microsoft MXC runtime %1?").arg(QLatin1String(packageVersion))
          + "</p><p>"
          + Tr::tr("The 'Sandbox commands' setting confines the bash tool on Windows to a "
                   "Microsoft MXC process container. The pinned wxc-exec.exe runtime is not "
                   "installed on this system, so it can be downloaded here instead. MXC is "
                   "published under the MIT license.")
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
        if (hash.result().toHex() == QByteArray(packageSha256))
            return;
    }
    promise.future().cancel();
}

// Takes the launcher binary out of the unpacked package (verifying its size
// and checksum) and leaves it where the Windows sandbox looks for it.
static Result<> install(const FilePath &unpacked)
{
    const FilePath binary = unpacked / QLatin1String(binaryMember);

    if (!binary.isFile())
        return ResultError(Tr::tr("The package holds no wxc-exec.exe binary."));

    const Result<QByteArray> contents = binary.fileContents();
    if (!contents || qint64(contents->size()) != qint64(binarySize))
        return ResultError(Tr::tr("The wxc-exec.exe binary has an unexpected size."));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    hash.addData(*contents);
    if (hash.result().toHex() != QByteArray(binarySha256))
        return ResultError(Tr::tr("The wxc-exec.exe binary does not match its expected "
                                  "checksum."));

    const FilePath directory = downloadDirectory();
    if (const Result<> result = directory.ensureWritableDir(); !result)
        return result;

    const FilePath target = directory / binaryName();
    target.removeFile();
    if (const Result<> result = binary.copyFile(target); !result)
        return result;

    // Remove previously downloaded versions: the directory is versioned
    // and the pinned release replaces them.
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
            warn(Tr::tr("The Microsoft MXC runtime is available for Windows x86-64 only."));
            return SetupResult::StopWithError;
        }
        if (!acceptLicense())
            return SetupResult::StopWithError;

        storage->temporaryDirectory = std::make_unique<TemporaryDirectory>("llama-mxc-XXXXXX");
        storage->package = storage->temporaryDirectory->filePath(
            QStringLiteral("mxc-sdk-%1.tgz").arg(QLatin1String(packageVersion)));
        storage->progressDialog.reset(createProgressDialog(
            100, dialogTitle(), Tr::tr("Downloading the MXC runtime...")));
        return SetupResult::Continue;
    };

    const auto onQuerySetup = [storage](QNetworkReplyWrapper &query) {
        query.setRequest(QNetworkRequest(QUrl(QLatin1String(packageUrl))));
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
            warn(Tr::tr("Downloading the MXC runtime failed: %1").arg(reply->errorString()));
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
        storage->progressDialog->setLabelText(Tr::tr("Unpacking the MXC runtime..."));
        task.setArchive(storage->package);
        task.setDestination(storage->temporaryDirectory->path());
    };
    const auto onUnarchiveDone = [storage](const Unarchiver &task) {
        const Result<> unpacked = task.result();
        if (!unpacked) {
            warn(Tr::tr("Unpacking the MXC runtime failed: %1").arg(unpacked.error()));
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

} // namespace LlamaCpp::Tools::Mxc
