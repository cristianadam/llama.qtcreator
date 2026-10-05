#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QEventLoop>
#include <QHostAddress>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QProcessEnvironment>
#include <QtTest/QtTest>

#include <unistd.h>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/projectmanager.h>

#include <utils/filepath.h>

#include <llamachatmanager.h>
#include <llamahtmlexporter.h>
#include <llamasettings.h>
#include <projectinstructions.h>
#include <skills.h>
#include <llamathinkingsectionparser.h>
#include <markdownrenderer.h>
#include <tools/apply_patch_tool.h>
#include <tools/factory.h>
#include <tools/mcptool.h>
#include <tools/windows_sandbox.h>
#include <tools/mcpserverconfig.h>
#include <tools/patch.h>
#include <tools/bash_tool.h>
#include <tools/readfile_tool.h>
#include <tools/skill_tool.h>
#include <tools/find_tool.h>
#include <tools/ls_tool.h>
#include <tools/ripgrep.h>
#include <tools/tool.h>
#include <tools/search_tool.h>
#include <tools/task_tool.h>
#include <tools/todowrite_tool.h>
#include <tools/webfetch_tool.h>
#include <tools/write_tool.h>
#include <tools/edit_file_tool.h>
#include <tools/tool_utils.h>
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
    void codeSpan_escaping();
    void preview_truncatedPreview();
    void preview_bashTail();
    void preview_bashShort();
    void preview_editDiff();
    void preview_todoBase();

    // TodoWriteTool
    void todowrite_ok();
    void todowrite_slidingWindow();
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
    void websearch_isConfigured();
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
    void bash_sandbox();
    void bash_sandboxWithStub();
    void bash_sandboxDenyRead();
    void bash_sandboxNetwork();
    void bashLiveOutputHandlerLifetime();
    void streamingToolCallAggregation();
    void toolResultOrdering();
    void sandboxFileTools();
    void projectSandboxOverride();
    void windowsSandboxSpecNotProvisioned();
    void windowsSandboxSpecUnavailable();
    void windowsSandboxSpec();
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

    // LsTool
    void ls_toolDefinition();
    void ls_run();
    void ls_emptyDir();
    void ls_limit();
    void ls_badPath();
    void ls_summaries();

    // Ripgrep (download module)
    void ripgrep_metadata();
    void ripgrep_resolvedPath();

    // McpServerConfig (configured MCP servers, settings JSON)
    void mcpserverconfig_jsonRoundTrip();
    void mcpserverconfig_invalidEntriesSkipped();

    // McpTool (MCP server tools)
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

    // ChatManager::messageToMarkdown
    void messageToMarkdown_userAssistant();
    void messageToMarkdown_thinkingSection();
    void messageToMarkdown_toolCall();
    void messageToMarkdown_toolOnlyAssistant();
    void messageToMarkdown_embedsCachedMermaidSvg();
    void messageToMarkdown_embedsCachedMathSvg();
    void messageToMarkdown_leavesUncachedDiagramsAlone();

    // HtmlExporter
    void htmlExport_conversationDocument();
    void htmlExport_userAssistant();
    void htmlExport_codeBlockHighlighted();
    void htmlExport_toolCall();
    void htmlExport_thinkingSection();
    void htmlExport_embedsCachedDiagrams();

    // Project instructions (AGENTS.md / CLAUDE.md)
    void projectInstructions_candidatePriority();
    void projectInstructions_walksToGitRoot();
    void projectInstructions_stopsAtGitRoot();
    void projectInstructions_noGitOnlyProjectDir();
    void projectInstructions_content();
    void projectInstructions_truncation();

    // Skills (scanner + prompt formatting)
    void skills_scanBasic();
    void skills_scanNestedAndRootFiles();
    void skills_scanSkillRootStopsRecursion();
    void skills_scanValidationAndCollisions();
    void skills_scanDisableModelInvocation();
    void skills_scanSkipsSymlinks();
    void skills_formatForPrompt();
    void skills_enabledSkillsFiltering();
    void skillTool_loadsSkill();
    void skillTool_truncatesLongContent();
    void skillTool_unknownSkill();

    // ReadFileTool limits
    void readfile_rangeContinuationHint();
    void readfile_detailsMarkdown();
    void readfile_wholeFileCap();
    void readfile_byteCap();
    void readfile_emptyFile();

    // ReadFileTool image support / tool result image helpers
    void readfile_imageAttachment();
    void readfile_imageTooLarge();
    void toolImage_wrapSplitRoundTrip();
    void toolImage_textFromContentParts();

    // EditFileTool argument tolerance / result
    void editfile_editsAsJsonString();
    void editfile_editsAsObject();
    void editfile_legacyTopLevel();
    void editfile_resultContainsDiff();

    // repairJson (string‑literal repair for small‑model tool arguments)
    void repairJson_controlChars();
    void repairJson_invalidEscapes();
    void repairJson_validUnchanged();
    void repairJson_structureNotFixed();
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
    QCOMPARE(tool.oneLineSummary(add), QString("Add `moonphase7/main.cpp`"));

    QJsonObject single;
    single["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: src/main.cpp\n"
        "@@\n"
        "-a\n"
        "+b\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(single), QString("Edit `src/main.cpp`"));

    QJsonObject del;
    del["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Delete File: obsolete.txt\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(del), QString("Delete `obsolete.txt`"));

    QJsonObject move;
    move["patchText"] = QStringLiteral(
        "*** Begin Patch\n"
        "*** Update File: a.txt\n"
        "*** Move to: dir/b.txt\n"
        "@@\n"
        "-a\n"
        "+b\n"
        "*** End Patch");
    QCOMPARE(tool.oneLineSummary(move), QString("Move `a.txt` to `dir/b.txt`"));

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
             QString("Add `moonphase8/main.cpp`"));
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
             QString("Edit `src/app.py`"));
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
             QString("Move `old/name.txt` to `new/name.txt`"));
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
             QString("Edit `a.txt`"));
}

void LlamaToolsTest::tool_streamingSummary_deleteFile()
{
    ApplyPatchTool tool;
    QCOMPARE(tool.streamingSummary(QStringLiteral(
                 "{\"patchText\": \"*** Begin Patch\\n"
                 "*** Delete File: obsolete.txt")),
             QString("Delete `obsolete.txt`"));
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
             QString("Add `a.txt`"));
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

    // Failed patch: the error text is shown as-is (no error header; the ✗
    // icon in the summary marks the failure).
    QCOMPARE(tool.detailsMarkdown(addArgs, QStringLiteral("apply_patch verification failed: boom"), false),
             QStringLiteral("apply_patch verification failed: boom"));
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

    QCOMPARE(tool.oneLineSummary(args), QString("edit `src/main.cpp`"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"src/main.cpp\"")),
              QString("edit `src/main.cpp`"));
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

    QCOMPARE(tool.oneLineSummary(args), QString("write `src/main.cpp`"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"src/main.cpp\"")),
              QString("write `src/main.cpp`"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString());

    // On success the details markdown shows the written content as a code block.
    const QString md = tool.detailsMarkdown(args, QStringLiteral("Successfully wrote 16 bytes to src/main.cpp."), true);
    QVERIFY(md.contains("```"));
    QVERIFY(md.contains("int main() {}"));
    QVERIFY(!md.startsWith(QStringLiteral("**Error**")));

    // A failed run shows the error as-is (no error header; the ✗ icon in
    // the summary marks the failure).
    QVERIFY(tool.detailsMarkdown(args, QStringLiteral("Cannot write \"src/main.cpp\": boom"), false)
                .startsWith(QStringLiteral("Cannot write")));
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

    // Content that already ends with a newline (file content, command
    // output) must not gain a spurious empty last line in the fence body.
    QCOMPARE(codeFence(QStringLiteral("a\nb\n")), QString("```\na\nb\n```"));
    QCOMPARE(codeFence(QStringLiteral("a\nb\n\n")), QString("```\na\nb\n\n```"));
}

void LlamaToolsTest::codeSpan_escaping()
{
    // Plain text gets a single code span.
    QCOMPARE(codeSpan(QStringLiteral("src/main.cpp")),
             QString("`src/main.cpp`"));

    // Text containing a backtick needs a double backtick, or the inner
    // backtick would close the span early and render live markdown.
    QCOMPARE(codeSpan(QStringLiteral("a`b")), QString("`` a`b ``"));
    QCOMPARE(codeSpan(QStringLiteral("`")), QString("`` ` ``"));

    // The summaries of the file tools use the same escaping for paths.
    Tools::WriteTool writeTool;
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("we`ird.txt");
    args[QStringLiteral("content")] = QStringLiteral("x");
    QCOMPARE(writeTool.oneLineSummary(args), QString("write `` we`ird.txt ``"));

    Tools::LsTool lsTool;
    QJsonObject lsArgs;
    lsArgs[QStringLiteral("path")] = QStringLiteral("we`ird");
    QCOMPARE(lsTool.oneLineSummary(lsArgs),
             QString("list directory `` we`ird ``"));
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
    QVERIFY(md.contains(QStringLiteral("````markdown\n# Title")));
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
    QCOMPARE(preview, QString("```terminal\n…\nl4\nl5\n```"));
    QVERIFY(!preview.contains("l1"));

    // Short output passes through, still fenced.
    const QString shortPreview = tool.summaryPreview(QJsonObject(), QStringLiteral("ok"), true);
    QCOMPARE(shortPreview, QString("```terminal\nok\n```"));

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
    QVERIFY(md.contains("- [ ] Implement it")); // in_progress renders as an unchecked box
    QVERIFY(md.contains("- [ ] Test it"));
}

void LlamaToolsTest::todowrite_slidingWindow()
{
    Tools::TodoWriteTool todo;

    auto makeArgs = [](const QStringList &statuses) {
        QJsonObject args;
        QJsonArray todos;
        for (int i = 0; i < statuses.size(); ++i) {
            QJsonObject t;
            t[QStringLiteral("content")] = QStringLiteral("task%1").arg(i + 1);
            t[QStringLiteral("status")] = statuses.at(i);
            todos.append(t);
        }
        args[QStringLiteral("todos")] = todos;
        return args;
    };

    // in_progress in the middle: the window is centred on it.
    QJsonObject args = makeArgs({QStringLiteral("completed"),
                                 QStringLiteral("completed"),
                                 QStringLiteral("in_progress"),
                                 QStringLiteral("pending"),
                                 QStringLiteral("pending")});
    QString md = todo.detailsMarkdown(args, QString(), true);
    QVERIFY(!md.contains("task1"));
    QVERIFY(md.contains("- [x] task2"));
    QVERIFY(md.contains("- [ ] task3"));
    QVERIFY(md.contains("- [ ] task4"));
    QVERIFY(!md.contains("task5"));

    // in_progress at the start: the first 3.
    args = makeArgs({QStringLiteral("in_progress"),
                     QStringLiteral("pending"),
                     QStringLiteral("pending"),
                     QStringLiteral("pending"),
                     QStringLiteral("pending")});
    md = todo.detailsMarkdown(args, QString(), true);
    QVERIFY(md.contains("- [ ] task1"));
    QVERIFY(md.contains("- [ ] task2"));
    QVERIFY(md.contains("- [ ] task3"));
    QVERIFY(!md.contains("task4"));

    // in_progress at the end: the last 3.
    args = makeArgs({QStringLiteral("completed"),
                     QStringLiteral("completed"),
                     QStringLiteral("completed"),
                     QStringLiteral("completed"),
                     QStringLiteral("in_progress")});
    md = todo.detailsMarkdown(args, QString(), true);
    QVERIFY(!md.contains("task1"));
    QVERIFY(!md.contains("task2"));
    QVERIFY(md.contains("- [x] task3"));
    QVERIFY(md.contains("- [x] task4"));
    QVERIFY(md.contains("- [ ] task5"));

    // all done: the last 3.
    args = makeArgs({QStringLiteral("completed"),
                     QStringLiteral("completed"),
                     QStringLiteral("completed"),
                     QStringLiteral("completed"),
                     QStringLiteral("completed")});
    md = todo.detailsMarkdown(args, QString(), true);
    QVERIFY(!md.contains("task1"));
    QVERIFY(!md.contains("task2"));
    QVERIFY(md.contains("- [x] task3"));
    QVERIFY(md.contains("- [x] task4"));
    QVERIFY(md.contains("- [x] task5"));

    // some done, none in progress: the first 3 (the completed head).
    args = makeArgs({QStringLiteral("completed"),
                     QStringLiteral("completed"),
                     QStringLiteral("pending"),
                     QStringLiteral("pending"),
                     QStringLiteral("pending")});
    md = todo.detailsMarkdown(args, QString(), true);
    QVERIFY(md.contains("- [x] task1"));
    QVERIFY(md.contains("- [x] task2"));
    QVERIFY(md.contains("- [ ] task3"));
    QVERIFY(!md.contains("task4"));

    // 3 or fewer tasks: always the whole list.
    args = makeArgs({QStringLiteral("in_progress"),
                     QStringLiteral("pending"),
                     QStringLiteral("pending")});
    md = todo.detailsMarkdown(args, QString(), true);
    QVERIFY(md.contains("- [ ] task1"));
    QVERIFY(md.contains("- [ ] task2"));
    QVERIFY(md.contains("- [ ] task3"));
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
    QCOMPARE(tool.detailsMarkdown(args, output, false), output);
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

void LlamaToolsTest::websearch_isConfigured()
{
    using Config = Tools::WebSearchConfig;

    // Exa: hosted endpoint needs a key.
    Config c;
    c.provider = QStringLiteral("exa");
    QVERIFY(!c.isConfigured());
    c.exaApiKey = QStringLiteral("secret");
    QVERIFY(c.isConfigured());

    // Exa: the settings aspect pre-fills the hosted URL as default - that
    // must not count as a configured custom endpoint (regression: the tool
    // would be advertised to every keyless user).
    c.exaApiKey.clear();
    c.exaUrl = QStringLiteral("https://mcp.exa.ai/mcp");
    QVERIFY(!c.isConfigured());

    // Exa: a *custom* endpoint (e.g. local proxy) works keyless.
    c.exaUrl = QStringLiteral("http://127.0.0.1:8080/mcp");
    QVERIFY(c.isConfigured());

    // Google: needs both key and cx.
    c = Config{};
    c.provider = QStringLiteral("google");
    QVERIFY(!c.isConfigured());
    c.googleApiKey = QStringLiteral("secret");
    QVERIFY(!c.isConfigured());
    c.googleCx = QStringLiteral("123");
    QVERIFY(c.isConfigured());

    // Brave / Tavily: key only.
    c = Config{};
    c.provider = QStringLiteral("brave");
    QVERIFY(!c.isConfigured());
    c.braveApiKey = QStringLiteral("secret");
    QVERIFY(c.isConfigured());

    c = Config{};
    c.provider = QStringLiteral("tavily");
    QVERIFY(!c.isConfigured());
    c.tavilyApiKey = QStringLiteral("secret");
    QVERIFY(c.isConfigured());
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

// Restores the global sandbox setting (and, when \a restoreHome is set,
// the HOME environment variable) when the scope is left, even when a
// failing assertion returns early from the test.
class SandboxTestEnv
{
public:
    explicit SandboxTestEnv(bool restoreHome = false)
        : m_sandbox(settings().sandboxCommands()),
          m_home(qEnvironmentVariable("HOME")),
          m_hadHome(!m_home.isEmpty()),
          m_restoreHome(restoreHome)
    {
        settings().sandboxCommands.setValue(true);
    }

    ~SandboxTestEnv()
    {
        settings().sandboxCommands.setValue(m_sandbox);
        if (m_restoreHome) {
            if (m_hadHome)
                qputenv("HOME", m_home.toUtf8());
            else
                qunsetenv("HOME");
        }
    }

private:
    bool m_sandbox = false;
    QString m_home;
    bool m_hadHome = false;
    bool m_restoreHome = false;
};

void LlamaToolsTest::bash_sandbox()
{
#if defined(Q_OS_WIN)
    if (QStandardPaths::findExecutable(QStringLiteral("srt-win")).isEmpty())
        QSKIP("srt-win (@anthropic-ai/sandbox-runtime) is not installed");
#elif defined(Q_OS_MACOS)
    if (QStandardPaths::findExecutable(QStringLiteral("sandbox-exec")).isEmpty())
        QSKIP("sandbox-exec is not available on this system");
#else
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        QSKIP("bubblewrap (bwrap) is not installed");
#endif

    QTemporaryDir workdir;
    QVERIFY(workdir.isValid());

    const SandboxTestEnv env;

    // Writing in the working directory keeps working inside the sandbox.
    QJsonObject args;
    args["command"] = QStringLiteral(
        "touch llama-sandbox-file && rm llama-sandbox-file && echo "
        "sandbox-cwd-ok");
    args["workdir"] = workdir.path();
    const auto [output, ok] = runBashTool(args);

    // Writing to a system location is refused.
    QJsonObject deniedArgs;
    deniedArgs["command"] = QStringLiteral("touch /usr/llama-sandbox-test");
    deniedArgs["workdir"] = workdir.path();
    const auto denied = runBashTool(deniedArgs);

    QVERIFY(ok);
    QVERIFY(output.contains("sandbox-cwd-ok"));

    QVERIFY(!denied.second);
    QVERIFY(!QFileInfo::exists(QStringLiteral("/usr/llama-sandbox-test")));
}

// The stub path in BashTool::run used to bypass the sandbox; this test
// installs a fake process stub (a script that execs everything after its
// "--" separator) via LLAMA_SHELL_STUB and verifies the sandbox still
// applies.
void LlamaToolsTest::bash_sandboxWithStub()
{
#if defined(Q_OS_WIN)
    if (QStandardPaths::findExecutable(QStringLiteral("srt-win")).isEmpty())
        QSKIP("srt-win (@anthropic-ai/sandbox-runtime) is not installed");
#elif defined(Q_OS_MACOS)
    if (QStandardPaths::findExecutable(QStringLiteral("sandbox-exec")).isEmpty())
        QSKIP("sandbox-exec is not available on this system");
#else
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        QSKIP("bubblewrap (bwrap) is not installed");
#endif

    QTemporaryDir stubDir;
    QVERIFY(stubDir.isValid());
    const QString stubPath = stubDir.path() + QStringLiteral("/fake_stub.sh");
    QFile stub(stubPath);
    QVERIFY(stub.open(QIODevice::WriteOnly));
    stub.write("#!/bin/sh\n"
               "while [ $# -gt 0 ] && [ \"$1\" != \"--\" ]; do shift; done\n"
               "shift\n"
               "exec \"$@\"\n");
    stub.close();
    QVERIFY(QFile::setPermissions(stubPath,
                                  QFile::ReadOwner | QFile::WriteOwner |
                                  QFile::ExeOwner | QFile::ReadGroup |
                                  QFile::ExeGroup | QFile::ReadOther |
                                  QFile::ExeOther));

    qputenv("LLAMA_SHELL_STUB", stubPath.toUtf8());

    QTemporaryDir workdir;
    QVERIFY(workdir.isValid());

    const SandboxTestEnv env;
    QJsonObject args;
    args["command"] = QStringLiteral("touch /usr/llama-sandbox-stub-test");
    args["workdir"] = workdir.path();
    const auto denied = runBashTool(args);

    qunsetenv("LLAMA_SHELL_STUB");

    QVERIFY(!denied.second);
    QVERIFY(!QFileInfo::exists(QStringLiteral("/usr/llama-sandbox-stub-test")));
}

void LlamaToolsTest::bash_sandboxDenyRead()
{
#if defined(Q_OS_WIN)
    if (QStandardPaths::findExecutable(QStringLiteral("srt-win")).isEmpty())
        QSKIP("srt-win (@anthropic-ai/sandbox-runtime) is not installed");
#elif defined(Q_OS_MACOS)
    if (QStandardPaths::findExecutable(QStringLiteral("sandbox-exec")).isEmpty())
        QSKIP("sandbox-exec is not available on this system");
#else
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        QSKIP("bubblewrap (bwrap) is not installed");
#endif

    // Point HOME at a temporary directory with a fake credential file so the
    // test never touches the real user's ~/.ssh & co.
    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    const QString keyFile = fakeHome.path() + QStringLiteral("/.ssh/id_test");
    QDir(fakeHome.path()).mkpath(QStringLiteral(".ssh"));
    QFile file(keyFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("llama-secret-key");

    QTemporaryDir workdir;
    QVERIFY(workdir.isValid());

    // Captures (and later restores) HOME and the sandbox setting.
    const SandboxTestEnv env(/* restoreHome = */ true);
    qputenv("HOME", fakeHome.path().toUtf8());
    QJsonObject args;
    args["command"] = QStringLiteral("cat .ssh/id_test");
    args["workdir"] = workdir.path();
    const auto [output, ok] = runBashTool(args);

    QVERIFY(!ok);
    QVERIFY(!output.contains("llama-secret-key"));
}

// Sandboxed commands have no network access: a connection to a local
// listener that succeeds without the sandbox must fail inside it. The
// listener makes the test deterministic (a refused connection to a closed
// port would fail with or without the sandbox).
void LlamaToolsTest::bash_sandboxNetwork()
{
#if defined(Q_OS_WIN)
    if (QStandardPaths::findExecutable(QStringLiteral("srt-win")).isEmpty())
        QSKIP("srt-win (@anthropic-ai/sandbox-runtime) is not installed");
#elif defined(Q_OS_MACOS)
    if (QStandardPaths::findExecutable(QStringLiteral("sandbox-exec")).isEmpty())
        QSKIP("sandbox-exec is not available on this system");
#else
    if (QStandardPaths::findExecutable(QStringLiteral("bwrap")).isEmpty())
        QSKIP("bubblewrap (bwrap) is not installed");
#endif

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    const quint16 port = server.serverPort();

    QTemporaryDir workdir;
    QVERIFY(workdir.isValid());

    const SandboxTestEnv env;
    QJsonObject args;
    // /dev/tcp is a bash builtin, so the test has no external dependencies.
    args["command"] = QStringLiteral("echo probe > /dev/tcp/127.0.0.1/%1")
                          .arg(port);
    args["workdir"] = workdir.path();

    // Control: the same connection works without the sandbox, so a failure
    // below is the sandbox's doing.
    settings().sandboxCommands.setValue(false);
    const auto control = runBashTool(args);
    settings().sandboxCommands.setValue(true);
    QVERIFY(control.second);

    const auto denied = runBashTool(args);
    QVERIFY(!denied.second);
    // sandbox-exec: EPERM from connect(); bwrap (empty network namespace,
    // loopback down): ENETUNREACH.
    QVERIFY(denied.first.contains("Operation not permitted")
            || denied.first.contains("Network is unreachable"));
}

// Streaming aggregation of (parallel) tool calls, as implemented in
// ChatManager::mergeToolCallDeltas()/finalizeToolCallBatch() – the same
// scheme as the llama.cpp web UI's mergeToolCallDeltas.
void LlamaToolsTest::streamingToolCallAggregation()
{
    auto deltas = [](std::initializer_list<QPair<int, QJsonObject>> list) {
        QJsonArray arr;
        for (const auto &p : list) {
            QJsonObject o = p.second;
            o["index"] = p.first;
            arr.append(o);
        }
        return arr;
    };
    auto fnDelta = [](const QString &name, const QString &args, const QString &id = QString()) {
        QJsonObject fn;
        if (!name.isEmpty())
            fn["name"] = name;
        if (!args.isEmpty())
            fn["arguments"] = args;
        QJsonObject o;
        o["function"] = fn;
        if (!id.isEmpty())
            o["id"] = id;
        return o;
    };
    auto committedCalls = [](const QList<QVariantMap> &committed) {
        QJsonArray calls;
        for (const QVariantMap &e : committed)
            calls.append(e["tool_calls"].toJsonArray().first());
        return calls;
    };

    // One call, arguments split over several deltas.
    {
        LlamaCpp::StreamingToolCalls state;
        QList<QVariantMap> committed;
        ChatManager::mergeToolCallDeltas(
            state, deltas({{0, fnDelta("read_file", "{\"path\": ")}}), &committed);
        ChatManager::mergeToolCallDeltas(
            state, deltas({{0, fnDelta(QString(), "\"a.cpp\"}")}}), &committed);
        QCOMPARE(committed.size(), 1);
        const QJsonObject call = committedCalls(committed).first().toObject();
        QCOMPARE(call["id"].toString(), QString("tool_0")); // synthetic id
        QCOMPARE(call["function"].toObject()["name"].toString(), QString("read_file"));
        QCOMPARE(call["function"].toObject()["arguments"].toString(),
                 QString("{\"path\": \"a.cpp\"}"));
        // Further deltas for the committed call must not commit it twice.
        ChatManager::mergeToolCallDeltas(state, deltas({{0, fnDelta(QString(), "")}}), &committed);
        QCOMPARE(committed.size(), 1);
    }

    // Two parallel calls with interleaved deltas; the first one completes
    // while the second is still streaming (the old code removed the
    // completed slot, which shifted the second call's index and lost its
    // early argument chunks).
    {
        LlamaCpp::StreamingToolCalls state;
        QList<QVariantMap> committed;
        ChatManager::mergeToolCallDeltas(
            state,
            deltas({{0, fnDelta("read_file", "{\"path\": \"a\"}", "id_a")},
                    {1, fnDelta("read_file", "{\"pa")}}),
            &committed);
        QCOMPARE(committed.size(), 1); // call 0 complete, call 1 not yet
        ChatManager::mergeToolCallDeltas(
            state, deltas({{1, fnDelta(QString(), "th\": \"b\"}")}}), &committed);
        QCOMPARE(committed.size(), 2);
        const QJsonArray calls = committedCalls(committed);
        QCOMPARE(calls.size(), 2);
        QCOMPARE(calls.at(0)["id"].toString(), QString("id_a"));
        QCOMPARE(calls.at(1)["id"].toString(), QString("tool_1"));
        QCOMPARE(calls.at(1)["function"].toObject()["arguments"].toString(),
                 QString("{\"path\": \"b\"}"));
    }

    // A text chunk between two tool-call batches re-bases the indices
    // (llama-server restarts them at 0); without the finalize the second
    // batch would overwrite the first call.
    {
        LlamaCpp::StreamingToolCalls state;
        QList<QVariantMap> committed;
        ChatManager::mergeToolCallDeltas(
            state, deltas({{0, fnDelta("read_file", "{\"path\": \"a\"}", "id_a")}}), &committed);
        ChatManager::finalizeToolCallBatch(state);
        ChatManager::mergeToolCallDeltas(
            state, deltas({{0, fnDelta("write", "{\"path\": \"b\"}", "id_b")}}), &committed);
        QCOMPARE(committed.size(), 2);
        const QJsonArray calls = committedCalls(committed);
        QCOMPARE(calls.at(0)["id"].toString(), QString("id_a"));
        QCOMPARE(calls.at(1)["id"].toString(), QString("id_b"));
    }

    // A delta without a usable index is appended at the end, not dropped.
    {
        LlamaCpp::StreamingToolCalls state;
        QList<QVariantMap> committed;
        QJsonObject noIndex = fnDelta("ls", "{}", "id_x");
        QJsonArray arr;
        arr.append(noIndex);
        ChatManager::mergeToolCallDeltas(state, arr, &committed);
        QCOMPARE(committed.size(), 1);
        QCOMPARE(committedCalls(committed).first()["id"].toString(), QString("id_x"));
    }

    // Two batches back-to-back with NO text chunk in between: the second
    // batch's index 0 must not merge into the first (committed) slot – it
    // carries a different id, so the batch is re-based implicitly.
    {
        LlamaCpp::StreamingToolCalls state;
        QList<QVariantMap> committed;
        ChatManager::mergeToolCallDeltas(
            state, deltas({{0, fnDelta("read_file", "{\"path\": \"a\"}", "id_a")}}), &committed);
        ChatManager::mergeToolCallDeltas(
            state, deltas({{0, fnDelta("write", "{\"path\": \"b\"}", "id_b")}}), &committed);
        QCOMPARE(committed.size(), 2);
        const QJsonArray calls = committedCalls(committed);
        QCOMPARE(calls.at(0)["id"].toString(), QString("id_a"));
        QCOMPARE(calls.at(1)["id"].toString(), QString("id_b"));
    }

    // Committed extras keep the slot (call) order, not the completion
    // order: call 1 finishes streaming before call 0.
    {
        LlamaCpp::StreamingToolCalls state;
        QList<QVariantMap> committed;
        ChatManager::mergeToolCallDeltas(
            state,
            deltas({{0, fnDelta("read_file", "{\"path\": ", "id_a")},
                    {1, fnDelta("read_file", "{\"path\": \"b\"}", "id_b")}}),
            &committed);
        QCOMPARE(committed.size(), 1); // call 1 complete, call 0 not yet
        ChatManager::mergeToolCallDeltas(
            state, deltas({{0, fnDelta(QString(), "\"a\"}")}}), &committed);
        QCOMPARE(committed.size(), 2);
        const QJsonArray calls = committedCalls(committed);
        QCOMPARE(calls.at(0)["id"].toString(), QString("id_a"));
        QCOMPARE(calls.at(1)["id"].toString(), QString("id_b"));
    }
}

// The tool results of a parallel batch are emitted in the assistant's
// tool_calls order (ChatManager::orderedToolResults, used by
// normalizeMsgsForAPI), regardless of the storage (completion) order, and
// a result whose call id the model did not reference is still sent.
void LlamaToolsTest::toolResultOrdering()
{
    auto toolMsg = [](qint64 id, qint64 parent, const QString &callId) {
        Message m;
        m.id = id;
        m.parent = parent;
        m.role = "tool";
        m.content = QStringLiteral("result of %1").arg(callId);
        QJsonObject toolResult;
        toolResult["tool_call_id"] = callId;
        toolResult["content"] = m.content;
        QVariantMap extra;
        extra["tool_result"] = toolResult;
        m.extra << extra;
        return m;
    };
    const Message a = toolMsg(10, 1, "call_a");
    const Message b = toolMsg(11, 1, "call_b");
    const Message c = toolMsg(12, 1, "call_c");

    // Call order a, b, c; storage order b, a, c → emitted a, b, c.
    {
        QHash<QString, const Message *> results;
        results.insert("call_b", &b);
        results.insert("call_a", &a);
        results.insert("call_c", &c);
        const QList<const Message *> ordered = ChatManager::orderedToolResults(
            results, {QStringLiteral("call_a"), QStringLiteral("call_b"),
                      QStringLiteral("call_c")});
        QCOMPARE(ordered.size(), 3);
        QCOMPARE(ordered.at(0), &a);
        QCOMPARE(ordered.at(1), &b);
        QCOMPARE(ordered.at(2), &c);
    }

    // A result the model did not reference is appended, not dropped.
    {
        QHash<QString, const Message *> results;
        results.insert("call_a", &a);
        results.insert("call_c", &c);
        const QList<const Message *> ordered = ChatManager::orderedToolResults(
            results, {QStringLiteral("call_a"), QStringLiteral("call_b")});
        QCOMPARE(ordered.size(), 2);
        QCOMPARE(ordered.at(0), &a);
        QCOMPARE(ordered.at(1), &c);
    }

    // No call order known → the results' iteration order.
    {
        QHash<QString, const Message *> results;
        results.insert("call_a", &a);
        const QList<const Message *> ordered =
            ChatManager::orderedToolResults(results, {});
        QCOMPARE(ordered.size(), 1);
        QCOMPARE(ordered.at(0), &a);
    }
}

// The live-output handler is a local that is destroyed while the command is
// still running – the tool must keep its own copy (regression test for a
// use‑after‑free of the caller's handler on the first readyRead).
void LlamaToolsTest::bashLiveOutputHandlerLifetime()
{
    Tools::BashTool tool;
    QJsonObject args;
    args[QStringLiteral("command")]
        = QStringLiteral("echo hello-live; sleep 0.3; echo done");

    QString liveTail;
    QString result;
    bool ok = false;
    bool finished = false;

    {
        // Deliberately scoped: it dies long before the command finishes.
        // (readyRead events are only delivered in the loop below, so with a
        // handler pointer into this scope the first delivery would touch a
        // dead object.)
        Tool::OutputHandler onOutput = [&liveTail](const QString &tail) {
            liveTail = tail;
        };
        tool.runLive(args,
                     onOutput,
                     [&result, &ok, &finished](const QString &out, bool success) {
                         result = out;
                         ok = success;
                         finished = true;
                     });
    }

    QEventLoop loop;
    QTimer poll;
    poll.setInterval(10);
    QObject::connect(&poll, &QTimer::timeout, [&] { if (finished)
                                                         loop.quit(); });
    poll.start();
    QTimer::singleShot(30000, &loop, [&] { poll.stop();
                                           loop.quit(); });
    loop.exec();

    QVERIFY2(finished, qPrintable(result));
    QVERIFY(ok);
    QVERIFY(result.contains(QStringLiteral("hello-live")));
    QVERIFY(result.contains(QStringLiteral("done")));
    // The (copied) handler kept receiving the tail while the command ran.
    QVERIFY(!liveTail.isEmpty());
}

// The file tools run in-process and must honor the same sandbox rules as
// the bash tool: no reading credential directories, no writes outside the
// project directory and the temporary locations.
void LlamaToolsTest::sandboxFileTools()
{
    // The file tools run in-process and their path checks are platform
    // independent, so this test runs on Windows as well.
    // The real home directory, captured before HOME is pointed at fakeHome
    // below (QDir::homePath() follows $HOME).
    const QString realHome = QDir::homePath();

    QTemporaryDir fakeHome;
    QVERIFY(fakeHome.isValid());
    const QString keyFile = fakeHome.path() + QStringLiteral("/.ssh/id_test");
    QDir(fakeHome.path()).mkpath(QStringLiteral(".ssh"));
    QFile file(keyFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("llama-secret-key");

    QTemporaryDir workdir;
    QVERIFY(workdir.isValid());

    // Captures (and later restores) HOME and the sandbox setting.
    const SandboxTestEnv env(/* restoreHome = */ true);
    qputenv("HOME", fakeHome.path().toUtf8());

    // read_file: credential files are not readable.
    QJsonObject readArgs;
    readArgs["file_path"] = keyFile;
    const auto readRes = runTool<Tools::ReadFileTool>(readArgs);
    QVERIFY(!readRes.second);
    QVERIFY(!readRes.first.contains("llama-secret-key"));

    // write / edit: outside the project directory is not writable.
    QJsonObject writeArgs;
    writeArgs["path"] = QStringLiteral("/usr/llama-sandbox-write-test");
    writeArgs["content"] = QStringLiteral("x");
    const auto writeRes = runTool<Tools::WriteTool>(writeArgs);
    QVERIFY(!writeRes.second);
    QVERIFY(!QFile::exists(QStringLiteral("/usr/llama-sandbox-write-test")));

    QJsonObject editEntry;
    editEntry["oldText"] = QStringLiteral("a");
    editEntry["newText"] = QStringLiteral("b");
    QJsonObject editArgs;
    editArgs["path"] = QStringLiteral("/etc/llama-sandbox-edit-test");
    editArgs["edits"] = QJsonArray{editEntry};
    const auto editRes = runTool<Tools::EditFileTool>(editArgs);
    QVERIFY(!editRes.second);

    // search / find: credential directories are not searchable.
    QJsonObject searchArgs;
    searchArgs["path"] = QString(fakeHome.path() + QStringLiteral("/.ssh"));
    searchArgs["pattern"] = QStringLiteral("llama");
    const auto searchRes = runTool<Tools::SearchTool>(searchArgs);
    QVERIFY(!searchRes.second);
    QVERIFY(!searchRes.first.contains("llama-secret-key"));

    QJsonObject findArgs;
    findArgs["path"] = QString(fakeHome.path() + QStringLiteral("/.ssh"));
    findArgs["pattern"] = QStringLiteral("id_*");
    const auto findRes = runTool<Tools::FindTool>(findArgs);
    QVERIFY(!findRes.second);
    QVERIFY(!findRes.first.contains("id_test"));

    // Writing inside a temporary location keeps working (the sandbox also
    // allows the project directory, but no project is open in this test).
    QJsonObject okWriteArgs;
    okWriteArgs["path"] = QString(workdir.path() + QStringLiteral("/allowed.txt"));
    okWriteArgs["content"] = QStringLiteral("fine");
    const auto okWriteRes = runTool<Tools::WriteTool>(okWriteArgs);
    QVERIFY(okWriteRes.second);

    // apply_patch: an in-place update outside the project directory and the
    // temporary locations is not allowed even though the file is readable
    // (the write check must not be skipped when source == target). The
    // fixture lives in the home directory: fakeHome is a temporary location
    // and therefore writable by design.
    const QString patchTarget =
        realHome + QStringLiteral("/llama-sandbox-patch-target.txt");
    {
        QFile patchFile(patchTarget);
        QVERIFY(patchFile.open(QIODevice::WriteOnly));
        patchFile.write("a\n");
    }
    QJsonObject patchArgs;
    patchArgs["patchText"] = QStringLiteral(
                                  "*** Begin Patch\n"
                                  "*** Update File: %1\n"
                                  "@@\n"
                                  "-a\n"
                                  "+b\n"
                                  "*** End Patch")
                                 .arg(patchTarget);
    const auto patchRes = runTool<LlamaCpp::ApplyPatchTool>(patchArgs);
    QVERIFY(!patchRes.second);
    {
        QFile checkFile(patchTarget);
        QVERIFY(checkFile.open(QIODevice::ReadOnly));
        QCOMPARE(checkFile.readAll(), QByteArray("a\n"));
    }
    QVERIFY(QFile::remove(patchTarget));

    // apply_patch: deleting a file outside the project directory and the
    // temporary locations is not allowed. A delete has no target, so the
    // source must be checked as a write, or any readable file could be
    // removed.
    const QString deleteTarget =
        realHome + QStringLiteral("/llama-sandbox-delete-target.txt");
    {
        QFile deleteFile(deleteTarget);
        QVERIFY(deleteFile.open(QIODevice::WriteOnly));
        deleteFile.write("a\n");
    }
    QJsonObject deleteArgs;
    deleteArgs["patchText"] = QStringLiteral(
                                  "*** Begin Patch\n"
                                  "*** Delete File: %1\n"
                                  "*** End Patch")
                                 .arg(deleteTarget);
    const auto deleteRes = runTool<LlamaCpp::ApplyPatchTool>(deleteArgs);
    QVERIFY(!deleteRes.second);
    QVERIFY(QFile::exists(deleteTarget));
    QVERIFY(QFile::remove(deleteTarget));

    // bash: a model-supplied workdir outside the project directory and the
    // temporary locations would redefine the sandbox boundary, so it is
    // refused (independent of the platform sandbox executable).
    QJsonObject workdirArgs;
    workdirArgs["command"] = QStringLiteral("echo sandbox-workdir-test");
    workdirArgs["workdir"] = QStringLiteral("/usr");
    const auto workdirRes = runBashTool(workdirArgs);
    QVERIFY(!workdirRes.second);
    QVERIFY(workdirRes.first.contains("not allowed"));
}

void LlamaToolsTest::projectSandboxOverride()
{
    QTemporaryDir workdir;
    QVERIFY(workdir.isValid());
    ProjectExplorer::Project *project =
        ProjectExplorer::ProjectManager::setStartupProject(workdir.path());

    LlamaProjectSettings projectSettings(project);

    // Restores the global setting when the scope is left, even when a
    // failing assertion returns early from the test.
    const SandboxTestEnv env;

    // With "use global settings" the global value applies.
    settings().sandboxCommands.setValue(true);
    QVERIFY(projectSettings.isSandboxEnabled());
    settings().sandboxCommands.setValue(false);
    QVERIFY(!projectSettings.isSandboxEnabled());

    // With a project override the global value is ignored.
    projectSettings.useGlobalSettings.setValue(false);
    QVERIFY(!projectSettings.isSandboxEnabled()); // project default is off
    projectSettings.sandboxCommands.setValue(true);
    QVERIFY(projectSettings.isSandboxEnabled());
    settings().sandboxCommands.setValue(true);
    QVERIFY(projectSettings.isSandboxEnabled());
    projectSettings.sandboxCommands.setValue(false);
    QVERIFY(!projectSettings.isSandboxEnabled());

    ProjectExplorer::ProjectManager::resetStartupProject();
}

// Writes a fake srt-win shell script that answers `user status` with
// \a statusJson and accepts `acl grant`/`acl revoke`, so the Windows
// wrapper construction can be exercised on any platform. Returns the
// script path (executable).
static QString writeFakeSrtWin(const QString &dir, const QString &statusJson)
{
    Q_ASSERT(!statusJson.contains(QLatin1Char('\'')));
    const QString path = dir + QStringLiteral("/srt-win");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return {};
    file.write((QStringLiteral("#!/bin/sh\n")
                    + QStringLiteral("if [ \"$1\" = user ]; then\n")
                    + QStringLiteral("  echo '")
                    + statusJson + QStringLiteral("'\n")
                    + QStringLiteral("elif [ \"$1\" = acl ]; then\n")
                    + QStringLiteral("  cat > /dev/null\n")
                    + QStringLiteral("fi\nexit 0\n"))
                       .toUtf8());
    file.close();
    QFile::setPermissions(path,
                          QFile::permissions(path)
                                  | QFileDevice::ExeOwner
                                  | QFileDevice::ExeGroup
                                  | QFileDevice::ExeOther);
    return path;
}

// The successful provisioning probe is cached per srt-win executable for
// the session; this test uses its own fake (a fresh QTemporaryDir path),
// so the not-provisioned path is reachable regardless of test order.
void LlamaToolsTest::windowsSandboxSpecNotProvisioned()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fake = writeFakeSrtWin(
        dir.path(),
        QStringLiteral("{\"user\":{\"exists\":false},\"cred_present\":false}"));
    qputenv("LLAMA_SRT_WIN", fake.toUtf8());

    const LlamaCpp::Tools::WindowsSandboxSpec spec =
        LlamaCpp::Tools::windowsSandboxSpec(QDir::tempPath(), QProcessEnvironment());

    qunsetenv("LLAMA_SRT_WIN");

    QVERIFY(spec.program.isEmpty());
    QVERIFY(!spec.error.isEmpty());
    QVERIFY(spec.error.contains("not provisioned"));
    // The error is actionable: it points at the one-time install.
    QVERIFY(spec.error.contains("windows-install"));
}

void LlamaToolsTest::windowsSandboxSpecUnavailable()
{
    if (!QStandardPaths::findExecutable(QStringLiteral("srt-win")).isEmpty())
        QSKIP("srt-win is on the PATH");
    qputenv("LLAMA_SRT_WIN", "/nonexistent/srt-win");

    const LlamaCpp::Tools::WindowsSandboxSpec spec =
        LlamaCpp::Tools::windowsSandboxSpec(QDir::tempPath(), QProcessEnvironment());

    qunsetenv("LLAMA_SRT_WIN");

    QVERIFY(spec.program.isEmpty());
    QVERIFY(!spec.error.isEmpty());
    QVERIFY(spec.error.contains(QStringLiteral("srt-win")));
    QVERIFY(spec.error.contains("windows-install"));
}

void LlamaToolsTest::windowsSandboxSpec()
{
    QTemporaryDir home;
    QTemporaryDir workdir;
    QVERIFY(home.isValid());
    QVERIFY(workdir.isValid());
    const QString fake = writeFakeSrtWin(
        home.path(),
        QStringLiteral("{\"user\":{\"exists\":true,\"sid\":\"S-1-5-21-1-10-1\"},"
                       "\"cred_present\":true}"));

    // secretReadPaths() follows $HOME.
    const QString realHome = qEnvironmentVariable("HOME");
    const bool hadHome = !realHome.isEmpty();
    qputenv("HOME", home.path().toUtf8());
    qputenv("LLAMA_SRT_WIN", fake.toUtf8());

    QProcessEnvironment env;
    env.insert("PATH", "/usr/bin");
    env.insert("FOO", "bar baz");

    const LlamaCpp::Tools::WindowsSandboxSpec spec =
        LlamaCpp::Tools::windowsSandboxSpec(workdir.path(), env);

    // An oversized environment exceeds the CreateProcessW command line
    // limit and is refused with an actionable error. (Kept before the
    // LLAMA_SRT_WIN/HOME restore below so this call uses the fake too.)
    QProcessEnvironment bigEnv = env;
    bigEnv.insert("BIG", QString(40000, QLatin1Char('x')));
    const LlamaCpp::Tools::WindowsSandboxSpec bigSpec =
        LlamaCpp::Tools::windowsSandboxSpec(workdir.path(), bigEnv);

    qunsetenv("LLAMA_SRT_WIN");
    if (hadHome)
        qputenv("HOME", realHome.toUtf8());
    else
        qunsetenv("HOME");

    QVERIFY2(spec.error.isEmpty(), qPrintable(spec.error));
    QCOMPARE(spec.program, fake);
    // The `exec --quiet` wrapper; the shell and the command follow the `--`.
    QCOMPARE((spec.arguments.mid(0, 2)),
             (QStringList{ QStringLiteral("exec"), QStringLiteral("--quiet") }));
    QCOMPARE(spec.arguments.last(), QStringLiteral("--"));
    // The credential locations are denied for read and write. Directory
    // targets carry a trailing backslash (srt-win materializes a missing
    // deny target as an empty directory then); the .netrc file does not.
    const int denyRead = spec.arguments.indexOf(QStringLiteral("--deny-read"));
    QVERIFY(denyRead != -1);
    const QString ssh = spec.arguments.at(denyRead + 1);
    QVERIFY(ssh.startsWith(home.path() + QStringLiteral("/.ssh")));
    QVERIFY(ssh.endsWith(QLatin1Char('\\')));
    const QString netrc = home.path() + QStringLiteral("/.netrc");
    const int netrcIndex = spec.arguments.indexOf(netrc);
    QVERIFY(netrcIndex > 0);
    QCOMPARE(spec.arguments.at(netrcIndex - 1), QStringLiteral("--deny-read"));
    const int netrcDenyWrite = spec.arguments.indexOf(QStringLiteral("--deny-write"),
                                                      netrcIndex);
    QVERIFY(netrcDenyWrite != -1);
    QCOMPARE(spec.arguments.at(netrcDenyWrite + 1), netrc);
    // The caller's environment is passed as the --env overlay (the
    // sandboxed child starts with the sandbox user's profile env only).
    QVERIFY(spec.arguments.contains(QStringLiteral("PATH=/usr/bin")));
    QVERIFY(spec.arguments.contains(QStringLiteral("FOO=bar baz")));

    QVERIFY(!bigSpec.error.isEmpty());
    QVERIFY(bigSpec.error.contains("too long"));
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
    QCOMPARE(tool.oneLineSummary(args), QString("running `` echo \"`date`\" ``"));

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
    // The command and its output appear in a single terminal‑style block:
    // the command with a "$ " prompt, the output directly below it.
    QVERIFY(md.contains("```terminal\n$ git status\nOn branch main\n```"));
    QVERIFY(md.contains("/some/dir"));

    QJsonObject noWorkdir;
    noWorkdir["command"] = QStringLiteral("git status");
    QVERIFY(!tool.detailsMarkdown(noWorkdir, QStringLiteral("out"), true)
                 .contains("Working directory"));

    // A failed run has no error header: the output carries the error and
    // the ✗ icon in the summary marks the failure.
    QVERIFY(!tool.detailsMarkdown(args, QStringLiteral("Command exited with code 1."), false)
                 .contains(QStringLiteral("**Error**")));
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
    QCOMPARE(tool.oneLineSummary(args), QString("search for `` a`b ``"));

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
    QVERIFY(md.contains("Path: `/some/dir`"));
    QVERIFY(md.contains("x.json"));
}

// ============================================================================
// LsTool
// ============================================================================

void LlamaToolsTest::ls_toolDefinition()
{
    Tools::LsTool tool;
    QCOMPARE(tool.name(), QString("ls"));

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(tool.toolDefinition().toUtf8(), &err);
    QCOMPARE(err.error, QJsonParseError::NoError);
    QVERIFY(doc.isObject());

    const QJsonObject function = doc.object()["function"].toObject();
    QCOMPARE(function["name"].toString(), QString("ls"));
    QVERIFY(!function["description"].toString().isEmpty());

    const QJsonObject parameters = function["parameters"].toObject();
    QCOMPARE(parameters["type"].toString(), QString("object"));
    QVERIFY(parameters["strict"].toBool());
    const QJsonObject properties = parameters["properties"].toObject();
    for (const QString &name : {QStringLiteral("path"), QStringLiteral("limit")})
        QVERIFY2(properties.contains(name), qPrintable(name));
    // Nothing is required: a bare ls lists the project directory.
    QCOMPARE(parameters["required"].toArray().size(), 0);
}

void LlamaToolsTest::ls_run()
{
    const QString root = gTempDir->path() + QStringLiteral("/ls_run");
    QDir().mkpath(root + QStringLiteral("/sub"));
    writeTextFile(root + QStringLiteral("/a.cpp"), "a\n");
    writeTextFile(root + QStringLiteral("/Zebra.txt"), "z\n");
    writeTextFile(root + QStringLiteral("/.hidden"), "h\n");

    Tools::LsTool tool;
    QJsonObject args;
    args["path"] = root;

    const auto [output, ok] = runTool<Tools::LsTool>(args);
    QVERIFY2(ok, qPrintable(output));
    // Case-insensitive alphabetical order, dotfiles included, directories
    // carry a '/' suffix.
    QCOMPARE(output, QStringLiteral(".hidden\na.cpp\nsub/\nZebra.txt"));
}

void LlamaToolsTest::ls_emptyDir()
{
    const QString root = gTempDir->path() + QStringLiteral("/ls_empty");
    QDir().mkpath(root);

    Tools::LsTool tool;
    QJsonObject args;
    args["path"] = root;

    const auto [output, ok] = runTool<Tools::LsTool>(args);
    QVERIFY(ok);
    QCOMPARE(output, QString("Directory is empty."));
}

void LlamaToolsTest::ls_limit()
{
    const QString root = gTempDir->path() + QStringLiteral("/ls_limit");
    QDir().mkpath(root);
    for (int i = 1; i <= 10; ++i)
        writeTextFile(root + QStringLiteral("/file_%1.txt").arg(i), QStringLiteral("x\n"));

    Tools::LsTool tool;
    QJsonObject args;
    args["path"] = root;
    args["limit"] = 4;

    const auto [output, ok] = runTool<Tools::LsTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(output.count("file_"), 4);
    QVERIFY(output.contains("4 entries limit reached. Use limit=8"));
}

void LlamaToolsTest::ls_badPath()
{
    Tools::LsTool tool;
    QJsonObject args;
    args["path"] = QStringLiteral("/nonexistent/llama-ls-dir");

    const auto [output, ok] = runTool<Tools::LsTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains("not a directory"));
}

void LlamaToolsTest::ls_summaries()
{
    Tools::LsTool tool;
    QCOMPARE(tool.name(), QString("ls"));

    QJsonObject args;
    args["path"] = QStringLiteral("src");
    QCOMPARE(tool.oneLineSummary(args), QString("list directory `src`"));
    QCOMPARE(tool.oneLineSummary(QJsonObject()), QString("list directory `.`"));

    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"src\"}")),
             QString("list directory `src`"));
    // While the path is not (fully) visible yet, fall back to the default.
    QCOMPARE(tool.streamingSummary(QStringLiteral("{\"path\": \"src")),
             QString("list directory `.`"));
    QCOMPARE(tool.streamingSummary(QStringLiteral("{}")), QString("list directory `.`"));

    QJsonObject mdArgs;
    mdArgs["path"] = QStringLiteral("/some/dir");
    const QString md = tool.detailsMarkdown(mdArgs, QStringLiteral("a.txt\nsub/"), true);
    QVERIFY(md.contains("Path: `/some/dir`"));
    QVERIFY(md.contains("a.txt"));
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
// McpServerConfig (configured MCP servers, settings JSON)
// ============================================================================

void LlamaToolsTest::mcpserverconfig_jsonRoundTrip()
{
    Tools::McpServerConfig a;
    a.name = QStringLiteral("Alpha");
    a.url = QUrl(QStringLiteral("http://127.0.0.1:1234/mcp"));
    a.headers = {QStringLiteral("Authorization: Bearer token123")};

    Tools::McpServerConfig b;
    b.name = QStringLiteral("Beta");
    b.url = QUrl(QStringLiteral("https://mcp.example.com/sse"));

    const QVector<Tools::McpServerConfig> servers = {a, b};
    const QVector<Tools::McpServerConfig> restored =
            Tools::McpServerConfig::fromJson(Tools::McpServerConfig::toJson(servers));

    QCOMPARE(restored.size(), 2);
    QCOMPARE(restored[0].name, a.name);
    QCOMPARE(restored[0].url, a.url);
    QCOMPARE(restored[0].headers, a.headers);
    QCOMPARE(restored[1].name, b.name);
    QCOMPARE(restored[1].url, b.url);
    QVERIFY(restored[1].headers.isEmpty());
}

void LlamaToolsTest::mcpserverconfig_invalidEntriesSkipped()
{
    // Empty and malformed input yields an empty list.
    QVERIFY(Tools::McpServerConfig::fromJson(QString()).isEmpty());
    QVERIFY(Tools::McpServerConfig::fromJson(QStringLiteral("{not json")).isEmpty());

    // Invalid entries (empty name, non-http(s) URL) are dropped, valid
    // ones kept; blank headers are dropped too.
    const QString json = QStringLiteral(
        "[{\"name\": \"\", \"url\": \"http://ok.example/mcp\"},"
        "{\"name\": \"Bad\", \"url\": \"ftp://nope.example/mcp\"},"
        "{\"name\": \"Fine\", \"url\": \"http://fine.example/mcp\","
        "\"headers\": [\"Authorization: Bearer x\", \"   \"]},"
        "{\"name\": \"Fine\", \"url\": \"http://duplicate.example/mcp\"}]");
    const QVector<Tools::McpServerConfig> servers = Tools::McpServerConfig::fromJson(json);
    QCOMPARE(servers.size(), 1);
    QCOMPARE(servers.first().name, QString("Fine"));
    // The first entry with a name wins over a later duplicate.
    QCOMPARE(servers.first().url, QUrl(QStringLiteral("http://fine.example/mcp")));
    QCOMPARE(servers.first().headers, QStringList{QStringLiteral("Authorization: Bearer x")});
}

// McpTool / remote tool provider (MCP servers)
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

    // A failed call has no error header; the result still carries the error
    QVERIFY(tool.detailsMarkdown(args, QStringLiteral("MCP tool failed: boom"), false)
                .contains(QStringLiteral("MCP tool failed: boom")));
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

void LlamaToolsTest::messageToMarkdown_userAssistant()
{
    Message user;
    user.role = "user";
    user.content = "What is 2+2?";

    Message assistant;
    assistant.role = "assistant";
    assistant.content = "It is 4.";

    QCOMPARE(ChatManager::messageToMarkdown(user),
             QStringLiteral("### User\n\nWhat is 2+2?\n\n"));
    QCOMPARE(ChatManager::messageToMarkdown(assistant),
             QStringLiteral("### Assistant\n\nIt is 4.\n\n"));
}

void LlamaToolsTest::messageToMarkdown_thinkingSection()
{
    Message assistant;
    assistant.role = "assistant";
    assistant.content = ThinkingSectionParser::startToken() + "\nlet me think\n"
                        + ThinkingSectionParser::endToken() + "\n4";

    const QString md = ChatManager::messageToMarkdown(assistant);
    QVERIFY2(md.contains(QStringLiteral("<details><summary>Thought</summary>\n")),
             qPrintable(md));
    QVERIFY(md.contains(QStringLiteral("let me think")));
    QVERIFY(md.contains(QStringLiteral("</details>")));
    QVERIFY(md.contains(QStringLiteral("4")));
}

void LlamaToolsTest::messageToMarkdown_toolCall()
{
    Message toolMsg;
    toolMsg.role = "tool";

    QJsonObject fn;
    fn[QStringLiteral("name")] = QStringLiteral("bash");
    fn[QStringLiteral("arguments")]
        = QStringLiteral("{\"command\":\"ls -la\"}");
    QJsonObject call;
    call[QStringLiteral("function")] = fn;
    QJsonArray calls;
    calls.append(call);

    QVariantMap callExtra;
    callExtra[QStringLiteral("tool_calls")] = calls;
    toolMsg.extra.append(callExtra);

    QJsonObject result;
    result[QStringLiteral("content")] = QStringLiteral("total 0\n");
    QVariantMap resultExtra;
    resultExtra[QStringLiteral("tool_result")] = result;
    resultExtra[QStringLiteral("tool_status")] = QStringLiteral("success");
    toolMsg.extra.append(resultExtra);

    const QString md = ChatManager::messageToMarkdown(toolMsg);
    QVERIFY2(md.startsWith(QStringLiteral("<details>\n<summary>")), qPrintable(md));
    // one-line summary of the bash tool, plus the status
    QVERIFY(md.contains(QStringLiteral("`ls -la`")));
    QVERIFY(md.contains(QStringLiteral("(success)")));
    QVERIFY(md.contains(QStringLiteral("**Arguments**")));
    QVERIFY(md.contains(QStringLiteral("\"command\"")));
    QVERIFY(md.contains(QStringLiteral("**Result**")));
    QVERIFY(md.contains(QStringLiteral("total 0")));
    QVERIFY(md.endsWith(QStringLiteral("\n</details>\n\n")));
}

void LlamaToolsTest::messageToMarkdown_toolOnlyAssistant()
{
    // An assistant message that only carries the tool call has no content of
    // its own – it renders through the tool bubble and exports as nothing.
    Message assistant;
    assistant.role = "assistant";
    assistant.content = QString();

    QJsonObject fn;
    fn[QStringLiteral("name")] = QStringLiteral("bash");
    fn[QStringLiteral("arguments")] = QStringLiteral("{\"command\":\"ls\"}");
    QJsonObject call;
    call[QStringLiteral("function")] = fn;
    QJsonArray calls;
    calls.append(call);

    QVariantMap callExtra;
    callExtra[QStringLiteral("tool_calls")] = calls;
    assistant.extra.append(callExtra);

    QVERIFY(ChatManager::messageToMarkdown(assistant).isEmpty());

    // …while an empty assistant message without tool calls keeps its header
    // (previous export behaviour).
    Message emptyAssistant;
    emptyAssistant.role = "assistant";
    QVERIFY(ChatManager::messageToMarkdown(emptyAssistant)
                .startsWith(QStringLiteral("### Assistant")));
}

void LlamaToolsTest::messageToMarkdown_embedsCachedMermaidSvg()
{
    // A ```mermaid block with a persisted render (a "diagram" extra entry
    // keyed by the source) is exported as a <details> section carrying the
    // rendered SVG and the source; blocks without a cached SVG are kept
    // verbatim.
    const QString source = QStringLiteral("flowchart LR\n  A[Start] --> B[End]");
    Message assistant;
    assistant.role = "assistant";
    assistant.content
        = QStringLiteral("Here you go:\n\n```mermaid\n%1\n```\n\nAnd an uncached one:\n\n")
              .arg(source)
          + QStringLiteral("```mermaid\ngraph TD\n  X --> Y\n```");

    QVariantMap entry;
    entry[QStringLiteral("type")] = QStringLiteral("diagram");
    entry[QStringLiteral("key")]
        = MarkdownRenderer::mermaidDiagramKey(source);
    entry[QStringLiteral("svg")]
        = QString::fromLatin1(
            QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\">cached</svg>").toBase64());
    assistant.extra.append(entry);

    const QString md = ChatManager::messageToMarkdown(assistant);
    // The cached block is replaced by the picture + source section...
    QVERIFY2(md.contains(QStringLiteral("<details>\n<summary>Mermaid diagram</summary>")),
             qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("<svg xmlns=\"http://www.w3.org/2000/svg\">cached</svg>")),
             qPrintable(md));
    // ...with the source kept in a fence below the picture...
    QVERIFY2(md.contains(QStringLiteral("```mermaid\n") + source + QStringLiteral("\n```")),
             qPrintable(md));
    // ...and the uncached block is untouched.
    QVERIFY2(md.contains(QStringLiteral("```mermaid\ngraph TD\n  X --> Y\n```")),
             qPrintable(md));
}

void LlamaToolsTest::messageToMarkdown_embedsCachedMathSvg()
{
    // $...$ and $$...$$ spans with persisted renders are exported as inline
    // SVGs; unspanned dollar signs (prices) and uncached spans stay text.
    Message assistant;
    assistant.role = "assistant";
    assistant.content
        = QStringLiteral("Inline $e^2$ and display\n\n$$\n\\frac{a}{b}\n$$\n\n")
          + QStringLiteral("Costs $5 and $10, and uncached $x^2$.");

    auto addEntry = [&assistant](const QString &key, const QString &svg) {
        QVariantMap entry;
        entry[QStringLiteral("type")] = QStringLiteral("diagram");
        entry[QStringLiteral("key")] = key;
        entry[QStringLiteral("svg")]
            = QString::fromLatin1(svg.toUtf8().toBase64());
        assistant.extra.append(entry);
    };
    addEntry(MarkdownRenderer::katexDiagramKey(QStringLiteral("e^2"), false),
             QStringLiteral("<svg>inline</svg>"));
    addEntry(MarkdownRenderer::katexDiagramKey(QStringLiteral("\\frac{a}{b}"), true),
             QStringLiteral("<svg>display</svg>"));

    const QString md = ChatManager::messageToMarkdown(assistant);
    QVERIFY2(md.contains(QStringLiteral("Inline <svg>inline</svg> and display")),
             qPrintable(md));
    QVERIFY2(md.contains(QStringLiteral("<svg>display</svg>")), qPrintable(md));
    // The prices are not a math span, and the uncached formula is kept.
    QVERIFY2(md.contains(QStringLiteral("Costs $5 and $10, and uncached $x^2$.")),
             qPrintable(md));
}

void LlamaToolsTest::messageToMarkdown_leavesUncachedDiagramsAlone()
{
    // No "diagram" entries in the extra: the export is byte-identical to the
    // plain rendering (diagrams are left as source).
    Message assistant;
    assistant.role = "assistant";
    assistant.content = QStringLiteral("```mermaid\nA --> B\n```\n\nand $x$.");

    const QString md = ChatManager::messageToMarkdown(assistant);
    QCOMPARE(md, QStringLiteral("### Assistant\n\n```mermaid\nA --> B\n```\n\nand $x$.\n\n"));
}

void LlamaToolsTest::htmlExport_conversationDocument()
{
    Message user;
    user.role = "user";
    user.content = "Hello";

    Message assistant;
    assistant.role = "assistant";
    assistant.content = "Hi!";

    const QString html = HtmlExporter::conversationHtml("My Chat", {user, assistant});
    QVERIFY2(html.startsWith("<!DOCTYPE html>"), qPrintable(html.left(100)));
    QVERIFY(html.contains(QStringLiteral("<title>My Chat</title>")));
    QVERIFY(html.contains(QStringLiteral("<style>")));
    // Theme colours were resolved to hex values, no token names left behind.
    QVERIFY(html.contains(QStringLiteral("background: #")));
    QVERIFY(!html.contains(QStringLiteral("Token_")));
    QVERIFY(html.contains(QStringLiteral("<div class=\"msg user\">")));
    QVERIFY(html.contains(QStringLiteral("<div class=\"msg assistant\">")));
    QVERIFY(html.contains(QStringLiteral("<p>Hello</p>")));
    QVERIFY(html.contains(QStringLiteral("<p>Hi!</p>")));
    // Tables get the chat renderer's alternating row shading.
    QVERIFY(html.contains(QStringLiteral("tbody tr:nth-child(odd)")));
}

void LlamaToolsTest::htmlExport_userAssistant()
{
    Message user;
    user.role = "user";
    user.content = QStringLiteral("See [the docs](https://example.com) and note 1 < 2.");

    Message assistant;
    assistant.role = "assistant";
    assistant.content = QStringLiteral("**bold** and `inline code`\n\n- one\n- two");

    const QString userHtml = HtmlExporter::messageToHtml(user);
    QVERIFY2(userHtml.contains(QStringLiteral("<div class=\"msg user\">")), qPrintable(userHtml));
    QVERIFY2(userHtml.contains(QStringLiteral("<a href=\"https://example.com\">the docs</a>")),
             qPrintable(userHtml));
    // The literal < in the text is escaped, not a real element.
    QVERIFY(userHtml.contains(QStringLiteral("1 &lt; 2")));

    const QString assistantHtml = HtmlExporter::messageToHtml(assistant);
    QVERIFY2(assistantHtml.contains(QStringLiteral("<strong>bold</strong>")),
             qPrintable(assistantHtml));
    QVERIFY2(assistantHtml.contains(QStringLiteral("<code>inline code</code>")),
             qPrintable(assistantHtml));
    QVERIFY2(assistantHtml.contains(QStringLiteral("<li>one</li>")), qPrintable(assistantHtml));
}

void LlamaToolsTest::htmlExport_codeBlockHighlighted()
{
    Message assistant;
    assistant.role = "assistant";
    assistant.content = QStringLiteral("```cpp\nint main() { return 0; }\n```");

    const QString html = HtmlExporter::messageToHtml(assistant);
    QVERIFY2(html.contains(QStringLiteral("<pre><code>")), qPrintable(html));
    // The C++ definition produced coloured runs (the keyword "int" at least).
    QVERIFY2(html.contains(QStringLiteral("<span style=\"color:#")), qPrintable(html));
    // The runs may be split across spans, so check the pieces.
    QVERIFY2(html.contains(QStringLiteral("int")), qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("return")), qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("0;")), qPrintable(html));

    // An untagged block is plain escaped text, no spans.
    Message plain;
    plain.role = "assistant";
    plain.content = QStringLiteral("```\na < b && c > d\n```");
    const QString plainHtml = HtmlExporter::messageToHtml(plain);
    QVERIFY2(plainHtml.contains(QStringLiteral("a &lt; b &amp;&amp; c &gt; d")),
             qPrintable(plainHtml));
    QVERIFY(!plainHtml.contains(QStringLiteral("<span")));
}

void LlamaToolsTest::htmlExport_toolCall()
{
    Message toolMsg;
    toolMsg.role = "tool";

    QJsonObject fn;
    fn[QStringLiteral("name")] = QStringLiteral("bash");
    fn[QStringLiteral("arguments")] = QStringLiteral("{\"command\":\"ls -la\"}");
    QJsonObject call;
    call[QStringLiteral("function")] = fn;
    QJsonArray calls;
    calls.append(call);

    QVariantMap callExtra;
    callExtra[QStringLiteral("tool_calls")] = calls;
    toolMsg.extra.append(callExtra);

    QJsonObject result;
    result[QStringLiteral("content")] = QStringLiteral("total 0\n");
    QVariantMap resultExtra;
    resultExtra[QStringLiteral("tool_result")] = result;
    resultExtra[QStringLiteral("tool_status")] = QStringLiteral("success");
    toolMsg.extra.append(resultExtra);

    const QString html = HtmlExporter::messageToHtml(toolMsg);
    QVERIFY2(html.contains(QStringLiteral("<div class=\"msg tool\">")), qPrintable(html));
    // one-line summary of the bash tool (backticks as <code>), plus the status
    QVERIFY2(html.contains(QStringLiteral("<code>ls -la</code>")), qPrintable(html));
    QVERIFY(html.contains(QStringLiteral("(success)")));
    QVERIFY(html.contains(QStringLiteral("<strong>Arguments</strong>")));
    // JSON quotes are HTML-escaped inside the code block.
    QVERIFY2(html.contains(QStringLiteral("&quot;command&quot;")), qPrintable(html));
    QVERIFY(html.contains(QStringLiteral("<strong>Result</strong>")));
    QVERIFY(html.contains(QStringLiteral("total 0")));
    QVERIFY(html.contains(QStringLiteral("</details>")));

    // A tool message without a call exports as nothing, and a tool‑call‑only
    // assistant message renders through the tool bubble.
    Message emptyTool;
    emptyTool.role = "tool";
    QVERIFY(HtmlExporter::messageToHtml(emptyTool).isEmpty());

    Message assistant;
    assistant.role = "assistant";
    assistant.content = QString();
    QVariantMap assistantExtra;
    assistantExtra[QStringLiteral("tool_calls")] = calls;
    assistant.extra.append(assistantExtra);
    QVERIFY(HtmlExporter::messageToHtml(assistant).isEmpty());
}

void LlamaToolsTest::htmlExport_embedsCachedDiagrams()
{
    const QString source = QStringLiteral("flowchart LR\n  A[Start] --> B[End]");
    Message assistant;
    assistant.role = "assistant";
    assistant.content
        = QStringLiteral("```mermaid\n%1\n```\n\nInline $e^2$ here.").arg(source);

    const QByteArray svg = QByteArray("<svg xmlns=\"http://www.w3.org/2000/svg\">cached</svg>");
    const QString b64 = QString::fromLatin1(svg.toBase64());

    QVariantMap entry;
    entry[QStringLiteral("type")] = QStringLiteral("diagram");
    entry[QStringLiteral("key")] = MarkdownRenderer::mermaidDiagramKey(source);
    entry[QStringLiteral("svg")] = b64;
    assistant.extra.append(entry);

    QVariantMap mathEntry;
    mathEntry[QStringLiteral("type")] = QStringLiteral("diagram");
    mathEntry[QStringLiteral("key")] = MarkdownRenderer::katexDiagramKey(QStringLiteral("e^2"), false);
    mathEntry[QStringLiteral("svg")] = b64;
    assistant.extra.append(mathEntry);

    const QString html = HtmlExporter::messageToHtml(assistant);
    // The cached mermaid block becomes a section with a base64 image and the
    // source below it...
    QVERIFY2(html.contains(QStringLiteral("<summary>Mermaid diagram</summary>")),
             qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("data:image/svg+xml;base64,") + b64),
             qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("A[Start]")), qPrintable(html));
    QVERIFY2(html.contains(QStringLiteral("--&gt; B[End]")), qPrintable(html));
    // ...and the cached math span an inline image, aligned like the chat
    // view (re-boxed SVGs pin to the line bottom, display math is centred).
    QVERIFY2(html.contains(QStringLiteral("<img class=\"math\" src=\"data:image/svg+xml;base64,")),
             qPrintable(html));

    // Display math gets the centred block variant. Mixed with inline math in
    // the same message: the display pass must not leave a $...$ that the
    // inline pass matches inside the generated markup.
    QVariantMap displayEntry;
    displayEntry[QStringLiteral("type")] = QStringLiteral("diagram");
    displayEntry[QStringLiteral("key")]
        = MarkdownRenderer::katexDiagramKey(QStringLiteral("e^2"), true);
    displayEntry[QStringLiteral("svg")] = b64;
    assistant.extra.append(displayEntry);
    Message displayMsg = assistant;
    displayMsg.content = QStringLiteral("And $$e^2$$ overall, like $e^2$ inline.");
    const QString displayHtml = HtmlExporter::messageToHtml(displayMsg);
    QVERIFY2(displayHtml.contains(QStringLiteral("<img class=\"math display\"")),
             qPrintable(displayHtml));
    QVERIFY2(displayHtml.contains(QStringLiteral("<img class=\"math\" src=")),
             qPrintable(displayHtml));
    // No escaped img tags (that is what a corrupted attribute looks like).
    QVERIFY(!displayHtml.contains(QLatin1String("&lt;img")));

    // User messages carry their diagram SVGs too.
    Message user;
    user.role = "user";
    user.content = QStringLiteral("Look: $e^2$");
    user.extra.append(mathEntry);
    const QString userHtml = HtmlExporter::messageToHtml(user);
    QVERIFY2(userHtml.contains(QStringLiteral("<img class=\"math\" src=\"data:image/svg+xml;base64,")),
             qPrintable(userHtml));
}

void LlamaToolsTest::htmlExport_thinkingSection()
{
    Message assistant;
    assistant.role = "assistant";
    assistant.content
        = ThinkingSectionParser::startToken() + QStringLiteral("let me think")
        + ThinkingSectionParser::endToken() + QStringLiteral("The answer is 42.");

    const QString html = HtmlExporter::messageToHtml(assistant);
    // The thinking section becomes a collapsible <details> section whose
    // summary is not wrapped in a <p> (not valid phrasing content).
    QVERIFY2(html.contains(QStringLiteral("<summary>Thought</summary>")), qPrintable(html));
    QVERIFY(html.contains(QStringLiteral("let me think")));
    QVERIFY(html.contains(QStringLiteral("The answer is 42.")));
    QVERIFY(!html.contains(QStringLiteral("<summary><p>")));
}

void LlamaToolsTest::projectInstructions_candidatePriority()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const Utils::FilePath root = Utils::FilePath::fromString(dir.path());
    writeTextFile(root.pathAppended("AGENTS.md").path(), "agents");
    writeTextFile(root.pathAppended("CLAUDE.md").path(), "claude");

    // AGENTS.md wins over CLAUDE.md in the same directory...
    QCOMPARE(findProjectInstructionsFile(root).fileName(), QString("AGENTS.md"));
    // ...and CLAUDE.md is the fallback once AGENTS.md is gone.
    QFile agentsFile(root.pathAppended("AGENTS.md").path());
    QVERIFY(agentsFile.remove());
    QCOMPARE(findProjectInstructionsFile(root).fileName(), QString("CLAUDE.md"));
    // No candidate at all: empty path.
    QFile claudeFile(root.pathAppended("CLAUDE.md").path());
    QVERIFY(claudeFile.remove());
    QVERIFY(findProjectInstructionsFile(root).isEmpty());
}

void LlamaToolsTest::projectInstructions_walksToGitRoot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const Utils::FilePath root = Utils::FilePath::fromString(dir.path());
    // The project is a subdirectory of the repo; the instructions live at
    // the repo root.
    QVERIFY(QDir(root.path()).mkdir(QStringLiteral(".git")));
    writeTextFile(root.pathAppended("AGENTS.md").path(), "root instructions");
    const Utils::FilePath project = root.pathAppended("sub").pathAppended("proj");
    QVERIFY(QDir().mkpath(project.path()));

    QCOMPARE(findProjectInstructionsFile(project), root.pathAppended("AGENTS.md"));
}

void LlamaToolsTest::projectInstructions_stopsAtGitRoot()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const Utils::FilePath outer = Utils::FilePath::fromString(dir.path());
    // An instructions file above the git root must not be picked up (in a
    // real check-out that would be e.g. $HOME/AGENTS.md).
    writeTextFile(outer.pathAppended("AGENTS.md").path(), "outer");
    const Utils::FilePath repo = outer.pathAppended("repo");
    QVERIFY(QDir().mkpath(repo.path()));
    QVERIFY(QDir(repo.path()).mkdir(QStringLiteral(".git")));
    const Utils::FilePath project = repo.pathAppended("sub");
    QVERIFY(QDir().mkpath(project.path()));

    QVERIFY(findProjectInstructionsFile(project).isEmpty());
}

void LlamaToolsTest::projectInstructions_noGitOnlyProjectDir()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const Utils::FilePath outer = Utils::FilePath::fromString(dir.path());
    writeTextFile(outer.pathAppended("AGENTS.md").path(), "outer");
    const Utils::FilePath project = outer.pathAppended("proj");
    QVERIFY(QDir().mkpath(project.path()));

    // Without a git root only the project directory itself is searched.
    QVERIFY(findProjectInstructionsFile(project).isEmpty());
    writeTextFile(project.pathAppended("CLAUDE.md").path(), "inner");
    QCOMPARE(findProjectInstructionsFile(project), project.pathAppended("CLAUDE.md"));
}

void LlamaToolsTest::projectInstructions_content()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const Utils::FilePath root = Utils::FilePath::fromString(dir.path());
    // BOM must be stripped from the loaded content.
    writeTextFile(root.pathAppended("AGENTS.md").path(),
                  QString(QChar(0xFEFF)) + QStringLiteral("Use tabs.\n"));

    const QString text = loadProjectInstructions(root);
    QVERIFY2(text.startsWith(QStringLiteral("Project instructions from ")),
             qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("AGENTS.md")), qPrintable(text));
    QVERIFY2(text.endsWith(QStringLiteral("Use tabs.\n")), qPrintable(text));
    QVERIFY(!text.contains(QChar(0xFEFF)));

    // An empty (or whitespace-only) file yields no instructions at all.
    writeTextFile(root.pathAppended("AGENTS.md").path(), QStringLiteral("  \n"));
    QVERIFY(loadProjectInstructions(root).isEmpty());
}

void LlamaToolsTest::projectInstructions_truncation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const Utils::FilePath root = Utils::FilePath::fromString(dir.path());
    // Multi‑byte content: the 32 KB cut must land on a character boundary.
    const QString content = QStringLiteral("é").repeated(20000); // 40 KB
    writeTextFile(root.pathAppended("AGENTS.md").path(), content);

    const QString text = loadProjectInstructions(root);
    QVERIFY2(text.contains(QStringLiteral("[... truncated ...]")), qPrintable(text));
    // The truncated body (after the header line) is valid UTF‑8 (fromUtf8
    // would insert U+FFFD for a split sequence).
    QVERIFY(!text.contains(QChar(0xFFFD)));
    QByteArray body = text.mid(text.indexOf(QLatin1Char('\n')) + 1).toUtf8();
    const QByteArray marker = QStringLiteral("\n\n[... truncated ...]").toUtf8();
    QVERIFY(body.endsWith(marker));
    body.chop(marker.size());
    // The cut lands exactly on a character boundary (32768 bytes = 16384 ×
    // é): nothing beyond the cap is dropped, nothing of it kept.
    QCOMPARE(body.size(), 32 * 1024);
    const QByteArray expected = QByteArray::fromHex(QByteArray("c3a9")).repeated(16384);
    QCOMPARE(body, expected);

    // A cut on an ASCII boundary must not eat the last byte either.
    const QString ascii = QStringLiteral("a").repeated(40000);
    writeTextFile(root.pathAppended("AGENTS.md").path(), ascii);
    const QString asciiText = loadProjectInstructions(root);
    QVERIFY(asciiText.contains(QStringLiteral("[... truncated ...]")));
    body = asciiText.mid(asciiText.indexOf(QLatin1Char('\n')) + 1).toUtf8();
    QVERIFY(body.endsWith(marker));
    body.chop(marker.size());
    QCOMPARE(body.size(), 32 * 1024);
}

// Restores the global skill settings (and clears the scan cache) when the
// scope is left, even when a failing assertion returns early from the test.
class SkillsTestEnv
{
public:
    SkillsTestEnv()
        : m_dirs(settings().skillsDirectories()),
          m_disabled(settings().disabledSkillsList())
    {
    }

    ~SkillsTestEnv()
    {
        settings().skillsDirectories.setValue(m_dirs);
        settings().disabledSkillsList.setValue(m_disabled);
        Skills::clearCache();
    }

private:
    QStringList m_dirs;
    QStringList m_disabled;
};

// (no assertions in here: QtTest macros expand to a bare return, which is
// not allowed in a non‑void function)
QString makeSkillFile(const QString &dir, const QString &relPath, const QString &content)
{
    const QString path = dir + QLatin1Char('/') + relPath;
    QDir().mkpath(QFileInfo(path).absolutePath());
    writeTextFile(path, content);
    return path;
}

void LlamaToolsTest::skills_scanBasic()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    makeSkillFile(dir.path(),
                  QStringLiteral("pdf-tools/SKILL.md"),
                  QStringLiteral("---\nname: pdf-tools\ndescription: Extract text from PDFs.\n"
                                 "---\n\n# PDF tools\n\nBody text."));

    const SkillScanResult result = Skills::scan({dir.path()});
    // (first() on an empty list would crash – guard the message)
    QVERIFY2(result.diagnostics.isEmpty(),
             result.diagnostics.isEmpty() ? "" : qPrintable(result.diagnostics.first().message));
    QCOMPARE(result.skills.size(), 1);
    const Skill &skill = result.skills.first();
    QCOMPARE(skill.name, QStringLiteral("pdf-tools"));
    QCOMPARE(skill.description, QStringLiteral("Extract text from PDFs."));
    QCOMPARE(skill.baseDir,
             QFileInfo(dir.path() + QStringLiteral("/pdf-tools")).canonicalFilePath());
    QVERIFY(skill.filePath.endsWith(QStringLiteral("/SKILL.md")));
    QCOMPARE(skill.content, QStringLiteral("# PDF tools\n\nBody text."));
    QVERIFY(!skill.disableModelInvocation);
}

void LlamaToolsTest::skills_scanNestedAndRootFiles()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    makeSkillFile(dir.path(),
                  QStringLiteral("a/deep/SKILL.md"),
                  QStringLiteral("---\nname: deep\ndescription: Nested skill.\n---\nBody"));
    // Bare root‑level Markdown with a description is a skill, the name
    // falling back to the file name.
    makeSkillFile(dir.path(),
                  QStringLiteral("root-skill.md"),
                  QStringLiteral("---\ndescription: A root level skill.\n---\nBody"));
    // No frontmatter description – not a skill, skipped silently.
    makeSkillFile(dir.path(), QStringLiteral("plain.md"), QStringLiteral("Just notes."));

    const SkillScanResult result = Skills::scan({dir.path()});
    QVERIFY2(result.diagnostics.isEmpty(),
             result.diagnostics.isEmpty() ? "" : qPrintable(result.diagnostics.first().message));
    QCOMPARE(result.skills.size(), 2);
    QStringList names;
    for (const Skill &skill : result.skills)
        names << skill.name;
    QVERIFY(names.contains(QStringLiteral("deep")));
    QVERIFY(names.contains(QStringLiteral("root-skill")));
}

void LlamaToolsTest::skills_scanSkillRootStopsRecursion()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    makeSkillFile(dir.path(),
                  QStringLiteral("outer/SKILL.md"),
                  QStringLiteral("---\nname: outer\ndescription: Outer skill.\n---\nBody"));
    // Inside a skill root – must not be picked up.
    makeSkillFile(dir.path(),
                  QStringLiteral("outer/inner/SKILL.md"),
                  QStringLiteral("---\nname: inner\ndescription: Inner skill.\n---\nBody"));

    const SkillScanResult result = Skills::scan({dir.path()});
    QVERIFY(result.diagnostics.isEmpty());
    QCOMPARE(result.skills.size(), 1);
    QCOMPARE(result.skills.first().name, QStringLiteral("outer"));
}

void LlamaToolsTest::skills_scanValidationAndCollisions()
{
    QTemporaryDir dir1;
    QTemporaryDir dir2;
    QVERIFY(dir1.isValid() && dir2.isValid());
    // Invalid name (uppercase + underscore) – skipped with a diagnostic.
    makeSkillFile(dir1.path(),
                  QStringLiteral("bad-name/SKILL.md"),
                  QStringLiteral("---\nname: Bad_Name\ndescription: Bad name.\n---\nBody"));
    // Declared skill without a description – skipped with a diagnostic.
    makeSkillFile(dir2.path(),
                  QStringLiteral("no-desc/SKILL.md"),
                  QStringLiteral("---\nname: no-desc\n---\nBody"));
    // Name collision – the first discovered skill wins.
    makeSkillFile(dir1.path(),
                  QStringLiteral("dup/SKILL.md"),
                  QStringLiteral("---\nname: dup\ndescription: First.\n---\nBody"));
    makeSkillFile(dir2.path(),
                  QStringLiteral("dup/SKILL.md"),
                  QStringLiteral("---\nname: dup\ndescription: Second.\n---\nBody"));
    // Name does not match its directory – kept, but flagged (like pi).
    makeSkillFile(dir2.path(),
                  QStringLiteral("mismatch/SKILL.md"),
                  QStringLiteral("---\nname: other\ndescription: Mismatch.\n---\nBody"));

    const SkillScanResult result = Skills::scan({dir1.path(), dir2.path()});
    QCOMPARE(result.skills.size(), 2);
    const Skill *dup = nullptr;
    const Skill *other = nullptr;
    for (const Skill &skill : result.skills) {
        if (skill.name == QStringLiteral("dup"))
            dup = &skill;
        else if (skill.name == QStringLiteral("other"))
            other = &skill;
    }
    QVERIFY(dup && other);
    QCOMPARE(dup->description, QStringLiteral("First."));
    // Two invalid files + one collision + one name/directory mismatch.
    QCOMPARE(result.diagnostics.size(), 4);
    bool sawMismatch = false;
    for (const SkillDiagnostic &d : result.diagnostics)
        if (d.message.contains(QLatin1String("does not match")))
            sawMismatch = true;
    QVERIFY(sawMismatch);
}

void LlamaToolsTest::skills_scanDisableModelInvocation()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    makeSkillFile(dir.path(),
                  QStringLiteral("hidden/SKILL.md"),
                  QStringLiteral("---\nname: hidden\ndescription: Explicit only.\n"
                                 "disable-model-invocation: true\n---\nBody"));

    const SkillScanResult result = Skills::scan({dir.path()});
    QVERIFY(result.diagnostics.isEmpty());
    QCOMPARE(result.skills.size(), 1);
    QVERIFY(result.skills.first().disableModelInvocation);
}

void LlamaToolsTest::skills_scanSkipsSymlinks()
{
    QTemporaryDir dir;
    QTemporaryDir outside;
    QVERIFY(dir.isValid() && outside.isValid());
    makeSkillFile(outside.path(),
                  QStringLiteral("linked/SKILL.md"),
                  QStringLiteral("---\nname: linked\ndescription: Outside.\n---\nBody"));
    if (::symlink((outside.path() + QStringLiteral("/linked")).toLocal8Bit().constData(),
                  (dir.path() + QStringLiteral("link")).toLocal8Bit().constData())
        != 0)
        QSKIP("symlinks are not supported here");

    const SkillScanResult result = Skills::scan({dir.path()});
    QVERIFY(result.diagnostics.isEmpty());
    QCOMPARE(result.skills.size(), 0);
}

void LlamaToolsTest::skills_formatForPrompt()
{
    QCOMPARE(Skills::formatForPrompt({}), QString());

    Skill skill;
    skill.name = QStringLiteral("demo");
    skill.description = QStringLiteral("Handle <files> & \"stuff\"");
    skill.filePath = QStringLiteral("/tmp/skills/demo/SKILL.md");

    const QString prompt = Skills::formatForPrompt({skill});
    QVERIFY(prompt.contains(QStringLiteral("<available_skills>")));
    QVERIFY(prompt.contains(QStringLiteral("</available_skills>")));
    QVERIFY(prompt.contains(QStringLiteral("<name>demo</name>")));
    // XML‑escaped description.
    QVERIFY(prompt.contains(QStringLiteral("&lt;files&gt; &amp; &quot;stuff&quot;")));
    QVERIFY(!prompt.contains(QStringLiteral("<files>")));
    // The skill tool (not read_file) loads the skill, so the list carries
    // no location.
    QVERIFY(prompt.contains(QStringLiteral("skill tool")));
    QVERIFY(!prompt.contains(QStringLiteral("read_file")));
    QVERIFY(!prompt.contains(QStringLiteral("<location>")));
}

void LlamaToolsTest::skills_enabledSkillsFiltering()
{
    const SkillsTestEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString p1 = makeSkillFile(dir.path(),
                                     QStringLiteral("one/SKILL.md"),
                                     QStringLiteral("---\nname: one\ndescription: First.\n---\nB"));
    const QString p2 = makeSkillFile(dir.path(),
                                     QStringLiteral("two/SKILL.md"),
                                     QStringLiteral("---\nname: two\ndescription: Second.\n---\nB"));

    settings().skillsDirectories.setValue({dir.path()});
    settings().disabledSkillsList.setValue({QFileInfo(p2).canonicalFilePath()});
    Skills::clearCache();

    const QVector<Skill> enabled = Skills::enabledSkills();
    QCOMPARE(enabled.size(), 1);
    QCOMPARE(enabled.first().name, QStringLiteral("one"));

    // The disabled list is re‑read on every call (only the scan is cached).
    settings().disabledSkillsList.setValue(QStringList());
    QCOMPARE(Skills::enabledSkills().size(), 2);
}

void LlamaToolsTest::skillTool_loadsSkill()
{
    const SkillsTestEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    makeSkillFile(dir.path(),
                  QStringLiteral("gerrit-diff/SKILL.md"),
                  QStringLiteral("---\nname: gerrit-diff\ndescription: Fetch diffs.\n---\n\n"
                                 "# Gerrit Diff\n\nRun the script.\n"));
    // Accompanying files the model should be told about.
    makeSkillFile(dir.path(), QStringLiteral("gerrit-diff/scripts/fetch.py"), "print(1)");
    makeSkillFile(dir.path(), QStringLiteral("gerrit-diff/references/notes.md"), "notes");
    // A dot directory must not be listed.
    makeSkillFile(dir.path(), QStringLiteral("gerrit-diff/.git/config"), "x");

    settings().skillsDirectories.setValue({dir.path()});
    Skills::clearCache();

    QJsonObject args;
    args[QStringLiteral("name")] = QStringLiteral("gerrit-diff");
    auto [output, ok] = runTool<Tools::SkillTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QVERIFY(output.contains(QStringLiteral("<skill_content name=\"gerrit-diff\">")));
    QVERIFY(output.contains(QStringLiteral("# Gerrit Diff")));
    QVERIFY(output.contains(QStringLiteral("Run the script.")));
    // The frontmatter is not part of the content.
    QVERIFY(!output.contains(QStringLiteral("description: Fetch diffs")));
    const QString baseDir = QFileInfo(dir.path() + QStringLiteral("/gerrit-diff")).canonicalFilePath();
    QVERIFY(output.contains(QString::fromLatin1("Base directory for this skill: ") + baseDir));
    // Sampled file list, relative paths, SKILL.md and dot files excluded.
    QVERIFY(output.contains(QStringLiteral("<skill_files>")));
    QVERIFY(output.contains(QStringLiteral("<file>references/notes.md</file>")));
    QVERIFY(output.contains(QStringLiteral("<file>scripts/fetch.py</file>")));
    QVERIFY(!output.contains(QStringLiteral("SKILL.md</file>")));
    QVERIFY(!output.contains(QStringLiteral(".git")));
    QVERIFY(output.contains(QStringLiteral("</skill_content>")));

    // A skill disabled in the settings is still loadable by explicit name.
    settings().disabledSkillsList.setValue({QFileInfo(dir.path() + "/gerrit-diff/SKILL.md").canonicalFilePath()});
    auto [disabledOutput, disabledOk] = runTool<Tools::SkillTool>(args);
    QVERIFY2(disabledOk, qPrintable(disabledOutput));
    QVERIFY(disabledOutput.contains(QStringLiteral("<skill_content")));
}

void LlamaToolsTest::skillTool_truncatesLongContent()
{
    const SkillsTestEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString body = QStringLiteral("x").repeated(40 * 1024);
    makeSkillFile(dir.path(),
                  QStringLiteral("big/SKILL.md"),
                  QStringLiteral("---\nname: big\ndescription: Big skill.\n---\n") + body);

    settings().skillsDirectories.setValue({dir.path()});
    Skills::clearCache();

    QJsonObject args;
    args[QStringLiteral("name")] = QStringLiteral("big");
    auto [output, ok] = runTool<Tools::SkillTool>(args);
    QVERIFY2(ok, qPrintable(output));
    // Capped like project instructions, with a notice.
    QVERIFY(output.contains(QStringLiteral("[... truncated ...]")));
    QVERIFY(output.length() < 40 * 1024);
}

void LlamaToolsTest::skillTool_unknownSkill()
{
    const SkillsTestEnv env;
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    makeSkillFile(dir.path(),
                  QStringLiteral("one/SKILL.md"),
                  QStringLiteral("---\nname: one\ndescription: First.\n---\nB"));
    makeSkillFile(dir.path(),
                  QStringLiteral("two/SKILL.md"),
                  QStringLiteral("---\nname: two\ndescription: Second.\n---\nB"));

    settings().skillsDirectories.setValue({dir.path()});
    Skills::clearCache();

    QJsonObject args;
    args[QStringLiteral("name")] = QStringLiteral("nope");
    auto [output, ok] = runTool<Tools::SkillTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains(QStringLiteral("nope")));
    // The available names are listed to steer the model back on track.
    QVERIFY(output.contains(QStringLiteral("one")));
    QVERIFY(output.contains(QStringLiteral("two")));

    // Missing name argument.
    auto [emptyOutput, emptyOk] = runTool<Tools::SkillTool>(QJsonObject());
    QVERIFY(!emptyOk);
}

// ============================================================================
// ReadFileTool limits
// ============================================================================

void LlamaToolsTest::readfile_rangeContinuationHint()
{
    QStringList lines;
    for (int i = 1; i <= 300; ++i)
        lines << QStringLiteral("line%1").arg(i);
    writeTextFile(gTempDir->path() + "/hint.txt", lines.join(QStringLiteral("\n")));

    QJsonObject args;
    args[QStringLiteral("file_path")] = QStringLiteral("hint.txt");
    args[QStringLiteral("first_line")] = 1;
    args[QStringLiteral("last_line_inclusive")] = 300;
    auto [output, ok] = runTool<Tools::ReadFileTool>(args);
    QVERIFY(ok);
    QVERIFY(output.contains(QStringLiteral("line1")));
    QVERIFY(output.contains(QStringLiteral("line250")));
    QVERIFY(!output.contains(QStringLiteral("line251")));
    // Actionable continuation hint instead of a silent cut.
    QVERIFY(output.contains(QStringLiteral("[Showing lines 1-250 of 300.")));
    QVERIFY(output.contains(QStringLiteral("first_line=251")));
}

void LlamaToolsTest::readfile_detailsMarkdown()
{
    const Tools::ReadFileTool tool;

    // The file content is fenced with the language derived from the suffix.
    QJsonObject args;
    args[QStringLiteral("file_path")] = QStringLiteral("README.md");
    args[QStringLiteral("first_line")] = 1;
    args[QStringLiteral("last_line_inclusive")] = 3;
    const QString md = tool.detailsMarkdown(args, QStringLiteral("# Title\n\nSome text"), true);
    QVERIFY(md.startsWith(QStringLiteral("```markdown\n# Title")));
    QVERIFY(md.endsWith(QStringLiteral("\n```")));

    // The continuation hint is not part of the file and stays outside the
    // code fence.
    const QString truncated = QStringLiteral("line1\nline2\n\n[Showing lines 1-2 of 10. "
                                             "Continue with first_line=3, last_line_inclusive=251.]");
    const QString md2 = tool.detailsMarkdown(args, truncated, true);
    const int fenceClose = md2.lastIndexOf(QStringLiteral("```"));
    QVERIFY(fenceClose > 0);
    QVERIFY(!md2.left(fenceClose).contains(QStringLiteral("[Showing lines ")));
    // The hint must start on its own line right after the closing fence, so
    // the fence is a valid closing fence and the block renders closed.
    QCOMPARE(md2.mid(fenceClose + 3),
             QStringLiteral("\n[Showing lines 1-2 of 10. "
                            "Continue with first_line=3, last_line_inclusive=251.]"));

    // Unmapped suffixes fall back to "text"; failures show the raw error.
    QJsonObject binArgs;
    binArgs[QStringLiteral("file_path")] = QStringLiteral("data.bin");
    QVERIFY(tool.detailsMarkdown(binArgs, QStringLiteral("x"), true)
                .startsWith(QStringLiteral("```text\nx")));
    QCOMPARE(tool.detailsMarkdown(args, QStringLiteral("File \"README.md\" does not exist."), false),
             QStringLiteral("File \"README.md\" does not exist."));
    QCOMPARE(tool.detailsMarkdown(args, QString(), true), QString());
}

void LlamaToolsTest::readfile_wholeFileCap()
{
    QStringList lines;
    for (int i = 1; i <= 3000; ++i)
        lines << QStringLiteral("line%1").arg(i);
    writeTextFile(gTempDir->path() + "/big.txt", lines.join(QStringLiteral("\n")));

    QJsonObject args;
    args[QStringLiteral("file_path")] = QStringLiteral("big.txt");
    args[QStringLiteral("first_line")] = 1;
    args[QStringLiteral("last_line_inclusive")] = 3000;
    args[QStringLiteral("should_read_entire_file")] = true;
    auto [output, ok] = runTool<Tools::ReadFileTool>(args);
    QVERIFY(ok);
    QVERIFY(output.contains(QStringLiteral("line2000")));
    QVERIFY(!output.contains(QStringLiteral("line2001")));
    QVERIFY(output.contains(QStringLiteral("[Showing lines 1-2000 of 3000.")));
    QVERIFY(output.contains(QStringLiteral("first_line=2001")));
}

void LlamaToolsTest::readfile_byteCap()
{
    // 60 lines of 1000 bytes: past the 50 KB cap.
    const QString bigLine(1000, QLatin1Char('x'));
    QString content;
    for (int i = 0; i < 60; ++i)
        content += (i > 0 ? QStringLiteral("\n") : QString()) + bigLine;
    writeTextFile(gTempDir->path() + "/wide.txt", content);

    QJsonObject args;
    args[QStringLiteral("file_path")] = QStringLiteral("wide.txt");
    args[QStringLiteral("first_line")] = 1;
    args[QStringLiteral("last_line_inclusive")] = 60;
    args[QStringLiteral("should_read_entire_file")] = true;
    auto [output, ok] = runTool<Tools::ReadFileTool>(args);
    QVERIFY(ok);
    QVERIFY(output.contains(QStringLiteral("50 KB limit reached")));
    // 51 lines fit (51 x 1001 bytes), the 52nd does not.
    QVERIFY(output.contains(QStringLiteral("first_line=52")));
}

void LlamaToolsTest::readfile_emptyFile()
{
    // A 0‑byte file is a legitimate empty result, not a byte‑cap error.
    writeTextFile(gTempDir->path() + "/empty.txt", {});

    QJsonObject args;
    args[QStringLiteral("file_path")] = QStringLiteral("empty.txt");
    args[QStringLiteral("first_line")] = 1;
    args[QStringLiteral("last_line_inclusive")] = 1;
    args[QStringLiteral("should_read_entire_file")] = true;
    auto [output, ok] = runTool<Tools::ReadFileTool>(args);
    QVERIFY(ok);
    QCOMPARE(output, QString());

    // … and so is a slice covering only empty lines (with the usual
    // continuation hint, since more lines follow).
    writeTextFile(gTempDir->path() + "/blanklines.txt", QStringLiteral("\n\n"));
    args[QStringLiteral("file_path")] = QStringLiteral("blanklines.txt");
    args[QStringLiteral("last_line_inclusive")] = 1;
    args[QStringLiteral("should_read_entire_file")] = false;
    std::tie(output, ok) = runTool<Tools::ReadFileTool>(args);
    QVERIFY(ok);
    QVERIFY(output.trimmed().startsWith(QStringLiteral("[Showing lines 1-1 of 3.")));
    QVERIFY(!output.contains(QStringLiteral("limit")));
}

void LlamaToolsTest::readfile_imageAttachment()
{
    // A minimal 1x1 transparent PNG.
    const QByteArray png = QByteArray::fromHex(
        "89504E470D0A1A0A0000000D49484452000000010000000108060000001F15C489"
        "0000000D49444154789C62000100000500010D0A2DB40000000049454E44AE426082");
    QVERIFY(!png.isEmpty());
    const QString path = gTempDir->path() + "/pixel.png";
    {
        QFile f(path);
        QVERIFY(f.open(QFile::WriteOnly));
        f.write(png);
    }

    QJsonObject args;
    args[QStringLiteral("file_path")] = QStringLiteral("pixel.png");
    args[QStringLiteral("first_line")] = 1;
    args[QStringLiteral("last_line_inclusive")] = 1;
    auto [output, ok] = runTool<Tools::ReadFileTool>(args);
    QVERIFY2(ok, qPrintable(output));
    // The result carries the image as a data URL behind the markers.
    QVERIFY(output.contains(QStringLiteral("[[llama:image]]")));
    QVERIFY(output.contains(QStringLiteral("data:image/png;base64,")));

    QString text;
    QString dataUrl;
    QVERIFY2(splitToolResultImage(output, text, dataUrl), qPrintable(output));
    QVERIFY(text.contains(QStringLiteral("Image file")));
    QVERIFY(text.contains(QStringLiteral("image/png")));
    QCOMPARE(dataUrl,
             QStringLiteral("data:image/png;base64,") + QString::fromLatin1(png.toBase64()));
}

void LlamaToolsTest::readfile_imageTooLarge()
{
    // 10 MB + 1 of JPEG magic + padding: past the image read limit.
    const QString path = gTempDir->path() + "/huge.jpg";
    {
        QFile f(path);
        QVERIFY(f.open(QFile::WriteOnly));
        f.write(QByteArray::fromHex("FFD8FF"));
        f.write(QByteArray(10 * 1024 * 1024, 0));
    }

    QJsonObject args;
    args[QStringLiteral("file_path")] = QStringLiteral("huge.jpg");
    auto [output, ok] = runTool<Tools::ReadFileTool>(args);
    QVERIFY(!ok);
    QVERIFY(output.contains(QStringLiteral("10 MB read limit")));
}

void LlamaToolsTest::toolImage_wrapSplitRoundTrip()
{
    const QString wrapped = toolResultWithImage(QStringLiteral("hello"),
                                                 QStringLiteral("data:image/png;base64,AAA="));
    QString text;
    QString url;
    QVERIFY(splitToolResultImage(wrapped, text, url));
    QCOMPARE(text, QString("hello"));
    QCOMPARE(url, QString("data:image/png;base64,AAA="));

    // An empty text part is fine (the markers delimit the base64 data URL).
    const QString wrappedNoText =
            toolResultWithImage(QString(), QStringLiteral("data:image/jpeg;base64,BBB="));
    QVERIFY(splitToolResultImage(wrappedNoText, text, url));
    QCOMPARE(text, QString());
    QCOMPARE(url, QString("data:image/jpeg;base64,BBB="));

    // Plain results are left alone.
    QVERIFY(!splitToolResultImage(QStringLiteral("plain output"), text, url));
    // A marker without a data URL payload is not an image attachment.
    QVERIFY(!splitToolResultImage(QStringLiteral("[[llama:image]]not-a-url[[/llama:image]]"),
                                  text,
                                  url));
}

void LlamaToolsTest::toolImage_textFromContentParts()
{
    // Plain string content.
    QCOMPARE(toolResultText(QJsonValue(QStringLiteral("plain"))), QString("plain"));

    // Content parts: text parts are joined, image parts are skipped.
    QJsonArray parts;
    QJsonObject textPart1;
    textPart1["type"] = QStringLiteral("text");
    textPart1["text"] = QStringLiteral("line1");
    parts.append(textPart1);
    QJsonObject imagePart;
    imagePart["type"] = QStringLiteral("image_url");
    imagePart["image_url"] = QJsonObject{{"url", QStringLiteral("data:image/png;base64,AAA=")}};
    parts.append(imagePart);
    QJsonObject textPart2;
    textPart2["type"] = QStringLiteral("text");
    textPart2["text"] = QStringLiteral("line2");
    parts.append(textPart2);
    QCOMPARE(toolResultText(QJsonValue(parts)), QString("line1\nline2"));

    // No content at all.
    QCOMPARE(toolResultText(QJsonValue()), QString());
}

// ============================================================================
// EditFileTool argument tolerance / result
// ============================================================================

void LlamaToolsTest::editfile_editsAsJsonString()
{
    writeTextFile(gTempDir->path() + "/str.txt", QStringLiteral("alpha\n"));
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("str.txt");
    args[QStringLiteral("edits")]
        = QStringLiteral("[{\"oldText\": \"alpha\", \"newText\": \"beta\"}]");
    auto [output, ok] = runTool<Tools::EditFileTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(readTextFile(gTempDir->path() + "/str.txt"), QString("beta\n"));
}

void LlamaToolsTest::editfile_editsAsObject()
{
    writeTextFile(gTempDir->path() + "/obj.txt", QStringLiteral("alpha\n"));
    QJsonObject edit;
    edit[QStringLiteral("oldText")] = QStringLiteral("alpha");
    edit[QStringLiteral("newText")] = QStringLiteral("beta");
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("obj.txt");
    args[QStringLiteral("edits")] = edit; // single object, not an array
    auto [output, ok] = runTool<Tools::EditFileTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(readTextFile(gTempDir->path() + "/obj.txt"), QString("beta\n"));
}

void LlamaToolsTest::editfile_legacyTopLevel()
{
    writeTextFile(gTempDir->path() + "/legacy.txt", QStringLiteral("alpha\n"));
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("legacy.txt");
    args[QStringLiteral("oldText")] = QStringLiteral("alpha");
    args[QStringLiteral("newText")] = QStringLiteral("beta");
    auto [output, ok] = runTool<Tools::EditFileTool>(args);
    QVERIFY2(ok, qPrintable(output));
    QCOMPARE(readTextFile(gTempDir->path() + "/legacy.txt"), QString("beta\n"));
}

void LlamaToolsTest::editfile_resultContainsDiff()
{
    writeTextFile(gTempDir->path() + "/diff.txt", QStringLiteral("alpha\n"));
    QJsonObject edit;
    edit[QStringLiteral("oldText")] = QStringLiteral("alpha");
    edit[QStringLiteral("newText")] = QStringLiteral("beta");
    QJsonObject args;
    args[QStringLiteral("path")] = QStringLiteral("diff.txt");
    args[QStringLiteral("edits")] = QJsonArray{edit};
    auto [output, ok] = runTool<Tools::EditFileTool>(args);
    QVERIFY2(ok, qPrintable(output));
    // The model sees the change without re‑reading the file.
    QVERIFY(output.contains(QStringLiteral("```diff")));
    QVERIFY(output.contains(QStringLiteral("-alpha")));
    QVERIFY(output.contains(QStringLiteral("+beta")));
}

// ============================================================================
// repairJson (string‑literal repair for small‑model tool arguments)
// ============================================================================

void LlamaToolsTest::repairJson_controlChars()
{
    // A real newline and tab inside a JSON string (the #1 small‑model
    // malformations, e.g. multi‑line bash commands).  Qt's parser already
    // tolerates these; the repair must keep the value intact.
    const QString broken = QStringLiteral("{\"command\": \"echo hi\n\tworld\"}");
    const QString repaired = repairJson(broken);
    QVERIFY(repaired != broken);
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(repaired.toUtf8(), &err);
    QVERIFY2(err.error == QJsonParseError::NoError, qPrintable(repaired));
    QCOMPARE(doc.object().value(QStringLiteral("command")).toString(),
             QString(QStringLiteral("echo hi\n\tworld")));
}

void LlamaToolsTest::repairJson_invalidEscapes()
{
    // A backslash before an invalid escape character (a Windows‑ish path).
    // Qt's parser accepts this but silently *drops* the backslash
    // ("C:.projects"); the repair doubles it, so the tool receives the
    // intended literal.
    const QString broken = QStringLiteral("{\"path\": \"C:\\.projects\"}");
    QJsonParseError err;
    const QJsonDocument unrepaired = QJsonDocument::fromJson(broken.toUtf8(), &err);
    QCOMPARE(unrepaired.object().value(QStringLiteral("path")).toString(),
             QString(QStringLiteral("C:.projects")));

    const QJsonDocument doc = QJsonDocument::fromJson(repairJson(broken).toUtf8(), &err);
    QVERIFY2(err.error == QJsonParseError::NoError, qPrintable(repairJson(broken)));
    QCOMPARE(doc.object().value(QStringLiteral("path")).toString(),
             QString(QStringLiteral("C:\\.projects")));
}

void LlamaToolsTest::repairJson_validUnchanged()
{
    const QString valid = QStringLiteral("{\"a\": 1, \"b\": \"x\\n\\u0041\\t\", \"c\": true}");
    QCOMPARE(repairJson(valid), valid);
}

void LlamaToolsTest::repairJson_structureNotFixed()
{
    // Truncated / structurally broken JSON is out of scope: the repair must
    // not hide it (the caller reports the parse failure to the model).
    QJsonParseError err;
    const QString truncated = QStringLiteral("{\"a\": \"b\"");
    QVERIFY(QJsonDocument::fromJson(repairJson(truncated).toUtf8(), &err).isNull());

    const QString notJson = QStringLiteral("sure, here is the JSON: {\"a\": 1");
    QVERIFY(QJsonDocument::fromJson(repairJson(notJson).toUtf8(), &err).isNull());
}

} // namespace LlamaCpp

QTEST_MAIN(LlamaCpp::LlamaToolsTest)
#include "llamatools_test.moc"
