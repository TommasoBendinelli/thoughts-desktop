// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyjson.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QSet>
#include <QString>

#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;

// The Fedora source API client of thoughts_server.source_agent.SourceApi:
// bearer token, trace header that the server must echo, identity transfer
// encoding, no proxies, and failures as ApiError(status, body). Requests are
// synchronous; a QCoreApplication must exist.
namespace Thoughts::Publisher
{

class SourceApi
{
public:
    // THOUGHTS_TRACE_ID (when a UUID) becomes the trace sent with every request.
    SourceApi(const QString &baseUrl, const QString &token, int timeoutSeconds = 60);
    ~SourceApi();

    QString traceId() const { return m_traceId; }
    // Called after each counted transfer with (uploaded, downloaded, direction).
    void setTransferProgress(std::function<void(qint64, qint64, const QString &)> callback);
    QPair<qint64, qint64> transferTotals() const;

    QString uploadObject(const QByteArray &content, const QString &mediaType = QStringLiteral("text/plain; charset=utf-8"));
    bool hasObject(const QString &digest);
    QByteArray download(const QString &digest);
    Py::Json sourceTree();
    QSet<QString> resolvedConflictIds();
    QList<Py::Json> sourceConflicts(const QString &status);
    Py::Json discardSourceConflict(const QString &conflictId, const Py::Json &revision, const QString &digest);
    Py::Json apply(const Py::Json &payload);
    // GET /v1/device-sequence: this device's latest and next free sequence on Fedora.
    Py::Json deviceSequence();
    Py::Json publish(const Py::Json &payload);
    Py::Json publishBuildInputs(const Py::Json &payload);
    Py::Json latestBuildInputIdentities();
    std::optional<Py::Json> latestBuildInputs(const QString &target);
    Py::Json requestBuild(const QString &target, const Py::Json &sourceFrontier, const std::optional<Py::Json> &inputSnapshotSha256,
                          const std::optional<QString> &requestId = std::nullopt);
    Py::Json requestRenderCheck(const QString &target);
    Py::Json requestRepair(const QString &target);
    Py::Json renderStatus(const QString &target);
    Py::Json build(const QString &buildId);
    Py::Json buildOutput(const QString &buildId, qint64 offset);

    struct Response {
        int status = 0;
        QByteArray body;
    };
    // _request: raises ApiError for an HTTP error or a network failure.
    Response request(const QByteArray &method, const QString &path, const std::optional<Py::Json> &payload = std::nullopt,
                     const std::optional<QByteArray> &content = std::nullopt, const QList<QPair<QByteArray, QByteArray>> &headers = {});

private:
    QString m_baseUrl;
    QString m_token;
    int m_timeoutSeconds;
    QString m_traceId;
    std::unique_ptr<QNetworkAccessManager> m_network;
    QMap<QString, qint64> m_uploadedObjects;
    QMap<QString, qint64> m_uploadedPayloads;
    QMap<QString, qint64> m_downloadedObjects;
    std::function<void(qint64, qint64, const QString &)> m_transferProgress;

    void notifyTransferProgress(const QString &direction);
    void recordPayloadUpload(const QString &category, const Py::Json &payload);
    Py::Json postJson(const QString &path, const Py::Json &payload, const QString &category);
};

// json.dumps(payload, separators=(",", ":")): the request body encoding.
QByteArray compactJson(const Py::Json &payload);
// str(uuid.UUID(text)) for an optional environment value, or nothing.
std::optional<QString> canonicalUuid(const QString &text);

}
