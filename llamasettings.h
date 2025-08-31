#pragma once

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
    Utils::IntegerAspect nPrefix{this};
    Utils::IntegerAspect nSuffix{this};
    Utils::IntegerAspect nPredict{this};
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
};

LlamaSettings &settings();

class LlamaProjectSettings : public Utils::AspectContainer
{
public:
    explicit LlamaProjectSettings(ProjectExplorer::Project *project);

    void save(ProjectExplorer::Project *project);
    void setUseGlobalSettings(bool useGlobalSettings);

    bool isEnabled() const;

    Utils::BoolAspect enableLlamaCpp{this};
    Utils::BoolAspect useGlobalSettings{this};
};

} // namespace LlamaCpp
