#include <QBuffer>
#include <QClipboard>
#include <QDesktopServices>
#include <QFile>
#include <QList>
#include <QMovie>
#include <QResizeEvent>
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

    auto policy = QSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);

    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);

    connect(document(), &QTextDocument::contentsChanged, this, &MarkdownLabel::updateGeometry);

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
    connect(this, &MarkdownRenderer::saveClicked, this, [this](const QString &code) {
        emit saveToFile(QString(), code);
    });
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

    document()->setTextWidth(viewport()->width());
    updateGeometry();
}

void MarkdownLabel::resizeEvent(QResizeEvent *event)
{
    MarkdownRenderer::resizeEvent(event);          // keep normal behaviour
    document()->setTextWidth(viewport()->width()); // re‑wrap at new width
    updateGeometry();                              // notify layout
}

int MarkdownLabel::heightForWidth(int w) const
{
    // Ask the document what height it needs for w px
    // (this does not change the widget’s real geometry)
    QTextDocument *doc = const_cast<QTextDocument *>(document());
    doc->setTextWidth(w);
    return qRound(doc->size().height() + m_heightAdjustment);
}

void MarkdownLabel::invalidate()
{
    m_markdownConversionTimer.invalidate();
}

QSize MarkdownLabel::sizeHint() const
{
    QSize sh = MarkdownRenderer::sizeHint();
    sh.setWidth(minimumWidth());
    return sh;
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

    // Default handling for everything else (e.g. normal file URLs).
    return MarkdownRenderer::loadResource(type, name);
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
        setMinimumWidth(longestLineWidth);
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

    // Overlays (the copy/save buttons)
    // Overlay background is usually transparent
    QColor overlayBg = creatorColor(Theme::Token_Background_Default);
    overlayBg.setAlpha(0);
    newPalette[MarkdownRenderer::OverlayBackground] = overlayBg;

    newPalette[MarkdownRenderer::OverlayButtonBackground] = creatorColor(
        Theme::Token_Background_Default);
    newPalette[MarkdownRenderer::OverlayButtonBackgroundHover] = creatorColor(
        Theme::Token_Foreground_Muted);
    newPalette[MarkdownRenderer::OverlayButtonBorder] = creatorColor(Theme::Token_Foreground_Muted);

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
