#include "llamasyntaxhighlighter.h"

#include <abstracthighlighter_p.h>
#include <definition.h>
#include <definition_p.h>
#include <definitiondownloader.h>
#include <format.h>

#include <coreplugin/icore.h>
#include <coreplugin/messagemanager.h>
#include <repository.h>
#include <state.h>
#include <theme.h>

#include <QGuiApplication>

#include <texteditor/fontsettings.h>
#include <utils/filepath.h>
#include <utils/theme/theme.h>

#include "llamatr.h"

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

namespace {

// A few syntax definitions that are part of the full KSyntaxHighlighting set
// but not of the small set Qt Creator ships (it only bundles a license‑clear
// subset).  If these are missing, the full set has not been downloaded yet.
// The first two are named in the welcome‑screen warning.
struct CommonSyntaxDefinition
{
    const char *name;        // KSyntaxHighlighting name (case‑insensitive)
    const char *displayName; // human‑readable name for the warning
};

constexpr CommonSyntaxDefinition kCommonSyntaxDefinitions[] = {
    {"cpp", "C++"},
    {"python", "Python"},
    {"javascript", "JavaScript"},
    {"c", "C"},
};

} // namespace

QStringList missingCommonSyntaxDefinitions()
{
    const auto &repository = *highlightRepository();
    QStringList missing;
    for (const auto &definition : kCommonSyntaxDefinitions) {
        if (!repository.definitionForName(QLatin1String(definition.name)).isValid())
            missing.append(QLatin1String(definition.displayName));
    }
    return missing;
}

void downloadSyntaxDefinitions(std::function<void(const QString &)> onFinished)
{
    using namespace KSyntaxHighlighting;

    // Parented to the (long‑lived) repository so it survives until done();
    // deleteLater() on done() keeps it from leaking if the user never sees
    // the completion.
    auto *downloader = new DefinitionDownloader(highlightRepository());
    QObject::connect(downloader,
                     &DefinitionDownloader::informationMessage,
                     [](const QString &message) {
                         MessageManager::writeSilently(
                             Tr::tr("Syntax definitions: %1").arg(message));
                     });
    QObject::connect(downloader, &DefinitionDownloader::done, [downloader, onFinished]() {
        const QString message = Tr::tr("Syntax definitions: done");
        MessageManager::writeFlashing(message);
        if (onFinished)
            onFinished(message);
        downloader->deleteLater();
    });
    MessageManager::writeDisrupting(Tr::tr("Syntax definitions: starting download"));
    downloader->start();
}

SyntaxHighlighter::SyntaxHighlighter()
{
    // The color scheme is fetched lazily in setDefinition(): constructing
    // the FontSettings singleton builds Utils aspects that need a
    // QGuiApplication (a QPixmap), and the unit tests run without one, in
    // which case the uncolored default format is fine.
}

void SyntaxHighlighter::setDefinition(const KSyntaxHighlighting::Definition &def)
{
    AbstractHighlighter::setDefinition(def);
    m_definition = def;
    // Highlighting is about to start: fetch the theme's color scheme now,
    // once, instead of on every applyFormat() call. Only when running inside
    // Qt Creator (ICore is initialized): constructing the FontSettings
    // singleton builds Utils aspects that require a fully-initialized
    // QGuiApplication (a QPixmap), which the unit tests don't provide, in
    // which case the uncolored default format is fine.
    if (def.isValid() && m_colorScheme.isEmpty()
        && QCoreApplication::instance()
        && ICore::instance())
        m_colorScheme = globalFontSettings().data().colorScheme();
}

void SyntaxHighlighter::applyFormat(int offset,
                                    int length,
                                    const KSyntaxHighlighting::Format &format)
{
    // Without a valid definition highlightLine() still reports a Normal
    // format for every line; passing it through the colour scheme would tint
    // the whole block with the editor scheme's text colour (olive on the
    // "Dark (2024)" scheme, black-on-dark in the chat view on a scheme whose
    // text colour is dark). Use the caller's (theme-aware) default format
    // instead, and keep emitting the runs: the HTML exporter relies on the
    // highlighter to produce the text.
    if (!m_definition.isValid()) {
        m_recordedFormats.append({offset, length, m_defaultFmt});
        return;
    }

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

void SyntaxHighlighter::resetState()
{
    m_state = KSyntaxHighlighting::State();
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
