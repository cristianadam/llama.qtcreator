#include "ansitext.h"

#include <utils/theme/theme.h>

namespace LlamaCpp {

namespace {

// The SGR state a run of text is rendered with.
struct SgrState
{
    QColor fg;
    QColor bg;
    bool hasFg = false;
    bool hasBg = false;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
};

// The 256-color palette: 0-15 are the theme's colors, 16-231 the 6x6x6
// color cube, 232-255 the gray ramp.
QColor color256(int index, const QColor *palette)
{
    if (index < 16)
        return palette[index];
    if (index < 232) {
        static const int levels[6] = {0, 95, 135, 175, 215, 255};
        index -= 16;
        return QColor(levels[index / 36], levels[(index / 6) % 6], levels[index % 6]);
    }
    const int gray = 8 + 10 * (index - 232);
    return QColor(gray, gray, gray);
}

QTextCharFormat formatForState(const SgrState &state, const QTextCharFormat &base)
{
    QTextCharFormat fmt = base;
    if (state.hasFg) {
        QColor fg = state.fg;
        if (state.dim)
            fg.setAlphaF(0.6);
        fmt.setForeground(QBrush(fg));
    }
    if (state.hasBg)
        fmt.setBackground(QBrush(state.bg));
    if (state.bold)
        fmt.setFontWeight(QFont::Bold);
    if (state.italic)
        fmt.setFontItalic(true);
    if (state.underline)
        fmt.setUnderlineStyle(QTextCharFormat::SingleUnderline);
    return fmt;
}

// Applies one SGR parameter. \a palette is the terminal theme's 16 colors.
void applySgrCode(int code, SgrState &state, const QColor *palette)
{
    switch (code) {
    case 0:
        state = SgrState{};
        break;
    case 1:
        state.bold = true;
        break;
    case 2:
        state.dim = true;
        break;
    case 3:
        state.italic = true;
        break;
    case 4:
        state.underline = true;
        break;
    case 22:
        state.bold = false;
        state.dim = false;
        break;
    case 23:
        state.italic = false;
        break;
    case 24:
        state.underline = false;
        break;
    case 39:
        state.hasFg = false;
        break;
    case 49:
        state.hasBg = false;
        break;
    default:
        if (code >= 30 && code <= 37) {
            state.fg = palette[code - 30];
            state.hasFg = true;
        } else if (code >= 40 && code <= 47) {
            state.bg = palette[code - 40];
            state.hasBg = true;
        } else if (code >= 90 && code <= 97) {
            state.fg = palette[code - 90 + 8];
            state.hasFg = true;
        } else if (code >= 100 && code <= 107) {
            state.bg = palette[code - 100 + 8];
            state.hasBg = true;
        }
        break;
    }
}

// Applies a complete SGR sequence ("m" terminator) with the given, already
// split parameters (an empty string means a bare reset).
void applySgrSequence(const QString &params, SgrState &state, const QColor *palette)
{
    const QStringList parts = params.split(QLatin1Char(';'));
    int k = 0;
    while (k < parts.size()) {
        const int code = parts.at(k).toInt(); // empty parameter means 0
        if (code == 38 || code == 48) { // extended color: 5;n or 2;r;g;b
            const bool isFg = code == 38;
            if (k + 2 < parts.size() && parts.at(k + 1) == QLatin1String("5")) {
                const QColor c = color256(parts.at(k + 2).toInt(), palette);
                if (isFg) {
                    state.fg = c;
                    state.hasFg = true;
                } else {
                    state.bg = c;
                    state.hasBg = true;
                }
                k += 3;
                continue;
            }
            if (k + 4 < parts.size() && parts.at(k + 1) == QLatin1String("2")) {
                const QColor c(parts.at(k + 2).toInt(), parts.at(k + 3).toInt(),
                               parts.at(k + 4).toInt());
                if (isFg) {
                    state.fg = c;
                    state.hasFg = true;
                } else {
                    state.bg = c;
                    state.hasBg = true;
                }
                k += 5;
                continue;
            }
            // Malformed color introducer (e.g. a truncated 38;2;r;g; that
            // already carries its 'm' terminator): the remaining parameters
            // are unreliable, so stop processing this sequence rather than
            // misreading them as plain SGR codes.
            break;
        }
        applySgrCode(code, state, palette);
        ++k;
    }
}

} // namespace

// The palette is the theme's terminal tokens (the same source of truth the
// Terminal plugin derives its defaults from).
TerminalPalette terminalPalette()
{
    TerminalPalette palette;
    for (int i = 0; i < 16; ++i)
        palette.colors[i] =
                Utils::creatorColor(Utils::Theme::Color(Utils::Theme::TerminalAnsi0 + i));
    palette.foreground = Utils::creatorColor(Utils::Theme::TerminalForeground);
    return palette;
}

bool containsAnsiSequences(const QString &text)
{
    return text.contains(QChar(0x1B));
}

QVector<HighlightFragment> decodeAnsiText(const QString &text, const QTextCharFormat &base,
                                          const TerminalPalette &palette)
{
    QVector<HighlightFragment> fragments;
    if (text.isEmpty())
        return fragments;

    // Unstyled runs use the terminal's default foreground, like a real
    // terminal: the theme's TerminalForeground, not the surrounding text
    // color.
    const QTextCharFormat effectiveBase = [&] {
        QTextCharFormat b = base;
        b.setForeground(QBrush(palette.foreground));
        return b;
    }();

    SgrState state;
    QString pending;
    auto flush = [&] {
        if (!pending.isEmpty())
            fragments.append(HighlightFragment{pending, formatForState(state, effectiveBase)});
        pending.clear();
    };

    int i = 0;
    const int n = text.size();
    while (i < n) {
        const QChar c = text.at(i);
        if (c != QChar(0x1B)) {
            pending += c;
            ++i;
            continue;
        }
        // An escape sequence starts here.
        if (i + 1 >= n || text.at(i + 1) != QLatin1Char('[')) {
            // A lone ESC (or one at the very end): drop it.
            ++i;
            continue;
        }
        int j = i + 2;
        while (j < n) {
            const ushort u = text.at(j).unicode();
            if (u >= 0x40 && u <= 0x7E)
                break;
            ++j;
        }
        if (j >= n)
            break; // incomplete CSI at the end (streaming): drop it, the
                   // next render carries the complete sequence
        const QString params = text.mid(i + 2, j - i - 2);
        i = j + 1;
        if (text.at(j) != QLatin1Char('m'))
            continue; // not SGR (cursor movement, ...): no format change
        flush();
        applySgrSequence(params, state, palette.colors);
    }
    flush();
    return fragments;
}

QVector<HighlightFragment> decodeAnsiText(const QString &text, const QTextCharFormat &base)
{
    return decodeAnsiText(text, base, terminalPalette());
}

QString stripAnsiSequences(const QString &text)
{
    if (!containsAnsiSequences(text))
        return text;

    QString out;
    out.reserve(text.size());
    int i = 0;
    const int n = text.size();
    while (i < n) {
        const QChar c = text.at(i);
        if (c != QChar(0x1B)) {
            out += c;
            ++i;
            continue;
        }
        if (i + 1 >= n)
            break; // trailing lone ESC
        const QChar next = text.at(i + 1);
        if (next == QLatin1Char('[')) { // CSI: drop up to the final byte
            int j = i + 2;
            while (j < n) {
                const ushort u = text.at(j).unicode();
                if (u >= 0x40 && u <= 0x7E)
                    break;
                ++j;
            }
            i = (j < n) ? j + 1 : n;
        } else if (next == QLatin1Char(']')) { // OSC: drop up to BEL or ST
            int j = i + 2;
            while (j < n) {
                if (text.at(j) == QLatin1Char('\x07'))
                    break;
                if (text.at(j) == QChar(0x1B) && j + 1 < n
                    && text.at(j + 1) == QLatin1Char('\\')) {
                    ++j;
                    break;
                }
                ++j;
            }
            i = (j < n) ? j + 1 : n;
        } else {
            i += 2; // ESC + one character (Fe keys, ...): drop both
        }
    }
    return out;
}

} // namespace LlamaCpp
