#pragma once

#include <utils/expected.h>

#include <QElapsedTimer>
#include <QString>
#include <QTextBrowser>

#include "llamahtmlhighlighter.h"

namespace LlamaCpp {
class MarkdownLabel : public QTextBrowser
{
    Q_OBJECT
public:
    explicit MarkdownLabel(QWidget *parent = nullptr);
    ~MarkdownLabel() = default;

    void setMarkdown(const QString &markdown);
    void setStyleSheet();
    void resizeEvent(QResizeEvent *event) override;
    int heightForWidth(int w) const override;
    void invalidate();
    QSize sizeHint() const override;

signals:
    void copyToClipboard(const QString &verbatimText, const QString &highlightedText);
    void saveToFile(const QString &fileName, const QString &verbatimText);

private:
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
        QByteArray outputHtml;
        QList<CodeBlock> codeBlocks;
        std::unique_ptr<HtmlHighlighter> highlighter;
        bool awaitingNewLine{false};
        QList<QByteArray> outputHtmlSections;
    };

private:
    Utils::expected<Data, QString> markdownToHtml(const QString &markdown);
    void adjustMinimumWidth(const QString &markdown);
    int commonPrefixLength(const QList<QByteArray> &a,
                                          const QList<QByteArray> &b) const;
    void removeHtmlSection(int index);
    void insertHtmlSection(const QByteArray &html, int index);
    void updateDocumentHtmlSections(const Data &newData);

    Data m_data;
    QMap<int, QPair<int, int>> m_insertedHtmlSection;
    QElapsedTimer m_markdownConversionTimer;
};
} // namespace LlamaCpp
