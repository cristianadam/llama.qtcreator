#pragma once

#include <coreplugin/dialogs/ioptionspage.h>
#include <projectexplorer/useglobalaspect.h>
#include <utils/aspects.h>

namespace ProjectExplorer {
    class Project;
}

namespace LlamaCpp {

class LlamaSettings : public Utils::AspectContainer
{
public:
    LlamaSettings();

    Utils::BoolAspect enableLlamaCpp{this};

    // FIM
    Utils::StringAspect endpoint{this};
    Utils::StringAspect apiKey{this};
    Utils::StringAspect modelFim{this};
    Utils::IntegerAspect nPrefix{this};
    Utils::IntegerAspect nSuffix{this};
    Utils::IntegerAspect nPredict{this};
    Utils::IntegerAspect nCmpl{this};
    Utils::StringAspect stopStrings{this};
    Utils::IntegerAspect tMaxPromptMs{this};
    Utils::IntegerAspect tMaxPredictMs{this};
    Utils::IntegerAspect showInfo{this};
    Utils::BoolAspect autoFim{this};
    Utils::IntegerAspect maxLineSuffix{this};
    Utils::IntegerAspect maxCacheKeys{this};

    Utils::IntegerAspect ringNChunks{this};
    Utils::IntegerAspect ringChunkSize{this};
    Utils::IntegerAspect ringScope{this};
    Utils::IntegerAspect ringUpdateMs{this};

    // Chat
    Utils::StringAspect chatEndpoint{this};
    Utils::StringAspect chatApiKey{this};
    Utils::StringAspect systemMessage{this};
    Utils::IntegerAspect pasteLongTextToFileLen{this};
    Utils::StringAspect samplers{this};
    Utils::DoubleAspect temperature{this};
    Utils::DoubleAspect dynatemp_range{this};
    Utils::DoubleAspect dynatemp_exponent{this};
    Utils::IntegerAspect top_k{this};
    Utils::DoubleAspect top_p{this};
    Utils::DoubleAspect min_p{this};
    Utils::DoubleAspect xtc_probability{this};
    Utils::DoubleAspect xtc_threshold{this};
    Utils::DoubleAspect typical_p{this};
    Utils::IntegerAspect repeat_last_n{this};
    Utils::DoubleAspect repeat_penalty{this};
    Utils::DoubleAspect presence_penalty{this};
    Utils::DoubleAspect frequency_penalty{this};
    Utils::DoubleAspect dry_multiplier{this};
    Utils::DoubleAspect dry_base{this};
    Utils::IntegerAspect dry_allowed_length{this};
    Utils::IntegerAspect dry_penalty_last_n{this};
    Utils::IntegerAspect max_tokens{this};
    Utils::StringAspect customJson{this};
    Utils::BoolAspect showTokensPerSecond{this};
    // Optional (typically smaller) model used for auxiliary requests –
    // conversation titles and follow‑up suggestions. Empty means: use the
    // active chat model.
    Utils::StringAspect utilityModel{this};
    // "default", "off", "low", "medium", "high" or "max" (sent to the
    // server as the OAI "reasoning_effort" field).
    Utils::StringAspect thinkingLevel{this};

    Utils::StringListAspect enabledToolsList{this};
    // Tools served by the Qt Creator MCP server are disabled by default
    // (they eat a lot of context); only the ones the user explicitly turned
    // on land in this list.
    Utils::StringListAspect enabledMcpToolsList{this};
    Utils::BoolAspect toolsEnabled{this};

    // Editable prompts (see the "Prompts" settings page)
    Utils::StringAspect titlePrompt{this};
    Utils::StringAspect followUpPrompt{this};
    // Toggled by the "Follow up" button in the chat status bar: when on,
    // follow-up question suggestions are generated after each complete reply.
    Utils::BoolAspect followUpEnabled{this};
    // "ll" locator prompts: only the first line of a prompt is shown in the
    // menu, the full text is sent to the model.
    Utils::StringListAspect locatorPrompts{this};

    // Web search (websearch tool)
    Utils::StringAspect webSearchProvider{this};
    Utils::StringAspect webSearchExaUrl{this};
    Utils::StringAspect webSearchExaApiKey{this};
    Utils::StringAspect webSearchGoogleUrl{this};
    Utils::StringAspect webSearchGoogleApiKey{this};
    Utils::StringAspect webSearchGoogleCx{this};
    Utils::StringAspect webSearchBraveUrl{this};
    Utils::StringAspect webSearchBraveApiKey{this};
    Utils::StringAspect webSearchTavilyUrl{this};
    Utils::StringAspect webSearchTavilyApiKey{this};
};

LlamaSettings &settings();

//! Built-in default prompts, shown on the "Prompts" settings page and used
//! by the "Reset to Default" button there.
QString defaultTitlePrompt();
QString defaultFollowUpPrompt();
QStringList defaultLocatorPrompts();

class LlamaProjectSettings : public Utils::AspectContainer
{
public:
    explicit LlamaProjectSettings(ProjectExplorer::Project *project);

    void save(ProjectExplorer::Project *project);
    void setUseGlobalSettings(bool useGlobalSettings);

    bool isEnabled() const;

    Utils::BoolAspect enableLlamaCpp{this};
    ProjectExplorer::UseGlobalAspect useGlobalSettings{Utils::Id(), this};
};

class ToolsSettingsPage : public Core::IOptionsPage
{
public:
    ToolsSettingsPage();
};

} // namespace LlamaCpp
