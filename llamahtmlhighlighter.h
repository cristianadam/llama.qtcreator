#pragma once

#include <abstracthighlighter.h>
#include <state.h>

#include <texteditor/colorscheme.h>
#include <QString>

namespace LlamaCpp {

class HtmlHighlighter : public KSyntaxHighlighting::AbstractHighlighter
{
public:
    HtmlHighlighter();

    void setDefinition(const KSyntaxHighlighting::Definition &def) override;

    QString hightlightCodeLine(const QString &line);

    void applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format) override;
private:
    KSyntaxHighlighting::Definition m_definition;
    QVector<QString> m_htmlStyles;
    KSyntaxHighlighting::State m_state;

    QString m_outputLine;
    QString m_currentLine;
    TextEditor::ColorScheme m_colorScheme;
};
} // namespace LlamaCpp
