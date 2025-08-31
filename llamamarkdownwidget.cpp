#include <QDebug>
#include <QFile>
#include <QResizeEvent>

#include <3rdparty/md4c/src/md4c-html.h>
#include <3rdparty/md4c/src/md4c.h>

#include "llamamarkdownwidget.h"

namespace LlamaCpp {

QByteArray MarkdownLabel::m_css = R"##(
R"##(
html {
    display: block;
    height:100%;
    width:100%;
    position: relative;
}

head {
    display: none
}

meta {
    display: none
}

title {
    display: none
}

link {
    display: none
}

style {
    display: none
}

script {
    display: none
}

body {
    display:block;
    margin:8px;
    height:100%;
    width:100%;
}

p {
    display:block;
    margin-top:1em;
    margin-bottom:1em;
}

b, strong {
    display:inline;
    font-weight:bold;
}

i, em {
    display:inline;
    font-style:italic;
}

center
{
    text-align:center;
    display:block;
}

a:link
{
    text-decoration: underline;
    color: #00f;
    cursor: pointer;
}

h1, h2, h3, h4, h5, h6, div {
    display:block;
}

h1 {
    font-weight:bold;
    margin-top:0.67em;
    margin-bottom:0.67em;
    font-size: 2em;
}

h2 {
    font-weight:bold;
    margin-top:0.83em;
    margin-bottom:0.83em;
    font-size: 1.5em;
}

h3 {
    font-weight:bold;
    margin-top:1em;
    margin-bottom:1em;
    font-size:1.17em;
}

h4 {
    font-weight:bold;
    margin-top:1.33em;
    margin-bottom:1.33em
}

h5 {
    font-weight:bold;
    margin-top:1.67em;
    margin-bottom:1.67em;
    font-size:.83em;
}

h6 {
    font-weight:bold;
    margin-top:2.33em;
    margin-bottom:2.33em;
    font-size:.67em;
}

br {
    display:inline-block;
}

br[clear="all"]
{
    clear:both;
}

br[clear="left"]
{
    clear:left;
}

br[clear="right"]
{
    clear:right;
}

span {
    display:inline
}

img {
    display: inline-block;
}

img[align="right"]
{
    float: right;
}

img[align="left"]
{
    float: left;
}

hr {
    display: block;
    margin-top: 0.5em;
    margin-bottom: 0.5em;
    margin-left: auto;
    margin-right: auto;
    border-style: inset;
    border-width: 1px
}


/***************** TABLES ********************/

table {
    display: table;
    border-collapse: separate;
    border-spacing: 2px;
    border-top-color:gray;
    border-left-color:gray;
    border-bottom-color:black;
    border-right-color:black;
}

tbody, tfoot, thead {
    display:table-row-group;
    vertical-align:middle;
}

tr {
    display: table-row;
    vertical-align: inherit;
    border-color: inherit;
}

td, th {
    display: table-cell;
    vertical-align: inherit;
    border-width:1px;
    padding:1px;
}

th {
    font-weight: bold;
}

table[border] {
    border-style:solid;
}

table[border|=0] {
    border-style:none;
}

table[border] td, table[border] th {
    border-style:solid;
    border-top-color:black;
    border-left-color:black;
    border-bottom-color:gray;
    border-right-color:gray;
}

table[border|=0] td, table[border|=0] th {
    border-style:none;
}

caption {
    display: table-caption;
}

td[nowrap], th[nowrap] {
    white-space:nowrap;
}

tt, code, kbd, samp {
    font-family: monospace
}

pre, xmp, plaintext, listing {
    display: block;
    font-family: monospace;
    white-space: pre;
    margin: 1em 0
}

/***************** LISTS ********************/

ul, menu, dir {
    display: block;
    list-style-type: disc;
    margin-top: 1em;
    margin-bottom: 1em;
    margin-left: 0;
    margin-right: 0;
    padding-left: 40px
}

ol {
    display: block;
    list-style-type: decimal;
    margin-top: 1em;
    margin-bottom: 1em;
    margin-left: 0;
    margin-right: 0;
    padding-left: 40px
}

li {
    display: list-item;
}

ul ul, ol ul {
    list-style-type: circle;
}

ol ol ul, ol ul ul, ul ol ul, ul ul ul {
    list-style-type: square;
}

dd {
    display: block;
    margin-left: 40px;
}

dl {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
    margin-left: 0;
    margin-right: 0;
}

dt {
    display: block;
}

ol ul, ul ol, ul ul, ol ol {
    margin-top: 0;
    margin-bottom: 0
}

blockquote {
    display: block;
    margin-top: 1em;
    margin-bottom: 1em;
    margin-left: 40px;
    margin-right: 40px;
}

/*********** FORM ELEMENTS ************/

form {
    display: block;
    margin-top: 0em;
}

option {
    display: none;
}

input, textarea, keygen, select, button, isindex {
    margin: 0em;
    color: initial;
    line-height: normal;
    text-transform: none;
    text-indent: 0;
    text-shadow: none;
    display: inline-block;
}
input[type="hidden"] {
    display: none;
}

article, aside, footer, header, hgroup, nav, section
{
    display: block;
}

)##";

MarkdownLabel::MarkdownLabel(QWidget *parent)
    : QLabel(parent)
{
    QSizePolicy policy = sizePolicy();
    policy.setHeightForWidth(true);
    if (policy != sizePolicy()) // ### should be replaced by WA_WState_OwnSizePolicy idiom
        setSizePolicy(policy);

    setTextInteractionFlags(Qt::TextSelectableByMouse);
}

void MarkdownLabel::setMarkdown(const QString &markdown)
{
    auto html = markdownToHtml(markdown);
    if (html) {
        // Feed the HTML to qlitehtml

#ifdef USE_QLABEL
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
#else
        m_container.setDefaultFont(font());
        m_context.setMasterStyleSheet(QString::fromUtf8(m_css));
        m_container.setDocument(html.value(), &m_context);
        m_container.render(width(), height());
        qDebug() << Q_FUNC_INFO << m_container.documentWidth() << m_container.documentHeight()
                 << width() << height();

        resize(m_container.documentWidth(), m_container.documentHeight());
        m_container.render(m_container.documentWidth(), m_container.documentHeight());

        m_heightForWidth.clear();
        m_heightForWidth[m_container.documentWidth()] = m_container.documentHeight();

        updateGeometry();
        qDebug() << Q_FUNC_INFO << m_container.documentWidth() << m_container.documentHeight()
                 << width() << height();
#endif

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
#ifdef USE_QLABEL
    return QLabel::paintEvent(ev);
#else
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::Antialiasing, true);
    m_container.draw(&p, contentsRect());

    qDebug() << Q_FUNC_INFO << m_container.documentWidth() << m_container.documentHeight()
             << contentsRect();
#endif
}

#ifndef USE_QLABEL
void MarkdownLabel::resizeEvent(QResizeEvent *ev)
{
    qDebug() << Q_FUNC_INFO;
    m_container.render(ev->size().width(), ev->size().height());

    m_heightForWidth.clear();
    m_heightForWidth[m_container.documentWidth()] = m_container.documentHeight();

    updateGeometry();
}

QSize MarkdownLabel::sizeHint() const
{
    return {m_container.documentWidth(), m_container.documentHeight()};
}

QSize MarkdownLabel::minimumSizeHint() const
{
    return {m_container.documentWidth(), m_container.documentHeight()};
}

int MarkdownLabel::heightForWidth(int w) const
{
    if (m_heightForWidth.contains(w))
        return m_heightForWidth[w];

    m_container.render(w, 0);

    m_heightForWidth[w] = m_container.documentHeight();
    qDebug() << Q_FUNC_INFO << w << m_container.documentHeight();

    return m_container.documentHeight();
}
#endif

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
