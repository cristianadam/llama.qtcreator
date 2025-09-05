#include <QDebug>
#include <QFile>
#include <QResizeEvent>

#include <3rdparty/md4c/src/md4c-html.h>
#include <3rdparty/md4c/src/md4c.h>

#include "llamamarkdownwidget.h"

namespace LlamaCpp {

QByteArray MarkdownLabel::m_css = R"##(
)##";

MarkdownLabel::MarkdownLabel(QWidget *parent)
    : QLabel(parent)
{
    setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void MarkdownLabel::setMarkdown(const QString &markdown)
{
    const QStringList lines = markdown.split("\n");
    const QString longestLine = *std::ranges::max_element(lines, std::less{}, &QString::length);
    QFontMetrics fm(font());
    const int longestLineWidth = fm.horizontalAdvance(longestLine) + 21;

    if (minimumWidth() == 0 || lines.size() < 5 && minimumWidth() < longestLineWidth)
        setMinimumWidth(fm.horizontalAdvance(longestLine) + 21);

    auto html = markdownToHtml(markdown);
    if (html) {
        // Inject the optional CSS before the </head> tag
        if (!m_css.isEmpty()) {
            const QByteArray headEnd = "</head>";
            int pos = html.value().indexOf(headEnd);
            if (pos != -1)
                html.value().insert(pos, QByteArray("\n<style>" + m_css + "</style>"));
            else
                html.value().prepend(QByteArray("<style>" + m_css + "</style>"));
        }

        setText(QString::fromUtf8(html.value()));
        setTextFormat(Qt::RichText);

        // emit rendered(html);
        // QFile htmlFile("rendered.html");
        // if (htmlFile.open(QFile::ReadWrite)) {
        //     htmlFile.write(html.value().toUtf8());
        // }
    } else {
        qWarning() << "Markdown conversion failed:" << html.error();
    }
}

void MarkdownLabel::setStyleSheet(const QString &css)
{
    m_css = css.toUtf8();
}

void MarkdownLabel::paintEvent(QPaintEvent *ev)
{
    return QLabel::paintEvent(ev);
}

Utils::expected<QByteArray, QString> MarkdownLabel::markdownToHtml(const QString &markdown) const
{
    if (markdown.isEmpty())
        return {};

    // md4c expects UTF‑8 data
    QByteArray md = markdown.toUtf8();

    // Prepare a std::string to capture the output
    std::string html;
    html.reserve(md.size() * 4); // heuristic

    // md4c output callback
    auto append_cb = [](const MD_CHAR *data, MD_SIZE length, void *user_data) -> void {
        std::string *out = static_cast<std::string *>(user_data);
        out->append(data, length);
    };

    // Render Markdown to HTML
    int rc = md_html(reinterpret_cast<const MD_CHAR *>(md.constData()),
                     static_cast<MD_SIZE>(md.size()),
                     append_cb,
                     reinterpret_cast<void *>(&html),
                     MD_DIALECT_GITHUB,
                     0);

    if (rc != 0)
        return Utils::make_unexpected(QString("md4c failed to render"));

    return QByteArray(html.c_str(), static_cast<int>(html.size()));
}
} // namespace LlamaCpp
