#include "llamaicons.h"

#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>

namespace LlamaCpp {

QIcon lucideGlyph(QChar glyph, const QColor &color, int pixelSize)
{
    const qreal dpr = QGuiApplication::primaryScreen()
                          ? QGuiApplication::primaryScreen()->devicePixelRatio()
                          : 1.0;
    QFont font(QStringLiteral("lucideicons_outline"));
    font.setPixelSize(pixelSize);
    const int physicalSize = int(pixelSize * dpr);
    QPixmap pixmap(physicalSize, physicalSize);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.setFont(font);
    painter.setPen(color);
    painter.drawText(QRectF(0, 0, pixelSize, pixelSize), Qt::AlignCenter, QString(glyph));
    return QIcon(pixmap);
}

QIcon lucideButtonIcon(QChar glyph, const QPushButton *button)
{
    return lucideGlyph(glyph, button->palette().color(QPalette::WindowText), 16);
}

} // namespace LlamaCpp
