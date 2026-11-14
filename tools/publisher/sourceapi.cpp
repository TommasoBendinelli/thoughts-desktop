// SPDX-License-Identifier: GPL-2.0-or-later
#include "sourceapi.h"

#include "pyerror.h"
#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "pyurl.h"
#include "serverids.h"
#include "sha256.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QThread>
#include <QUrl>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

// The text Python's urllib gives a failed connection, so messages and the
// timeout classification ("timed out") read as before.
QString networkErrorText(QNetworkReply::NetworkError error, const QString &qtText)
{
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
#if defined(Q_OS_WIN)
        return QStringLiteral("<urlopen error [WinError 10061] No connection could be made because the target machine actively refused it>");
#elif defined(Q_OS_DARWIN)
        return QStringLiteral("<urlopen error [Errno 61] Connection refused>");
#else
        return QStringLiteral("<urlopen error [Errno 111] Connection refused>");
#endif
    case QNetworkReply::HostNotFoundError:
#if defined(Q_OS_WIN)
        return QStringLiteral("<urlopen error [Errno 11001] getaddrinfo failed>");
#elif defined(Q_OS_DARWIN)
        return QStringLiteral("<urlopen error [Errno 8] nodename nor servname provided, or not known>");
#else
        return QStringLiteral("<urlopen error [Errno -2] Name or service not known>");
#endif
    case QNetworkReply::TimeoutError:
    case QNetworkReply::OperationCanceledError:
        return QStringLiteral("<urlopen error timed out>");
    case QNetworkReply::RemoteHostClosedError:
        return QStringLiteral("<urlopen error Remote end closed connection without response>");
    default:
        return QStringLiteral("<urlopen error %1>").arg(qtText);
    }
}

QList<Json> requireList(const Json &rows)
{
    if (!rows.isArray()) {
        Py::raiseValueError(QStringLiteral("server returned an invalid source conflict list"));
    }
    return rows.items();
}
}

QByteArray compactJson(const Json &payload)
{
    Py::DumpOptions options;
    options.separators = QPair<QString, QString>(QStringLiteral(","), QStringLiteral(":"));
    return Py::dumpsChecked(payload, options);
}

std::optional<QString> canonicalUuid(const QString &text)
{
    QString hex = text;
    hex.replace(QStringLiteral("urn:"), QString()).replace(QStringLiteral("uuid:"), QString());
    hex = Py::strip(hex, QStringLiteral("{}"));
    hex.remove(QLatin1Char('-'));
    if (hex.size() != 32) {
        return std::nullopt;
    }
    for (const QChar c : hex) {
        if (!((c >= QLatin1Char('0') && c <= QLatin1Char('9')) || (c >= QLatin1Char('a') && c <= QLatin1Char('f'))
              || (c >= QLatin1Char('A') && c <= QLatin1Char('F')))) {
            return std::nullopt;
        }
    }
    hex = hex.toLower();
    return hex.left(8) + QLatin1Char('-') + hex.mid(8, 4) + QLatin1Char('-') + hex.mid(12, 4) + QLatin1Char('-') + hex.mid(16, 4) + QLatin1Char('-')
        + hex.mid(20);
}

SourceApi::SourceApi(const QString &baseUrl, const QString &token, int timeoutSeconds)
    : m_baseUrl(baseUrl)
    , m_token(token)
    , m_timeoutSeconds(timeoutSeconds)
    , m_network(std::make_unique<QNetworkAccessManager>())
{
    while (m_baseUrl.endsWith(QLatin1Char('/'))) {
        m_baseUrl.chop(1);
    }
    const QString candidate = Py::strip(qEnvironmentVariable("THOUGHTS_TRACE_ID"));
    m_traceId = candidate.isEmpty() ? QString() : canonicalUuid(candidate).value_or(QString());
    m_network->setProxy(QNetworkProxy::NoProxy);
}

SourceApi::~SourceApi() = default;

void SourceApi::setTransferProgress(std::function<void(qint64, qint64, const QString &)> callback)
{
    m_transferProgress = std::move(callback);
}

QPair<qint64, qint64> SourceApi::transferTotals() const
{
    qint64 uploaded = 0;
    for (const qint64 size : m_uploadedObjects) {
        uploaded += size;
    }
    for (const qint64 size : m_uploadedPayloads) {
        uploaded += size;
    }
    qint64 downloaded = 0;
    for (const qint64 size : m_downloadedObjects) {
        downloaded += size;
    }
    return {uploaded, downloaded};
}

void SourceApi::notifyTransferProgress(const QString &direction)
{
    if (m_transferProgress) {
        const auto [uploaded, downloaded] = transferTotals();
        m_transferProgress(uploaded, downloaded, direction);
    }
}

void SourceApi::recordPayloadUpload(const QString &category, const Json &payload)
{
    const QByteArray content = compactJson(payload);
    const QString key = category + QLatin1Char(':') + sha256Hex(content);
    if (!m_uploadedPayloads.contains(key)) {
        m_uploadedPayloads.insert(key, content.size());
    }
    notifyTransferProgress(QStringLiteral("upload"));
}

namespace
{
// Made on first use: a test may set the pauses before any other static of this file exists.
QList<int> &retryDelaysMs()
{
    static QList<int> delays{1000, 2000, 4000};
    return delays;
}

// Repeating these changes nothing: an object is stored under its digest, a publication
// and a render check are answered again under their identity, a build-input snapshot is
// the same snapshot.
bool repeatable(const QByteArray &method, const QString &path)
{
    if (method == "GET" || method == "HEAD") {
        return true;
    }
    static const QStringList identified{QStringLiteral("/v1/objects"), QStringLiteral("/v1/source-publications"), QStringLiteral("/v1/build-input-snapshots"),
                                        QStringLiteral("/v1/render-checks")};
    return method == "POST" && identified.contains(path);
}
}

void SourceApi::setRetryDelays(const QList<int> &milliseconds)
{
    retryDelaysMs() = milliseconds;
}

bool SourceApi::transientFailure(const Py::Exception &error)
{
    if (error.type != Py::apiErrorType) {
        return false;
    }
    // Status 0 is also used for answers that fail a check; only "no answer" is transient.
    return error.status == 502 || error.status == 503 || error.status == 504 || (error.status == 0 && error.message.startsWith(QLatin1String("<urlopen error")));
}

SourceApi::Response SourceApi::request(const QByteArray &method, const QString &path, const std::optional<Json> &payload,
                                       const std::optional<QByteArray> &content, const QList<QPair<QByteArray, QByteArray>> &headers)
{
    for (qsizetype attempt = 0;; ++attempt) {
        try {
            return requestOnce(method, path, payload, content, headers);
        } catch (const Py::Exception &error) {
            if (attempt >= retryDelaysMs().size() || !repeatable(method, path) || !transientFailure(error)) {
                throw;
            }
        }
        QThread::msleep(static_cast<unsigned long>(qMax(0, retryDelaysMs().at(attempt))));
    }
}

SourceApi::Response SourceApi::requestOnce(const QByteArray &method, const QString &path, const std::optional<Json> &payload,
                                           const std::optional<QByteArray> &content, const QList<QPair<QByteArray, QByteArray>> &headers)
{
    QList<QPair<QByteArray, QByteArray>> outgoing{{"Authorization", "Bearer " + m_token.toUtf8()}, {"Accept", "application/json"}};
    for (const auto &header : headers) {
        bool replaced = false;
        for (auto &existing : outgoing) {
            if (existing.first.compare(header.first, Qt::CaseInsensitive) == 0) {
                existing.second = header.second;
                replaced = true;
            }
        }
        if (!replaced) {
            outgoing.append(header);
        }
    }
    if (!m_traceId.isEmpty()) {
        outgoing.append(QPair<QByteArray, QByteArray>("X-Thoughts-Trace-Id", m_traceId.toLatin1()));
    }
    std::optional<QByteArray> body = content;
    if (payload) {
        body = compactJson(*payload);
        outgoing.append(QPair<QByteArray, QByteArray>("Content-Type", "application/json"));
    }
    QUrl url(m_baseUrl + path, QUrl::TolerantMode);
    QByteArray verb = method;
    // urllib follows redirects: a POST becomes a GET without its body on
    // 301/302/303 and fails on 307/308.
    for (int redirects = 0;; ++redirects) {
        QNetworkRequest request(url);
        for (const auto &[name, value] : std::as_const(outgoing)) {
            request.setRawHeader(name, value);
        }
        request.setRawHeader("Accept-Encoding", "identity");
        request.setRawHeader("User-Agent", "thoughts-publish-sources");
        request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(m_timeoutSeconds * 1000);
        if (body) {
            request.setHeader(QNetworkRequest::ContentLengthHeader, body->size());
        }
        QNetworkReply *reply = nullptr;
        if (verb == "HEAD" && !body) {
            reply = m_network->head(request);
        } else if (verb == "GET" && !body) {
            reply = m_network->get(request);
        } else {
            reply = body ? m_network->sendCustomRequest(request, verb, *body) : m_network->sendCustomRequest(request, verb);
        }
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        if (!reply->isFinished()) {
            loop.exec(QEventLoop::ExcludeUserInputEvents);
        }
        const std::unique_ptr<QNetworkReply, void (*)(QNetworkReply *)> owner(reply, [](QNetworkReply *r) {
            r->deleteLater();
        });
        const QVariant statusAttribute = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        const int status = statusAttribute.isValid() ? statusAttribute.toInt() : 0;
        const QByteArray responseBody = reply->readAll();
        if (status == 0) {
            Py::raiseApiError(0, networkErrorText(reply->error(), reply->errorString()));
        }
        const bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        const bool bodiless = verb == "GET" || verb == "HEAD";
        if (redirect && (bodiless || status == 301 || status == 302 || status == 303) && redirects < 10) {
            const QUrl location = url.resolved(QUrl::fromEncoded(reply->rawHeader("Location")));
            if (location.isValid() && (location.scheme() == QLatin1String("http") || location.scheme() == QLatin1String("https"))) {
                // The token is this server's: a redirect to another origin
                // (scheme, host or port) is followed without it, and so is
                // everything after it.
                if (location.scheme() != url.scheme() || location.host().compare(url.host(), Qt::CaseInsensitive) != 0
                    || location.port(location.scheme() == QLatin1String("https") ? 443 : 80) != url.port(url.scheme() == QLatin1String("https") ? 443 : 80)) {
                    outgoing.removeIf([](const QPair<QByteArray, QByteArray> &header) {
                        return header.first.compare("Authorization", Qt::CaseInsensitive) == 0;
                    });
                }
                url = location;
                if (!bodiless) {
                    verb = "GET";
                    body.reset();
                    outgoing.removeIf([](const QPair<QByteArray, QByteArray> &header) {
                        return header.first.compare("Content-Type", Qt::CaseInsensitive) == 0
                            || header.first.compare("Content-Length", Qt::CaseInsensitive) == 0;
                    });
                }
                continue;
            }
        }
        if (status >= 200 && status < 300) {
            if (!m_traceId.isEmpty() && QString::fromLatin1(reply->rawHeader("X-Thoughts-Trace-Id")) != m_traceId) {
                Py::raiseApiError(0, QStringLiteral("server did not echo the source-publication trace ID"));
            }
            return {status, responseBody};
        }
        const QString detail = Py::decodeUtf8Replacing(responseBody);
        const QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        Py::raiseApiError(status, detail.isEmpty() ? QStringLiteral("HTTP Error %1: %2").arg(status).arg(reason) : detail);
    }
}

QString SourceApi::uploadObject(const QByteArray &content, const QString &mediaType)
{
    const QString digest = sha256Hex(content);
    const Response response =
        request("POST", QStringLiteral("/v1/objects"), std::nullopt, content, {{"Content-Type", mediaType.toUtf8()}, {"X-Content-SHA256", digest.toLatin1()}});
    const Json value = Py::loadsBytes(response.body);
    if (Py::get(value, QStringLiteral("sha256")) != Json(digest)) {
        Py::raiseApiError(0, QStringLiteral("server returned the wrong object identity"));
    }
    if (!m_uploadedObjects.contains(digest)) {
        m_uploadedObjects.insert(digest, content.size());
    }
    notifyTransferProgress(QStringLiteral("upload"));
    return digest;
}

bool SourceApi::hasObject(const QString &digest)
{
    try {
        request("HEAD", QStringLiteral("/v1/objects/") + requireObjectDigest(digest));
    } catch (const Py::Exception &error) {
        if (error.type == Py::apiErrorType && error.status == 404) {
            return false;
        }
        throw;
    }
    return true;
}

QByteArray SourceApi::download(const QString &digest)
{
    const QByteArray raw = request("GET", QStringLiteral("/v1/objects/") + requireObjectDigest(digest)).body;
    if (sha256Hex(raw) != digest) {
        Py::raiseApiError(0, QStringLiteral("downloaded source object failed SHA-256 verification"));
    }
    if (!m_downloadedObjects.contains(digest)) {
        m_downloadedObjects.insert(digest, raw.size());
    }
    notifyTransferProgress(QStringLiteral("download"));
    return raw;
}

Json SourceApi::sourceTree()
{
    return Py::loadsBytes(request("GET", QStringLiteral("/v1/sources")).body);
}

Json SourceApi::syncState()
{
    return Py::loadsBytes(request("GET", QStringLiteral("/v2/sync/state")).body);
}

QSet<QString> SourceApi::resolvedConflictIds()
{
    const Json rows = Py::loadsBytes(request("GET", QStringLiteral("/v1/conflicts?status=resolved")).body);
    QSet<QString> ids;
    for (const Json &item : requireList(rows)) {
        ids.insert(Py::hashKey(Py::subscript(item, QStringLiteral("id"))));
    }
    return ids;
}

QList<Json> SourceApi::sourceConflicts(const QString &status)
{
    return requireList(Py::loadsBytes(request("GET", QStringLiteral("/v1/conflicts?status=") + Py::quote(status, QString())).body));
}

Json SourceApi::discardSourceConflict(const QString &conflictId, const Json &revision, const QString &digest)
{
    const Json payload = Json::object({{QStringLiteral("expected_revision"), revision}, {QStringLiteral("expected_sha256"), Json(digest)}});
    return Py::loadsBytes(request("POST", QStringLiteral("/v1/conflicts/") + Py::quote(requireConflictId(conflictId), QString()) + QStringLiteral("/discard"), payload).body);
}

Json SourceApi::postJson(const QString &path, const Json &payload, const QString &category)
{
    const QByteArray raw = request("POST", path, payload).body;
    recordPayloadUpload(category, payload);
    return Py::loadsBytes(raw);
}

Json SourceApi::apply(const Json &payload)
{
    return postJson(QStringLiteral("/v1/sources/apply"), payload, QStringLiteral("source_apply"));
}

Json SourceApi::deviceSequence()
{
    return Py::loadsBytes(request("GET", QStringLiteral("/v1/device-sequence")).body);
}

Json SourceApi::publish(const Json &payload)
{
    return postJson(QStringLiteral("/v1/source-publications"), payload, QStringLiteral("source_publication"));
}

Json SourceApi::publishBuildInputs(const Json &payload)
{
    return postJson(QStringLiteral("/v1/build-input-snapshots"), payload, QStringLiteral("build_input_snapshot"));
}

Json SourceApi::latestBuildInputIdentities()
{
    return Py::loadsBytes(request("GET", QStringLiteral("/v1/build-input-snapshots/latest-identities")).body);
}

std::optional<Json> SourceApi::latestBuildInputs(const QString &target)
{
    try {
        return Py::loadsBytes(request("GET", QStringLiteral("/v1/build-input-snapshots/latest?target=") + Py::quote(target, QString())).body);
    } catch (const Py::Exception &error) {
        if (error.type == Py::apiErrorType && error.status == 404) {
            return std::nullopt;
        }
        throw;
    }
}

Json SourceApi::requestBuild(const QString &target, const Json &sourceFrontier, const std::optional<Json> &inputSnapshotSha256,
                             const std::optional<QString> &requestId)
{
    Json payload = Json::object({{QStringLiteral("target"), Json(target)}, {QStringLiteral("source_frontier"), sourceFrontier}});
    if (inputSnapshotSha256) {
        payload.insert(QStringLiteral("input_snapshot_sha256"), *inputSnapshotSha256);
    }
    if (requestId) {
        payload.insert(QStringLiteral("request_id"), Json(*requestId));
    }
    return postJson(QStringLiteral("/v1/builds"), payload, QStringLiteral("build_request"));
}

Json SourceApi::requestRenderCheck(const QString &target)
{
    return postJson(QStringLiteral("/v1/render-checks"), Json::object({{QStringLiteral("target"), Json(target)}}), QStringLiteral("render_check"));
}

Json SourceApi::renderStatus(const QString &target)
{
    return Py::loadsBytes(request("GET", QStringLiteral("/v1/render-status?target=") + Py::quote(target, QString())).body);
}

Json SourceApi::build(const QString &buildId)
{
    return Py::loadsBytes(request("GET", QStringLiteral("/v1/builds/") + Py::quote(buildId, QString())).body);
}

Json SourceApi::buildOutput(const QString &buildId, qint64 offset)
{
    return Py::loadsBytes(request("GET", QStringLiteral("/v1/builds/%1/output?offset=%2").arg(Py::quote(buildId, QString())).arg(offset)).body);
}

}
