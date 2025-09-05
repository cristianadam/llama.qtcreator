#include <QMap>
#include <QString>

#include <utils/theme/theme.h>

#include "llamatheme.h"

using namespace Utils;

namespace LlamaCpp {

#define STRING_TO_ENUM(x) colormap[#x] = Theme::x

QString replaceThemeColorNamesWithRGBNames(const QString &styleSheet)
{
    static QMap<QString, Theme::Color> colormap;
    if (colormap.isEmpty()) {
        STRING_TO_ENUM(Token_Basic_Black);
        STRING_TO_ENUM(Token_Basic_White);
        STRING_TO_ENUM(Token_Accent_Default);
        STRING_TO_ENUM(Token_Accent_Muted);
        STRING_TO_ENUM(Token_Accent_Subtle);
        STRING_TO_ENUM(Token_Background_Default);
        STRING_TO_ENUM(Token_Background_Muted);
        STRING_TO_ENUM(Token_Background_Subtle);
        STRING_TO_ENUM(Token_Foreground_Default);
        STRING_TO_ENUM(Token_Foreground_Muted);
        STRING_TO_ENUM(Token_Foreground_Subtle);
        STRING_TO_ENUM(Token_Text_Default);
        STRING_TO_ENUM(Token_Text_Muted);
        STRING_TO_ENUM(Token_Text_Subtle);
        STRING_TO_ENUM(Token_Text_Accent);
        STRING_TO_ENUM(Token_Stroke_Strong);
        STRING_TO_ENUM(Token_Stroke_Muted);
        STRING_TO_ENUM(Token_Stroke_Subtle);
        STRING_TO_ENUM(Token_Notification_Alert_Default);
        STRING_TO_ENUM(Token_Notification_Alert_Muted);
        STRING_TO_ENUM(Token_Notification_Alert_Subtle);
        STRING_TO_ENUM(Token_Notification_Success_Default);
        STRING_TO_ENUM(Token_Notification_Success_Muted);
        STRING_TO_ENUM(Token_Notification_Success_Subtle);
        STRING_TO_ENUM(Token_Notification_Neutral_Default);
        STRING_TO_ENUM(Token_Notification_Neutral_Muted);
        STRING_TO_ENUM(Token_Notification_Neutral_Subtle);
        STRING_TO_ENUM(Token_Notification_Danger_Default);
        STRING_TO_ENUM(Token_Notification_Danger_Muted);
        STRING_TO_ENUM(Token_Notification_Danger_Subtle);
        STRING_TO_ENUM(Token_Gradient01_Start);
        STRING_TO_ENUM(Token_Gradient01_End);
        STRING_TO_ENUM(Token_Gradient02_Start);
        STRING_TO_ENUM(Token_Gradient02_End);

    }

    QString result = styleSheet;
    for (const auto &k : colormap.keys())
        result.replace(k, creatorColor(colormap[k]).name(QColor::HexRgb));

    return result;
}
} // namespace LlamaCpp
