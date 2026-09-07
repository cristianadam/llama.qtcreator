#pragma once

#include <QString>
#include <QTextCharFormat>
#include <QVector>
#include <abstracthighlighter.h>
#include <state.h>

#include <texteditor/colorscheme.h>

namespace LlamaCpp {

// A run of text together with the char format it should be rendered with.
struct HighlightFragment
{
    QString text;
    QTextCharFormat format;
};

// Look up a syntax definition by its Qt Creator name (e.g. "cpp", "python").
KSyntaxHighlighting::Definition syntaxDefinitionForName(const QString &name);

class SyntaxHighlighter : public KSyntaxHighlighting::AbstractHighlighter
{
public:
    SyntaxHighlighter();

    void setDefinition(const KSyntaxHighlighting::Definition &def) override;

    // Highlights a complete chunk of text (e.g. a finished code block) and
    // appends the resulting formatted runs to \a fragments. Unformatted runs
    // get \a defaultFmt.
    void highlight(const QString &content,
                   const QTextCharFormat &defaultFmt,
                   QVector<HighlightFragment> &fragments);

protected:
    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override;

private:
    void processLine(const QString &line,
                     const QTextCharFormat &defaultFmt,
                     QVector<HighlightFragment> &fragments);

    KSyntaxHighlighting::Definition m_definition;
    TextEditor::ColorScheme m_colorScheme;
    KSyntaxHighlighting::State m_state;

    struct RecordedFormat
    {
        int offset;
        int length;
        QTextCharFormat charFmt;
    };
    QVector<RecordedFormat> m_recordedFormats;
    QTextCharFormat m_defaultFmt;
};
} // namespace LlamaCpp
