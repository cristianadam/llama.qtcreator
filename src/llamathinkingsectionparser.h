#pragma once

#include <QString>

namespace LlamaCpp {

struct LlamaCppServerProps;

class ThinkingSectionParser
{
private:
    static QString m_startToken;
    static QString m_endToken;

public:
    static QPair<QString, QString> parseThinkingSection(const QString &text);
    static bool hasThinkingSection(const QString &text);
    static QString replaceThinkingSections(const QString &src, bool completed);
    static QString formatThinkingHtml(const QString &content, bool isFinished);
    static QString startToken();
    static QString endToken();
};
} // namespace LlamaCpp
