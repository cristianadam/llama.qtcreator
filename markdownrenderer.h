#pragma once

#include <QByteArray>
#include <QFont>
#include <QHash>
#include <QMap>
#include <QStack>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextList>
#include <QTextListFormat>
#include <QTextTable>
#include <QVector>

#include "llamasyntaxhighlighter.h"
#include <repository.h>

class QFrame;
#include <3rdparty/md4c/src/md4c.h>

namespace LlamaCpp {
struct MarkdownOp;

class MarkdownRenderer : public QTextBrowser
{
    Q_OBJECT
public:
    explicit MarkdownRenderer(QWidget *parent = nullptr);
    ~MarkdownRenderer() override;

    enum ColorRole {
        TextForeground,
        BlockquoteLine,
        BlockquoteText,
        HorizontalRuler,
        TableBorder,
        TableOddRow,
        TableEvenRow,
        CodeBlockBackground,
        CodeBlockBorder,
        InlineCodeBackground,
        Link,
        OverlayBackground,
        OverlayButtonBackground,
        OverlayButtonBackgroundHover,
        OverlayButtonBorder,
    };

    static const int DetailsSectionIdProp = QTextFormat::UserProperty;
    static const int DetailsToggleBlockProp = QTextFormat::UserProperty + 1;
    static const int DetailsSummaryTextProp = QTextFormat::UserProperty + 2;
    static const int BlockCodeIdProp = QTextFormat::UserProperty + 3;
    static const int HorizontalRulerIdProp = QTextFormat::UserProperty + 4;

    static constexpr QChar ZeroWidthSpace = QChar(L'\u200b');

    void feed(const QByteArray &chunk);
    void finish();

    void setColor(ColorRole role, const QColor &color);
    QColor color(ColorRole role) const;
    void setColorPalette(const QHash<ColorRole, QColor> &palette);
    void setBaseFont(const QFont &font);
    void setMonoFont(const QFont &font);
    void setBaseFontFamily(const QString &family);
    void setBaseFontSize(int pointSize);

    bool expandDetailsByDefault() const;
    void setExpandDetailsByDefault(bool newExpandDetailsByDefault);

    QByteArray buffer() const;
    void setBuffer(const QByteArray &newBuffer);

signals:
    void copyClicked(const QString &verbatim, const QString &formattedCode);
    void saveClicked(const QString &code);

protected:
    void paintEvent(QPaintEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    void applyOpToCursor(const MarkdownOp &op);
    void updateAllOverlaysGeometry();
    void createOverlayForCodeBlock(int blockId);
    QPointF contentOffset() const;
    QRectF blockBoundingRect(const QTextBlock &block) const;
    QTextBlock blockForCodeId(int id) const;
    QMap<int, QRectF> collectBlockRects(int prop, int skipProp = -1) const;
    QPair<QString, QString> collectCodeById(int id) const;
    void toggleSection(int secId);
    QString detailsHtmlLabel(const QString &text, int secId, bool isVisible);

    static int md_enter_block(MD_BLOCKTYPE type, void *detail, void *userdata);
    static int md_leave_block(MD_BLOCKTYPE type, void *detail, void *userdata);
    static int md_enter_span(MD_SPANTYPE type, void *detail, void *userdata);
    static int md_leave_span(MD_SPANTYPE type, void *detail, void *userdata);
    static int md_text(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size, void *userdata);

    void setupDocumentSettings();

private:
    MD_PARSER m_parser;
    QByteArray m_buffer;
    QTextDocument *m_doc = nullptr;
    QTextCursor m_cursor;

    QVector<MarkdownOp> m_lastOps;
    QStack<QTextTable *> m_tableStack;
    QStack<QTextList *> m_listStack;
    QStack<QTextBlock> m_detailsStartStack;
    QMap<int, bool> m_toggleDetails;

    QHash<int, QFrame *> m_codeOverlays;
    QFont m_baseFont;
    QFont m_monoFont;
    double m_baseFontSize = 0.0;
    int m_paragraphMargin = 0;
    QHash<ColorRole, QColor> m_colorMap;
    bool m_expandDetailsByDefault = true;
};

// Represents a single atomic instruction for the QTextDocument.
// This allows us to "replay" the exact state of the parser.
struct MarkdownOp
{
    enum Type {
        InsertText,
        InsertCode,
        InsertHeading,
        InsertCodeBlock,
        InsertBlock,
        InsertDetails,
        InsertHtml,
        InsertImage,
        InsertTable,
        InsertTableCell,
        InsertThematicBreak,
        InsertList,
        SetCharFormat,
        SetBlockFormat,
        CloseTable,
        CloseList,
        CloseDetails,
    };

    Type type;
    QString content;
    std::optional<QTextCharFormat> charFmt;
    std::optional<QTextBlockFormat> blockFmt;
    std::optional<QTextListFormat> listFmt;
    QTextTableFormat tableFmt;

    // Table metadata
    int tableRows = 0;
    int tableCols = 0;
    int cellRow = 0;
    int cellCol = 0;
    Qt::Alignment cellAlign = Qt::AlignLeft;
    QTextCharFormat cellBgFmt;

    // Target position for MoveCursor or the position AFTER this op is applied
    int docPosition = 0;

    bool operator==(const MarkdownOp &other) const
    {
        return type == other.type && content == other.content && charFmt == other.charFmt
               && blockFmt == other.blockFmt && listFmt == other.listFmt
               && tableFmt == other.tableFmt && tableRows == other.tableRows
               && tableCols == other.tableCols && cellRow == other.cellRow
               && cellCol == other.cellCol && cellAlign == other.cellAlign
               && cellBgFmt == other.cellBgFmt;
    }
    bool operator!=(const MarkdownOp &other) const { return !(*this == other); }
};

struct MarkdownParserContext
{
    struct ListState
    {
        QTextListFormat fmt;
    };
    struct TableState
    {
        int rows = 0;
        int cols = 0;
        int curRow = -1;
        int curCol = -1;
        QVector<Qt::Alignment> colAlign;
    };

    QVector<MarkdownOp> ops;
    QStack<ListState> listStack;
    QStack<TableState> tableStack;
    QStack<QTextCharFormat> textCharFormatStack;
    QStack<QTextBlockFormat> codeBlockFormatStack;
    QStack<int> detailsIdStack;

    int blockQuoteDepth = 0;
    int currentHeadingLevel = 0;
    bool skipNextParagraph = false;
    int nextDetailsId = 0;
    int nextCodeBlockId = 0;

    QFont baseFont;
    QFont monoFont;
    double baseFontSize = 0.0;
    int paragraphMargin = 0;
    QHash<MarkdownRenderer::ColorRole, QColor> colorMap;
    static const int indentWidth = 30;
    std::unique_ptr<SyntaxHighlighter> highlighter;

    static KSyntaxHighlighting::Repository *highlightRepository();
    static KSyntaxHighlighting::Definition definitionForName(const QString &name);

    int getBlockQuoteMargin(int depth) const;

    // Parser logic methods
    void handleHeading(MD_BLOCK_H_DETAIL *detail);
    void handleParagraph();
    void handleBlockQuote();
    void handleCodeBlock(MD_BLOCK_CODE_DETAIL *detail);
    void handleThematicBreak();
    void handleList(MD_BLOCKTYPE type, void *detail);
    void handleItem(MD_BLOCK_LI_DETAIL *detail);
    void handleText(MD_TEXTTYPE type, const MD_CHAR *text, MD_SIZE size);
    void handleEmph();
    void handleStrong();
    void handleInlineCode();
    void handleLink(MD_SPAN_A_DETAIL *detail);
    void handleImage(MD_SPAN_IMG_DETAIL *detail);
    void handleTable(MD_BLOCK_TABLE_DETAIL *detail);
    void handleTableRow();
    void handleTableCell(MD_BLOCK_TD_DETAIL *detail);
    void handleStrikethrough();
    void handleHtml(const QString &html);

    QString mdAttrToString(const MD_ATTRIBUTE &attr);
};
} // namespace LlamaCpp