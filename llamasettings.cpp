#include "llamasettings.h"
#include "llamaconstants.h"
#include "llamatr.h"

#include <coreplugin/dialogs/ioptionspage.h>
#include <projectexplorer/project.h>
#include <utils/layoutbuilder.h>

#include <QToolTip>

using namespace Utils;

namespace LlamaCpp {

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
    setSettingsGroup(Constants::LLAMA_SETTINGS_GROUP);
    setAutoApply(false);

    //
    // FIM
    //

    endpoint.setDisplayName(Tr::tr("Endpoint"));
    endpoint.setDisplayStyle(StringAspect::LineEditDisplay);
    endpoint.setSettingsKey("Endpoint");
    endpoint.setLabelText(Tr::tr("Endpoint:"));
    endpoint.setDefaultValue("http://127.0.0.1:8012/infill");
    endpoint.setToolTip(Tr::tr("llama.cpp server endpoint"));
    endpoint.setHistoryCompleter("LlamaCpp.Endpoint.History");

    apiKey.setDisplayName(Tr::tr("API Key"));
    apiKey.setDisplayStyle(StringAspect::LineEditDisplay);
    apiKey.setSettingsKey("ApiKey");
    apiKey.setLabelText(Tr::tr("API Key:"));
    apiKey.setDefaultValue("");
    apiKey.setToolTip(Tr::tr("llama.cpp server api key (optional)"));
    apiKey.setHistoryCompleter("LlamaCpp.ApiKey.History");

    nPrefix.setDisplayName(Tr::tr("Prefix Code Lines"));
    nPrefix.setSettingsKey("NPrefix");
    nPrefix.setLabelText(Tr::tr("Prefix Code Lines:"));
    nPrefix.setDefaultValue(256);
    nPrefix.setToolTip(
        Tr::tr("Number of code lines before the cursor location to include in the local prefix."));
    nPrefix.setRange(0, 65535);

    nSuffix.setDisplayName(Tr::tr("Suffix Code Lines"));
    nSuffix.setSettingsKey("NSuffix");
    nSuffix.setLabelText(Tr::tr("Suffix Code Lines:"));
    nSuffix.setDefaultValue(64);
    nSuffix.setToolTip(
        Tr::tr("Number of code lines after  the cursor location to include in the local suffix."));
    nSuffix.setRange(0, 65535);

    nPredict.setDisplayName(Tr::tr("Max Token Predictions"));
    nPredict.setSettingsKey("NPredict");
    nPredict.setLabelText(Tr::tr("Max Token Predictions:"));
    nPredict.setDefaultValue(128);
    nPredict.setToolTip(Tr::tr("Max number of tokens to predict."));
    nPredict.setRange(0, 65535);

    stopStrings.setDisplayName(Tr::tr("Stop Strings"));
    stopStrings.setDisplayStyle(StringAspect::LineEditDisplay);
    stopStrings.setSettingsKey("StopStrings");
    stopStrings.setLabelText(Tr::tr("Stop Strings:"));
    stopStrings.setDefaultValue("");
    stopStrings.setToolTip(
        Tr::tr("Return the result immediately as soon as any of these strings "
               "are encountered in the generated text. Separated by semicolons."));
    stopStrings.setHistoryCompleter("LlamaCpp.StopStrings.History");

    tMaxPromptMs.setDisplayName(Tr::tr("Max Prompt Time (ms)"));
    tMaxPromptMs.setSettingsKey("TMaxPromptMs");
    tMaxPromptMs.setLabelText(Tr::tr("Max Prompt Time (ms):"));
    tMaxPromptMs.setDefaultValue(500);
    tMaxPromptMs.setToolTip(
        Tr::tr("Max alloted time for the prompt processing (TODO: not yet supported)."));
    tMaxPromptMs.setRange(0, 65535);

    tMaxPredictMs.setDisplayName(Tr::tr("Max Predict Time (ms)"));
    tMaxPredictMs.setSettingsKey("TMaxPredictMs");
    tMaxPredictMs.setLabelText(Tr::tr("Max Predict Time (ms):"));
    tMaxPredictMs.setDefaultValue(1000);
    tMaxPredictMs.setToolTip(Tr::tr("Max alloted time for the prediction."));
    tMaxPredictMs.setRange(0, 65535);

    showInfo.setDisplayName(Tr::tr("Show Info"));
    showInfo.setSettingsKey("ShowInfo");
    showInfo.setLabelText(Tr::tr("Show Info:"));
    showInfo.setDefaultValue(2);
    showInfo.setToolTip(
        Tr::tr("Show extra info about the inference (0 - disabled, 1 - statusline, 2 - inline)."));
    showInfo.setRange(0, 2);

    autoFim.setDisplayName(Tr::tr("Auto FIM"));
    autoFim.setSettingsKey("AutoFim");
    autoFim.setLabelText(Tr::tr("Auto FIM"));
    autoFim.setDefaultValue(true);
    autoFim.setToolTip(
        Tr::tr("Trigger FIM (Fill-in-the-Middle) completion automatically on cursor movement."));

    maxLineSuffix.setDisplayName(Tr::tr("Max Line Suffix"));
    maxLineSuffix.setSettingsKey("MaxlineSuffix");
    maxLineSuffix.setLabelText(Tr::tr("Max Line Suffix:"));
    maxLineSuffix.setDefaultValue(8);
    maxLineSuffix.setToolTip(Tr::tr("Do not auto-trigger FIM completion if there are more than "
                                    "this number of characters to the right of the cursor."));
    maxLineSuffix.setRange(0, 65535);

    maxCacheKeys.setDisplayName(Tr::tr("Max Cache Keys"));
    maxCacheKeys.setSettingsKey("MaxCacheKeys");
    maxCacheKeys.setLabelText(Tr::tr("Max Cache Keys:"));
    maxCacheKeys.setDefaultValue(250);
    maxCacheKeys.setToolTip(Tr::tr("Max number of cached completions to keep in result_cache."));
    maxCacheKeys.setRange(0, 65535);

    ringNChunks.setDisplayName(Tr::tr("Ring Chunks"));
    ringNChunks.setSettingsKey("RingNChunks");
    ringNChunks.setLabelText(Tr::tr("Ring Chunks:"));
    ringNChunks.setDefaultValue(16);
    ringNChunks.setToolTip(
        Tr::tr("Max number of chunks to pass as extra context to the server (0 to disable)."));
    ringNChunks.setRange(0, 65535);

    ringChunkSize.setDisplayName(Tr::tr("Chunk Line Size"));
    ringChunkSize.setSettingsKey("RingChunkSize");
    ringChunkSize.setLabelText(Tr::tr("Chunk Line Size:"));
    ringChunkSize.setDefaultValue(64);
    ringChunkSize.setToolTip(Tr::tr(
        "Max size of the chunks (in number of lines).<br/><br/>Note: adjust these numbers so that "
        "you don't overrun your context. At ring_n_chunks = 64 and ring_chunk_size = 64 you "
        "need ~32k context."));
    ringChunkSize.setRange(0, 65535);

    ringScope.setDisplayName(Tr::tr("Ring Line Scope"));
    ringScope.setSettingsKey("RingScope");
    ringScope.setLabelText(Tr::tr("Ring Line Scope:"));
    ringScope.setDefaultValue(1024);
    ringScope.setToolTip(Tr::tr("The range around the cursor position (in number of lines) for "
                                "gathering chunks after FIM."));
    ringScope.setRange(0, 65535);

    ringUpdateMs.setDisplayName(Tr::tr("Update Interval (ms)"));
    ringUpdateMs.setSettingsKey("RingUpdateMs");
    ringUpdateMs.setLabelText(Tr::tr("Update Interval (ms):"));
    ringUpdateMs.setDefaultValue(100);
    ringUpdateMs.setToolTip(Tr::tr("How often to process queued chunks in normal mode."));
    ringUpdateMs.setRange(0, 65535);

    //
    // Chat
    //

    chatEndpoint.setDisplayName(Tr::tr("Chat Endpoint"));
    chatEndpoint.setSettingsKey("ChatEndpoint");
    chatEndpoint.setLabelText(Tr::tr("Chat Endpoint:"));
    chatEndpoint.setDisplayStyle(StringAspect::LineEditDisplay);
    chatEndpoint.setDefaultValue("http://127.0.0.1:8080");
    chatEndpoint.setToolTip(Tr::tr("llama.cpp server chat endpoint"));
    chatEndpoint.setHistoryCompleter("LlamaCpp.ChatEndpoint.History");

    chatApiKey.setDisplayName(Tr::tr("Chat API Key"));
    chatApiKey.setSettingsKey("ChatApiKey");
    chatApiKey.setLabelText(Tr::tr("Chat API Key:"));
    chatApiKey.setDisplayStyle(StringAspect::LineEditDisplay);
    chatApiKey.setDefaultValue("");
    chatApiKey.setToolTip(
        Tr::tr("Set the API Key if you are using --api-key option for the server."));
    chatApiKey.setHistoryCompleter("LlamaCpp.ChatApiKey.History");

    systemMessage.setDisplayName(Tr::tr("System Message"));
    systemMessage.setSettingsKey("SystemMessage");
    systemMessage.setLabelText(Tr::tr("System Message:"));
    systemMessage.setDefaultValue("");
    systemMessage.setPlaceHolderText(Tr::tr("Default: none"));
    systemMessage.setDisplayStyle(StringAspect::LineEditDisplay);
    systemMessage.setToolTip(Tr::tr("The starting message that defines how model should behave. "
                                    "Will be disabled if left empty."));
    systemMessage.setHistoryCompleter("LlamaCpp.SystemMessage.History");

    pasteLongTextToFileLen.setDisplayName(Tr::tr("Paste Long Text to File Length"));
    pasteLongTextToFileLen.setSettingsKey("PasteLongTextToFileLen");
    pasteLongTextToFileLen.setLabelText(Tr::tr("Paste Long Text to File Length:"));
    pasteLongTextToFileLen.setDefaultValue(2500);
    pasteLongTextToFileLen.setToolTip(
        Tr::tr("On pasting long text, it will be converted to a file. You can control the file "
               "length by setting the value of this parameter. Value 0 means disable."));
    pasteLongTextToFileLen.setRange(0, 65535);

    samplers.setDisplayName(Tr::tr("Samplers"));
    samplers.setSettingsKey("Samplers");
    samplers.setLabelText(Tr::tr("Samplers:"));
    samplers.setDefaultValue("edkypmxt");
    samplers.setToolTip(
        Tr::tr("The order at which samplers are applied, in simplified way. Default is "
               "\"edkypmxt\": dry->top_k->typ_p->top_p->min_p->xtc->temperature"));
    samplers.setHistoryCompleter("LlamaCpp.Samplers.History");
    samplers.setDisplayStyle(StringAspect::LineEditDisplay);

    temperature.setDisplayName(Tr::tr("Temperature"));
    temperature.setSettingsKey("Temperature");
    temperature.setLabelText(Tr::tr("Temperature:"));
    temperature.setDefaultValue(0.8);
    temperature.setToolTip(
        Tr::tr("Controls the randomness of the generated text by affecting the probability "
               "distribution of the output tokens. Higher = more random, lower = more focused."));
    temperature.setRange(0.0, 1.0);

    dynatemp_range.setDisplayName(Tr::tr("Dynamic Temperature Range"));
    dynatemp_range.setSettingsKey("DynatempRange");
    dynatemp_range.setLabelText(Tr::tr("Dynamic Temperature Range:"));
    dynatemp_range.setToolTip(
        Tr::tr("Addon for the temperature sampler. The added value to the range of dynamic "
               "temperature, which adjusts probabilities by entropy of tokens."));
    dynatemp_range.setDefaultValue(0.0);
    dynatemp_range.setRange(0.0, 1.0);

    dynatemp_exponent.setDisplayName(Tr::tr("Dynamic Temperature Exponent"));
    dynatemp_exponent.setSettingsKey("DynatempExponent");
    dynatemp_exponent.setLabelText(Tr::tr("Dynamic Temperature Exponent:"));
    dynatemp_exponent.setToolTip(
        Tr::tr("Addon for the temperature sampler. Smoothes out the probability redistribution "
               "based on the most probable token."));
    dynatemp_exponent.setDefaultValue(1.0);
    dynatemp_exponent.setRange(0.0, 10.0);

    top_k.setDisplayName(Tr::tr("Top K"));
    top_k.setSettingsKey("TopK");
    top_k.setLabelText(Tr::tr("Top K:"));
    top_k.setDefaultValue(40);
    top_k.setToolTip(Tr::tr("Keeps only k top tokens."));
    top_k.setRange(0, 100);

    top_p.setDisplayName(Tr::tr("Top P"));
    top_p.setSettingsKey("TopP");
    top_p.setLabelText(Tr::tr("Top P:"));
    top_p.setDefaultValue(0.95);
    top_p.setToolTip(
        Tr::tr("Limits tokens to those that together have a cumulative probability of at least p"));
    top_p.setRange(0.0, 1.0);

    min_p.setDisplayName(Tr::tr("Min P"));
    min_p.setSettingsKey("MinP");
    min_p.setLabelText(Tr::tr("Min P:"));
    min_p.setDefaultValue(0.05);
    min_p.setToolTip(Tr::tr("Limits tokens based on the minimum probability for a token to be "
                            "considered, relative to the probability of the most likely token."));
    min_p.setRange(0.0, 1.0);

    xtc_probability.setDisplayName(Tr::tr("XTC Probability"));
    xtc_probability.setSettingsKey("XtcProbability");
    xtc_probability.setLabelText(Tr::tr("XTC Probability:"));
    xtc_probability.setToolTip(Tr::tr("XTC sampler cuts out top tokens; this parameter controls "
                                      "the chance of cutting tokens at all. 0 disables XTC."));
    xtc_probability.setDefaultValue(0.0);
    xtc_probability.setRange(0.0, 1.0);

    xtc_threshold.setDisplayName(Tr::tr("XTC Threshold"));
    xtc_threshold.setSettingsKey("XtcThreshold");
    xtc_threshold.setLabelText(Tr::tr("XTC Threshold:"));
    xtc_threshold.setToolTip(Tr::tr("XTC sampler cuts out top tokens; this parameter controls the "
                                    "token probability that is required to cut that token."));
    xtc_threshold.setDefaultValue(0.1);
    xtc_threshold.setRange(0.0, 1.0);

    typical_p.setDisplayName(Tr::tr("Typical P"));
    typical_p.setSettingsKey("TypicalP");
    typical_p.setLabelText(Tr::tr("Typical P:"));
    typical_p.setDefaultValue(1.0);
    typical_p.setToolTip(Tr::tr(
        "Sorts and limits tokens based on the difference between log-probability and entropy."));
    typical_p.setRange(0.0, 1.0);

    repeat_last_n.setDisplayName(Tr::tr("Repeat Last N"));
    repeat_last_n.setSettingsKey("RepeatLastN");
    repeat_last_n.setLabelText(Tr::tr("Repeat Last N:"));
    repeat_last_n.setDefaultValue(64);
    repeat_last_n.setToolTip(Tr::tr("Last n tokens to consider for penalizing repetition"));
    repeat_last_n.setRange(-1, 1048576);

    repeat_penalty.setDisplayName(Tr::tr("Repeat Penalty"));
    repeat_penalty.setSettingsKey("RepeatPenalty");
    repeat_penalty.setLabelText(Tr::tr("Repeat Penalty:"));
    repeat_penalty.setDefaultValue(1.0);
    repeat_penalty.setToolTip(
        Tr::tr("Controls the repetition of token sequences in the generated text"));

    presence_penalty.setDisplayName(Tr::tr("Presence Penalty"));
    presence_penalty.setSettingsKey("PresencePenalty");
    presence_penalty.setLabelText(Tr::tr("Presence Penalty:"));
    presence_penalty.setDefaultValue(0.0);
    presence_penalty.setToolTip(
        Tr::tr("Limits tokens based on whether they appear in the output or not."));

    frequency_penalty.setDisplayName(Tr::tr("Frequency Penalty"));
    frequency_penalty.setSettingsKey("FrequencyPenalty");
    frequency_penalty.setLabelText(Tr::tr("Frequency Penalty:"));
    frequency_penalty.setDefaultValue(0.0);
    frequency_penalty.setToolTip(
        Tr::tr("Limits tokens based on how often they appear in the output."));

    dry_multiplier.setDisplayName(Tr::tr("DRY Multiplier"));
    dry_multiplier.setSettingsKey("DryMultiplier");
    dry_multiplier.setLabelText(Tr::tr("DRY Multiplier:"));
    dry_multiplier.setToolTip(
        Tr::tr("DRY sampling reduces repetition in generated text even across long contexts. This "
               "parameter sets the DRY sampling multiplier."));
    dry_multiplier.setDefaultValue(0.0);
    dry_multiplier.setRange(0.0, 10.0);

    dry_base.setDisplayName(Tr::tr("DRY Base"));
    dry_base.setSettingsKey("DryBase");
    dry_base.setLabelText(Tr::tr("DRY Base:"));
    dry_base.setToolTip(Tr::tr("DRY sampling reduces repetition in generated text even across long "
                               "contexts. This parameter sets the DRY sampling base value."));
    dry_base.setDefaultValue(1.75);
    dry_base.setRange(0.0, 10.0);

    dry_allowed_length.setDisplayName(Tr::tr("DRY Allowed Length"));
    dry_allowed_length.setSettingsKey("DryAllowedLength");
    dry_allowed_length.setLabelText(Tr::tr("DRY Allowed Length:"));
    dry_allowed_length.setToolTip(
        Tr::tr("DRY sampling reduces repetition in generated text even across long contexts. This "
               "parameter sets the allowed length for DRY sampling."));
    dry_allowed_length.setDefaultValue(2);
    dry_allowed_length.setRange(0, 100);

    dry_penalty_last_n.setDisplayName(Tr::tr("DRY Penalty Last N"));
    dry_penalty_last_n.setSettingsKey("DryPenaltyLastN");
    dry_penalty_last_n.setLabelText(Tr::tr("DRY Penalty Last N:"));
    dry_penalty_last_n.setToolTip(
        Tr::tr("DRY sampling reduces repetition in generated text even across long contexts. This "
               "parameter sets DRY penalty for the last n tokens."));
    dry_penalty_last_n.setDefaultValue(-1);
    dry_penalty_last_n.setRange(-1, 1048576);

    max_tokens.setDisplayName(Tr::tr("Max Tokens"));
    max_tokens.setSettingsKey("MaxTokens");
    max_tokens.setLabelText(Tr::tr("Max Tokens:"));
    max_tokens.setDefaultValue(-1);
    max_tokens.setToolTip(Tr::tr("The maximum number of token per output. -1 means no limit."));
    max_tokens.setRange(-1, 1048576);

    customJson.setDisplayName(Tr::tr("Custom JSON config"));
    customJson.setSettingsKey("CustomJson");
    customJson.setLabelText(Tr::tr("Custom JSON config:"));
    customJson.setToolTip(Tr::tr("Custom JSON string of extra parameters."));
    customJson.setHistoryCompleter("LlamaCpp.Custom.History");
    customJson.setDisplayStyle(StringAspect::TextEditDisplay);

    showTokensPerSecond.setDisplayName(Tr::tr("Show Tokens Per Second"));
    showTokensPerSecond.setSettingsKey("ShowTokensPerSecond");
    showTokensPerSecond.setLabelText(Tr::tr("Show Tokens Per Second"));
    showTokensPerSecond.setDefaultValue(false);
    showTokensPerSecond.setToolTip(Tr::tr("Show tokens per second in the chat UI."));

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

    chatEndpoint.setEnabler(&enableLlamaCpp);
    chatApiKey.setEnabler(&enableLlamaCpp);
    systemMessage.setEnabler(&enableLlamaCpp);
    pasteLongTextToFileLen.setEnabler(&enableLlamaCpp);
    samplers.setEnabler(&enableLlamaCpp);
    temperature.setEnabler(&enableLlamaCpp);
    dynatemp_range.setEnabler(&enableLlamaCpp);
    dynatemp_exponent.setEnabler(&enableLlamaCpp);
    top_k.setEnabler(&enableLlamaCpp);
    top_p.setEnabler(&enableLlamaCpp);
    min_p.setEnabler(&enableLlamaCpp);
    xtc_probability.setEnabler(&enableLlamaCpp);
    xtc_threshold.setEnabler(&enableLlamaCpp);
    typical_p.setEnabler(&enableLlamaCpp);
    repeat_last_n.setEnabler(&enableLlamaCpp);
    repeat_penalty.setEnabler(&enableLlamaCpp);
    presence_penalty.setEnabler(&enableLlamaCpp);
    frequency_penalty.setEnabler(&enableLlamaCpp);
    dry_multiplier.setEnabler(&enableLlamaCpp);
    dry_base.setEnabler(&enableLlamaCpp);
    dry_allowed_length.setEnabler(&enableLlamaCpp);
    dry_penalty_last_n.setEnabler(&enableLlamaCpp);
    max_tokens.setEnabler(&enableLlamaCpp);
    customJson.setEnabler(&enableLlamaCpp);

    setLayouter([this] {
        using namespace Layouting;

        // clang-format off
        Group fim {
            Column {
                endpoint, br,
                Row {apiKey}, br,
                Row {nPrefix}, br,
                Row {nSuffix}, br,
                Row {nPredict}, br,
                Row {stopStrings}, br,
                Row {tMaxPromptMs}, br,
                Row {tMaxPredictMs}, br,
                Row {showInfo}, br,
                Row {autoFim}, br,
                Row {maxLineSuffix}, br,
                Row {maxCacheKeys}, br,
                hr, br,
                Row {ringNChunks}, br,
                Row {ringChunkSize}, br,
                Row {ringScope}, br,
                Row {ringUpdateMs}, br,
            },
        };

        Group chat {
            Column {
                chatEndpoint, br,
                Row {chatApiKey}, br,
                systemMessage, br,
                Row {temperature}, br,
                Row {top_k}, br,
                Row {top_p}, br,
                Row {min_p}, br,
                Row {max_tokens}, br,
                Row {pasteLongTextToFileLen}, br,
                hr, br,
                Row {samplers}, br,
                Row {dynatemp_range}, br,
                Row {dynatemp_exponent}, br,
                Row {typical_p}, br,
                Row {xtc_probability}, br,
                Row {xtc_threshold}, br,
                hr, br,
                Row {repeat_last_n}, br,
                Row {repeat_penalty}, br,
                Row {presence_penalty}, br,
                Row {frequency_penalty}, br,
                Row {dry_multiplier}, br,
                Row {dry_base}, br,
                Row {dry_allowed_length}, br,
                Row {dry_penalty_last_n}, br,
                hr, br,
                showTokensPerSecond, br,
                customJson, br,
            },
        };

        return Column {
            enableLlamaCpp, br,
            Row {
                Column { fim, st },
                Column { chat, st }
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

} // namespace LlamaCpp
