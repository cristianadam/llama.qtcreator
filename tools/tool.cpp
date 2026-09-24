#include "tool.h"

#include "llamatr.h"

#include <algorithm>

namespace LlamaCpp {

QString Tool::detailsMarkdown(const QJsonObject &arguments, const QString &result, bool ok) const
{
    Q_UNUSED(arguments);
    Q_UNUSED(ok);
    // Generic fallback – just show the raw result.  Failures need no error
    // header: the ✗ icon in the summary already marks the call as failed.
    if (result.isEmpty())
        return {};
    return codeFence(result);
}

QString Tool::summaryPreview(const QJsonObject &arguments, const QString &result, bool ok) const
{
    // On failure the ✗ icon plus the one‑line summary are enough in the
    // collapsed view; the error text is only shown in the expanded details.
    if (!ok)
        return {};
    return truncatedPreview(detailsMarkdown(arguments, result, ok), 3);
}

QString codeFence(const QString &content, const QString &info)
{
    // Longest run of consecutive backticks in the content.
    int maxRun = 0;
    int run = 0;
    for (const QChar c : content) {
        run = (c == QLatin1Char('`')) ? run + 1 : 0;
        if (run > maxRun)
            maxRun = run;
    }
    // File contents and command output usually end with a newline, and the
    // fence template below adds its own before the closing fence; without
    // this the rendered block shows a spurious empty last line.
    QString body = content;
    if (body.endsWith(QLatin1Char('\n')))
        body.chop(1);

    const int length = qMax(3, maxRun + 1);
    const QString fence(QString(length, QLatin1Char('`')));
    return QStringLiteral("%1%2\n%3\n%1").arg(fence, info, body);
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
    int fenceLength = 3;
    for (const QString &line : std::as_const(preview).split(QLatin1Char('\n'))) {
        const QString trimmed = line.trimmed();
        int length = 0;
        while (length < trimmed.size() && trimmed.at(length) == QLatin1Char('`'))
            ++length;
        if (length >= 3) {
            ++fences;
            fenceLength = qMax(fenceLength, length);
        }
    }
    if (fences % 2 == 1)
        preview += QLatin1Char('\n') + QString(fenceLength, QLatin1Char('`'));
    return preview;
}

} // namespace LlamaCpp
