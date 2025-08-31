#pragma once

#include <utils/expected.h>

#include <QLabel>
#include <QString>
#include <container_qpainter.h>

#define USE_QLABEL

namespace LlamaCpp {
class MarkdownLabel : public QLabel
{
    Q_OBJECT
public:
    explicit MarkdownLabel(QWidget *parent = nullptr);
    ~MarkdownLabel() = default;

    void setMarkdown(const QString &markdown);
    void setStyleSheet(const QString &css);

    void paintEvent(QPaintEvent *ev) override;
#ifndef USE_QLABEL
    void resizeEvent(QResizeEvent *ev) override;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    int heightForWidth(int w) const override;
#endif
private:
    mutable DocumentContainer m_container;
    DocumentContainerContext m_context;
    mutable QHash<int, int> m_heightForWidth;

signals:
    // /** Emitted when the Markdown → HTML conversion succeeds. */
    // void rendered(const QString &html);

    // /** Emitted if an error occurs while parsing the Markdown. */
    // void error(const QString &errorString);

private:
    Utils::expected<QByteArray, QString> markdownToHtml(const QString &markdown) const;

    static QByteArray m_css;
};
} // namespace LlamaCpp
