#include <format.h>
#include <abstracthighlighter_p.h>
#include <definition.h>
#include <definition_p.h>
#include <repository.h>
#include <state.h>
#include <theme.h>

#include <texteditor/fontsettings.h>
#include <texteditor/texteditorsettings.h>
#include <utils/theme/theme.h>

#include "llamahtmlhighlighter.h"

using namespace TextEditor;

namespace LlamaCpp {

HtmlHighlighter::HtmlHighlighter()
{
    m_colorScheme = TextEditorSettings::fontSettings().colorScheme();
}

TextStyle categoryForTextStyle(int style, KSyntaxHighlighting::Definition definition)
{
    switch (style) {
    case KSyntaxHighlighting::Theme::Normal:
        return C_TEXT;
    case KSyntaxHighlighting::Theme::Keyword:
        return C_KEYWORD;
    case KSyntaxHighlighting::Theme::Function:
        return C_FUNCTION;
    case KSyntaxHighlighting::Theme::Variable:
        return definition.name() == "Diff" ?  C_ADDED_LINE : C_LOCAL;
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
    }
    return C_TEXT;
}

void HtmlHighlighter::setDefinition(const KSyntaxHighlighting::Definition &definition)
{
    AbstractHighlighter::setDefinition(definition);

    m_definition = definition;

    auto definitions = m_definition.includedDefinitions();
    definitions.append(m_definition);

    int maxId = 0;
    for (const auto &definition : std::as_const(definitions)) {
        for (const auto &format :
             std::as_const(KSyntaxHighlighting::DefinitionData::get(definition)->formats)) {
            maxId = qMax(maxId, format.id());
        }
    }
    m_htmlStyles.clear();
    m_htmlStyles.resize(maxId + 1);

    for (const auto &definition : std::as_const(definitions)) {
        for (const auto &format :
             std::as_const(KSyntaxHighlighting::DefinitionData::get(definition)->formats)) {
            auto &buffer = m_htmlStyles[format.id()];

            TextStyle ts = categoryForTextStyle(format.textStyle(), m_definition);
            Format fm = m_colorScheme.formatFor(ts);

            buffer += "color:" + fm.foreground().name() + ';';
            if (fm.bold())
                buffer += "font-weight:bold;";

            if (fm.italic())
                buffer += "font-style:italic;";

            if (!buffer.isEmpty()) {
                buffer.insert(0, "<span style=\"");
                // replace last ';'
                buffer.back() = u'"';
                buffer += u'>';
            }
        }
    }
}

QString HtmlHighlighter::hightlightCodeLine(const QString &line)
{
    m_outputLine.clear();

    m_currentLine += line;
    auto newLineIndex = m_currentLine.indexOf("\n");
    if (newLineIndex == -1)
        return m_outputLine;

    QString fullLine = m_currentLine.left(newLineIndex);
    QString rest = m_currentLine.mid(newLineIndex + 1);

    m_currentLine = fullLine;
    m_state = highlightLine(m_currentLine, m_state);

    m_currentLine = rest;
    return m_outputLine + "<br>";
}

void HtmlHighlighter::applyFormat(int offset, int length, const KSyntaxHighlighting::Format &format)
{
    if (length == 0)
        return;

    auto const &htmlStyle = m_htmlStyles[format.id()];
    if (!htmlStyle.isEmpty())
        m_outputLine.append(htmlStyle);

    for (QChar ch : QStringView(m_currentLine).sliced(offset, length)) {
        if (ch == u'<')
            m_outputLine.append(QStringLiteral("&lt;"));
        else if (ch == u'&')
            m_outputLine.append(QStringLiteral("&amp;"));
        else
            m_outputLine.append(ch);
    }

    if (!htmlStyle.isEmpty())
        m_outputLine.append(QStringLiteral("</span>"));
}
} // namespace LlamaCpp
