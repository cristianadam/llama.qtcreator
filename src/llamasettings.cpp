#include "llamasettings.h"
#include "llamaconstants.h"
#include "llamatr.h"
#include "promptssettingswidget.h"
#include "sandboxsettingswidget.h"
#include "skillssettingswidget.h"
#include "tools/factory.h"
#include "tools/tool.h"
#include "toolsettingswidget.h"

#include <coreplugin/dialogs/ioptionspage.h>
#include <projectexplorer/project.h>
#include <utils/guiutils.h>
#include <utils/layoutbuilder.h>
#include <utils/pathchooser.h>

#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QToolTip>

using namespace Utils;

namespace LlamaCpp {

QString defaultTitlePrompt()
{
    return Tr::tr("Summarize the title of the conversation in a few words including one emoji. "
                  "Use the language used in the conversation. Use plain text, no markdown.");
}

QString defaultFollowUpPrompt()
{
    return Tr::tr("Generate up to five follow up questions in the context of the "
                  "current conversation. The questions are from the user point of view. "
                  "Only questions, no explanations. Use the language used in the conversation. "
                  "Return a JSON object with a single key \"follow_ups\" containing an "
                  "array of plain text question strings, no markdown.");
}

QString defaultHandoffPrompt()
{
    return Tr::tr("The conversation above is long and will be continued in a new "
                  "conversation with a fresh context window. Write a handoff document "
                  "that lets the new conversation continue the work seamlessly without "
                  "seeing the earlier messages. Structure it as markdown with these "
                  "sections: ## Task (what the user asked for and the overall goal), "
                  "## Key Decisions (important decisions, constraints and preferences), "
                  "## Work Done (files touched, commands run, code changes and their "
                  "outcomes), ## Current State (what is completed, what is in progress, "
                  "errors encountered) and ## Next Steps (what to do next to finish the "
                  "task). Be precise and concrete: keep file paths, function names and "
                  "exact values. Do not answer the user's last message; only write the "
                  "handoff document.");
}

QStringList defaultLocatorPrompts()
{
    return {Tr::tr("Create a summary of {selection}"),
            Tr::tr("Create a commit message for the uncommitted changes.\n\n"
                   "{git_diff}"),
            Tr::tr("Explain the code in {selection}"),
            Tr::tr("Do spell checking and fix any typos in {selection}"),
            Tr::tr("Fix the following errors and warnings in {file}.\n\n{diagnostics}")};
}

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

// The static instance makes the page appear automatically.

static void initEnableAspect(BoolAspect &enableFim)
{
    enableFim.setSettingsKey(Constants::ENABLE_LLAMACPP);
    enableFim.setDisplayName(Tr::tr("Enable FIM"));
    enableFim.setLabelText(Tr::tr("Enable FIM"));
    enableFim.setToolTip(Tr::tr("Enables the FIM (fill-in-the-middle) completion in the text "
                                "editor. The chat, tools and web search settings are independent "
                                "of this."));
    enableFim.setDefaultValue(true);
}

LlamaSettings &settings()
{
    static ToolsSettingsPage theToolsPage;
    static SkillsSettingsPage theSkillsPage;
    static SandboxSettingsPage theSandboxPage;

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

    modelFim.setDisplayName(Tr::tr("FIM Model"));
    modelFim.setDisplayStyle(StringAspect::LineEditDisplay);
    modelFim.setSettingsKey("ModelFim");
    modelFim.setLabelText(Tr::tr("FIM Model:"));
    modelFim.setDefaultValue("");
    modelFim.setToolTip(
        Tr::tr("Model name to use for FIM completion in case when multiple models are loaded "
               "on the server (optional, recommended: Qwen3 Coder)."));
    modelFim.setHistoryCompleter("LlamaCpp.ModelFim.History");

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

    nCmpl.setDisplayName(Tr::tr("Completions per Position"));
    nCmpl.setSettingsKey("NCmpl");
    nCmpl.setLabelText(Tr::tr("Completions per Position:"));
    nCmpl.setDefaultValue(1);
    nCmpl.setToolTip(Tr::tr("Number of completions to cache per position (ring buffer). "
                            "Use the Next/Previous Completion shortcuts (Ctrl+G then Down/Up) "
                            "to cycle through them."));
    nCmpl.setRange(1, 32);

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
    maxCacheKeys.setToolTip(Tr::tr("Max number of cached context keys to keep in the result "
                                   "cache. Each key can hold up to 'Completions per Position' "
                                   "individual completions."));
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

    // Only exposed through the thinking-level dropdown in the chat status
    // bar, so no options-page entry is needed.
    thinkingLevel.setDisplayName(Tr::tr("Thinking Level"));
    thinkingLevel.setSettingsKey("ThinkingLevel");
    thinkingLevel.setDefaultValue("default");
    thinkingLevel.setToolTip(Tr::tr(
        "Thinking (reasoning) level for thinking-capable models. \"default\" "
        "uses the model's default, \"off\" disables thinking, and the concrete "
        "levels are the ones the server reports for the model's chat template."));

    // No options-page entry: the chat font zoom is adjusted with
    // Ctrl+mouse-wheel over the conversation or the chat zoom commands.
    chatFontZoom.setDisplayName(Tr::tr("Chat Font Zoom"));
    chatFontZoom.setSettingsKey("ChatFontZoom");
    chatFontZoom.setDefaultValue(100);
    chatFontZoom.setRange(50, 300);
    chatFontZoom.setToolTip(
        Tr::tr("Size of the chat conversation font, as a percentage of the editor "
               "font size. Adjust with Ctrl+mouse-wheel over the chat or the chat "
               "zoom in/out commands."));

    utilityModel.setDisplayName(Tr::tr("Utility Model"));
    utilityModel.setSettingsKey("UtilityModel");
    utilityModel.setLabelText(Tr::tr("Utility Model:"));
    utilityModel.setDefaultValue("");
    utilityModel.setPlaceHolderText(Tr::tr("Default: chat model"));
    utilityModel.setDisplayStyle(StringAspect::LineEditDisplay);
    utilityModel.setToolTip(
        Tr::tr("Model used for auxiliary requests such as conversation titles and "
               "follow-up suggestions. Use a small model to keep the main chat "
               "model free. Leave empty to use the active chat model."));
    utilityModel.setHistoryCompleter("LlamaCpp.UtilityModel.History");

    titlePrompt.setDisplayName(Tr::tr("Conversation Title Prompt"));
    titlePrompt.setSettingsKey("TitlePrompt");
    titlePrompt.setDefaultValue(defaultTitlePrompt());
    titlePrompt.setToolTip(
        Tr::tr("Prompt sent to the model to generate a short conversation title "
               "after the first reply."));

    followUpPrompt.setDisplayName(Tr::tr("Follow-Up Questions Prompt"));
    followUpPrompt.setSettingsKey("FollowUpPrompt");
    followUpPrompt.setDefaultValue(defaultFollowUpPrompt());
    followUpPrompt.setToolTip(
        Tr::tr("Prompt sent to the model to generate follow-up questions after a "
               "complete reply."));

    handoffPrompt.setDisplayName(Tr::tr("Handoff Prompt"));
    handoffPrompt.setSettingsKey("HandoffPrompt");
    handoffPrompt.setDefaultValue(defaultHandoffPrompt());
    handoffPrompt.setToolTip(
        Tr::tr("Prompt sent to the model to write the handoff document (task, "
               "current state, next steps) that seeds the new conversation when a "
               "conversation is continued in a new one."));

    // Only exposed through the "Follow up" toggle button in the chat status
    // bar, so no options-page entry is needed.
    followUpEnabled.setDisplayName(Tr::tr("Follow-Up Questions"));
    followUpEnabled.setSettingsKey("FollowUpEnabled");
    followUpEnabled.setDefaultValue(true);
    followUpEnabled.setToolTip(
        Tr::tr("When enabled, follow-up question suggestions are generated after "
               "each complete assistant reply."));

    // Only exposed through the "Title" toggle button in the chat status
    // bar, so no options-page entry is needed.
    autoTitle.setDisplayName(Tr::tr("Auto-generate Conversation Title"));
    autoTitle.setSettingsKey("AutoTitle");
    autoTitle.setDefaultValue(true);
    autoTitle.setToolTip(
        Tr::tr("When enabled, a short conversation title is generated by the "
               "model after the first assistant reply."));

    autoHandoff.setSettingsKey("AutoHandoff");
    autoHandoff.setDefaultValue(true);
    autoHandoff.setDisplayName(Tr::tr("Automatically continue in a new conversation"));
    autoHandoff.setLabelText(Tr::tr("Automatically continue in a new conversation"));
    autoHandoff.setToolTip(Tr::tr(
        "When the conversation uses more than the configured threshold of the "
        "model's context window, its history is summarized into a handoff document "
        "(task, current state, next steps), a new conversation seeded with that "
        "document is created and opened, and the original conversation stays "
        "untouched. This lets a long task keep going on a model with a small "
        "context window."));

    handoffThreshold.setSettingsKey("HandoffThreshold");
    handoffThreshold.setDefaultValue(80);
    handoffThreshold.setRange(10, 100);
    handoffThreshold.setDisplayName(Tr::tr("Handoff Threshold"));
    handoffThreshold.setLabelText(Tr::tr("Handoff Threshold:"));
    handoffThreshold.setToolTip(Tr::tr(
        "The conversation is continued in a new one when its history uses more "
        "than this percentage of the model's context window."));

    locatorPrompts.setDisplayName(Tr::tr("Locator Prompts"));
    locatorPrompts.setSettingsKey("LocatorPrompts");
    locatorPrompts.setDefaultValue(defaultLocatorPrompts());
    locatorPrompts.setToolTip(
        Tr::tr("Built-in prompts of the \"ll\" locator. Only the first line of a "
               "prompt is shown in the menu; the full text is sent to the model. "
               "\"{selection}\" is replaced with the selected text, \"{file}\" and "
               "\"{line}\" with the current file and cursor line, \"{diagnostics}\" "
               "with the errors and warnings of the current file, and \"{git_diff}\" "
               "with the uncommitted changes of its git repository."));

    selectionPromptButton.setDisplayName(Tr::tr("Selection prompt button"));
    selectionPromptButton.setLabelText(Tr::tr("Selection prompt button"));
    selectionPromptButton.setSettingsKey("SelectionPromptButton");
    selectionPromptButton.setDefaultValue(true);
    selectionPromptButton.setToolTip(
        Tr::tr("Show a small button at the bottom of a text selection in the editor. "
               "Clicking it opens the llama.cpp locator so the selection can be "
               "sent to llama.cpp with one of the prompts."));

    //
    // Skills
    //
    skillsDirectories.setSettingsKey("SkillsDirectories");
    skillsDirectories.setDefaultValue({QDir::homePath() + QStringLiteral("/.llama/skills")});
    skillsDirectories.setDisplayName(Tr::tr("Skill directories"));
    skillsDirectories.setToolTip(Tr::tr(
        "Directories scanned recursively for skills. A skill is a directory "
        "containing a SKILL.md file (Agent Skills specification) with a name "
        "and a description in its YAML frontmatter."));

    // Discovered skills are enabled by default; only the ones the user
    // unchecked on the Skills settings page land in this list.
    disabledSkillsList.setSettingsKey("DisabledSkillsList");
    disabledSkillsList.setDefaultValue(QStringList());

    //
    // Tools
    //
    const QStringList creatorsList = ToolFactory::instance().creatorsList();
    enabledToolsList.setSettingsKey("EnabledToolsList");
    enabledToolsList.setDefaultValue(creatorsList);

    // MCP tools are disabled by default - enabling them individually keeps
    // the context usage under control.
    enabledMcpToolsList.setSettingsKey("EnabledMcpToolsList");
    enabledMcpToolsList.setDefaultValue(QStringList());

    // Configured MCP servers, as a JSON array (Tools::McpServerConfig);
    // empty means: only the builtin Qt Creator MCP server.
    mcpServersJson.setSettingsKey("McpServers");
    mcpServersJson.setDefaultValue(QString());

    toolsEnabled.setSettingsKey("ToolsEnabled");
    toolsEnabled.setDefaultValue(false);
    toolsEnabled.setDisplayName(Tr::tr("Enable Tools in Chat"));
    toolsEnabled.setLabelText(Tr::tr("Enable Tools in Chat"));
    toolsEnabled.setToolTip(
        Tr::tr("If checked the chat will start with the Tools button turned on."));

    workingDirectory.setSettingsKey("WorkingDirectory");
    workingDirectory.setDefaultValue(QString());
    workingDirectory.setDisplayName(Tr::tr("Working directory"));
    workingDirectory.setLabelText(Tr::tr("Working directory:"));
    workingDirectory.setDisplayStyle(StringAspect::LineEditDisplay);
    workingDirectory.setHistoryCompleter("LlamaCpp.WorkingDirectory.History");
    workingDirectory.setToolTip(Tr::tr(
        "Directory the chat tools operate in. Leave empty to use the startup "
        "project's directory (or the default projects directory when no "
        "project is open). It can also be changed by clicking the working "
        "directory in the chat welcome screen."));

    sandboxCommands.setSettingsKey("SandboxCommands");
    sandboxCommands.setDefaultValue(false);
    sandboxCommands.setDisplayName(Tr::tr("Sandbox commands"));
    sandboxCommands.setLabelText(Tr::tr("Sandbox commands"));
    sandboxCommands.setToolTip(Tr::tr(
        "If checked, the chat tools are confined to a sandbox: bash commands "
        "run in a platform sandbox that blocks writes to system locations "
        "(bubblewrap/bwrap on Linux, sandbox-exec on macOS, a Microsoft MXC "
        "process container on Windows), and the file tools may "
        "only write inside the project directory and temporary locations. "
        "Credential locations (~/.ssh, ~/.aws, ~/.gnupg, ~/.kube, ~/.netrc) "
        "are not readable by any tool, and sandboxed commands have no "
        "network access (the webfetch and websearch tools are not "
        "affected). On Windows the sandbox uses the pinned wxc-exec.exe "
        "runtime, downloadable from the Sandbox settings page (a runtime "
        "of one's own can be pointed to there), and needs Windows 11 24H2 "
        "or newer. The "
        "additional directories and the network policy are configured on "
        "the Sandbox settings page."));

    // Sandbox details (managed on the "Sandbox" settings page, not in the
    // auto-generated layout): extra writable / read-only / deny-read
    // directories and the network policy of the sandboxed commands.
    sandboxWritableDirs.setSettingsKey("SandboxWritableDirs");
    sandboxWritableDirs.setDefaultValue(QStringList());
    sandboxWritableDirs.setDisplayName(Tr::tr("Sandbox writable directories"));
    sandboxWritableDirs.setToolTip(Tr::tr(
        "Directories the sandboxed tools may write to, in addition to the "
        "project directory and the temporary locations. Managed on the "
        "Sandbox settings page."));

    sandboxReadOnlyDirs.setSettingsKey("SandboxReadOnlyDirs");
    sandboxReadOnlyDirs.setDefaultValue(QStringList());
    sandboxReadOnlyDirs.setDisplayName(Tr::tr("Sandbox read-only directories"));
    sandboxReadOnlyDirs.setToolTip(Tr::tr(
        "Additional directories readable inside the sandbox. Only the "
        "Windows process container restricts reads (the other platforms "
        "already expose the file system read-only), so this takes effect "
        "on Windows. Managed on the Sandbox settings page."));

    sandboxDenyReadDirs.setSettingsKey("SandboxDenyReadDirs");
    sandboxDenyReadDirs.setDefaultValue(QStringList());
    sandboxDenyReadDirs.setDisplayName(Tr::tr("Sandbox deny-read directories"));
    sandboxDenyReadDirs.setToolTip(Tr::tr(
        "Additional directories that no tool may read (or write to), on top "
        "of the built-in credential locations, which are always protected. "
        "Managed on the Sandbox settings page."));

    sandboxAllowNetwork.setSettingsKey("SandboxAllowNetwork");
    sandboxAllowNetwork.setDefaultValue(false);
    sandboxAllowNetwork.setDisplayName(Tr::tr("Sandbox: allow network access"));
    sandboxAllowNetwork.setToolTip(Tr::tr(
        "If checked, sandboxed bash commands may use the network (e.g. "
        "'curl' or 'npm install'). Off by default: the sandbox has no "
        "network access. The webfetch and websearch tools always run "
        "outside the sandbox and are not affected. Managed on the Sandbox "
        "settings page."));

    mxcPath.setSettingsKey("MxcPath");
    mxcPath.setLabelText(Tr::tr("MXC runtime:"));
    mxcPath.setExpectedKind(PathChooserKind::File);
    mxcPath.setToolTip(Tr::tr(
        "A wxc-exec.exe launcher to run the Windows sandbox in. If empty, "
        "the runtime downloaded from this page is used."));

    ripgrepPath.setSettingsKey("RipgrepPath");
    ripgrepPath.setLabelText(Tr::tr("ripgrep:"));
    ripgrepPath.setExpectedKind(PathChooserKind::File);
    ripgrepPath.setToolTip(Tr::tr(
        "A ripgrep (rg) executable to run the search and find tools with. "
        "If empty, the one on the PATH is used, then the one downloaded "
        "from this page."));

    bashPath.setSettingsKey("BashPath");
    bashPath.setLabelText(Tr::tr("bash:"));
    bashPath.setExpectedKind(PathChooserKind::File);
    bashPath.setToolTip(Tr::tr(
        "The bash executable to run the bash tool with. If empty, the "
        "default bash is used (Git Bash). niubash - a native, MSYS2-free "
        "Bash - can additionally run in the Windows sandbox, where Git "
        "Bash cannot; use the Download... button to install it and Reset... "
        "to switch between the two."));

    loadProjectInstructions.setSettingsKey("LoadProjectInstructions");
    loadProjectInstructions.setDefaultValue(false);
    loadProjectInstructions.setDisplayName(Tr::tr("Load project instructions"));
    loadProjectInstructions.setLabelText(Tr::tr("Load project instructions"));
    loadProjectInstructions.setToolTip(Tr::tr(
        "If checked, the project instructions file of the current project is "
        "appended to the chat system message. The file is AGENTS.md, "
        "falling back to CLAUDE.md, and is looked up in the project "
        "directory and walked up to the git repository root, so a monorepo "
        "can keep a single instructions file for all of its projects. "
        "Files larger than 32 KB are truncated."));

    maxToolTurns.setSettingsKey("MaxToolTurns");
    maxToolTurns.setDefaultValue(100);
    maxToolTurns.setRange(0, 100000);
    maxToolTurns.setDisplayName(Tr::tr("Max consecutive tool turns"));
    maxToolTurns.setLabelText(Tr::tr("Max consecutive tool turns:"));
    maxToolTurns.setToolTip(Tr::tr(
        "A local model can loop tool calls forever, so after this many "
        "consecutive tool-only turns the calls are not executed - the model "
        "gets a failed result telling it to finish instead. Task "
        "(sub-agent) conversations are not limited. Set to 0 for no "
        "limit."));

    toolPreviewLines.setSettingsKey("ToolPreviewLines");
    toolPreviewLines.setDefaultValue(8);
    toolPreviewLines.setRange(1, 100);
    toolPreviewLines.setDisplayName(Tr::tr("Tool preview lines"));
    toolPreviewLines.setLabelText(Tr::tr("Tool preview lines:"));
    toolPreviewLines.setToolTip(Tr::tr(
        "The number of lines of a tool's result that are shown in the "
        "collapsed tool-call summary, below the one-line description. The "
        "same count is used by all tools, and by the live output tail of a "
        "running call. The preview is additionally capped at 120 characters "
        "per line."));

    //
    // Web search (websearch tool)
    //

    webSearchProvider.setDisplayName(Tr::tr("Web Search Provider"));
    webSearchProvider.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchProvider.setSettingsKey("WebSearchProvider");
    webSearchProvider.setLabelText(Tr::tr("Web Search Provider:"));
    webSearchProvider.setDefaultValue("duckduckgo");
    webSearchProvider.setToolTip(Tr::tr(
        "Backend used by the websearch tool: \"duckduckgo\" (default, no API key "
        "required), \"exa\" (Exa search API), \"google\" (Custom Search JSON API, "
        "requires an API key and a search engine ID), \"brave\" (Brave Search API, "
        "requires an API key) or \"tavily\" (requires an API key)."));
    webSearchProvider.setHistoryCompleter("LlamaCpp.WebSearchProvider.History");

    webSearchExaUrl.setDisplayName(Tr::tr("Exa Endpoint"));
    webSearchExaUrl.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchExaUrl.setSettingsKey("WebSearchExaUrl");
    webSearchExaUrl.setLabelText(Tr::tr("Exa Endpoint:"));
    webSearchExaUrl.setDefaultValue("https://mcp.exa.ai/mcp");
    webSearchExaUrl.setToolTip(Tr::tr("URL of the Exa MCP endpoint used by the websearch tool."));
    webSearchExaUrl.setHistoryCompleter("LlamaCpp.WebSearchExaUrl.History");

    webSearchExaApiKey.setDisplayName(Tr::tr("Exa API Key"));
    webSearchExaApiKey.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchExaApiKey.setSettingsKey("WebSearchExaApiKey");
    webSearchExaApiKey.setLabelText(Tr::tr("Exa API Key:"));
    webSearchExaApiKey.setDefaultValue("");
    webSearchExaApiKey.setToolTip(Tr::tr(
        "Optional Exa API key. Without a key the shared (rate-limited) endpoint is used."));
    webSearchExaApiKey.setHistoryCompleter("LlamaCpp.WebSearchExaApiKey.History");

    webSearchGoogleUrl.setDisplayName(Tr::tr("Google Endpoint"));
    webSearchGoogleUrl.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchGoogleUrl.setSettingsKey("WebSearchGoogleUrl");
    webSearchGoogleUrl.setLabelText(Tr::tr("Google Endpoint:"));
    webSearchGoogleUrl.setDefaultValue("https://www.googleapis.com/customsearch/v1");
    webSearchGoogleUrl.setToolTip(Tr::tr(
        "URL of the Google Custom Search (customsearch/v1) endpoint used by the websearch tool."));
    webSearchGoogleUrl.setHistoryCompleter("LlamaCpp.WebSearchGoogleUrl.History");

    webSearchGoogleApiKey.setDisplayName(Tr::tr("Google API Key"));
    webSearchGoogleApiKey.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchGoogleApiKey.setSettingsKey("WebSearchGoogleApiKey");
    webSearchGoogleApiKey.setLabelText(Tr::tr("Google API Key:"));
    webSearchGoogleApiKey.setDefaultValue("");
    webSearchGoogleApiKey.setToolTip(Tr::tr(
        "API key for the Google Custom Search JSON API (required for the \"google\" provider)."));
    webSearchGoogleApiKey.setHistoryCompleter("LlamaCpp.WebSearchGoogleApiKey.History");

    webSearchGoogleCx.setDisplayName(Tr::tr("Google Search Engine ID (cx)"));
    webSearchGoogleCx.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchGoogleCx.setSettingsKey("WebSearchGoogleCx");
    webSearchGoogleCx.setLabelText(Tr::tr("Google Search Engine ID (cx):"));
    webSearchGoogleCx.setDefaultValue("");
    webSearchGoogleCx.setToolTip(Tr::tr(
        "The cx (search engine ID) of the Google Custom Search engine "
        "(required for the \"google\" provider)."));
    webSearchGoogleCx.setHistoryCompleter("LlamaCpp.WebSearchGoogleCx.History");

    webSearchBraveUrl.setDisplayName(Tr::tr("Brave Search Endpoint"));
    webSearchBraveUrl.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchBraveUrl.setSettingsKey("WebSearchBraveUrl");
    webSearchBraveUrl.setLabelText(Tr::tr("Brave Search Endpoint:"));
    webSearchBraveUrl.setDefaultValue("https://api.search.brave.com/res/v1/web/search");
    webSearchBraveUrl.setToolTip(Tr::tr(
        "URL of the Brave Search API endpoint used by the websearch tool "
        "(the \"brave\" provider)."));
    webSearchBraveUrl.setHistoryCompleter("LlamaCpp.WebSearchBraveUrl.History");

    webSearchBraveApiKey.setDisplayName(Tr::tr("Brave Search API Key"));
    webSearchBraveApiKey.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchBraveApiKey.setSettingsKey("WebSearchBraveApiKey");
    webSearchBraveApiKey.setLabelText(Tr::tr("Brave Search API Key:"));
    webSearchBraveApiKey.setDefaultValue("");
    webSearchBraveApiKey.setToolTip(Tr::tr(
        "API key for the Brave Search API, get one at brave.com/search/api "
        "(required for the \"brave\" provider)."));
    webSearchBraveApiKey.setHistoryCompleter("LlamaCpp.WebSearchBraveApiKey.History");

    webSearchTavilyUrl.setDisplayName(Tr::tr("Tavily Endpoint"));
    webSearchTavilyUrl.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchTavilyUrl.setSettingsKey("WebSearchTavilyUrl");
    webSearchTavilyUrl.setLabelText(Tr::tr("Tavily Endpoint:"));
    webSearchTavilyUrl.setDefaultValue("https://api.tavily.com/search");
    webSearchTavilyUrl.setToolTip(Tr::tr(
        "URL of the Tavily search endpoint used by the websearch tool "
        "(the \"tavily\" provider)."));
    webSearchTavilyUrl.setHistoryCompleter("LlamaCpp.WebSearchTavilyUrl.History");

    webSearchTavilyApiKey.setDisplayName(Tr::tr("Tavily API Key"));
    webSearchTavilyApiKey.setDisplayStyle(StringAspect::LineEditDisplay);
    webSearchTavilyApiKey.setSettingsKey("WebSearchTavilyApiKey");
    webSearchTavilyApiKey.setLabelText(Tr::tr("Tavily API Key:"));
    webSearchTavilyApiKey.setDefaultValue("");
    webSearchTavilyApiKey.setToolTip(Tr::tr(
        "API key for Tavily, get one at tavily.com "
        "(required for the \"tavily\" provider)."));
    webSearchTavilyApiKey.setHistoryCompleter("LlamaCpp.WebSearchTavilyApiKey.History");

    initEnableAspect(enableFim);

    readSettings();

    // The FIM page's "Enable FIM" toggle gates only the FIM aspects; the
    // chat, tools and web search pages work with FIM turned off.
    endpoint.setEnabler(&enableFim);
    apiKey.setEnabler(&enableFim);
    modelFim.setEnabler(&enableFim);
    nPrefix.setEnabler(&enableFim);
    nSuffix.setEnabler(&enableFim);
    nPredict.setEnabler(&enableFim);
    nCmpl.setEnabler(&enableFim);
    stopStrings.setEnabler(&enableFim);
    tMaxPromptMs.setEnabler(&enableFim);
    tMaxPredictMs.setEnabler(&enableFim);
    showInfo.setEnabler(&enableFim);
    autoFim.setEnabler(&enableFim);
    maxLineSuffix.setEnabler(&enableFim);
    maxCacheKeys.setEnabler(&enableFim);
    ringNChunks.setEnabler(&enableFim);
    ringChunkSize.setEnabler(&enableFim);
    ringScope.setEnabler(&enableFim);
    ringUpdateMs.setEnabler(&enableFim);
    handoffThreshold.setEnabler(&autoHandoff);

    // The options dialog wires up its dirty tracking (Apply button, "unapplied
    // changes" prompt) from the aspect container only for provider pages
    // (IOptionsPageWidgetPrivate::setAspects). All llama.cpp pages use custom
    // widgets (setWidgetCreator), so replicate that wiring here: re-check the
    // dialog's dirty state whenever any aspect changes. Without it, editing a
    // field leaves the Apply button disabled and the change is silently
    // discarded when leaving the settings mode (volatile values are never
    // applied, so they do not reach writeSettings()).
    connect(this, &AspectContainer::volatileValueChanged, &Utils::checkSettingsDirty);
}

LlamaProjectSettings::LlamaProjectSettings(ProjectExplorer::Project *project)
{
    setAutoApply(true);

    useGlobalSettings.setSettingsKey(Constants::LLAMACPP_USE_GLOBAL_SETTINGS);
    useGlobalSettings.setSettingsPageId(Constants::LLAMACPP_FIM_OPTIONS_ID);

    initEnableAspect(enableFim);

    sandboxCommands.setSettingsKey(Constants::LLAMACPP_PROJECT_SANDBOX_COMMANDS);
    sandboxCommands.setDefaultValue(false);
    sandboxCommands.setDisplayName(Tr::tr("Sandbox commands"));
    sandboxCommands.setLabelText(Tr::tr("Sandbox commands"));
    sandboxCommands.setToolTip(
        Tr::tr("Overrides the global 'Sandbox commands' setting for this "
               "project. See the global setting on the Llama Tools page for "
               "what the sandbox restricts."));

    Store map = storeFromVariant(project->namedSettings(Constants::LLAMACPP_PROJECT_SETTINGS_ID));
    fromMap(map);

    enableFim.addOnChanged(this, [this, project] { save(project); });
    useGlobalSettings.addOnChanged(this, [this, project] { save(project); });
    loadProjectInstructions.setSettingsKey(Constants::LLAMACPP_PROJECT_LOAD_PROJECT_INSTRUCTIONS);
    loadProjectInstructions.setDefaultValue(false);
    loadProjectInstructions.setDisplayName(Tr::tr("Load project instructions"));
    loadProjectInstructions.setLabelText(Tr::tr("Load project instructions"));
    loadProjectInstructions.setToolTip(
        Tr::tr("Overrides the global 'Load project instructions' setting for "
               "this project. See the global setting on the Llama Tools page "
               "for what is loaded."));

    sandboxCommands.addOnChanged(this, [this, project] { save(project); });
    loadProjectInstructions.addOnChanged(this, [this, project] { save(project); });
}

bool LlamaProjectSettings::isSandboxEnabled() const
{
    if (useGlobalSettings())
        return settings().sandboxCommands();
    return sandboxCommands();
}

bool LlamaProjectSettings::isLoadProjectInstructionsEnabled() const
{
    if (useGlobalSettings())
        return settings().loadProjectInstructions();
    return loadProjectInstructions();
}

void LlamaProjectSettings::setUseGlobalSettings(bool useGlobal)
{
    useGlobalSettings.setValue(useGlobal);
}

bool LlamaProjectSettings::isFimEnabled() const
{
    if (useGlobalSettings())
        return settings().enableFim();
    return enableFim();
}

void LlamaProjectSettings::save(ProjectExplorer::Project *project)
{
    Store map;
    toMap(map);
    project->setNamedSettings(Constants::LLAMACPP_PROJECT_SETTINGS_ID, variantFromStore(map));

    settings().apply();
}

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
    // clang-format off
    Column {
        s.enableFim, br,
        Row {s.endpoint}, br,
        Row {s.apiKey}, br,
        Row {s.modelFim, st}, br,
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
    // clang-format off
    Row {
        Column {
            Row {s.chatEndpoint}, br,
            Row {s.chatApiKey}, br,
            s.systemMessage, br,
            Row {s.utilityModel, st}, br,
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
