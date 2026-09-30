#pragma once

#include <utils/expected.h>

#include <QElapsedTimer>
#include <QString>
#include <QTextBrowser>

#include "llamasyntaxhighlighter.h"
#include "markdownrenderer.h"

namespace LlamaCpp {
class MarkdownLabel : public MarkdownRenderer
{
    Q_OBJECT
public:
    explicit MarkdownLabel(QWidget *parent = nullptr);
    ~MarkdownLabel() = default;

    void setMarkdown(const QString &markdown, bool completed);
    void resizeEvent(QResizeEvent *event) override;
    int heightForWidth(int w) const override;
    void invalidate();
    QSize sizeHint() const override;

    /* set the movie that will be used for all URLs that start with
       "spinner://".  The movie is *not* owned by the document – you can
       reuse the same QMovie for many documents if you wish. */
    void setMovie(QMovie *movie);

    void setHeightAdjustment(int newHeightAdjustment);
    int heightAdjustment() const { return m_heightAdjustment; }

    void notifyGeometryChanged() override;

protected:
    // Called by the layout whenever an image resource is required.
    QVariant loadResource(int type, const QUrl &name) override;

private slots:
    void onSpinnerFrameChanged(int);

signals:
    void copyToClipboard(const QString &verbatimText, const QString &highlightedText);

private:
    void adjustMinimumWidth(const QString &markdown);
    int commonPrefixLength(const QList<QByteArray> &a, const QList<QByteArray> &b) const;
    void updateColorPalette();
    QVariant renderSvgResource(const QUrl &name);
    void refreshSvgResources();
    // Scales a data: image URL or a local image file to the current document
    // width (null for anything else); the result is what loadResource() and
    // refreshDisplayImages() serve for those URLs.
    QVariant displayImageForUrl(const QUrl &name);
    void refreshDisplayImages();

    QElapsedTimer m_markdownConversionTimer;
    QMovie *m_spinner = nullptr;
    QSet<QUrl> m_spinnerUrls;
    QSet<QUrl> m_svgUrls;
    // data: / local-file image URLs served through displayImageForUrl(); they
    // are re-scaled and re-served on resize (like the SVGs), because
    // loadResource() is not called again for URLs the document has fetched.
    QSet<QUrl> m_displayImageUrls;
    int m_heightAdjustment{0};
};
} // namespace LlamaCpp
