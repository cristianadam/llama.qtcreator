#include "llamasyntaxhighlighter.h"
#include "markdownrenderer.h"
#include <abstracthighlighter_p.h>
#include <definition.h>
#include <definition_p.h>
#include <format.h>

#include <repository.h>
#include <state.h>
#include <theme.h>

#include <texteditor/fontsettings.h>
#include <texteditor/texteditorsettings.h>
#include <utils/theme/theme.h>

namespace LlamaCpp {

using namespace TextEditor;
using namespace Internal;

// Helper to map KSyntaxHighlighting styles to our Theme/TextEditor styles
static TextStyle categoryForTextStyle(int style, const KSyntaxHighlighting::Definition &definition)
{
    switch (style) {
    case KSyntaxHighlighting::Theme::Normal:
        return C_TEXT;
    case KSyntaxHighlighting::Theme::Keyword:
        return C_KEYWORD;
    case KSyntaxHighlighting::Theme::Function:
        return C_FUNCTION;
    case KSyntaxHighlighting::Theme::Variable:
        return definition.name() == "Diff" ? C_ADDED_LINE : C_LOCAL;
    case KSyntaxHighlighting::Theme::ControlFlow:
        return C_KEYWORD;
    case KSyntaxHighlighting::Theme::Operator:
        return C_OPERATOR;
    case KSyntaxHighlighting::Theme::BuiltIn:
        return C_PRIMITIVE_TYPE;
    case KSyntaxHighlighting::Theme::Extension:
        return C_GLOBAL;
    case KSyntaxHighlighting::Theme::Preprocessor:
        return C_PREPROCESSOR;
    case KSyntaxHighlighting::Theme::Attribute:
        return C_LOCAL;
    case KSyntaxHighlighting::Theme::Char:
        return C_STRING;
    case KSyntaxHighlighting::Theme::SpecialChar:
        return C_STRING;
    case KSyntaxHighlighting::Theme::String:
        return definition.name() == "Diff" ? C_REMOVED_LINE : C_STRING;
    case KSyntaxHighlighting::Theme::VerbatimString:
        return C_STRING;
    case KSyntaxHighlighting::Theme::SpecialString:
        return C_STRING;
    case KSyntaxHighlighting::Theme::Import:
        return C_PREPROCESSOR;
    case KSyntaxHighlighting::Theme::DataType:
        return C_TYPE;
    case KSyntaxHighlighting::Theme::DecVal:
        return C_NUMBER;
    case KSyntaxHighlighting::Theme::BaseN:
        return C_NUMBER;
    case KSyntaxHighlighting::Theme::Float:
        return C_NUMBER;
    case KSyntaxHighlighting::Theme::Constant:
        return C_KEYWORD;
    case KSyntaxHighlighting::Theme::Comment:
        return C_COMMENT;
    case KSyntaxHighlighting::Theme::Documentation:
        return C_DOXYGEN_COMMENT;
    case KSyntaxHighlighting::Theme::Annotation:
        return C_DOXYGEN_TAG;
    case KSyntaxHighlighting::Theme::CommentVar:
        return C_DOXYGEN_TAG;
    case KSyntaxHighlighting::Theme::RegionMarker:
        return C_PREPROCESSOR;
    case KSyntaxHighlighting::Theme::Information:
        return C_WARNING;
    case KSyntaxHighlighting::Theme::Warning:
        return C_WARNING;
    case KSyntaxHighlighting::Theme::Alert:
        return C_ERROR;
    case KSyntaxHighlighting::Theme::Error:
        return C_ERROR;
    case KSyntaxHighlighting::Theme::Others:
        return C_TEXT;
    default:
        return C_TEXT;
    }
}

SyntaxHighlighter::SyntaxHighlighter()
{
    m_colorScheme = TextEditorSettings::fontSettings().colorScheme();
}

void SyntaxHighlighter::setDefinition(const KSyntaxHighlighting::Definition &def)
{
    AbstractHighlighter::setDefinition(def);
    m_definition = def;
}

void SyntaxHighlighter::applyFormat(int offset,
                                    int length,
                                    const KSyntaxHighlighting::Format &format)
{
    TextStyle ts = categoryForTextStyle(format.textStyle(), m_definition);
    TextEditor::Format fm = m_colorScheme.formatFor(ts);

    QTextCharFormat charFmt = m_defaultFmt;
    charFmt.setForeground(fm.foreground());
    charFmt.setFontWeight(fm.bold() ? QFont::Bold : QFont::Normal);
    charFmt.setFontItalic(fm.italic());

    m_recordedFormats.append({offset, length, charFmt});
}

void SyntaxHighlighter::processLine(const QString &line, MarkdownParserContext *ctx)
{
    m_recordedFormats.clear();

    // highlightLine updates m_state and calls applyFormat
    m_state = highlightLine(line, m_state);

    int currentPos = 0;
    for (const auto &rec : std::as_const(m_recordedFormats)) {
        // Insert text from end of last format to start of this one
        if (rec.offset > currentPos) {
            MarkdownOp op{MarkdownOp::InsertText, line.mid(currentPos, rec.offset - currentPos)};
            op.charFmt = m_defaultFmt;
            op.blockFmt = m_defaultBlockFmt;
            ctx->ops.push_back(op);
        }
        // Apply the new format
        MarkdownOp op{MarkdownOp::InsertText, line.mid(rec.offset, rec.length)};
        op.charFmt = rec.charFmt;
        op.blockFmt = m_defaultBlockFmt;
        ctx->ops.push_back(op);

        currentPos = rec.offset + rec.length;
    }

    // Insert remaining text
    if (currentPos < line.length()) {
        MarkdownOp op{MarkdownOp::InsertText, line.mid(currentPos)};
        op.charFmt = m_defaultFmt;
        op.blockFmt = m_defaultBlockFmt;
        ctx->ops.push_back(op);
    }
}

void SyntaxHighlighter::processChunk(const QString &chunk, MarkdownParserContext *ctx)
{
    QString combined = m_leftover + chunk;

    // Find the last newline to see if we have a complete line
    int lastNewline = combined.lastIndexOf('\n');
    if (lastNewline == -1) {
        m_leftover = combined;
        return;
    }

    // Split into lines, keeping the newline logic in mind
    QStringList lines = combined.split('\n');
    // The last element is potentially a partial line for the next chunk
    m_leftover = lines.takeLast();

    // The default format for this line (usually mono font from the code block)
    m_defaultFmt = !ctx->textCharFormatStack.isEmpty() ? ctx->textCharFormatStack.top()
                                                       : QTextCharFormat();
    m_defaultBlockFmt = !ctx->codeBlockFormatStack.isEmpty() ? ctx->codeBlockFormatStack.top()
                                                             : QTextBlockFormat();

    for (const QString &line : std::as_const(lines)) {
        processLine(line, ctx);

        // Push the newline character itself
        MarkdownOp newlineOp{MarkdownOp::InsertText, "\n"};
        newlineOp.charFmt = m_defaultFmt;
        newlineOp.blockFmt = m_defaultBlockFmt;
        ctx->ops.push_back(newlineOp);
    }
}

void SyntaxHighlighter::finish(MarkdownParserContext *ctx)
{
    if (!m_leftover.isEmpty()) {
        processLine(m_leftover, ctx);
        MarkdownOp newlineOp{MarkdownOp::InsertText, "\n"};
        newlineOp.charFmt = ctx->textCharFormatStack.isEmpty() ? QTextCharFormat()
                                                               : ctx->textCharFormatStack.top();
        ctx->ops.push_back(newlineOp);
        m_leftover.clear();
    }
}

} // namespace LlamaCpp
