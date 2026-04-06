#include "markdownrenderer.h"

#include <QAbstractTextDocumentLayout>
#include <QClipboard>
#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QRegularExpression>
#include <QResizeEvent>
#include <QScrollBar>
#include <QStyle>
#include <QToolButton>
#include <QToolTip>

#include <coreplugin/icore.h>
#include <utils/filepath.h>

#include "llamatr.h"

using namespace LlamaCpp;
using namespace Utils;

static QString colorToRgba(const QColor &c)
{
    return QString("rgba(%1,%2,%3,%4)").arg(c.red()).arg(c.green()).arg(c.blue()).arg(c.alpha());
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

    m_parser.abi_version = 0;
    m_parser.flags = MD_DIALECT_GITHUB | MD_FLAG_NOHTMLSPANS;
    m_parser.enter_block = &MarkdownRenderer::md_enter_block;
    m_parser.leave_block = &MarkdownRenderer::md_leave_block;
    m_parser.enter_span = &MarkdownRenderer::md_enter_span;
    m_parser.leave_span = &MarkdownRenderer::md_leave_span;
    m_parser.text = &MarkdownRenderer::md_text;

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

void MarkdownRenderer::feed(const QByteArray &chunk)
{
    m_buffer = chunk;

    if (!m_doc) {
        m_doc = new QTextDocument(this);
        m_doc->setIndentWidth(MarkdownParserContext::indentWidth);
        setDocument(m_doc);
        setupDocumentSettings();
        m_cursor = QTextCursor(m_doc);
    }

    MarkdownParserContext context;
    context.baseFont = m_baseFont;
    context.monoFont = m_monoFont;
    context.baseFontSize = m_baseFontSize;
    context.paragraphMargin = m_paragraphMargin;
    context.colorMap = m_colorMap;

    md_parse(m_buffer.constData(), m_buffer.size(), &m_parser, &context);

    auto validIndex = [](const MarkdownOp &op) {
        return op.type == MarkdownOp::InsertHeading && op.docPosition != 0;
    };

    // Find divergence point
    int diffIndex = m_lastOps.size() ? m_lastOps.size() - 1 : 0;
    while (diffIndex > 0 && diffIndex < context.ops.size()) {
        if (m_lastOps[diffIndex] == context.ops[diffIndex] && validIndex(m_lastOps[diffIndex]))
            break;
        --diffIndex;
    }

    m_cursor = QTextCursor(m_doc);
    m_cursor.beginEditBlock();

    if (diffIndex == 0) {
        // This can happen with [url] like this
        // [url]: https://url
        m_doc->clear();
        m_cursor.movePosition(QTextCursor::Start);

        m_listStack.clear();
        m_tableStack.clear();
        m_detailsStartStack.clear();
    } else {
        // Revert to the last known good position
        int lastGoodPos = m_lastOps[diffIndex].docPosition;
        m_cursor.setPosition(lastGoodPos);
        m_cursor.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
        m_cursor.removeSelectedText();
    }

    // Replay all operations from the divergence point onwards
    for (int i = diffIndex; i < context.ops.size(); ++i) {
        context.ops[i].docPosition = m_cursor.position();
        applyOpToCursor(context.ops[i]);
    }

    m_lastOps = context.ops;
    m_cursor.endEditBlock();

    updateAllOverlaysGeometry();
}

void MarkdownRenderer::finish()
{
    m_lastOps.clear();
}

void MarkdownRenderer::applyOpToCursor(const MarkdownOp &op)
{
    switch (op.type) {
    case MarkdownOp::InsertText:
        m_cursor.setCharFormat(op.charFmt.value_or(m_cursor.charFormat()));
        m_cursor.insertText(op.content);
        break;
    case MarkdownOp::InsertCode:
        m_cursor.setCharFormat(op.charFmt.value_or(m_cursor.charFormat()));
        m_cursor.setBlockFormat(op.blockFmt.value_or(m_cursor.blockFormat()));
        m_cursor.insertText(op.content);
        break;
    case MarkdownOp::InsertHeading: {
        // Removal of old text up to a heading will result in empty paragraphs
        bool reuseExistingBlock = m_cursor.atBlockStart() && m_cursor.block().text().isEmpty()
                                  && m_cursor.block().isVisible();
        if (m_cursor.document()->isEmpty() || reuseExistingBlock) {
            m_cursor.setBlockFormat(*op.blockFmt);
            m_cursor.setCharFormat(*op.charFmt);
        } else {
            m_cursor.insertBlock(*op.blockFmt, *op.charFmt);
        }
        break;
    }
    case MarkdownOp::InsertBlock: {
        QTextBlockFormat blkFmt = op.blockFmt.value_or(m_cursor.blockFormat());
        QTextCharFormat chFmt = op.charFmt.value_or(m_cursor.charFormat());

        if (!m_listStack.isEmpty() && !op.listFmt)
            blkFmt.setIndent(m_listStack.size());

        if (m_cursor.document()->isEmpty()) {
            m_cursor.setBlockFormat(blkFmt);
            m_cursor.setCharFormat(chFmt);
        } else {
            m_cursor.insertBlock(blkFmt, chFmt);
        }
        if (op.listFmt) {
            if (!m_listStack.isEmpty()) {
                QTextList *&list = m_listStack.last();
                if (!list) {
                    list = m_cursor.createList(*op.listFmt);
                } else {
                    if (!m_cursor.document()->isEmpty())
                        list->add(m_cursor.block());
                }
            }
        }

        break;
    }
    case MarkdownOp::InsertCodeBlock: {
        QTextBlockFormat blkFmt = op.blockFmt.value_or(m_cursor.blockFormat());
        QTextCharFormat chFmt = op.charFmt.value_or(m_cursor.charFormat());
        if (m_cursor.document()->isEmpty()) {
            m_cursor.setBlockFormat(blkFmt);
            m_cursor.setCharFormat(chFmt);
        } else {
            m_cursor.insertBlock(blkFmt, chFmt);
        }
        if (blkFmt.hasProperty(MarkdownRenderer::BlockCodeIdProp)) {
            createOverlayForCodeBlock(blkFmt.property(MarkdownRenderer::BlockCodeIdProp).toInt());
        }
        break;
    }
    case MarkdownOp::InsertHtml:
        m_cursor.insertHtml(op.content);
        break;
    case MarkdownOp::InsertImage: {
        QTextImageFormat imgFmt;
        imgFmt.setName(op.content);
        m_cursor.insertImage(imgFmt);
        break;
    }
    case MarkdownOp::InsertTable:
        m_tableStack.push(m_cursor.insertTable(op.tableRows, op.tableCols, op.tableFmt));
        break;
    case MarkdownOp::InsertTableCell: {
        if (!m_tableStack.isEmpty()) {
            QTextTable *table = m_tableStack.last();
            QTextTableCell cell = table->cellAt(op.cellRow, op.cellCol);
            if (cell.isValid()) {
                QTextCursor cellCursor = cell.firstCursorPosition();

                QTextBlockFormat blockFmt = op.blockFmt.value_or(cellCursor.blockFormat());
                blockFmt.setAlignment(op.cellAlign);
                cellCursor.setBlockFormat(blockFmt);
                if (op.charFmt)
                    cellCursor.setCharFormat(*op.charFmt);
                cell.setFormat(op.cellBgFmt);
                m_cursor = cellCursor;
                if (op.charFmt)
                    m_cursor.insertText(op.content, *op.charFmt);
                else
                    m_cursor.insertText(op.content);
            }
        }
        break;
    }
    case MarkdownOp::InsertThematicBreak:
        m_cursor.insertBlock(*op.blockFmt);
        // Insert a zero space character. Not to get removed by a heading cleanup.
        m_cursor.insertText(ZeroWidthSpace);
        break;
    case MarkdownOp::InsertList:
        m_listStack.push_back(nullptr);
        break;
    case MarkdownOp::SetCharFormat:
        m_cursor.setCharFormat(*op.charFmt);
        break;
    case MarkdownOp::SetBlockFormat:
        m_cursor.setBlockFormat(*op.blockFmt);
        break;
    case MarkdownOp::CloseTable:
        if (!m_tableStack.isEmpty()) {
            QTextTable *table = m_tableStack.takeLast();
            m_cursor = table->lastCursorPosition();
            m_cursor.movePosition(QTextCursor::End);
        }
        break;
    case MarkdownOp::CloseList:
        if (!m_listStack.isEmpty())
            m_listStack.removeLast();
        break;
    case MarkdownOp::InsertDetails: {
        QTextBlockFormat blkFmt = op.blockFmt.value_or(m_cursor.blockFormat());
        m_cursor.insertBlock(blkFmt);
        m_detailsStartStack.push(m_cursor.block());
        int secId = blkFmt.property(MarkdownRenderer::DetailsSectionIdProp).toInt();
        if (!m_toggleDetails.contains(secId))
            m_toggleDetails.insert(secId, m_expandDetailsByDefault);

        m_cursor.insertHtml(detailsHtmlLabel(op.content, secId, m_expandDetailsByDefault));

        break;
    }
    case MarkdownOp::CloseDetails:
        if (!m_detailsStartStack.isEmpty()) {
            QTextBlock hiddenStart = m_detailsStartStack.pop();
            int secId
                = hiddenStart.blockFormat().property(MarkdownRenderer::DetailsSectionIdProp).toInt();
            hiddenStart = hiddenStart.next();
            for (QTextBlock blk = hiddenStart; blk.isValid(); blk = blk.next()) {
                if (blk.blockFormat().property(MarkdownRenderer::DetailsSectionIdProp).toInt()
                        == secId
                    && blk.blockFormat().property(MarkdownRenderer::DetailsToggleBlockProp).isNull()) {
                    blk.setVisible(m_toggleDetails.value(secId));
                }
            }
        }
        break;
    }
}

int MarkdownRenderer::md_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    auto *ctx = static_cast<MarkdownParserContext *>(userdata);
    switch (type) {
    case MD_BLOCK_H:
        ctx->handleHeading(static_cast<MD_BLOCK_H_DETAIL *>(detail));
        break;
    case MD_BLOCK_P:
        ctx->handleParagraph();
        break;
    case MD_BLOCK_QUOTE:
        ctx->handleBlockQuote();
        break;
    case MD_BLOCK_CODE:
        ctx->handleCodeBlock(static_cast<MD_BLOCK_CODE_DETAIL *>(detail));
        break;
    case MD_BLOCK_HR:
        ctx->handleThematicBreak();
        break;
    case MD_BLOCK_UL:
    case MD_BLOCK_OL:
        ctx->handleList(type, detail);
        break;
    case MD_BLOCK_LI:
        ctx->handleItem(static_cast<MD_BLOCK_LI_DETAIL *>(detail));
        break;
    case MD_BLOCK_TABLE:
        ctx->handleTable(static_cast<MD_BLOCK_TABLE_DETAIL *>(detail));
        break;
    case MD_BLOCK_TR:
        ctx->handleTableRow();
        break;
    case MD_BLOCK_TD:
    case MD_BLOCK_TH:
        ctx->handleTableCell(static_cast<MD_BLOCK_TD_DETAIL *>(detail));
        break;
    default:
        break;
    }
    return 0;
}

int MarkdownRenderer::md_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata)
{
    auto *ctx = static_cast<MarkdownParserContext *>(userdata);

    if (type == MD_BLOCK_QUOTE) {
        --ctx->blockQuoteDepth;
        if (!ctx->textCharFormatStack.isEmpty())
            ctx->textCharFormatStack.pop();
    } else if (type == MD_BLOCK_UL || type == MD_BLOCK_OL) {
        ctx->skipNextParagraph = false;
        if (!ctx->listStack.isEmpty()) {
            ctx->listStack.removeLast();
            ctx->ops.push_back({MarkdownOp::CloseList});
        }
    } else if (type == MD_BLOCK_H) {
        ctx->currentHeadingLevel = 0;
        if (!ctx->textCharFormatStack.isEmpty())
            ctx->textCharFormatStack.pop();
    } else if (type == MD_BLOCK_TABLE) {
        if (!ctx->tableStack.isEmpty()) {
            ctx->tableStack.pop();
            ctx->ops.push_back({MarkdownOp::CloseTable});
        }
    } else if (type == MD_BLOCK_CODE) {
        if (ctx->highlighter) {
            ctx->highlighter->finish(ctx);
            ctx->highlighter.reset();
        }
        if (!ctx->codeBlockFormatStack.isEmpty())
            ctx->codeBlockFormatStack.pop();
        if (!ctx->textCharFormatStack.isEmpty())
            ctx->textCharFormatStack.pop();

        if (!ctx->ops.isEmpty()) {
            MarkdownOp spacer = ctx->ops.last();
            // A newline with a zero space character that is small
            spacer.content = spacer.content == "\n" ? QString(ZeroWidthSpace)
                                                    : QString("\n" + ZeroWidthSpace);
            spacer.charFmt->setFontPointSize(1);
            ctx->ops.push_back(spacer);
        }
    }
    return 0;
}

int MarkdownRenderer::md_enter_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    auto *ctx = static_cast<MarkdownParserContext *>(userdata);
    switch (type) {
    case MD_SPAN_EM:
        ctx->handleEmph();
        break;
    case MD_SPAN_STRONG:
        ctx->handleStrong();
        break;
    case MD_SPAN_DEL:
        ctx->handleStrikethrough();
        break;
    case MD_SPAN_CODE:
        ctx->handleInlineCode();
        break;
    case MD_SPAN_A:
        ctx->handleLink(static_cast<MD_SPAN_A_DETAIL *>(detail));
        break;
    case MD_SPAN_IMG:
        ctx->handleImage(static_cast<MD_SPAN_IMG_DETAIL *>(detail));
        break;
    default:
        break;
    }
    return 0;
}

int MarkdownRenderer::md_leave_span(MD_SPANTYPE type, void *detail, void *userdata)
{
    auto *ctx = static_cast<MarkdownParserContext *>(userdata);
    if (!ctx->textCharFormatStack.isEmpty())
        ctx->textCharFormatStack.pop();

    MarkdownOp op{MarkdownOp::SetCharFormat};
    op.charFmt = QTextCharFormat();
    if (!ctx->textCharFormatStack.isEmpty())
        op.charFmt = ctx->textCharFormatStack.top();

    ctx->ops.push_back(op);

    return 0;
}

int MarkdownRenderer::md_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata)
{
    auto *ctx = static_cast<MarkdownParserContext *>(userdata);
    if (type == MD_TEXT_HTML)
        ctx->handleHtml(QString::fromUtf8(reinterpret_cast<const char *>(text), size));
    else
        ctx->handleText(type, text, size);

    return 0;
}

KSyntaxHighlighting::Repository *MarkdownParserContext::highlightRepository()
{
    static KSyntaxHighlighting::Repository *repository = nullptr;
    if (!repository) {
        repository = new KSyntaxHighlighting::Repository();
        const FilePath dir = Core::ICore::resourcePath("generic-highlighter/syntax");
        if (dir.exists())
            repository->addCustomSearchPath(dir.parentDir().path());
        const FilePath userDir = Core::ICore::userResourcePath("generic-highlighter");
        if (userDir.exists())
            repository->addCustomSearchPath(userDir.path());
    }
    return repository;
}

KSyntaxHighlighting::Definition MarkdownParserContext::definitionForName(const QString &name)
{
    return highlightRepository()->definitionForName(name);
}

int MarkdownParserContext::getBlockQuoteMargin(int depth) const
{
    if (depth <= 0)
        return 0;
    int baseIndent = paragraphMargin;
    int extraPerLevel = 8;
    return baseIndent + extraPerLevel * (depth - 1);
}

void MarkdownParserContext::handleHeading(MD_BLOCK_H_DETAIL *detail)
{
    MarkdownOp op;
    op.type = MarkdownOp::InsertHeading;
    currentHeadingLevel = detail->level;
    QTextBlockFormat blkFmt;
    blkFmt.setHeadingLevel(currentHeadingLevel);
    blkFmt.setTopMargin(24);
    blkFmt.setBottomMargin(16);
    op.blockFmt = blkFmt;

    QTextCharFormat chFmt;
    static const double mult[6] = {2.0, 1.5, 1.25, 1.0, 0.875, 0.85};
    qreal fontSize = baseFontSize * mult[currentHeadingLevel - 1];
    chFmt.setFontPointSize(fontSize);
    chFmt.setFontWeight(QFont::Bold);
    op.charFmt = chFmt;

    ops.push_back(op);

    textCharFormatStack.push(chFmt);
}

void MarkdownParserContext::handleParagraph()
{
    if (skipNextParagraph) {
        skipNextParagraph = false;
        return;
    }
    MarkdownOp op;
    op.type = MarkdownOp::InsertBlock;
    QTextBlockFormat blkFmt;
    blkFmt.setTopMargin(0);
    blkFmt.setBottomMargin(10);
    if (blockQuoteDepth > 0) {
        blkFmt.setProperty(QTextFormat::BlockQuoteLevel, blockQuoteDepth);
        blkFmt.setLeftMargin(getBlockQuoteMargin(blockQuoteDepth));
    }
    op.blockFmt = blkFmt;
    op.charFmt = QTextCharFormat();
    ops.push_back(op);
}

void MarkdownParserContext::handleBlockQuote()
{
    ++blockQuoteDepth;
    QTextCharFormat fmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                        : textCharFormatStack.top();
    fmt.setForeground(colorMap[MarkdownRenderer::BlockquoteText]);
    textCharFormatStack.push(fmt);

    MarkdownOp op;
    op.type = MarkdownOp::InsertBlock;

    QTextBlockFormat blkFmt;
    blkFmt.setProperty(QTextFormat::BlockQuoteLevel, blockQuoteDepth);
    blkFmt.setTopMargin(paragraphMargin);
    blkFmt.setBottomMargin(paragraphMargin);
    blkFmt.setLeftMargin(getBlockQuoteMargin(blockQuoteDepth));

    op.blockFmt = blkFmt;
    op.charFmt = fmt;
    ops.push_back(op);
    skipNextParagraph = true;
}

void MarkdownParserContext::handleCodeBlock(MD_BLOCK_CODE_DETAIL *detail)
{
    QString codeBlockLanguage = mdAttrToString(detail->lang);
    QChar codeFenceChar = detail->fence_char ? QChar::fromLatin1(detail->fence_char) : QChar::Null;

    highlighter = std::make_unique<SyntaxHighlighter>();
    highlighter->setDefinition(definitionForName(codeBlockLanguage));

    MarkdownOp op;
    op.type = MarkdownOp::InsertCodeBlock;
    int blockId = ++nextCodeBlockId;
    QTextBlockFormat blkFmt;
    blkFmt.setProperty(MarkdownRenderer::BlockCodeIdProp, blockId);
    blkFmt.setLineHeight(70, QTextBlockFormat::ProportionalHeight);
    blkFmt.setAlignment(Qt::AlignVCenter);
    blkFmt.setTopMargin(paragraphMargin);
    blkFmt.setBottomMargin(paragraphMargin);

    blkFmt.setProperty(QTextFormat::BlockCodeLanguage, codeBlockLanguage);
    if (blockQuoteDepth > 0) {
        blkFmt.setProperty(QTextFormat::BlockQuoteLevel, blockQuoteDepth);
        blkFmt.setLeftMargin(getBlockQuoteMargin(blockQuoteDepth) + paragraphMargin);
    } else {
        blkFmt.setLeftMargin(paragraphMargin);
    }

    if (!listStack.isEmpty())
        blkFmt.setIndent(listStack.size());

    if (!codeFenceChar.isNull()) {
        blkFmt.setNonBreakableLines(true);
        blkFmt.setProperty(QTextFormat::BlockCodeFence, QString(codeFenceChar));
    }

    codeBlockFormatStack.push(blkFmt);

    QTextCharFormat charFmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                            : textCharFormatStack.top();
    charFmt.setFont(monoFont, QTextCharFormat::FontPropertiesSpecifiedOnly);
    charFmt.setFontFixedPitch(true);
    charFmt.setFontPointSize(baseFontSize * 0.90);
    textCharFormatStack.push(charFmt);

    op.blockFmt = blkFmt;
    op.charFmt = charFmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleThematicBreak()
{
    MarkdownOp op;
    op.type = MarkdownOp::InsertThematicBreak;
    QTextBlockFormat blkFmt;
    blkFmt.setProperty(MarkdownRenderer::HorizontalRulerIdProp, 1);
    blkFmt.setTopMargin(paragraphMargin);
    blkFmt.setBottomMargin(paragraphMargin);
    if (blockQuoteDepth > 0) {
        blkFmt.setProperty(QTextFormat::BlockQuoteLevel, blockQuoteDepth);
        blkFmt.setLeftMargin(getBlockQuoteMargin(blockQuoteDepth));
    }
    op.blockFmt = blkFmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleList(MD_BLOCKTYPE type, void *detail)
{
    ListState ls;
    ls.fmt.setIndent(listStack.size());
    if (type == MD_BLOCK_UL) {
        auto *d = static_cast<MD_BLOCK_UL_DETAIL *>(detail);
        ls.fmt.setStyle(d->mark == '*' ? QTextListFormat::ListCircle
                                       : (d->mark == '+' ? QTextListFormat::ListSquare
                                                         : QTextListFormat::ListDisc));
    } else {
        auto *d = static_cast<MD_BLOCK_OL_DETAIL *>(detail);
        ls.fmt.setStyle(listStack.isEmpty() ? QTextListFormat::ListDecimal
                                            : QTextListFormat::ListLowerRoman);
        ls.fmt.setStart(d->start);
    }
    listStack.append(ls);

    MarkdownOp op;
    op.type = MarkdownOp::InsertList;
    op.listFmt = ls.fmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleItem(MD_BLOCK_LI_DETAIL *detail)
{
    MarkdownOp op;
    op.type = MarkdownOp::InsertBlock;
    skipNextParagraph = true;
    QTextBlockFormat blkFmt;
    blkFmt.setTopMargin(static_cast<int>(baseFontSize * 0.25));
    blkFmt.setBottomMargin(paragraphMargin);
    if (blockQuoteDepth > 0) {
        blkFmt.setProperty(QTextFormat::BlockQuoteLevel, blockQuoteDepth);
        blkFmt.setLeftMargin(getBlockQuoteMargin(blockQuoteDepth) + indentWidth);
    }
    if (detail->is_task) {
        blkFmt.setMarker(detail->task_mark == 'x' || detail->task_mark == 'X'
                             ? QTextBlockFormat::MarkerType::Checked
                             : QTextBlockFormat::MarkerType::Unchecked);
    }
    op.charFmt = textCharFormatStack.isEmpty() ? QTextCharFormat() : textCharFormatStack.top();

    if (!listStack.isEmpty()) {
        op.listFmt = listStack.last().fmt;
        blkFmt.setLeftMargin(paragraphMargin + indentWidth);
    }
    op.blockFmt = blkFmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleText(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size)
{
    QString str = QString::fromUtf8(reinterpret_cast<const char *>(text), size);
    if (type == MD_TEXT_BR) {
        ops.push_back({MarkdownOp::InsertText, "\n"});
    } else if (type == MD_TEXT_SOFTBR) {
        ops.push_back({MarkdownOp::InsertText, " "});
    } else if (type == MD_TEXT_NORMAL) {
        MarkdownOp op{MarkdownOp::InsertText, str};
        if (!textCharFormatStack.isEmpty())
            op.charFmt = textCharFormatStack.top();
        ops.push_back(op);
    } else if (type == MD_TEXT_CODE) {
        if (!ops.isEmpty() && ops.last().type == MarkdownOp::InsertCodeBlock) {
            MarkdownOp spacer{MarkdownOp::InsertCode};
            // A newline with a zero space character that is small
            spacer.content = MarkdownRenderer::ZeroWidthSpace + "\n";
            spacer.charFmt = !textCharFormatStack.isEmpty() ? textCharFormatStack.top()
                                                            : QTextCharFormat();
            if (!codeBlockFormatStack.isEmpty())
                spacer.blockFmt = codeBlockFormatStack.top();
            spacer.charFmt->setFontPointSize(1);
            ops.push_back(spacer);
        }

        if (highlighter) {
            // Use the syntax highlighter to generate ops instead of a single InsertCode
            highlighter->processChunk(str, this);
        } else {
            // Fallback if highlighter isn't ready
            MarkdownOp op{MarkdownOp::InsertCode, str};
            if (!textCharFormatStack.isEmpty())
                op.charFmt = textCharFormatStack.top();
            if (!codeBlockFormatStack.isEmpty())
                op.blockFmt = codeBlockFormatStack.top();
            ops.push_back(op);
        }
    } else if (type == MD_TEXT_HTML) {
        ops.push_back({MarkdownOp::InsertHtml, str});
    }
}

void MarkdownParserContext::handleEmph()
{
    QTextCharFormat fmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                        : textCharFormatStack.top();
    fmt.setFontItalic(true);
    textCharFormatStack.push(fmt);

    MarkdownOp op{MarkdownOp::SetCharFormat};
    op.charFmt = fmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleStrong()
{
    QTextCharFormat fmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                        : textCharFormatStack.top();
    fmt.setFontWeight(QFont::Bold);
    textCharFormatStack.push(fmt);

    MarkdownOp op{MarkdownOp::SetCharFormat};
    op.charFmt = fmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleInlineCode()
{
    QTextCharFormat fmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                        : textCharFormatStack.top();
    fmt.setFont(monoFont, QTextCharFormat::FontPropertiesSpecifiedOnly);
    fmt.setFontFixedPitch(true);
    if (currentHeadingLevel == 0) {
        fmt.setBackground(colorMap[MarkdownRenderer::InlineCodeBackground]);
        fmt.setFontPointSize(baseFontSize * 0.90);
    }
    textCharFormatStack.push(fmt);

    MarkdownOp op{MarkdownOp::SetCharFormat};
    op.charFmt = fmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleLink(MD_SPAN_A_DETAIL *detail)
{
    QString href = QString::fromUtf8(detail->href.text, detail->href.size);
    QTextCharFormat fmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                        : textCharFormatStack.top();
    fmt.setAnchor(true);
    fmt.setAnchorHref(href);
    fmt.setForeground(colorMap[MarkdownRenderer::Link]);
    fmt.setFontUnderline(false);
    if (detail->title.size > 0)
        fmt.setToolTip(QString::fromUtf8(detail->title.text, detail->title.size));
    textCharFormatStack.push(fmt);

    MarkdownOp op{MarkdownOp::SetCharFormat};
    op.charFmt = fmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleImage(MD_SPAN_IMG_DETAIL *detail)
{
    MarkdownOp op;
    op.type = MarkdownOp::InsertImage;
    op.content = mdAttrToString(detail->src);
    ops.push_back(op);
}

void MarkdownParserContext::handleTable(MD_BLOCK_TABLE_DETAIL *detail)
{
    QTextTableFormat tblFmt;
    tblFmt.setBorder(1);
    tblFmt.setBorderStyle(QTextFrameFormat::BorderStyle_Solid);
    tblFmt.setBorderBrush(QBrush(colorMap[MarkdownRenderer::TableBorder]));
    tblFmt.setCellPadding(6);
    tblFmt.setCellSpacing(0);
    tblFmt.setTopMargin(10);
    tblFmt.setBottomMargin(10);

    TableState ts;
    ts.rows = detail->body_row_count + detail->head_row_count;
    ts.cols = detail->col_count;
    ts.curRow = -1;
    ts.curCol = -1;
    ts.colAlign.fill(Qt::AlignLeft, ts.cols);
    tableStack.push_back(ts);

    MarkdownOp op;
    op.type = MarkdownOp::InsertTable;
    op.tableRows = ts.rows;
    op.tableCols = ts.cols;
    op.tableFmt = tblFmt;
    ops.push_back(op);
}

void MarkdownParserContext::handleTableRow()
{
    auto &ts = tableStack.last();
    ts.curRow++;
    ts.curCol = -1;
}

void MarkdownParserContext::handleTableCell(MD_BLOCK_TD_DETAIL *detail)
{
    auto &ts = tableStack.last();
    ts.curCol++;
    MarkdownOp op;
    op.type = MarkdownOp::InsertTableCell;
    op.charFmt = QTextCharFormat();
    op.cellRow = ts.curRow;
    op.cellCol = ts.curCol;
    op.cellAlign = (detail->align == MD_ALIGN_CENTER)
                       ? Qt::AlignHCenter
                       : (detail->align == MD_ALIGN_RIGHT ? Qt::AlignRight : Qt::AlignLeft);
    if (ts.curRow < 1)
        op.charFmt->setFontWeight(QFont::Bold);
    else
        op.cellBgFmt.setBackground(ts.curRow % 2 == 1 ? colorMap[MarkdownRenderer::TableOddRow]
                                                      : colorMap[MarkdownRenderer::TableEvenRow]);
    ops.push_back(op);
}

void MarkdownParserContext::handleStrikethrough()
{
    QTextCharFormat fmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                        : textCharFormatStack.top();
    fmt.setFontStrikeOut(true);
    textCharFormatStack.push(fmt);
}

void MarkdownParserContext::handleHtml(const QString &html)
{
    static const QRegularExpression detailsRegex(
        R"(<details(?:\s+[^>]*)?>(?:<summary(?:\s+[^>]*)?>([\s\S]*?)<\/summary>)?([\s\S]*?)(?:<\/details>|$))",
        QRegularExpression::CaseInsensitiveOption);

    static const QRegularExpression closeDetailsRegex(R"(<\/details>)",
                                                      QRegularExpression::CaseInsensitiveOption);

    if (html.contains(QRegularExpression("<details", QRegularExpression::CaseInsensitiveOption))) {
        QRegularExpressionMatch match = detailsRegex.match(html);
        if (match.hasMatch()) {
            QString summaryText = match.captured(1).trimmed();
            QString bodyText = match.captured(2);

            int secId = ++nextDetailsId;
            QTextBlockFormat sumFmt;
            sumFmt.setProperty(MarkdownRenderer::DetailsSectionIdProp, secId);
            sumFmt.setProperty(MarkdownRenderer::DetailsToggleBlockProp, true);
            sumFmt.setProperty(MarkdownRenderer::DetailsSummaryTextProp, summaryText);

            MarkdownOp op{MarkdownOp::InsertDetails};
            op.blockFmt = sumFmt;
            op.content = summaryText.isEmpty() ? "Details" : summaryText;
            detailsIdStack.push(secId);

            ops.push_back(op);

            if (!bodyText.isEmpty()) {
                QTextBlockFormat blkFmt;
                blkFmt.setProperty(MarkdownRenderer::DetailsSectionIdProp, secId);

                MarkdownOp op{MarkdownOp::InsertBlock};
                op.blockFmt = blkFmt;
                ops.push_back(op);

                MarkdownOp textOp{MarkdownOp::InsertText, bodyText};
                textOp.charFmt = textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                               : textCharFormatStack.top();
                textOp.blockFmt = blkFmt;
                ops.push_back(textOp);
            }

            return;
        }
    }

    if (html.contains(closeDetailsRegex)) {
        if (!detailsIdStack.isEmpty()) {
            QString secId = QString::number(detailsIdStack.pop());

            int blockQuoteLevel = blockQuoteDepth + 1;

            MarkdownOp closeDetailsOp{MarkdownOp::CloseDetails};
            closeDetailsOp.blockFmt = QTextBlockFormat();
            closeDetailsOp.blockFmt->setProperty(MarkdownRenderer::DetailsSectionIdProp, secId);
            ops.push_back(closeDetailsOp);

            auto insertIt = std::find_if(ops.begin(), ops.end(), [secId](const MarkdownOp &op) {
                return op.type == MarkdownOp::InsertDetails
                       && op.blockFmt->property(MarkdownRenderer::DetailsSectionIdProp).toString()
                              == secId;
            });
            auto closeIt = std::find_if(ops.begin(), ops.end(), [secId](const MarkdownOp &op) {
                return op.type == MarkdownOp::CloseDetails
                       && op.blockFmt->property(MarkdownRenderer::DetailsSectionIdProp).toString()
                              == secId;
            });

            for (auto it = ++insertIt; it != closeIt; ++it) {
                if (it->blockFmt) {
                    QTextBlockFormat blkFmt = *it->blockFmt;
                    blkFmt.setProperty(MarkdownRenderer::DetailsSectionIdProp, secId);
                    blkFmt.setProperty(QTextFormat::BlockQuoteLevel, blockQuoteLevel);

                    blkFmt.setTopMargin(paragraphMargin);
                    blkFmt.setBottomMargin(paragraphMargin);

                    int baseIndent = paragraphMargin;
                    int extraPerLevel = 8;
                    blkFmt.setLeftMargin(baseIndent + extraPerLevel * blockQuoteLevel);

                    it->blockFmt = blkFmt;
                }

                if (it->charFmt)
                    it->charFmt->setForeground(colorMap[MarkdownRenderer::BlockquoteText]);

                if (it->listFmt)
                    it->listFmt->setIndent(blockQuoteLevel);
            }
        }

        return;
    }

    ops.push_back({MarkdownOp::InsertHtml, html});
}

QString MarkdownParserContext::mdAttrToString(const MD_ATTRIBUTE &attr)
{
    return QString::fromUtf8(attr.text, attr.size);
}

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
                summary = tr("Details");

            QTextCursor cursor(blk);
            cursor.movePosition(QTextCursor::StartOfBlock);
            cursor.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
            cursor.insertHtml(detailsHtmlLabel(summary, secId, makeVisible));

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

QString MarkdownRenderer::detailsHtmlLabel(const QString &summary, int secId, bool isVisible)
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

void MarkdownRenderer::paintEvent(QPaintEvent *ev)
{
    QPainter painter(viewport());
    const QRectF visibleRect = viewport()->rect();
    QMap<int, QRectF> codeBlocksRects = collectBlockRects(BlockCodeIdProp);
    const int codePadding = m_paragraphMargin;
    const int radius = 6;
    for (const QRectF &blkRect : std::as_const(codeBlocksRects)) {
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

    // Remove the "padding" needed by the renderer
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
