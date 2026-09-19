#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtTest/QtTest>

#include "fimcache.h"

using namespace LlamaCpp;

namespace {

QString contentOf(const QByteArray &resp)
{
    const QJsonDocument doc = QJsonDocument::fromJson(resp);
    return doc.object().value("content").toString();
}

QStringList completionList(const LlamaCpp::Fim::ResultCache &cache, const QByteArray &key)
{
    QStringList contents;
    if (const auto *list = cache.find(key)) {
        for (const QByteArray &resp : std::as_const(*list))
            contents.append(contentOf(resp));
    }
    return contents;
}

} // namespace

class FimCacheTest : public QObject
{
    Q_OBJECT

private slots:
    // chunkSim
    void chunkSim_emptyChunks();
    void chunkSim_identicalChunks();
    void chunkSim_disjointChunks();
    void chunkSim_partialOverlap();
    void chunkSim_tokenBased();

    // contextHashes
    void contextHashes_numberOfHashes();
    void contextHashes_values();
    void contextHashes_trimmedPrefixMatchesFullContext();
    void contextHashes_separatorByte();

    // splitResponse
    void splitResponse_singleObject();
    void splitResponse_array();
    void splitResponse_invalidJson();
    void splitResponse_arrayWithoutObjects();

    // ResultCache
    void resultCache_insertAndLookup();
    void resultCache_storedUnderAllKeys();
    void resultCache_ringEviction();
    void resultCache_contentDedup();
    void resultCache_lruMaxCost();
    void resultCache_lookup();
    void resultCache_containsAny();
    void resultCache_clear();
};

// ---------------------------------------------------------------------------
// chunkSim
// ---------------------------------------------------------------------------

void FimCacheTest::chunkSim_emptyChunks()
{
    // Like llama.vim, two empty chunks are considered identical.
    QCOMPARE(Fim::chunkSim({}, {}), 1.0);
    QCOMPARE(Fim::chunkSim({}, {"   ", "\t"}), 1.0); // whitespace-only has no tokens
}

void FimCacheTest::chunkSim_identicalChunks()
{
    const QStringList chunk{"int main()", "{", "    return 0;", "}"};
    QCOMPARE(Fim::chunkSim(chunk, chunk), 1.0);
}

void FimCacheTest::chunkSim_disjointChunks()
{
    QCOMPARE(Fim::chunkSim({"a", "b"}, {"c", "d"}), 0.0);
}

void FimCacheTest::chunkSim_partialOverlap()
{
    // one common token out of four total -> 2 * 1 / 4
    QCOMPARE(Fim::chunkSim({"a", "b"}, {"a", "c"}), 0.5);
}

// The similarity is computed on tokens (runs of word characters), not on
// whole lines: chunks sharing many tokens on different lines are similar.
void FimCacheTest::chunkSim_tokenBased()
{
    // tokens: int, x, 1 vs int, y, 2 -> one common token
    const double res = Fim::chunkSim({"int x = 1;"}, {"int y = 2;"});
    QCOMPARE(res, 2.0 / 6.0);

    // tokens are split on non-word characters, and the order of lines and
    // tokens does not matter: all three tokens match even though no line does
    QCOMPARE(Fim::chunkSim({"a b", "c"}, {"b a", "c"}), 1.0);
    QCOMPARE(Fim::chunkSim({"a-b_c"}, {"a b c"}), 0.4); // b_c is a single token

    // tokens of the second chunk are counted with multiplicity
    // (mirroring llama.vim: the result may exceed 1.0 for very unequal chunks)
    QCOMPARE(Fim::chunkSim({"a"}, {"a a a"}), 2.0 * 3 / 4.0);
}

// ---------------------------------------------------------------------------
// contextHashes
// ---------------------------------------------------------------------------

void FimCacheTest::contextHashes_numberOfHashes()
{
    const QString middle = "int x";
    const QString suffix = ";";

    // four or more prefix lines -> full hash + 3 trimmed variants
    QCOMPARE(Fim::contextHashes("a\nb\nc\nd\n", middle, suffix).size(), 4);
    // two prefix lines -> full hash + 1 trimmed variant
    QCOMPARE(Fim::contextHashes("a\nb\n", middle, suffix).size(), 2);
    // one prefix line -> full hash only
    QCOMPARE(Fim::contextHashes("a\n", middle, suffix).size(), 1);
    // empty prefix -> full hash only
    QCOMPARE(Fim::contextHashes("", middle, suffix).size(), 1);
}

void FimCacheTest::contextHashes_values()
{
    const QString prefix = "a\nb\nc\n";
    const QString middle = "int x";
    const QString suffix = ";";

    const QByteArrayList hashes = Fim::contextHashes(prefix, middle, suffix);

    const auto sha256 = [](const QString &s) {
        return QCryptographicHash::hash(s.toUtf8(), QCryptographicHash::Sha256).toHex();
    };

    QCOMPARE(hashes[0], sha256(prefix + middle + Fim::kContextSeparator + suffix));
    QCOMPARE(hashes[1], sha256("b\nc\n" + middle + Fim::kContextSeparator + suffix));
    QCOMPARE(hashes[2], sha256("c\n" + middle + Fim::kContextSeparator + suffix));
}

// The core use case: the user generated a completion with prefix "a\nb\nc\n" and
// the cursor has since moved up one line, so the context is now "b\nc\n". The
// full hash of the new context must be one of the hashes computed for the
// original one.
void FimCacheTest::contextHashes_trimmedPrefixMatchesFullContext()
{
    const QString middle = "int x";
    const QString suffix = ";";

    const QByteArrayList original = Fim::contextHashes("a\nb\nc\n", middle, suffix);
    const QByteArrayList moved = Fim::contextHashes("b\nc\n", middle, suffix);

    QVERIFY(original.contains(moved.first()));

    // and two lines up
    const QByteArrayList movedTwice = Fim::contextHashes("c\n", middle, suffix);
    QVERIFY(original.contains(movedTwice.first()));
}

// The hash must be computed over the UTF-8 encoded separator (0xC3 0x8E),
// byte-identical to what the original inline computation produced.
void FimCacheTest::contextHashes_separatorByte()
{
    const QByteArray separator = Fim::kContextSeparator.toUtf8();
    QCOMPARE(separator, QByteArray::fromHex("c38e"));

    const QByteArrayList hashes = Fim::contextHashes("", "m", "s");
    const QByteArray input = (QStringLiteral("m") + Fim::kContextSeparator + 's').toUtf8();
    QCOMPARE(input, QByteArray("m") + separator + QByteArray("s"));
    QCOMPARE(hashes.first(), QCryptographicHash::hash(input, QCryptographicHash::Sha256).toHex());
}

// ---------------------------------------------------------------------------
// splitResponse
// ---------------------------------------------------------------------------

void FimCacheTest::splitResponse_singleObject()
{
    const QByteArray raw = R"({"content": "hello", "tokens_cached": 42})";
    const QList<QByteArray> responses = Fim::splitResponse(raw);
    QCOMPARE(responses.size(), 1);
    QCOMPARE(contentOf(responses[0]), QString("hello"));
}

void FimCacheTest::splitResponse_array()
{
    const QByteArray raw
        = R"([{"content": "first"}, {"content": "second"}, {"content": "third"}])";
    const QList<QByteArray> responses = Fim::splitResponse(raw);
    QCOMPARE(responses.size(), 3);
    QCOMPARE(contentOf(responses[0]), QString("first"));
    QCOMPARE(contentOf(responses[1]), QString("second"));
    QCOMPARE(contentOf(responses[2]), QString("third"));
}

void FimCacheTest::splitResponse_invalidJson()
{
    const QByteArray raw = "this is not json";
    const QList<QByteArray> responses = Fim::splitResponse(raw);
    QCOMPARE(responses.size(), 1);
    QCOMPARE(responses[0], raw);
}

void FimCacheTest::splitResponse_arrayWithoutObjects()
{
    const QByteArray raw = "[1, 2, \"three\"]";
    const QList<QByteArray> responses = Fim::splitResponse(raw);
    QCOMPARE(responses.size(), 1);
    QCOMPARE(responses[0], raw);
}

// ---------------------------------------------------------------------------
// ResultCache
// ---------------------------------------------------------------------------

void FimCacheTest::resultCache_insertAndLookup()
{
    Fim::ResultCache cache;
    const QByteArrayList keys = Fim::contextHashes("a\n", "m", "s");

    QVERIFY(!cache.containsAny(keys));

    cache.insert(keys, R"({"content": "hello"})", 1);

    QVERIFY(cache.containsAny(keys));
    QCOMPARE(cache.size(), 1);
    QCOMPARE(completionList(cache, keys.first()), QStringList({"hello"}));
}

void FimCacheTest::resultCache_storedUnderAllKeys()
{
    Fim::ResultCache cache;
    const QByteArrayList keys = Fim::contextHashes("a\nb\nc\nd\n", "m", "s");
    QCOMPARE(keys.size(), 4);

    cache.insert(keys, R"({"content": "hello"})", 1);

    // every key (full and trimmed) must map to the completion, so that a
    // lookup after the cursor moved up a few lines hits the cache
    for (const QByteArray &key : std::as_const(keys)) {
        QCOMPARE(completionList(cache, key), QStringList({"hello"}));
    }
}

void FimCacheTest::resultCache_ringEviction()
{
    Fim::ResultCache cache;
    const QByteArrayList keys{QByteArray("key")};

    cache.insert(keys, R"({"content": "one"})", 2);
    cache.insert(keys, R"({"content": "two"})", 2);
    QCOMPARE(completionList(cache, keys.first()), QStringList({"one", "two"}));

    // ring is full: the oldest entry is evicted
    cache.insert(keys, R"({"content": "three"})", 2);
    QCOMPARE(completionList(cache, keys.first()), QStringList({"two", "three"}));
}

void FimCacheTest::resultCache_contentDedup()
{
    Fim::ResultCache cache;
    const QByteArrayList keys{QByteArray("key")};

    // identical raw response
    cache.insert(keys, R"({"content": "same"})", 4);
    cache.insert(keys, R"({"content": "same"})", 4);
    QCOMPARE(completionList(cache, keys.first()), QStringList({"same"}));

    // same content, different surrounding fields (e.g. timings)
    cache.insert(keys, R"({"content": "same", "tokens_cached": 1})", 4);
    QCOMPARE(completionList(cache, keys.first()), QStringList({"same"}));
}

void FimCacheTest::resultCache_lruMaxCost()
{
    Fim::ResultCache cache;
    cache.setMaxCost(2);

    cache.insert({QByteArray("a")}, R"({"content": "A"})", 1);
    cache.insert({QByteArray("b")}, R"({"content": "B"})", 1);
    cache.insert({QByteArray("c")}, R"({"content": "C"})", 1);

    QVERIFY(cache.size() <= 2);
    // the most recently inserted key must still be present
    QCOMPARE(completionList(cache, QByteArray("c")), QStringList({"C"}));
}

void FimCacheTest::resultCache_lookup()
{
    Fim::ResultCache cache;
    const QByteArray key{"key"};

    // unlike operator[], lookup() must not create an entry for unknown keys
    QVERIFY2(cache.lookup(key) == nullptr, "lookup created an entry");
    QCOMPARE(cache.size(), 0);

    cache.insert({key}, R"({"content": "hello"})", 1);
    QCOMPARE(contentOf(cache.lookup(key)->first()), QString("hello"));
    QCOMPARE(cache.size(), 1);
}

void FimCacheTest::resultCache_containsAny()
{
    Fim::ResultCache cache;
    const QByteArrayList keys = Fim::contextHashes("a\nb\nc\n", "m", "s");

    cache.insert(keys, R"({"content": "hello"})", 1);

    // an unrelated key alone does not count
    QVERIFY(!cache.containsAny({QByteArray("unrelated")}));

    // but any one of the original keys does
    QVERIFY(cache.containsAny({keys.last()}));
}

void FimCacheTest::resultCache_clear()
{
    Fim::ResultCache cache;
    const QByteArrayList keys{QByteArray("key")};

    cache.insert(keys, R"({"content": "hello"})", 1);
    QVERIFY(cache.containsAny(keys));

    cache.clear();
    QCOMPARE(cache.size(), 0);
    QVERIFY(!cache.containsAny(keys));
}

QTEST_MAIN(FimCacheTest)

#include "fimcache_test.moc"
