#include "tool.h"

#include "llamatr.h"

#include <algorithm>

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

QString Tool::summaryPreview(const QJsonObject &arguments, const QString &result, bool ok) const
{
    return truncatedPreview(detailsMarkdown(arguments, result, ok), 3);
}

QString truncatedPreview(const QString &text, int maxLines)
{
    if (text.isEmpty())
        return {};

    constexpr int kMaxChars = 300;

    const QStringList lines = text.split(QLatin1Char('\n'));
    QString preview = text;
    if (lines.size() > maxLines)
        preview = lines.mid(0, maxLines).join(QLatin1Char('\n'));
    if (preview.size() > kMaxChars) {
        preview = preview.left(kMaxChars - 1);
        // A character cut may leave a partial code fence at the end.
        while (preview.endsWith(QLatin1Char('`')))
            preview.chop(1);
    }

    // Close a code fence left open by the cut so the markdown stays
    // well‑formed.  No trailing ellipsis: the details header already gets
    // the expand (down) icon appended, which signals more content.
    int fences = 0;
    for (const QString &line : std::as_const(preview).split(QLatin1Char('\n')))
        if (line.trimmed().startsWith(QStringLiteral("```")))
            ++fences;
    if (fences % 2 == 1)
        preview += QStringLiteral("\n```");
    return preview;
}

} // namespace LlamaCpp
