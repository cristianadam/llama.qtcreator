#pragma once

#include <QMap>
#include <QString>

namespace LlamaCpp {
// Replaces the Token_* color names in a style sheet with the current theme's
// hex values. When a token cannot be resolved (e.g. no theme is loaded in
// the tests), \a fallbacks supplies a replacement for it.
QString replaceThemeColorNamesWithRGBNames(
    const QString &styleSheet, const QMap<QString, QString> &fallbacks = {});
}
