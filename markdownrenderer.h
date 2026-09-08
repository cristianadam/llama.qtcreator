#pragma once

#include <QByteArray>
#include <QColor>
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

#include <3rdparty/markus/markus.h>

class QFrame;

namespace LlamaCpp {

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

    // Feed the full markdown buffer rendered so far. Only the suffix that is
    // new compared to the previous buffer is parsed; if the buffer diverged
    // (e.g. thinking sections rewritten) the renderer resets and re-renders.
    void feed(const QByteArray &buffer);
    void finish();
    void reset();

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
    struct ListState
    {
        QTextListFormat fmt;
        QTextList *list = nullptr;
    };
    struct TableState
    {
        QTextTable *qtTable = nullptr;
        int columns = 0;
        QVector<Qt::Alignment> colAlign;
        bool header = false;
        int curRow = 0;
        int curCol = -1;
    };

    // Render the finalized top-level blocks delivered by the streaming parser.
    void renderBlocks(const markus::Document &doc, size_t first, size_t last);
    void renderBlock(const markus::Document &doc, const markus::BlockNode &node);
    void renderBlockIds(const markus::Document &doc,
                        const std::pmr::vector<markus::BlockNodeId> &ids);
    void renderInlines(const markus::Document &doc,
                       const std::pmr::vector<markus::InlineNodeId> &ids);
    void renderInline(const markus::Document &doc, markus::InlineNodeId id);
    // Render a balanced inline-HTML element (e.g. <img .../> or <span>x</span>)
    // with a single insertHtml so Qt keeps the element intact; reports the
    // number of consumed nodes in outEnd. Returns false if it cannot be
    // rendered atomically (the caller then falls back to per-node rendering).
    bool renderInlineHtmlElement(const markus::Document &doc,
                                 const std::pmr::vector<markus::InlineNodeId> &ids,
                                 size_t start, size_t &outEnd);

    // Re-render the parser's held-back tail (the block that may still grow),
    // so in-progress paragraphs, code blocks and <details> sections stream
    // live instead of waiting for their terminator.
    void renderPendingTail();
    void clearTailRegion();
    void pruneStaleCodeBlocks();
    int documentEndPosition() const;

    void beginBlock();
    void handleHeading(int level);
    void leaveHeading();
    void handleParagraph();
    void handleBlockQuote();
    void leaveBlockQuote();
    void handleCodeBlock(const markus::CodeBlock &code);
    void handleThematicBreak();
    void handleList(const markus::List &list);
    void leaveList();
    void handleItem(const markus::ListItem &item);
    void renderDetails(const markus::Document &doc, const markus::DetailsBlock &details);
    void renderGenericHtmlBlock(const markus::HtmlBlock &block);
    void renderTable(const markus::Document &doc, const markus::Table &table);
    void renderImage(const markus::Image &img);
    void handleEmph();
    void handleStrong();
    void handleStrikethrough();
    void handleInlineCode();
    void handleLink(const markus::Link &link);
    void popCharFormat();

    void createOverlayForCodeBlock(int blockId);
    void updateAllOverlaysGeometry();
    void toggleSection(int secId);
    void setupDocumentSettings();

    static QString languageFromInfoString(const markus::CodeBlock &code);
    static int getBlockQuoteMargin(int depth, int paragraphMargin);
    // Expand/collapse icon rendered at the start of a <details> header. Wraps
    // the glyph in a "details-toggle" anchor so it gets the hover tooltip and
    // a non-link colour; the glyph itself is drawn with the icon font.
    QString sectionIconHtml(int secId, bool isVisible) const;

    QPointF contentOffset() const;
    QRectF blockBoundingRect(const QTextBlock &block) const;
    QTextBlock blockForCodeId(int id) const;
    QPair<QString, QString> collectCodeById(int id) const;
    QMap<int, QRectF> collectBlockRects(int prop, int skipProp = -1) const;

    markus::StreamingBlockParser m_streamParser;
    markus::Options m_options;
    QByteArray m_buffer;
    QTextDocument *m_doc = nullptr;
    QTextCursor m_cursor;
    // Document position where the in-progress tail starts (-1 = none).
    // Kept as a plain position rather than a QTextCursor so that appending
    // text does not drag the boundary forward (which would defeat clearing).
    int m_tailStart = -1;

    QVector<ListState> m_listStack;
    QVector<TableState> m_tableStack;
    QStack<QTextCharFormat> m_textCharFormatStack;
    QMap<int, bool> m_toggleDetails;
    // Section id of the <details> block currently being rendered (0 = none).
    // beginBlock() tags every inner block with it so the section can be
    // toggled as a unit.
    int m_detailsSecId = 0;

    QHash<int, QFrame *> m_codeOverlays;
    int m_blockQuoteDepth = 0;
    // Level of the heading currently being rendered (0 = none). While non-zero,
    // inline markup (e.g. `code`) inherits the heading's font instead of the
    // inline-code "chip" styling.
    int m_headingLevel = 0;
    bool m_codeBlock = false;
    QString m_codeBlockLanguage;
    QChar m_codeFenceChar = QChar::Null;
    int m_nextDetailsId = 0;
    int m_nextCodeBlockId = 0;
    int m_paragraphMargin = 0;
    bool m_skipNextParagraphBlock = false;
    bool m_expandDetailsByDefault = true;
    double m_baseFontSize = 0.0;
    QFont m_baseFont;
    QFont m_monoFont;
    QHash<ColorRole, QColor> m_colorMap;
};
} // namespace LlamaCpp
