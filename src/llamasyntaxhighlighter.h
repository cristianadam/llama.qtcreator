#pragma once

#include <functional>

#include <QString>
#include <QStringList>
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

// The display names of a small set of commonly used syntax definitions that
// are missing from the repository.  Qt Creator only ships a small,
// license-clear subset of the KSyntaxHighlighting definitions; the rest
// (C++, Python, JavaScript, ...) are downloaded on demand (the "Download
// Definitions" button on the Text Editor settings page).  An empty list means
// all the representative definitions are present.
QStringList missingCommonSyntaxDefinitions();

// Downloads the full KSyntaxHighlighting definition set - the same action as
// the "Download Definitions" button on the Text Editor settings page.  The
// repository is reloaded when the download finishes.  \a onFinished (optional)
// is invoked on the main thread once, with a short status message.
void downloadSyntaxDefinitions(std::function<void(const QString &)> onFinished = {});

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

    // Resets the inter-line state (e.g. an open code fence). Call this
    // before highlighting a new document starting at its first block.
    void resetState();

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
