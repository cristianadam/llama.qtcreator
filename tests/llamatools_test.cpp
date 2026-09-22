#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QtTest/QtTest>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/projectmanager.h>

#include <utils/filepath.h>

#include <llamathinkingsectionparser.h>
#include <tools/apply_patch_tool.h>
#include <tools/factory.h>
#include <tools/mcptool.h>
#include <tools/patch.h>
#include <tools/bash_tool.h>
#include <tools/find_tool.h>
#include <tools/ripgrep.h>
#include <tools/search_tool.h>
#include <tools/task_tool.h>
#include <tools/todowrite_tool.h>
#include <tools/webfetch_tool.h>
#include <tools/write_tool.h>
#include <tools/edit_file_tool.h>
#include <tools/websearch_tool.h>
#include <tools/web_utils.h>

namespace LlamaCpp {

namespace {

bool writeTextFile(const QString &absPath, const QString &content)
{
    QFile f(absPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(content.toUtf8());
    return true;
}

QString readTextFile(const QString &absPath)
{
    QFile f(absPath);
    if (!f.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(f.readAll());
}

QString applyChunks(const QString &oldContent,
                    const QVector<Patch::UpdateChunk> &chunks,
                    const QString &filePath = QStringLiteral("test.txt"))
{
    QString newContent;
    const QString err = Patch::applyUpdateChunks(oldContent, chunks, filePath, newContent);
    if (!err.isEmpty())
        return QStringLiteral("<error: %1>").arg(err);
    return newContent;
}

QString applyEdits(const QString &content,
                   const QVector<QPair<QString, QString>> &edits,
                   bool replaceAll = false)
{
    QString newContent;
    const QString err = Tools::applyTextEdits(content, edits, replaceAll, newContent);
    if (!err.isEmpty())
        return QStringLiteral("<error: %1>").arg(err);
    return newContent;
}

Patch::UpdateChunk makeChunk(const QStringList &oldLines,
                             const QStringList &newLines,
                             const QString &context = {},
                             bool eof = false)
{
    Patch::UpdateChunk chunk;
    chunk.oldLines = oldLines;
    chunk.newLines = newLines;
    chunk.changeContext = context;
    chunk.endOfFile = eof;
    return chunk;
}

template <typename ToolT = ApplyPatchTool>
std::pair<QString, bool> runTool(const QJsonObject &args)
{
    QString output;
    bool ok = false;
    ToolT tool;
    tool.run(args,
             [&output, &ok](const QString &out, bool success) {
                 output = out;
                 ok = success;
             });
    return {output, ok};
}

// Runs an asynchronous (process‑backed) tool such as bash / search / find
// and spins the event loop until its callback fires or waitMs is exceeded.
template <typename ToolT>
std::pair<QString, bool> runAsyncTool(ToolT &tool, const QJsonObject &args, int waitMs = 30000)
{
    QString output;
    bool ok = false;
    bool finished = false;
    tool.run(args,
             [&output, &ok, &finished](const QString &out, bool success) {
                 output = out;
                 ok = success;
                 finished = true;
             });

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] { if (finished)
                                                         loop.quit(); });
    poll.start();
    QTimer::singleShot(waitMs, &loop, [&] { poll.stop();
                                            loop.quit(); });
    loop.exec();
    return {output, ok};
}

// Runs the (asynchronous) bash tool, spinning the event loop until done.
std::pair<QString, bool> runBashTool(const QJsonObject &args, int waitMs = 30000)
{
    Tools::BashTool tool;
    return runAsyncTool(tool, args, waitMs);
}

// The search / find tools spawn ripgrep; the process‑based test cases only
// make sense when a ripgrep (system or downloaded) is available.
bool ripgrepAvailable()
{
    return !Tools::Ripgrep::resolvedPath().isEmpty();
}

// A stand‑in for a tool served by the Qt Creator MCP server.
class FakeRemoteTool : public Tool
{
public:
    explicit FakeRemoteTool(const QString &name)
        : m_name(name)
    {}

    QString name() const override { return m_name; }
    QString toolDefinition() const override { return QStringLiteral("{}"); }
    QString oneLineSummary(const QJsonObject &) const override { return m_name; }
    void run(const QJsonObject &,
             std::function<void(const QString &, bool)> done) const override
    {
        done(QStringLiteral("ok"), true);
    }

private:
    QString m_name;
};

class FakeRemoteProvider : public LlamaCpp::RemoteToolProvider
{
public:
    QStringList toolNames() const override
    {
        return {QStringLiteral("remote_tool_a"), QStringLiteral("remote_tool_b")};
    }

    std::unique_ptr<Tool> createTool(const QString &name) const override
    {
        if (name == QLatin1String("remote_tool_a") || name == QLatin1String("remote_tool_b"))
            return std::make_unique<FakeRemoteTool>(name);
        return nullptr;
    }
};

// A minimal HTTP/1.1 server for the web_utils round‑trip tests. It answers
// any request with \a body, except POST requests, where it echoes the
// request body back (so the tests can verify that posted data arrived).
class MiniHttpServer : public QTcpServer
{
public:
    MiniHttpServer(const QByteArray &body, const QByteArray &contentType,
                   QObject *parent = nullptr)
        : QTcpServer(parent)
        , m_body(body)
        , m_contentType(contentType)
    {}

    bool start(quint16 &port)
    {
        if (!listen(QHostAddress::LocalHost, 0))
            return false;
        port = serverPort();
        return true;
    }

protected:
    void incomingConnection(qintptr handle) override
    {
        auto *socket = new QTcpSocket(this);
        socket->setSocketDescriptor(handle);

        struct Pending : QObject
        {
            explicit Pending(QObject *parent) : QObject(parent) {}
            QByteArray data;
        };
        auto *pending = new Pending(socket);

        QObject::connect(socket,
                         &QTcpSocket::readyRead,
                         this,
                         [this, socket, pending]() {
                             pending->data.append(socket->readAll());
                             const int headerEnd = pending->data.indexOf("\r\n\r\n");
                             if (headerEnd < 0)
                                 return; // Headers not complete yet.

                             const QByteArray headers = pending->data.left(headerEnd);
                             const QByteArray request = pending->data.mid(headerEnd + 4);

                             qint64 contentLength = 0;
                             for (const QByteArray &line : headers.split('\n'))
                                 if (const QByteArray trimmed = line.trimmed();
                                      trimmed.toLower().startsWith("content-length:"))
                                     contentLength = trimmed.mid(15).trimmed().toLongLong();
                             if (request.size() < contentLength)
                                 return; // Body not complete yet.

                             const bool isPost = headers.startsWith("POST");
                             const QByteArray body =
                                 isPost ? request.left(contentLength) : m_body;
                             const QByteArray response =
                                 QStringLiteral("HTTP/1.1 200 OK\r\n"
                                                "Content-Type: %1\r\n"
                                                "Content-Length: %2\r\n"
                                                "Connection: close\r\n"
                                                "\r\n")
                                     .arg(isPost ? QStringLiteral("text/plain")
                                                : QString::fromLatin1(m_contentType),
                                          QString::number(body.size()))
                                     .toUtf8()
                                 + body;
                             socket->write(response);
                             socket->disconnectFromHost();
                         });
    }

private:
    QByteArray m_body;
    QByteArray m_contentType;
};

struct WebResult
{
    QByteArray body;
    QString contentType;
    QString error;
    bool finished = false;
};

// Launches an httpGet/httpPost call and spins the event loop until its
// callback fires or waitMs is exceeded.
WebResult runHttpCall(std::function<void(Tools::HttpResponseCallback)> launch, int waitMs = 10000)
{
    WebResult result;
    launch([&result](const QByteArray &body, const QString &contentType, const QString &error) {
        result.body = body;
        result.contentType = contentType;
        result.error = error;
        result.finished = true;
    });

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] {
        if (result.finished)
            loop.quit();
    });
    poll.start();
    QTimer::singleShot(waitMs, &loop, [&] {
        poll.stop();
        loop.quit();
    });
    loop.exec();
    return result;
}

} // namespace

class LlamaToolsTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();

    // Patch::parse
    void parse_addFile();
    void parse_addUpdateDelete();
    void parse_move();
    void parse_missingMarkers();
    void parse_missingMarkersTolerated();
    void parse_updateWithoutChunks();
    void parse_emptyEnvelope();
    void parse_addFileMissingPrefixTolerated();
    void parse_addFileStarLines();
    void parse_addFileBlankLineTolerated();
    void parse_crlfTolerated();
    void parse_chunkInvalidLine();

    // Patch::applyUpdateChunks
    void match_exact();
    void match_trailingWhitespace();
    void match_unicodeNormalized();
    void match_contextDisambiguates();
    void match_endOfFileAnchor();
    void match_pureInsertion();
    void match_trailingBlankLineTolerance();
    void match_outOfOrderFails();
    void match_bomPreserved();
    void match_notFound();
    void match_contextNotFound();
    void match_closestMatchHint();
    void match_outOfOrderHint();

    // ApplyPatchTool::run
    void tool_fullPatch();
    void tool_addFileInStartupProject();
    void tool_addMarkdownFile();
    void tool_move();
    void tool_updateSameFileTwice();
    void tool_updateNoChanges();
    void tool_emptyPatchText();
    void tool_invalidFormat();
    void tool_emptyEnvelope();
    void tool_updateMissingFile();
    void tool_atomicFailure();
    void tool_toolDefinition();
    void tool_oneLineSummary();
    void tool_streamingSummary_empty();
    void tool_streamingSummary_addFile();
    void tool_streamingSummary_updateFile();
    void tool_streamingSummary_moveFile();
    void tool_streamingSummary_moveAfterHunkIgnored();
    void tool_streamingSummary_deleteFile();
    void tool_streamingSummary_firstSectionWins();
    void tool_detailsMarkdown();

    // EditFileTool
    void editfile_exact();
    void editfile_multipleEdits();
    void editfile_multiline();
    void editfile_uniqueError();
    void editfile_replaceAll();
    void editfile_notFound();
    void editfile_noChange();
    void editfile_emptyOldText();
    void editfile_overlap();
    void editfile_fuzzyUnicode();
    void editfile_fuzzyTrailingWhitespace();
    void editfile_crlf();
    void editfile_bom();
    void editfile_missingFile();
    void editfile_emptyEdits();
    void editfile_toolDefinition();
    void editfile_summaries();

    // WriteTool
    void write_newFile();
    void write_relativePathInStartupProject();
    void write_nestedDirs();
    void write_overwrite();
    void write_missingPath();
    void write_missingContent();
    void write_markdownContent();
    void write_parentIsFile();
    void write_toolDefinition();
    void write_summaries();

    // codeFence / summaryPreview / truncatedPreview
    void codeFence_escaping();
    void preview_truncatedPreview();
    void preview_bashTail();
    void preview_bashShort();
    void preview_editDiff();
    void preview_todoBase();

    // TodoWriteTool
    void todowrite_ok();
    void todowrite_emptyList();
    void todowrite_emptyContent();
    void todowrite_unknownStatus();
    void todowrite_twoInProgress();
    void todowrite_toolDefinition();
    void todowrite_summaries();

    // WebFetchTool
    void webfetch_normalizeUrl();
    void webfetch_htmlToMarkdown();
    void webfetch_htmlToText();
    void webfetch_toolDefinition();
    void webfetch_summaries();

    // WebUtils (task‑tree based HTTP)
    void webutils_httpGet();
    void webutils_httpPost();
    void webutils_httpGetTooLarge();

    // WebSearchTool
    void websearch_parseBraveResults();
    void websearch_parseTavilyResults();
    void websearch_toolDefinition();
    void websearch_summaries();
    void websearch_exaEndpointUrl();
    void websearch_parseMcpResponse();
    void websearch_parseMcpResponseSse();
    void websearch_parseGoogleResults();
    void websearch_formatResults();

    // BashTool
    void bash_run();
    void bash_stderrMerged();
    void bash_nonZeroExit();
    void bash_missingWorkdir();
    void bash_emptyCommand();
    void bash_timeout();
    void bash_truncation();
    void bash_summaries();
    void bash_detailsMarkdown();

    // TaskTool
    void task_toolDefinition();
    void task_summaries();
    void task_toolsFor();
    void task_systemPrompt();
    void task_finalReport();

    // SearchTool
    void search_toolDefinition();
    void search_run();
    void search_noMatches();
    void search_limit();
    void search_context();
    void search_ignoresGitDir();
    void search_findsDotfiles();
    void search_badPath();
    void search_summaries();

    // FindTool
    void find_toolDefinition();
    void find_run();
    void find_noMatches();
    void find_limit();
    void find_dotfiles();
    void find_badPath();
    void find_summaries();

    // Ripgrep (download module)
    void ripgrep_metadata();
    void ripgrep_resolvedPath();

    // McpTool (Qt Creator MCP server tools)
    void mcptool_toolDefinition();
    void mcptool_oneLineSummary();
    void mcptool_streamingSummary();
    void mcptool_detailsMarkdown();

    // Factory registration
    void factory_applyPatch();
    void factory_webTools();
    void factory_task();
    void factory_searchFind();
    void factory_remoteProvider();
};

static QTemporaryDir *gTempDir = nullptr;

void LlamaToolsTest::initTestCase()
{
    gTempDir = new QTemporaryDir;
    QVERIFY2(gTempDir->isValid(), "Failed to create temporary directory");
    Core::DocumentManager::setProjectsDirectory(Utils::FilePath::fromString(gTempDir->path()));
}

void LlamaToolsTest::cleanupTestCase()
{
    ProjectExplorer::ProjectManager::resetStartupProject();
    delete gTempDir;
    gTempDir = nullptr;
}

// ============================================================================
// Patch::parse
// ============================================================================

void LlamaToolsTest::parse_addFile()
{
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: test.txt\n"
        "+Hello World\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks.first().type, Patch::HunkType::Add);
    QCOMPARE(hunks.first().path, QString("test.txt"));
    QCOMPARE(hunks.first().contents, QString("Hello World"));
}

void LlamaToolsTest::parse_addUpdateDelete()
{
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: add.txt\n"
        "+added\n"
        "*** Update File: update.txt\n"
        "@@ section\n"
        "-old\n"
        "+new\n"
        "*** Delete File: delete.txt\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.size(), 3);

    QCOMPARE(hunks[0].type, Patch::HunkType::Add);
    QCOMPARE(hunks[0].path, QString("add.txt"));
    QCOMPARE(hunks[0].contents, QString("added"));

    QCOMPARE(hunks[1].type, Patch::HunkType::Update);
    QCOMPARE(hunks[1].path, QString("update.txt"));
    QCOMPARE(hunks[1].chunks.size(), 1);
    QCOMPARE(hunks[1].chunks.first().oldLines, QStringList({"old"}));
    QCOMPARE(hunks[1].chunks.first().newLines, QStringList({"new"}));
    QCOMPARE(hunks[1].chunks.first().changeContext, QString("section"));
    QVERIFY(!hunks[1].chunks.first().endOfFile);

    QCOMPARE(hunks[2].type, Patch::HunkType::Delete);
    QCOMPARE(hunks[2].path, QString("delete.txt"));
}

void LlamaToolsTest::parse_move()
{
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: old/name.txt\n"
        "*** Move to: renamed/name.txt\n"
        "@@\n"
        "-old content\n"
        "+new content\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks.first().type, Patch::HunkType::Update);
    QCOMPARE(hunks.first().path, QString("old/name.txt"));
    QCOMPARE(hunks.first().movePath, QString("renamed/name.txt"));
}

void LlamaToolsTest::parse_missingMarkers()
{
    QVector<Patch::Hunk> hunks;
    const QString error = Patch::parse(QStringLiteral("just some text"), hunks);
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains("Begin/End"));
    QVERIFY(hunks.isEmpty());
}

void LlamaToolsTest::parse_missingMarkersTolerated()
{
    // The model forgot the envelope markers - the parser must still accept it.
    const QString patchText = QStringLiteral("*** Add File: x.txt\n+hi");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.size(), 1);
    QCOMPARE(hunks.first().type, Patch::HunkType::Add);
    QCOMPARE(hunks.first().contents, QString("hi"));
}

void LlamaToolsTest::parse_updateWithoutChunks()
{
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: update.txt\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    const QString error = Patch::parse(patchText, hunks);
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains("at least one @@ chunk"));
}

void LlamaToolsTest::parse_emptyEnvelope()
{
    const QString patchText = QStringLiteral("*** Begin Patch\n*** End Patch");

    QVector<Patch::Hunk> hunks;
    const QString error = Patch::parse(patchText, hunks);
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains("no hunks"));
}

void LlamaToolsTest::parse_addFileMissingPrefixTolerated()
{
    // A content line without the '+' prefix is kept as file content rather
    // than rejected or silently dropped.
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: test.txt\n"
        "+first\n"
        "missing plus prefix\n"
        "+last\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.first().contents, QString("first\nmissing plus prefix\nlast"));
}

void LlamaToolsTest::parse_addFileStarLines()
{
    // Lines starting with '*' (comments, markdown emphasis) are content, not
    // section markers; only "*** " starts a new section.
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: doc.md\n"
        "+/**\n"
        " * doxygen comment\n"
        "+*/\n"
        "***bold*** text\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.first().contents, QString("/**\n * doxygen comment\n*/\n***bold*** text"));
}

void LlamaToolsTest::parse_crlfTolerated()
{
    // CRLF line endings must not leak into the content.
    const QString patchText = QStringLiteral(
        "*** Begin Patch\r\n"
        "*** Add File: test.txt\r\n"
        "+first\r\n"
        "second\r\n"
        "*** End Patch\r\n");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.first().contents, QString("first\nsecond"));
}

void LlamaToolsTest::parse_addFileBlankLineTolerated()
{
    // An empty line in an Add File section stands for an empty file line.
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: test.txt\n"
        "+a\n"
        "\n"
        "+b\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    QCOMPARE(Patch::parse(patchText, hunks), QString());
    QCOMPARE(hunks.first().contents, QString("a\n\nb"));
}

void LlamaToolsTest::parse_chunkInvalidLine()
{
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: test.txt\n"
        "@@\n"
        "-old\n"
        "x not a valid change line\n"
        "+new\n"
        "*** End Patch");

    QVector<Patch::Hunk> hunks;
    const QString error = Patch::parse(patchText, hunks);
    QVERIFY(!error.isEmpty());
    QVERIFY(error.contains("Invalid update chunk line"));
    QVERIFY(hunks.isEmpty());
}

// ============================================================================
// Patch::applyUpdateChunks
// ============================================================================

void LlamaToolsTest::match_exact()
{
    const QString result
        = applyChunks(QStringLiteral("line1\nold\nline3"), {makeChunk({"old"}, {"new"})});
    QCOMPARE(result, QString("line1\nnew\nline3\n"));
}

void LlamaToolsTest::match_trailingWhitespace()
{
    const QString result = applyChunks(QStringLiteral("line1  \nline2\nline3   \n"),
                                       {makeChunk({"line2"}, {"changed"})});
    QCOMPARE(result, QString("line1  \nchanged\nline3   \n"));
}

void LlamaToolsTest::match_unicodeNormalized()
{
    const QString original = QStringLiteral("He said \u201Chello\u201D\nsome\u2014dash\nend\n");
    const QString result = applyChunks(original,
                                       {makeChunk({QStringLiteral("He said \"hello\"")},
                                                  {QStringLiteral("He said \"hi\"")})});
    QCOMPARE(result, QString("He said \"hi\"\nsome\u2014dash\nend\n"));
}

void LlamaToolsTest::match_contextDisambiguates()
{
    const QString original = QStringLiteral("fn a\nx=10\ny=2\nfn b\nx=10\ny=20\n");
    const QString result = applyChunks(original, {makeChunk({"x=10"}, {"x=11"}, "fn b")});
    QCOMPARE(result, QString("fn a\nx=10\ny=2\nfn b\nx=11\ny=20\n"));
}

void LlamaToolsTest::match_endOfFileAnchor()
{
    const QString original = QStringLiteral("start\nmarker\nmiddle\nmarker\nend\n");
    const QString result
        = applyChunks(original, {makeChunk({"marker", "end"}, {"marker-changed", "end"}, {}, true)});
    QCOMPARE(result, QString("start\nmarker\nmiddle\nmarker-changed\nend\n"));
}

void LlamaToolsTest::match_pureInsertion()
{
    const QString result = applyChunks(QStringLiteral("a\nb\n"), {makeChunk({}, {"x"})});
    QCOMPARE(result, QString("a\nb\nx\n"));
}

void LlamaToolsTest::match_trailingBlankLineTolerance()
{
    // The chunk carries a trailing blank context line the file does not have
    // (missing final newline) - it must still match.
    const QString result
        = applyChunks(QStringLiteral("x\n"), {makeChunk({"x", ""}, {"y", ""})});
    QCOMPARE(result, QString("y\n"));
}

void LlamaToolsTest::match_outOfOrderFails()
{
    // Chunks must appear in file order: after matching "4" the search
    // cursor is past "2", so the second chunk must fail.
    const QString result
        = applyChunks(QStringLiteral("1\n2\n3\n4\n"),
                      {makeChunk({"4"}, {"44"}), makeChunk({"2"}, {"22"})});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("Failed to find expected lines"));
}

void LlamaToolsTest::match_bomPreserved()
{
    const QString result
        = applyChunks(QString(QChar(0xFEFF)) + "a\nb\n", {makeChunk({"b"}, {"c"})});
    QCOMPARE(result, QString(QString(QChar(0xFEFF)) + "a\nc\n"));
}

void LlamaToolsTest::match_notFound()
{
    const QString result = applyChunks(QStringLiteral("foo\n"), {makeChunk({"bar"}, {"baz"})});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("Failed to find expected lines"));
}

void LlamaToolsTest::match_contextNotFound()
{
    const QString result = applyChunks(QStringLiteral("foo\n"), {makeChunk({"foo"}, {"bar"}, "nope")});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("Failed to find context"));
}

void LlamaToolsTest::match_closestMatchHint()
{
    // The chunk almost matches (typo in the middle line) – the error should
    // point at the closest match and the first differing line.
    const QString result = applyChunks(QStringLiteral("alpha\nbeta\ngamma\ndelta\n"),
                                       {makeChunk({"alpha", "bett", "gamma"}, {"A"})});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("Failed to find expected lines"));
    QVERIFY(result.contains("Closest match at line 1"));
    QVERIFY(result.contains("1 of 3 lines matched"));
    QVERIFY(result.contains("bett"));
    QVERIFY(result.contains("beta"));
}

void LlamaToolsTest::match_outOfOrderHint()
{
    // The second chunk matches, but before the first one – the error should
    // say the hunks are out of file order instead of a bare "not found".
    const QString result = applyChunks(QStringLiteral("first\nA\nmiddle\nB\nlast\n"),
                                       {makeChunk({"B"}, {"2"}), makeChunk({"A"}, {"1"})});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("in file order"));
    QVERIFY(result.contains("line 2"));
}

// ============================================================================
// ApplyPatchTool::run
// ============================================================================

void LlamaToolsTest::tool_addFileInStartupProject()
{
    // Same resolution rule as write: a patch that adds a file with a
    // relative path must create it inside the startup project.
    const Utils::FilePath projectDir = Utils::FilePath::fromString(gTempDir->filePath("myproject"));
    QVERIFY(projectDir.ensureWritableDir());
    ProjectExplorer::ProjectManager::setStartupProject(projectDir.toString());

    QJsonObject args;
    args[QStringLiteral("patchText")] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: src/added.txt\n"
        "+added\n"
        "*** End Patch");

    auto [output, ok] = runTool(args);
    QVERIFY2(ok, qPrintable(output));

    QCOMPARE(readTextFile(projectDir.pathAppended("src/added.txt").toString()),
             QString("added\n"));
    QVERIFY(!QFile::exists(gTempDir->filePath("src/added.txt")));

    ProjectExplorer::ProjectManager::resetStartupProject();
}

void LlamaToolsTest::tool_fullPatch()
{
    writeTextFile(gTempDir->filePath("delete.txt"), "to delete\n");
    writeTextFile(gTempDir->filePath("modify.txt"), "line1\nline2\n");

    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: nested/new.txt\n"
        "+created\n"
        "*** Delete File: delete.txt\n"
        "*** Update File: modify.txt\n"
        "@@\n"
        "-line2\n"
        "+changed\n"
        "*** End Patch");

    QJsonObject args;
    args["patchText"] = patchText;

    auto [output, ok] = runTool(args);
    QVERIFY2(ok, qPrintable(output));
    QVERIFY(output.contains("A nested/new.txt"));
    QVERIFY(output.contains("D delete.txt"));
    QVERIFY(output.contains("M modify.txt"));

    QCOMPARE(readTextFile(gTempDir->filePath("nested/new.txt")), QString("created\n"));
    QVERIFY(!QFile::exists(gTempDir->filePath("delete.txt")));
    QCOMPARE(readTextFile(gTempDir->filePath("modify.txt")), QString("line1\nchanged\n"));
}

void LlamaToolsTest::tool_move()
{
    writeTextFile(gTempDir->filePath("a.txt"), "old content\n");

    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: a.txt\n"
        "*** Move to: dir/b.txt\n"
        "@@\n"
        "-old content\n"
        "+new content\n"
        "*** End Patch");

    QJsonObject args;
    args["patchText"] = patchText;

    auto [output, ok] = runTool(args);
    QVERIFY2(ok, qPrintable(output));
    QVERIFY(output.contains("M dir/b.txt"));

    QVERIFY(!QFile::exists(gTempDir->filePath("a.txt")));
    QCOMPARE(readTextFile(gTempDir->filePath("dir/b.txt")), QString("new content\n"));
}

void LlamaToolsTest::tool_updateSameFileTwice()
{
    // Two Update sections for the same file must both take effect: the second
    // is applied on top of the first, not on the original content.
    writeTextFile(gTempDir->filePath("multi.txt"), "l1\nl2\nl3\n");

    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: multi.txt\n"
        "@@\n"
        "-l2\n"
        "+two\n"
        "*** Update File: multi.txt\n"
        "@@\n"
        "-l3\n"
        "+three\n"
        "*** End Patch");

    QJsonObject args;
    args[QStringLiteral("patchText")] = patchText;

    auto [output, ok] = runTool(args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(readTextFile(gTempDir->filePath("multi.txt")), QString("l1\ntwo\nthree\n"));
}

void LlamaToolsTest::tool_updateNoChanges()
{
    // An Update whose removed and added lines are identical must be rejected
    // instead of reporting a no-op "success".
    writeTextFile(gTempDir->filePath("noop.txt"), "one\ntwo\n");

    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: noop.txt\n"
        "@@\n"
        "-two\n"
        "+two\n"
        "*** End Patch");

    QJsonObject args;
    args[QStringLiteral("patchText")] = patchText;

    auto [output, ok] = runTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("no changes made"));
    // The file must be left untouched.
    QCOMPARE(readTextFile(gTempDir->filePath("noop.txt")), QString("one\ntwo\n"));
}

void LlamaToolsTest::tool_emptyPatchText()
{
    QJsonObject args;
    auto [output, ok] = runTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("patchText"));
}

void LlamaToolsTest::tool_invalidFormat()
{
    QJsonObject args;
    args["patchText"] = "this is not a patch";
    auto [output, ok] = runTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("apply_patch verification failed"));
    QVERIFY(output.contains("missing Begin/End markers"));
}

void LlamaToolsTest::tool_emptyEnvelope()
{
    QJsonObject args;
    args["patchText"] = "*** Begin Patch\n*** End Patch";
    auto [output, ok] = runTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("no hunks"));
}

void LlamaToolsTest::tool_updateMissingFile()
{
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: does_not_exist.txt\n"
        "@@\n"
        "-x\n"
        "+y\n"
        "*** End Patch");

    QJsonObject args;
    args["patchText"] = patchText;
    auto [output, ok] = runTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("Failed to read file to update"));
}

void LlamaToolsTest::tool_atomicFailure()
{
    // A valid Add followed by an Update of a missing file: nothing
    // may be written to disk.
    const QString patchText = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: ok.txt\n"
        "+ok\n"
        "*** Update File: missing.txt\n"
        "@@\n"
        "-x\n"
        "+y\n"
        "*** End Patch");

    QJsonObject args;
    args["patchText"] = patchText;
    auto [output, ok] = runTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("apply_patch verification failed"));
    QVERIFY(!QFile::exists(gTempDir->filePath("ok.txt")));
}

void LlamaToolsTest::tool_toolDefinition()
{
    ApplyPatchTool tool;
    const QString def = tool.toolDefinition();

    QVERIFY(def.contains("apply_patch"));
    QVERIFY(def.contains("patchText"));
    QVERIFY(def.contains("*** Begin Patch"));
    QVERIFY(def.contains("required"));
    QVERIFY(def.contains("Example"));

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());

    // The definition must be human‑readable, i.e. formatted over multiple lines
    QVERIFY2(def.count(QLatin1Char('\n')) > 10, qPrintable(def));
}

void LlamaToolsTest::tool_oneLineSummary()
{
    ApplyPatchTool tool;

    QJsonObject add;
    add["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: moonphase7/main.cpp\n"
        "+int main() { return 0; }\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(add), QString("Add moonphase7/main.cpp"));

    QJsonObject single;
    single["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: src/main.cpp\n"
        "@@\n"
        "-a\n"
        "+b\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(single), QString("Edit src/main.cpp"));

    QJsonObject del;
    del["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Delete File: obsolete.txt\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(del), QString("Delete obsolete.txt"));

    QJsonObject move;
    move["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: a.txt\n"
        "*** Move to: dir/b.txt\n"
        "@@\n"
        "-a\n"
        "+b\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(move), QString("Move a.txt to dir/b.txt"));

    QJsonObject multi;
    multi["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: a.txt\n"
        "+x\n"
        "*** Delete File: b.txt\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(multi), QString("Apply patch to 2 files"));

    QJsonObject invalid;
    invalid["patchText"] = "garbage";
    QCOMPARE(tool.oneLineSummary(invalid), QString("Apply patch"));
}

void LlamaToolsTest::tool_streamingSummary_empty()
{
    ApplyPatchTool tool;
    QCOMPARE(tool.streamingSummary(QString()), QString());

    // Only the envelope has arrived so far - no file section yet.
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n\"")),
             QString());
}

void LlamaToolsTest::tool_streamingSummary_addFile()
{
    ApplyPatchTool tool;
    // The raw argument JSON as it looks mid-stream: the patch text is
    // truncated inside the JSON string value, newlines are escaped.
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n"
                 "*** Add File: moonphase8/main.cpp\\n"
                 "+int main() {")),
             QString("Add moonphase8/main.cpp"));
}

void LlamaToolsTest::tool_streamingSummary_updateFile()
{
    ApplyPatchTool tool;
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n"
                 "*** Update File: src/app.py\\n"
                 "@@ def greet():\\n"
                 " def greet():\\n"
                 "-    print(")),
             QString("Edit src/app.py"));
}

void LlamaToolsTest::tool_streamingSummary_moveFile()
{
    ApplyPatchTool tool;
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n"
                 "*** Update File: old/name.txt\\n"
                 "*** Move to: new/name.txt\\n"
                 "@@\\n"
                 "-old content")),
             QString("Move old/name.txt to new/name.txt"));
}

void LlamaToolsTest::tool_streamingSummary_moveAfterHunkIgnored()
{
    // A "Move to" that appears after the first hunk is not a rename.
    ApplyPatchTool tool;
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n"
                 "*** Update File: a.txt\\n"
                 "@@\\n"
                 "-x\\n"
                 "*** Move to: b.txt")),
             QString("Edit a.txt"));
}

void LlamaToolsTest::tool_streamingSummary_deleteFile()
{
    ApplyPatchTool tool;
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n"
                 "*** Delete File: obsolete.txt")),
             QString("Delete obsolete.txt"));
}

void LlamaToolsTest::tool_streamingSummary_firstSectionWins()
{
    // Multiple files: the summary reflects the first file section.
    ApplyPatchTool tool;
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n"
                 "*** Add File: a.txt\\n"
                 "+x\\n"
                 "*** Update File: b.txt\\n"
                 "@@")),
             QString("Add a.txt"));
}

void LlamaToolsTest::tool_detailsMarkdown()
{
    ApplyPatchTool tool;

    // Created file: full content in a language‑tagged code block, no diff
    QJsonObject addArgs;
    addArgs["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: src/main.cpp\n"
        "+int main() { return 0; }\n"
        "*** End Patch");
    const QString addDetails
        = tool.detailsMarkdown(addArgs, QStringLiteral("Success. Updated the following files:\nA src/main.cpp"), true);
    // Single-file patch: no per-file header (the summary already says
    // "Add src/main.cpp").
    QVERIFY(!addDetails.contains("created"));
    QVERIFY(addDetails.startsWith("```cpp"));
    QVERIFY(addDetails.contains("int main() { return 0; }"));
    QVERIFY(!addDetails.contains("@@"));

    // Edited file: unified diff with line numbers in the @@ header
    writeTextFile(gTempDir->filePath("modify.txt"), "line1\nline2\n");
    QJsonObject editArgs;
    editArgs["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: modify.txt\n"
        "@@\n"
        "-line2\n"
        "+changed\n"
        "*** End Patch");
    auto [output, ok] = runTool(editArgs);
    QVERIFY2(ok, qPrintable(output));
    const QString editDetails = tool.detailsMarkdown(editArgs, output, ok);
    QVERIFY(!editDetails.contains("edited"));
    QVERIFY(editDetails.contains("@@ -2,1 +2,1 @@"));
    QVERIFY(editDetails.contains("-line2"));
    QVERIFY(editDetails.contains("+changed"));
    QVERIFY(editDetails.startsWith("```diff"));

    // Multi-file patches keep per-file headers to tell the sections apart
    // (the summary only says "Apply patch to N files" there).
    QJsonObject multiArgs;
    multiArgs[QStringLiteral("patchText")] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: a.txt\n"
        "+one\n"
        "*** Delete File: b.txt\n"
        "*** End Patch");
    const QString multiDetails = tool.detailsMarkdown(multiArgs, QStringLiteral("Success"), true);
    QVERIFY(multiDetails.contains(QStringLiteral("**created** `a.txt`")));
    QVERIFY(multiDetails.contains(QStringLiteral("**deleted** `b.txt`")));

    // Failed patch: the error text is shown, flagged with an error header
    QCOMPARE(tool.detailsMarkdown(addArgs, QStringLiteral("apply_patch verification failed: boom"), false),
             QString("**Error**\n\napply_patch verification failed: boom"));
}

// ============================================================================
// EditFileTool
// ============================================================================

void LlamaToolsTest::editfile_exact()
{
    QCOMPARE(applyEdits(QStringLiteral("alpha\nbeta\ngamma\n"),
                        {{QStringLiteral("beta"), QStringLiteral("B")}}),
              QString("alpha\nB\ngamma\n"));
}

void LlamaToolsTest::editfile_multipleEdits()
{
    // Both edits are matched against the original content.
    QCOMPARE(applyEdits(QStringLiteral("one\ntwo\nthree\nfour\n"),
                        {{QStringLiteral("one"), QStringLiteral("1")},
                         {QStringLiteral("four"), QStringLiteral("4")}}),
              QString("1\ntwo\nthree\n4\n"));
}

void LlamaToolsTest::editfile_multiline()
{
    QCOMPARE(applyEdits(QStringLiteral("int main()\n{\n    return 1;\n}\n"),
                        {{QStringLiteral("    return 1;"), QStringLiteral("    return 0;")}}),
              QString("int main()\n{\n    return 0;\n}\n"));
}

void LlamaToolsTest::editfile_uniqueError()
{
    const QString result = applyEdits(QStringLiteral("x\nx\n"), {{QStringLiteral("x"), QStringLiteral("y")}});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("Found 2 occurrences"));
    QVERIFY(result.contains("replace_all"));
}

void LlamaToolsTest::editfile_replaceAll()
{
    QString newContent;
    int replacements = 0;
    const QString err = Tools::applyTextEdits(QStringLiteral("x\ny\nx\n"),
                                               {{QStringLiteral("x"), QStringLiteral("X")}},
                                               true,
                                               newContent,
                                               &replacements);
    QCOMPARE(err, QString());
    QCOMPARE(newContent, QString("X\ny\nX\n"));
    QCOMPARE(replacements, 2);
}

void LlamaToolsTest::editfile_notFound()
{
    const QString result = applyEdits(QStringLiteral("alpha\n"), {{QStringLiteral("beta"), QStringLiteral("B")}});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("Could not find"));
}

void LlamaToolsTest::editfile_noChange()
{
    const QString result = applyEdits(QStringLiteral("alpha\n"), {{QStringLiteral("alpha"), QStringLiteral("alpha")}});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("identical"));
}

void LlamaToolsTest::editfile_emptyOldText()
{
    const QString result = applyEdits(QStringLiteral("alpha\n"), {{QString(), QStringLiteral("x")}});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("must not be empty"));
}

void LlamaToolsTest::editfile_overlap()
{
    const QString result = applyEdits(QStringLiteral("a b c\n"),
                                      {{QStringLiteral("a b"), QStringLiteral("1")},
                                       {QStringLiteral("b c"), QStringLiteral("2")}});
    QVERIFY(result.startsWith("<error:"));
    QVERIFY(result.contains("overlap"));
}

void LlamaToolsTest::editfile_fuzzyUnicode()
{
    // The file uses a smart quote the model did not reproduce – the exact
    // match fails, the line-based fuzzy match succeeds, and the rest of the
    // file keeps its original bytes.
    const QString file = QStringLiteral("line1\nsay \u201Chello\u201D\nline3\n");
    QCOMPARE(applyEdits(file, {{QStringLiteral("say \"hello\""), QStringLiteral("say goodbye")}}),
              QString("line1\nsay goodbye\nline3\n"));
}

void LlamaToolsTest::editfile_fuzzyTrailingWhitespace()
{
    // The model's oldText has a trailing space, the file a trailing tab:
    // the exact match fails, the fuzzy (trailing‑whitespace‑insensitive)
    // line match succeeds and the whole line is replaced.
    const QString file = QStringLiteral("int a = 1;   \nint b = 2;\t\n");
    QCOMPARE(applyEdits(file, {{QStringLiteral("int b = 2; "), QStringLiteral("int b = 3;")}}),
              QString("int a = 1;   \nint b = 3;\n"));
}

void LlamaToolsTest::editfile_crlf()
{
    QCOMPARE(applyEdits(QStringLiteral("a\r\nb\r\nc\r\n"),
                        {{QStringLiteral("b"), QStringLiteral("B")}}),
              QString("a\r\nB\r\nc\r\n"));
}

void LlamaToolsTest::editfile_bom()
{
    QCOMPARE(applyEdits(QString(QChar(0xFEFF)) + QStringLiteral("a\nb\n"),
                        {{QStringLiteral("b"), QStringLiteral("B")}}),
              QString(QChar(0xFEFF)) + QString("a\nB\n"));
}

void LlamaToolsTest::editfile_missingFile()
{
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("does_not_exist.txt");
    QJsonArray edits;
    QJsonObject edit;
    edit[QStringLiteral("oldText")] = QStringLiteral("x");
    edit[QStringLiteral("newText")] = QStringLiteral("y");
    edits.append(edit);
    args[QStringLiteral("edits")] = edits;

    auto [output, ok] = runTool<Tools::EditFileTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("file not found"));
}

void LlamaToolsTest::editfile_emptyEdits()
{
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("x.txt");
    args[QStringLiteral("edits")] = QJsonArray{};

    auto [output, ok] = runTool<Tools::EditFileTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("at least one replacement"));
}

void LlamaToolsTest::editfile_toolDefinition()
{
    Tools::EditFileTool tool;
    const QString def = tool.toolDefinition();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QCOMPARE(doc.object()["function"].toObject()["name"].toString(), QString("edit_file"));
}

void LlamaToolsTest::editfile_summaries()
{
    Tools::EditFileTool tool;

    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("src/main.cpp");
    QJsonArray edits;
    QJsonObject edit;
    edit[QStringLiteral("oldText")] = QStringLiteral("a");
    edit[QStringLiteral("newText")] = QStringLiteral("b");
    edits.append(edit);
    args[QStringLiteral("edits")] = edits;

    QCOMPARE(tool.oneLineSummary(args), QString("edit src/main.cpp"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"src/main.cpp\"")),
              QString("edit src/main.cpp"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString());

    // On success the details markdown shows the replacement as a diff,
    // without per-edit numbering.
    args[QStringLiteral("path")] = QStringLiteral("f.txt");
    const QString md = tool.detailsMarkdown(args, QStringLiteral("Successfully replaced 1 block(s) in f.txt."), true);
    QVERIFY(md.contains("```diff"));
    QVERIFY(md.contains("-a"));
    QVERIFY(md.contains("+b"));
    QVERIFY(!md.contains(QStringLiteral("**1**")));

    // Multiple edits still produce a single combined diff block.
    QJsonObject edit2;
    edit2[QStringLiteral("oldText")] = QStringLiteral("c");
    edit2[QStringLiteral("newText")] = QStringLiteral("d");
    edits.append(edit2);
    args[QStringLiteral("edits")] = edits; // QJsonObject copies – refresh
    const QString md2 = tool.detailsMarkdown(args, QStringLiteral("Successfully replaced 2 block(s) in f.txt."), true);
    QCOMPARE(md2.count(QStringLiteral("```diff")), 1);
    QVERIFY(md2.contains(QStringLiteral("-c")));
    QVERIFY(md2.contains(QStringLiteral("+d")));
}

// ============================================================================
// WriteTool
// ============================================================================

void LlamaToolsTest::write_relativePathInStartupProject()
{
    // A startup project in its own directory, distinct from the general
    // project directory (gTempDir).  A relative path to a new file must be
    // resolved against the project directory, not the general one.
    const Utils::FilePath projectDir = Utils::FilePath::fromString(gTempDir->filePath("myproject"));
    QVERIFY(projectDir.ensureWritableDir());
    ProjectExplorer::ProjectManager::setStartupProject(projectDir.toString());

    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("src/newfile.txt");
    args[QStringLiteral("content")] = QStringLiteral("in the project\n");

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY2(ok, qPrintable(output));

    QCOMPARE(readTextFile(projectDir.pathAppended("src/newfile.txt").toString()),
             QString("in the project\n"));
    QVERIFY(!QFile::exists(gTempDir->filePath("src/newfile.txt")));

    ProjectExplorer::ProjectManager::resetStartupProject();
}

void LlamaToolsTest::write_newFile()
{
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("hello.txt");
    args[QStringLiteral("content")] = QStringLiteral("world\n");

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QVERIFY(output.startsWith("Successfully wrote"));
    QCOMPARE(readTextFile(gTempDir->filePath("hello.txt")), QString("world\n"));
}

void LlamaToolsTest::write_nestedDirs()
{
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("a/b/c/deep.txt");
    args[QStringLiteral("content")] = QStringLiteral("deep\n");

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(readTextFile(gTempDir->filePath("a/b/c/deep.txt")), QString("deep\n"));
}

void LlamaToolsTest::write_overwrite()
{
    writeTextFile(gTempDir->filePath("old.txt"), QStringLiteral("old content\n"));

    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("old.txt");
    args[QStringLiteral("content")] = QStringLiteral("new content\n");

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(readTextFile(gTempDir->filePath("old.txt")), QString("new content\n"));
}

void LlamaToolsTest::write_missingPath()
{
    QJsonObject args;
    args[QStringLiteral("path")] = QString();
    args[QStringLiteral("content")] = QStringLiteral("x");

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("non-empty"));
}

void LlamaToolsTest::write_missingContent()
{
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("nocontent.txt");

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("content"));
}

void LlamaToolsTest::write_parentIsFile()
{
    writeTextFile(gTempDir->filePath("plainfile"), QStringLiteral("not a dir\n"));

    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("plainfile/inner.txt");
    args[QStringLiteral("content")] = QStringLiteral("x\n");

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("parent path is a file"));
}

void LlamaToolsTest::write_toolDefinition()
{
    Tools::WriteTool tool;
    const QString def = tool.toolDefinition();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject fn = doc.object()[QStringLiteral("function")].toObject();
    QCOMPARE(fn[QStringLiteral("name")].toString(), QString("write"));
    const QJsonObject params = fn[QStringLiteral("parameters")].toObject();
    QCOMPARE(params[QStringLiteral("required")].toArray().size(), 2);
}

void LlamaToolsTest::write_summaries()
{
    Tools::WriteTool tool;

    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("src/main.cpp");
    args[QStringLiteral("content")] = QStringLiteral("int main() {}\n");

    QCOMPARE(tool.oneLineSummary(args), QString("write src/main.cpp"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"src/main.cpp\"")),
              QString("write src/main.cpp"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString());

    // On success the details markdown shows the written content as a code block.
    const QString md = tool.detailsMarkdown(args, QStringLiteral("Successfully wrote 16 bytes to src/main.cpp."), true);
    QVERIFY(md.contains("```"));
    QVERIFY(md.contains("int main() {}"));
    QVERIFY(!md.startsWith(QStringLiteral("**Error**")));

    // A failed run shows the error, flagged with an error header.
    QVERIFY(tool.detailsMarkdown(args, QStringLiteral("Cannot write \"src/main.cpp\": boom"), false)
                .startsWith(QStringLiteral("**Error**\n\nCannot write")));
}

// ============================================================================
// codeFence / summaryPreview / truncatedPreview
// ============================================================================

void LlamaToolsTest::codeFence_escaping()
{
    // Plain content gets a normal fence.
    QCOMPARE(codeFence(QStringLiteral("plain")), QString("```\nplain\n```"));

    // Content with triple backticks needs a longer fence, or it would close
    // the block early and the inner markdown would render live.
    QCOMPARE(codeFence(QStringLiteral("# T\n\n```cpp\ncode\n```")),
             QString("````\n# T\n\n```cpp\ncode\n```\n````"));
    QCOMPARE(codeFence(QStringLiteral("a ``` b"), QStringLiteral("json")),
             QString("````json\na ``` b\n````"));

    // Content with quadruple backticks needs five.
    QCOMPARE(codeFence(QStringLiteral("x ```` y")),
             QString("`````\nx ```` y\n`````"));

    // Empty content is fine.
    QCOMPARE(codeFence(QString()), QString("```\n\n```"));
}

void LlamaToolsTest::write_markdownContent()
{
    // Writing a markdown file that contains code fences must not leak the
    // inner fences into the details view (they would render as live
    // markdown).
    const QString content = QStringLiteral("# Title\n\n```cpp\nint main() {}\n```\n");
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("README.md");
    args[QStringLiteral("content")] = content;

    auto [output, ok] = runTool<Tools::WriteTool>(args);
    QVERIFY2(ok, qPrintable(output));

    Tools::WriteTool tool;
    const QString md = tool.detailsMarkdown(args, output, true);
    QVERIFY(md.contains(QStringLiteral("````\n# Title")));
    QVERIFY(md.contains(QStringLiteral("```cpp\nint main() {}\n```")));
    // The outer fence is closed by a quadruple fence, not a triple one.
    QVERIFY(md.endsWith(QStringLiteral("````")));

    // The preview inherits the longer fence as well.
    const QString preview = tool.summaryPreview(args, output, true);
    QVERIFY(preview.contains(QStringLiteral("````")));
}

void LlamaToolsTest::tool_addMarkdownFile()
{
    // Same for apply_patch: a patch that adds a markdown file with code
    // blocks must fence the content with more than three backticks.
    QJsonObject args;
    args[QStringLiteral("patchText")] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Add File: docs/notes.md\n"
        "+# Hello\n"
        "+```python\n"
        "+print(1)\n"
        "+```\n"
        "*** End Patch");

    auto [output, ok] = runTool(args);
    QVERIFY2(ok, qPrintable(output));

    ApplyPatchTool tool;
    const QString md = tool.detailsMarkdown(args, output, true);
    QVERIFY(md.contains(QStringLiteral("````markdown\n# Hello")));
    QVERIFY(md.contains(QStringLiteral("```python\nprint(1)\n```")));
    QVERIFY(md.endsWith(QStringLiteral("````")));
}

// ============================================================================
// summaryPreview / truncatedPreview
// ============================================================================

void LlamaToolsTest::preview_truncatedPreview()
{
    // Empty input stays empty.
    QCOMPARE(truncatedPreview(QString(), 3), QString());

    // Short text passes through untouched (no ellipsis).
    QCOMPARE(truncatedPreview(QStringLiteral("a\nb\nc"), 3), QString("a\nb\nc"));

    // Line overflow: cut to maxLines, an open fence is closed, no ellipsis
    // (the expand icon in the header signals more content).
    const QString longMd = QStringLiteral("```\nline1\nline2\nline3\nline4\nline5");
    QCOMPARE(truncatedPreview(longMd, 3), QString("```\nline1\nline2\n```"));

    // An already balanced fence is not double-closed.
    const QString balanced = QStringLiteral("```\nx\n```\nmore\neven more");
    QCOMPARE(truncatedPreview(balanced, 2), QString("```\nx\n```"));

    // Character overflow also truncates and keeps the fence closed.
    const QString wide = QStringLiteral("```\n") + QString(500, QLatin1Char('x'));
    const QString wideCut = truncatedPreview(wide, 10);
    QVERIFY(wideCut.size() <= 300 + 10); // cut + fence close
    QVERIFY(wideCut.endsWith(QStringLiteral("\n```")));

    // Fenceless text is just cut.
    QCOMPARE(truncatedPreview(QStringLiteral("a\nb\nc\nd"), 2), QString("a\nb"));
}

void LlamaToolsTest::preview_bashTail()
{
    Tools::BashTool tool;

    // Long output: the *tail* is shown, prefixed with an ellipsis line.
    const QString output = QStringLiteral("l1\nl2\nl3\nl4\nl5");
    const QString preview = tool.summaryPreview(QJsonObject(), output, true);
    QCOMPARE(preview, QString("```\n…\nl4\nl5\n```"));
    QVERIFY(!preview.contains("l1"));

    // Short output passes through, still fenced.
    const QString shortPreview = tool.summaryPreview(QJsonObject(), QStringLiteral("ok"), true);
    QCOMPARE(shortPreview, QString("```\nok\n```"));

    // Empty output: no preview.
    QCOMPARE(tool.summaryPreview(QJsonObject(), QString(), true), QString());
}

void LlamaToolsTest::preview_bashShort()
{
    // A single long line is character‑capped.
    Tools::BashTool tool;
    const QString preview = tool.summaryPreview(QJsonObject(), QString(500, QLatin1Char('y')), true);
    QVERIFY(preview.size() < 300);
    // The fence stays clean; no trailing ellipsis.
    QVERIFY(preview.endsWith(QStringLiteral("\n```")));
}

void LlamaToolsTest::preview_editDiff()
{
    Tools::EditFileTool tool;

    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("f.txt");
    QJsonArray edits;
    QJsonObject edit;
    edit[QStringLiteral("oldText")] = QStringLiteral("a");
    edit[QStringLiteral("newText")] = QStringLiteral("b");
    edits.append(edit);
    args[QStringLiteral("edits")] = edits;

    const QString preview = tool.summaryPreview(
        args, QStringLiteral("Successfully replaced 1 block(s) in f.txt."), true);
    // The diff lines make it into the summary preview.
    QVERIFY(preview.contains("-a"));
    QVERIFY(preview.contains("+b"));
    QVERIFY(preview.contains("```diff"));
}

void LlamaToolsTest::preview_todoBase()
{
    // The base implementation truncates the details markdown (the
    // checklist for todo_write).
    Tools::TodoWriteTool tool;

    QJsonObject args;
    QJsonArray todos;
    for (int i = 0; i < 5; ++i) {
        QJsonObject t;
        t[QStringLiteral("content")] = QStringLiteral("task %1").arg(i);
        t[QStringLiteral("status")] = QStringLiteral("pending");
        todos.append(t);
    }
    args[QStringLiteral("todos")] = todos;

    const QString preview = tool.summaryPreview(args, QStringLiteral("Task list updated: 0 of 5 completed."), true);
    QVERIFY(preview.contains("- [ ] task 0"));
    QVERIFY(!preview.contains("task 4")); // cut by the line limit
    QVERIFY(preview.endsWith(QStringLiteral("- [ ] task 2")));
}

// ============================================================================
// TodoWriteTool
// ============================================================================

void LlamaToolsTest::todowrite_ok()
{
    Tools::TodoWriteTool todo;

    QJsonObject args;
    QJsonArray todos;
    QJsonObject t1;
    t1[QStringLiteral("content")] = QStringLiteral("Plan the work");
    t1[QStringLiteral("status")] = QStringLiteral("completed");
    QJsonObject t2;
    t2[QStringLiteral("content")] = QStringLiteral("Implement it");
    t2[QStringLiteral("status")] = QStringLiteral("in_progress");
    QJsonObject t3;
    t3[QStringLiteral("content")] = QStringLiteral("Test it");
    t3[QStringLiteral("status")] = QStringLiteral("pending");
    todos.append(t1);
    todos.append(t2);
    todos.append(t3);
    args[QStringLiteral("todos")] = todos;

    auto [output, ok] = runTool<Tools::TodoWriteTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QVERIFY(output.contains("1 of 3 completed"));

    const QString md = todo.detailsMarkdown(args, output, ok);
    QVERIFY(md.contains("- [x] Plan the work"));
    QVERIFY(md.contains("- [~] Implement it"));
    QVERIFY(md.contains("- [ ] Test it"));
}

void LlamaToolsTest::todowrite_emptyList()
{
    QJsonObject args;
    args[QStringLiteral("todos")] = QJsonArray{};

    auto [output, ok] = runTool<Tools::TodoWriteTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("at least one task"));
}

void LlamaToolsTest::todowrite_emptyContent()
{
    QJsonObject args;
    QJsonArray todos;
    QJsonObject t;
    t[QStringLiteral("content")] = QStringLiteral("  ");
    t[QStringLiteral("status")] = QStringLiteral("pending");
    todos.append(t);
    args[QStringLiteral("todos")] = todos;

    auto [output, ok] = runTool<Tools::TodoWriteTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("empty"));
}

void LlamaToolsTest::todowrite_unknownStatus()
{
    QJsonObject args;
    QJsonArray todos;
    QJsonObject t;
    t[QStringLiteral("content")] = QStringLiteral("x");
    t[QStringLiteral("status")] = QStringLiteral("done");
    todos.append(t);
    args[QStringLiteral("todos")] = todos;

    auto [output, ok] = runTool<Tools::TodoWriteTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("unknown status"));

    // A failed run shows the error text, not the rejected list.
    Tools::TodoWriteTool tool;
    QCOMPARE(tool.detailsMarkdown(args, output, false),
             QStringLiteral("**Error**\n\n%1").arg(output));
}

void LlamaToolsTest::todowrite_twoInProgress()
{
    QJsonObject args;
    QJsonArray todos;
    for (int i = 0; i < 2; ++i) {
        QJsonObject t;
        t[QStringLiteral("content")] = QStringLiteral("task %1").arg(i);
        t[QStringLiteral("status")] = QStringLiteral("in_progress");
        todos.append(t);
    }
    args[QStringLiteral("todos")] = todos;

    auto [output, ok] = runTool<Tools::TodoWriteTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("only one task"));
}

void LlamaToolsTest::todowrite_toolDefinition()
{
    Tools::TodoWriteTool tool;
    const QString def = tool.toolDefinition();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject fn = doc.object()[QStringLiteral("function")].toObject();
    QCOMPARE(fn[QStringLiteral("name")].toString(), QString("todo_write"));
}

void LlamaToolsTest::todowrite_summaries()
{
    Tools::TodoWriteTool tool;

    QJsonObject args;
    QJsonArray todos;
    QJsonObject t1;
    t1[QStringLiteral("content")] = QStringLiteral("a");
    t1[QStringLiteral("status")] = QStringLiteral("pending");
    QJsonObject t2;
    t2[QStringLiteral("content")] = QStringLiteral("b");
    t2[QStringLiteral("status")] = QStringLiteral("pending");
    todos.append(t1);
    todos.append(t2);
    args[QStringLiteral("todos")] = todos;

    QCOMPARE(tool.oneLineSummary(args), QString("update task list (2 tasks)"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"todos\": []")),
              QString("update task list"));
}

// ============================================================================
// WebFetchTool
// ============================================================================

void LlamaToolsTest::webfetch_normalizeUrl()
{
    QCOMPARE(Tools::normalizeFetchUrl(QStringLiteral(" https://doc.qt.io/qt-6/ ")),
             QString("https://doc.qt.io/qt-6/"));
    QCOMPARE(Tools::normalizeFetchUrl(QStringLiteral("http://localhost:8080/api")),
             QString("http://localhost:8080/api"));
    QVERIFY(Tools::normalizeFetchUrl(QStringLiteral("doc.qt.io/qt-6/")).isEmpty());
    QVERIFY(Tools::normalizeFetchUrl(QStringLiteral("ftp://example.com/x")).isEmpty());
    QVERIFY(Tools::normalizeFetchUrl(QString()).isEmpty());
}

void LlamaToolsTest::webfetch_htmlToMarkdown()
{
    const QString html = QStringLiteral(
        "<!DOCTYPE html><html><head><title>Ignored</title>"
        "<script>var x = 1;</script><style>body { color: red }</style></head><body>"
        "<h1>Title Here</h1>"
        "<p>Some <a href=\"https://example.com/page\">link</a>, "
        "<code>inline code</code> and <b>bold</b> text.</p>"
        "<ul><li>first item</li><li>second item</li></ul>"
        "<pre><code>int x = 1;\nint y = 2;</code></pre>"
        "</body></html>");

    const QString md = Tools::htmlToMarkdown(html);
    QVERIFY2(md.contains(QStringLiteral("# Title Here")), qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("[link](https://example.com/page)")), qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("`inline code`")), qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("**bold**")), qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("- first item")), qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("- second item")), qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("```\nint x = 1;\nint y = 2;\n```")), qPrintable(md));

    // Boilerplate must be stripped
    QVERIFY(!md.contains("var x = 1"));
    QVERIFY(!md.contains("color: red"));
    QVERIFY(!md.contains("Ignored"));
}

void LlamaToolsTest::webfetch_htmlToText()
{
    const QString html = QStringLiteral(
        "<html><head><script>secret()</script></head>"
        "<body><h2>Heading</h2><p>Visible <b>text</b> here.</p></body></html>");

    const QString text = Tools::htmlToText(html);
    QVERIFY2(text.contains("Heading"), qPrintable(text));
    QVERIFY2(text.contains("Visible text here."), qPrintable(text));
    QVERIFY(!text.contains("secret()"));
}

void LlamaToolsTest::webfetch_toolDefinition()
{
    Tools::WebFetchTool tool;
    const QString def = tool.toolDefinition();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QCOMPARE(doc.object()["function"].toObject()["name"].toString(), QString("webfetch"));
    QVERIFY(def.contains("markdown"));
    QVERIFY(def.contains("required"));
}

void LlamaToolsTest::webfetch_summaries()
{
    Tools::WebFetchTool tool;

    QJsonObject args;
    args["url"] = "https://doc.qt.io/qt-6/";
    QCOMPARE(tool.oneLineSummary(args), QString("fetch https://doc.qt.io/qt-6/"));

    // Partial argument JSON, still streaming in
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"url\": \"https://doc.qt.io/qt-6")),
             QString("fetch https://doc.qt.io/qt-6"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"url\": \"")), QString());
    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString());
}

// ============================================================================
// WebSearchTool
// ============================================================================

void LlamaToolsTest::websearch_parseBraveResults()
{
    // Fixture mirroring the real api.search.brave.com response structure
    const QString json = QStringLiteral(
        "{\"type\":\"application/json\",\"web\":{\"results\":["
        "{\"title\":\"Qt 6 Reference Docs\","
        "\"url\":\"https://doc.qt.io/qt-6/index.html\","
        "\"description\":\"Official documentation for Qt 6.\"},"
        "{\"title\":\"Second\",\"url\":\"https://example.org/second\"},"
        "{\"title\":\"Dropped\"}]},\"takeaways\":{\"results\":[]}}");

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);

    const auto results = Tools::parseBraveResults(doc.object());
    QCOMPARE(results.size(), 2);
    QCOMPARE(results[0].title, QString("Qt 6 Reference Docs"));
    QCOMPARE(results[0].url, QString("https://doc.qt.io/qt-6/index.html"));
    QCOMPARE(results[0].snippet, QString("Official documentation for Qt 6."));

    // A result without a description gets an empty snippet
    QCOMPARE(results[1].title, QString("Second"));
    QCOMPARE(results[1].url, QString("https://example.org/second"));
    QVERIFY(results[1].snippet.isEmpty());

    QVERIFY(Tools::parseBraveResults(QJsonObject()).isEmpty());
}

void LlamaToolsTest::websearch_parseTavilyResults()
{
    // Fixture mirroring the real api.tavily.com response structure
    const QString json = QStringLiteral(
        "{\"query\":\"qt 6\",\"results\":["
        "{\"title\":\"Qt 6 Reference Docs\","
        "\"url\":\"https://doc.qt.io/qt-6/index.html\","
        "\"content\":\"Official documentation for Qt 6.\"},"
        "{\"title\":\"Second\",\"url\":\"https://example.org/second\"},"
        "{\"title\":\"Dropped\"}],\"answer\":null}");

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);

    const auto results = Tools::parseTavilyResults(doc.object());
    QCOMPARE(results.size(), 2);
    QCOMPARE(results[0].title, QString("Qt 6 Reference Docs"));
    QCOMPARE(results[0].url, QString("https://doc.qt.io/qt-6/index.html"));
    QCOMPARE(results[0].snippet, QString("Official documentation for Qt 6."));

    // A result without content gets an empty snippet
    QCOMPARE(results[1].title, QString("Second"));
    QCOMPARE(results[1].url, QString("https://example.org/second"));
    QVERIFY(results[1].snippet.isEmpty());

    QVERIFY(Tools::parseTavilyResults(QJsonObject()).isEmpty());
}

void LlamaToolsTest::websearch_toolDefinition()
{
    Tools::WebSearchTool tool;
    const QString def = tool.toolDefinition();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QCOMPARE(doc.object()["function"].toObject()["name"].toString(), QString("websearch"));
}

void LlamaToolsTest::websearch_summaries()
{
    Tools::WebSearchTool tool;

    QJsonObject args;
    args["query"] = "qt webengine offline";
    QCOMPARE(tool.oneLineSummary(args), QString("search qt webengine offline"));

    QCOMPARE(tool.streamingSummary(QStringLiteral(
                  "{\"query\": \"qt webengine")),
              QString("search qt webengine"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"query\": \"")), QString());
}

void LlamaToolsTest::websearch_exaEndpointUrl()
{
    // Without a key the endpoint is used as‑is
    QCOMPARE(Tools::exaEndpointUrl(QStringLiteral("https://mcp.exa.ai/mcp"), QString()),
              QString("https://mcp.exa.ai/mcp"));

    // With a key it is appended as a query parameter
    QCOMPARE(Tools::exaEndpointUrl(QStringLiteral("https://mcp.exa.ai/mcp"),
                                   QStringLiteral("secret-key")),
              QString("https://mcp.exa.ai/mcp?exaApiKey=secret-key"));

    // Pre‑existing query parameters are preserved
    QCOMPARE(Tools::exaEndpointUrl(QStringLiteral("https://mcp.exa.ai/mcp?x=1"),
                                   QStringLiteral("k2")),
              QString("https://mcp.exa.ai/mcp?x=1&exaApiKey=k2"));

    QVERIFY(Tools::exaEndpointUrl(QString(), QStringLiteral("k")).isEmpty());
}

void LlamaToolsTest::websearch_parseMcpResponse()
{
    // Plain JSON body
    const QString json = QStringLiteral(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"content\":"
        "[{\"type\":\"text\",\"text\":\"Title: A\\nURL: https://a.example\"}]}}");
    QCOMPARE(Tools::parseMcpSearchResponse(json),
              QString("Title: A\nURL: https://a.example"));

    // A non‑text content item is skipped
    const QString mixed = QStringLiteral(
        "{\"result\":{\"content\":[{\"type\":\"image\"},{\"type\":\"text\",\"text\":\"hello\"}]}}");
    QCOMPARE(Tools::parseMcpSearchResponse(mixed), QString("hello"));

    // Garbage yields an empty string
    QCOMPARE(Tools::parseMcpSearchResponse(QStringLiteral("not json")), QString());
    QCOMPARE(Tools::parseMcpSearchResponse(QStringLiteral("{\"result\":{}}")), QString());
    QCOMPARE(Tools::parseMcpSearchResponse(QString()), QString());
}

void LlamaToolsTest::websearch_parseMcpResponseSse()
{
    // Server‑sent‑events body, as returned by mcp.exa.ai
    const QString sse = QStringLiteral(
        "event: message\n"
        "data: {\"result\":{\"content\":["
        "{\"type\":\"text\",\"text\":\"Title: B\\nURL: https://b.example\"}]}}\n"
        "\n");
    QCOMPARE(Tools::parseMcpSearchResponse(sse),
              QString("Title: B\nURL: https://b.example"));

    // A leading non‑JSON data line is skipped
    const QString sseSkipped = QStringLiteral(
        "data: [NOTICE] synthetic\n"
        "data: {\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"ok\"}]}}\n");
    QCOMPARE(Tools::parseMcpSearchResponse(sseSkipped), QString("ok"));
}

void LlamaToolsTest::websearch_parseGoogleResults()
{
    QJsonObject response;
    QJsonArray items;
    QJsonObject first;
    first["title"] = "First";
    first["link"] = "https://first.example";
    first["snippet"] = "First snippet";
    items.append(first);
    QJsonObject second;
    second["title"] = "Second";
    second["link"] = "https://second.example";
    items.append(second);
    QJsonObject noLink;
    noLink["title"] = "Dropped";
    items.append(noLink);
    response["items"] = items;

    const auto results = Tools::parseGoogleResults(response);
    QCOMPARE(results.size(), 2);
    QCOMPARE(results[0].title, QString("First"));
    QCOMPARE(results[0].url, QString("https://first.example"));
    QCOMPARE(results[0].snippet, QString("First snippet"));
    QCOMPARE(results[1].title, QString("Second"));
    QVERIFY(results[1].snippet.isEmpty());

    QVERIFY(Tools::parseGoogleResults(QJsonObject()).isEmpty());
}

void LlamaToolsTest::websearch_formatResults()
{
    QVector<Tools::SearchResult> results;
    Tools::SearchResult a;
    a.title = QStringLiteral("A");
    a.url = QStringLiteral("https://a.example");
    a.snippet = QStringLiteral("Snippet A");
    results.append(a);
    Tools::SearchResult b;
    b.title = QStringLiteral("B");
    b.url = QStringLiteral("https://b.example");
    results.append(b);

    QCOMPARE(Tools::formatResults(QStringLiteral("q"), results),
              QString("Search results for \"q\":\n\n"
                      "1. A\n   https://a.example\n   Snippet A\n\n"
                      "2. B\n   https://b.example"));
}

// ============================================================================
// BashTool
// ============================================================================

void LlamaToolsTest::bash_run()
{
    QJsonObject args;
    args["command"] = QStringLiteral("echo llama-bash-test");

    const auto [output, ok] = runBashTool(args);
    QVERIFY(ok);
    QVERIFY(output.contains("llama-bash-test"));
    QVERIFY(!output.contains("truncated"));
    QVERIFY(!output.contains("exit code"));
}

void LlamaToolsTest::bash_stderrMerged()
{
    QJsonObject args;
    args["command"] = QStringLiteral("echo llama-stdout && echo llama-stderr 1>&2");

    const auto [output, ok] = runBashTool(args);
    QVERIFY(ok);
    QVERIFY(output.contains("llama-stdout"));
    QVERIFY(output.contains("llama-stderr"));
}

void LlamaToolsTest::bash_nonZeroExit()
{
    QJsonObject args;
    args["command"] = QStringLiteral("echo partial-output && exit 3");

    const auto [output, ok] = runBashTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("partial-output"));
    QVERIFY(output.contains("exited with code 3"));
}

void LlamaToolsTest::bash_missingWorkdir()
{
    QJsonObject args;
    args["command"] = QStringLiteral("echo should-not-run");
    args["workdir"] = QStringLiteral("/nonexistent/llama-bash-dir");

    const auto [output, ok] = runBashTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("working directory does not exist"));
}

void LlamaToolsTest::bash_emptyCommand()
{
    QJsonObject args;
    args["command"] = QStringLiteral("   ");

    const auto [output, ok] = runBashTool(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("must not be empty"));
}

void LlamaToolsTest::bash_timeout()
{
    QJsonObject args;
    // bash/POSIX on all platforms (Windows uses Git Bash)
    args["command"] = QStringLiteral("sleep 30");
    args["timeout"] = 500;

    const auto [output, ok] = runBashTool(args, 30000);
    QVERIFY(!ok);
    QVERIFY(output.contains("timed out after 500 ms"));
}

void LlamaToolsTest::bash_truncation()
{
    QJsonObject args;
    // bash/POSIX on all platforms (Windows uses Git Bash)
    args["command"] = QStringLiteral("seq 1 3000");

    const auto [output, ok] = runBashTool(args);
    QVERIFY(ok);
    QVERIFY(output.contains("[Output truncated: showing last 2000 of 3000 lines"));
    QVERIFY(output.contains("Full output saved to: "));
    // The tail is kept ...
    QVERIFY(output.contains(QStringLiteral("2999\n3000\n")));
    // ... and the head is gone.
    QVERIFY(!output.contains(QStringLiteral("\n2\n")));

    // The full output was saved to a temp file.
    const int from = output.lastIndexOf(QStringLiteral("Full output saved to: "));
    const QString path = output.mid(from + QStringLiteral("Full output saved to: ").size()).trimmed();
    QFileInfo saved(path);
    QVERIFY2(saved.exists(), qPrintable(path));
    const QString full = readTextFile(path);
    QVERIFY(full.startsWith("1\n"));
    QVERIFY(full.endsWith("3000\n"));
}

void LlamaToolsTest::bash_summaries()
{
    Tools::BashTool tool;
    QCOMPARE(tool.name(), QString("bash"));

    QJsonObject args;
    args["command"] = QStringLiteral("git status");
    QCOMPARE(tool.oneLineSummary(args), QString("running `git status`"));

    // Multi‑line commands (heredocs etc.) are shortened to the first line.
    args["command"] = QStringLiteral("cat <<'EOF'\nhello\nEOF");
    QCOMPARE(tool.oneLineSummary(args), QString("running `cat <<'EOF' …`"));

    // Backticks in the command need a double‑backtick code span.
    args["command"] = QStringLiteral("echo \"`date`\"");
    QCOMPARE(tool.oneLineSummary(args), QString("running ``echo \"`date`\"``"));

    QVERIFY(tool.toolDefinition().contains("\"bash\""));
    QVERIFY(tool.toolDefinition().contains("120000"));
}

void LlamaToolsTest::bash_detailsMarkdown()
{
    Tools::BashTool tool;

    QJsonObject args;
    args["command"] = QStringLiteral("git status");
    args["workdir"] = QStringLiteral("/some/dir");
    const QString md = tool.detailsMarkdown(args, QStringLiteral("On branch main"), true);
    QVERIFY(md.contains("```bash\ngit status\n```"));
    QVERIFY(md.contains("/some/dir"));
    QVERIFY(md.contains("On branch main"));

    QJsonObject noWorkdir;
    noWorkdir["command"] = QStringLiteral("git status");
    QVERIFY(!tool.detailsMarkdown(noWorkdir, QStringLiteral("out"), true)
                 .contains("Working directory"));

    // A failed run is flagged with an error header
    QVERIFY(tool.detailsMarkdown(args, QStringLiteral("Command exited with code 1."), false)
                .startsWith(QStringLiteral("**Error**")));
    QVERIFY(!md.startsWith(QStringLiteral("**Error**")));
}

// ============================================================================
// TaskTool
// ============================================================================

void LlamaToolsTest::task_toolDefinition()
{
    Tools::TaskTool tool;
    const QString def = tool.toolDefinition();

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    const QJsonObject function = doc.object()["function"].toObject();
    QCOMPARE(function["name"].toString(), QString("task"));

    const QJsonObject params = function["parameters"].toObject();
    QStringList required;
    for (const QJsonValue &v : params["required"].toArray())
        required << v.toString();
    QCOMPARE(required, (QStringList{QString("description"), QString("prompt")}));
    const QJsonArray enumValues = params["properties"].toObject()["subagent_type"]
                                      .toObject()["enum"].toArray();
    QStringList enumList;
    for (const QJsonValue &v : enumValues)
        enumList << v.toString();
    QCOMPARE(enumList, (QStringList{QString("explore"), QString("general")}));
}

void LlamaToolsTest::task_summaries()
{
    Tools::TaskTool tool;

    QJsonObject args;
    args["description"] = "Explore build system";
    QCOMPARE(tool.oneLineSummary(args), QString("task: Explore build system"));

    // The whole call must stay on a single line: when this QCOMPARE spans
    // multiple lines the clang 21 preprocessor in this environment fails
    // with "unterminated function‑like macro invocation".
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"description\": \"Explore build")),
             QString("task: Explore build"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"description\": \"")), QString());
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"prompt\": \"x\"")), QString());

    args["prompt"] = "Find the entry point";
    const QString md = tool.detailsMarkdown(args, QStringLiteral("Found main()"), true);
    QVERIFY(md.contains("### Explore build system"));
    QVERIFY(md.contains("Find the entry point"));
    QVERIFY(md.contains("Found main()"));
}

void LlamaToolsTest::task_toolsFor()
{
    const QStringList all = {QStringLiteral("task"),
                             QStringLiteral("ask_user"),
                             QStringLiteral("read_file"),
                             QStringLiteral("bash"),
                             QStringLiteral("edit_file")};

    // explore: only the read‑only subset
    QCOMPARE(Tools::taskToolsFor(QStringLiteral("explore"), all),
             (QStringList{QStringLiteral("read_file")}));

    // general: everything except task and ask_user, original order kept
    QCOMPARE(Tools::taskToolsFor(QStringLiteral("general"), all),
             (QStringList{QStringLiteral("read_file"),
                          QStringLiteral("bash"),
                          QStringLiteral("edit_file")}));
}

void LlamaToolsTest::task_systemPrompt()
{
    const QString explore = Tools::taskSystemPromptFor(QStringLiteral("explore"));
    const QString general = Tools::taskSystemPromptFor(QStringLiteral("general"));
    QVERIFY(!explore.isEmpty());
    QVERIFY(!general.isEmpty());
    QVERIFY(explore.contains("explore"));
    QVERIFY(general.contains("general"));
    QVERIFY(explore != general);
}

void LlamaToolsTest::task_finalReport()
{
    const QString start = ThinkingSectionParser::startToken();
    const QString end = ThinkingSectionParser::endToken();

    QCOMPARE(Tools::taskFinalReport(QStringLiteral("plain report")),
             QString("plain report"));
    QCOMPARE(Tools::taskFinalReport(start + "thinking here" + end + " final answer"),
             QString("final answer"));
    QCOMPARE(Tools::taskFinalReport(start + "only thinking"), QString());
    QCOMPARE(Tools::taskFinalReport(QString()), QString());
}

// ============================================================================
// SearchTool
// ============================================================================

void LlamaToolsTest::search_toolDefinition()
{
    Tools::SearchTool tool;
    QCOMPARE(tool.name(), QString("search"));

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(tool.toolDefinition().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());

    const QJsonObject function = doc.object()["function"].toObject();
    QCOMPARE(function["name"].toString(), QString("search"));
    QVERIFY(!function["description"].toString().isEmpty());

    const QJsonObject parameters = function["parameters"].toObject();
    QCOMPARE(parameters["type"].toString(), QString("object"));
    QVERIFY(parameters["strict"].toBool());
    const QJsonObject properties = parameters["properties"].toObject();
    for (const QString &name : {QStringLiteral("pattern"), QStringLiteral("path"),
                                 QStringLiteral("glob"), QStringLiteral("ignore_case"),
                                 QStringLiteral("literal"), QStringLiteral("context"),
                                 QStringLiteral("limit")})
        QVERIFY2(properties.contains(name), qPrintable(name));
    const QJsonArray required = parameters["required"].toArray();
    QCOMPARE(required.size(), 1);
    QCOMPARE(required.first().toString(), QString("pattern"));
}

void LlamaToolsTest::search_run()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/search_run");
    QDir().mkpath(root + QStringLiteral("/sub"));
    writeTextFile(root + QStringLiteral("/alpha.cpp"), "int alpha() { return 1; }\n");
    writeTextFile(root + QStringLiteral("/sub/beta.cpp"), "int beta() { return 2; }\n");
    writeTextFile(root + QStringLiteral("/notes.txt"), "alpha beta gamma\n");

    Tools::SearchTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("alpha");
    args["path"] = root;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    // Matches carry a path relative to the search root, a line number, the text.
    QVERIFY(output.contains(QStringLiteral("alpha.cpp:1: int alpha() { return 1; }")));
    QVERIFY(output.contains(QStringLiteral("notes.txt:1: alpha beta gamma")));
    // beta.cpp holds no "alpha".
    QVERIFY(!output.contains("beta.cpp"));
}

void LlamaToolsTest::search_noMatches()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/search_none");
    QDir().mkpath(root);
    writeTextFile(root + QStringLiteral("/a.txt"), "hello\n");

    Tools::SearchTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("zorgy_no_such_match");
    args["path"] = root;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY(ok);
    QCOMPARE(output, QString("No matches found."));
}

void LlamaToolsTest::search_limit()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/search_limit");
    QDir().mkpath(root);
    QString content;
    for (int i = 1; i <= 30; ++i)
        content += QStringLiteral("needle line %1\n").arg(i);
    writeTextFile(root + QStringLiteral("/many.txt"), content);

    Tools::SearchTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("needle");
    args["path"] = root;
    args["limit"] = 5;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(output.count("needle line "), 5);
    QVERIFY(output.contains("5 matches limit reached. Use limit=10"));
}

void LlamaToolsTest::search_context()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/search_ctx");
    QDir().mkpath(root);
    writeTextFile(root + QStringLiteral("/ctx.txt"), "line1\nMATCH\nline3\nline4\n");

    Tools::SearchTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("MATCH");
    args["path"] = root;
    args["context"] = 1;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("ctx.txt-1- line1")));
    QVERIFY(output.contains(QStringLiteral("ctx.txt:2: MATCH")));
    QVERIFY(output.contains(QStringLiteral("ctx.txt-3- line3")));
    // line4 lies outside the context window.
    QVERIFY(!output.contains("line4"));
}

void LlamaToolsTest::search_ignoresGitDir()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/search_git");
    QDir().mkpath(root + QStringLiteral("/.git"));
    // ".git" holds a line with the pattern; it must never be reported.
    writeTextFile(root + QStringLiteral("/.git/config"), "[core]\n\tno_match_needle = 1\n");
    writeTextFile(root + QStringLiteral("/a.cpp"), "needle in code\n");

    Tools::SearchTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("needle");
    args["path"] = root;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("a.cpp:1: needle in code")));
    // The .git directory is never searched.
    QVERIFY(!output.contains(".git/"));
}

void LlamaToolsTest::search_findsDotfiles()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/search_dot");
    QDir().mkpath(root + QStringLiteral("/.github/workflows"));
    writeTextFile(root + QStringLiteral("/.github/workflows/ci.yml"), "jobs: build-needle\n");
    writeTextFile(root + QStringLiteral("/a.cpp"), "other\n");

    Tools::SearchTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("build-needle");
    args["path"] = root;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    // Hidden files (e.g. GitHub workflows) are still searched.
    QVERIFY(output.contains(QStringLiteral(".github/workflows/ci.yml:1: jobs: build-needle")));
}

void LlamaToolsTest::search_badPath()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    Tools::SearchTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("x");
    args["path"] = QStringLiteral("/nonexistent/llama-search-dir");

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY(!ok);
    QVERIFY(output.contains("path does not exist"));
}

void LlamaToolsTest::search_summaries()
{
    Tools::SearchTool tool;
    QCOMPARE(tool.name(), QString("search"));

    QJsonObject args;
    args["pattern"] = QStringLiteral("foo bar");
    QCOMPARE(tool.oneLineSummary(args), QString("search for `foo bar`"));

    // Backticks in the pattern need a double‑backtick code span.
    args["pattern"] = QStringLiteral("a`b");
    QCOMPARE(tool.oneLineSummary(args), QString("search for ``a`b``"));

    // No pattern yet.
    QCOMPARE(tool.oneLineSummary(QJsonObject()), QString());

    // Streaming: the pattern is summarised only once its closing quote arrives.
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"pattern\": \"abc\"}")),
             QString("search for `abc`"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"pattern\": \"abc")), QString());
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"x\"}")), QString());
    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString());

    // detailsMarkdown shows the pattern, the glob and the raw result.
    QJsonObject mdArgs;
    mdArgs["pattern"] = QStringLiteral("alpha");
    mdArgs["glob"] = QStringLiteral("*.cpp");
    const QString md = tool.detailsMarkdown(mdArgs, QStringLiteral("alpha.cpp:1: x"), true);
    QVERIFY(md.contains("Pattern: `alpha`"));
    QVERIFY(md.contains("Glob: `*.cpp`"));
    QVERIFY(md.contains("alpha.cpp:1: x"));
}

// ============================================================================
// FindTool
// ============================================================================

void LlamaToolsTest::find_toolDefinition()
{
    Tools::FindTool tool;
    QCOMPARE(tool.name(), QString("find"));

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(tool.toolDefinition().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());

    const QJsonObject function = doc.object()["function"].toObject();
    QCOMPARE(function["name"].toString(), QString("find"));
    QVERIFY(!function["description"].toString().isEmpty());

    const QJsonObject parameters = function["parameters"].toObject();
    QCOMPARE(parameters["type"].toString(), QString("object"));
    QVERIFY(parameters["strict"].toBool());
    const QJsonObject properties = parameters["properties"].toObject();
    for (const QString &name : {QStringLiteral("pattern"), QStringLiteral("path"),
                                 QStringLiteral("limit")})
        QVERIFY2(properties.contains(name), qPrintable(name));
    const QJsonArray required = parameters["required"].toArray();
    QCOMPARE(required.size(), 1);
    QCOMPARE(required.first().toString(), QString("pattern"));
}

void LlamaToolsTest::find_run()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/find_run");
    QDir().mkpath(root + QStringLiteral("/sub"));
    writeTextFile(root + QStringLiteral("/a.cpp"), "a\n");
    writeTextFile(root + QStringLiteral("/sub/b.cpp"), "b\n");
    writeTextFile(root + QStringLiteral("/c.h"), "c\n");

    Tools::FindTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("*.cpp");
    args["path"] = root;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    // Paths are relative to the search root; *.cpp matches nested files too.
    QVERIFY(output.contains("a.cpp"));
    QVERIFY(output.contains("sub/b.cpp"));
    QVERIFY(!output.contains("c.h"));
}

void LlamaToolsTest::find_noMatches()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/find_none");
    QDir().mkpath(root);
    writeTextFile(root + QStringLiteral("/a.txt"), "a\n");

    Tools::FindTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("zzz_no_such_*");
    args["path"] = root;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY(ok);
    QCOMPARE(output, QString("No files found."));
}

void LlamaToolsTest::find_limit()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/find_limit");
    QDir().mkpath(root);
    for (int i = 1; i <= 30; ++i)
        writeTextFile(root + QStringLiteral("/file_%1.txt").arg(i), QStringLiteral("x\n"));

    Tools::FindTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("*.txt");
    args["path"] = root;
    args["limit"] = 5;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(output.count("file_"), 5);
    QVERIFY(output.contains("5 results limit reached. Use limit=10"));
}

void LlamaToolsTest::find_dotfiles()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    const QString root = gTempDir->path() + QStringLiteral("/find_dot");
    QDir().mkpath(root + QStringLiteral("/.github/workflows"));
    QDir().mkpath(root + QStringLiteral("/.git"));
    writeTextFile(root + QStringLiteral("/.github/workflows/ci.yml"), "jobs: b\n");
    writeTextFile(root + QStringLiteral("/.git/config"), "x\n");
    writeTextFile(root + QStringLiteral("/config"), "x\n");

    Tools::FindTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("*");
    args["path"] = root;

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY2(ok, qPrintable(output));
    // Dotfiles are listed ... but never the .git directory.
    QVERIFY(output.contains(".github/workflows/ci.yml"));
    QVERIFY(output.contains("config"));
    QVERIFY(!output.contains(".git/")); // "\.git" alone would also match ".github"
    QVERIFY(!output.contains(".git/config"));
}

void LlamaToolsTest::find_badPath()
{
    if (!ripgrepAvailable())
        QSKIP("ripgrep is not available in this environment");

    Tools::FindTool tool;
    QJsonObject args;
    args["pattern"] = QStringLiteral("*.txt");
    args["path"] = QStringLiteral("/nonexistent/llama-find-dir");

    const auto [output, ok] = runAsyncTool(tool, args);
    QVERIFY(!ok);
    QVERIFY(output.contains("not a directory"));
}

void LlamaToolsTest::find_summaries()
{
    Tools::FindTool tool;
    QCOMPARE(tool.name(), QString("find"));

    QJsonObject args;
    args["pattern"] = QStringLiteral("*.cpp");
    QCOMPARE(tool.oneLineSummary(args), QString("find files `*.cpp`"));

    QCOMPARE(tool.oneLineSummary(QJsonObject()), QString());

    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"pattern\": \"*.ts\"}")),
             QString("find files `*.ts`"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"pattern\": \"*.ts")), QString());
    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString());

    QJsonObject mdArgs;
    mdArgs["pattern"] = QStringLiteral("*.json");
    mdArgs["path"] = QStringLiteral("/some/dir");
    const QString md = tool.detailsMarkdown(mdArgs, QStringLiteral("x.json"), true);
    QVERIFY(md.contains("Pattern: `*.json`"));
    QVERIFY(md.contains("Path: /some/dir"));
    QVERIFY(md.contains("x.json"));
}

// ============================================================================
// Ripgrep (download module)
// ============================================================================

void LlamaToolsTest::ripgrep_metadata()
{
    QCOMPARE(Tools::Ripgrep::version(), QString("15.2.0"));
    QCOMPARE(Tools::Ripgrep::dialogTitle(), QString("Download ripgrep"));
    // This build targets a supported desktop platform.
    QVERIFY(Tools::Ripgrep::isSupportedPlatform());
    // The downloaded copy lives under a per‑version user resource directory.
    const Utils::FilePath dir = Tools::Ripgrep::downloadDirectory();
    const QString dirString = dir.toUserOutput();
    QVERIFY2(dirString.endsWith(Tools::Ripgrep::version()), qPrintable(dirString));
}

void LlamaToolsTest::ripgrep_resolvedPath()
{
    const Utils::FilePath resolved = Tools::Ripgrep::resolvedPath();
    if (resolved.isEmpty()) {
        // Neither a system nor a downloaded copy exists.
        QVERIFY(!Tools::Ripgrep::isDownloaded());
        // The search tool then points at the settings‑page download.
        Tools::SearchTool tool;
        QJsonObject args;
        args["pattern"] = QStringLiteral("x");
        const auto [output, ok] = runAsyncTool(tool, args);
        QVERIFY(!ok);
        QVERIFY(output.contains("tools settings page"));
    } else {
        QVERIFY(resolved.isFile());
    }
}

// ============================================================================
// Factory registration
// ============================================================================

void LlamaToolsTest::factory_applyPatch()
{
    auto &factory = ToolFactory::instance();
    auto tool = factory.create("apply_patch");
    QVERIFY(tool != nullptr);
    QCOMPARE(tool->name(), QString("apply_patch"));
}

void LlamaToolsTest::factory_webTools()
{
    auto &factory = ToolFactory::instance();
    for (const QString &name : {QStringLiteral("webfetch"), QStringLiteral("websearch")}) {
        auto tool = factory.create(name);
        QVERIFY2(tool != nullptr, qPrintable(name));
        QCOMPARE(tool->name(), name);
    }
}

void LlamaToolsTest::factory_task()
{
    auto &factory = ToolFactory::instance();
    auto tool = factory.create("task");
    QVERIFY(tool != nullptr);
    QCOMPARE(tool->name(), QString("task"));
}

void LlamaToolsTest::factory_searchFind()
{
    auto &factory = ToolFactory::instance();
    for (const QString &name : {QStringLiteral("search"), QStringLiteral("find")}) {
        auto tool = factory.create(name);
        QVERIFY2(tool != nullptr, qPrintable(name));
        QCOMPARE(tool->name(), name);
    }
}

// ============================================================================
// McpTool / remote tool provider (Qt Creator MCP server)
// ============================================================================

void LlamaToolsTest::mcptool_toolDefinition()
{
    Tools::McpTool tool(QStringLiteral("some_mcp_tool"));
    QCOMPARE(tool.name(), QString("some_mcp_tool"));

    // No MCP server is connected in the test environment, so the definition
    // must fall back to a generic (but valid) schema.
    const QString def = tool.toolDefinition();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(def.toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);

    const QJsonObject function = doc.object()["function"].toObject();
    QCOMPARE(function["name"].toString(), QString("some_mcp_tool"));
    QVERIFY(!function["description"].toString().isEmpty());
    QCOMPARE(function["parameters"].toObject()["type"].toString(), QString("object"));
}

void LlamaToolsTest::mcptool_oneLineSummary()
{
    Tools::McpTool tool(QStringLiteral("build_project"));

    QJsonObject args;
    args["project_path"] = "CMakeLists.txt";
    QCOMPARE(tool.oneLineSummary(args), QString("build_project CMakeLists.txt"));

    // No arguments – just the tool name
    QCOMPARE(tool.oneLineSummary(QJsonObject()), QString("build_project"));

    // Non‑string arguments are stringified compactly
    QJsonObject num;
    num["timeout"] = 30;
    QCOMPARE(Tools::McpTool(QStringLiteral("x")).oneLineSummary(num), QString("x {\"timeout\":30}"));

    // Long values are truncated with an ellipsis
    QJsonObject longArg;
    longArg["path"] = QString(100, QLatin1Char('a'));
    QVERIFY(Tools::McpTool(QStringLiteral("t")).oneLineSummary(longArg).endsWith(QLatin1String("...")));
}

void LlamaToolsTest::mcptool_streamingSummary()
{
    Tools::McpTool tool(QStringLiteral("project_open"));

    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"~/Projects/foo")), QString("project_open ~/Projects/foo"));

    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"")), QString());
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": 42}")), QString());

    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString());
    QCOMPARE(tool.streamingSummary(QString()), QString());
}

void LlamaToolsTest::mcptool_detailsMarkdown()
{
    Tools::McpTool tool(QStringLiteral("build_project"));

    QJsonObject args;
    args["project_path"] = "CMakeLists.txt";
    const QString md = tool.detailsMarkdown(args, QStringLiteral("Build succeeded"), true);
    QVERIFY(md.contains("**Arguments**"));
    QVERIFY(md.contains("CMakeLists.txt"));
    QVERIFY(md.contains("Build succeeded"));
    QVERIFY(md.contains("```"));

    // Nothing to show
    QCOMPARE(tool.detailsMarkdown(QJsonObject(), QString(), true), QString());

    // A failed call is flagged with an error header
    QVERIFY(tool.detailsMarkdown(args, QStringLiteral("MCP tool failed: boom"), false)
                .startsWith(QStringLiteral("**Error**")));
    QVERIFY(!md.startsWith(QStringLiteral("**Error**")));
}

void LlamaToolsTest::factory_remoteProvider()
{
    auto &factory = ToolFactory::instance();
    FakeRemoteProvider provider;
    factory.setRemoteToolProvider(&provider);

    // Local tools are unaffected.
    QVERIFY(factory.create("apply_patch") != nullptr);

    // Remote tools are available through the same factory.
    auto remote = factory.create("remote_tool_a");
    QVERIFY(remote != nullptr);
    QCOMPARE(remote->name(), QString("remote_tool_a"));

    // creatorsList exposes both local and remote names.
    const QStringList list = factory.creatorsList();
    QVERIFY(list.contains("apply_patch"));
    QVERIFY(list.contains("remote_tool_a"));
    QVERIFY(list.contains("remote_tool_b"));

    // Without a provider the remote tools disappear again.
    factory.setRemoteToolProvider(nullptr);
    QVERIFY(factory.create("remote_tool_a") == nullptr);
    QVERIFY(!factory.creatorsList().contains("remote_tool_a"));
}

// WebUtils (task‑tree based HTTP)

void LlamaToolsTest::webutils_httpGet()
{
    quint16 port = 0;
    {
        MiniHttpServer server(QByteArrayLiteral("hello world"),
                              QByteArrayLiteral("text/plain; charset=utf-8"));
        QVERIFY(server.start(port));

        const WebResult result = runHttpCall([&](Tools::HttpResponseCallback callback) {
            Tools::httpGet(QStringLiteral("http://127.0.0.1:%1/greet").arg(port),
                           10,
                           1024 * 1024,
                           std::move(callback));
        });
        QVERIFY2(result.finished, "httpGet callback was not invoked");
        QCOMPARE(result.error, QString());
        QCOMPARE(result.body, QByteArrayLiteral("hello world"));
        QCOMPARE(result.contentType, QString("text/plain; charset=utf-8"));
    }
}

void LlamaToolsTest::webutils_httpPost()
{
    quint16 port = 0;
    {
        MiniHttpServer server({}, {}); // Echoes the POST body.
        QVERIFY(server.start(port));

        QList<QPair<QByteArray, QByteArray>> headers;
        headers.append({QByteArrayLiteral("X-Test"), QByteArrayLiteral("1")});

        const WebResult result = runHttpCall([&](Tools::HttpResponseCallback callback) {
            Tools::httpPost(QStringLiteral("http://127.0.0.1:%1/echo").arg(port),
                            QByteArrayLiteral("the-payload"),
                            headers,
                            10,
                            1024 * 1024,
                            std::move(callback));
        });
        QVERIFY2(result.finished, "httpPost callback was not invoked");
        QCOMPARE(result.error, QString());
        QCOMPARE(result.body, QByteArrayLiteral("the-payload"));
        QCOMPARE(result.contentType, QString("text/plain"));
    }
}

void LlamaToolsTest::webutils_httpGetTooLarge()
{
    quint16 port = 0;
    {
        const QByteArray largeBody(256 * 1024, 'x');
        MiniHttpServer server(largeBody, QByteArrayLiteral("application/octet-stream"));
        QVERIFY(server.start(port));

        const WebResult result = runHttpCall([&](Tools::HttpResponseCallback callback) {
            Tools::httpGet(QStringLiteral("http://127.0.0.1:%1/large").arg(port),
                           10,
                           64 * 1024,
                           std::move(callback));
        });
        QVERIFY2(result.finished, "httpGet callback was not invoked");
        QVERIFY(result.error.contains(QLatin1String("size limit")));
        QVERIFY(result.body.isEmpty());
    }
}

} // namespace LlamaCpp

QTEST_MAIN(LlamaCpp::LlamaToolsTest)
#include "llamatools_test.moc"
