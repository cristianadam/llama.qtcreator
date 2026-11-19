#include "getdatetime_tool.h"
#include "factory.h"
#include "llamatr.h"

#include <QDateTime>
#include <QTimeZone>

namespace LlamaCpp::Tools {

const bool registered = [] {
    ToolFactory::instance().registerCreator(GetDatetimeTool{}.name(),
                                            []() { return std::make_unique<GetDatetimeTool>(); });
    return true;
}();

QString GetDatetimeTool::name() const
{
    return QStringLiteral("get_datetime");
}

QString GetDatetimeTool::toolDefinition() const
{
    return R"raw(
    {
        "type": "function",
        "function": {
            "name": "get_datetime",
            "description": "Returns the current local date and time as ISO 8601 with the UTC offset, plus the IANA time zone name. Use it whenever the task depends on the current date or time: relative dates (\"today\", \"tomorrow\", \"next week\"), timestamps, file dates.",
            "parameters": {
                "type": "object",
                "properties": {},
                "required": [],
                "strict": true
            }
        }
    })raw";
}

QString GetDatetimeTool::oneLineSummary(const QJsonObject &args) const
{
    Q_UNUSED(args);
    return Tr::tr("get current time");
}

void GetDatetimeTool::run(const QJsonObject &args,
                          std::function<void(const QString &, bool)> done) const
{
    Q_UNUSED(args);
    // Local ISO 8601 with the explicit UTC offset plus the IANA zone name:
    // the model sees the user's wall clock, not UTC.  The offset is computed
    // from the system time zone instead of using the 'zzz' format specifier,
    // which does not render the offset in some Qt versions.
    const QDateTime now = QDateTime::currentDateTime();
    const int offsetSeconds = QTimeZone::systemTimeZone().offsetFromUtc(now);
    const QString offset = QString::asprintf("%c%02d:%02d",
                                             offsetSeconds < 0 ? '-' : '+',
                                             qAbs(offsetSeconds) / 3600,
                                             (qAbs(offsetSeconds) % 3600) / 60);
    const QString iso = now.toString(QStringLiteral("yyyy-MM-dd'T'HH:mm:ss")) + offset;
    const QString zone = QString::fromUtf8(QTimeZone::systemTimeZoneId());
    done(QStringLiteral("%1 (%2)").arg(iso, zone), true);
}

} // namespace LlamaCpp::Tools
