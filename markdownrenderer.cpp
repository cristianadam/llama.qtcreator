#include "markdownrenderer.h"

#include <QAbstractTextDocumentLayout>
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
    m_colorMap[OverlayBackground] = QColor(255, 255, 255, 0);
    m_colorMap[OverlayButtonBackground] = QColor(240, 240, 240, 200);
    m_colorMap[OverlayButtonBackgroundHover] = QColor(220, 220, 220, 200);
    m_colorMap[OverlayButtonBorder] = m_colorMap[TableBorder];

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
    m_toggleDetails.clear();
    m_listStack.clear();
    m_tableStack.clear();
    m_textCharFormatStack.clear();
    m_blockQuoteDepth = 0;
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

void MarkdownRenderer::renderInlines(const markus::Document &doc,
                                     const std::pmr::vector<markus::InlineNodeId> &ids)
{
    for (markus::InlineNodeId id : ids)
        renderInline(doc, id);
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
                m_cursor.insertText(QString::fromUtf8(n.content.data(), n.content.size()));
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
    for (const markus::BlockNode &block : tailDoc.children)
        renderBlock(tailDoc, block);
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

    // Invisible 1pt zero-width spacer lines at the top and bottom of the
    // block keep the code away from the edges of the rounded background.
    // They are part of the code block (same block id) and are stripped from
    // copies by collectCodeById().
    QTextCharFormat spacerFmt = baseCharFmt;
    spacerFmt.setFontPointSize(1);
    m_cursor.insertText(ZeroWidthSpace + QLatin1String("\n"), spacerFmt);

    QVector<HighlightFragment> fragments;
    SyntaxHighlighter highlighter;
    highlighter.setDefinition(syntaxDefinitionForName(m_codeBlockLanguage));
    highlighter.highlight(content, baseCharFmt, fragments);
    if (fragments.isEmpty()) {
        m_cursor.insertText(content);
    } else {
        for (const HighlightFragment &fragment : fragments)
            m_cursor.insertText(fragment.text, fragment.format);
    }

    // Same zero-width space for the bottom.
    m_cursor.insertText(ZeroWidthSpace + QLatin1String("\n"), spacerFmt);

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

void MarkdownRenderer::renderDetails(const markus::Document &doc,
                                     const markus::DetailsBlock &details)
{
    int secId = ++m_nextDetailsId;
    if (!m_toggleDetails.contains(secId))
        m_toggleDetails.insert(secId, m_expandDetailsByDefault);
    const bool visible = m_toggleDetails.value(secId);
    const QString summary = fromStdString(details.summary);

    // Summary (toggle) block.
    // Note: insertHtml() on an empty block clobbers the block format, so the
    // format with the details properties must be applied *after* the insert.
    beginBlock();
    m_cursor.insertHtml(detailsHtmlLabel(summary, secId, visible));
    QTextBlockFormat sumFmt = m_cursor.blockFormat();
    sumFmt.setProperty(DetailsSectionIdProp, secId);
    sumFmt.setProperty(DetailsToggleBlockProp, true);
    sumFmt.setProperty(DetailsSummaryTextProp, summary);
    m_cursor.setBlockFormat(sumFmt);
    QTextBlock toggleBlock = m_cursor.block();

    // Content: markdown blocks that markus parsed from the section body.
    // While rendering them, m_detailsSecId makes beginBlock() tag every inner
    // block with the section id and the extra quote level indents the content
    // (and makes paintEvent() draw the quote line).
    const int prevSecId = m_detailsSecId;
    m_detailsSecId = secId;
    const int prevQuoteDepth = m_blockQuoteDepth;
    m_blockQuoteDepth += 1;

    QTextCharFormat quoteCharFmt;
    quoteCharFmt.setForeground(color(BlockquoteText));
    m_textCharFormatStack.push(quoteCharFmt);

    renderBlockIds(doc, details.children);

    m_textCharFormatStack.pop();
    m_blockQuoteDepth = prevQuoteDepth;
    m_detailsSecId = prevSecId;

    for (QTextBlock blk = toggleBlock.next(); blk.isValid(); blk = blk.next()) {
        // Blocks of a nested section carry their own id and were already
        // shown/hidden by that section's renderDetails().
        if (blk.blockFormat().property(DetailsSectionIdProp).toInt() != secId)
            continue;
        if (!blk.blockFormat().property(DetailsToggleBlockProp).toBool())
            blk.setVisible(visible);
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

// ---------------------------------------------------------------------------
// Details sections
// ---------------------------------------------------------------------------

void MarkdownRenderer::toggleSection(int secId)
{
    bool makeVisible = !m_toggleDetails.value(secId);
    for (QTextBlock blk = document()->firstBlock(); blk.isValid(); blk = blk.next()) {
        if (blk.blockFormat().property(DetailsSectionIdProp).toInt() == secId) {
            if (blk.blockFormat().property(DetailsToggleBlockProp).toBool())
                continue;
            blk.setVisible(makeVisible);
        }
    }

    for (QTextBlock blk = document()->firstBlock(); blk.isValid(); blk = blk.next()) {
        // Find the block that acts as the clickable header for this ID
        if (blk.blockFormat().property(DetailsSectionIdProp).toInt() == secId
            && blk.blockFormat().property(DetailsToggleBlockProp).toBool()) {
            // Retrieve the original summary text we stored earlier
            QString summary = blk.blockFormat().property(DetailsSummaryTextProp).toString();
            if (summary.isEmpty())
                summary = Tr::tr("Details");

            // insertHtml() clobbers the block format, so re-apply it (it
            // carries the details section id / toggle properties).
            const QTextBlockFormat blkFmt = blk.blockFormat();
            QTextCursor cursor(blk);
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            cursor.insertHtml(detailsHtmlLabel(summary, secId, makeVisible));
            cursor.setBlockFormat(blkFmt);

            break;
        }
    }

    m_toggleDetails[secId] = makeVisible;

    setupDocumentSettings();
    updateAllOverlaysGeometry();
    viewport()->update();

    document()->setTextWidth(viewport()->width());
    updateGeometry();
}

QString MarkdownRenderer::detailsHtmlLabel(const QString &summary, int secId, bool isVisible) const
{
    QString icon = isVisible ? "M" : "N";
    QString label = QString(
                        "<a href=\"details-toggle:%1\" style=\"text-decoration:none; color: %2\">"
                        "%3&nbsp;<span style=\"font-family: heroicons_outline\">%4</span></a>")
                        .arg(secId)
                        .arg(colorToRgba(color(TextForeground)))
                        .arg(summary)
                        .arg(icon);
    return label;
}

// ---------------------------------------------------------------------------
// Code block overlays
// ---------------------------------------------------------------------------

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
    updateGeometry();
}

void MarkdownRenderer::updateAllOverlaysGeometry()
{
    if (m_codeOverlays.isEmpty())
        return;
    const int margin = m_paragraphMargin / 2;
    const int topAdjust = 0;
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
        while (true) {
            QTextBlock nxt = last.next();
            if (!nxt.isValid())
                break;
            if (nxt.blockFormat().property(BlockCodeIdProp).toInt() != blockId)
                break;
            last = nxt;
        }
        if (last != blk) {
            QRectF lastRect = blockBoundingRect(last);
            rect.setBottom(lastRect.bottom());
        }
        QRectF viewRect = rect.translated(offset);
        int x = static_cast<int>(viewRect.right() - overlay->width() - margin);
        int y = static_cast<int>(viewRect.top() + margin + topAdjust);
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

    overlay->setStyleSheet(QString("QWidget { background: %1; }"
                                    "QToolButton { "
                                    "  background: %2; "
                                    "  border: 1px solid %3; "
                                    "  border-radius: 6px; "
                                    "  padding: 4px -2px; "
                                    "  font-family: heroicons_outline; "
                                    "  font-size: 14px; "
                                    "  color: %4; "
                                    "} "
                                    "QToolButton:hover { "
                                    "  background-color: %5; "
                                    "}")
                                .arg(colorToRgba(color(OverlayBackground)))
                                .arg(colorToRgba(color(OverlayButtonBackground)))
                                .arg(color(OverlayButtonBorder).name())
                                .arg(colorToRgba(color(TextForeground)))
                                .arg(color(OverlayButtonBackgroundHover).name()));

    QHBoxLayout *hl = new QHBoxLayout(overlay);
    hl->setContentsMargins(0, 0, 5, 0);
    hl->setSpacing(10);

    QToolButton *copyBtn = new QToolButton(overlay);
    copyBtn->setText("E"); // Heroicon character for copy
    copyBtn->setToolTip(Tr::tr("Copy the code below to Clipboard"));
    hl->addWidget(copyBtn);

    QToolButton *saveBtn = new QToolButton(overlay);
    saveBtn->setText("F"); // Heroicon character for save
    saveBtn->setToolTip(Tr::tr("Save the code below into a file on disk"));
    hl->addWidget(saveBtn);

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

    connect(saveBtn, &QToolButton::clicked, this, [this, blockId] {
        auto [code, formattedCode] = collectCodeById(blockId);
        if (code.isEmpty())
            return;

        emit saveClicked(code);
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
        QRectF viewRect = blkRect.translated(contentOffset());
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

    // Remove the invisible spacer lines used for the rounded background padding
    if (firstBlock.isValid() && firstBlock.text() == ZeroWidthSpace)
        firstBlock = firstBlock.next();
    if (lastBlock.isValid() && lastBlock.text() == ZeroWidthSpace)
        lastBlock = lastBlock.previous();

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
