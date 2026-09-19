#pragma once

#include <QByteArray>
#include <QCache>
#include <QList>
#include <QString>
#include <QStringList>

namespace LlamaCpp {
namespace Fim {

// Separator placed between the middle and suffix parts of the context when hashing.
// Any distinctive character works, but it must be identical everywhere hashes are computed.
extern const QString kContextSeparator;

// Compute how similar two chunks of text are: 0 - no similarity, 1 - high
// similarity. Token-based Sørensen-Dice measure (tokens are runs of word
// characters), mirroring llama.vim s:chunk_sim.
double chunkSim(const QStringList &c0, const QStringList &c1);

// Compute the SHA-256 hashes of a local context (prefix/middle/suffix), including
// variants with the first few lines of the prefix trimmed. Those variants match
// completions for which the first few lines are missing, which happens when the
// cursor has moved up a bit from where the original generation was done.
QByteArrayList contextHashes(const QString &prefix, const QString &middle, const QString &suffix);

// Split a raw /infill response into individual completion responses.
// n_cmpl == 1 returns a single object {"content": ...}, n_cmpl > 1 an array
// [{"content": ...}, ...]. Falls back to a list containing the raw response
// when nothing could be parsed.
QList<QByteArray> splitResponse(const QByteArray &raw);

// A cache of completion responses, used to avoid re-computing the same completions
// and to allow cycling through multiple completions generated for the same context.
//
// Each key maps to a ring buffer of up to nCmpl individual responses (responses with
// the same content are not stored twice); the underlying QCache evicts the least
// recently used keys once setMaxCost() is exceeded.
class ResultCache
{
public:
    void setMaxCost(qint64 cost) { m_cache.setMaxCost(cost); }
    qint64 size() const { return m_cache.size(); }
    void clear() { m_cache.clear(); }

    // Return the cached completions for the key (may be null/empty), updating the
    // LRU order.
    QList<QByteArray> *operator[](const QByteArray &key) { return m_cache[key]; }

    // Read-only lookup that does not update the LRU order.
    const QList<QByteArray> *find(const QByteArray &key) const { return m_cache[key]; }

    // Return the cached completions for the key, updating the LRU order (like
    // llama.vim s:cache_get), or nullptr without creating an entry.
    QList<QByteArray> *lookup(const QByteArray &key);

    // True if at least one of the keys maps to a non-empty list of completions.
    bool containsAny(const QByteArrayList &keys);

    // Split the raw response and insert the individual completions into the ring
    // buffers of all the given keys (see contextHashes()).
    void insert(const QByteArrayList &keys, const QByteArray &response, int nCmpl);

private:
    QCache<QByteArray, QList<QByteArray>> m_cache;
};

} // namespace Fim
} // namespace LlamaCpp
