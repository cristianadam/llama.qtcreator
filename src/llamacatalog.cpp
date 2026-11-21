#include "llamacatalog.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace LlamaCpp {

QString catalogUrl()
{
    return QStringLiteral("https://llama.app/v1/catalog.json");
}

namespace {
QList<CatalogBuild> parseBuilds(const QJsonArray &array)
{
    QList<CatalogBuild> builds;
    for (const auto &value : array) {
        const QJsonObject obj = value.toObject();
        CatalogBuild build;
        build.quant = obj.value("quant").toString();
        build.size = obj.value("size").toString();
        build.sizeBytes = obj.value("sizeBytes").toVariant().toLongLong();
        build.repo = obj.value("repo").toString();
        if (build.repo.isEmpty())
            continue;
        builds.append(build);
    }
    return builds;
}

QList<CatalogSize> parseSizes(const QJsonArray &array)
{
    QList<CatalogSize> sizes;
    for (const auto &value : array) {
        const QJsonObject obj = value.toObject();
        CatalogSize size;
        size.name = obj.value("name").toString();
        size.params = obj.value("params").toString();
        // An explicit modality list wins over the vision/audio flags (text is always in).
        const QJsonArray modalities = obj.value("inputModalities").toArray();
        if (!modalities.isEmpty()) {
            for (const auto &modality : modalities)
                size.inputModalities.append(modality.toString());
        } else {
            size.inputModalities << QStringLiteral("text");
            if (obj.value("vision").toBool())
                size.inputModalities << QStringLiteral("image");
            if (obj.value("audio").toBool())
                size.inputModalities << QStringLiteral("audio");
        }
        size.builds = parseBuilds(obj.value("builds").toArray());
        const bool goodFirstModel = obj.value("goodFirstModel").toBool();
        for (CatalogBuild &build : size.builds)
            build.goodFirstModel = goodFirstModel;
        if (!size.builds.isEmpty())
            sizes.append(size);
    }
    return sizes;
}
} // namespace

QList<CatalogFamily> parseCatalog(const QByteArray &json)
{
    QList<CatalogFamily> families;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonArray array = doc.isArray() ? doc.array()
                                           : doc.object().value("families").toArray();
    for (const auto &value : array) {
        const QJsonObject obj = value.toObject();
        CatalogFamily family;
        family.name = obj.value("name").toString();
        family.brand = obj.value("brand").toString();
        family.description = obj.value("description").toString();
        family.license = obj.value("license").toString();
        family.featured = obj.value("featured").toBool();
        family.nonCommercial = obj.value("nonCommercial").toBool();
        family.decision = obj.value("decision").toBool();
        family.embedding = obj.value("embedding").toBool();
        family.sizes = parseSizes(obj.value("sizes").toArray());
        if (!family.name.isEmpty() && !family.sizes.isEmpty())
            families.append(family);
    }
    return families;
}

} // namespace LlamaCpp
