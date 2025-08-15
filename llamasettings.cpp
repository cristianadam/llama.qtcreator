#include "llamasettings.h"
#include "llamaconstants.h"
#include "llamatr.h"

#include <coreplugin/dialogs/ioptionspage.h>
#include <projectexplorer/project.h>
#include <utils/layoutbuilder.h>

#include <QToolTip>

using namespace Utils;

namespace LlamaCpp::Internal {

static void initEnableAspect(BoolAspect &enableLlamaCpp)
{
    enableLlamaCpp.setSettingsKey(Constants::ENABLE_LLAMACPP);
    enableLlamaCpp.setDisplayName(Tr::tr("Enable llama.cpp"));
    enableLlamaCpp.setLabelText(Tr::tr("Enable llama.cpp"));
    enableLlamaCpp.setToolTip(Tr::tr("Enables the llama.cpp integration."));
    enableLlamaCpp.setDefaultValue(true);
}

LlamaSettings &settings()
{
    static LlamaSettings settings;
    return settings;
}

LlamaSettings::LlamaSettings()
{
    setAutoApply(false);

    endpoint.setDisplayName(Tr::tr("Endpoint"));
    endpoint.setDisplayStyle(StringAspect::LineEditDisplay);
    endpoint.setSettingsKey("LlamaCpp.Endpoint");
    endpoint.setLabelText(Tr::tr("Endpoint:"));
    endpoint.setDefaultValue("http://127.0.0.1:8012/infill");
    endpoint.setToolTip(Tr::tr("llama.cpp server endpoint"));
    endpoint.setHistoryCompleter("LlamaCpp.Endpoint.History");

    apiKey.setDisplayName(Tr::tr("API Key"));
    apiKey.setDisplayStyle(StringAspect::LineEditDisplay);
    apiKey.setSettingsKey("LlamaCpp.ApiKey");
    apiKey.setLabelText(Tr::tr("API Key:"));
    apiKey.setDefaultValue("");
    apiKey.setToolTip(Tr::tr("llama.cpp server api key (optional)"));
    apiKey.setHistoryCompleter("LlamaCpp.ApiKey.History");

    nPrefix.setDisplayName(Tr::tr("Prefix Code Lines"));
    nPrefix.setSettingsKey("LlamaCpp.NPrefix");
    nPrefix.setLabelText(Tr::tr("Prefix Code Lines:"));
    nPrefix.setDefaultValue(256);
    nPrefix.setToolTip(
        Tr::tr("Number of code lines before the cursor location to include in the local prefix."));
    nPrefix.setRange(0, 65535);

    nSuffix.setDisplayName(Tr::tr("Suffix Code Lines"));
    nSuffix.setSettingsKey("LlamaCpp.NSuffix");
    nSuffix.setLabelText(Tr::tr("Suffix Code Lines:"));
    nSuffix.setDefaultValue(64);
    nSuffix.setToolTip(
        Tr::tr("Number of code lines after  the cursor location to include in the local suffix."));
    nSuffix.setRange(0, 65535);

    nPredict.setDisplayName(Tr::tr("Max Token Predictions"));
    nPredict.setSettingsKey("LlamaCpp.NPredict");
    nPredict.setLabelText(Tr::tr("Max Token Predictions:"));
    nPredict.setDefaultValue(128);
    nPredict.setToolTip(Tr::tr("Max number of tokens to predict."));
    nPredict.setRange(0, 65535);

    stopStrings.setDisplayName(Tr::tr("Stop Strings"));
    stopStrings.setDisplayStyle(StringAspect::LineEditDisplay);
    stopStrings.setSettingsKey("LlamaCpp.StopStrings");
    stopStrings.setLabelText(Tr::tr("Stop Strings:"));
    stopStrings.setDefaultValue("");
    stopStrings.setToolTip(Tr::tr("Return the result immediately as soon as any of these strings "
                                  "are encountered in the generated text. Separated by semicolons."));
    stopStrings.setHistoryCompleter("LlamaCpp.StopStrings.History");

    tMaxPromptMs.setDisplayName(Tr::tr("Max Prompt Time (ms)"));
    tMaxPromptMs.setSettingsKey("LlamaCpp.TMaxPromptMs");
    tMaxPromptMs.setLabelText(Tr::tr("Max Prompt Time (ms):"));
    tMaxPromptMs.setDefaultValue(500);
    tMaxPromptMs.setToolTip(
        Tr::tr("Max alloted time for the prompt processing (TODO: not yet supported)."));
    tMaxPromptMs.setRange(0, 65535);

    tMaxPredictMs.setDisplayName(Tr::tr("Max Predict Time (ms)"));
    tMaxPredictMs.setSettingsKey("LlamaCpp.TMaxPredictMs");
    tMaxPredictMs.setLabelText(Tr::tr("Max Predict Time (ms):"));
    tMaxPredictMs.setDefaultValue(1000);
    tMaxPredictMs.setToolTip(Tr::tr("Max alloted time for the prediction."));
    tMaxPredictMs.setRange(0, 65535);

    showInfo.setDisplayName(Tr::tr("Show Info"));
    showInfo.setSettingsKey("LlamaCpp.ShowInfo");
    showInfo.setLabelText(Tr::tr("Show Info:"));
    showInfo.setDefaultValue(2);
    showInfo.setToolTip(
        Tr::tr("Show extra info about the inference (0 - disabled, 1 - statusline, 2 - inline)."));
    showInfo.setRange(0, 2);

    autoFim.setDisplayName(Tr::tr("Auto FIM"));
    autoFim.setSettingsKey("LlamaCpp.AutoFim");
    autoFim.setLabelText(Tr::tr("Auto FIM"));
    autoFim.setDefaultValue(true);
    autoFim.setToolTip(Tr::tr("Trigger FIM (Fill-in-the-Middle) completion automatically on cursor movement."));

    maxLineSuffix.setDisplayName(Tr::tr("Max Line Suffix"));
    maxLineSuffix.setSettingsKey("LlamaCpp.MaxlineSuffix");
    maxLineSuffix.setLabelText(Tr::tr("Max Line Suffix:"));
    maxLineSuffix.setDefaultValue(8);
    maxLineSuffix.setToolTip(Tr::tr("Do not auto-trigger FIM completion if there are more than "
                                    "this number of characters to the right of the cursor."));
    maxLineSuffix.setRange(0, 65535);

    maxCacheKeys.setDisplayName(Tr::tr("Max Cache Keys"));
    maxCacheKeys.setSettingsKey("LlamaCpp.MaxCacheKeys");
    maxCacheKeys.setLabelText(Tr::tr("Max Cache Keys:"));
    maxCacheKeys.setDefaultValue(250);
    maxCacheKeys.setToolTip(Tr::tr("Max number of cached completions to keep in result_cache."));
    maxCacheKeys.setRange(0, 65535);

    ringNChunks.setDisplayName(Tr::tr("Ring Chunks"));
    ringNChunks.setSettingsKey("LlamaCpp.RingNChunks");
    ringNChunks.setLabelText(Tr::tr("Ring Chunks:"));
    ringNChunks.setDefaultValue(16);
    ringNChunks.setToolTip(
        Tr::tr("Max number of chunks to pass as extra context to the server (0 to disable)."));
    ringNChunks.setRange(0, 65535);

    ringChunkSize.setDisplayName(Tr::tr("Chunk Line Size"));
    ringChunkSize.setSettingsKey("LlamaCpp.RingChunkSize");
    ringChunkSize.setLabelText(Tr::tr("Chunk Line Size:"));
    ringChunkSize.setDefaultValue(64);
    ringChunkSize.setToolTip(Tr::tr(
        "Max size of the chunks (in number of lines).<br/><br/>Note: adjust these numbers so that "
        "you don't overrun your context. At ring_n_chunks = 64 and ring_chunk_size = 64 you "
        "need ~32k context."));
    ringChunkSize.setRange(0, 65535);

    ringScope.setDisplayName(Tr::tr("Ring Line Scope"));
    ringScope.setSettingsKey("LlamaCpp.RingScope");
    ringScope.setLabelText(Tr::tr("Ring Line Scope:"));
    ringScope.setDefaultValue(1024);
    ringScope.setToolTip(Tr::tr("The range around the cursor position (in number of lines) for "
                                "gathering chunks after FIM."));
    ringScope.setRange(0, 65535);

    ringUpdateMs.setDisplayName(Tr::tr("Update Interval (ms)"));
    ringUpdateMs.setSettingsKey("LlamaCpp.RingUpdateMs");
    ringUpdateMs.setLabelText(Tr::tr("Update Interval (ms):"));
    ringUpdateMs.setDefaultValue(100);
    ringUpdateMs.setToolTip(Tr::tr("How often to process queued chunks in normal mode."));
    ringUpdateMs.setRange(0, 65535);

    initEnableAspect(enableLlamaCpp);

    readSettings();

    endpoint.setEnabler(&enableLlamaCpp);
    apiKey.setEnabler(&enableLlamaCpp);
    nPrefix.setEnabler(&enableLlamaCpp);
    nSuffix.setEnabler(&enableLlamaCpp);
    nPredict.setEnabler(&enableLlamaCpp);
    stopStrings.setEnabler(&enableLlamaCpp);
    tMaxPromptMs.setEnabler(&enableLlamaCpp);
    tMaxPredictMs.setEnabler(&enableLlamaCpp);
    showInfo.setEnabler(&enableLlamaCpp);
    autoFim.setEnabler(&enableLlamaCpp);
    maxLineSuffix.setEnabler(&enableLlamaCpp);
    maxCacheKeys.setEnabler(&enableLlamaCpp);
    ringNChunks.setEnabler(&enableLlamaCpp);
    ringChunkSize.setEnabler(&enableLlamaCpp);
    ringScope.setEnabler(&enableLlamaCpp);
    ringUpdateMs.setEnabler(&enableLlamaCpp);

    setLayouter([this] {
        using namespace Layouting;

        // clang-format off
        return Column {
            Form {
                enableLlamaCpp, br,
                br,endpoint, br,
                br,apiKey, br,
                br,nPrefix, br,
                br,nSuffix, br,
                br,nPredict, br,
                br,stopStrings, br,
                br,tMaxPromptMs, br,
                tMaxPredictMs, br,
                showInfo, br,
                autoFim, br,
                maxLineSuffix, br,
                maxCacheKeys, br,
                hr, br,
                ringNChunks, br,
                ringChunkSize, br,
                ringScope, br,
                ringUpdateMs, br,
            },
            st
        };
        // clang-format on
    });
}

LlamaProjectSettings::LlamaProjectSettings(ProjectExplorer::Project *project)
{
    setAutoApply(true);

    useGlobalSettings.setSettingsKey(Constants::LLAMACPP_USE_GLOBAL_SETTINGS);
    useGlobalSettings.setDefaultValue(true);

    initEnableAspect(enableLlamaCpp);

    Store map = storeFromVariant(project->namedSettings(Constants::LLAMACPP_PROJECT_SETTINGS_ID));
    fromMap(map);

    enableLlamaCpp.addOnChanged(this, [this, project] { save(project); });
    useGlobalSettings.addOnChanged(this, [this, project] { save(project); });
}

void LlamaProjectSettings::setUseGlobalSettings(bool useGlobal)
{
    useGlobalSettings.setValue(useGlobal);
}

bool LlamaProjectSettings::isEnabled() const
{
    if (useGlobalSettings())
        return settings().enableLlamaCpp();
    return enableLlamaCpp();
}

void LlamaProjectSettings::save(ProjectExplorer::Project *project)
{
    Store map;
    toMap(map);
    project->setNamedSettings(Constants::LLAMACPP_PROJECT_SETTINGS_ID, variantFromStore(map));

    settings().apply();
}

class LlamaCppSettingsPage : public Core::IOptionsPage
{
public:
    LlamaCppSettingsPage()
    {
        setId(Constants::LLAMACPP_GENERAL_OPTIONS_ID);
        setDisplayName("llama.cpp");
        setCategory(Constants::LLAMACPP_GENERAL_OPTIONS_CATEGORY);
        setSettingsProvider([] { return &settings(); });
    }
};

const LlamaCppSettingsPage settingsPage;

} // namespace LlamaCpp::Internal
