#pragma once

#include <QImageIOHandler>
#include <QImageIOPlugin>
#include <QMovie>
#include <QVector>

namespace LlamaCpp {

class SpinnerPlugin : public QImageIOPlugin
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE
                          "llamaspinner.json")

public:
    Capabilities capabilities(QIODevice *device, const QByteArray &format) const override;
    QImageIOHandler *create(QIODevice *device, const QByteArray &format) const override;
};

class SpinnerHandler : public QImageIOHandler
{
public:
    explicit SpinnerHandler();

    bool canRead() const override;
    bool read(QImage *image) override;

    bool jumpToImage(int imageNumber) override;
    int currentImageNumber() const override;
    int imageCount() const override;
    int loopCount() const override;
    int nextImageDelay() const override;

    QByteArray name() const;

    bool supportsOption(ImageOption option) const override;
    void setOption(ImageOption option, const QVariant &value) override;
    QVariant option(ImageOption option) const override;

private:
    int frameIndex = 0;
    QSize m_size{64, 64};
    QColor backgroundColor{Qt::transparent};
    QColor foregroundColor{Qt::black};
    QImage::Format m_imageFormat{QImage::Format_ARGB32};
    int delay{33};
};

// The single spinner movie shared by every MarkdownLabel: created lazily on
// first use and reused from then on.  A QMovie is safe to use in many
// widgets (repeated start() calls only bump an internal reference count), so
// the frames are rendered once per tick instead of once per chat message.
// Never deleted - the plugin runs for the whole app lifetime.
QMovie *sharedSpinnerMovie();

} // namespace LlamaCpp
