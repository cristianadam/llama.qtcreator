// Tests of the managed-server helpers.

#include <QtTest>

#include "llamaserver.h"
#include "llamaruntime.h"

#include <QDir>
#include <QFile>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <utils/hostosinfo.h>

using namespace LlamaCpp;

static void touchFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QFile::WriteOnly))
        qWarning() << "touchFile: could not create" << path;
}

class LlamaServerTest : public QObject
{
    Q_OBJECT

private slots:
    void tokenize_simple();
    void tokenize_quotes();
    void tokenize_empty();
    void tokenize_whitespace();
    void tokenize_emptyQuotes();

    void serveArgs_llamaCli();
    void serveArgs_llamaServer();
    void serveArgs_noModelsDir();
    void serveArgs_extraArguments();

    void versionTag_buildNumber();
    void versionTag_bTag();
    void versionTag_none();

    void environment_colorForcing();
    void environment_apiKey();

    void connectAddress_wildcards();
    void connectAddress_unchanged();

    void installScript_ok();
    void installScript_httpError();
    void installScript_redirected();
    void installScript_wrongUri();
    void installScript_tooLarge();

    void managedDirectory_installScriptLocation();

    void quant_standard();
    void quant_udPrefix();
    void quant_iqMxfpF16();
    void quant_noQuant();
    void quant_familyNamesAreNoQuant();

    void parseModels_full();
    void parseModels_skipsEmptyEntries();
    void parseModels_malformed();

    void scanDir_topLevelFiles();
    void scanDir_subdirectories();
    void scanDir_sidecarsSkipped();
    void scanDir_missingOrEmpty();

    void formatSize_gb();
    void formatSize_mb();
    void formatSize_unknown();
};

void LlamaServerTest::tokenize_simple()
{
    QCOMPARE(tokenizeServeArguments(QStringLiteral("--ctx-size 8192")),
             (QStringList{QStringLiteral("--ctx-size"), QStringLiteral("8192")}));
}

void LlamaServerTest::tokenize_quotes()
{
    QCOMPARE(tokenizeServeArguments(QStringLiteral("--stop \"foo bar\" --temp")),
             (QStringList{QStringLiteral("--stop"),
                          QStringLiteral("foo bar"),
                          QStringLiteral("--temp")}));
}

void LlamaServerTest::tokenize_empty()
{
    QCOMPARE(tokenizeServeArguments(QString()), QStringList());
    QCOMPARE(tokenizeServeArguments(QStringLiteral("   \n\t ")), QStringList());
}

void LlamaServerTest::tokenize_whitespace()
{
    QCOMPARE(tokenizeServeArguments(QStringLiteral("  --a\n\t--b  ")),
             (QStringList{QStringLiteral("--a"), QStringLiteral("--b")}));
}

void LlamaServerTest::tokenize_emptyQuotes()
{
    QCOMPARE(tokenizeServeArguments(QStringLiteral("--x \"\"")),
             (QStringList{QStringLiteral("--x"), QString()}));
}

void LlamaServerTest::serveArgs_llamaCli()
{
    const QStringList args = buildServeArguments(
        QStringLiteral("llama"),
        QStringLiteral("127.0.0.1"),
        9931,
        QStringLiteral("/home/user/.llama/models"),
        QStringLiteral("--ctx-size 4096"));
    QCOMPARE(args.at(0), QStringLiteral("serve"));
    QVERIFY(args.contains(QStringLiteral("--host"))
            && args.at(args.indexOf(QStringLiteral("--host")) + 1) == QStringLiteral("127.0.0.1"));
    QVERIFY(args.contains(QStringLiteral("--port"))
            && args.at(args.indexOf(QStringLiteral("--port")) + 1) == QStringLiteral("9931"));
    QVERIFY(args.contains(QStringLiteral("--jinja")));
    QVERIFY(args.contains(QStringLiteral("--models-dir"))
            && args.at(args.indexOf(QStringLiteral("--models-dir")) + 1)
               == QStringLiteral("/home/user/.llama/models"));
    // The extra arguments go last, so they win on repeated flags.
    QCOMPARE(args.last(), QStringLiteral("4096"));
    QCOMPARE(args.at(args.size() - 2), QStringLiteral("--ctx-size"));
}

void LlamaServerTest::serveArgs_llamaServer()
{
    const QStringList args
        = buildServeArguments(QStringLiteral("llama-server"),
                              QStringLiteral("0.0.0.0"),
                              8080,
                              QString(),
                              QString());
    // A standalone llama-server takes the flags directly, no subcommand.
    QCOMPARE(args.at(0), QStringLiteral("--host"));
    QVERIFY(!args.contains(QStringLiteral("serve")));
    QVERIFY(!args.contains(QStringLiteral("--models-dir")));
}

void LlamaServerTest::serveArgs_noModelsDir()
{
    const QStringList args
        = buildServeArguments(QStringLiteral("llama"),
                              QStringLiteral("127.0.0.1"),
                              9931,
                              QString(),
                              QString());
    QVERIFY(!args.contains(QStringLiteral("--models-dir")));
}

void LlamaServerTest::serveArgs_extraArguments()
{
    const QStringList args
        = buildServeArguments(QStringLiteral("llama"),
                              QStringLiteral("127.0.0.1"),
                              9931,
                              QStringLiteral("/models"),
                              QStringLiteral("--cache-type-k q8_0 --stop \"end of text\""));
    QCOMPARE(args,
             (QStringList{QStringLiteral("serve"),
                          QStringLiteral("--host"),
                          QStringLiteral("127.0.0.1"),
                          QStringLiteral("--port"),
                          QStringLiteral("9931"),
                          QStringLiteral("--jinja"),
                          QStringLiteral("--models-dir"),
                          QStringLiteral("/models"),
                          QStringLiteral("--cache-type-k"),
                          QStringLiteral("q8_0"),
                          QStringLiteral("--stop"),
                          QStringLiteral("end of text")}));
}

void LlamaServerTest::versionTag_buildNumber()
{
    QCOMPARE(parseVersionTag(QStringLiteral(
                 "llama-server version 0.9.0 (build 12110, abcdef)")),
             QStringLiteral("b12110"));
}

void LlamaServerTest::versionTag_bTag()
{
    QCOMPARE(parseVersionTag(QStringLiteral("llama-cli ... b9553 (abcdef)")),
             QStringLiteral("b9553"));
}

void LlamaServerTest::versionTag_none()
{
    QCOMPARE(parseVersionTag(QStringLiteral("not a version output")), QString());
    QCOMPARE(parseVersionTag(QString()), QString());
}

void LlamaServerTest::environment_colorForcing()
{
    // The server only colorizes its log when it detects a terminal
    // (isatty), and it runs with piped output: the environment must force
    // the colors on (LLAMA_ARG_LOG_COLORS; CLICOLOR_FORCE & co. are not
    // consulted by llama.cpp).
    const QProcessEnvironment env = serverProcessEnvironment(QString());
    QCOMPARE(env.value(QStringLiteral("LLAMA_ARG_LOG_COLORS")), QStringLiteral("on"));
    // The system environment is carried over (except the overridden keys).
    const QProcessEnvironment systemEnv = QProcessEnvironment::systemEnvironment();
    for (const QString &key : systemEnv.keys()) {
        if (key == QLatin1String("LLAMA_API_KEY")
            || key == QLatin1String("LLAMA_ARG_LOG_COLORS"))
            continue;
        QCOMPARE(env.value(key), systemEnv.value(key));
    }
}

void LlamaServerTest::environment_apiKey()
{
    QCOMPARE(serverProcessEnvironment(QStringLiteral("secret")).value(QStringLiteral("LLAMA_API_KEY")),
             QStringLiteral("secret"));
    // No key: nothing is inserted (only assertable when the system has none).
    if (!QProcessEnvironment::systemEnvironment().contains(QStringLiteral("LLAMA_API_KEY")))
        QVERIFY(!serverProcessEnvironment(QString()).contains(QStringLiteral("LLAMA_API_KEY")));
}

void LlamaServerTest::connectAddress_wildcards()
{
    QCOMPARE(serverConnectAddress(QStringLiteral("0.0.0.0")), QStringLiteral("127.0.0.1"));
    QCOMPARE(serverConnectAddress(QStringLiteral("::")), QStringLiteral("127.0.0.1"));
}

void LlamaServerTest::connectAddress_unchanged()
{
    QCOMPARE(serverConnectAddress(QStringLiteral("127.0.0.1")), QStringLiteral("127.0.0.1"));
    QCOMPARE(serverConnectAddress(QStringLiteral("192.168.1.5")), QStringLiteral("192.168.1.5"));
    QCOMPARE(serverConnectAddress(QStringLiteral("localhost")), QStringLiteral("localhost"));
}

void LlamaServerTest::installScript_ok()
{
    // The pinned URL, a direct 200, a plausible size.
    QCOMPARE(verifyInstallScript(QUrl(installScriptUrl()), 200, 12345),
             InstallScriptVerdict::Ok);
    // An unknown Content-Length is fine; the body is size-checked when read.
    QCOMPARE(verifyInstallScript(QUrl(installScriptUrl()), 200, -1),
             InstallScriptVerdict::Ok);
}

void LlamaServerTest::installScript_httpError()
{
    QCOMPARE(verifyInstallScript(QUrl(installScriptUrl()), 404, 0),
             InstallScriptVerdict::HttpError);
    QCOMPARE(verifyInstallScript(QUrl(installScriptUrl()), 500, 0),
             InstallScriptVerdict::HttpError);
}

void LlamaServerTest::installScript_redirected()
{
    QCOMPARE(verifyInstallScript(QUrl(installScriptUrl()), 302, 0),
             InstallScriptVerdict::Redirected);
}

void LlamaServerTest::installScript_wrongUri()
{
    QCOMPARE(verifyInstallScript(QUrl(QStringLiteral("https://example.com/install.sh")), 200, 100),
             InstallScriptVerdict::WrongUri);
    // Same host, different path.
    QCOMPARE(verifyInstallScript(
                 QUrl(QStringLiteral("https://llama.app/other.sh")), 200, 100),
             InstallScriptVerdict::WrongUri);
}

void LlamaServerTest::installScript_tooLarge()
{
    QCOMPARE(verifyInstallScript(QUrl(installScriptUrl()), 200, 2 * 1024 * 1024),
             InstallScriptVerdict::TooLarge);
}

void LlamaServerTest::managedDirectory_installScriptLocation()
{
    // The managed directory is where the official install scripts actually
    // leave the binary: install.sh keeps it in ~/.llama-app (no "bin"
    // subdirectory, plus a copy in ~/.local/bin); install.ps1 moves it to
    // %LOCALAPPDATA%\Microsoft\WindowsApps and removes its scratch dir.
    if (Utils::HostOsInfo::isWindowsHost())
    {
        QString localAppData = qEnvironmentVariable("LOCALAPPDATA");
        if (localAppData.isEmpty())
            localAppData = QDir(QDir::homePath()).filePath(QStringLiteral("AppData/Local"));
        QCOMPARE(LlamaRuntime::managedDirectory(),
                 QDir(localAppData).filePath(QStringLiteral("Microsoft/WindowsApps")));
    }
    else
        QCOMPARE(LlamaRuntime::managedDirectory(),
                 QDir(QDir::homePath()).filePath(QStringLiteral(".llama-app")));
}

void LlamaServerTest::quant_standard()
{
    QCOMPARE(quantFromFileName(QStringLiteral("Qwen2.5-7B-Instruct-Q4_K_M.gguf")),
             QStringLiteral("Q4_K_M"));
    QCOMPARE(quantFromFileName(QStringLiteral("Llama-3-8B-Instruct-Q8_0.gguf")),
             QStringLiteral("Q8_0"));
    QCOMPARE(quantFromFileName(QStringLiteral("model.gguf")), QStringLiteral(""));
}

void LlamaServerTest::quant_udPrefix()
{
    // The UD- prefix belongs to the quant label, across the dash split.
    QCOMPARE(quantFromFileName(QStringLiteral("Llama-3.2-1B-UD-Q4_K_XL.gguf")),
             QStringLiteral("UD-Q4_K_XL"));
    QCOMPARE(quantFromFileName(QStringLiteral("UD-Q5_K_S.gguf")),
             QStringLiteral("UD-Q5_K_S"));
}

void LlamaServerTest::quant_iqMxfpF16()
{
    QCOMPARE(quantFromFileName(QStringLiteral("gemma-2b-IQ2_XXS.gguf")),
             QStringLiteral("IQ2_XXS"));
    QCOMPARE(quantFromFileName(QStringLiteral("GPT-OSS-20B-mxfp4.gguf")),
             QStringLiteral("mxfp4"));
    QCOMPARE(quantFromFileName(QStringLiteral("model-F16.gguf")), QStringLiteral("F16"));
    QCOMPARE(quantFromFileName(QStringLiteral("model-BF16.gguf")), QStringLiteral("BF16"));
    QCOMPARE(quantFromFileName(QStringLiteral("model-F32.gguf")), QStringLiteral("F32"));
}

void LlamaServerTest::quant_noQuant()
{
    QCOMPARE(quantFromFileName(QStringLiteral("my-model.gguf")), QString());
    QCOMPARE(quantFromFileName(QStringLiteral("")), QString());
}

void LlamaServerTest::quant_familyNamesAreNoQuant()
{
    // A "Q" only counts when followed by digits: family names must not be
    // mistaken for quants.
    QCOMPARE(quantFromFileName(QStringLiteral("Qwen2.5-0.5B-Instruct.gguf")), QString());
    QCOMPARE(quantFromFileName(QStringLiteral("QwQ-32B-Instruct.gguf")), QString());
    QCOMPARE(quantFromFileName(QStringLiteral("SmolLM2-360M-Instruct-Q4_0.gguf")),
             QStringLiteral("Q4_0"));
}

void LlamaServerTest::parseModels_full()
{
    const QByteArray json = R"({
        "data": [
            {
                "id": "Qwen2.5-7B-Instruct-Q4_K_M",
                "status": {"value": "loaded"},
                "context_length": 32768,
                "architecture": {"input_modalities": ["text", "image"]},
                "source": "local",
                "can_remove": false
            },
            {
                "id": "other",
                "status": {"value": "unloaded"},
                "source": "cache",
                "can_remove": true
            }
        ],
        "object": "list"
    })";
    const QList<ServerModel> models = parseModelsResponse(json);
    QCOMPARE(models.size(), 2);
    QCOMPARE(models.at(0).id, QStringLiteral("Qwen2.5-7B-Instruct-Q4_K_M"));
    QCOMPARE(models.at(0).status, QStringLiteral("loaded"));
    QCOMPARE(models.at(0).contextLength, 32768);
    QCOMPARE(models.at(0).inputModalities, (QStringList{QStringLiteral("text"),
                                                        QStringLiteral("image")}));
    QCOMPARE(models.at(0).source, QStringLiteral("local"));
    QVERIFY(!models.at(0).canRemove);
    QCOMPARE(models.at(1).id, QStringLiteral("other"));
    QCOMPARE(models.at(1).status, QStringLiteral("unloaded"));
    QCOMPARE(models.at(1).contextLength, 0);
    QVERIFY(models.at(1).inputModalities.isEmpty());
    QVERIFY(models.at(1).canRemove);
}

void LlamaServerTest::parseModels_skipsEmptyEntries()
{
    const QByteArray json = R"({"data": [{"id": ""}, {"id": "a"}, {"noId": true}]})";
    const QList<ServerModel> models = parseModelsResponse(json);
    QCOMPARE(models.size(), 1);
    QCOMPARE(models.at(0).id, QStringLiteral("a"));
}

void LlamaServerTest::parseModels_malformed()
{
    QVERIFY(parseModelsResponse(QByteArray()).isEmpty());
    QVERIFY(parseModelsResponse(QByteArrayLiteral("not json")).isEmpty());
    QVERIFY(parseModelsResponse(QByteArrayLiteral("{}")).isEmpty());
}

void LlamaServerTest::scanDir_topLevelFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    touchFile(dir.filePath(QStringLiteral("Qwen2.5-7B-Instruct-Q4_K_M.gguf")));
    touchFile(dir.filePath(QStringLiteral("Llama-3-8B-Q8_0.gguf")));
    touchFile(dir.filePath(QStringLiteral("notes.txt")));

    const QList<ServerModel> models = scanModelsDirectory(dir.path());
    QCOMPARE(models.size(), 2);
    QCOMPARE(models.at(0).id, QStringLiteral("Llama-3-8B-Q8_0"));
    QCOMPARE(models.at(1).id, QStringLiteral("Qwen2.5-7B-Instruct-Q4_K_M"));
    QVERIFY(models.at(0).path.endsWith(QStringLiteral(".gguf")));
    QVERIFY(QFileInfo(models.at(0).path).exists());
}

void LlamaServerTest::scanDir_subdirectories()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // A subdirectory is one model, listed by its name; the size is the sum
    // of the shards and the main file is the first one.
    const QString sub = dir.filePath(QStringLiteral("big-model"));
    QDir().mkpath(sub);
    touchFile(sub + QStringLiteral("/big-model-00001-of-00002.gguf"));
    touchFile(sub + QStringLiteral("/big-model-00002-of-00002.gguf"));
    touchFile(sub + QStringLiteral("/mmproj-big.gguf"));

    const QList<ServerModel> models = scanModelsDirectory(dir.path());
    QCOMPARE(models.size(), 1);
    QCOMPARE(models.at(0).id, QStringLiteral("big-model"));
    QCOMPARE(models.at(0).path,
             sub + QStringLiteral("/big-model-00001-of-00002.gguf"));

    // A directory without a model file is not a model.
    QDir().mkpath(dir.filePath(QStringLiteral("empty")));
    QCOMPARE(scanModelsDirectory(dir.path()).size(), 1);
}

void LlamaServerTest::scanDir_sidecarsSkipped()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    touchFile(dir.filePath(QStringLiteral("mmproj-Qwen2-VL.gguf")));
    touchFile(dir.filePath(QStringLiteral("Qwen2-VL-imatrix.dat")));
    touchFile(dir.filePath(QStringLiteral("mtp-draft.gguf")));
    touchFile(dir.filePath(QStringLiteral("model.gguf")));

    const QList<ServerModel> models = scanModelsDirectory(dir.path());
    QCOMPARE(models.size(), 1);
    QCOMPARE(models.at(0).id, QStringLiteral("model"));
}

void LlamaServerTest::scanDir_missingOrEmpty()
{
    QVERIFY(scanModelsDirectory(QStringLiteral("/nonexistent/llama-test-dir")).isEmpty());
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QVERIFY(scanModelsDirectory(dir.path()).isEmpty());
}

void LlamaServerTest::formatSize_gb()
{
    // LL: 2 GiB and 12 GiB overflow a 32-bit int literal.
    QCOMPARE(formatByteSize(2LL * 1024 * 1024 * 1024), QStringLiteral("2 GB"));
    QCOMPARE(formatByteSize(4404019866), QStringLiteral("4.1 GB")); // 4.1 GiB
    QCOMPARE(formatByteSize(12LL * 1024 * 1024 * 1024), QStringLiteral("12 GB"));
}

void LlamaServerTest::formatSize_mb()
{
    QCOMPARE(formatByteSize(256 * 1024 * 1024), QStringLiteral("256 MB"));
    QCOMPARE(formatByteSize(1024 * 1024), QStringLiteral("1 MB"));
}

void LlamaServerTest::formatSize_unknown()
{
    QCOMPARE(formatByteSize(0), QStringLiteral("?"));
    QCOMPARE(formatByteSize(-1), QStringLiteral("?"));
}

QTEST_GUILESS_MAIN(LlamaServerTest)
#include "llamaserver_test.moc"
