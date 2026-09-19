#include "fimcache.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

#include <QSet>
#include <tuple>

namespace LlamaCpp {
namespace Fim {

const QString kContextSeparator = QStringLiteral("\u00CE");

double chunkSim(const QStringList &c0, const QStringList &c1)
{
    // Token-based Sørensen-Dice similarity, mirroring llama.vim s:chunk_sim:
    // the lines are joined and split into runs of word characters, and
    // 'common' counts the tokens of the second chunk (with multiplicity) that
    // occur in the first chunk. Two empty chunks are considered identical.
    static const QRegularExpression tokenRe(QStringLiteral("[A-Za-z0-9_]+"));

    const auto tokenize = [](const QStringList &lines) {
        QStringList tokens;
        const QString text = lines.join('\n');
        int from = 0;
        while (true) {
            const QRegularExpressionMatch match = tokenRe.match(text, from);
            if (!match.hasMatch())
                break;
            tokens.append(match.captured(0));
            from = match.capturedEnd(0);
        }
        return tokens;
    };

    const QStringList tokens0 = tokenize(c0);
    const QStringList tokens1 = tokenize(c1);

    if (tokens0.isEmpty() && tokens1.isEmpty())
        return 1.0;

    const QSet<QString> set0(std::as_const(tokens0).cbegin(),
                             std::as_const(tokens0).cend());

    int common = 0;
    for (const QString &token : std::as_const(tokens1)) {
        if (set0.contains(token))
            common++;
    }

    return 2.0 * common / (tokens0.size() + tokens1.size());
}

QByteArrayList contextHashes(const QString &prefix, const QString &middle, const QString &suffix)
{
    auto hashOf = [&middle, &suffix](const QString &p) {
        return QCryptographicHash::hash((p + middle + kContextSeparator + suffix).toUtf8(),
                                        QCryptographicHash::Sha256)
            .toHex();
    };

    QByteArrayList hashes;
    hashes.append(hashOf(prefix));

    // Compute additional hashes for which the first few lines of the prefix are missing.
    static QRegularExpression re("^[^\n]*\n");
    QString prefixTrim = prefix;
    for (int i = 0; i < 3; ++i) {
        prefixTrim = prefixTrim.replace(re, "");
        if (prefixTrim.isEmpty())
            break;
        hashes.append(hashOf(prefixTrim));
    }

    return hashes;
}

QList<QByteArray> splitResponse(const QByteArray &raw)
{
    QList<QByteArray> responses;

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(raw, &error);
    if (error.error == QJsonParseError::NoError) {
        if (doc.isObject()) {
            responses.append(raw);
        } else if (doc.isArray()) {
            const QJsonArray array = doc.array();
            for (const QJsonValue &value : std::as_const(array)) {
                if (value.isObject())
                    responses.append(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact));
            }
        }
    }

    // Fallback: keep the raw response if nothing could be parsed.
    if (responses.isEmpty())
        responses.append(raw);

    return responses;
}

QList<QByteArray> *ResultCache::lookup(const QByteArray &key)
{
    // unlike operator[], do not insert a null entry for unknown keys
    if (!m_cache.contains(key))
        return nullptr;
    return m_cache[key];
}

bool ResultCache::containsAny(const QByteArrayList &keys)
{
    for (const QByteArray &key : keys) {
        if (const QList<QByteArray> *list = find(key)) {
            if (!list->isEmpty())
                return true;
        }
    }
    return false;
}

void ResultCache::insert(const QByteArrayList &keys, const QByteArray &response, int nCmpl)
{
    const QList<QByteArray> responses = splitResponse(response);
    const int maxEntries = qMax(1, nCmpl);

    for (const QByteArray &key : keys) {
        QList<QByteArray> *list = m_cache[key];
        if (!list) {
            auto *newList = new QList<QByteArray>();
            m_cache.insert(key, newList);
            list = newList;
        }

        for (const QByteArray &resp : std::as_const(responses)) {
            QJsonParseError respError;
            QJsonDocument respDoc = QJsonDocument::fromJson(resp, &respError);
            const QString content
                = (respError.error == QJsonParseError::NoError && respDoc.isObject())
                      ? respDoc.object().value("content").toString()
                      : QString();

            // Skip if a completion with the same content already exists for this key.
            bool exists = false;
            for (const QByteArray &existing : std::as_const(*list)) {
                if (existing == resp) {
                    exists = true;
                    break;
                }
                if (!content.isEmpty()) {
                    QJsonDocument existingDoc = QJsonDocument::fromJson(existing);
                    if (existingDoc.isObject()
                        && existingDoc.object().value("content").toString() == content) {
                        exists = true;
                        break;
                    }
                }
            }
            if (exists)
                continue;

            // Add to the ring buffer, evict the oldest if full.
            if (list->size() >= maxEntries)
                list->removeFirst();
            list->append(resp);
        }
    }
}

} // namespace Fim
} // namespace LlamaCpp
