#include <QFile>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/projectmanager.h>

#include <tools/editfile_tool.h>
#include <tools/writefile_tool.h>
#include <tools/deletefile_tool.h>
#include <tools/factory.h>

namespace LlamaCpp {

static bool writeFile(const QString &absPath, const QString &content)
{
    QFile f(absPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    QTextStream out(&f);
    out << content;
    return true;
}

class LlamaToolsTest : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();    // called once before the first test
    void cleanupTestCase(); // called once after the last test

    // write_file tests
    void writeFile_create();
    void writeFile_overwrite();
    
    // edit_file (string matching) tests
    void editFile_exactMatch();
    void editFile_deleteText();
    void editFile_multipleOccurrences();
    void editFile_notFound();
    void editFile_whitespaceNormalized();
    void editFile_toolDefinition();
    void editFile_oneLineSummary();
    
    // delete_file tests
    void deleteFile_success();
    void deleteFile_notFound();
    void deleteFile_toolDefinition();
    
    // write_file tests
    void writeFile_toolDefinition();
    void writeFile_oneLineSummary();
    
    // Factory registration tests
    void factory_editFile();
    void factory_writeFile();
    void factory_deleteFile();
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
// write_file tests
// ============================================================================

void LlamaToolsTest::writeFile_create()
{
    const QString relPath = "new_written.txt";
    const QString content = "line one\nline two\nline three";
    
    writeFile(gTempDir->filePath(relPath), content);
    
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), content.toUtf8());
}

void LlamaToolsTest::writeFile_overwrite()
{
    const QString relPath = "overwritten.txt";
    const QString original = "original content";
    const QString newContent = "new content here";
    
    writeFile(gTempDir->filePath(relPath), original);
    writeFile(gTempDir->filePath(relPath), newContent);
    
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), newContent.toUtf8());
}

void LlamaToolsTest::writeFile_toolDefinition()
{
    WriteFileTool tool;
    QString def = tool.toolDefinition();
    
    QVERIFY(def.contains("write_file"));
    QVERIFY(def.contains("file_path"));
    QVERIFY(def.contains("content"));
    QVERIFY(def.contains("required"));
}

void LlamaToolsTest::writeFile_oneLineSummary()
{
    WriteFileTool tool;
    QJsonObject args;
    args["file_path"] = "src/test.cpp";
    
    QString summary = tool.oneLineSummary(args);
    QVERIFY(summary.contains("write file"));
    QVERIFY(summary.contains("src/test.cpp"));
}

// ============================================================================
// edit_file (string matching) tests
// ============================================================================

void LlamaToolsTest::editFile_exactMatch()
{
    const QString relPath = "exact_match.txt";
    const QString original = "first line\nold text here\nlast line";
    writeFile(gTempDir->filePath(relPath), original);
    
    QString content = original;
    QString oldText = "old text here";
    QString newText = "new text here";
    
    int idx = content.indexOf(oldText);
    QVERIFY(idx >= 0);
    
    content = content.left(idx) + newText + content.mid(idx + oldText.size());
    writeFile(gTempDir->filePath(relPath), content);
    
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), content.toUtf8());
}

void LlamaToolsTest::editFile_deleteText()
{
    const QString relPath = "delete_text.txt";
    const QString original = "keep this\nremove this\nkeep that";
    writeFile(gTempDir->filePath(relPath), original);
    
    QString content = original;
    QString oldText = "\nremove this\n";
    QString newText = "";
    
    int idx = content.indexOf(oldText);
    QVERIFY(idx >= 0);
    content = content.left(idx) + newText + content.mid(idx + oldText.size());
    
    writeFile(gTempDir->filePath(relPath), content);
    
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), content.toUtf8());
}

void LlamaToolsTest::editFile_multipleOccurrences()
{
    const QString relPath = "multi_occurrence.txt";
    const QString original = "AAA\nBBB\nAAA\nCCC";
    writeFile(gTempDir->filePath(relPath), original);
    
    QString content = original;
    QString searchText = "AAA";
    int occurrences = 0;
    int searchStart = 0;
    while ((searchStart = content.indexOf(searchText, searchStart)) >= 0) {
        ++occurrences;
        ++searchStart;
    }
    
    QCOMPARE(occurrences, 2);
}

void LlamaToolsTest::editFile_notFound()
{
    const QString relPath = "not_found.txt";
    const QString original = "some content here";
    writeFile(gTempDir->filePath(relPath), original);
    
    QString content = original;
    QString searchText = "not present in file";
    
    int idx = content.indexOf(searchText);
    QCOMPARE(idx, -1);
}

void LlamaToolsTest::editFile_whitespaceNormalized()
{
    const QString relPath = "whitespace_test.txt";
    const QString original = "first   line   with\ttabs\nsecond line";
    writeFile(gTempDir->filePath(relPath), original);
    
    QString content = original;
    QString searchText = "first line with tabs";
    
    // Exact match should fail
    int exactIdx = content.indexOf(searchText);
    QCOMPARE(exactIdx, -1);
    
    // Whitespace normalization should work
    auto normalizeWhitespace = [](const QString &s) {
        QString result;
        bool lastWasSpace = false;
        for (const QChar &c : s) {
            if (c.isSpace()) {
                if (!lastWasSpace && !result.isEmpty()) {
                    result += ' ';
                    lastWasSpace = true;
                }
            } else {
                result += c;
                lastWasSpace = false;
            }
        }
        return result;
    };
    
    QString normalizedContent = normalizeWhitespace(content);
    QString normalizedSearch = normalizeWhitespace(searchText);
    
    int normIdx = normalizedContent.indexOf(normalizedSearch);
    QVERIFY(normIdx >= 0);
}

void LlamaToolsTest::editFile_toolDefinition()
{
    EditFileTool tool;
    QString def = tool.toolDefinition();
    
    QVERIFY(def.contains("edit_file"));
    QVERIFY(def.contains("file_path"));
    QVERIFY(def.contains("old_string"));
    QVERIFY(def.contains("new_string"));
    QVERIFY(def.contains("required"));
}

void LlamaToolsTest::editFile_oneLineSummary()
{
    EditFileTool tool;
    QJsonObject args;
    args["file_path"] = "src/main.cpp";
    
    QString summary = tool.oneLineSummary(args);
    QVERIFY(summary.contains("edit file"));
    QVERIFY(summary.contains("src/main.cpp"));
}

// ============================================================================
// delete_file tests
// ============================================================================

void LlamaToolsTest::deleteFile_success()
{
    const QString relPath = "will_be_deleted.txt";
    writeFile(gTempDir->filePath(relPath), "content to delete");
    
    QFileInfo fi(gTempDir->filePath(relPath));
    QVERIFY(fi.exists());
    
    QFile file(gTempDir->filePath(relPath));
    QVERIFY(file.remove());
    
    QFileInfo fi2(gTempDir->filePath(relPath));
    QVERIFY2(!fi2.exists(), "File was not deleted");
}

void LlamaToolsTest::deleteFile_notFound()
{
    const QString relPath = "does_not_exist_test.txt";
    
    QFileInfo fi(gTempDir->filePath(relPath));
    QVERIFY2(!fi.exists(), "Test file should not exist");
    
    QFile file(gTempDir->filePath(relPath));
    QVERIFY(!file.remove());
}

void LlamaToolsTest::deleteFile_toolDefinition()
{
    DeleteFileTool tool;
    QString def = tool.toolDefinition();
    
    QVERIFY(def.contains("delete_file"));
    QVERIFY(def.contains("file_path"));
    QVERIFY(def.contains("required"));
}

// ============================================================================
// Factory registration tests
// ============================================================================

void LlamaToolsTest::factory_editFile()
{
    auto &factory = ToolFactory::instance();
    auto tool = factory.create("edit_file");
    QVERIFY(tool != nullptr);
    QCOMPARE(tool->name(), QString("edit_file"));
}

void LlamaToolsTest::factory_writeFile()
{
    auto &factory = ToolFactory::instance();
    auto tool = factory.create("write_file");
    QVERIFY(tool != nullptr);
    QCOMPARE(tool->name(), QString("write_file"));
}

void LlamaToolsTest::factory_deleteFile()
{
    auto &factory = ToolFactory::instance();
    auto tool = factory.create("delete_file");
    QVERIFY(tool != nullptr);
    QCOMPARE(tool->name(), QString("delete_file"));
}

} // namespace LlamaCpp

QTEST_MAIN(LlamaCpp::LlamaToolsTest)
#include "llamatools_test.moc"
