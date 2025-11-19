#include <QFile>
#include <QString>
#include <QStringLiteral>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest/QtTest>

#include <coreplugin/documentmanager.h>
#include <projectexplorer/projectmanager.h>
#include <texteditor/basefilefind.h>

namespace LlamaCpp {
QString createFile(const QString &relPath, const QString &content);
QString deleteFile(const QString &relPath);
QString editFile(const QString &path,
                 const QString &operation,
                 const QString &search,
                 const QString &replace,
                 const QString &newContent);
QString diffForEditFile(const QString &filePath,
                        const QString &operation,
                        const QString &search,
                        const QString &replace,
                        const QString &newFileContent);
} // namespace LLamaCpp
using namespace LlamaCpp;

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

    void editCreate();
    void editReplace();
    void editDelete();
    void editInsertBefore();
    void editInsertAfter();
    void editDeleteFile();

    void diffCreate();
    void diffReplace();
    void diffDelete();
    void diffInsertBefore();
    void diffInsertAfter();
    void diffDeleteFile();
};

static QTemporaryDir *gTempDir = nullptr;

void LlamaToolsTest::initTestCase()
{
    // Create a fresh temporary directory that lives for the whole test run
    gTempDir = new QTemporaryDir;
    QVERIFY2(gTempDir->isValid(), "Failed to create temporary directory");

    // Tell the tools to use this directory as the “working directory”.
    // Core::DocumentManager::setProjectsDirectory() is a public static setter.
    Core::DocumentManager::setProjectsDirectory(Utils::FilePath::fromString(gTempDir->path()));
}

void LlamaToolsTest::cleanupTestCase()
{
    delete gTempDir;
    gTempDir = nullptr;
}

void LlamaToolsTest::editCreate()
{
    const QString relPath = "new_file.txt";
    const QString content = "first line\nsecond line\nthird line";

    // invoke the helper
    const QString result = editFile(relPath, "create", {}, {}, content);
    QVERIFY2(result.contains("Created"), result.toLocal8Bit().constData());

    // verify that the file really exists and contains exactly what we passed
    QFileInfo fi(gTempDir->filePath(relPath));
    QVERIFY2(fi.isFile(), "File was not created");
    QFile file(fi.filePath());
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), content.toUtf8());
}

void LlamaToolsTest::editReplace()
{
    const QString relPath = "replace.txt";
    const QString original = "AAA\nBBB\nCCC\nDDD\nEEE";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "BBB\nCCC";
    const QString replace = "bb\ncc";

    const QString result = editFile(relPath, "replace", search, replace, {});
    QVERIFY2(result.contains("Edited"), result.toLocal8Bit().constData());

    const QString expected = "AAA\nbb\ncc\nDDD\nEEE";
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), expected.toUtf8());
}

void LlamaToolsTest::editDelete()
{
    const QString relPath = "delete.txt";
    const QString original = "111\n222\n333\n444\n555";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "222\n333";

    const QString result = editFile(relPath, "delete", search, {}, {});
    QVERIFY2(result.contains("Edited"), result.toLocal8Bit().constData());

    const QString expected = "111\n444\n555";
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), expected.toUtf8());
}

void LlamaToolsTest::editInsertBefore()
{
    const QString relPath = "insert_before.txt";
    const QString original = "alpha\nbeta\ngamma";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "beta";
    const QString replace = "INSERTED";

    const QString result = editFile(relPath, "insert_before", search, replace, {});
    QVERIFY2(result.contains("Edited"), result.toLocal8Bit().constData());

    const QString expected = "alpha\nINSERTED\nbeta\ngamma";
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), expected.toUtf8());
}

void LlamaToolsTest::editInsertAfter()
{
    const QString relPath = "insert_after.txt";
    const QString original = "one\ntwo\nthree";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "two";
    const QString replace = "INSERTED";

    const QString result = editFile(relPath, "insert_after", search, replace, {});
    QVERIFY2(result.contains("Edited"), result.toLocal8Bit().constData());

    const QString expected = "one\ntwo\nINSERTED\nthree";
    QFile file(gTempDir->filePath(relPath));
    file.open(QFile::ReadWrite);
    QCOMPARE(file.readAll(), expected.toUtf8());
}

void LlamaToolsTest::editDeleteFile()
{
    const QString relPath = "to_be_removed.txt";
    writeFile(gTempDir->filePath(relPath), "something");

    const QString result = editFile(relPath, "delete_file", {}, {}, {});
    QVERIFY2(result.contains("Deleted"), result.toLocal8Bit().constData());

    QFileInfo fi(gTempDir->filePath(relPath));
    QVERIFY2(!fi.exists(), "File was not deleted");
}

void LlamaToolsTest::diffCreate()
{
    const QString relPath = "diff_create.txt";
    const QString newContent = "a\nb\nc";

    const QString diff = diffForEditFile(relPath, "create", {}, {}, newContent);
    QVERIFY2(diff.contains("--- a/" + relPath), "Missing --- header");
    QVERIFY2(diff.contains("+++ b/" + relPath), "Missing +++ header");
    QVERIFY2(diff.contains("@@ -0,0 +1,3 @@"), "Wrong hunk header");
    QVERIFY2(diff.contains("+a"), "Missing added line a");
    QVERIFY2(diff.contains("+b"), "Missing added line b");
    QVERIFY2(diff.contains("+c"), "Missing added line c");
}

void LlamaToolsTest::diffReplace()
{
    const QString relPath = "diff_replace.txt";
    const QString original = "line1\nold\nline3";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "old";
    const QString replace = "new";

    const QString diff = diffForEditFile(relPath, "replace", search, replace, {});
    // The diff should remove the line “old” and add “new”
    QVERIFY2(diff.contains("-old"), "Old line not removed in diff");
    QVERIFY2(diff.contains("+new"), "New line not added in diff");
}

void LlamaToolsTest::diffDelete()
{
    const QString relPath = "diff_delete.txt";
    const QString original = "A\nB\nC\nD";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "B\nC";

    const QString diff = diffForEditFile(relPath, "delete", search, {}, {});
    QVERIFY2(diff.contains("-B"), "Missing deletion of line B");
    QVERIFY2(diff.contains("-C"), "Missing deletion of line C");
}

void LlamaToolsTest::diffInsertBefore()
{
    const QString relPath = "diff_insert_before.txt";
    const QString original = "start\nmid\nend";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "mid";
    const QString replace = "INSERTED";

    const QString diff = diffForEditFile(relPath, "insert_before", search, replace, {});
    QVERIFY2(diff.contains("+INSERTED"), "Inserted line missing");
    // The context line “mid” must still be present (it is not removed)
    QVERIFY2(diff.contains(" mid"), "Context line missing");
}

void LlamaToolsTest::diffInsertAfter()
{
    const QString relPath = "diff_insert_after.txt";
    const QString original = "head\nbody\nfoot";
    writeFile(gTempDir->filePath(relPath), original);

    const QString search = "body";
    const QString replace = "INSERTED";

    const QString diff = diffForEditFile(relPath, "insert_after", search, replace, {});
    QVERIFY2(diff.contains("+INSERTED"), "Inserted line missing");
    // The line “body” should still appear in the diff as context
    QVERIFY2(diff.contains(" body"), "Context line missing");
}

void LlamaToolsTest::diffDeleteFile()
{
    const QString relPath = "diff_delete_file.txt";
    const QString original = "some content";
    writeFile(gTempDir->filePath(relPath), original);

    const QString diff = diffForEditFile(relPath, "delete_file", {}, {}, {});
    // For a delete‑file operation the diff has the content with -
    QVERIFY2(diff.startsWith("--- a/" + relPath), "Missing --- header");
    QVERIFY2(diff.contains("+++ b/" + relPath), "Missing +++ header");
    // No '+' lines should be present
    QVERIFY2(!diff.contains("+some content"), "Unexpected addition lines in delete‑file diff");
    QVERIFY2(diff.contains("-some content"), "Missing deletion lines in delete‑file diff");
}

QTEST_MAIN(LlamaToolsTest)
#include "llamatools_test.moc"
