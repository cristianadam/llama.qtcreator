#include <QApplication>
#include <QPainter>
#include <QScreen>

#include "llamaspinner.h"
#include "llamatheme.h"

namespace LlamaCpp {

QImageIOPlugin::Capabilities SpinnerPlugin::capabilities(QIODevice *device,
                                                         const QByteArray &format) const
{
    Q_UNUSED(device);
    if (format == "spinner")
        return CanRead;
    return {};
}

QImageIOHandler *SpinnerPlugin::create(QIODevice *device, const QByteArray &format) const
{
    Q_UNUSED(device);
    Q_UNUSED(format);
    return new SpinnerHandler();
}

SpinnerHandler::SpinnerHandler()
{
    foregroundColor = QColor(replaceThemeColorNamesWithRGBNames("Token_Text_Default"));
}

bool SpinnerHandler::canRead() const
{
    return true;
}

bool SpinnerHandler::read(QImage *image)
{
    if (!image)
        return false;

    const qreal dpr = QApplication::primaryScreen()->devicePixelRatio();
    const QSize logicalSize = m_size;
    const QSize physicalSize = logicalSize * dpr;

    QImage img(physicalSize, m_imageFormat);
    img.setDevicePixelRatio(dpr); // tell Qt this is a DPR‑scaled image
    img.fill(backgroundColor);

    QPainter painter(&img);
    painter.setRenderHint(QPainter::Antialiasing);

    const qreal w = logicalSize.width();
    const qreal h = logicalSize.height();
    const qreal side = qMin(w, h);           // keep it square
    const qreal radius = (side / 2.0) - 1.0; // leave 1 px for the pen width

    const QPointF centre(w / 2.0, h / 2.0);

    // Background circle (the “track” – 25 % opacity)
    QColor bgColor(foregroundColor);
    bgColor.setAlphaF(0.25);

    QPen bgPen(bgColor);
    bgPen.setWidthF(2.0 * dpr);
    bgPen.setCapStyle(Qt::RoundCap);
    bgPen.setColor(bgColor);
    bgPen.setCosmetic(true); // keep width constant on HiDPI
    bgPen.setBrush(bgColor);

    painter.setPen(bgPen);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(centre, radius, radius);

    // Foreground “spinner” arc
    const qreal arcSpanDeg = 90.0;                            // 90°
    const qreal startDeg = -arcSpanDeg / 2.0;                 // centre the arc at 0°
    const int startAngle = static_cast<int>((startDeg) * 16); // QPainter uses 1/16°
    const int spanAngle = static_cast<int>(arcSpanDeg * 16);

    // Compute the rotation that corresponds to the current frame.
    // imageCount() returns 10, so each frame advances 36°.
    const qreal rotationDeg = (frameIndex % imageCount())
                              * (360.0 / static_cast<qreal>(imageCount()));
    painter.save();
    painter.translate(centre);
    painter.rotate(rotationDeg);
    painter.translate(-centre);

    // Pen for the moving arc – full opacity, same colour as foreground.
    QPen fgPen(foregroundColor);
    fgPen.setWidthF(2.0 * dpr);
    fgPen.setCapStyle(Qt::RoundCap);
    fgPen.setCosmetic(true);

    painter.setPen(fgPen);
    painter.setBrush(Qt::NoBrush);

    // The rectangle that defines the full circle the arc lives on.
    QRectF arcRect(centre.x() - radius, centre.y() - radius, radius * 2.0, radius * 2.0);

    painter.drawArc(arcRect, startAngle, spanAngle);
    painter.restore();

    *image = img;

    // advance to the next frame for the next call
    ++frameIndex;

    return true;
}

bool SpinnerHandler::jumpToImage(int imageNumber)
{
    if (imageNumber < 0 || imageNumber >= imageCount())
        return false;
    frameIndex = imageNumber;
    return true;
}

int SpinnerHandler::currentImageNumber() const
{
    return frameIndex;
}

int SpinnerHandler::imageCount() const
{
    return 25;
}

int SpinnerHandler::nextImageDelay() const
{
    return 33;
}

int SpinnerHandler::loopCount() const
{
    return -1; // infinite looping
}

QByteArray SpinnerHandler::name() const
{
    return "spn";
}

bool SpinnerHandler::supportsOption(ImageOption option) const
{
    switch (option) {
    case Size:
    case ImageFormat:
    case Animation:
    case ScaledSize:
    case BackgroundColor:
        return true;
    default:
        return false;
    }
}

QVariant SpinnerHandler::option(ImageOption option) const
{
    switch (option) {
    case Size:
    case ScaledSize:
        return m_size;
    case BackgroundColor:
        return backgroundColor;
    case ImageFormat:
        return m_imageFormat;
    case Animation:
        return true;
    default:
        return QVariant();
    }
}

void SpinnerHandler::setOption(ImageOption option, const QVariant &value)
{
    switch (option) {
    case Size:
    case ScaledSize:
        m_size = value.toSize();
        break;
    case BackgroundColor:
        backgroundColor = value.value<QColor>();
        break;
    case ImageFormat:
        m_imageFormat = value.value<QImage::Format>();
        break;
    default:
        break;
    }
}

} // namespace LlamaCpp
