#include <QJsonArray>
#include <QFileInfo>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QtConcurrent>
#include <QtTaskTree/QTaskTree>

#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/icore.h>
#include <coreplugin/locator/ilocatorfilter.h>
#include <coreplugin/progressmanager/progressmanager.h>
#include <coreplugin/progressmanager/futureprogress.h>
#include <git/gitconstants.h>
#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>
#include <texteditor/textmark.h>
#include <utils/algorithm.h>
#include <utils/id.h>
#include <utils/mimeutils.h>

#include "llamachatmanager.h"
#include "llamaconstants.h"
#include "llamalocatorfilter.h"
#include "llamalocatorprompts.h"
#include "llamasettings.h"
#include "llamatr.h"
#include "llamatypes.h"

using namespace Utils;
using namespace Core;
using namespace TextEditor;

namespace LlamaCpp {

namespace {

// Bookmarks are TextMarks as well, but they are not diagnostics. The category
// constant lives in the TextEditor plugin's bookmark.cpp and is not exported,
// so it is repeated here.
constexpr char kBookmarkCategory[] = "Bookmarks.TextMarkCategory";

QList<LocatorDiagnostic> collectDiagnostics(const TextDocument *document)
{
    QList<LocatorDiagnostic> result;
    const TextMarks marks = document->marks();
    for (TextMark *mark : marks) {
        if (!mark || !mark->isVisible())
            continue;
        const TextMarkCategory &category = mark->category();
        if (category.id == Utils::Id(kBookmarkCategory))
            continue;

        // The line annotation carries the diagnostic message where the code
        // model provides one (clangd); the tooltip does so elsewhere (QML,
        // build tasks). Either may be the generic "Code Model Error/Warning".
        QString message = mark->lineAnnotation().trimmed();
        if (message.isEmpty())
            message = mark->toolTip().trimmed();
        QString text = category.displayName;
        if (!message.isEmpty() && message != text)
            text += QLatin1Char(' ') + message;
        if (text.trimmed().isEmpty())
            continue;
        result.append({mark->lineNumber(), text});
    }
    return result;
}

bool runGit(const QString &workingDirectory, const QStringList &arguments, QString *output)
{
    QProcess process;
    process.setWorkingDirectory(workingDirectory);
    process.start(QLatin1String("git"), arguments);
    if (!process.waitForStarted(5000) || !process.waitForFinished(10000)) {
        process.kill();
        return false;
    }
    *output = QString::fromUtf8(process.readAllStandardOutput());
    return process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

QString collectGitDiff(const QString &path)
{
    if (path.isEmpty())
        return QStringLiteral("(no file is open, so there is no repository to diff)");

    // git locates the repository root from the working directory itself.
    const QString dir = QFileInfo(path).absolutePath();

    QString status;
    if (!runGit(dir, {QLatin1String("--no-pager"),
                      QLatin1String("status"),
                      QLatin1String("--porcelain")},
                &status))
        return formatGitDiff(QString(), QString(), /*gitFailed=*/true);

    QString diff;
    bool gitFailed = false;
    if (!runGit(dir, {QLatin1String("--no-pager"),
                      QLatin1String("diff"),
                      QLatin1String("HEAD"),
                      QLatin1String("--no-color")},
                &diff)) {
        // Distinguish "no HEAD yet" (empty repository) from a real failure.
        static const QString kEmptyTree = QLatin1String(
            "4b825dc642cb6eb9a060e54bf8d69288fbee4904");
        QString unused;
        const bool hasHead = runGit(dir, {QLatin1String("rev-parse"),
                                          QLatin1String("--verify"),
                                          QLatin1String("--quiet"),
                                          QLatin1String("HEAD")},
                                    &unused);
        if (hasHead) {
            gitFailed = true;
        } else {
            // No HEAD yet (empty repository): show the staged changes against
            // the empty tree instead.
            gitFailed = !runGit(dir, {QLatin1String("--no-pager"),
                                      QLatin1String("diff"),
                                      QLatin1String("--no-color"),
                                      QLatin1String("--cached"),
                                      kEmptyTree},
                                &diff);
        }
    }
    return formatGitDiff(status, diff, gitFailed);
}

// Opens (or raises) the chat editor of \a conversation. Non-const: the
// editor's title pattern takes a pointer to the name that must outlive the
// call, which is only true for a non-const object.
void openConversationEditor(Conversation &c)
{
    Core::EditorManager::openEditorWithContents(Constants::LLAMACPP_VIEWER_ID,
                                                &c.name,
                                                c.id.toUtf8(),
                                                c.id);
}

} // namespace

class LocatorFilter final : public Core::ILocatorFilter
{
    Q_OBJECT
public:
    explicit LocatorFilter();

    Core::LocatorMatcherTasks matchers() final;

    void saveState(QJsonObject &object) const final;
    void restoreState(const QJsonObject &object) final;

private:
    void acceptPrompt(const QString &prompt);

    // Built‑in prompts, user‑editable through the "Prompts" settings page.
    // Read from the settings on every call so that changes made on that page
    // take effect without a restart.
    QStringList predefinedPrompts() const;

    QList<QString> m_history;
    static constexpr int MaxHistory = 50;
};

QStringList LocatorFilter::predefinedPrompts() const
{
    const QStringList fromSettings = settings().locatorPrompts.value();
    if (!fromSettings.isEmpty())
        return fromSettings;
    return defaultLocatorPrompts();
}

LocatorFilter::LocatorFilter()
{
    setId(Constants::LLAMACPP_LOCATOR_ID);
    setDisplayName("llama.cpp – Prompt ...");
    setDescription(Tr::tr("Send the current selection to llama.cpp with a prompt.\n"
                          "Built‑in prompts: %1\n"
                          "You can type any other prompt – they are remembered for next time.")
                       .arg(transform(predefinedPrompts(),
                                      [](const QString &p) {
                                          return p.section(QLatin1Char('\n'), 0, 0).trimmed();
                                      })
                                .join(", ")));
    setDefaultShortcutString("ll");
    setPriority(High);
    setHidden(false);
}

LocatorMatcherTasks LocatorFilter::matchers()
{
    const auto onSetup = [this]() {
        // Build a list of entries for the built‑in prompts + history
        const LocatorStorage &storage = *LocatorStorage::storage();
        const QString input = storage.input();

        const QRegularExpression regexp
            = ILocatorFilter::createRegExp(input, ILocatorFilter::caseSensitivity(input));
        if (!regexp.isValid())
            return;
        LocatorFilterEntries entries[int(ILocatorFilter::MatchLevel::Count)];

        auto addEntry = [&](const QString &prompt) {
            LocatorFilterEntry e;
            // Only the top line is displayed in the menu; the full prompt is
            // what gets sent to the model (see the "Prompts" settings page).
            e.displayName = prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
            e.acceptor = [this, prompt] {
                acceptPrompt(prompt);
                return AcceptResult();
            };

            const QRegularExpressionMatch match = regexp.match(e.displayName);
            if (match.hasMatch()) {
                e.highlightInfo = ILocatorFilter::highlightInfo(match);
                if (match.capturedStart() == 0)
                    entries[int(ILocatorFilter::MatchLevel::Best)].append(e);
                else if (match.lastCapturedIndex() == 1)
                    entries[int(ILocatorFilter::MatchLevel::Better)].append(e);
                else
                    entries[int(ILocatorFilter::MatchLevel::Good)].append(e);
            }
        };

        for (const QString &p : predefinedPrompts())
            addEntry(p);
        for (const QString &p : m_history)
            addEntry(p);

        if (!m_history.contains(input) && !input.trimmed().isEmpty())
            addEntry(input);

        return storage.reportOutput(
            std::accumulate(std::begin(entries), std::end(entries), LocatorFilterEntries()));
    };
    return {QtTaskTree::QSyncTask(onSetup)};
}

void LocatorFilter::acceptPrompt(const QString &prompt)
{
    LocatorPlaceholders values;
    QString text;
    QString language;
    QString path;
    int startLine = 0;
    int endLine = 0;
    if (TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget()) {
        text = TextDocument::convertToPlainText(editor->selectedText());

        const MimeType mimeType = mimeTypeForName(editor->textDocument()->mimeType());
        language = mimeType.preferredSuffix();

        // Manually select the language for either mimetype or editor id
        if (mimeType.name() == "text/x-cmake-project")
            language = "cmake";

        if (editor->textDocument()->id() == Git::Constants::GIT_LOG_EDITOR_ID)
            language = "diff";

        path = editor->textDocument()->filePath().nativePath();
        const QTextDocument *doc = editor->document();
        const QTextCursor cursor = editor->textCursor();
        if (!text.isEmpty()) {
            // blockNumber() is 0‑based; +1 for the 1‑based line the model sees.
            startLine = doc->findBlock(cursor.selectionStart()).blockNumber() + 1;
            endLine = doc->findBlock(cursor.selectionEnd() - 1).blockNumber() + 1;
        }

        values.file = path;
        values.line = QString::number(doc->findBlock(cursor.position()).blockNumber() + 1);
        if (prompt.contains(QLatin1String("{diagnostics}")))
            values.diagnostics = formatDiagnostics(collectDiagnostics(editor->textDocument()));
    }

    // The fence info string carries the file location (path + line range) so
    // the model knows where the selection comes from; it degrades to just the
    // language when there is no path. Without a selection the {selection}
    // placeholder simply becomes an empty string.
    QString info = language;
    if (!path.isEmpty())
        info += QLatin1Char(' ') + path;
    if (startLine > 0)
        info += QStringLiteral(":%1-%2").arg(startLine).arg(endLine);
    if (!text.isEmpty())
        values.selection = QString("\n```%1\n%2\n```\n").arg(info, text);

    // Without a text editor (or a file path) the {file}/{diagnostics}
    // placeholders would become an empty string and the prompt would read
    // "Fix the errors in ." – give the model an explicit hint instead (like
    // {git_diff} does).
    if (values.file.isEmpty() && prompt.contains(QLatin1String("{file}")))
        values.file = QStringLiteral("(no file is open)");
    if (values.diagnostics.isEmpty() && prompt.contains(QLatin1String("{diagnostics}")))
        values.diagnostics = QStringLiteral(
            "(no file is open, so there are no errors or warnings)");

    // The conversation is named after the (single‑line) prompt as displayed
    // in the locator menu, not after the full multi‑line prompt text.
    const QString name = prompt.section(QLatin1Char('\n'), 0, 0).trimmed().left(250);

    if (!prompt.contains(QLatin1String("{git_diff}"))) {
        // Everything the prompt needs is in memory: no background work.
        Conversation c = ChatManager::instance().createConversation(name);
        ChatManager::instance().sendMessage(c.id,
                                            c.currNode,
                                            applyLocatorPlaceholders(prompt, values),
                                            {},
                                            [](qint64) {});
        openConversationEditor(c);
    } else {
        // `git status`/`git diff` can take a while in large repositories, so
        // the diff is collected in the background with a status bar progress
        // bar (the same ProgressManager::addTask pattern as the CMake presets
        // kit creation). The conversation is created up front so that clicking
        // the progress bar can already open it; the message is sent – and the
        // editor opened – once the diff is in, which is where the flow ends
        // when the user does not click.
        Conversation c = ChatManager::instance().createConversation(name);
        const QFuture<QString> future =
                QtConcurrent::run([path] { return collectGitDiff(path); });
        FutureProgress *progress = ProgressManager::addTask(
                future,
                Tr::tr("Collecting git changes"),
                Utils::Id("LlamaCpp.LocatorPrompt"));
        connect(progress, &FutureProgress::clicked, this, [c = c] () mutable {
            if (ChatManager::instance().conversationExists(c.id))
                openConversationEditor(c);
        });
        connect(progress, &FutureProgress::finished, this,
                [future, prompt, values, c] () mutable {
            // The conversation may have been deleted while the diff was being
            // collected: sending to it would create an orphan message and a
            // generation on an empty history.
            if (!ChatManager::instance().conversationExists(c.id))
                return;
            values.gitDiff = future.result();
            ChatManager::instance().sendMessage(c.id,
                                                c.currNode,
                                                applyLocatorPlaceholders(prompt, values),
                                                {},
                                                [](qint64) {});
            openConversationEditor(c);
        });
    }

    if (!predefinedPrompts().contains(prompt)) {
        m_history.removeAll(prompt);
        m_history.prepend(prompt);
        while (m_history.size() > MaxHistory)
            m_history.removeLast();
    }
}

void LocatorFilter::saveState(QJsonObject &object) const
{
    object["history"] = QJsonArray::fromStringList(m_history);
}

void LocatorFilter::restoreState(const QJsonObject &object)
{
    if (object.contains("history") && object["history"].isArray())
        m_history = Utils::transform(object["history"].toArray().toVariantList(),
                                     &QVariant::toString);
}

void setupLocatorFilter()
{
    static LocatorFilter theFilter;
}

} // namespace LlamaCpp

#include "llamalocatorfilter.moc"
