// The plugin's options pages. They live in their own translation unit
// because the FIM and Chat pages need ChatManager (the model combos list
// its model list), which the test targets that compile llamasettings.cpp
// do not link.
#include "llamasettings.h"
#include "llamaconstants.h"
#include "llamachatmanager.h"
#include "llamatr.h"
#include "promptssettingswidget.h"
#include "sandboxsettingswidget.h"
#include "serversettingswidget.h"
#include "skillssettingswidget.h"
#include "toolsettingswidget.h"

#include <coreplugin/dialogs/ioptionspage.h>
#include <utils/guiutils.h>
#include <utils/layoutbuilder.h>

#include <QComboBox>
#include <QFont>
#include <QLabel>
#include <QLineEdit>

using namespace Utils;

namespace LlamaCpp {

ToolsSettingsPage::ToolsSettingsPage()
{
    setId(Constants::LLAMACPP_TOOLS_OPTIONS_ID); // unique identifier
    setDisplayName(Tr::tr("Tools"));             // title shown in the Settings dialog
    setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
    setSettingsProvider([] { return &LlamaCpp::settings(); });
    setWidgetCreator([] { return new LlamaCpp::ToolsSettingsWidget; });
}

SkillsSettingsPage::SkillsSettingsPage()
{
    setId(Constants::LLAMACPP_SKILLS_OPTIONS_ID); // unique identifier
    setDisplayName(Tr::tr("Skills"));             // title shown in the Settings dialog
    setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
    setSettingsProvider([] { return &LlamaCpp::settings(); });
    setWidgetCreator([] { return new LlamaCpp::SkillsSettingsWidget; });
}

SandboxSettingsPage::SandboxSettingsPage()
{
    setId(Constants::LLAMACPP_SANDBOX_OPTIONS_ID); // unique identifier
    setDisplayName(Tr::tr("Sandbox"));             // title shown in the Settings dialog
    setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
    setSettingsProvider([] { return &LlamaCpp::settings(); });
    setWidgetCreator([] { return new LlamaCpp::SandboxSettingsWidget; });
}

ServerSettingsPage::ServerSettingsPage()
{
    setId(Constants::LLAMACPP_SERVER_OPTIONS_ID); // unique identifier
    setDisplayName(Tr::tr("Server"));             // title shown in the Settings dialog
    setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
    setSettingsProvider([] { return &LlamaCpp::settings(); });
    setWidgetCreator([] { return new LlamaCpp::ServerSettingsWidget; });
}

// The static instances make the pages appear automatically.
const ToolsSettingsPage toolsSettingsPage;
const SkillsSettingsPage skillsSettingsPage;
const SandboxSettingsPage sandboxSettingsPage;
const ServerSettingsPage serverSettingsPage;

// The three settings pages that replace the former single "llama.cpp" page
// (its three columns): FIM, Chat and Web Search. Each page embeds its
// aspects in a plain IOptionsPageWidget; a settings-provider page would
// render the container's single layouter on every page, so a widget creator
// is needed for per-page layouts. The FIM page carries the "Enable FIM"
// toggle; it gates only the FIM aspects, so the chat and web search pages
// keep working with FIM turned off.
// Applying or cancelling a page acts on the whole shared settings
// container, which is what the old single page did.
namespace {

// An editable combo bound to a StringAspect: the rest of the entries are
// the server's models (ChatManager, the chat endpoint) with a status suffix
// while not loaded, refreshed on modelsUpdated(). The empty ("default")
// value is shown as the line edit's placeholder, not as an item text; the
// first entry exists only to select it back from the popup. It stays
// editable so a typed name still works - the FIM endpoint may point at a
// different server, and the list is empty while the server is down.
class ModelComboBox : public QComboBox
{
public:
    ModelComboBox(StringAspect *aspect, const QString &defaultText, QWidget *parent = nullptr)
        : QComboBox(parent)
        , m_aspect(aspect)
    {
        setEditable(true);
        setToolTip(m_aspect->toolTip());
        // The empty value has no text of its own: the placeholder stands in
        // for it, so the display text can never be saved as the value.
        lineEdit()->setPlaceholderText(defaultText);
        addItem(defaultText, QString());
        updateModels();
        connect(this, qOverload<int>(&QComboBox::activated), this, [this](int) {
            const QString value = currentData().toString();
            m_aspect->setVolatileValue(value);
            // QComboBox sets the line edit to the item text; an empty value
            // must show the placeholder instead (syncFromAspect() is skipped
            // while the line edit has focus).
            if (value.isEmpty())
                lineEdit()->setText(QString());
        });
        connect(lineEdit(), &QLineEdit::editingFinished, this, [this] {
            const int idx = findText(currentText());
            // The default entry's text stands in for the empty value; a
            // focus-out with it must keep the current value, not clear it.
            const QString value =
                    idx == 0 ? m_aspect->volatileValue()
                    : idx > 0 ? currentData().toString()
                              : currentText().trimmed();
            m_aspect->setVolatileValue(value);
        });
        m_aspect->addOnVolatileValueChanged(this, [this] { syncFromAspect(); });
        connect(&ChatManager::instance(), &ChatManager::modelsUpdated, this, [this] {
            updateModels();
        });
    }

private:
    void updateModels()
    {
        while (count() > 1)
            removeItem(1);
        for (const auto &m : ChatManager::instance().models()) {
            addItem(m.id, m.id);
            if (m.status == QLatin1String("loaded")) {
                // Loaded models are bold (the list updates live when a load
                // finishes).
                QFont itemFont = this->font();
                itemFont.setBold(true);
                setItemData(count() - 1, itemFont, Qt::FontRole);
            }
        }
        syncFromAspect(/* force */ true);
    }

    void syncFromAspect(bool force = false)
    {
        // The user is typing a not-yet-committed name: leave the text
        // alone, unless force (after updateModels() the text is an
        // artifact of the wipe, never user input).
        if (!force && lineEdit()->hasFocus() && lineEdit()->text() != m_aspect->volatileValue())
            return;
        const QString value = m_aspect->volatileValue();
        // A picked model not in the server's list (yet) stays selectable,
        // unbolded like an unloaded one.
        int idx = findData(value);
        if (idx < 0 && !value.isEmpty()) {
            addItem(value, value);
            idx = count() - 1;
        }
        if (idx >= 0 && idx != currentIndex())
            setCurrentIndex(idx);
        // The line edit shows the value itself (the item texts equal their
        // data); an empty one shows the placeholder. setCurrentIndex() would
        // reset it to the default entry's text, so set it explicitly.
        if (lineEdit()->text() != value)
            lineEdit()->setText(value);
    }

    StringAspect *m_aspect;
};

void setupLlamaSettingsPage(Core::IOptionsPageWidget *widget)
{
    widget->setOnApply([] {
        settings().apply();
        settings().writeSettings();
    });
    widget->setOnCancel([] { settings().cancel(); });
    widget->setDirtyChecker([] { return settings().isDirty(); });
}

Core::IOptionsPageWidget *createFimSettingsWidget()
{
    using namespace Layouting;
    auto &s = settings();

    auto *widget = new Core::IOptionsPageWidget;

    // The FIM model row: a ModelComboBox bound to the modelFim aspect (the
    // aspect's own line edit is not used). setEnabler only affects the
    // aspect's own editor, so the "Enable FIM" state is mirrored manually.
    auto *fimModelLabel = new QLabel(s.modelFim.labelText(), widget);
    auto *fimModelCombo = new ModelComboBox(&s.modelFim, Tr::tr("Default"), widget);
    fimModelLabel->setBuddy(fimModelCombo);
    auto updateFimModelRowEnabled = [fimModelLabel, fimModelCombo] {
        const bool on = settings().enableFim.volatileValue();
        fimModelLabel->setEnabled(on);
        fimModelCombo->setEnabled(on);
    };
    s.enableFim.addOnVolatileValueChanged(widget, updateFimModelRowEnabled);
    updateFimModelRowEnabled();

    // clang-format off
    Column {
        s.enableFim, br,
        Row {s.endpoint}, br,
        Row {s.apiKey}, br,
        Row {fimModelLabel, fimModelCombo, st}, br,
        Row {s.nPrefix, st}, br,
        Row {s.nSuffix, st}, br,
        Row {s.nPredict, st}, br,
        Row {s.nCmpl, st}, br,
        Row {s.stopStrings}, br,
        Row {s.tMaxPromptMs, st}, br,
        Row {s.tMaxPredictMs, st}, br,
        Row {s.showInfo, st}, br,
        Row {s.autoFim}, br,
        Row {s.maxLineSuffix, st}, br,
        Row {s.maxCacheKeys, st}, br,
        hr, br,
        Row {s.ringNChunks, st}, br,
        Row {s.ringChunkSize, st}, br,
        Row {s.ringScope, st}, br,
        Row {s.ringUpdateMs, st}, br,
        st
    }.attachTo(widget);
    // clang-format on

    setupLlamaSettingsPage(widget);
    return widget;
}

Core::IOptionsPageWidget *createChatSettingsWidget()
{
    using namespace Layouting;
    auto &s = settings();

    auto *widget = new Core::IOptionsPageWidget;

    // The chat model row: a ModelComboBox bound to the lastSelectedModel
    // aspect (the aspect's own line edit is not used); applying the page
    // selects and loads the model (see ChatManager).
    auto *chatModelLabel = new QLabel(s.lastSelectedModel.labelText(), widget);
    auto *chatModelCombo =
            new ModelComboBox(&s.lastSelectedModel, Tr::tr("No model"), widget);
    chatModelLabel->setBuddy(chatModelCombo);

    // The utility model row: a ModelComboBox bound to the utilityModel
    // aspect (the aspect's own line edit is not used).
    auto *utilityModelLabel = new QLabel(s.utilityModel.labelText(), widget);
    auto *utilityModelCombo =
            new ModelComboBox(&s.utilityModel, Tr::tr("Default: chat model"), widget);
    utilityModelLabel->setBuddy(utilityModelCombo);

    // clang-format off
    Row {
        Column {
            Row {s.chatEndpoint}, br,
            Row {s.chatApiKey}, br,
            s.systemMessage, br,
            Row {chatModelLabel, chatModelCombo, st}, br,
            Row {utilityModelLabel, utilityModelCombo, st}, br,
            Row {s.temperature, st}, br,
            Row {s.top_k, st}, br,
            Row {s.top_p, st}, br,
            Row {s.min_p, st}, br,
            Row {s.max_tokens, st}, br,
            Row {s.pasteLongTextToFileLen, st}, br,
            hr, br,
            Row {s.samplers}, br,
            Row {s.dynatemp_range, st}, br,
            Row {s.dynatemp_exponent, st}, br,
            Row {s.typical_p, st}, br,
            Row {s.xtc_probability, st}, br,
            Row {s.xtc_threshold, st}, br,
            hr, br,
            Row {s.repeat_last_n, st}, br,
            Row {s.repeat_penalty, st}, br,
            Row {s.presence_penalty, st}, br,
            Row {s.frequency_penalty, st}, br,
            Row {s.dry_multiplier, st}, br,
            Row {s.dry_base, st}, br,
            Row {s.dry_allowed_length, st}, br,
            Row {s.dry_penalty_last_n, st}, br,
            st
        },
        Column {
            s.toolsEnabled, br,
            s.selectionPromptButton, br,
            Row {s.autoHandoff}, br,
            Row {s.handoffThreshold, st}, br,
            s.customJson, br,
            st
        }
    }.attachTo(widget);
    // clang-format on

    setupLlamaSettingsPage(widget);
    return widget;
}

Core::IOptionsPageWidget *createWebSearchSettingsWidget()
{
    using namespace Layouting;
    auto &s = settings();

    auto *widget = new Core::IOptionsPageWidget;

    auto *providerCombo = new QComboBox(widget);
    providerCombo->addItems({QStringLiteral("duckduckgo"),
                             QStringLiteral("exa"),
                             QStringLiteral("google"),
                             QStringLiteral("brave"),
                             QStringLiteral("tavily")});
    providerCombo->setCurrentIndex(qMax(0, providerCombo->findText(s.webSearchProvider())));
    QObject::connect(providerCombo,
                     &QComboBox::currentTextChanged,
                     widget,
                     [combo = providerCombo](const QString &text) {
                         settings().webSearchProvider.setValue(text);
                     });
    s.webSearchProvider.addOnChanged(providerCombo, [combo = providerCombo]() {
        combo->setCurrentIndex(qMax(0, combo->findText(settings().webSearchProvider())));
    });

    // clang-format off
    Column {
        Row {new QLabel(s.webSearchProvider.labelText(), widget),
             providerCombo,
             new QLabel(Tr::tr("\"duckduckgo\" needs no API key"), widget),
             st}, br,
        hr, br,
        Row {s.webSearchExaUrl}, br,
        Row {s.webSearchExaApiKey}, br,
        hr, br,
        Row {s.webSearchGoogleUrl}, br,
        Row {s.webSearchGoogleApiKey}, br,
        Row {s.webSearchGoogleCx}, br,
        hr, br,
        Row {s.webSearchBraveUrl}, br,
        Row {s.webSearchBraveApiKey}, br,
        hr, br,
        Row {s.webSearchTavilyUrl}, br,
        Row {s.webSearchTavilyApiKey}, br,
        st
    }.attachTo(widget);
    // clang-format on

    setupLlamaSettingsPage(widget);
    return widget;
}

} // namespace

class LlamaFimSettingsPage : public Core::IOptionsPage
{
public:
    LlamaFimSettingsPage()
    {
        setId(Constants::LLAMACPP_FIM_OPTIONS_ID); // unique identifier
        setDisplayName(Tr::tr("FIM"));             // title shown in the Settings dialog
        setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
        setSettingsProvider([] { return &LlamaCpp::settings(); });
        setWidgetCreator(createFimSettingsWidget);
    }
};

const LlamaFimSettingsPage fimSettingsPage;

class LlamaChatSettingsPage : public Core::IOptionsPage
{
public:
    LlamaChatSettingsPage()
    {
        setId(Constants::LLAMACPP_CHAT_OPTIONS_ID); // unique identifier
        setDisplayName(Tr::tr("Chat"));             // title shown in the Settings dialog
        setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
        setSettingsProvider([] { return &LlamaCpp::settings(); });
        setWidgetCreator(createChatSettingsWidget);
    }
};

const LlamaChatSettingsPage chatSettingsPage;

class LlamaWebSearchSettingsPage : public Core::IOptionsPage
{
public:
    LlamaWebSearchSettingsPage()
    {
        setId(Constants::LLAMACPP_WEBSEARCH_OPTIONS_ID); // unique identifier
        setDisplayName(Tr::tr("Web Search"));            // title shown in the Settings dialog
        setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
        setSettingsProvider([] { return &LlamaCpp::settings(); });
        setWidgetCreator(createWebSearchSettingsWidget);
    }
};

const LlamaWebSearchSettingsPage webSearchSettingsPage;

class LlamaPromptsPage : public Core::IOptionsPage
{
public:
    LlamaPromptsPage()
    {
        setId(Constants::LLAMACPP_PROMPTS_OPTIONS_ID);
        setDisplayName(Tr::tr("Prompts"));
        setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
        setSettingsProvider([] { return &settings(); });
        setWidgetCreator([] { return new PromptsSettingsWidget; });
    }
};

const LlamaPromptsPage promptsPage;

} // namespace LlamaCpp
