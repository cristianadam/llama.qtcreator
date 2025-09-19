#include "llamathinkingsectionparser.h"
#include "llamatypes.h"

namespace LlamaCpp {
QString ThinkingSectionParser::m_startToken;
QString ThinkingSectionParser::m_endToken;

void ThinkingSectionParser::setTokensFromServerProps(const LlamaCppServerProps &serverProps)
{
    if (serverProps.model_path.contains("gpt-oss", Qt::CaseInsensitive)) {
        m_startToken = "<|channel|>analysis<|message|>";
        m_endToken = "<|end|>";
    } else {
        // Tested with DeepSeek.
        m_startToken = "<think>";
        m_endToken = "</think>";
    }
}

QPair<QString, QString> ThinkingSectionParser::parseThinkingSection(const QString &text)
{
    int startIdx = text.indexOf(m_startToken);
    int endIdx = text.indexOf(m_endToken);

    if (startIdx != -1 && endIdx != -1 && startIdx + m_startToken.length() < endIdx) {
        QString thinkingContent = text.mid(startIdx + m_startToken.length(),
                                           endIdx - startIdx - m_startToken.length());
        QString restContent = text.mid(endIdx + m_endToken.length());
        return {thinkingContent, restContent};
    } else if (startIdx != -1 && endIdx == -1) {
        QString thinkingContent = text.mid(startIdx + m_startToken.length());
        return {thinkingContent, {}};
    }

    return {{}, text};
}

bool ThinkingSectionParser::hasThinkingSection(const QString &text)
{
    int startIdx = text.indexOf(m_startToken);
    int endIdx = text.indexOf(m_endToken);
    return (startIdx != -1 && endIdx != -1 && startIdx < endIdx)
           || (startIdx != -1 && endIdx == -1);
}

QString ThinkingSectionParser::formatThinkingContent(const QString &thinkingContent)
{
    QString formatted = thinkingContent;
    formatted.replace("\n", "\n>");
    return ">" + formatted;
}
} // namespace LlamaCpp
