#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QVector>

namespace LlamaCpp::Tools {

/*! A user-configured MCP server, managed on the "Tools" settings page.
    Persisted as a JSON array in the McpServers setting, one object per
    server: {"name": ..., "url": ..., "headers": ["Name: value", ...]}. */
struct McpServerConfig
{
    QString name; //!< display name (also the tool group name in the chat)
    QUrl url; //!< streamable-HTTP endpoint
    QStringList headers; //!< raw "Name: value" request headers

    bool isValid() const
    {
        return !name.trimmed().isEmpty()
            && url.isValid()
            && (url.scheme() == QLatin1String("http") || url.scheme() == QLatin1String("https"));
    }

    /*! Parses the stored JSON. Empty or malformed input yields an empty
        list; individual entries that are not valid (empty name, non-http(s)
        URL) are skipped, as are entries whose name duplicates an earlier
        one (the first entry wins – the settings JSON is hand-editable and
        duplicate names would collapse into a single bridge client). */
    static QVector<McpServerConfig> fromJson(const QString &json)
    {
        QVector<McpServerConfig> servers;
        if (json.trimmed().isEmpty())
            return servers;

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError || !doc.isArray())
            return servers;

        QSet<QString> seenNames;
        for (const QJsonValue &v : doc.array()) {
            const QJsonObject o = v.toObject();
            McpServerConfig config;
            config.name = o.value(QStringLiteral("name")).toString().trimmed();
            config.url = QUrl(o.value(QStringLiteral("url")).toString().trimmed());
            for (const QJsonValue &h : o.value(QStringLiteral("headers")).toArray()) {
                const QString header = h.toString().trimmed();
                if (!header.isEmpty())
                    config.headers << header;
            }
            if (config.isValid() && !seenNames.contains(config.name)) {
                seenNames.insert(config.name);
                servers.append(config);
            }
        }
        return servers;
    }

    static QString toJson(const QVector<McpServerConfig> &servers)
    {
        QJsonArray array;
        for (const McpServerConfig &server : servers) {
            QJsonObject o;
            o[QStringLiteral("name")] = server.name;
            o[QStringLiteral("url")] = server.url.toString();
            QJsonArray headers;
            for (const QString &header : server.headers)
                headers.append(header);
            o[QStringLiteral("headers")] = headers;
            array.append(o);
        }
        return QString::fromUtf8(QJsonDocument(array).toJson(QJsonDocument::Compact));
    }
};

} // namespace LlamaCpp::Tools
