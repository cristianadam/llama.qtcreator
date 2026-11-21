#pragma once

#include <utils/icon.h>

#include <QChar>
#include <QColor>
#include <QIcon>

class QPushButton;

namespace LlamaCpp {
static const Utils::Icon LLAMACPP_ICON({{":/images/llama.png", Utils::Theme::IconsBaseColor}});

// The lucide glyphs of the bundled "lucideicons_outline" font (code points from fontforge-svg-importer's mapping; keep in sync).
namespace Lucide {
inline const QChar ArrowUp = QChar('B'); // arrow-up
inline const QChar Bot = QChar('5'); // bot
inline const QChar Check = QChar('R'); // check
inline const QChar ChevronDown = QChar('N'); // chevron-down
inline const QChar ChevronLeft = QChar('C'); // chevron-left
inline const QChar ChevronRight = QChar('D'); // chevron-right
inline const QChar ChevronUp = QChar('M'); // chevron-up
inline const QChar ChevronsUpDown = QChar('P'); // chevrons-up-down
inline const QChar CircleCheck = QChar('Z'); // circle-check
inline const QChar CircleFadingArrowUp = QChar('X'); // circle-fading-arrow-up
inline const QChar Clock = QChar('S'); // clock
inline const QChar Copy = QChar('E'); // copy
inline const QChar Cpu = QChar('Q'); // cpu
inline const QChar Download = QChar('1'); // download
inline const QChar Ear = QChar('6'); // ear
inline const QChar Eye = QChar('2'); // eye
inline const QChar FolderDown = QChar('F'); // folder-down
inline const QChar Lightbulb = QChar('L'); // lightbulb
inline const QChar LoaderCircle = QChar('0'); // loader-circle
inline const QChar Paperclip = QChar('G'); // paperclip
inline const QChar Play = QChar('Y'); // play
inline const QChar RefreshCw = QChar('A'); // refresh-cw
inline const QChar Square = QChar('I'); // square
inline const QChar SquarePen = QChar('H'); // square-pen
inline const QChar SquareTerminal = QChar('O'); // square-terminal
inline const QChar Timer = QChar('T'); // timer
inline const QChar Trash = QChar('K'); // trash
inline const QChar TriangleExclamationPoint = QChar('4'); // triangle-exclamation-point
inline const QChar Video = QChar('3'); // video
inline const QChar X = QChar('J'); // x
} // namespace Lucide

// A lucide glyph as a pixmap (an icon-font character would show up verbatim in a themed widget).
QIcon lucideGlyph(QChar glyph, const QColor &color, int pixelSize);

// A 16 px glyph in the button's text color, for flat settings-page buttons.
QIcon lucideButtonIcon(QChar glyph, const QPushButton *button);
} // namespace LlamaCpp
