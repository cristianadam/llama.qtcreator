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
