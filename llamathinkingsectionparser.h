#pragma once

#include <QString>

namespace LlamaCpp {

class LlamaCppServerProps;

class ThinkingSectionParser
{
private:
    static QString m_startToken;
    static QString m_endToken;

public:
    static void setTokensFromServerProps(const LlamaCppServerProps &serverProps);

    static QPair<QString, QString> parseThinkingSection(const QString &text);
    static bool hasThinkingSection(const QString &text);
    static QString formatThinkingContent(const QString &thinkingContent);
};
} // namespace LlamaCpp
