#include "llamathinkingsectionparser.h"
#include "llamatr.h"
#include "llamatypes.h"

namespace LlamaCpp {
QString ThinkingSectionParser::m_startToken = "<think>";
QString ThinkingSectionParser::m_endToken = "</think>";

QString ThinkingSectionParser::startToken()
{
    return m_startToken;
}

QString ThinkingSectionParser::endToken()
{
    return m_endToken;
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

QString ThinkingSectionParser::formatThinkingHtml(const QString &content, bool isFinished)
{
    QString statusIcon;
    QString label;

    if (isFinished) {
        statusIcon = "<span style=\"font-family: heroicons_outline;\">Q</span>";
        label = Tr::tr("Thought Process");
    } else {
        statusIcon = "<img src=\"spinner://tool\" style=\"vertical-align: middle;\"/>";
        label = Tr::tr("Thinking");
    }

    return QStringLiteral("<details><summary>%1&nbsp;%2</summary>\n\n%3\n</details>\n")
        .arg(statusIcon, label, content);
}

QString ThinkingSectionParser::replaceThinkingSections(const QString &src, bool completed)
{
    if (src.isEmpty())
        return {};

    QString out;
    int currentPos = 0;

    while (currentPos < src.length()) {
        int startIdx = src.indexOf(m_startToken, currentPos);

        // If no more thinking sections, append the rest of the text and exit
        if (startIdx == -1) {
            out.append(src.mid(currentPos));
            break;
        }

        // Append everything from the current position up to the start token
        out.append(src.mid(currentPos, startIdx - currentPos));

        // Look for the end token starting from the end of the start token
        int contentStart = startIdx + m_startToken.length();
        int endIdx = src.indexOf(m_endToken, contentStart);

        if (endIdx != -1) {
            // Fully formed block <think>...</think>
            QString thinkingContent = src.mid(contentStart, endIdx - contentStart);
            out.append(formatThinkingHtml(thinkingContent, true));

            // Move cursor past the end token
            currentPos = endIdx + m_endToken.length();

            // Add a separator if there is more text coming
            if (currentPos < src.length()) {
                out.append("\n\n");
            }
        } else {
            // Open-ended block <think>... (streaming)
            // Note: We treat the rest of the string as the thinking content
            QString thinkingContent = src.mid(contentStart);
            out.append(formatThinkingHtml(thinkingContent, false));

            // Since this block consumes the rest of the string, we are done
            currentPos = src.length();
        }
    }

    return out;
}

} // namespace LlamaCpp
