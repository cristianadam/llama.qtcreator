#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <functional>

class QHostAddress;
class QNetworkReply;

namespace LlamaCpp::Tools {

using HttpResponseCallback
    = std::function<void(const QByteArray &body, const QString &contentType, const QString &error)>;

/*! Performs an asynchronous HTTP GET with a transfer timeout and a
    response-size cap.  \a done is called exactly once when the request
    finishes: on success \a error is empty and \a body holds the response
    (aborted when it exceeds \a maxResponseBytes), on failure \a body is
    empty and \a error a human‑readable message.

    When \a replyOut is not null, it receives the QNetworkReply of the
    in‑flight request, so the caller can abort it (the reply is deleted
    before \a done is called, so the caller must not use it afterwards). */
void httpGet(const QString &url,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done,
             QNetworkReply **replyOut = nullptr);

/*! True when \a address may be fetched on behalf of the model: not
    loopback, link-local, private (RFC 1918, CGNAT 100.64/10, IPv6 ULA),
    unspecified, multicast, broadcast or otherwise reserved, also when the
    IPv4 address is embedded in an IPv6 one (IPv4-mapped, -compatible,
    NAT64, 6to4). */
bool isPublicAddress(const QHostAddress &address);

/*! Like httpGet, but for untrusted (model-supplied) URLs, against
    server-side request forgery: only http and https are allowed, and the
    host is resolved and refused unless all of its addresses are public
    (isPublicAddress()) - before the request and again for every redirect
    target. Configured endpoints (llama-server, search providers) use
    httpGet/httpPost and are not restricted.

    Known limit: the network stack resolves the host once more for the
    connection, so a DNS rebinding attack with a very short TTL can still
    race the check. */
void httpGetPublic(const QString &url,
                   int timeoutSeconds,
                   qint64 maxResponseBytes,
                   HttpResponseCallback done,
                   QNetworkReply **replyOut = nullptr);

/*! Like httpGet, but also sends the given raw headers. */
void httpGet(const QString &url,
             const QList<QPair<QByteArray, QByteArray>> &headers,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done,
             QNetworkReply **replyOut = nullptr);

/*! Like httpGet, but POSTs \a body with the given raw headers. */
void httpPost(const QString &url,
              const QByteArray &body,
              const QList<QPair<QByteArray, QByteArray>> &headers,
              int timeoutSeconds,
              qint64 maxResponseBytes,
              HttpResponseCallback done,
              QNetworkReply **replyOut = nullptr);

} // namespace LlamaCpp::Tools
