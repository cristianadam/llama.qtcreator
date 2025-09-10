#include <QJsonArray>
#include <QJsonObject>
#include <QMessageBox>
#include <coreplugin/editormanager/editormanager.h>
#include <coreplugin/editormanager/ieditor.h>
#include <coreplugin/icore.h>
#include <coreplugin/locator/ilocatorfilter.h>
#include <git/gitconstants.h>
#include <solutions/tasking/tasktree.h>
#include <texteditor/textdocument.h>
#include <texteditor/texteditor.h>
#include <utils/algorithm.h>
#include <utils/mimeutils.h>

#include "llamachatmanager.h"
#include "llamaconstants.h"
#include "llamalocatorfilter.h"
#include "llamatr.h"
#include "llamatypes.h"

using namespace Utils;
using namespace Core;
using namespace TextEditor;

namespace LlamaCpp {

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

    // built‑in prompts – keep them in a list to generate the menu
    const QList<QString> m_predefined
        = {Tr::tr("Create a summary of {selection}"),
           Tr::tr("Create a commit message for {selection}"),
           Tr::tr("Explain the code in {selection}"),
           Tr::tr("Do spell checking and fix any typos in {selection}"),
           Tr::tr("Generate test cases for {selection}. Output only code. No explanations")};

    QList<QString> m_history;
    static constexpr int MaxHistory = 50;
};

LocatorFilter::LocatorFilter()
{
    setId(Constants::LLAMACPP_LOCATOR_ID);
    setDisplayName("llama.cpp – Prompt ...");
    setDescription(Tr::tr("Send the current selection to llama.cpp with a prompt.\n"
                          "Built‑in prompts: %1\n"
                          "You can type any other prompt – they are remembered for next time.")
                       .arg(m_predefined.join(", ")));
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
            e.displayName = prompt;
            e.acceptor = [this, prompt] {
                acceptPrompt(prompt);
                return AcceptResult();
            };

            const QRegularExpressionMatch match = regexp.match(prompt);
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

        for (const QString &p : m_predefined)
            addEntry(p);
        for (const QString &p : m_history)
            addEntry(p);

        if (!m_history.contains(input) && !input.trimmed().isEmpty())
            addEntry(input);

        return storage.reportOutput(
            std::accumulate(std::begin(entries), std::end(entries), LocatorFilterEntries()));
    };
    return {Tasking::Sync(onSetup)};
}

void LocatorFilter::acceptPrompt(const QString &prompt)
{
    QString message = prompt;
    QString text;
    QString language;
    if (TextEditorWidget *editor = TextEditorWidget::currentTextEditorWidget()) {
        text = TextDocument::convertToPlainText(editor->selectedText());

        const MimeType mimeType = mimeTypeForName(editor->textDocument()->mimeType());
        language = mimeType.preferredSuffix();

        // Manually select the language for either mimetype or editor id
        if (mimeType.name() == "text/x-cmake-project")
            language = "cmake";

        if (editor->textDocument()->id() == Git::Constants::GIT_LOG_EDITOR_ID)
            language = "diff";
    }

    message.replace("{selection}", QString("\n```%1\n%2\n```\n").arg(language, text));

    Conversation c = ChatManager::instance().createConversation(prompt.left(250));
    ChatManager::instance().sendMessage(c.id, c.currNode, message, {}, [](qint64 leafId) {});

    Core::EditorManager::openEditorWithContents(Constants::LLAMACPP_VIEWER_ID,
                                                &c.name,
                                                c.id.toUtf8());

    if (!m_predefined.contains(prompt)) {
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
