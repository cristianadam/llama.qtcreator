#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <functional>

namespace LlamaCpp::Tools {

using HttpResponseCallback
    = std::function<void(const QByteArray &body, const QString &contentType, const QString &error)>;

/*! Performs an asynchronous HTTP GET with a transfer timeout and a
    response-size cap.  \a done is called exactly once when the request
    finishes: on success \a error is empty and \a body holds the response
    (aborted when it exceeds \a maxResponseBytes), on failure \a body is
    empty and \a error a human‑readable message. */
void httpGet(const QString &url,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done);

/*! Like httpGet, but POSTs \a body with the given raw headers. */
void httpPost(const QString &url,
              const QByteArray &body,
              const QList<QPair<QByteArray, QByteArray>> &headers,
              int timeoutSeconds,
              qint64 maxResponseBytes,
              HttpResponseCallback done);

} // namespace LlamaCpp::Tools
