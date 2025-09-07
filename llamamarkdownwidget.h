#pragma once

#include <utils/expected.h>

#include <QLabel>
#include <QString>

#include "llamahtmlhighlighter.h"

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

signals:
    void copyToClipboard(const QString &verbatimText, const QString &highlightedText);
    void saveToFile(const QString &fileName, const QString &verbatimText);

private:
    Utils::expected<QByteArray, QString> markdownToHtml(const QString &markdown);
    QByteArray m_css;

    struct CodeBlock
    {
        std::optional<QString> language;
        std::optional<QString> fileName;
        QString verbatimCode;
        QString hightlightedCode;
    };

    struct Data
    {
        enum State {
            NormalHtml = 0,
            PreCode,
            Class,
            LanguageName,
            PreCodeEndQuote,
            PreCodeEndTag,
            SourceFile,
            Code,
        };
        State state{NormalHtml};
        QByteArray output_html;
        QList<CodeBlock> codeBlocks;
        std::unique_ptr<HtmlHighlighter> highlighter;
        bool awaitingNewLine{false};
    };

    Data m_data;
};
} // namespace LlamaCpp
