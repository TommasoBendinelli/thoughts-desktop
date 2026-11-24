// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>

#include <functional>

class QNetworkReply;

namespace Thoughts
{
class SyncClient;

// Earlier versions of Library PDFs, for opening the PDF a review or a piece of feedback was given
// on (the History of a review card). Every feedback record names its version by the SHA-256 of
// the PDF bytes (FeedbackVersion, "review_version"; answer feedback also anchor.pdf.pdf_sha256),
// which for a Library PDF is the server's PDF object, and the server keeps every stamped version.
// A version is copied once into <application data>/pdf-versions/<sha256>.pdf and opened from
// there: outside the Library folder and under a name no Library document has, so the reader
// treats it as a plain PDF (no comment sync, no annotations, no review masks, no Sync).
namespace PdfVersions
{
// The PDF a record was made on: its review_version, else the PDF its answer-feedback anchor was
// made on. Empty when the record names none (records older than the stamps).
QString recordedSha256(const QJsonObject &record);

QString defaultCacheRoot();
QString cachePath(const QString &cacheRoot, const QString &sha256);
// Whether @p path is a version in the cache (under @p cacheRoot, the default when empty).
bool isCachedVersion(const QString &path, const QString &cacheRoot = {});

// A PDF with these bytes still on this computer: a Library generation in @p docsRoot whose index
// lists the SHA-256 (the current one and the earlier ones kept next to it). Empty when none.
// The bytes are verified when they are copied into the cache.
QString localCopy(const QString &docsRoot, const QString &sha256);

// Writes @p contents to the cache as the version @p sha256, after checking they are those bytes,
// and returns its path; empty (with @p error) otherwise. At most maximumCachedVersions are kept.
constexpr int maximumCachedVersions = 64;
QString store(const QString &cacheRoot, const QString &sha256, const QByteArray &contents, QString *error = nullptr);

// The title the reader shows for a cached version (kept next to it so a restored tab has it too).
void setTitle(const QString &path, const QString &title);
QString title(const QString &path);

enum class Failure {
    None,
    NotConnected, // no server address or device token on this computer
    NotFound,     // the server no longer has the version (pruned)
    Unreachable,  // offline, refused, timed out
    Mismatch,     // the bytes were not the version asked for
    Failed,       // anything else (detail says what)
};

struct Result {
    QString path;
    Failure failure = Failure::None;
    QString detail;
};

// Gets versions into the cache: a cache hit, else a local copy, else GET /v1/objects/{sha256}
// from the Thoughts server. Hashing, copying and writing run off the calling thread; the network
// request is asynchronous. Requests for one version share one fetch.
class Fetcher : public QObject
{
    Q_OBJECT

public:
    // Starts the download of @p sha256, or returns nullptr with @p failure set. The default asks
    // the configured server with this computer's device token, as the other client requests do.
    using Download = std::function<QNetworkReply *(const QString &sha256, Failure *failure)>;
    using Done = std::function<void(const Result &result)>;

    explicit Fetcher(QString docsRoot, QString cacheRoot = {}, Download download = {}, QObject *parent = nullptr);
    ~Fetcher() override;

    QString cacheRoot() const
    {
        return m_cacheRoot;
    }
    // Calls @p done later on this thread (never from inside fetch()), unless @p context is gone.
    void fetch(const QString &sha256, QObject *context, Done done);

private:
    void download(const QString &sha256);
    void finish(const QString &sha256, const Result &result);
    QNetworkReply *serverDownload(const QString &sha256, Failure *failure);

    QString m_docsRoot;
    QString m_cacheRoot;
    Download m_download;
    SyncClient *m_client = nullptr;
    quint64 m_clientGeneration = 0;
    QHash<QString, QList<QPair<QPointer<QObject>, Done>>> m_waiting;
};
}
}
