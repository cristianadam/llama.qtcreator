#pragma once

#include <QByteArray>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QList>
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
class QMediaPlayer;
class QVideoWidget;

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
    };

    static const int DetailsSectionIdProp = QTextFormat::UserProperty;
    static const int DetailsToggleBlockProp = QTextFormat::UserProperty + 1;
    static const int DetailsSummaryTextProp = QTextFormat::UserProperty + 2;
    static const int BlockCodeIdProp = QTextFormat::UserProperty + 3;
    static const int HorizontalRulerIdProp = QTextFormat::UserProperty + 4;
    // Indent level of a non-quoted <details> body (tool output). Used by the
    // code-block overlay geometry, mirroring the BlockQuoteLevel adjustment.
    static const int DetailsBodyIndentProp = QTextFormat::UserProperty + 5;
    // Set on the placeholder block of a mermaid diagram that is still being
    // rendered: holds the pending key that onMermaidRendered() looks up to
    // swap the spinner for the finished picture (or to remove it on error).
    static const int MermaidPendingKeyProp = QTextFormat::UserProperty + 6;

    static constexpr QChar ZeroWidthSpace = QChar(L'\u200b');

    // Three-per-em space (U+2004, ~4 px at the default size) inserted on
    // both sides of a non-heading inline code span: it gives the painted
    // chip (paintInlineCodeChips()) visible padding around the code without
    // inserting regular space characters into the document text. (The hair
    // space U+200A would be the natural choice, but it is only ~0.8 px wide
    // in the system font — invisible; U+2005 at ~3 px felt too thin.) It is
    // stripped again on clipboard copy (copy()) and in
    // ChatMessage::plainText().
    static constexpr QChar InlineCodePadding = QChar(L'\u2004');

    // A rendered diagram SVG (mermaid diagram, KaTeX math) persisted with
    // the message that displayed it (a "diagram" entry in Message.extra),
    // so that reopening a conversation does not pay the render cost again
    // and the Markdown export can embed the picture. \a context holds the
    // render parameters baked into the SVG (the mermaid theme; the KaTeX
    // fill colour and font size): an entry is only reused when the context
    // matches, otherwise the diagram is re-rendered and the entry updated.
    struct DiagramSvg
    {
        QByteArray svg;
        QString context;
        double width = 0; // math only: re-boxed image size in px
        double height = 0;
    };

    // Cache keys, derived from the diagram source only (not from the
    // per-render URL), so the persisted entries can be matched again when
    // the message is re-rendered or exported.
    static QString mermaidDiagramKey(const QString &source);
    static QString katexDiagramKey(const QString &tex, bool display);

    // The diagram SVGs (mermaid diagrams, math) persisted with a message
    // ("diagram" entries in its extra field), keyed as the renderer keys
    // them.
    static QMap<QString, DiagramSvg> diagramSvgsFromExtra(const QList<QVariantMap> &extra);

    // Embeds the persisted diagram SVGs into a Markdown export: ```mermaid
    // blocks become <details> sections with the rendered picture in the
    // header and the source in the body (mirroring the chat UI), and
    // $...$ / $$...$$ spans become inline SVGs. Diagrams without a cached
    // SVG are left untouched.
    static QString embedDiagramSvgs(QString content, const QMap<QString, DiagramSvg> &diagrams);

    // Seeds the persistent diagram cache (from Message.extra). Entries are
    // consulted before the render engines and survive reset().
    void seedDiagramCache(const QString &key, const DiagramSvg &entry);

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

    bool collapseToolCallsByDefault() const;
    void setCollapseToolCallsByDefault(bool newCollapseToolCallsByDefault);

    QByteArray buffer() const;
    void setBuffer(const QByteArray &newBuffer);

    // Raw SVG stored for a "llamasvg://" image URL (see renderSvgCodeBlock);
    // empty for unknown or foreign URLs.  MarkdownLabel::loadResource() turns
    // it into pixels.
    QByteArray svgContentForUrl(const QUrl &url) const;

    // Local file path behind a "llamavideo://" image URL (see
    // renderVideoElement); empty for unknown or foreign URLs.
    QString videoPathForUrl(const QUrl &url) const;

    // Viewport rect of the "llamavideo://" image fragment \a key (empty if
    // not found or in a hidden block); the QVideoWidget overlay is kept
    // exactly over it.
    QRectF videoFragmentRect(const QString &key) const;

    // Copies the selection with the inline-code chip padding stripped (it is
    // layout padding only, not user text). QTextEdit's copy() slot is not
    // virtual, so keyPressEvent() routes the Copy shortcut here and
    // mouseReleaseEvent() redoes drag-copies (written by QTextEdit itself).
    void copySelection();

    //! Prepares \a image for inline display: scaled down to fit \a maxWidth
    //! (smaller images keep their native size), height capped at 600 px
    //! (like the SVG drawings), rasterized at \a devicePixelRatio so the
    //! picture stays crisp on Retina.  Strong down-scales are done in
    //! halving steps with a final smooth pass, which resamples noticeably
    //! better than a single large scale.
    static QImage scaledImageForDisplay(const QImage &image, double maxWidth,
                                        qreal devicePixelRatio);

signals:
    void copyClicked(const QString &verbatim, const QString &formattedCode);

    // A diagram SVG was freshly rendered (not served from the persistent
    // cache): the chat UI persists it with the message (Message.extra) so
    // the next render — conversation reopen, export — can reuse it.
    void diagramRendered(const QString &key, const DiagramSvg &svg);

protected:
    QVariant loadResource(int type, const QUrl &name) override;

    void paintEvent(QPaintEvent *ev) override;

    // Paints the rounded inline-code chip backgrounds; see
    // paintInlineCodeChips() in the .cpp.
    void paintInlineCodeChips(QPainter &painter, const QRectF &visibleRect);
    void keyPressEvent(QKeyEvent *ev) override;
    void mousePressEvent(QMouseEvent *ev) override;
    void mouseReleaseEvent(QMouseEvent *ev) override;
    void resizeEvent(QResizeEvent *event) override;

    // Called whenever the rendered document changed in a way that can change
    // its height (content edit, text-width change, <details> toggle). The
    // base implementation just requests a re-layout of this widget;
    // MarkdownLabel overrides it to also invalidate the height-for-width
    // caches of every ancestor layout (see the .cpp for why that is needed).
    virtual void notifyGeometryChanged();

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
    // Renders a code block as (optionally highlighted) source text, i.e. the
    // regular code view.
    void renderCodeBlockText(const markus::CodeBlock &code);
    // Renders a complete SVG code block as a <details> section: the rendered
    // picture in the (collapsed by default) header, the source in the body.
    // Returns false when the block should fall back to the regular code view
    // (still streaming or invalid SVG).
    bool renderSvgCodeBlock(const markus::CodeBlock &code);
    // Same for a complete ```mermaid code block, rendered through the
    // MermaidEngine (QuickJS + mermaid.js). Returns false while the block is
    // still streaming or when the diagram is invalid.
    bool renderMermaidCodeBlock(const markus::CodeBlock &code);
    // Shared layout of renderSvgCodeBlock()/renderMermaidCodeBlock(): a
    // <details> section with the picture (\a imageUrl, e.g. "llamasvg://..."
    // or the "spinner://..." busy placeholder while a mermaid render is in
    // flight) in the (collapsed by default) header and the source as a code
    // block in the body. \a svg is the image content to serve from
    // svgContentForUrl(); pass an empty one for the spinner placeholder.
    // \a pendingKey (mermaid only) tags the picture block as in-flight.
    bool renderDiagramAsDetails(const markus::CodeBlock &code, const QByteArray &svg,
                                const QString &imageUrl, const QString &summaryText,
                                const std::string &bodyInfoString = "xml",
                                const QString &pendingKey = QString());
    // The async render finished for the in-flight diagram \a key: \a svg is
    // the rendered picture, or empty on failure. Swaps the spinner
    // placeholder for the picture, or removes the placeholder line on
    // failure (the source stays available in the details body).
    void onMermaidRendered(const QString &key, const QByteArray &svg);
    // Renders a <video> element: a "llamavideo://" image fragment (served as
    // a 16:9 placeholder) plus a QMediaPlayer in a QVideoWidget overlay that
    // updateAllOverlaysGeometry() keeps over the fragment. Missing files
    // leave a visible note instead.
    void renderVideoElement(const QString &src, double width, double height,
                            bool muted, bool loop, bool autoplay);
    // The ordinal id of the next <details> section; tail sections get the
    // stable ids they will have once finalized, so user expand/collapse
    // choices survive tail re-renders.
    int nextDetailsId();
    // Sets the left margin/indent (and quote/details/list tags) on \a fmt so
    // the block lines up with the surrounding code blocks.
    void applyHorizontalIndent(QTextBlockFormat &fmt) const;
    void handleThematicBreak();
    void handleList(const markus::List &list);
    void leaveList();
    void handleItem(const markus::ListItem &item);
    void renderDetails(const markus::Document &doc, const markus::DetailsBlock &details);
    void renderGenericHtmlBlock(const markus::HtmlBlock &block);
    void renderTable(const markus::Document &doc, const markus::Table &table);
    void renderImage(const markus::Image &img);
    // Renders a LaTeX math span ($...$ or $$...$$) as an inline SVG image
    // via the KaTeXEngine; falls back to the verbatim source on failure.
    void renderMath(const markus::Math &math);
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
    static int getDetailsBodyMargin(int depth);
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
    // Indent level of a <details> body that is not a block quote (tool-call
    // output). Indents the body so it reads as sub-content, but paintEvent()
    // only draws the vertical line for BlockQuoteLevel blocks, so there is no
    // quote line, no muted text color, and code blocks keep their syntax
    // highlighting.
    int m_detailsBodyDepth = 0;
    // Level of the heading currently being rendered (0 = none). While non-zero,
    // inline markup (e.g. `code`) inherits the heading's font instead of the
    // inline-code "chip" styling.
    int m_headingLevel = 0;
    bool m_codeBlock = false;
    QString m_codeBlockLanguage;
    QChar m_codeFenceChar = QChar::Null;
    int m_nextDetailsId = 0;
    // Index of the <details> block within the in-progress tail being rendered
    // (-1 = not rendering the tail). Gives tail sections the stable ordinal
    // ids they will have once finalized.
    int m_tailDetailsIndex = -1;
    int m_nextCodeBlockId = 0;
    // Document ranges of the inline `code` spans (outside headings).
    // paintInlineCodeChips() paints the rounded chip background behind them;
    // the char format carries no background of its own. Stable for the
    // finalized document; pruned by clearTailRegion() and reset().
    struct InlineCodeRange
    {
        int start = 0;
        int end = 0;
    };
    QVector<InlineCodeRange> m_inlineCodeRanges;
    // Viewport position of the last mouse press; mouseReleaseEvent() uses it
    // to detect drag-copies (which QTextEdit puts on the clipboard itself).
    QPoint m_pressPos;
    // True while renderPendingTail() is re-rendering the in-progress tail.
    // Diagrams (mermaid) are only rendered for finalized blocks: rendering
    // is comparatively expensive and the tail changes on every feed, so a
    // streaming ```mermaid block shows its source and switches to the picture
    // once the fence is closed.
    bool m_renderingTail = false;
    // SVG source for llamasvg:// image URLs, keyed by a hash of the content
    // (SVG blocks) or by the pending key (mermaid blocks, which get it as
    // soon as the async render finishes).
    QHash<QString, QByteArray> m_svgStore;
    // Persistent diagram cache (mermaid + math), keyed by
    // mermaidDiagramKey()/katexDiagramKey(). Seeded from Message.extra and
    // grown as diagrams render; unlike m_svgStore it survives reset(), so a
    // divergent re-feed does not lose the persisted SVGs.
    QHash<QString, DiagramSvg> m_diagramCache;
    // <video> elements: "vid-<hash>" key to the local file path, and the
    // playing QMediaPlayer + QVideoWidget overlay per key (the widget is a
    // viewport child, re-positioned by updateAllOverlaysGeometry()).
    struct Video
    {
        QMediaPlayer *player = nullptr;
        QVideoWidget *widget = nullptr;
    };
    QHash<QString, QString> m_videoStore;
    QHash<QString, Video> m_videos;
    // Ordinal counter for pending mermaid diagram keys ("mmd-<n>").
    int m_nextMermaidKey = 0;
    int m_paragraphMargin = 0;
    bool m_skipNextParagraphBlock = false;
    bool m_expandDetailsByDefault = true;
    bool m_collapseToolCallsByDefault = true;
    double m_baseFontSize = 0.0;
    QFont m_baseFont;
    QFont m_monoFont;
    QHash<ColorRole, QColor> m_colorMap;
};
} // namespace LlamaCpp
