#include "web_utils.h"

#include <QtTaskTree/QNetworkReplyWrapper>
#include <QtTaskTree/QTaskTree>

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
    HttpResponseCallback done;
};

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
                  HttpResponseCallback done)
{
    auto *state = new FetchState{};
    state->done = std::move(done);

    auto *tree = new QTaskTree;
    // The wrapper only holds a raw pointer to the manager, so keep it alive
    // as long as the tree (and with it the request) is.
    auto *manager = new QNetworkAccessManager(tree);

    const auto onSetup = [state, request, operation, postData, manager, maxResponseBytes](
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
                         const QString error = state->tooLarge
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

void httpGet(const QString &url,
             int timeoutSeconds,
             qint64 maxResponseBytes,
             HttpResponseCallback done)
{
    QNetworkRequest request = makeRequest(QUrl(url));
    request.setTransferTimeout(qMax(1, timeoutSeconds) * 1000);
    startRequest(request,
                 QNetworkAccessManager::GetOperation,
                 {},
                 maxResponseBytes,
                 std::move(done));
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
    startRequest(request,
                 QNetworkAccessManager::GetOperation,
                 {},
                 maxResponseBytes,
                 std::move(done));
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
    startRequest(request,
                 QNetworkAccessManager::PostOperation,
                 body,
                 maxResponseBytes,
                 std::move(done));
}

} // namespace LlamaCpp::Tools
