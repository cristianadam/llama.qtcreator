#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

namespace LlamaCpp {

// One downloadable build (a quantization) of a model size, from the llama.app catalog.
struct CatalogBuild
{
    QString quant; // "Q4_K_M", "mxfp4", ...
    QString size; // "12.1 GB"
    qint64 sizeBytes = 0;
    QString repo; // the Hugging Face repo id ("ggml-org/LFM3-Omni-2B-GGUF")
    bool goodFirstModel = false;

    // The "repo:quant" id for the server's POST /models (the quant pins the exact build).
    QString serverModelId() const
    {
        return quant.isEmpty() ? repo : (repo + QStringLiteral(":") + quant);
    }
};

// One parameter size of a model family (the builds are its quants).
struct CatalogSize
{
    QString name; // "GPT-OSS 20B"
    QString params; // "20B"
    QStringList inputModalities; // "text" plus "image"/"audio" when present
    QList<CatalogBuild> builds;
};

// A model family of the catalog, with one or more sizes.
struct CatalogFamily
{
    QString name; // "GPT-OSS"
    QString brand; // "OpenAI"
    QString description;
    QString license;
    bool featured = false;
    bool nonCommercial = false;
    // Model-kind flags: decision / embedding models are not chat models.
    bool decision = false;
    bool embedding = false;
    QList<CatalogSize> sizes;
};

// The llama.app model catalog (the same source the Llama tray apps browse).
QString catalogUrl();

// The "families" array of a catalog.json document ({} when unparseable).
QList<CatalogFamily> parseCatalog(const QByteArray &json);

} // namespace LlamaCpp
