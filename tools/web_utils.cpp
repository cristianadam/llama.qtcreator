#include "web_utils.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace LlamaCpp::Tools {

namespace {

const char kUserAgent[] =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/143.0.0.0 Safari/537.36";

struct FetchState
{
    QByteArray body;
    bool tooLarge = false;
    HttpResponseCallback done;
};

QNetworkRequest makeRequest(const QUrl &url)
{
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader, kUserAgent);
    request.setRawHeader("Accept-Language", "en-US,en;q=0.9");
    return request;
}

void start(QNetworkAccessManager *manager,
           QNetworkReply *reply,
           HttpResponseCallback done,
           qint64 maxResponseBytes)
{
    auto *state = new FetchState{QByteArray{}, false, std::move(done)};

    QObject::connect(reply,
                     &QNetworkReply::readyRead,
                     reply,
                     [reply, state, maxResponseBytes]() {
                         state->body.append(reply->readAll());
                         if (state->body.size() > maxResponseBytes) {
                             state->tooLarge = true;
                             reply->abort();
                         }
                     });

    QObject::connect(reply, &QNetworkReply::finished, reply, [manager, reply, state]() {
        state->body.append(reply->readAll());
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();

        auto doneFn = std::move(state->done);
        if (state->tooLarge)
            doneFn({}, contentType, QStringLiteral("response exceeds the size limit"));
        else if (reply->error() != QNetworkReply::NoError)
            doneFn({}, contentType, reply->errorString());
        else
            doneFn(state->body, contentType, {});

        delete state;
        manager->deleteLater();
        reply->deleteLater();
    });
}

} // namespace

void httpGet(const QString &url,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done)
{
    QNetworkRequest request = makeRequest(QUrl(url));
    request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);

    auto *manager = new QNetworkAccessManager();
    start(manager, manager->get(request), std::move(done), maxResponseBytes);
}

void httpGet(const QString &url,
             const QList<QPair<QByteArray, QByteArray>> &headers,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done)
{
    QNetworkRequest request = makeRequest(QUrl(url));
    request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);
    for (const auto &header : headers)
        request.setRawHeader(header.first, header.second);

    auto *manager = new QNetworkAccessManager();
    start(manager, manager->get(request), std::move(done), maxResponseBytes);
}

void httpPost(const QString &url,
              const QByteArray &body,
              const QList<QPair<QByteArray, QByteArray>> &headers,
              int timeoutSeconds,
              qint64 maxResponseBytes,
              HttpResponseCallback done)
{
    QNetworkRequest request = makeRequest(QUrl(url));
    request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);
    for (const auto &header : headers)
        request.setRawHeader(header.first, header.second);

    auto *manager = new QNetworkAccessManager();
    start(manager, manager->post(request, body), std::move(done), maxResponseBytes);
}

} // namespace LlamaCpp::Tools
