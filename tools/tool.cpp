#include "tool.h"

namespace LlamaCpp {

QString Tool::detailsMarkdown(const QJsonObject &arguments, const QString &result) const
{
    Q_UNUSED(arguments);
    // Generic fallback – just show the raw result.
    if (result.isEmpty())
        return {};

    return QString("**Result:**\n```\n%1\n```").arg(result);
}

} // namespace LlamaCpp
