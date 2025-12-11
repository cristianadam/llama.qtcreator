#include "llamathinkingsectionparser.h"
#include "llamatr.h"
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

QString ThinkingSectionParser::replaceThinkingSections(const QString &src, bool completed)
{
    QString out;
    int pos = 0;

    while (true) {
        int startIdx = src.indexOf(m_startToken, pos);
        if (startIdx == -1) {
            out.append(src.mid(pos));
            break;
        }

        // Find the matching end token – it may be missing while the model
        // is still streaming.
        int endIdx = src.indexOf(m_endToken, startIdx + m_startToken.length());

        // copy everything before the thinking block
        out.append(src.mid(pos, startIdx - pos));

        QString thinking;
        bool doneThinking = false;
        if (endIdx != -1) {
            doneThinking = true;
            thinking = src.mid(startIdx + m_startToken.length(),
                               endIdx - startIdx - m_startToken.length());
        } else {
            // open‑ended – everything after the start token belongs to the
            // thinking part (the answer will be appended on the next render).
            thinking = src.mid(startIdx + m_startToken.length());
        }

        QString formattedThinking = formatThinkingContent(thinking);

        QString statusIcon;
        if (doneThinking) {
            statusIcon = "<span style=\"font-family: heroicons_outline;\">Q</span>";
        } else {
            // spinner while the answer is still being generated
            statusIcon = "<img src=\"spinner://tool\" style=\"vertical-align: middle;\"/>";
        }

        const QString detailsBlock
            = QStringLiteral("<details><summary>%1&nbsp;%2</summary>\n\n%3\n</details>\n")
                  .arg(statusIcon)
                  .arg(doneThinking ? Tr::tr("Thought Process") : Tr::tr("Thinking"))
                  .arg(formattedThinking);

        out.append(detailsBlock);

        if (endIdx != -1) {
            pos = endIdx + m_endToken.length();

            // We have a details section with new content afterwards, insert a sepparation
            if (pos != src.length())
                out.append("\n\n<br/><br/>");
        } else {
            // nothing after an open‑ended block – we are done for now
            pos = src.length();
            break;
        }
    }

    return out;
}

} // namespace LlamaCpp
