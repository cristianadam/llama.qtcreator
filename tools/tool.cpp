#include "tool.h"

#include "llamatr.h"

namespace LlamaCpp {

QString Tool::detailsMarkdown(const QJsonObject &arguments, const QString &result, bool ok) const
{
    Q_UNUSED(arguments);
    // Generic fallback – just show the raw result.
    if (result.isEmpty())
        return {};

    QString md;
    if (!ok)
        md = QStringLiteral("**%1**\n\n").arg(Tr::tr("Error"));
    return md + QStringLiteral("```\n%1\n```").arg(result);
}

} // namespace LlamaCpp
