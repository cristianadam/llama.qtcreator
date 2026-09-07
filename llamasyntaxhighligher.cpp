#include "llamasyntaxhighlighter.h"

#include <abstracthighlighter_p.h>
#include <definition.h>
#include <definition_p.h>
#include <format.h>

#include <coreplugin/icore.h>
#include <repository.h>
#include <state.h>
#include <theme.h>

#include <texteditor/fontsettings.h>
#include <utils/filepath.h>
#include <utils/theme/theme.h>

namespace LlamaCpp {

using namespace Core;
using namespace TextEditor;
using namespace Internal;
using namespace Utils;

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

static KSyntaxHighlighting::Repository *highlightRepository()
{
    static KSyntaxHighlighting::Repository *repository = nullptr;
    if (!repository) {
        repository = new KSyntaxHighlighting::Repository();
        const FilePath dir = ICore::resourcePath("generic-highlighter/syntax");
        if (dir.exists())
            repository->addCustomSearchPath(dir.parentDir().path());
        const FilePath userDir = ICore::userResourcePath("generic-highlighter");
        if (userDir.exists())
            repository->addCustomSearchPath(userDir.path());
    }
    return repository;
}

KSyntaxHighlighting::Definition syntaxDefinitionForName(const QString &name)
{
    return highlightRepository()->definitionForName(name);
}

SyntaxHighlighter::SyntaxHighlighter()
{
    m_colorScheme = globalFontSettings().data().colorScheme();
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

void SyntaxHighlighter::processLine(const QString &line,
                                    const QTextCharFormat &defaultFmt,
                                    QVector<HighlightFragment> &fragments)
{
    m_recordedFormats.clear();
    m_defaultFmt = defaultFmt;

    // highlightLine updates m_state and calls applyFormat
    m_state = highlightLine(line, m_state);

    int currentPos = 0;
    for (const auto &rec : std::as_const(m_recordedFormats)) {
        // Insert text from end of last format to start of this one
        if (rec.offset > currentPos)
            fragments.append({line.mid(currentPos, rec.offset - currentPos), defaultFmt});

        // Apply the new format
        fragments.append({line.mid(rec.offset, rec.length), rec.charFmt});

        currentPos = rec.offset + rec.length;
    }

    // Append remaining text
    if (currentPos < line.length())
        fragments.append({line.mid(currentPos), defaultFmt});
}

void SyntaxHighlighter::highlight(const QString &content,
                                  const QTextCharFormat &defaultFmt,
                                  QVector<HighlightFragment> &fragments)
{
    if (content.isEmpty())
        return;

    QStringList lines = content.split('\n');
    if (content.endsWith('\n'))
        lines.removeLast();

    for (int i = 0; i < lines.size(); ++i) {
        processLine(lines[i], defaultFmt, fragments);
        if (i != lines.size() - 1)
            fragments.append({QStringLiteral("\n"), defaultFmt});
    }
}

} // namespace LlamaCpp
