#include "markdownrenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QLayout>
#include <QClipboard>
#include <QColor>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QResizeEvent>
#include <QScrollBar>
#include <QToolButton>
#include <QToolTip>

#include "llamasyntaxhighlighter.h"
#include "llamatr.h"

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
                handleInlineCode();
                m_cursor.insertText(fromStdString(n.content));
                popCharFormat();
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
    for (const markus::BlockNode &block : tailDoc.children)
        renderBlock(tailDoc, block);
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

    m_codeBlock = false;
    m_codeBlockLanguage.clear();
    m_codeFenceChar = QChar::Null;

    createOverlayForCodeBlock(blockId);
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
    int secId;
    if (m_tailDetailsIndex >= 0) {
        secId = m_nextDetailsId + m_tailDetailsIndex + 1;
        ++m_tailDetailsIndex;
    } else {
        secId = ++m_nextDetailsId;
    }

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
    // Inside a heading, keep the heading's font size and skip the inline-code
    // background so the code stays bold and scales with the heading.
    if (m_headingLevel == 0) {
        fmt.setBackground(color(InlineCodeBackground));
        fmt.setFontPointSize(m_baseFontSize * 0.90);
    }
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
    if (m_codeOverlays.isEmpty())
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
    m_baseFontSize = s;
    setFont(m_baseFont);
}
