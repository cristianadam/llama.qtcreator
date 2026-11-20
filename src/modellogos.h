#pragma once

#include <QPair>
#include <QString>
#include <QVector>

namespace LlamaCpp {

// The brand logo of a model name (keyword match; the llama mark is the
// fallback).
inline QString modelLogoResource(const QString &name)
{
    static const QVector<QPair<QString, QString>> logos = {
        {QStringLiteral("qwen"), QStringLiteral(":/images/logos/qwen.svg")},
        {QStringLiteral("gemma"), QStringLiteral(":/images/logos/gemma.svg")},
        {QStringLiteral("openai"), QStringLiteral(":/images/logos/gpt.svg")},
        {QStringLiteral("gpt"), QStringLiteral(":/images/logos/gpt.svg")},
        {QStringLiteral("mistral"), QStringLiteral(":/images/logos/mistral.svg")},
        {QStringLiteral("ministral"), QStringLiteral(":/images/logos/mistral.svg")},
        {QStringLiteral("devstral"), QStringLiteral(":/images/logos/mistral.svg")},
        {QStringLiteral("magistral"), QStringLiteral(":/images/logos/mistral.svg")},
        {QStringLiteral("glm"), QStringLiteral(":/images/logos/z.svg")},
        {QStringLiteral("z.ai"), QStringLiteral(":/images/logos/z.svg")},
        {QStringLiteral("nemotron"), QStringLiteral(":/images/logos/nvidia.svg")},
        {QStringLiteral("nvidia"), QStringLiteral(":/images/logos/nvidia.svg")},
        {QStringLiteral("laguna"), QStringLiteral(":/images/logos/poolside.svg")},
        {QStringLiteral("poolside"), QStringLiteral(":/images/logos/poolside.svg")},
        {QStringLiteral("deepseek"), QStringLiteral(":/images/logos/deepseek.svg")},
        {QStringLiteral("clef"), QStringLiteral(":/images/logos/cloudflare.svg")},
        {QStringLiteral("cloudflare"), QStringLiteral(":/images/logos/cloudflare.svg")},
        {QStringLiteral("lfm"), QStringLiteral(":/images/logos/liquid.svg")},
        {QStringLiteral("liquid"), QStringLiteral(":/images/logos/liquid.svg")},
        {QStringLiteral("muse"), QStringLiteral(":/images/logos/meta.svg")},
        {QStringLiteral("meta"), QStringLiteral(":/images/logos/meta.svg")},
    };
    const QString haystack = name.toLower();
    for (const auto &entry : logos)
        if (haystack.contains(entry.first))
            return entry.second;
    return QStringLiteral(":/images/llama.svg");
}

} // namespace LlamaCpp
