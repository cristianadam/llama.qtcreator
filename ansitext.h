#pragma once

#include <QColor>
#include <QString>
#include <QTextCharFormat>
#include <QVector>

#include "llamasyntaxhighlighter.h"

namespace LlamaCpp {

// The terminal theme's palette: the sixteen ANSI colors and the default
// foreground, as configured in Tools > Options > Environments > Terminal
// (the Terminal plugin's settings).
struct TerminalPalette
{
    QColor colors[16];
    QColor foreground;
};

TerminalPalette terminalPalette();

// True when \a text contains ANSI escape sequences.
bool containsAnsiSequences(const QString &text);

/*!
    Decodes the ANSI SGR (color/style) escape sequences in \a text into runs
    of text with matching char formats.

    \a palette provides the sixteen standard colors; 256-color (38;5;n) and
    truecolor (38;2;r;g;b) sequences are supported as well, as are bold, dim,
    italic and underline. \a base is the format unstyled runs start from.

    Escape sequences that are incomplete at the end of the text (the block
    is still streaming) are dropped: the next render receives the complete
    sequence. Non-SGR sequences (cursor movement, OSC titles, ...) are
    removed without effecting the formatting.
*/
QVector<HighlightFragment> decodeAnsiText(const QString &text, const QTextCharFormat &base,
                                          const TerminalPalette &palette);

// Convenience overload using the terminal theme's palette.
QVector<HighlightFragment> decodeAnsiText(const QString &text, const QTextCharFormat &base);

// Strips all ANSI escape sequences from \a text (HTML export, copy & paste).
QString stripAnsiSequences(const QString &text);

} // namespace LlamaCpp
