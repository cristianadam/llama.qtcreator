#include <QAbstractTextDocumentLayout>
#include <QBuffer>
#include <QClipboard>
#include <QDesktopServices>
#include <QLayout>
#include <QFile>
#include <QList>
#include <QMovie>
#include <QPainter>
#include <QResizeEvent>
#include <QSvgRenderer>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QToolTip>

#include <coreplugin/editormanager/editormanager.h>
#include <repository.h>
#include <utils/theme/theme.h>

#include "llamamarkdownwidget.h"
#include "llamatheme.h"
#include "llamatr.h"

using namespace TextEditor;
using namespace Utils;

namespace LlamaCpp {

class HoverFilter : public QObject
{
    Q_OBJECT
public:
    explicit HoverFilter(QObject *parent = nullptr)
        : QObject(parent)
    {}
    bool eventFilter(QObject *obj, QEvent *event)
    {
        if (event->type() == QEvent::MouseMove) {
            QMouseEvent *me = static_cast<QMouseEvent *>(event);
            MarkdownRenderer *te = qobject_cast<MarkdownRenderer *>(obj->parent());
            if (!te)
                return QObject::eventFilter(obj, event);

            QTextCursor cur = te->cursorForPosition(me->pos());
            if (!cur.isNull()) {
                QTextCharFormat fmt = cur.charFormat();
                if (fmt.isAnchor()) {
                    QString url = fmt.anchorHref();
                    emit linkHovered(url);
                    return true; // we handled it
                }
            }
        }
        return QObject::eventFilter(obj, event);
    }

signals:
    void linkHovered(const QString &link);
};

MarkdownLabel::MarkdownLabel(QWidget *parent)
    : MarkdownRenderer(parent)
{
    setTextInteractionFlags(Qt::TextBrowserInteraction);

    setReadOnly(true);
    setOpenLinks(false);
    setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    // Align content to the top so that when the label is taller than the
    // document (due to heightAdjustment / contentsMargins) the extra space
    // appears at the bottom, not split asymmetrically.
    setAlignment(Qt::AlignTop);

    // Use Preferred/Preferred WITHOUT heightForWidth.  We manage heights
    // explicitly via updateFixedHeight() connected to documentSizeChanged,
    // so we don't need the layout system to query heightForWidth during
    // construction (which fails because the document hasn't reflowed at
    // the final viewport width yet).
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);

    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    // Any change to the document that can change its height must invalidate
    // the height caches of the surrounding layouts; see notifyGeometryChanged().
    connect(document(), &QTextDocument::contentsChanged, this, &MarkdownLabel::notifyGeometryChanged);
    // layoutChanged() does not exist in Qt 6; documentSizeChanged() is
    // emitted whenever a re-layout changed the document size, which is
    // exactly the set of events that can change the required row height
    // (including QTextBlock::setVisible() from <details> toggling, which
    // does not emit contentsChanged()).
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            &MarkdownLabel::notifyGeometryChanged);

    viewport()->setMouseTracking(true);

    HoverFilter *hoverFilter = new HoverFilter(this);
    viewport()->installEventFilter(hoverFilter);

    QBuffer *buffer = new QBuffer(this);
    QMovie *spinnerMovie = new QMovie(buffer, "spinner", this);
    spinnerMovie->setScaledSize(QSize(14, 14));

    setMovie(spinnerMovie);

    connect(hoverFilter, &HoverFilter::linkHovered, this, [](const QString &link) {
        const int idx = link.indexOf(':');
        const QString command = link.left(idx);

        if (command == "details-toggle")
            QToolTip::showText(QCursor::pos(), Tr::tr("Toggle the details of the tool usage"));
        else
            QToolTip::showText(QCursor::pos(), link);
    });

    connect(this, &MarkdownRenderer::anchorClicked, this, [this](const QUrl &url) {
        const QString link = url.toString();
        const int idx = link.indexOf(':');
        const QString command = link.left(idx);

        if (command != "details-toggle") {
            if (url.isLocalFile())
                Core::EditorManager::openEditor(FilePath::fromUrl(url));
            else
                QDesktopServices::openUrl(url);
        }
    });

    connect(this, &MarkdownRenderer::copyClicked, this, &MarkdownLabel::copyToClipboard);
}

void MarkdownLabel::setMarkdown(const QString &markdown, bool completed)
{
    // We don't want something faster than 30fps, which would cause UI freezes
    if (m_markdownConversionTimer.isValid() && m_markdownConversionTimer.elapsed() < 33)
        return;

    m_markdownConversionTimer.start();
    auto guard = qScopeGuard([this] { m_markdownConversionTimer.restart(); });

    // Make sure the widget’s minimum width is large enough for the
    // longest line in the markdown.
    adjustMinimumWidth(markdown);

    updateColorPalette();

    feed(markdown.toUtf8());
    if (completed)
        finish();

    // Never lay the document out at width 0: the widget is usually not
    // shown yet when this first runs, and a zero text width produces
    // degenerate line wrapping (wildly wrong document height).
    const int vw = viewport()->width();
    if (vw > 0)
        document()->setTextWidth(vw);
    notifyGeometryChanged();
}

void MarkdownLabel::resizeEvent(QResizeEvent *event)
{
    MarkdownRenderer::resizeEvent(event);          // keep normal behaviour
    if (viewport()->width() > 0)
        document()->setTextWidth(viewport()->width()); // re‑wrap at new width
    // SVG images were rasterized at the old text width; re-serve them so
    // they track the new column width (loadResource() is not called again
    // for URLs the document has already fetched).
    refreshSvgResources();
    notifyGeometryChanged();                       // notify layout
}

void MarkdownLabel::notifyGeometryChanged()
{
    // The document height just changed. Invalidate the height‑for‑width
    // state of this widget *and* of every ancestor that has a layout.
    //
    // Qt 6.11 caches HFW results in two places:
    //  - QBoxLayoutPrivate::calcHfw() stores hfwWidth/hfwHeight and returns
    //    the cached height for every repeated query at the same width until
    //    the layout is invalidated (qboxlayout.cpp);
    //  - QWidgetItemV2::heightForWidth() keeps a small per‑item LRU cache of
    //    (width, height) pairs that is only cleared by
    //    QWidgetItemV2::invalidateSizeCache() (qlayoutitem.cpp).
    //
    // updateGeometry() only marks the item for re-layout but does NOT clear
    // the HFW cache.  We must call layout()->invalidate() on every ancestor
    // layout so that when a <details> section collapses, the cached (larger)
    // height is cleared and the row shrinks properly instead of leaving
    // white space.
    updateGeometry();
    for (QWidget *p = parentWidget(); p && p->layout(); p = p->parentWidget()) {
        p->layout()->invalidate();
        p->updateGeometry();
    }
}

int MarkdownLabel::heightForWidth(int w) const
{
    // heightForWidth is disabled for layout (setHeightForWidth(false)),
    // but the layout system may still query it.  Heights are managed
    // explicitly via ChatMessage::updateFixedHeight(), so just report the
    // height at the *current* text width — without reflowing the document
    // at the queried width (a side effect that produced inconsistent
    // results across sizing passes).
    Q_UNUSED(w)
    return qRound(document()->size().height() + m_heightAdjustment);
}

void MarkdownLabel::invalidate()
{
    m_markdownConversionTimer.invalidate();
}

QSize MarkdownLabel::sizeHint() const
{
    int w = width();
    if (w <= 0 && parentWidget())
        w = parentWidget()->width();
    if (w <= 0)
        w = 400;
    return QSize(w, qRound(document()->size().height() + m_heightAdjustment));
}

void MarkdownLabel::setMovie(QMovie *movie)
{
    if (m_spinner == movie)
        return;

    if (m_spinner)
        disconnect(m_spinner, &QMovie::frameChanged, this, &MarkdownLabel::onSpinnerFrameChanged);

    m_spinner = movie;

    if (m_spinner) {
        connect(m_spinner,
                &QMovie::frameChanged,
                this,
                &MarkdownLabel::onSpinnerFrameChanged,
                Qt::QueuedConnection);
        // make sure the movie is started – it will also be started lazily
        // the first time loadResource() is called.
        if (m_spinner->state() != QMovie::Running)
            m_spinner->start();
    }
}

QVariant MarkdownLabel::loadResource(int type, const QUrl &name)
{
    if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String("spinner")) {
        if (!m_spinner) {
            qWarning() << "MarkdownLabel: no movie set for spinner resource";
            return QVariant();
        }

        m_spinnerUrls.insert(name);

        // The movie already keeps the correct device pixel ratio.
        QImage frame = m_spinner->currentImage();
        if (frame.isNull()) {
            // The movie may not have produced a frame yet – force an update.
            m_spinner->jumpToNextFrame();
            frame = m_spinner->currentImage();
        }

        // Return a QImage – the layout will paint it directly.
        return frame;
    }

    if (type == QTextDocument::ImageResource && name.scheme() == QLatin1String("llamasvg")) {
        m_svgUrls.insert(name);
        return renderSvgResource(name);
    }

    // Default handling for everything else (e.g. normal file URLs).
    return MarkdownRenderer::loadResource(type, name);
}

QVariant MarkdownLabel::renderSvgResource(const QUrl &name)
{
    const QByteArray svg = svgContentForUrl(name);
    QSvgRenderer renderer(svg);
    if (!renderer.isValid())
        return {};

    // Render at the document's text width so the drawing fills the chat
    // column; smaller SVGs keep their native size (no up-scaling).
    double width = document()->textWidth();
    if (width <= 0)
        width = 600;
    const QSizeF defaultSize = renderer.defaultSize();
    double w = defaultSize.width() > 0 ? defaultSize.width() : width;
    double h = defaultSize.height() > 0 ? defaultSize.height() : w * 0.6;
    const double scale = qMin(1.0, width / w);
    w *= scale;
    h *= scale;
    constexpr double kMaxHeight = 600.0; // keep tall drawings from dominating the chat
    if (h > kMaxHeight) {
        const double factor = kMaxHeight / h;
        w *= factor;
        h *= factor;
    }

    // Render at device resolution so the picture stays crisp on HiDPI.
    const qreal dpr = devicePixelRatioF();
    QImage image(QSize(qCeil(w * dpr), qCeil(h * dpr)), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(dpr);
    QPainter painter(&image);
    renderer.render(&painter, QRectF(0, 0, w, h));
    return image;
}

void MarkdownLabel::refreshSvgResources()
{
    if (m_svgUrls.isEmpty())
        return;
    for (const QUrl &url : m_svgUrls)
        document()->addResource(QTextDocument::ImageResource, url, renderSvgResource(url));
}

void MarkdownLabel::onSpinnerFrameChanged(int)
{
    const QImage frame = m_spinner->currentImage();
    for (const QUrl &url : m_spinnerUrls)
        document()->addResource(QTextDocument::ImageResource, url, frame);

    viewport()->update();
}

void MarkdownLabel::adjustMinimumWidth(const QString &markdown)
{
    const QStringList lines = markdown.split('\n');
    const QString longestLine = *std::ranges::max_element(lines, std::less{}, &QString::length);
    QFontMetrics fm(font());
    const int longestLineWidth = fm.horizontalAdvance(longestLine) + 20;

    if (minimumWidth() == 0 || (lines.size() < 5 && minimumWidth() < longestLineWidth))
        setMinimumWidth(qMin(longestLineWidth, 600));
}

int MarkdownLabel::commonPrefixLength(const QList<QByteArray> &a, const QList<QByteArray> &b) const
{
    const int n = std::min(a.size(), b.size());
    int i = 0;
    while (i < n && a[i] == b[i])
        ++i;
    return i;
}

void MarkdownLabel::updateColorPalette()
{
    QHash<MarkdownRenderer::ColorRole, QColor> newPalette;

    // Text & Links
    newPalette[MarkdownRenderer::TextForeground] = creatorColor(Theme::Token_Text_Default);
    newPalette[MarkdownRenderer::Link] = creatorColor(Theme::Token_Accent_Default);

    // Blockquotes & Rules
    newPalette[MarkdownRenderer::BlockquoteLine] = creatorColor(Theme::Token_Foreground_Muted);
    newPalette[MarkdownRenderer::BlockquoteText] = creatorColor(Theme::Token_Text_Muted);
    newPalette[MarkdownRenderer::HorizontalRuler] = creatorColor(Theme::Token_Foreground_Muted);

    // Tables
    newPalette[MarkdownRenderer::TableBorder] = creatorColor(Theme::Token_Foreground_Muted);
    newPalette[MarkdownRenderer::TableOddRow] = creatorColor(Theme::Token_Background_Muted);
    newPalette[MarkdownRenderer::TableEvenRow] = creatorColor(Theme::Token_Background_Default);

    // Code Blocks
    newPalette[MarkdownRenderer::CodeBlockBackground] = creatorColor(Theme::Token_Background_Muted);
    newPalette[MarkdownRenderer::CodeBlockBorder] = creatorColor(Theme::Token_Foreground_Muted);
    newPalette[MarkdownRenderer::InlineCodeBackground] = creatorColor(
        Theme::Token_Background_Muted);

    setColorPalette(newPalette);

    // Also update the standard Qt Palette for the Text color
    QPalette p = palette();
    p.setColor(QPalette::Text, newPalette[MarkdownRenderer::TextForeground]);
    setPalette(p);
}

void MarkdownLabel::setHeightAdjustment(int newHeightAdjustment)
{
    m_heightAdjustment = newHeightAdjustment;
}

} // namespace LlamaCpp

#include "llamamarkdownwidget.moc"
