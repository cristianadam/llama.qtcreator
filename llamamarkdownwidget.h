#pragma once

#include <utils/expected.h>

#include <QLabel>
#include <QString>

namespace LlamaCpp {
class MarkdownLabel : public QLabel
{
    Q_OBJECT
public:
    explicit MarkdownLabel(QWidget *parent = nullptr);
    ~MarkdownLabel() = default;

    void setMarkdown(const QString &markdown);
    void setStyleSheet();

    void paintEvent(QPaintEvent *ev) override;

private:
    Utils::expected<QByteArray, QString> markdownToHtml(const QString &markdown) const;
    QByteArray m_css;
};
} // namespace LlamaCpp
