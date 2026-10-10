#include <QtTest/QtTest>

#include "llamalocatorprompts.h"

using namespace LlamaCpp;

class LlamaLocatorPromptsTest : public QObject
{
    Q_OBJECT

private slots:
    // applyLocatorPlaceholders
    void apply_noPlaceholders();
    void apply_allPlaceholders();
    void apply_missingValueBecomesEmpty();
    void apply_multipleOccurrences();
    void apply_valueContainingPlaceholderIsNotRescanned();

    // formatDiagnostics
    void diagnostics_empty();
    void diagnostics_sortedAndFormatted();
    void diagnostics_capped();

    // formatGitDiff
    void gitDiff_failed();
    void gitDiff_cleanRepository();
    void gitDiff_untrackedOnly();
    void gitDiff_withDiff();
    void gitDiff_untrackedCapped();
    void gitDiff_truncatedAtLineBoundary();
};

void LlamaLocatorPromptsTest::apply_noPlaceholders()
{
    LocatorPlaceholders values;
    const QString prompt = QStringLiteral("Just a plain prompt");
    QCOMPARE(applyLocatorPlaceholders(prompt, values), prompt);
}

void LlamaLocatorPromptsTest::apply_allPlaceholders()
{
    LocatorPlaceholders values;
    values.selection = QStringLiteral("\n```cpp /path/a.cpp:1-3\nint x;\n```\n");
    values.file = QStringLiteral("/path/a.cpp");
    values.line = QStringLiteral("7");
    values.diagnostics = QStringLiteral("line 7: Clangd: error: ...");
    values.gitDiff = QStringLiteral("\n```diff\n+1\n```\n");

    const QString prompt = QStringLiteral(
        "Fix {diagnostics} in {file} at line {line} for {selection} after {git_diff}");
    const QString expected = QStringLiteral(
        "Fix line 7: Clangd: error: ... in /path/a.cpp at line 7 for "
        "\n```cpp /path/a.cpp:1-3\nint x;\n```\n after \n```diff\n+1\n```\n");
    QCOMPARE(applyLocatorPlaceholders(prompt, values), expected);
}

void LlamaLocatorPromptsTest::apply_missingValueBecomesEmpty()
{
    LocatorPlaceholders values; // all empty
    QCOMPARE(applyLocatorPlaceholders(QStringLiteral("Explain {selection}"), values),
             QStringLiteral("Explain "));
    QCOMPARE(applyLocatorPlaceholders(QStringLiteral("In {file}:{line}"), values),
             QStringLiteral("In :"));
}

void LlamaLocatorPromptsTest::apply_multipleOccurrences()
{
    LocatorPlaceholders values;
    values.file = QStringLiteral("a.cpp");
    QCOMPARE(applyLocatorPlaceholders(QStringLiteral("{file} and {file}"), values),
             QStringLiteral("a.cpp and a.cpp"));
}

void LlamaLocatorPromptsTest::apply_valueContainingPlaceholderIsNotRescanned()
{
    LocatorPlaceholders values;
    // The selected text itself mentions a placeholder: it must be inserted
    // verbatim, not substituted again.
    values.selection = QStringLiteral("{file}");
    values.file = QStringLiteral("a.cpp");
    QCOMPARE(applyLocatorPlaceholders(QStringLiteral("Look at {selection} in {file}"),
                                      values),
             QStringLiteral("Look at {file} in a.cpp"));
}

void LlamaLocatorPromptsTest::diagnostics_empty()
{
    QCOMPARE(formatDiagnostics({}),
             QStringLiteral("(no errors or warnings reported for this file)"));
}

void LlamaLocatorPromptsTest::diagnostics_sortedAndFormatted()
{
    QList<LocatorDiagnostic> diagnostics;
    diagnostics.append({12, QStringLiteral("Clangd error: no matching function")});
    diagnostics.append({3, QStringLiteral("QML Warning: unused property")});
    const QString result = formatDiagnostics(diagnostics);
    const QStringList lines = result.split('\n');
    QCOMPARE(lines.size(), 2);
    QCOMPARE(lines.at(0), QStringLiteral("line 3: QML Warning: unused property"));
    QCOMPARE(lines.at(1),
             QStringLiteral("line 12: Clangd error: no matching function"));
}

void LlamaLocatorPromptsTest::diagnostics_capped()
{
    QList<LocatorDiagnostic> diagnostics;
    for (int line = 1; line <= 30; ++line)
        diagnostics.append({line, QStringLiteral("error %1").arg(line)});
    const QStringList lines = formatDiagnostics(diagnostics).split('\n');
    QCOMPARE(lines.size(), 26); // 25 entries + the "… and 5 more" note
    QCOMPARE(lines.first(), QStringLiteral("line 1: error 1"));
    QCOMPARE(lines.at(24), QStringLiteral("line 25: error 25"));
    QCOMPARE(lines.last(), QStringLiteral("... and 5 more"));
}

void LlamaLocatorPromptsTest::gitDiff_failed()
{
    const QString result = formatGitDiff(QString(), QString(), /*gitFailed=*/true);
    QVERIFY(result.startsWith(QStringLiteral("(could not read the git repository")));
}

void LlamaLocatorPromptsTest::gitDiff_cleanRepository()
{
    QCOMPARE(formatGitDiff(QString(), QString(), /*gitFailed=*/false),
             QStringLiteral("(no uncommitted changes)"));
}

void LlamaLocatorPromptsTest::gitDiff_untrackedOnly()
{
    // `git diff` shows nothing for untracked files, but they must be listed.
    const QString result = formatGitDiff(QStringLiteral("?? new.cpp\n?? docs/README.md"),
                                         QString(),
                                         /*gitFailed=*/false);
    QCOMPARE(result,
             QStringLiteral("Untracked files (not part of the diff):\n"
                            "new.cpp\ndocs/README.md\n"));
}

void LlamaLocatorPromptsTest::gitDiff_withDiff()
{
    const QString diff = QStringLiteral("--- a/main.cpp\n+++ b/main.cpp\n@@ -1 +1 @@\n-int a;\n+int b;\n");
    const QString status = QStringLiteral(" M main.cpp\n?? new.cpp");
    const QString result = formatGitDiff(status, diff, /*gitFailed=*/false);
    QCOMPARE(result,
             QStringLiteral("\n```diff\n--- a/main.cpp\n+++ b/main.cpp\n"
                            "@@ -1 +1 @@\n-int a;\n+int b;\n\n```\n"
                            "Untracked files (not part of the diff):\nnew.cpp\n"));
}

void LlamaLocatorPromptsTest::gitDiff_untrackedCapped()
{
    // A repository full of untracked files must not blow up the prompt.
    QStringList status;
    for (int i = 0; i < 60; ++i)
        status << QStringLiteral("?? untracked_%1.cpp").arg(i);
    const QString result = formatGitDiff(status.join(QLatin1Char('\n')),
                                         QString(),
                                         /*gitFailed=*/false);
    const QStringList lines = result.split(QLatin1Char('\n'));
    // Header + 50 files + "... and 10 more" + trailing empty line.
    QCOMPARE(lines.size(), 53);
    QCOMPARE(lines.at(0), QStringLiteral("Untracked files (not part of the diff):"));
    QCOMPARE(lines.at(1), QStringLiteral("untracked_0.cpp"));
    QCOMPARE(lines.at(50), QStringLiteral("untracked_49.cpp"));
    QCOMPARE(lines.at(51), QStringLiteral("... and 10 more"));
}

void LlamaLocatorPromptsTest::gitDiff_truncatedAtLineBoundary()
{
    // 6000 lines of ~37 chars: ~220k chars, well over the 100k cap.
    QString diff;
    for (int i = 0; i < 6000; ++i)
        diff += QStringLiteral("+line %1 with some padding text\n").arg(i);
    QVERIFY(diff.size() > 100000);

    const QString result = formatGitDiff(QStringLiteral(" M big.cpp"),
                                         diff,
                                         /*gitFailed=*/false);
    QVERIFY(result.contains(QStringLiteral("... diff truncated (")));
    // The diff part must end on a line boundary (a newline before the fence).
    const int fence = result.indexOf(QStringLiteral("\n```\n"));
    QVERIFY(fence > 0);
    // Total length is bounded: note + capped diff + fence + no untracked list.
    QVERIFY(result.size() < 100000 + 200);
}

QTEST_MAIN(LlamaLocatorPromptsTest)
#include "llamalocator_test.moc"
