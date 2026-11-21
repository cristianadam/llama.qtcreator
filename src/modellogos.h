#pragma once

#include <QColor>
#include <QPair>
#include <QPixmap>
#include <QString>
#include <QVector>

#include <QFile>
#include <QGuiApplication>
#include <QPainter>
#include <QScreen>
#include <QtSvg/QSvgRenderer>

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

// The logo rendered in \a color: the SVGs fill with "currentColor", which
// QSvgRenderer does not resolve (it falls back to black - invisible on a
// dark background), so the requested color is substituted into the source
// before rendering. An empty pixmap is returned when the resource cannot be
// loaded.
inline QPixmap modelLogoPixmap(const QString &name, int size, const QColor &color)
{
    QFile file(modelLogoResource(name));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QString svg = QString::fromUtf8(file.readAll());
    svg.replace(QLatin1String("currentColor"), color.name());
    QSvgRenderer renderer(svg.toUtf8());
    if (renderer.defaultSize().isEmpty())
        return {};
    const qreal dpr = QGuiApplication::primaryScreen()
                          ? QGuiApplication::primaryScreen()->devicePixelRatio()
                          : 1.0;
    const int physicalSize = int(size * dpr);
    QPixmap pixmap(physicalSize, physicalSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    renderer.render(&painter, QRectF(0, 0, size, size));
    return pixmap;
}

} // namespace LlamaCpp
