#pragma once

#include <QString>
#include <QTextCharFormat>
#include <QVector>
#include <abstracthighlighter.h>
#include <state.h>

#include <texteditor/colorscheme.h>

namespace LlamaCpp {

struct MarkdownParserContext;

class SyntaxHighlighter : public KSyntaxHighlighting::AbstractHighlighter
{
public:
    SyntaxHighlighter();

    void setDefinition(const KSyntaxHighlighting::Definition &def) override;

    // Processes a chunk of text that might contain multiple lines
    void processChunk(const QString &chunk, MarkdownParserContext *ctx);

    // Flushes any leftover text (the last line of a block)
    void finish(MarkdownParserContext *ctx);

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override;

private:
    void processLine(const QString &line, MarkdownParserContext *ctx);

    KSyntaxHighlighting::Definition m_definition;
    TextEditor::ColorScheme m_colorScheme;
    KSyntaxHighlighting::State m_state;
    QString m_leftover; // Stores partial lines between chunks

    struct RecordedFormat
    {
        int offset;
        int length;
        QTextCharFormat charFmt;
    };
    QVector<RecordedFormat> m_recordedFormats;
    QTextCharFormat m_defaultFmt;
    QTextBlockFormat m_defaultBlockFmt;
};
} // namespace LlamaCpp
