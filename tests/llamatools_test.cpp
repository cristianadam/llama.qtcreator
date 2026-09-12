#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/projectmanager.h>

#include <llamathinkingsectionparser.h>
#include <tools/apply_patch_tool.h>
#include <tools/factory.h>
#include <tools/patch.h>
#include <tools/task_tool.h>
#include <tools/webfetch_tool.h>
#include <tools/websearch_tool.h>

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

std::pair<QString, bool> runTool(const QJsonObject &args)
{
    QString output;
    bool ok = false;
    ApplyPatchTool tool;
    tool.run(args,
             [&output, &ok](const QString &out, bool success) {
                 output = out;
                 ok = success;
             });
    return {output, ok};
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

    // ApplyPatchTool::run
    void tool_fullPatch();
    void tool_move();
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

    // WebFetchTool
    void webfetch_normalizeUrl();
    void webfetch_htmlToMarkdown();
    void webfetch_htmlToText();
    void webfetch_toolDefinition();
    void webfetch_summaries();

    // WebSearchTool
    void websearch_parseResults();
    void websearch_maxResults();
    void websearch_toolDefinition();
    void websearch_summaries();
    void websearch_exaEndpointUrl();
    void websearch_parseMcpResponse();
    void websearch_parseMcpResponseSse();
    void websearch_parseGoogleResults();
    void websearch_formatResults();

    // TaskTool
    void task_toolDefinition();
    void task_summaries();
    void task_toolsFor();
    void task_systemPrompt();
    void task_finalReport();

    // Factory registration
    void factory_applyPatch();
    void factory_webTools();
    void factory_task();
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

// ============================================================================
// ApplyPatchTool::run
// ============================================================================

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
        = tool.detailsMarkdown(addArgs, QStringLiteral("Success. Updated the following files:\nA src/main.cpp"));
    QVERIFY(addDetails.contains("created"));
    QVERIFY(addDetails.contains("```cpp"));
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
    const QString editDetails = tool.detailsMarkdown(editArgs, output);
    QVERIFY(editDetails.contains("edited"));
    QVERIFY(editDetails.contains("@@ -2,1 +2,1 @@"));
    QVERIFY(editDetails.contains("-line2"));
    QVERIFY(editDetails.contains("+changed"));
    QVERIFY(editDetails.contains("```diff"));

    // Failed patch: the error text is shown as‑is
    QCOMPARE(tool.detailsMarkdown(addArgs, QStringLiteral("apply_patch verification failed: boom")),
             QString("apply_patch verification failed: boom"));
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

namespace {

// Fixture mirroring the real lite.duckduckgo.com markup
const QString kDuckDuckGoFixture = QStringLiteral(
    "<html><body><form id=\"lite\"><table>"
    "<tr><td><a rel=\"nofollow\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fdoc.qt.io%2Fqt-6%2Findex.html&amp;rut=abc123\" "
    "class='result-link'>Qt 6 Reference Docs</a></td></tr>"
    "<tr><td class='result-snippet'>\n"
    "    Official documentation for <b>Qt 6</b> &amp; its modules.\n"
    "  </td></tr>"
    "<tr><td><a rel=\"nofollow\" "
    "href=\"https://example.org/second\" "
    "class='result-link'>Second &amp; Third</a></td></tr>"
    "<tr><td class='result-snippet'>Snippet for the second result.</td></tr>"
    "<tr><td><a rel=\"nofollow\" "
    "href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.org%2Fthird&amp;rut=def456\" "
    "class='result-link'>Third</a></td></tr>"
    "</table></form></body></html>");

} // namespace

void LlamaToolsTest::websearch_parseResults()
{
    const auto results = Tools::parseDuckDuckGoResults(kDuckDuckGoFixture, 10);
    QCOMPARE(results.size(), 3);

    QCOMPARE(results[0].title, QString("Qt 6 Reference Docs"));
    QCOMPARE(results[0].url, QString("https://doc.qt.io/qt-6/index.html"));
    QCOMPARE(results[0].snippet, QString("Official documentation for Qt 6 & its modules."));

    // A direct (non‑wrapped) href is used as‑is
    QCOMPARE(results[1].title, QString("Second & Third"));
    QCOMPARE(results[1].url, QString("https://example.org/second"));
    QCOMPARE(results[1].snippet, QString("Snippet for the second result."));

    // A result without a snippet gets an empty one
    QCOMPARE(results[2].title, QString("Third"));
    QCOMPARE(results[2].url, QString("https://example.org/third"));
    QVERIFY(results[2].snippet.isEmpty());
}

void LlamaToolsTest::websearch_maxResults()
{
    const auto results = Tools::parseDuckDuckGoResults(kDuckDuckGoFixture, 2);
    QCOMPARE(results.size(), 2);
    QCOMPARE(results[1].url, QString("https://example.org/second"));
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
    const QString md = tool.detailsMarkdown(args, QStringLiteral("Found main()"));
    QVERIFY(md.contains("### Explore build system"));
    QVERIFY(md.contains("Find the entry point"));
    QVERIFY(md.contains("Found main()"));
}

void LlamaToolsTest::task_toolsFor()
{
    const QStringList all = {QStringLiteral("task"),
                             QStringLiteral("ask_user"),
                             QStringLiteral("read_file"),
                             QStringLiteral("shell"),
                             QStringLiteral("edit_file")};

    // explore: only the read‑only subset
    QCOMPARE(Tools::taskToolsFor(QStringLiteral("explore"), all),
             (QStringList{QStringLiteral("read_file")}));

    // general: everything except task and ask_user, original order kept
    QCOMPARE(Tools::taskToolsFor(QStringLiteral("general"), all),
             (QStringList{QStringLiteral("read_file"),
                          QStringLiteral("shell"),
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

} // namespace LlamaCpp

QTEST_MAIN(LlamaCpp::LlamaToolsTest)
#include "llamatools_test.moc"
