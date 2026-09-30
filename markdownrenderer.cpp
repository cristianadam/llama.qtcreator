#include "markdownrenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QCryptographicHash>
#include <QLayout>
#include <QClipboard>
#include <QColor>
#include <QFrame>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGuiApplication>
#include <QAudioOutput>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QMediaPlayer>
#include <QMimeData>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QPolygonF>
#include <QSvgRenderer>
#include <QTextDocumentFragment>
#include <QTextFragment>
#include <QTextLayout>
#include <QtMultimediaWidgets/QVideoWidget>

#include <algorithm>

#include <functional>
#include <QToolButton>
#include <QToolTip>

#include "katexengine.h"
#include "llamasyntaxhighlighter.h"
#include "llamatr.h"
#include "mermaidengine.h"

using namespace LlamaCpp;

static QString colorToRgba(const QColor &c)
{
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
}

static QString fromStdString(const std::pmr::string &s)
{
    return QString::fromUtf8(s.data(), s.size());
}

// Escape a string for safe inclusion in an HTML snippet passed to insertHtml().
static QString escapeHtml(QString text);

// 16:9 placeholder served for "llamavideo://" image URLs: a dark rounded
// rectangle with a play triangle. It reserves the line the video occupies;
// the QVideoWidget overlay (renderVideoElement()) covers it, letterboxing
// videos whose aspect ratio differs.
static QImage videoPlaceholderImage()
{
    constexpr int kWidth = 480;
    constexpr int kHeight = 270;
    QImage image(kWidth, kHeight, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0x1F, 0x23, 0x28));
    painter.drawRoundedRect(QRectF(image.rect()), 10, 10);
    painter.setBrush(QColor(255, 255, 255, 160));
    QPolygonF triangle;
    triangle << QPointF(kWidth * 0.45, kHeight * 0.36)
             << QPointF(kWidth * 0.66, kHeight * 0.5)
             << QPointF(kWidth * 0.45, kHeight * 0.64);
    painter.drawPolygon(triangle);
    return image;
}

// Decode the HTML entities that can occur in a quoted attribute value
// (&amp; &lt; &gt; &quot; &apos; plus numeric references); a model-generated
// path may carry an escaped ampersand.
static QString decodeHtmlEntities(const QString &text)
{
    if (!text.contains(QLatin1Char('&')))
        return text;
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        if (text.at(i) != QLatin1Char('&')) {
            out.append(text.at(i));
            continue;
        }
        const int semi = text.indexOf(QLatin1Char(';'), i);
        if (semi < 0 || semi - i > 12) {
            out.append(text.at(i));
            continue;
        }
        const QString entity = text.mid(i + 1, semi - i - 1);
        QChar ch;
        if (entity == QLatin1String("amp"))
            ch = QLatin1Char('&');
        else if (entity == QLatin1String("lt"))
            ch = QLatin1Char('<');
        else if (entity == QLatin1String("gt"))
            ch = QLatin1Char('>');
        else if (entity == QLatin1String("quot"))
            ch = QLatin1Char('"');
        else if (entity == QLatin1String("apos"))
            ch = QLatin1Char('\'');
        else if (entity.startsWith(QStringLiteral("#x"), Qt::CaseInsensitive))
            ch = QChar(entity.mid(2).toInt(nullptr, 16));
        else if (entity.startsWith(QLatin1Char('#')))
            ch = QChar(entity.mid(1).toInt());
        if (ch.isNull()) {
            out.append(text.at(i));
            continue;
        }
        out.append(ch);
        i = semi;
    }
    return out;
}

// Parse the attributes of a <video> opening tag (the full tag, e.g.
// "<video src=\"x.mp4\" autoplay loop/>"). Bare flags (autoplay, loop,
// muted) may appear without a value. Chat videos are ambient, so loop and
// autoplay default to on; a missing src fails the parse.
static bool parseVideoAttributes(const QString &openTag, QString &src, double &width,
                                 double &height, bool &muted, bool &loop, bool &autoplay)
{
    src.clear();
    width = -1;
    height = -1;
    muted = false;
    loop = true;
    autoplay = true;
    // The attribute body starts at the first whitespace — HTML allows a tab
    // (or any whitespace) after the tag name, not just a space.
    int bodyStart = -1;
    for (int i = 0; i < openTag.size(); ++i) {
        if (openTag.at(i).isSpace()) {
            bodyStart = i;
            break;
        }
    }
    if (bodyStart < 0)
        return false;
    // Strip only the tag terminators (">" and a self-closing "/" right
    // before it) — never slashes inside the quoted attribute values.
    QString body = openTag.mid(bodyStart);
    while (body.endsWith('>'))
        body.chop(1);
    if (body.endsWith('/'))
        body.chop(1);
    static const QRegularExpression attrRe(
        QStringLiteral("([A-Za-z_:][-A-Za-z0-9_:.]*)\\s*(?:=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s\"']+)))?"));
    QRegularExpressionMatchIterator it = attrRe.globalMatch(body);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString name = m.captured(1).toLower();
        QString value;
        if (!m.captured(2).isEmpty())
            value = m.captured(2);
        else if (!m.captured(3).isEmpty())
            value = m.captured(3);
        else if (!m.captured(4).isEmpty())
            value = m.captured(4);
        else
            value = QStringLiteral("true"); // bare flag
        if (name == QLatin1String("src"))
            src = decodeHtmlEntities(value);
        else if (name == QLatin1String("width"))
            width = value.toDouble();
        else if (name == QLatin1String("height"))
            height = value.toDouble();
        else if (name == QLatin1String("muted"))
            muted = true;
        else if (name == QLatin1String("loop"))
            loop = value != QLatin1String("false");
        else if (name == QLatin1String("autoplay"))
            autoplay = value != QLatin1String("false");
    }
    return !src.isEmpty();
}

// True if \a tag opens a <video> element: the name must be followed by a
// delimiter (whitespace, ">", a self-closing "/") or end the node — so that
// e.g. <videos> is not mistaken for <video>.
static bool isVideoOpenTag(std::string_view tag)
{
    if (!tag.starts_with("<video"))
        return false;
    if (tag.size() == 6)
        return true;
    const char c = tag[6];
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '>' || c == '/';
}

// Rewrite the root <svg> height/viewBox of the tightly-cropped SVGs emitted
// by katex2svg.js to \a newHeight. The content keeps its top anchoring, so
// the math baseline stays at the same offset from the top of the box.
static QString reboxSvgHeight(const QByteArray &svg, double newHeight)
{
    const QString s = QString::fromUtf8(svg);
    const int openEnd = s.indexOf('>');
    const int closeStart = s.lastIndexOf(QLatin1String("</svg>"));
    if (openEnd < 0 || closeStart <= openEnd)
        return s;
    const QString h = QString::number(newHeight, 'g', 10);
    QString head = s.left(openEnd);
    const int hAttr = head.indexOf(QLatin1String("height=\""));
    if (hAttr < 0)
        return s;
    const int hEnd = head.indexOf(QLatin1Char('"'), hAttr + 8);
    if (hEnd < 0)
        return s;
    head.replace(hAttr + 8, hEnd - (hAttr + 8), h);
    const int vb = head.indexOf(QLatin1String("viewBox=\""));
    if (vb >= 0) {
        const int vbEnd = head.indexOf(QLatin1Char('"'), vb + 9);
        if (vbEnd > 0) {
            QString vbStr = head.mid(vb + 9, vbEnd - (vb + 9));
            const int lastSpace = vbStr.lastIndexOf(QLatin1Char(' '));
            if (lastSpace >= 0)
                vbStr = vbStr.left(lastSpace) + QLatin1Char(' ') + h;
            head.replace(vb + 9, vbEnd - (vb + 9), vbStr);
        }
    }
    return head + QLatin1Char('>') + s.mid(openEnd + 1, closeStart - (openEnd + 1))
           + QLatin1String("</svg>");
}

MarkdownRenderer::MarkdownRenderer(QWidget *parent)
    : QTextBrowser(parent)
{
    setReadOnly(true);

    // default palette
    m_colorMap[TextForeground] = QColor(0x1F2328);
    m_colorMap[BlockquoteLine] = QColor(0xd0d7de);
    m_colorMap[BlockquoteText] = QColor(0x656d76);
    m_colorMap[HorizontalRuler] = QColor(0xd0d7de);
    m_colorMap[TableBorder] = QColor(0xd0d7de);
    m_colorMap[TableOddRow] = QColor(0xf6f8fa);
    m_colorMap[TableEvenRow] = QColor(0xffffff);
    m_colorMap[CodeBlockBackground] = QColor(0xf6f8fa);
    m_colorMap[CodeBlockBorder] = m_colorMap[TableBorder];
    m_colorMap[InlineCodeBackground] = QColor(0xf6f8fa);
    m_colorMap[Link] = QColor(0x0969da);

    m_baseFont = QFont("SF Pro", 14);
    setFont(m_baseFont);
    m_monoFont = QFont("SF Mono");

    QPalette p = palette();
    p.setColor(QPalette::Text, color(TextForeground));
    setPalette(p);

    m_doc = new QTextDocument(this);
    m_doc->setIndentWidth(30);
    setDocument(m_doc);
    setupDocumentSettings();
    m_cursor = QTextCursor(m_doc);

    // Markus streaming parser setup (GitHub-flavored Markdown)
    m_options.enable_tables = true;
    m_options.enable_autolink = true;
    m_options.enable_strikethrough = true;
    m_options.enable_tasklist = true;
    m_options.enable_latex_math = true;
    m_streamParser.SetOptions(m_options);
    m_streamParser.setBlockCallback(
        [this](const markus::Document &doc, size_t first, size_t last) {
            renderBlocks(doc, first, last);
        });

    connect(verticalScrollBar(),
            &QScrollBar::valueChanged,
            this,
            &MarkdownRenderer::updateAllOverlaysGeometry);
    connect(horizontalScrollBar(),
            &QScrollBar::valueChanged,
            this,
            &MarkdownRenderer::updateAllOverlaysGeometry);
    // feed()/finish() run before the document has been laid out at its final
    // size, so the first overlay pass (video widgets in particular) usually
    // finds no fragment rects yet; re-run it when the layout completes.
    connect(m_doc->documentLayout(),
            &QAbstractTextDocumentLayout::documentSizeChanged,
            this,
            &MarkdownRenderer::updateAllOverlaysGeometry);
}

MarkdownRenderer::~MarkdownRenderer()
{
    qDeleteAll(m_codeOverlays);
}

void MarkdownRenderer::notifyGeometryChanged()
{
    updateGeometry();
}

void MarkdownRenderer::feed(const QByteArray &buffer)
{
    QByteArray delta = buffer;
    if (buffer.startsWith(m_buffer)) {
        delta = buffer.mid(m_buffer.size());
    } else {
        // The buffer diverged (e.g. a thinking section was rewritten):
        // start over and re-feed everything.
        reset();
        delta = buffer;
    }
    m_buffer = buffer;

    m_cursor.beginEditBlock();
    m_streamParser.Feed(std::string_view(delta.constData(), delta.size()));
    renderPendingTail();
    m_cursor.endEditBlock();

    updateAllOverlaysGeometry();
}

void MarkdownRenderer::finish()
{
    m_cursor.beginEditBlock();
    m_streamParser.Flush();
    renderPendingTail();
    m_cursor.endEditBlock();

    updateAllOverlaysGeometry();
}

void MarkdownRenderer::reset()
{
    m_streamParser.Reset();
    m_buffer.clear();

    qDeleteAll(m_codeOverlays);
    m_codeOverlays.clear();
    m_inlineCodeRanges.clear();
    // Keep m_toggleDetails: section ids are ordinal, so the user's
    // expand/collapse choices still apply to the re-rendered sections.
    m_nextDetailsId = 0;
    m_tailDetailsIndex = -1;
    m_listStack.clear();
    m_tableStack.clear();
    m_textCharFormatStack.clear();
    m_blockQuoteDepth = 0;
    m_detailsBodyDepth = 0;
    m_headingLevel = 0;
    m_codeBlock = false;
    m_codeBlockLanguage.clear();
    m_codeFenceChar = QChar::Null;
    m_skipNextParagraphBlock = false;
    m_tailStart = -1;
    // The document is cleared below, which invalidates every image URL, so
    // the stored SVGs (mermaid diagrams, math) can go with it: without this
    // the store would grow for the whole session (mermaid keys are unique
    // per render and never reused).
    // (m_diagramCache is kept on purpose: it mirrors the SVGs persisted
    // with the message, and a divergent re-feed must not lose them.)
    m_svgStore.clear();

    // Videos: the document is cleared below, so the placeholder fragments
    // (and with them the players' reason to exist) are gone. A re-feed
    // recreates them, restarting playback from the beginning.
    for (auto &video : m_videos) {
        if (video.player) {
            video.player->stop();
            delete video.player;
            video.player = nullptr;
        }
        if (video.widget) {
            delete video.widget;
            video.widget = nullptr;
        }
    }
    m_videos.clear();
    m_videoStore.clear();

    if (m_doc) {
        m_doc->clear();
        m_cursor = QTextCursor(m_doc);
    }
}

// ---------------------------------------------------------------------------
// AST rendering
// ---------------------------------------------------------------------------

void MarkdownRenderer::renderBlocks(const markus::Document &doc, size_t first, size_t last)
{
    clearTailRegion();
    for (size_t i = first; i < last; ++i)
        renderBlock(doc, doc.children[i]);

    // The blocks rendered above are now stable. Move the tail boundary to the
    // end so the renderPendingTail() that runs right after this does not treat
    // them as the (to-be-replaced) in-progress tail and wipe them out.
    m_tailStart = documentEndPosition();
}

void MarkdownRenderer::renderBlock(const markus::Document &doc, const markus::BlockNode &node)
{
    std::visit(
        [&](const auto &n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, markus::Paragraph>) {
                handleParagraph();
                renderInlines(doc, n.children);
            } else if constexpr (std::is_same_v<T, markus::Heading>) {
                handleHeading(n.level);
                renderInlines(doc, n.children);
                leaveHeading();
            } else if constexpr (std::is_same_v<T, markus::ThematicBreak>) {
                handleThematicBreak();
            } else if constexpr (std::is_same_v<T, markus::CodeBlock>) {
                handleCodeBlock(n);
            } else if constexpr (std::is_same_v<T, markus::DetailsBlock>) {
                renderDetails(doc, n);
            } else if constexpr (std::is_same_v<T, markus::HtmlBlock>) {
                renderGenericHtmlBlock(n);
            } else if constexpr (std::is_same_v<T, markus::BlockQuote>) {
                handleBlockQuote();
                renderBlockIds(doc, n.children);
                leaveBlockQuote();
            } else if constexpr (std::is_same_v<T, markus::List>) {
                handleList(n);
                for (const auto &item : n.items) {
                    handleItem(item);
                    renderBlockIds(doc, item.children);
                }
                leaveList();
            } else if constexpr (std::is_same_v<T, markus::ListItem>) {
                // Items are rendered as part of their List.
            } else if constexpr (std::is_same_v<T, markus::Table>) {
                renderTable(doc, n);
            }
        },
        node);
}

void MarkdownRenderer::renderBlockIds(const markus::Document &doc,
                                      const std::pmr::vector<markus::BlockNodeId> &ids)
{
    for (markus::BlockNodeId id : ids)
        renderBlock(doc, doc.block_nodes[id]);
}

// Net effect on tag-nesting depth of a single inline-HTML element as produced
// by markus: -1 for a closing tag, +1 for an opening tag, 0 for a self-closing
// tag or a self-contained construct (comment, CDATA, processing instruction,
// declaration).
static int inlineHtmlDepthChange(std::string_view tag)
{
    if (tag.size() < 2 || tag[0] != '<')
        return 0;
    if (tag[1] == '/')
        return -1;  // </tag>
    if (tag[1] == '?' || tag[1] == '!')
        return 0;   // <?...?>, <!--...-->, <![CDATA[...]]>, <!...>
    if (tag.back() == '>') {
        if (tag.size() >= 3 && tag[tag.size() - 2] == '/')
            return 0;  // <tag .../>
        return 1;      // <tag ...>
    }
    return 0;
}

void MarkdownRenderer::renderInlines(const markus::Document &doc,
                                     const std::pmr::vector<markus::InlineNodeId> &ids)
{
    size_t i = 0;
    while (i < ids.size()) {
        // Inline HTML (status icons, spinners) is split into individual tags by
        // markus, but Qt drops a tag that is not closed within a single
        // insertHtml call. Render a whole balanced element at once so it
        // survives; plain text and markdown are rendered node-by-node.
        if (std::holds_alternative<markus::HtmlInline>(doc.inline_nodes[ids[i]])) {
            size_t end = 0;
            if (renderInlineHtmlElement(doc, ids, i, end)) {
                i = end;
                continue;
            }
        }
        renderInline(doc, ids[i]);
        ++i;
    }
}

bool MarkdownRenderer::renderInlineHtmlElement(
    const markus::Document &doc,
    const std::pmr::vector<markus::InlineNodeId> &ids, size_t start, size_t &outEnd)
{
    // A <video> element has a dedicated rendering (a QVideoWidget overlay on
    // a placeholder image; QTextDocument has no native video support), so it
    // is detected before the generic balanced-element path below.
    if (auto *first = std::get_if<markus::HtmlInline>(&doc.inline_nodes[ids[start]]);
        first && isVideoOpenTag(first->content)) {
        QString openTag;
        int depth = 0;
        for (size_t i = start; i < ids.size(); ++i) {
            const markus::InlineNode &node = doc.inline_nodes[ids[i]];
            if (const auto *tag = std::get_if<markus::HtmlInline>(&node)) {
                const std::string_view t = tag->content;
                if (depth == 0)
                    openTag = QString::fromUtf8(t.data(), t.size());
                depth += inlineHtmlDepthChange(t);
                if (depth == 0) {
                    QString src;
                    double width = -1, height = -1;
                    bool muted = false, loop = false, autoplay = false;
                    if (parseVideoAttributes(openTag, src, width, height, muted, loop, autoplay)) {
                        renderVideoElement(src, width, height, muted, loop, autoplay);
                        outEnd = i + 1;
                        return true;
                    }
                    return false; // no usable src: fall back to per-node rendering
                }
                if (depth < 0)
                    return false;
            } else if (depth > 0) {
                // <video> content (if any) is a plain-text fallback
                // description in browsers; ignore it here.
                if (!std::get_if<markus::Text>(&node))
                    return false;
            } else {
                return false;
            }
        }
        return false; // never balanced within the available nodes
    }

    QString buffer;
    int depth = 0;
    for (size_t i = start; i < ids.size(); ++i) {
        const markus::InlineNode &node = doc.inline_nodes[ids[i]];
        if (const auto *html = std::get_if<markus::HtmlInline>(&node)) {
            const std::string_view tag = html->content;
            buffer += QString::fromUtf8(tag.data(), tag.size());
            depth += inlineHtmlDepthChange(tag);
            if (depth == 0) {
                outEnd = i + 1;
                // insertHtml() leaves the cursor's char format set to the last
                // inserted character (e.g. an icon font); restore the previous
                // format so the following markdown text is not rendered with it.
                const QTextCharFormat prev = m_cursor.charFormat();
                m_cursor.insertHtml(buffer);
                m_cursor.setCharFormat(prev);
                return true;
            }
            if (depth < 0)
                return false;  // a closing tag without a matching open
        } else if (depth > 0) {
            const auto *text = std::get_if<markus::Text>(&node);
            if (!text)
                return false;  // non-text markup inside the element
            buffer += escapeHtml(
                QString::fromUtf8(text->content.data(), text->content.size()));
        } else {
            return false;
        }
    }
    return false;  // never balanced within the available nodes
}

void MarkdownRenderer::renderInline(const markus::Document &doc, markus::InlineNodeId id)
{
    const markus::InlineNode &node = doc.inline_nodes[id];
    std::visit(
        [&](const auto &n) {
            using T = std::decay_t<decltype(n)>;
            if constexpr (std::is_same_v<T, markus::Text>) {
                m_cursor.insertText(QString::fromUtf8(n.content.data(), n.content.size()));
            } else if constexpr (std::is_same_v<T, markus::SoftBreak>) {
                m_cursor.insertText(QStringLiteral(" "));
            } else if constexpr (std::is_same_v<T, markus::HardBreak>) {
                m_cursor.insertText(QStringLiteral("\n"));
            } else if constexpr (std::is_same_v<T, markus::Code>) {
                // The padding spaces must keep the *surrounding* text's
                // format — capture it before handleInlineCode() pushes the
                // mono code format on the stack.
                const QTextCharFormat outerFormat
                    = m_textCharFormatStack.isEmpty()
                          ? QTextCharFormat()
                          : m_textCharFormatStack.top();
                handleInlineCode();
                if (m_headingLevel == 0) {
                    // The padding spaces around the span give the painted
                    // chip (paintInlineCodeChips()) real padding room — Qt
                    // 6.11 has no layout-level inline padding (the
                    // char-format edge spacing was removed, and nested
                    // frames break toPlainText()/clipboard) — without
                    // adding regular space characters to the text. copy()
                    // and ChatMessage::plainText() strip them again, so
                    // they never reach the clipboard or search matches.
                    // insertText(text, fmt) also sets the cursor's current
                    // char format, so the code content must be inserted
                    // with the mono format explicitly — after the first
                    // padding char the cursor would carry outerFormat.
                    const QTextCharFormat codeFormat = m_textCharFormatStack.top();
                    const int start = m_cursor.position();
                    m_cursor.insertText(InlineCodePadding, outerFormat);
                    m_cursor.insertText(fromStdString(n.content), codeFormat);
                    popCharFormat();
                    m_cursor.insertText(InlineCodePadding, outerFormat);
                    // Remember the range (including the padding) so
                    // paintEvent() can paint the rounded chip background.
                    m_inlineCodeRanges.append({start, m_cursor.position()});
                } else {
                    // Headings get the mono font (the cursor's current
                    // format, which handleInlineCode() set) but no chip
                    // background — the padding/chip path above is for
                    // regular text only.
                    m_cursor.insertText(fromStdString(n.content));
                    popCharFormat();
                }
            } else if constexpr (std::is_same_v<T, markus::Emphasis>) {
                handleEmph();
                renderInlines(doc, n.children);
                popCharFormat();
            } else if constexpr (std::is_same_v<T, markus::Strong>) {
                handleStrong();
                renderInlines(doc, n.children);
                popCharFormat();
            } else if constexpr (std::is_same_v<T, markus::Strikethrough>) {
                handleStrikethrough();
                renderInlines(doc, n.children);
                popCharFormat();
            } else if constexpr (std::is_same_v<T, markus::Link>) {
                handleLink(n);
                renderInlines(doc, n.children);
                popCharFormat();
            } else if constexpr (std::is_same_v<T, markus::Image>) {
                renderImage(n);
            } else if constexpr (std::is_same_v<T, markus::Math>) {
                renderMath(n);
            } else if constexpr (std::is_same_v<T, markus::HtmlInline>) {
                const QTextCharFormat prev = m_cursor.charFormat();
                m_cursor.insertHtml(QString::fromUtf8(n.content.data(), n.content.size()));
                m_cursor.setCharFormat(prev);
            }
        },
        node);
}

// ---------------------------------------------------------------------------
// Held-back tail (live preview of the block that may still grow)
// ---------------------------------------------------------------------------

int MarkdownRenderer::documentEndPosition() const
{
    if (!m_doc)
        return -1;
    QTextCursor cursor(m_doc);
    cursor.movePosition(QTextCursor::End);
    return cursor.position();
}

void MarkdownRenderer::renderPendingTail()
{
    clearTailRegion();

    // Mark where the in-progress tail starts. This is a plain document
    // position (not a QTextCursor), because a cursor left at this spot would be
    // pushed forward to the end by the insertions that follow, and the tail
    // would then never be cleared.
    m_tailStart = documentEndPosition();

    const std::string &pending = m_streamParser.pending();
    if (pending.empty())
        return;

    markus::Document tailDoc = markus::Parse(pending, m_options);
    m_tailDetailsIndex = 0;
    m_renderingTail = true;
    for (const markus::BlockNode &block : tailDoc.children)
        renderBlock(tailDoc, block);
    m_renderingTail = false;
    m_tailDetailsIndex = -1;
}

void MarkdownRenderer::clearTailRegion()
{
    if (!m_doc || m_tailStart < 0)
        return;
    QTextCursor cursor(m_doc);
    cursor.setPosition(qBound(0, m_tailStart, documentEndPosition()));
    cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
    if (cursor.hasSelection())
        cursor.removeSelectedText();
    pruneStaleCodeBlocks();
    // The wiped tail invalidates the chip ranges touching it (a code span
    // may run past m_tailStart and be re-rendered); ranges ending before
    // m_tailStart keep their positions.
    m_inlineCodeRanges.erase(
        std::remove_if(m_inlineCodeRanges.begin(), m_inlineCodeRanges.end(),
                       [this](const InlineCodeRange &r) { return r.end > m_tailStart; }),
        m_inlineCodeRanges.end());
}

void MarkdownRenderer::pruneStaleCodeBlocks()
{
    for (auto it = m_codeOverlays.begin(); it != m_codeOverlays.end();) {
        if (blockForCodeId(it.key()).isValid()) {
            ++it;
            continue;
        }
        delete it.value();
        it = m_codeOverlays.erase(it);
    }
}

// ---------------------------------------------------------------------------
// Block-level handlers
// ---------------------------------------------------------------------------

void MarkdownRenderer::beginBlock()
{
    QTextCharFormat charFmt;
    if (!m_textCharFormatStack.isEmpty())
        charFmt = m_textCharFormatStack.top();
    QTextBlockFormat blkFmt;
    if (!m_listStack.isEmpty())
        blkFmt.setIndent(m_listStack.size());
    if (m_detailsSecId)
        blkFmt.setProperty(DetailsSectionIdProp, m_detailsSecId);
    if (m_blockQuoteDepth) {
        blkFmt.setProperty(QTextFormat::BlockQuoteLevel, m_blockQuoteDepth);
        blkFmt.setLeftMargin(getBlockQuoteMargin(m_blockQuoteDepth, m_paragraphMargin));
    } else if (m_detailsBodyDepth) {
        // Indent the non-quoted <details> body (tool output); no quote level,
        // so paintEvent() draws no vertical line.
        blkFmt.setProperty(DetailsBodyIndentProp, m_detailsBodyDepth);
        blkFmt.setLeftMargin(getDetailsBodyMargin(m_detailsBodyDepth));
    }
    if (m_codeBlock) {
        blkFmt.setProperty(QTextFormat::BlockCodeLanguage, m_codeBlockLanguage);
        if (!m_codeFenceChar.isNull()) {
            blkFmt.setNonBreakableLines(true);
            blkFmt.setProperty(QTextFormat::BlockCodeFence, QString(m_codeFenceChar));
        }
        charFmt.setFont(m_monoFont);
    } else {
        blkFmt.setTopMargin(m_paragraphMargin);
        blkFmt.setBottomMargin(m_paragraphMargin);
    }

    if (m_cursor.document()->isEmpty()) {
        m_cursor.setBlockFormat(blkFmt);
        m_cursor.setCharFormat(charFmt);
    } else {
        m_cursor.insertBlock(blkFmt, charFmt);
    }
}

void MarkdownRenderer::handleHeading(int level)
{
    m_headingLevel = level;
    beginBlock();
    QTextBlockFormat blkFmt = m_cursor.blockFormat();
    blkFmt.setHeadingLevel(level);
    blkFmt.setTopMargin(24);
    blkFmt.setBottomMargin(16);
    m_cursor.setBlockFormat(blkFmt);

    QTextCharFormat chFmt = m_cursor.charFormat();
    static const double mult[6] = {2.0, 1.5, 1.25, 1.0, 0.875, 0.85};
    chFmt.setFontPointSize(m_baseFontSize * mult[level - 1]);
    chFmt.setFontWeight(QFont::Bold);
    // Push the heading's char format so inline markup (e.g. `code`) inherits
    // the bold, scaled font instead of restarting from the default format.
    m_textCharFormatStack.push(chFmt);
    m_cursor.setCharFormat(chFmt);
}

void MarkdownRenderer::leaveHeading()
{
    if (!m_textCharFormatStack.isEmpty())
        m_textCharFormatStack.pop();
    if (!m_textCharFormatStack.isEmpty())
        m_cursor.setCharFormat(m_textCharFormatStack.top());
    else
        m_cursor.setCharFormat(QTextCharFormat());
    m_headingLevel = 0;
}

void MarkdownRenderer::handleParagraph()
{
    if (m_skipNextParagraphBlock) {
        m_skipNextParagraphBlock = false;
        return;
    }

    beginBlock();
    QTextBlockFormat blkFmt = m_cursor.blockFormat();
    blkFmt.setTopMargin(0);
    blkFmt.setBottomMargin(10);
    m_cursor.setBlockFormat(blkFmt);
}

void MarkdownRenderer::handleBlockQuote()
{
    ++m_blockQuoteDepth;
    QTextCharFormat fmt = m_textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                          : m_textCharFormatStack.top();
    fmt.setForeground(color(BlockquoteText));
    m_textCharFormatStack.push(fmt);
    m_cursor.setCharFormat(fmt);

    beginBlock();
    QTextBlockFormat blkFmt = m_cursor.blockFormat();
    blkFmt.setLeftMargin(m_baseFontSize);
    m_cursor.setBlockFormat(blkFmt);
    m_skipNextParagraphBlock = true;
}

void MarkdownRenderer::leaveBlockQuote()
{
    --m_blockQuoteDepth;
    if (!m_textCharFormatStack.isEmpty())
        m_textCharFormatStack.pop();
    if (!m_textCharFormatStack.isEmpty())
        m_cursor.setCharFormat(m_textCharFormatStack.top());
    else
        m_cursor.setCharFormat(QTextCharFormat());
}

void MarkdownRenderer::handleCodeBlock(const markus::CodeBlock &code)
{
    m_codeBlockLanguage = languageFromInfoString(code);

    // Diagram blocks are shown as a picture (a <details> section: the image
    // in the collapsed header, the source in the body).
    if (renderSvgCodeBlock(code))
        return;
    if (renderMermaidCodeBlock(code))
        return;

    renderCodeBlockText(code);
}

void MarkdownRenderer::renderCodeBlockText(const markus::CodeBlock &code)
{
    m_codeBlock = true;
    m_codeBlockLanguage = languageFromInfoString(code);
    m_codeFenceChar = code.fence_char ? QChar::fromLatin1(code.fence_char) : QChar::Null;

    beginBlock();

    int blockId = ++m_nextCodeBlockId;
    QTextBlock first = m_cursor.block();
    QTextBlockFormat fmt = first.blockFormat();
    fmt.setProperty(BlockCodeIdProp, blockId);
    fmt.setLineHeight(70, QTextBlockFormat::ProportionalHeight);
    fmt.setAlignment(Qt::AlignVCenter);
    // Qt creates one block per code line and inherits this format, so the
    // per-line top/bottom margins produce the vertical spacing between lines.
    fmt.setTopMargin(m_paragraphMargin);
    fmt.setBottomMargin(m_paragraphMargin);
    fmt.setProperty(QTextFormat::BlockCodeLanguage, m_codeBlockLanguage);
    applyHorizontalIndent(fmt);
    if (!m_codeFenceChar.isNull()) {
        fmt.setNonBreakableLines(true);
        fmt.setProperty(QTextFormat::BlockCodeFence, QString(m_codeFenceChar));
    }
    m_cursor.setBlockFormat(fmt);

    QTextCharFormat baseCharFmt = m_cursor.charFormat();
    baseCharFmt.setFont(m_monoFont, QTextCharFormat::FontPropertiesSpecifiedOnly);
    baseCharFmt.setFontFixedPitch(true);
    baseCharFmt.setFontPointSize(m_baseFontSize * 0.90);
    m_cursor.setCharFormat(baseCharFmt);

    const QString content = fromStdString(code.content);

    // Inside a quote (e.g. a thinking section body) the code is muted like
    // the surrounding text; only unquoted code gets syntax highlighting.
    // The "rounded margin" around the code is not created with invisible
    // spacer lines but by expanding the block's bounding rect when the
    // background is painted (see codeBlockBackgroundRect()).
    if (m_blockQuoteDepth == 0) {
        QVector<HighlightFragment> fragments;
        SyntaxHighlighter highlighter;
        highlighter.setDefinition(syntaxDefinitionForName(m_codeBlockLanguage));
        highlighter.highlight(content, baseCharFmt, fragments);
        if (fragments.isEmpty()) {
            m_cursor.insertText(content, baseCharFmt);
        } else {
            for (const HighlightFragment &fragment : fragments)
                m_cursor.insertText(fragment.text, fragment.format);
        }
    } else {
        m_cursor.insertText(content, baseCharFmt);
    }

    // Qt collapses the margins of adjacent blocks, so with the usual
    // m_paragraphMargin line margins the block ends up flush with the
    // neighbouring text. Double the outer margins (first line's top, last
    // line's bottom) to leave a clear gap around the block's background,
    // like GitHub's spacing between a code block and the surrounding text.
    {
        QTextBlock blk = first;
        while (blk.next().isValid()
               && blk.next().blockFormat().property(BlockCodeIdProp).toInt() == blockId)
            blk = blk.next();
        QTextBlockFormat topFmt = first.blockFormat();
        topFmt.setTopMargin(2 * m_paragraphMargin);
        QTextCursor(first).setBlockFormat(topFmt);
        QTextBlockFormat botFmt = blk.blockFormat();
        botFmt.setBottomMargin(2 * m_paragraphMargin);
        QTextCursor(blk).setBlockFormat(botFmt);
    }

    m_codeBlock = false;
    m_codeBlockLanguage.clear();
    m_codeFenceChar = QChar::Null;

    createOverlayForCodeBlock(blockId);
}

bool MarkdownRenderer::renderSvgCodeBlock(const markus::CodeBlock &code)
{
    // Only *complete* SVGs are rendered as images: while the block is still
    // streaming (no closing tag yet) it is shown as plain code, and a broken
    // SVG falls back to the code view so the source stays available.
    //
    // Detection is by content, not by the fence language: models (and the
    // write/apply_patch tool views) frequently show SVG source in unlabeled
    // fences, and any fenced block that *is* an SVG document is better shown
    // as a picture.
    const QByteArray bytes(code.content.data(), int(code.content.size()));
    const QString text = QString::fromUtf8(bytes).trimmed();
    if (!text.startsWith(QLatin1String("<svg"), Qt::CaseInsensitive)
        || !text.contains(QLatin1String("</svg>"), Qt::CaseInsensitive))
        return false;

    QSvgRenderer renderer(bytes);
    if (!renderer.isValid()) {
        qWarning() << "MarkdownRenderer: SVG code block is not a renderable SVG, "
                    << "showing it as code instead";
        return false;
    }
    // The URL derives from the content, so re-rendering the in-progress tail
    // reuses the same resource once the content stops changing.
    const QString key = QString::fromLatin1(
            QCryptographicHash::hash(bytes, QCryptographicHash::Md5).toHex().left(16));
    return renderDiagramAsDetails(code, bytes, QStringLiteral("llamasvg://") + key,
                                  Tr::tr("SVG image"));
}

bool MarkdownRenderer::renderMermaidCodeBlock(const markus::CodeBlock &code)
{
    if (m_renderingTail)
        return false; // still streaming: show the source, render on finalize
    const QString lang = languageFromInfoString(code).toLower();
    if (lang != QLatin1String("mermaid") && lang != QLatin1String("mmd"))
        return false;

    const QString source(fromStdString(code.content));
    if (source.trimmed().isEmpty())
        return false;

    // Match the diagram to the UI theme: mermaid's "dark" theme uses light
    // text on dark shapes, "default" the other way around. Dark *foreground*
    // text means a light UI.
    const QString theme = color(TextForeground).lightness() < 128 ? QStringLiteral("default")
                                                                  : QStringLiteral("dark");

    // A persisted render (Message.extra) for the current theme skips the
    // engine entirely: the picture appears immediately, no spinner, when a
    // conversation is reopened.
    const QString cacheKey = mermaidDiagramKey(source.trimmed());
    const auto cacheIt = m_diagramCache.constFind(cacheKey);
    if (cacheIt != m_diagramCache.constEnd() && cacheIt->context == theme)
        return renderDiagramAsDetails(code,
                                      cacheIt->svg,
                                      QStringLiteral("llamasvg://") + cacheKey,
                                      Tr::tr("Mermaid diagram"),
                                      "mermaid");

    // The render runs on the engine's worker thread; the block shows a busy
    // spinner until the SVG arrives (onMermaidRendered swaps it in), so a
    // heavy diagram never blocks the UI while a conversation is opened or
    // streamed.
    const QString key = QStringLiteral("mmd-") + QString::number(++m_nextMermaidKey);
    if (!renderDiagramAsDetails(code, QByteArray(), QStringLiteral("spinner://mermaid-") + key,
                                Tr::tr("Mermaid diagram"), "mermaid", key))
        return false;
    MermaidEngine::instance()->renderAsync(source, theme, this,
                                           [this, key, cacheKey, theme](const QByteArray &svg) {
                                               if (!svg.isEmpty()) {
                                                   // Persist with the message so the
                                                   // render work is not repeated on the
                                                   // next open (and the export can
                                                   // embed the picture).
                                                   m_diagramCache.insert(
                                                       cacheKey, DiagramSvg{svg, theme, 0, 0});
                                                   emit diagramRendered(
                                                       cacheKey, m_diagramCache.value(cacheKey));
                                               }
                                               onMermaidRendered(key, svg);
                                           });
    return true;
}

bool MarkdownRenderer::renderDiagramAsDetails(const markus::CodeBlock &code,
                                              const QByteArray &svg,
                                              const QString &imageUrl,
                                              const QString &summaryText,
                                              const std::string &bodyInfoString,
                                              const QString &pendingKey)
{
    if (!svg.isEmpty())
        m_svgStore.insert(imageUrl.section(QLatin1String("://"), 1, 1), svg);

    // Render as a <details> section: the picture in the (collapsed by
    // default) header, the source in the body.  Section ids are ordinal, so
    // the user's expand/collapse choice survives tail re-renders.
    const int prevSecId = m_detailsSecId;
    m_detailsSecId = nextDetailsId();
    const int secId = m_detailsSecId;

    bool visible = m_toggleDetails.contains(secId) ? m_toggleDetails.value(secId)
                                                   : false;
    m_toggleDetails.insert(secId, visible);

    // Header, two lines, in their own paragraphs after the surrounding
    // text: the picture centered on its own line, the label with the
    // direction icon below it.  Both blocks become clickable toggle blocks,
    // so clicking the picture expands the source, too.
    beginBlock();
    const int headerPos = documentEndPosition();
    QTextBlockFormat imgBlockFmt = m_cursor.blockFormat();
    imgBlockFmt.setAlignment(Qt::AlignHCenter);
    imgBlockFmt.setTopMargin(m_paragraphMargin);
    imgBlockFmt.setBottomMargin(0);
    applyHorizontalIndent(imgBlockFmt);
    if (!pendingKey.isEmpty())
        imgBlockFmt.setProperty(MermaidPendingKeyProp, pendingKey);
    m_cursor.setBlockFormat(imgBlockFmt);

    QTextImageFormat imgFmt;
    imgFmt.setName(imageUrl);
    m_cursor.insertImage(imgFmt);

    beginBlock();
    QTextBlockFormat labelBlockFmt = m_cursor.blockFormat();
    labelBlockFmt.setAlignment(Qt::AlignHCenter);
    labelBlockFmt.setTopMargin(0);
    labelBlockFmt.setBottomMargin(m_paragraphMargin);
    applyHorizontalIndent(labelBlockFmt);
    m_cursor.setBlockFormat(labelBlockFmt);
    // insertImage() may leave the cursor's char format set to the image
    // format; the label text must not inherit it.
    m_cursor.setCharFormat(m_textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                           : m_textCharFormatStack.top());
    m_cursor.insertText(summaryText);
    // The direction icon goes last, like in regular details headers; restore
    // the char format afterwards (insertHtml leaves it set to the icon font).
    const QTextCharFormat prevIconFmt = m_cursor.charFormat();
    m_cursor.insertHtml(sectionIconHtml(secId, visible));
    m_cursor.setCharFormat(prevIconFmt);

    // Tag both header blocks as the section's clickable toggle blocks
    // (always visible, click toggles the body).
    const QTextBlock firstHeader = m_doc->findBlock(headerPos);
    const QTextBlock labelHeader = firstHeader.next();
    for (QTextBlock blk = firstHeader; blk.isValid() && blk != labelHeader.next();
         blk = blk.next()) {
        QTextBlockFormat hfmt = blk.blockFormat();
        hfmt.setProperty(DetailsSectionIdProp, secId);
        hfmt.setProperty(DetailsToggleBlockProp, true);
        if (blk == firstHeader)
            hfmt.setProperty(DetailsSummaryTextProp, summaryText);
        QTextCursor(blk).setBlockFormat(hfmt);
    }

    // Body: the source as a regular code block (syntax highlighting and the
    // copy overlay included); m_detailsSecId makes beginBlock() tag its
    // blocks with the section id.
    markus::CodeBlock source = code;
    // There is no dedicated SVG syntax definition; unlabeled SVG source is
    // highlighted as XML.
    source.info_string = bodyInfoString;
    renderCodeBlockText(source);

    // Show/hide the body blocks (they directly follow the header and carry
    // the section id; the header itself is a toggle block and is skipped by
    // toggleSection()).
    for (QTextBlock blk = m_doc->findBlock(headerPos);
         blk.isValid() && blk.blockFormat().property(DetailsSectionIdProp).toInt() == secId;
         blk = blk.next()) {
        if (blk.blockFormat().property(DetailsToggleBlockProp).toBool())
            continue;
        blk.setVisible(visible);
    }

    m_detailsSecId = prevSecId;
    return true;
}

int MarkdownRenderer::nextDetailsId()
{
    if (m_tailDetailsIndex >= 0) {
        const int id = m_nextDetailsId + m_tailDetailsIndex + 1;
        ++m_tailDetailsIndex;
        return id;
    }
    return ++m_nextDetailsId;
}

void MarkdownRenderer::applyHorizontalIndent(QTextBlockFormat &fmt) const
{
    if (m_blockQuoteDepth > 0) {
        fmt.setProperty(QTextFormat::BlockQuoteLevel, m_blockQuoteDepth);
        fmt.setLeftMargin(getBlockQuoteMargin(m_blockQuoteDepth, m_paragraphMargin)
                          + m_paragraphMargin);
    } else if (m_detailsBodyDepth > 0) {
        // Indent like a quote body, minus the vertical line (tool output).
        fmt.setProperty(DetailsBodyIndentProp, m_detailsBodyDepth);
        fmt.setLeftMargin(getDetailsBodyMargin(m_detailsBodyDepth) + m_paragraphMargin);
    } else {
        fmt.setLeftMargin(m_paragraphMargin);
    }
    if (!m_listStack.isEmpty())
        fmt.setIndent(m_listStack.size());
}

QByteArray MarkdownRenderer::svgContentForUrl(const QUrl &url) const
{
    if (url.scheme() != QLatin1String("llamasvg"))
        return {};
    return m_svgStore.value(url.authority());
}

QString MarkdownRenderer::videoPathForUrl(const QUrl &url) const
{
    if (url.scheme() != QLatin1String("llamavideo"))
        return {};
    return m_videoStore.value(url.authority());
}

QVariant MarkdownRenderer::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String("llamavideo"))
        return videoPlaceholderImage();
    return QTextBrowser::loadResource(type, name);
}

QImage MarkdownRenderer::scaledImageForDisplay(const QImage &image,
                                               double maxWidth,
                                               qreal devicePixelRatio)
{
    if (image.isNull())
        return {};

    // Logical display size: fit the width, cap the height like the SVG
    // drawings.  Smaller images keep their native size (up-scaling a small
    // picture only makes it blurrier).
    double w = image.width();
    double h = image.height();
    const double scale
        = std::min({1.0, maxWidth / w, 600.0 / h});
    if (scale >= 1.0)
        return image;
    w *= scale;
    h *= scale;

    // Rasterize at device resolution so the picture stays crisp on Retina.
    const qreal dpr = qMax(devicePixelRatio, 1.0);
    const int targetWidth = qMax(1, qRound(w * dpr));

    // A single large down-scale resamples poorly (aliasing, smearing): halve
    // repeatedly until within a factor of two of the target, then take one
    // final smooth step.
    QImage stepped = image;
    while (stepped.width() > targetWidth * 2)
        stepped = stepped.scaledToWidth(stepped.width() / 2, Qt::FastTransformation);
    stepped = stepped.scaledToWidth(targetWidth, Qt::SmoothTransformation);
    stepped.setDevicePixelRatio(dpr);
    return stepped;
}

// ---------------------------------------------------------------------------
// Persistent diagram cache (mermaid + math SVGs, stored with the message)
// ---------------------------------------------------------------------------

QString MarkdownRenderer::mermaidDiagramKey(const QString &source)
{
    // No colon in the prefix: the key doubles as the authority of the
    // "llamasvg://" image URL, which QUrl::authority() must parse back.
    return QStringLiteral("mermaid-") + QString::fromLatin1(
            QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Md5).toHex().left(16));
}

QString MarkdownRenderer::katexDiagramKey(const QString &tex, bool display)
{
    QByteArray input = tex.toUtf8();
    input.append(display ? '\1' : '\2');
    return QStringLiteral("katex-") + QString::fromLatin1(
            QCryptographicHash::hash(input, QCryptographicHash::Md5).toHex().left(16));
}

void MarkdownRenderer::seedDiagramCache(const QString &key, const DiagramSvg &entry)
{
    if (!key.isEmpty() && !entry.svg.isEmpty())
        m_diagramCache.insert(key, entry);
}

QMap<QString, MarkdownRenderer::DiagramSvg> MarkdownRenderer::diagramSvgsFromExtra(
    const QList<QVariantMap> &extra)
{
    QMap<QString, DiagramSvg> diagrams;
    for (const QVariantMap &e : extra) {
        if (e.value("type").toString() != QLatin1String("diagram"))
            continue;
        DiagramSvg svg;
        svg.svg = QByteArray::fromBase64(e.value("svg").toString().toLatin1());
        svg.context = e.value("context").toString();
        if (!svg.svg.isEmpty())
            diagrams.insert(e.value("key").toString(), svg);
    }
    return diagrams;
}

QString MarkdownRenderer::embedDiagramSvgs(QString content, const QMap<QString, DiagramSvg> &diagrams)
{
    if (diagrams.isEmpty())
        return content;

    auto lookup = [&diagrams](const QString &key) -> const DiagramSvg * {
        const auto it = diagrams.constFind(key);
        return it == diagrams.constEnd() ? nullptr : &it.value();
    };

    auto replaceMatches = [](const QString &text,
                             const QRegularExpression &re,
                             const std::function<QString(const QRegularExpressionMatch &)> &fn) {
        QString out;
        int lastEnd = 0;
        auto it = re.globalMatch(text);
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += text.mid(lastEnd, m.capturedStart() - lastEnd);
            out += fn(m);
            lastEnd = m.capturedEnd();
        }
        out += text.mid(lastEnd);
        return out;
    };

    // ```mermaid blocks -> <details> section with the rendered picture in
    // the header and the source in the body (mirroring the chat UI).
    const QRegularExpression mermaidRe(
        QStringLiteral("```(?:mermaid|mmd)\\s*\\n(.*?)\\n?```"),
        QRegularExpression::DotMatchesEverythingOption);
    content = replaceMatches(content, mermaidRe, [lookup](const QRegularExpressionMatch &m) -> QString {
        const DiagramSvg *svg = lookup(mermaidDiagramKey(m.captured(1).trimmed()));
        if (!svg)
            return m.captured(0);
        return QStringLiteral("<details>\n<summary>Mermaid diagram</summary>\n\n")
               + QString::fromUtf8(svg->svg) + QStringLiteral("\n\n```mermaid\n")
               + m.captured(1).trimmed() + QStringLiteral("\n```\n\n</details>");
    });

    // Math spans -> inline SVGs. Display math first (it contains the inline
    // delimiters); spans without a cached SVG are kept verbatim.
    const QRegularExpression displayRe(
        QStringLiteral("\\$\\$\\s*(.*?)\\s*\\$\\$"), QRegularExpression::DotMatchesEverythingOption);
    content = replaceMatches(content, displayRe, [lookup](const QRegularExpressionMatch &m) {
        const DiagramSvg *svg = lookup(katexDiagramKey(m.captured(1).trimmed(), true));
        return svg ? QString::fromUtf8(svg->svg) : m.captured(0);
    });
    // The content must not start or end with whitespace, and the delimiters
    // must not be escaped or part of a $$ pair (plain-dollar prices like
    // "$5 and $10" must stay text).
    // No (?<!\$) lookbehind: Qt's regex engine does not support it, and
    // $$ pairs are excluded anyway (the content cannot start or end with a
    // $ or whitespace, and the closing delimiter is not followed by a $).
    const QRegularExpression inlineRe(QStringLiteral(
        "(?<!\\\\)\\$([^\\s$](?:[^$\\n]*[^\\s$])?)\\$(?<!\\\\)(?!\\$)"));
    content = replaceMatches(content, inlineRe, [lookup](const QRegularExpressionMatch &m) {
        const DiagramSvg *svg = lookup(katexDiagramKey(m.captured(1), false));
        return svg ? QString::fromUtf8(svg->svg) : m.captured(0);
    });

    return content;
}

void MarkdownRenderer::onMermaidRendered(const QString &key, const QByteArray &svg)
{
    // The spinner placeholder block carries the pending key; a missing block
    // means the document was reset/re-fed since the render was requested.
    QTextBlock pendingBlock;
    for (QTextBlock blk = m_doc->firstBlock(); blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(MermaidPendingKeyProp).toString() == key) {
            pendingBlock = blk;
            break;
        }
    }
    if (!pendingBlock.isValid())
        return;

    // The spinner block is addressed by explicit position ranges: the
    // image character is counted inconsistently by QTextBlock::length() and
    // select(BlockUnderCursor) leaves a dangling character behind, so the
    // content extent is derived from the fragments instead.
    const int pos = pendingBlock.position();
    int contentEnd = pos;
    for (auto it = pendingBlock.begin(); it != pendingBlock.end(); ++it)
        contentEnd = qMax(contentEnd, it.fragment().position() + it.fragment().length());
    QTextCursor cur(m_doc);
    cur.setPosition(pos);
    m_cursor.beginEditBlock();
    if (svg.isEmpty()) {
        // Invalid diagram: drop the spinner line; the source stays available
        // in the (collapsed) details body. Select the block's content plus
        // its paragraph separator (clamped for the document's last block).
        const int sepEnd = pendingBlock.next().isValid()
                                ? pendingBlock.next().position()
                                : m_doc->characterCount() - 1;
        cur.setPosition(sepEnd, QTextCursor::KeepAnchor);
        cur.removeSelectedText();
        // The document's final block cannot be deleted; if its shell
        // survives, make sure it is no longer tagged as pending.
        const QTextBlock shell = m_doc->findBlock(qMin(pos, m_doc->characterCount() - 1));
        if (shell.isValid()
            && shell.blockFormat().property(MermaidPendingKeyProp).toString() == key) {
            QTextBlockFormat fmt = shell.blockFormat();
            fmt.clearProperty(MermaidPendingKeyProp);
            cur.setPosition(qMin(pos, m_doc->characterCount() - 1));
            cur.setBlockFormat(fmt);
        }
    } else {
        m_svgStore.insert(key, svg);
        // Swap the spinner (just its content, the paragraph separator stays)
        // for the picture; the document fetches "llamasvg://key" lazily via
        // loadResource() on the next paint.
        QTextImageFormat imgFmt;
        imgFmt.setName(QStringLiteral("llamasvg://") + key);
        cur.setPosition(contentEnd, QTextCursor::KeepAnchor);
        cur.insertText(QString(QChar(0xFFFC)), imgFmt);
        // No longer in flight: clear the pending marker.
        const QTextBlock blk = m_doc->findBlock(pos);
        if (blk.isValid()) {
            QTextBlockFormat fmt = blk.blockFormat();
            fmt.clearProperty(MermaidPendingKeyProp);
            cur.setPosition(pos);
            cur.setBlockFormat(fmt);
        }
    }
    m_cursor.endEditBlock();
    updateAllOverlaysGeometry();
}

void MarkdownRenderer::handleThematicBreak()
{
    beginBlock();
    QTextBlockFormat blkFmt = m_cursor.blockFormat();
    blkFmt.setProperty(HorizontalRulerIdProp, 1);
    m_cursor.setBlockFormat(blkFmt);
    // Insert a zero space character so the block keeps its height.
    m_cursor.insertText(ZeroWidthSpace);
}

void MarkdownRenderer::handleList(const markus::List &list)
{
    ListState ls;
    ls.list = nullptr;
    ls.fmt.setIndent(m_listStack.size());
    if (!list.is_ordered) {
        ls.fmt.setStyle(list.bullet_char == '*' ? QTextListFormat::ListCircle
                                                : (list.bullet_char == '+'
                                                       ? QTextListFormat::ListSquare
                                                       : QTextListFormat::ListDisc));
    } else {
        ls.fmt.setStyle(m_listStack.isEmpty() ? QTextListFormat::ListDecimal
                                              : QTextListFormat::ListLowerRoman);
        ls.fmt.setStart(list.start);
    }
    m_listStack.append(ls);
}

void MarkdownRenderer::leaveList()
{
    if (!m_listStack.isEmpty())
        m_listStack.removeLast();
    m_skipNextParagraphBlock = false;
}

void MarkdownRenderer::handleItem(const markus::ListItem &item)
{
    beginBlock();
    m_skipNextParagraphBlock = true;
    QTextBlockFormat blkFmt = m_cursor.blockFormat();
    blkFmt.setTopMargin(static_cast<int>(m_baseFontSize * 0.25));
    blkFmt.setBottomMargin(0);
    if (item.is_tasklist)
        blkFmt.setMarker(item.tasklist_checked ? QTextBlockFormat::MarkerType::Checked
                                               : QTextBlockFormat::MarkerType::Unchecked);
    m_cursor.setBlockFormat(blkFmt);

    if (!m_listStack.isEmpty()) {
        ListState &ls = m_listStack.last();
        if (!ls.list) {
            ls.list = m_cursor.createList(ls.fmt);
        } else {
            if (!m_cursor.document()->isEmpty())
                ls.list->add(m_cursor.block());
        }
    }
}

// Recursively extract the markup-free plain text of a run of inline nodes.
static QString inlinePlainText(const markus::Document &doc,
                               const std::pmr::vector<markus::InlineNodeId> &ids)
{
    QString out;
    for (markus::InlineNodeId id : ids) {
        const markus::InlineNode &node = doc.inline_nodes[id];
        std::visit(
            [&](const auto &n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, markus::Text>) {
                    out += QString::fromUtf8(n.content.data(), n.content.size());
                } else if constexpr (std::is_same_v<T, markus::Code>) {
                    out += fromStdString(n.content);
                } else if constexpr (std::is_same_v<T, markus::SoftBreak>) {
                    out += QChar::Space;
                } else if constexpr (std::is_same_v<T, markus::HardBreak>) {
                    out += QLatin1String("\n");
                } else if constexpr (std::is_same_v<T, markus::Emphasis>
                                     || std::is_same_v<T, markus::Strong>
                                     || std::is_same_v<T, markus::Strikethrough>
                                     || std::is_same_v<T, markus::Link>) {
                    out += inlinePlainText(doc, n.children);
                }
            },
            node);
    }
    return out;
}

// Markup-free plain text of a <details> <summary> (a mini block document),
// stored on the section header so the summary is available when it is clicked.
static QString summaryPlainText(const markus::Document &doc,
                                const std::pmr::vector<markus::BlockNodeId> &ids)
{
    QString out;
    for (markus::BlockNodeId id : ids) {
        const markus::BlockNode &node = doc.block_nodes[id];
        std::visit(
            [&](const auto &n) {
                using T = std::decay_t<decltype(n)>;
                if constexpr (std::is_same_v<T, markus::Paragraph>
                              || std::is_same_v<T, markus::Heading>) {
                    out += inlinePlainText(doc, n.children);
                } else if constexpr (std::is_same_v<T, markus::CodeBlock>) {
                    out += fromStdString(n.content);
                } else if constexpr (std::is_same_v<T, markus::HtmlBlock>) {
                    out += fromStdString(n.content);
                }
            },
            node);
        out += QChar::Space;  // separate consecutive summary blocks
    }
    return out.trimmed();
}

void MarkdownRenderer::renderDetails(const markus::Document &doc,
                                      const markus::DetailsBlock &details)
{
    // Section ids are ordinal (document order). The in-progress tail is
    // re-rendered on every feed, so its sections get the ids they will have
    // once finalized – user expand/collapse choices survive re-renders.
    const int secId = nextDetailsId();

    const QString summaryText
        = details.summary.empty() ? Tr::tr("Details")
                                  : summaryPlainText(doc, details.summary);
    // Tool calls are marked with a data-tool attribute on the <details> tag
    // (markus keeps the opening tag's attributes); collapse them by default.
    // Thinking sections and other details use m_expandDetailsByDefault.
    bool isToolCall = false;
    for (const auto &attribute : details.attributes) {
        const QString name = fromStdString(attribute.first);
        if (name == QLatin1String("data-tool")) {
            const QString value = fromStdString(attribute.second);
            if (value.isEmpty() || value == QLatin1String("true")) {
                isToolCall = true;
                break;
            }
        }
    }
    
    // Tool calls are collapsed by default; thinking sections respect user setting
    bool visible = m_toggleDetails.contains(secId) 
                   ? m_toggleDetails.value(secId)
                   : (isToolCall ? !m_collapseToolCallsByDefault : m_expandDetailsByDefault);
    
    if (!m_toggleDetails.contains(secId))
        m_toggleDetails.insert(secId, visible);

    const int prevSecId = m_detailsSecId;
    m_detailsSecId = secId;

    // The <summary> content is markdown; render it as the section's clickable,
    // always-visible header. While rendering it, m_detailsSecId makes
    // beginBlock() tag every header block with the section id.
    const int firstTogglePos = documentEndPosition();
    if (details.summary.empty()) {
        beginBlock();
        m_cursor.insertText(Tr::tr("Details"));
    } else {
        renderBlockIds(doc, details.summary);
    }
    // Capture the header block range as block references (stable across the
    // icon append below, which only grows the last block).
    const QTextBlock firstToggle = m_doc->findBlock(firstTogglePos);
    const QTextBlock lastToggle = m_cursor.block();

    // Append the expand/collapse direction icon at the END of the last header
    // block, i.e. after the summary text (layout: [status icon][summary][dir]).
    // m_cursor is already at the end of the summary, so appending here also
    // keeps it positioned for the body rendered next. Restore the char format
    // afterwards: insertHtml() leaves it set to the icon font, which would
    // otherwise tint any following text.
    const QTextCharFormat prevIconFmt = m_cursor.charFormat();
    m_cursor.insertHtml(sectionIconHtml(secId, visible));
    m_cursor.setCharFormat(prevIconFmt);

    // Mark every header block as a toggle block (clickable, never hidden).
    for (QTextBlock blk = firstToggle; blk.isValid(); blk = blk.next()) {
        QTextBlockFormat fmt = blk.blockFormat();
        fmt.setProperty(DetailsSectionIdProp, secId);
        fmt.setProperty(DetailsToggleBlockProp, true);
        if (blk == firstToggle)
            fmt.setProperty(DetailsSummaryTextProp, summaryText);
        QTextCursor cursor(blk);
        cursor.setBlockFormat(fmt);
        if (blk == lastToggle)
            break;
    }

    // Body: markdown blocks parsed from the section. Thinking sections and
    // other details get an extra quote level, which indents the content and
    // makes paintEvent() draw the quote line. Tool-call output is only
    // indented (m_detailsBodyDepth): no quote line, normal text color, and
    // its code blocks still get syntax highlighting (see handleCodeBlock()).
    const bool quoted = !isToolCall;
    const int prevQuoteDepth = m_blockQuoteDepth;
    const int prevBodyDepth = m_detailsBodyDepth;
    if (quoted) {
        m_blockQuoteDepth += 1;
        QTextCharFormat quoteCharFmt;
        quoteCharFmt.setForeground(color(BlockquoteText));
        m_textCharFormatStack.push(quoteCharFmt);
    } else {
        m_detailsBodyDepth += 1;
    }

    renderBlockIds(doc, details.children);

    if (quoted) {
        m_textCharFormatStack.pop();
        m_blockQuoteDepth = prevQuoteDepth;
    } else {
        m_detailsBodyDepth = prevBodyDepth;
    }
    m_detailsSecId = prevSecId;

    // Show/hide the body blocks: those tagged with this section id that are not
    // toggle (header) blocks. Nested sections carry their own id and were
    // already handled by their own renderDetails().
    QTextBlock firstBody, lastBody;
    for (QTextBlock blk = firstToggle; blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(DetailsSectionIdProp).toInt() != secId)
            continue;
        if (blk.blockFormat().property(DetailsToggleBlockProp).toBool())
            continue;
        blk.setVisible(visible);
        if (!firstBody.isValid())
            firstBody = blk;
        lastBody = blk;
    }

    // Add a small vertical gap between the header (summary) and the expanded
    // content, and between the content and the next sibling. The extra bottom
    // margin is only in effect while the body is visible, so collapsed
    // sections are unaffected.
    if (firstBody.isValid()) {
        QTextBlockFormat fmt = firstBody.blockFormat();
        fmt.setTopMargin(fmt.topMargin() + m_paragraphMargin);
        QTextCursor(firstBody).setBlockFormat(fmt);
    }
    if (lastBody.isValid()) {
        QTextBlockFormat fmt = lastBody.blockFormat();
        fmt.setBottomMargin(fmt.bottomMargin() + m_paragraphMargin);
        QTextCursor(lastBody).setBlockFormat(fmt);
    }
}

void MarkdownRenderer::renderGenericHtmlBlock(const markus::HtmlBlock &block)
{
    QString text = fromStdString(block.content);
    if (text.endsWith('\n'))
        text.chop(1);
    if (text.trimmed().isEmpty())
        return;
    beginBlock();
    m_cursor.insertText(text);
}

void MarkdownRenderer::renderTable(const markus::Document &doc, const markus::Table &table)
{
    const int rows = static_cast<int>(table.rows.size());
    const int cols = static_cast<int>(table.alignments.size());

    QTextTableFormat tblFmt;
    tblFmt.setBorder(1);
    tblFmt.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    tblFmt.setBorderBrush(QBrush(color(TableBorder)));
    tblFmt.setCellPadding(6);
    tblFmt.setCellSpacing(0);
    tblFmt.setTopMargin(10);
    tblFmt.setBottomMargin(10);

    QTextTable *qtTable = m_cursor.insertTable(rows, cols, tblFmt);

    TableState ts;
    ts.qtTable = qtTable;
    ts.columns = cols;
    ts.colAlign.resize(cols);
    for (int i = 0; i < cols; ++i) {
        switch (table.alignments[i]) {
        case markus::TableAlign::kCenter:
            ts.colAlign[i] = Qt::AlignHCenter;
            break;
        case markus::TableAlign::kRight:
            ts.colAlign[i] = Qt::AlignRight;
            break;
        default:
            ts.colAlign[i] = Qt::AlignLeft;
            break;
        }
    }
    ts.curRow = -1;
    ts.curCol = -1;
    m_tableStack.append(ts);

    for (const auto &row : table.rows) {
        TableState &s = m_tableStack.last();
        ++s.curRow;
        s.header = row.is_header;
        s.curCol = -1;
        for (const auto &cell : row.cells) {
            ++s.curCol;

            QTextTableCell qtCell = s.qtTable->cellAt(s.curRow, s.curCol);
            if (!qtCell.isValid())
                continue;

            QTextCursor cellCursor = qtCell.firstCursorPosition();
            QTextBlockFormat blkFmt = cellCursor.blockFormat();
            blkFmt.setAlignment(s.colAlign.value(s.curCol, Qt::AlignLeft));
            cellCursor.setBlockFormat(blkFmt);

            if (s.header) {
                QTextCharFormat fmt = cellCursor.charFormat();
                fmt.setFontWeight(QFont::Bold);
                cellCursor.setCharFormat(fmt);
            } else {
                QTextCharFormat cellFmt = qtCell.format();
                cellFmt.setBackground(s.curRow % 2 == 1 ? color(TableOddRow)
                                                        : color(TableEvenRow));
                qtCell.setFormat(cellFmt);
            }
            m_cursor = cellCursor;
            renderInlines(doc, cell.children);
        }
    }

    TableState done = m_tableStack.takeLast();
    m_cursor = done.qtTable->lastCursorPosition();
    m_cursor.movePosition(QTextCursor::End);
}

void MarkdownRenderer::renderImage(const markus::Image &img)
{
    QTextImageFormat imgFmt;
    imgFmt.setName(fromStdString(img.destination));
    if (!img.title.empty())
        imgFmt.setToolTip(fromStdString(img.title));
    m_cursor.insertImage(imgFmt);
}

void MarkdownRenderer::renderVideoElement(const QString &src, double width, double height,
                                          bool muted, bool loop, bool autoplay)
{
    // Local files only (a relative path resolves against the working
    // directory, like the local images the preview loads).
    const QString path = QFileInfo(src).absoluteFilePath();
    if (!QFile::exists(path)) {
        // No player for a missing file: leave a visible note instead of a
        // dead placeholder.
        m_cursor.insertText(QStringLiteral("[video not found: %1]").arg(src));
        return;
    }

    // The key derives from the file path only, so tail re-renders of the
    // same video reuse the image URL, the store entry and the player.
    const QString key = QStringLiteral("vid-") + QString::fromLatin1(
            QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Md5).toHex().left(16));
    m_videoStore.insert(key, path);

    {
        QTextImageFormat imgFmt;
        imgFmt.setName(QStringLiteral("llamavideo://") + key);
        if (width > 0)
            imgFmt.setWidth(width);
        if (height > 0)
            imgFmt.setHeight(height);
        imgFmt.setVerticalAlignment(QTextCharFormat::AlignBottom);
        // insertImage() may leave the cursor's char format set to the image
        // format; the following text must not inherit it.
        const QTextCharFormat prev = m_cursor.charFormat();
        m_cursor.insertImage(imgFmt);
        m_cursor.setCharFormat(prev);
    }

    Video &video = m_videos[key];
    if (!video.player) {
        video.player = new QMediaPlayer(this);
        video.player->setSource(QUrl::fromLocalFile(path));
        // Qt's loop counts start at 1 (Once); setLoops(0) is normalized to
        // Once, so "no loop" is Once and looping is Infinite.
        video.player->setLoops(loop ? QMediaPlayer::Loops::Infinite
                                    : QMediaPlayer::Loops::Once);
        QAudioOutput *audio = new QAudioOutput(this);
        audio->setMuted(muted);
        video.player->setAudioOutput(audio);
        // The QVideoWidget is a viewport child (like the code-block
        // overlays); updateAllOverlaysGeometry() keeps it over the
        // placeholder fragment.
        video.widget = new QVideoWidget(viewport());
        video.player->setVideoOutput(video.widget);
        video.widget->hide();

        // play() before the media is loaded is ignored by the backends
        // (the source is opened asynchronously): start once it is ready.
        QMediaPlayer *player = video.player;
        const auto tryPlay = [player, autoplay] {
            if (autoplay && player->mediaStatus() == QMediaPlayer::LoadedMedia
                && !player->isPlaying())
                player->play();
        };
        connect(video.player, &QMediaPlayer::mediaStatusChanged, video.player,
                [tryPlay](QMediaPlayer::MediaStatus) { tryPlay(); });
        tryPlay();
    }
}

QRectF MarkdownRenderer::videoFragmentRect(const QString &key) const
{
    const QString name = QStringLiteral("llamavideo://") + key;
    // The image character is located by scanning the document (QTextLayout's
    // fragment walker is private API in Qt 6; there is no cheaper public way
    // to find it).  The box Qt paints it with is cursorRect() of a cursor
    // that *selects* the character: for a selection cursorRect() covers the
    // whole selection, while a collapsed cursor would report only the caret
    // position, not the image box.  This runs on every scroll/resize, so the
    // scan reuses a single cursor.
    QTextCursor cursor(m_doc);
    // The last character (the paragraph separator) can never be the image;
    // skipping it also keeps the selection below in range.
    for (int pos = 0; pos + 1 < m_doc->characterCount(); ++pos) {
        // A *collapsed* cursor's charFormat() reports the preceding
        // character's format (except at a block's start), so the character
        // must be selected to query its own format — matching on a plain
        // setPosition() would hit one character too late and place the
        // overlay past the placeholder for images not at the line start.
        cursor.setPosition(pos);
        cursor.setPosition(pos + 1, QTextCursor::KeepAnchor);
        const QTextCharFormat cf = cursor.charFormat();
        if (!cf.isImageFormat() || cf.toImageFormat().name() != name)
            continue;

        const QTextBlock block = m_doc->findBlock(pos);
        if (!block.isVisible() || !block.layout())
            return {}; // collapsed <details> body

        const QRectF selRect = cursorRect(cursor);
        if (!selRect.isValid())
            return {}; // not laid out yet

        // Displayed size: the image format's explicit width/height, else the
        // served resource's logical size.
        const QTextImageFormat imgFmt = cf.toImageFormat();
        double w = imgFmt.width();
        double h = imgFmt.height();
        if (w <= 0 || h <= 0) {
            const QImage image = m_doc->resource(QTextDocument::ImageResource, QUrl(name))
                                     .value<QImage>();
            if (!image.isNull()) {
                if (w <= 0)
                    w = image.width() / image.devicePixelRatio();
                if (h <= 0)
                    h = image.height() / image.devicePixelRatio();
            }
        }
        if (w <= 0 || h <= 0)
            return {};
        return QRectF(selRect.left(), selRect.top(), w, h);
    }
    return {};
}

void MarkdownRenderer::renderMath(const markus::Math &math)
{
    const QString tex = fromStdString(math.content);

    // Render at the surrounding text size and colour; the SVG's own padding
    // keeps it clear of the neighbouring glyphs. Re-renders of the same
    // formula (tail re-renders, conversations reopens) hit the engine cache.
    // (Render at 1x: scaling a larger vector *down* into the raster averages
    // the stroke coverage and makes the glyphs look thin and washed out.)
    // Free-standing display math is scaled up so the formula reads as its
    // own line rather than blending into the text size.
    const double displayScale = math.display ? 1.25 : 1.0;
    const int fontSize = qMax(1, QFontInfo(m_baseFont).pixelSize());
    const QString mathColor = color(TextForeground).name();

    auto insertMathImage = [this](const QByteArray &svg, double width, double height, bool display) {
        // The URL derives from the rendered content, so re-rendering the
        // in-progress tail reuses the same resource (svgContentForUrl serves it).
        const QString key = QStringLiteral("ktx-") + QString::fromLatin1(
                QCryptographicHash::hash(svg, QCryptographicHash::Md5).toHex().left(16));
        m_svgStore.insert(key, svg);

        if (display) {
            // Display math arrives as a paragraph holding the single math
            // span: centre that block so the formula reads as its own line.
            QTextBlockFormat blkFmt = m_cursor.blockFormat();
            blkFmt.setAlignment(Qt::AlignHCenter);
            m_cursor.setBlockFormat(blkFmt);
        }

        QTextImageFormat imgFmt;
        imgFmt.setName(QStringLiteral("llamasvg://") + key);
        imgFmt.setWidth(width);
        imgFmt.setHeight(height);
        imgFmt.setVerticalAlignment(QTextCharFormat::AlignBottom);
        // insertImage() may leave the cursor's char format set to the image
        // format; the following text must not inherit it.
        const QTextCharFormat prev = m_cursor.charFormat();
        m_cursor.insertImage(imgFmt);
        m_cursor.setCharFormat(prev);
    };

    // The fill colour and the font size are baked into the SVG, so they are
    // part of the persisted entry's context: a theme or font change
    // re-renders the formula (cheap) and updates the entry in place.
    const QString cacheKey = katexDiagramKey(tex.trimmed(), math.display);
    const QString cacheContext = mathColor + QLatin1Char('|')
                                 + QString::number(int(fontSize * displayScale));
    const auto cacheIt = m_diagramCache.constFind(cacheKey);
    if (cacheIt != m_diagramCache.constEnd() && cacheIt->context == cacheContext) {
        insertMathImage(cacheIt->svg, cacheIt->width, cacheIt->height, math.display);
        return;
    }

    const KaTeXEngine::Rendered rendered = KaTeXEngine::instance()->render(
            tex, math.display, mathColor, int(fontSize * displayScale));
    if (rendered.svg.isEmpty()) {
        // Invalid math: keep the source visible, with its delimiters.
        const QString delim = math.display ? QStringLiteral("$$") : QString();
        m_cursor.insertText(delim + tex + delim);
        return;
    }

    // Re-box the tightly-cropped SVG so the math baseline lands on the
    // surrounding text's baseline. Qt places an inline image's bottom edge
    // on the text baseline by default (qtextlayout.cpp), and honours
    // verticalAlignment() = AlignBottom (bottom on the line's bottom edge).
    // So: extend the box below the content until its bottom sits one font
    // descent below the math baseline, and pin the image to the line bottom
    // -> math baseline == line bottom - descent == text baseline, even when
    // the line grows to fit a tall formula. All in the supersampled space
    // (the box is scaled back to 1x for the image format below).
    const QFontMetricsF textMetrics(m_baseFont);
    const double depth = rendered.height - rendered.baseline; // content below baseline
    const double bottomSpace = qMax(textMetrics.descent() * displayScale, depth);
    const double boxHeight = rendered.baseline + bottomSpace;
    const QByteArray reboxed = reboxSvgHeight(rendered.svg, boxHeight).toUtf8();

    // Persist with the message so the render work is not repeated on the
    // next open (and the export can embed the picture).
    m_diagramCache.insert(
        cacheKey, DiagramSvg{reboxed, cacheContext, rendered.width, boxHeight});
    emit diagramRendered(cacheKey, m_diagramCache.value(cacheKey));

    insertMathImage(reboxed, rendered.width, boxHeight, math.display);
}

// ---------------------------------------------------------------------------
// Inline handlers
// ---------------------------------------------------------------------------

void MarkdownRenderer::handleEmph()
{
    QTextCharFormat fmt = m_textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                          : m_textCharFormatStack.top();
    fmt.setFontItalic(true);
    m_textCharFormatStack.push(fmt);
    m_cursor.setCharFormat(fmt);
}

void MarkdownRenderer::handleStrong()
{
    QTextCharFormat fmt = m_textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                          : m_textCharFormatStack.top();
    fmt.setFontWeight(QFont::Bold);
    m_textCharFormatStack.push(fmt);
    m_cursor.setCharFormat(fmt);
}

void MarkdownRenderer::handleStrikethrough()
{
    QTextCharFormat fmt = m_textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                          : m_textCharFormatStack.top();
    fmt.setFontStrikeOut(true);
    m_textCharFormatStack.push(fmt);
    m_cursor.setCharFormat(fmt);
}

void MarkdownRenderer::handleInlineCode()
{
    QTextCharFormat fmt = m_textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                          : m_textCharFormatStack.top();
    fmt.setFont(m_monoFont, QTextCharFormat::FontPropertiesSpecifiedOnly);
    fmt.setFontFixedPitch(true);
    // Inside a heading, keep the heading's font size (and weight) so the
    // code scales with the heading; it gets the mono font but no chip
    // background. The chip is not set here in any case: paintEvent() paints
    // a rounded rect behind regular-text spans (see m_inlineCodeRanges).
    if (m_headingLevel == 0)
        fmt.setFontPointSize(m_baseFontSize * 0.90);
    m_textCharFormatStack.push(fmt);
    m_cursor.setCharFormat(fmt);
}

void MarkdownRenderer::handleLink(const markus::Link &link)
{
    QTextCharFormat fmt = m_textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                          : m_textCharFormatStack.top();
    fmt.setAnchor(true);
    fmt.setAnchorHref(fromStdString(link.destination));
    fmt.setForeground(color(Link));
    fmt.setFontUnderline(false);
    if (!link.title.empty())
        fmt.setToolTip(fromStdString(link.title));
    m_textCharFormatStack.push(fmt);
    m_cursor.setCharFormat(fmt);
}

void MarkdownRenderer::popCharFormat()
{
    if (!m_textCharFormatStack.isEmpty())
        m_textCharFormatStack.pop();

    if (!m_textCharFormatStack.isEmpty())
        m_cursor.setCharFormat(m_textCharFormatStack.top());
    else
        m_cursor.setCharFormat(QTextCharFormat());
}

QString MarkdownRenderer::languageFromInfoString(const markus::CodeBlock &code)
{
    const auto &info = code.info_string;
    size_t end = info.find_first_of(" \t");
    std::string_view lang = (end == std::string_view::npos) ? std::string_view(info)
                                                            : std::string_view(info).substr(0, end);
    return QString::fromUtf8(lang.data(), lang.size());
}

int MarkdownRenderer::getBlockQuoteMargin(int depth, int paragraphMargin)
{
    if (depth <= 0)
        return 0;
    const int extraPerLevel = 8;
    return paragraphMargin + extraPerLevel * (depth - 1);
}

// Left margin for a non-quoted <details> body (tool output). A flat, clearly
// visible step per level – unlike getBlockQuoteMargin(), whose first level is
// only a single paragraph margin.
int MarkdownRenderer::getDetailsBodyMargin(int depth)
{
    if (depth <= 0)
        return 0;
    return 16 * depth;
}

// ---------------------------------------------------------------------------
// Details sections
// ---------------------------------------------------------------------------

// Replace the expand/collapse direction glyph in the section header block \a blk.
// The glyph is identified by its "details-toggle" anchor (plus the icon font),
// which distinguishes it from any status icon that is part of the summary text.
// Returns true if the block carried the direction icon.
static bool setSectionIconGlyph(QTextBlock &blk, bool visible)
{
    const QChar glyph = visible ? QChar('M') : QChar('N');
    for (auto it = blk.begin(); it != blk.end(); ++it) {
        const QTextFragment frag = it.fragment();
        const QTextCharFormat fmt = frag.charFormat();
        if (!frag.isValid()
            || !fmt.anchorHref().startsWith(QLatin1String("details-toggle:")))
            continue;
        if (!fmt.fontFamilies().toStringList().contains(QLatin1String("heroicons_outline")))
            continue;
        QTextCursor cursor(blk);
        cursor.setPosition(frag.position());
        cursor.setPosition(frag.position() + frag.length(), QTextCursor::KeepAnchor);
        cursor.insertText(QString(glyph), fmt);
        return true;
    }
    return false;
}

void MarkdownRenderer::toggleSection(int secId)
{
    bool makeVisible = !m_toggleDetails.value(secId);

    // Show/hide the body blocks of this section (the header/toggle blocks stay).
    for (QTextBlock blk = document()->firstBlock(); blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(DetailsSectionIdProp).toInt() == secId
            && !blk.blockFormat().property(DetailsToggleBlockProp).toBool())
            blk.setVisible(makeVisible);
    }

    // Flip the expand/collapse icon in the section header (the first toggle
    // block that carries the icon).
    for (QTextBlock blk = document()->firstBlock(); blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(DetailsSectionIdProp).toInt() != secId
            || !blk.blockFormat().property(DetailsToggleBlockProp).toBool())
            continue;
        if (setSectionIconGlyph(blk, makeVisible))
            break;
    }

    m_toggleDetails[secId] = makeVisible;

    setupDocumentSettings();
    updateAllOverlaysGeometry();
    viewport()->update();

    document()->setTextWidth(viewport()->width());
    notifyGeometryChanged();
}

QString MarkdownRenderer::sectionIconHtml(int secId, bool isVisible) const
{
    QString icon = isVisible ? "M" : "N";
    return QString(
                   "&nbsp;<a href=\"details-toggle:%1\" style=\"text-decoration:none; color: %2\">"
                   "<span style=\"font-family: heroicons_outline\">%3</span></a>")
                   .arg(secId)
                   .arg(colorToRgba(color(TextForeground)))
                   .arg(icon);
}

// ---------------------------------------------------------------------------
// Code block overlays
// ---------------------------------------------------------------------------

// The "rounded margin" (padding) of the rounded code-block background around
// the code. blockBoundingRect() hugs the code text vertically (block margins
// are not part of it), so the background rect is expanded by this much above
// and below; the horizontal margin already exists as the code block's left
// margin (m_paragraphMargin), keeping the padding even on all sides. The
// expansion replaces the old invisible zero-width spacer lines. It stays
// within the inter-block margins (each code line carries a
// m_paragraphMargin top/bottom margin), so it never overlaps neighbouring
// text.
static QRectF codeBlockBackgroundRect(const QRectF &rect, int paragraphMargin)
{
    const qreal pad = qMax(0, paragraphMargin - 6);
    return rect.adjusted(0, -pad, 0, pad);
}

QPointF MarkdownRenderer::contentOffset() const
{
    return QPointF(-horizontalScrollBar()->value(), -verticalScrollBar()->value());
}

QRectF MarkdownRenderer::blockBoundingRect(const QTextBlock &block) const
{
    QRectF blockRect = document()->documentLayout()->blockBoundingRect(block);

    // When having codeblock in lists, ajust the indent
    QVariant idVar = block.blockFormat().property(BlockCodeIdProp);
    if (idVar.isValid()) {
        qreal dx = block.blockFormat().indent() * document()->indentWidth();
        if (block.blockFormat().hasProperty(QTextFormat::BlockQuoteLevel))
            dx += m_paragraphMargin;
        else
            // Non-quoted <details> body (tool output): shift the overlay by
            // the extra left margin (the base paragraph margin is already
            // part of the block's bounding rect).
            dx += getDetailsBodyMargin(block.blockFormat().property(DetailsBodyIndentProp).toInt());
        blockRect.adjust(dx, 0, dx, 0);
    }

    return blockRect;
}

QTextBlock MarkdownRenderer::blockForCodeId(int id) const
{
    for (QTextBlock blk = document()->firstBlock(); blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(BlockCodeIdProp).toInt() == id)
            return blk;
    }
    return QTextBlock();
}

void MarkdownRenderer::setupDocumentSettings()
{
    if (!m_doc)
        return;
    m_doc->setDefaultFont(m_baseFont);
    m_baseFontSize = m_baseFont.pointSizeF();
    m_paragraphMargin = m_baseFontSize * 2 / 3;

    if (viewport()->width() > 0)
        document()->setTextWidth(viewport()->width());
    notifyGeometryChanged();
}

void MarkdownRenderer::updateAllOverlaysGeometry()
{
    if (m_codeOverlays.isEmpty() && m_videos.isEmpty())
        return;
    const int margin = m_paragraphMargin / 2;
    const QPointF offset = contentOffset();
    for (auto it = m_codeOverlays.constBegin(); it != m_codeOverlays.constEnd(); ++it) {
        int blockId = it.key();
        QFrame *overlay = it.value();
        if (!overlay)
            continue;
        QTextBlock blk = blockForCodeId(blockId);
        if (!blk.isValid() || !blk.isVisible()) {
            overlay->hide();
            continue;
        }
        QRectF rect = blockBoundingRect(blk);
        QTextBlock last = blk;
        int lineCount = 1;
        while (true) {
            QTextBlock nxt = last.next();
            if (!nxt.isValid())
                break;
            if (nxt.blockFormat().property(BlockCodeIdProp).toInt() != blockId)
                break;
            last = nxt;
            ++lineCount;
        }
        if (last != blk) {
            QRectF lastRect = blockBoundingRect(last);
            rect.setBottom(lastRect.bottom());
        }
        rect = codeBlockBackgroundRect(rect, m_paragraphMargin);
        QRectF viewRect = rect.translated(offset);
        int x = static_cast<int>(viewRect.right() - overlay->width() - margin);
        // One-line blocks are barely taller than the button, so the fixed top
        // margin would push the button off-centre; centre it vertically there.
        const int topOffset = (lineCount == 1)
                                  ? qMax<qreal>(0.0, (viewRect.height() - overlay->height()) / 2)
                                  : margin;
        int y = static_cast<int>(viewRect.top() + topOffset);
        overlay->move(x, y);
        overlay->show();
    }

    // Video overlays: cover the placeholder fragment exactly (the rect is
    // already in viewport coordinates, unlike the code-block rects above).
    for (auto it = m_videos.constBegin(); it != m_videos.constEnd(); ++it) {
        QVideoWidget *widget = it.value().widget;
        if (!widget)
            continue;
        const QRectF fragRect = videoFragmentRect(it.key());
        if (fragRect.isEmpty()) {
            widget->hide();
            continue;
        }
        widget->setGeometry(fragRect.toAlignedRect());
        widget->show();
        widget->raise();
    }
}

void MarkdownRenderer::createOverlayForCodeBlock(int blockId)
{
    if (m_codeOverlays.contains(blockId))
        return;

    QFrame *overlay = new QFrame(viewport());
    overlay->setObjectName(QStringLiteral("CodeOverlay"));
    overlay->setAttribute(Qt::WA_TransparentForMouseEvents, false);

    overlay->setStyleSheet(QString("QToolButton { "
                                    "  background: none; "
                                    "  border: none; "
                                    "  padding: 2px;"
                                    "  font-family: heroicons_outline; "
                                    "  font-size: 15px; "
                                    "  color: %1; "
                                    "} "
                                    "QToolButton:hover { "
                                    "  font-size: 17px; "
                                    "}")
                                .arg(colorToRgba(color(TextForeground))));

    QHBoxLayout *hl = new QHBoxLayout(overlay);
    hl->setContentsMargins(0, 0, 5, 0);
    hl->setSpacing(10);

    QToolButton *copyBtn = new QToolButton(overlay);
    copyBtn->setText("E"); // Heroicon character for copy
    copyBtn->setToolTip(Tr::tr("Copy the code below to Clipboard"));
    hl->addWidget(copyBtn);

    overlay->setLayout(hl);
    overlay->hide();

    connect(copyBtn, &QToolButton::clicked, this, [this, blockId, copyBtn] {
        auto [code, formattedCode] = collectCodeById(blockId);
        if (code.isEmpty())
            return;

        emit copyClicked(code, formattedCode);

        const QString tip = Tr::tr("Copied to clipboard");
        QPoint globalPos = copyBtn->mapToGlobal(QPoint(0, copyBtn->height()));
        QToolTip::showText(globalPos, tip, copyBtn);
    });

    m_codeOverlays.insert(blockId, overlay);
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------

// The effective font of the character at \a pos (the char format's font
// properties on top of the document's default font).
static QFont inlineCodeCharFont(QTextDocument *doc, int pos)
{
    QFont f = doc->defaultFont();
    QTextCursor cur(doc);
    cur.setPosition(pos);
    const QFont ff = cur.charFormat().font();
    if (!ff.family().isEmpty())
        f.setFamily(ff.family());
    if (ff.pointSizeF() > 0)
        f.setPointSizeF(ff.pointSizeF());
    if (ff.weight() != QFont::Normal)
        f.setWeight(ff.weight());
    if (ff.italic())
        f.setItalic(true);
    if (ff.fixedPitch())
        f.setFixedPitch(true);
    return f;
}

// Width of the text [from, to), measured per character (Qt's line layout is
// the sum of the per-char advances at the default letter spacing).
static qreal inlineCodeTextWidth(QTextDocument *doc, int from, int to)
{
    qreal w = 0;
    QFontMetricsF fm(doc->defaultFont());
    QFont lastFont;
    for (int i = from; i < to; ++i) {
        const QFont f = inlineCodeCharFont(doc, i);
        if (f != lastFont) {
            fm = QFontMetricsF(f);
            lastFont = f;
        }
        w += fm.horizontalAdvance(doc->characterAt(i));
    }
    return w;
}

// Viewport-coordinate chip fragments, one per line a span wraps across.
static QVector<QRectF> inlineCodeChipLines(MarkdownRenderer *renderer, int start, int end)
{
    QVector<QRectF> lines;
    QTextDocument *doc = renderer->document();
    // cursorRect() is in viewport coordinates; the font-metrics fallback
    // below computes in document coordinates and needs the scroll offset.
    const qreal scrollX = renderer->horizontalScrollBar()->value();
    const qreal scrollY = renderer->verticalScrollBar()->value();
    for (QTextBlock blk = doc->findBlock(start); blk.isValid() && blk.position() < end;
         blk = blk.next()) {
        QTextLayout *layout = blk.layout();
        if (!layout)
            continue;
        const int blockStart = blk.position();
        const int blockEnd = blockStart + blk.length();
        for (int li = 0; li < layout->lineCount(); ++li) {
            const QTextLine line = layout->lineAt(li);
            const int lineStart = blockStart + line.textStart();
            const int lineEnd = lineStart + line.textLength();
            const int from = qMax(start, lineStart);
            const int to = qMin(end, lineEnd);
            if (from >= to)
                continue;
            // Leading edge of the first char, trailing edge of the last one.
            // The range already includes the padding spaces
            // (InlineCodePadding) on both sides of the code — that is the
            // chip's padding. The rect must not extend into the surrounding
            // text (a neighbouring word space stays outside as the visible
            // gap; extending into it would pad the chip twice).
            //
            // cursorRect() is the same mechanism Qt uses to paint the text
            // cursor, so its edges line up with the rendered text (lists,
            // quotes, indents, hard-wrapped continuation lines — the text
            // draws at the line origin there). Two quirks to work around:
            // - at a line end that is not the block end, cursorRect(to)
            //   reports the *next* line's start, not the trailing edge of
            //   this line's text — so take the previous char's cursor
            //   position plus that single char's advance (one metrics
            //   lookup, no per-line width summation that would drift with
            //   kerning/mixed fonts);
            // - with no usable cursor rect at all (missing layout), fall
            //   back to the per-character advances from the line start.
            // The vertical extent is the full line (the cursor rect can be
            // shorter than the line on tall lines), in viewport coordinates
            // like the x edges.
            const qreal y = layout->position().y() + line.rect().y() - scrollY;
            const qreal h = line.rect().height();
            QTextCursor c1(doc);
            c1.setPosition(from);
            const QRectF r1 = renderer->cursorRect(c1);
            qreal x2 = -1;
            if (to < lineEnd || to == blockEnd - 1) {
                // The cursor at `to` sits on this line (mid-line, or at the
                // last text char of the block where there is no next line):
                // its rect is the trailing edge.
                QTextCursor c2(doc);
                c2.setPosition(to);
                x2 = renderer->cursorRect(c2).left();
            } else {
                // `to` is a mid-block line end: cursorRect(to) would report
                // the next line's start. Take the previous char's cursor
                // position plus that single char's advance.
                QTextCursor c2(doc);
                c2.setPosition(to - 1);
                const QRectF r2 = renderer->cursorRect(c2);
                x2 = r2.left() + QFontMetricsF(inlineCodeCharFont(doc, to - 1))
                                .horizontalAdvance(doc->characterAt(to - 1));
            }
            if (r1.isNull() || x2 <= r1.left()) {
                const qreal lineX = layout->position().x() + line.rect().x();
                const qreal fx1 = lineX + inlineCodeTextWidth(doc, lineStart, from) - scrollX;
                const qreal fx2 = lineX + inlineCodeTextWidth(doc, lineStart, to) - scrollX;
                if (fx2 <= fx1)
                    continue;
                lines.append(QRectF(fx1, y - 2, fx2 - fx1, h + 4));
                continue;
            }
            lines.append(QRectF(r1.left(), y - 2, x2 - r1.left(), h + 4));
        }
    }
    return lines;
}

// A rect with independently rounded corners (radius clamped to the rect),
// so a chip wrapping across several lines only rounds the corners at the
// true start/end of the span and stays square where it continues onto the
// next/previous line.
static QPainterPath chipPath(const QRectF &r, bool roundTopLeft, bool roundTopRight,
                             bool roundBottomLeft, bool roundBottomRight, qreal radius)
{
    const qreal rad = qMin(radius, qMin(r.width(), r.height()) / 2);
    const qreal l = r.left(), t = r.top(), rr = r.right(), b = r.bottom();
    QPainterPath path;
    path.moveTo(l + (roundTopLeft ? rad : 0), t);
    path.lineTo(rr - (roundTopRight ? rad : 0), t);
    if (roundTopRight)
        path.arcTo(QRectF(rr - 2 * rad, t, 2 * rad, 2 * rad), 90, -90);
    path.lineTo(rr, b - (roundBottomRight ? rad : 0));
    if (roundBottomRight)
        path.arcTo(QRectF(rr - 2 * rad, b - 2 * rad, 2 * rad, 2 * rad), 0, -90);
    path.lineTo(l + (roundBottomLeft ? rad : 0), b);
    if (roundBottomLeft)
        path.arcTo(QRectF(l, b - 2 * rad, 2 * rad, 2 * rad), 270, -90);
    path.lineTo(l, t + (roundTopLeft ? rad : 0));
    if (roundTopLeft)
        path.arcTo(QRectF(l, t, 2 * rad, 2 * rad), 180, -90);
    path.closeSubpath();
    return path;
}

// Removes the chip padding so copied text reads like the source markdown:
// a padding run touching a real space collapses into that one space ("how
// \u2004\u2004code" -> "how code", "code\u2004\u2004 is" -> "code is"), and a run with no
// neighbouring space is deleted outright ("code\u2004\u2004." -> "code.").
static QString stripInlineCodePadding(QString text)
{
    text.replace(QRegularExpression(QStringLiteral("[ ]*\u2004+[ ]")),
                 QStringLiteral(" "));
    text.replace(QRegularExpression(QStringLiteral("\u2004+")), QString());
    return text;
}

// The rounded inline-code chip backgrounds (ranges recorded in
// renderInline(); one fragment per line a span wraps across).
void MarkdownRenderer::paintInlineCodeChips(QPainter &painter, const QRectF &visibleRect)
{
    if (m_inlineCodeRanges.isEmpty())
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(color(InlineCodeBackground));
    painter.setPen(Qt::NoPen);
    for (const InlineCodeRange &range : m_inlineCodeRanges) {
        if (range.end <= range.start)
            continue;
        const QVector<QRectF> lines = inlineCodeChipLines(this, range.start, range.end);
        for (int i = 0; i < lines.size(); ++i) {
            const QRectF &r = lines.at(i);
            if (!r.intersects(visibleRect))
                continue;
            // Round the corners at the true top/bottom of the chip: the top
            // corners on the first line, the bottom corners on the last one.
            // Middle lines of a wrapped span stay square so the fragments
            // read as one continuous chip.
            const bool topEnd = (i == 0);
            const bool bottomEnd = (i == lines.size() - 1);
            painter.drawPath(chipPath(r, topEnd, topEnd, bottomEnd, bottomEnd, 4));
        }
    }
    painter.restore();
}

void MarkdownRenderer::copySelection()
{
    const QTextCursor cur = textCursor();
    if (!cur.hasSelection())
        return;
    // The padding spaces around the inline-code chips are layout padding
    // only; keep them out of what the user copies.
    QMimeData *mime = new QMimeData;
    // QTextEdit::selectionToHtml() was removed in Qt 6.11; the fragment
    // API still renders the selection to HTML.
    mime->setHtml(stripInlineCodePadding(QTextDocumentFragment(cur).toHtml()));
    mime->setText(stripInlineCodePadding(cur.selectedText()));
    QGuiApplication::clipboard()->clear();
    QGuiApplication::clipboard()->setMimeData(mime);
}

void MarkdownRenderer::keyPressEvent(QKeyEvent *ev)
{
    // copy() is not virtual, so the stripped copy is hooked into the Copy
    // shortcut (also posted by the context-menu action) here.
    if (ev->matches(QKeySequence::Copy) && textCursor().hasSelection()) {
        copySelection();
        ev->accept();
        return;
    }
    QTextBrowser::keyPressEvent(ev);
}

void MarkdownRenderer::paintEvent(QPaintEvent *ev)
{
    QPainter painter(viewport());
    const QRectF visibleRect = viewport()->rect();
    QMap<int, QRectF> codeBlocksRects = collectBlockRects(BlockCodeIdProp);
    const int radius = 6;
    for (const QRectF &blkRect : codeBlocksRects) {
        QRectF viewRect = codeBlockBackgroundRect(blkRect, m_paragraphMargin).translated(contentOffset());
        if (!viewRect.intersects(visibleRect))
            continue;

        painter.setRenderHint(QPainter::Antialiasing, true);
        painter.setBrush(color(CodeBlockBackground));
        painter.setPen(Qt::NoPen);
        painter.drawRoundedRect(viewRect, radius, radius);

        QPen borderPen(color(CodeBlockBorder));
        borderPen.setWidth(1);
        painter.setPen(borderPen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(viewRect, radius, radius);
    }

    paintInlineCodeChips(painter, visibleRect);

    QPen headingPen(color(HorizontalRuler));
    headingPen.setWidth(1);
    painter.setPen(headingPen);
    for (QTextBlock blk = document()->begin(); blk.isValid(); blk = blk.next()) {
        int lvl = blk.blockFormat().headingLevel();
        if (lvl == 1 || lvl == 2) {
            QRectF blkRect = blockBoundingRect(blk);
            QRectF viewRect = blkRect.translated(contentOffset());
            if (!viewRect.intersects(visibleRect))
                continue;

            qreal bottom = viewRect.bottom() + 2;
            painter.drawLine(QPointF(viewRect.left(), bottom), QPointF(viewRect.right(), bottom));
        }
    }

    // Draw the block quote lines
    const int quoteStep = 8;
    const int lineWidth = 2;
    int maxDepth = 0;
    for (QTextBlock blk = document()->begin(); blk.isValid(); blk = blk.next()) {
        int d = blk.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt();
        if (d > maxDepth)
            maxDepth = d;
    }

    auto drawQuoteLine = [this, visibleRect, &painter](int depth,
                                                        const QTextBlock &segmentStart,
                                                        const QTextBlock &lastInSegment) {
        QRectF startRect = blockBoundingRect(segmentStart);
        QRectF endRect = blockBoundingRect(lastInSegment);

        qreal top = startRect.top();
        qreal bottom = endRect.bottom();

        // Calculate X: documentMargin + offset + (incremental steps for nesting)
        qreal x = document()->documentMargin() + ((depth - 1) * quoteStep);

        QRectF lineRect(x, top, lineWidth, bottom - top);
        QRectF viewRect = lineRect.translated(contentOffset());

        if (viewRect.intersects(visibleRect)) {
            painter.fillRect(viewRect, color(BlockquoteLine));
        }
    };

    for (int depth = 1; depth <= maxDepth; ++depth) {
        QTextBlock segmentStart;
        QTextBlock lastInSegment;
        bool inSegment = false;
        for (QTextBlock blk = document()->begin(); blk.isValid(); blk = blk.next()) {
            bool satisfies = blk.blockFormat().property(QTextFormat::BlockQuoteLevel).toInt()
                             >= depth;

            if (satisfies) {
                if (!inSegment) {
                    segmentStart = blk;
                    inSegment = true;
                }
                lastInSegment = blk;
            } else {
                if (inSegment) {
                    // We reached a block that breaks the quote; draw the line for the completed segment
                    drawQuoteLine(depth, segmentStart, lastInSegment);
                    inSegment = false;
                }
            }
        }
        // Don't forget to draw the last segment if the document ends with a quote
        if (inSegment) {
            drawQuoteLine(depth, segmentStart, lastInSegment);
        }
    }

    for (QTextBlock blk = document()->begin(); blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(HorizontalRulerIdProp).toInt() > 0) {
            QRectF blkRect = blockBoundingRect(blk);
            QRectF viewRect = blkRect.translated(contentOffset());
            if (!viewRect.intersects(visibleRect))
                continue;

            const int hrHeight = 2;
            const int hrMargin = (viewRect.height() - hrHeight) / 2;
            QRectF hrRect(viewRect.left(), viewRect.top() + hrMargin, viewRect.width(), hrHeight);
            painter.setBrush(color(HorizontalRuler));
            painter.setPen(Qt::NoPen);
            painter.drawRect(hrRect);
        }
    }

    QTextBrowser::paintEvent(ev);
}

void MarkdownRenderer::mousePressEvent(QMouseEvent *ev)
{
    m_pressPos = ev->position().toPoint();
    QTextCursor cur = cursorForPosition(ev->pos());
    if (!cur.isNull()) {
        QTextBlock blk = cur.block();
        if (blk.isValid()) {
            const QTextBlockFormat fmt = blk.blockFormat();
            if (fmt.property(DetailsToggleBlockProp).toBool()) {
                int secId = fmt.property(DetailsSectionIdProp).toInt();
                toggleSection(secId);
                ev->accept();
                return;
            }
        }
    }
    QTextBrowser::mousePressEvent(ev);
}

void MarkdownRenderer::mouseReleaseEvent(QMouseEvent *ev)
{
    QTextBrowser::mouseReleaseEvent(ev);
    // A drag-copy is written to the clipboard by QTextEdit itself, bypassing
    // copySelection(); redo it with the chip padding stripped. The threshold
    // is Qt's internal (unexposed) drag distance, 8 px.
    if (textCursor().hasSelection()
        && (ev->position().toPoint() - m_pressPos).manhattanLength() > 8)
        copySelection();
}

void MarkdownRenderer::resizeEvent(QResizeEvent *event)
{
    QTextBrowser::resizeEvent(event);
    setupDocumentSettings();
    updateAllOverlaysGeometry();
}

// ---------------------------------------------------------------------------
// Code copy/save
// ---------------------------------------------------------------------------

static QString escapeHtml(QString text)
{
    return text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace("\"", "&quot;")
        .replace("'", "&#39;");
}

QPair<QString, QString> MarkdownRenderer::collectCodeById(int id) const
{
    QString plain;
    QString html;
    QTextDocument *doc = document();
    if (!doc)
        return {plain, html};

    QTextBlock firstBlock;
    QTextBlock lastBlock;
    bool found = false;

    // Find the range of blocks belonging to this code ID
    for (QTextBlock blk = doc->begin(); blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(BlockCodeIdProp).toInt() == id) {
            if (!found) {
                firstBlock = blk;
                found = true;
            }
            lastBlock = blk;
        }
    }

    if (found) {
        // Iterate through blocks and their fragments
        for (QTextBlock blk = firstBlock; blk.isValid() && (blk < lastBlock || blk == lastBlock);
             blk = blk.next()) {
            // Handle Plain Text
            // We append a newline because blk.text() doesn't include the separator
            plain += blk.text() + "\n";

            // Handle HTML
            html += "<div>";
            for (auto it = blk.begin(); it != blk.end(); ++it) {
                QString fragText = it.fragment().text();
                fragText = escapeHtml(fragText);

                QTextCharFormat fmt = it.fragment().charFormat();
                QColor color = fmt.foreground().color();

                // Only wrap in span if the color is different from the default text color
                if (color.isValid() && color != m_colorMap.value(TextForeground)) {
                    html += QString("<span style=\"color: %1;\">%2</span>")
                                .arg(color.name(), fragText);
                } else {
                    html += fragText;
                }
            }
            html += "\n</div>";
        }
    }

    return {plain, html};
}

QMap<int, QRectF> MarkdownRenderer::collectBlockRects(int prop, int skipProp /*= -1*/) const
{
    QTextDocument *doc = document();
    QMap<int, QRectF> rects;

    for (QTextBlock blk = doc->begin(); blk.isValid(); blk = blk.next()) {
        QVariant idVar = blk.blockFormat().property(prop);
        if (!idVar.isValid() || !blk.isVisible())
            continue;
        if (skipProp > 0 && blk.blockFormat().hasProperty(skipProp))
            continue;
        int id = idVar.toInt();
        QRectF blkRect = blockBoundingRect(blk);
        if (rects.contains(id))
            rects[id] = rects[id].united(blkRect);
        else
            rects[id] = blkRect;
    }
    return rects;
}

// ---------------------------------------------------------------------------
// Styling API
// ---------------------------------------------------------------------------

QByteArray MarkdownRenderer::buffer() const
{
    return m_buffer;
}

void MarkdownRenderer::setBuffer(const QByteArray &newBuffer)
{
    m_buffer = newBuffer;
}

bool MarkdownRenderer::expandDetailsByDefault() const
{
    return m_expandDetailsByDefault;
}

void MarkdownRenderer::setExpandDetailsByDefault(bool newExpandDetailsByDefault)
{
    m_expandDetailsByDefault = newExpandDetailsByDefault;
}

bool MarkdownRenderer::collapseToolCallsByDefault() const
{
    return m_collapseToolCallsByDefault;
}

void MarkdownRenderer::setCollapseToolCallsByDefault(bool newCollapseToolCallsByDefault)
{
    m_collapseToolCallsByDefault = newCollapseToolCallsByDefault;
}

void MarkdownRenderer::setColor(ColorRole role, const QColor &color)
{
    m_colorMap[role] = color;
}
QColor MarkdownRenderer::color(ColorRole role) const
{
    return m_colorMap.value(role);
}
void MarkdownRenderer::setColorPalette(const QHash<ColorRole, QColor> &p)
{
    m_colorMap = p;
}
void MarkdownRenderer::setBaseFont(const QFont &f)
{
    m_baseFont = f;
    setFont(f);
    m_baseFontSize = f.pointSizeF();
}
void MarkdownRenderer::setMonoFont(const QFont &f)
{
    m_monoFont = f;
}
void MarkdownRenderer::setBaseFontFamily(const QString &f)
{
    m_baseFont.setFamily(f);
    setFont(m_baseFont);
    m_baseFontSize = m_baseFont.pointSizeF();
}
void MarkdownRenderer::setBaseFontSize(int s)
{
    m_baseFont.setPointSize(s);
    m_baseFontSize = s;
    setFont(m_baseFont);
}
