#include "web_utils.h"

#include <QtTaskTree/QNetworkReplyWrapper>
#include <QtTaskTree/QTaskTree>

#include <QHostAddress>
#include <QHostInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStringList>
#include <QUrl>

namespace LlamaCpp::Tools {

using namespace QtTaskTree;

namespace {

const char kUserAgent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/143.0.0.0 Safari/537.36";

struct FetchState
{
    QByteArray body;
    QString contentType;
    QString error;
    bool tooLarge = false;
    QString refused; // why a redirect target was refused (httpGetPublic)
    HttpResponseCallback done;
};

// The IPv4 address embedded in an IPv6 address of a translation or
// tunneling range (IPv4-compatible ::/96, NAT64 64:ff9b::/96, 6to4
// 2002::/16); a null address otherwise. IPv4-mapped addresses are handled
// by QHostAddress::toIPv4Address() already.
QHostAddress embeddedIPv4(const QHostAddress &address)
{
    static const QPair<QHostAddress, int> compatible
        = QHostAddress::parseSubnet(QStringLiteral("::/96"));
    static const QPair<QHostAddress, int> nat64
        = QHostAddress::parseSubnet(QStringLiteral("64:ff9b::/96"));
    static const QPair<QHostAddress, int> sixToFour
        = QHostAddress::parseSubnet(QStringLiteral("2002::/16"));
    const Q_IPV6ADDR bytes = address.toIPv6Address();
    const auto ipv4At = [&bytes](int offset) {
        return QHostAddress((quint32(bytes[offset]) << 24) | (quint32(bytes[offset + 1]) << 16)
                            | (quint32(bytes[offset + 2]) << 8) | quint32(bytes[offset + 3]));
    };
    if (address.isInSubnet(compatible) || address.isInSubnet(nat64))
        return ipv4At(12);
    if (address.isInSubnet(sixToFour))
        return ipv4At(2);
    return {};
}

bool isPublicIPv4(const QHostAddress &address)
{
    static const QStringList blocked = {
        QStringLiteral("0.0.0.0/8"),       // "this network", unspecified
        QStringLiteral("10.0.0.0/8"),      // private
        QStringLiteral("100.64.0.0/10"),   // carrier-grade NAT
        QStringLiteral("127.0.0.0/8"),     // loopback
        QStringLiteral("169.254.0.0/16"),  // link-local (cloud metadata)
        QStringLiteral("172.16.0.0/12"),   // private
        QStringLiteral("192.0.0.0/24"),    // IETF protocol assignments
        QStringLiteral("192.0.2.0/24"),    // documentation
        QStringLiteral("192.168.0.0/16"),  // private
        QStringLiteral("198.18.0.0/15"),   // benchmarking
        QStringLiteral("198.51.100.0/24"), // documentation
        QStringLiteral("203.0.113.0/24"),  // documentation
        QStringLiteral("224.0.0.0/4"),     // multicast
        QStringLiteral("240.0.0.0/4"),     // reserved, broadcast
    };
    for (const QString &subnet : blocked)
        if (address.isInSubnet(QHostAddress::parseSubnet(subnet)))
            return false;
    return true;
}

// Checks that \a url may be fetched on behalf of the model and calls
// \a onChecked with an empty string when it may, or with the reason when
// not. Synchronous for address literals, otherwise after an asynchronous
// host lookup; \a context bounds the lifetime of that callback.
void checkPublicUrl(const QUrl &url,
                    const QObject *context,
                    const std::function<void(const QString &)> &onChecked)
{
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
        return onChecked(QStringLiteral("only http and https URLs can be fetched"));
    const QString host = url.host();
    if (host.isEmpty())
        return onChecked(QStringLiteral("the URL has no host"));
    const QString refused
        = QStringLiteral("%1 is a local or private network address and cannot be fetched")
              .arg(host);

    QHostAddress literal;
    if (literal.setAddress(host))
        return onChecked(isPublicAddress(literal) ? QString() : refused);

    QHostInfo::lookupHost(host, context, [host, refused, onChecked](const QHostInfo &info) {
        if (info.error() != QHostInfo::NoError || info.addresses().isEmpty())
            return onChecked(QStringLiteral("cannot resolve %1: %2").arg(host, info.errorString()));
        // The connection may use any of the addresses, so all of them must
        // be public.
        for (const QHostAddress &address : info.addresses())
            if (!isPublicAddress(address))
                return onChecked(refused);
        onChecked({});
    });
}

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    return request;
}

// Runs the request as a QNetworkReplyWrapper task inside its own task tree,
// the same way the ripgrep download is driven, and reports the result
// through \a done when the tree is finished.
void startRequest(const QNetworkRequest &request,
                  QNetworkAccessManager::Operation operation,
                  const QByteArray &postData,
                  qint64 maxResponseBytes,
                  HttpResponseCallback done,
                  QNetworkReply **replyOut,
                  bool publicOnly = false)
{
    auto *state = new FetchState{};
    state->done = std::move(done);

    auto *tree = new QTaskTree;
    // The wrapper only holds a raw pointer to the manager, so keep it alive
    // as long as the tree (and with it the request) is.
    auto *manager = new QNetworkAccessManager(tree);

    const auto onSetup = [state, request, operation, postData, manager,
                          maxResponseBytes, replyOut, publicOnly](
                             QNetworkReplyWrapper &wrapper) {
        wrapper.setRequest(request);
        wrapper.setOperation(operation);
        if (operation == QNetworkAccessManager::PostOperation
            || operation == QNetworkAccessManager::PutOperation)
            wrapper.setData(postData);
        wrapper.setNetworkAccessManager(manager);

        // Enforce the response size cap while the body is streaming in.
        // The reply is only guaranteed to be alive between the started()
        // and done() signals, so abort it from here, not later.
        QObject::connect(&wrapper,
                         &QNetworkReplyWrapper::downloadProgress,
                         &wrapper,
                         [&wrapper, state, maxResponseBytes](qint64 received, qint64) {
                             if (received > maxResponseBytes) {
                                 state->tooLarge = true;
                                 if (QNetworkReply *reply = wrapper.reply())
                                     reply->abort();
                             }
                         });

        // Hand the reply to the caller so it can abort the request.
        if (replyOut)
            QObject::connect(&wrapper,
                             &QNetworkReplyWrapper::started,
                             &wrapper,
                             [&wrapper, replyOut] { *replyOut = wrapper.reply(); });

        // Every redirect target is checked like the initial URL (the
        // request uses UserVerifiedRedirectPolicy, so the reply waits for
        // redirectAllowed()), or a public page could redirect to a local
        // service.
        if (publicOnly)
            QObject::connect(&wrapper, &QNetworkReplyWrapper::started, &wrapper, [&wrapper, state] {
                QNetworkReply *reply = wrapper.reply();
                if (!reply)
                    return;
                QObject::connect(reply,
                                 &QNetworkReply::redirected,
                                 reply,
                                 [reply, state](const QUrl &target) {
                                     checkPublicUrl(reply->url().resolved(target),
                                                    reply,
                                                    [reply, state](const QString &error) {
                                                        if (error.isEmpty()) {
                                                            emit reply->redirectAllowed();
                                                            return;
                                                        }
                                                        state->refused = QStringLiteral(
                                                                             "redirect refused: %1")
                                                                             .arg(error);
                                                        reply->abort();
                                                    });
                                 });
            });
    };

    // The wrapper deletes the reply just after emitting done(), so this is
    // the last chance to read it.
    const auto onDone = [state](const QNetworkReplyWrapper &wrapper, DoneWith) {
        if (QNetworkReply *reply = wrapper.reply()) {
            // Keep the body empty on failure, as the API contract says.
            if (!state->tooLarge)
                state->body = reply->readAll();
            state->contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
            if (reply->error() != QNetworkReply::NoError)
                state->error = reply->errorString();
        } else {
            state->error = QStringLiteral("the request could not be started");
        }
    };

    tree->setRecipe(Group{QNetworkReplyWrapperTask(onSetup, onDone)});

    QObject::connect(tree, &QTaskTree::done, tree, [tree, state](DoneWith result) {
                         const QString error = !state->refused.isEmpty()
                                                   ? state->refused
                                               : state->tooLarge
                                                   ? QStringLiteral("response exceeds the size limit")
                                                   : (result == DoneWith::Cancel
                                                          ? QStringLiteral("request was canceled")
                                                          : state->error);
                         auto doneFn = std::move(state->done);
                         doneFn(state->body, state->contentType, error);
                         delete state;
                         tree->deleteLater();
                     });
    tree->start();
}

} // namespace

bool isPublicAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    bool isIPv4 = false;
    // Also unwraps IPv4-mapped IPv6 addresses (::ffff:127.0.0.1).
    const quint32 ipv4 = address.toIPv4Address(&isIPv4);
    if (isIPv4)
        return isPublicIPv4(QHostAddress(ipv4));
    if (address.protocol() != QAbstractSocket::IPv6Protocol)
        return false;

    static const QStringList blocked = {
        QStringLiteral("::/128"),        // unspecified
        QStringLiteral("::1/128"),       // loopback
        QStringLiteral("100::/64"),      // discard-only
        QStringLiteral("2001:db8::/32"), // documentation
        QStringLiteral("fc00::/7"),      // unique local
        QStringLiteral("fe80::/10"),     // link-local
        QStringLiteral("fec0::/10"),     // site-local (deprecated)
        QStringLiteral("ff00::/8"),      // multicast
    };
    for (const QString &subnet : blocked)
        if (address.isInSubnet(QHostAddress::parseSubnet(subnet)))
            return false;
    const QHostAddress embedded = embeddedIPv4(address);
    return embedded.isNull() || isPublicIPv4(embedded);
}

void httpGetPublic(const QString &url,
                   int timeoutSeconds,
                   qint64 maxResponseBytes,
                   HttpResponseCallback done,
                   QNetworkReply **replyOut)
{
    const QUrl target(url);
    // Bounds the lifetime of the host lookup callback; released with it.
    auto *guard = new QObject;
    checkPublicUrl(target,
                   guard,
                   [guard, target, timeoutSeconds, maxResponseBytes, done, replyOut](
                       const QString &error) {
                       guard->deleteLater();
                       if (!error.isEmpty())
                           return done({}, {}, error);
                       QNetworkRequest request = makeRequest(target);
                       request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);
                       request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                            QNetworkRequest::UserVerifiedRedirectPolicy);
                       startRequest(request,
                                    QNetworkAccessManager::GetOperation,
                                    {},
                                    maxResponseBytes,
                                    done,
                                    replyOut,
                                    /*publicOnly=*/true);
                   });
}

void httpGet(const QString &url,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done,
             QNetworkReply **replyOut)
{
    QNetworkRequest request = makeRequest(QUrl(url));
    request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);
    startRequest(request,
                 QNetworkAccessManager::GetOperation,
                 {},
                 maxResponseBytes,
                 std::move(done),
                 replyOut);
}

void httpGet(const QString &url,
             const QList<QPair<QByteArray, QByteArray>> &headers,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done,
             QNetworkReply **replyOut)
{
    QNetworkRequest request = makeRequest(QUrl(url));
    request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);
    for (const auto &header : headers)
        request.setRawHeader(header.first, header.second);
    startRequest(request,
                 QNetworkAccessManager::GetOperation,
                 {},
                 maxResponseBytes,
                 std::move(done),
                 replyOut);
}

void httpPost(const QString &url,
              const QByteArray &body,
              const QList<QPair<QByteArray, QByteArray>> &headers,
              int timeoutSeconds,
              qint64 maxResponseBytes,
              HttpResponseCallback done,
              QNetworkReply **replyOut)
{
    QNetworkRequest request = makeRequest(QUrl(url));
    request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);
    for (const auto &header : headers)
        request.setRawHeader(header.first, header.second);
    startRequest(request,
                 QNetworkAccessManager::PostOperation,
                 body,
                 maxResponseBytes,
                 std::move(done),
                 replyOut);
}

} // namespace LlamaCpp::Tools
