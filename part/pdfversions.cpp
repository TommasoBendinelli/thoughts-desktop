// SPDX-License-Identifier: GPL-2.0-or-later
#include "pdfversions.h"

#include "feedbackversion.h"
#include "thoughtsplatform.h"
#include "thoughtsserveraddress.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTimer>
#include <QUrl>
#include <QtConcurrent/QtConcurrentRun>

#include <thoughts/syncclient.h>

#include <algorithm>

namespace Thoughts::PdfVersions
{
namespace
{
bool validSha256(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

QString sameDirectoryKey(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

QString titlePath(const QString &pdfPath)
{
    return pdfPath.chopped(4) + QStringLiteral(".json");
}

// Keeps the newest maximumCachedVersions versions (and their titles).
void prune(const QString &cacheRoot, const QString &keep)
{
    QFileInfoList versions = QDir(cacheRoot).entryInfoList({QStringLiteral("*.pdf")}, QDir::Files, QDir::Time);
    versions.removeIf([](const QFileInfo &info) { return !validSha256(info.completeBaseName()); });
    for (qsizetype i = maximumCachedVersions; i < versions.size(); ++i) {
        const QString path = versions.at(i).absoluteFilePath();
        if (QFileInfo(path) == QFileInfo(keep)) continue;
        QFile::remove(path);
        QFile::remove(titlePath(path));
    }
}

// The cached file of @p sha256 when its bytes are that version, else a verified copy of a local
// one; empty when neither is here. Runs off the UI thread.
QString cachedOrLocal(const QString &docsRoot, const QString &cacheRoot, const QString &sha256)
{
    const QString cached = cachePath(cacheRoot, sha256);
    if (QFileInfo(cached).isFile()) {
        if (FeedbackVersion::cachedFileSha256(cached) == sha256) return cached;
        QFile::remove(cached);
    }
    const QString local = localCopy(docsRoot, sha256);
    if (local.isEmpty()) return {};
    QFile file(local);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return store(cacheRoot, sha256, file.readAll());
}
}

QString recordedSha256(const QJsonObject &record)
{
    const QString stamped = record.value(FeedbackVersion::field()).toObject().value(QStringLiteral("pdf_sha256")).toString();
    if (validSha256(stamped)) return stamped;
    const QString anchored = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString();
    return validSha256(anchored) ? anchored : QString();
}

QString defaultCacheRoot()
{
    return QDir(Platform::applicationDataRoot()).filePath(QStringLiteral("pdf-versions"));
}

QString cachePath(const QString &cacheRoot, const QString &sha256)
{
    return QDir(cacheRoot).filePath(sha256 + QStringLiteral(".pdf"));
}

bool isCachedVersion(const QString &path, const QString &cacheRoot)
{
    if (path.isEmpty()) return false;
    const QFileInfo info(path);
    if (info.suffix() != QLatin1String("pdf") || !validSha256(info.completeBaseName())) return false;
    return sameDirectoryKey(info.absolutePath()) == sameDirectoryKey(cacheRoot.isEmpty() ? defaultCacheRoot() : cacheRoot);
}

QString localCopy(const QString &docsRoot, const QString &sha256)
{
    if (docsRoot.isEmpty() || !validSha256(sha256)) return {};
    // Each Library generation lists its PDFs with their SHA-256; current_pdfs is one of them.
    const QDir root(docsRoot);
    const QStringList generations = root.entryList({QStringLiteral(".thoughts-library-stage-*")}, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot);
    for (const QString &generation : generations) {
        QFile index(root.filePath(generation + QStringLiteral("/.thoughts-library-index.json")));
        if (!index.open(QIODevice::ReadOnly)) continue;
        for (const QJsonValue &value : QJsonDocument::fromJson(index.readAll()).object().value(QStringLiteral("pdfs")).toArray()) {
            const QJsonObject entry = value.toObject();
            const QString filename = entry.value(QStringLiteral("filename")).toString();
            if (entry.value(QStringLiteral("pdf_sha256")).toString() != sha256 || filename.isEmpty() || filename.contains(QLatin1Char('/'))
                || filename.contains(QLatin1Char('\\')) || filename.startsWith(QLatin1Char('.')))
                continue;
            const QString path = root.filePath(generation + QLatin1Char('/') + filename);
            if (QFileInfo(path).isFile()) return path;
        }
    }
    return {};
}

QString store(const QString &cacheRoot, const QString &sha256, const QByteArray &contents, QString *error)
{
    if (!validSha256(sha256) || QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()) != sha256) {
        if (error) *error = QStringLiteral("The PDF bytes are not the version %1.").arg(sha256);
        return {};
    }
    if (!QDir().mkpath(cacheRoot)) {
        if (error) *error = QStringLiteral("Cannot create %1.").arg(cacheRoot);
        return {};
    }
    const QString path = cachePath(cacheRoot, sha256);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(contents) != contents.size() || !file.commit()) {
        if (error) *error = QStringLiteral("Cannot write %1: %2").arg(path, file.errorString());
        return {};
    }
    prune(cacheRoot, path);
    return path;
}

void setTitle(const QString &path, const QString &title)
{
    if (!path.endsWith(QLatin1String(".pdf"))) return;
    QSaveFile file(titlePath(path));
    if (file.open(QIODevice::WriteOnly)) {
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("title"), title}}).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

QString title(const QString &path)
{
    if (!path.endsWith(QLatin1String(".pdf"))) return {};
    QFile file(titlePath(path));
    if (!file.open(QIODevice::ReadOnly)) return {};
    return QJsonDocument::fromJson(file.read(64 * 1024)).object().value(QStringLiteral("title")).toString();
}

Fetcher::Fetcher(QString docsRoot, QString cacheRoot, Download download, QObject *parent)
    : QObject(parent)
    , m_docsRoot(std::move(docsRoot))
    , m_cacheRoot(cacheRoot.isEmpty() ? defaultCacheRoot() : std::move(cacheRoot))
    , m_download(std::move(download))
{
}

Fetcher::~Fetcher() = default;

void Fetcher::fetch(const QString &sha256, QObject *context, Done done)
{
    if (!validSha256(sha256)) {
        QTimer::singleShot(0, context ? context : this, [done = std::move(done), sha256] {
            done({{}, Failure::Failed, QStringLiteral("Not a PDF version: %1").arg(sha256)});
        });
        return;
    }
    auto &waiting = m_waiting[sha256];
    waiting.append({QPointer<QObject>(context ? context : this), std::move(done)});
    if (waiting.size() > 1) return;
    auto *watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, sha256] {
        const QString path = watcher->result();
        watcher->deleteLater();
        if (!path.isEmpty()) finish(sha256, {path, Failure::None, {}});
        else download(sha256);
    });
    watcher->setFuture(QtConcurrent::run(cachedOrLocal, m_docsRoot, m_cacheRoot, sha256));
}

QNetworkReply *Fetcher::serverDownload(const QString &sha256, Failure *failure)
{
    if (!m_client) {
        QByteArray token = qgetenv("THOUGHTS_TOKEN").trimmed();
        const QString address = ServerAddress::configured();
        if (token.isEmpty()) token = ServerAddress::storedToken(Platform::desktopDeviceAccount(), address);
        const QUrl url(ServerAddress::normalized(address));
        if (token.isEmpty() || url.isEmpty()) {
            *failure = Failure::NotConnected;
            return nullptr;
        }
        m_client = new SyncClient(url, token, this);
    }
    // Without a Sync plan: the plain object download, which every authenticated client may use.
    return m_client->downloadObject(sha256.toLatin1());
}

void Fetcher::download(const QString &sha256)
{
    Failure failure = Failure::Failed;
    QNetworkReply *reply = m_download ? m_download(sha256, &failure) : serverDownload(sha256, &failure);
    if (!reply) {
        finish(sha256, {{}, failure == Failure::None ? Failure::Failed : failure, {}});
        return;
    }
    connect(reply, &QNetworkReply::finished, this, [this, reply, sha256] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError error = reply->error();
        if (status == 404 || status == 410 || error == QNetworkReply::ContentNotFoundError || error == QNetworkReply::ContentGoneError) {
            finish(sha256, {{}, Failure::NotFound, reply->errorString()});
            return;
        }
        if (error != QNetworkReply::NoError) {
            // No HTTP answer at all: offline, refused, unknown host, timed out.
            const bool unreachable = status == 0 && error < QNetworkReply::ContentAccessDenied;
            finish(sha256, {{}, unreachable ? Failure::Unreachable : Failure::Failed, reply->errorString()});
            return;
        }
        auto *watcher = new QFutureWatcher<Result>(this);
        connect(watcher, &QFutureWatcher<Result>::finished, this, [this, watcher, sha256] {
            const Result result = watcher->result();
            watcher->deleteLater();
            finish(sha256, result);
        });
        watcher->setFuture(QtConcurrent::run([cacheRoot = m_cacheRoot, sha256, contents = reply->readAll()] {
            QString error;
            const QString path = store(cacheRoot, sha256, contents, &error);
            if (!path.isEmpty()) return Result{path, Failure::None, {}};
            const bool mismatch = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()) != sha256;
            return Result{{}, mismatch ? Failure::Mismatch : Failure::Failed, error};
        }));
    });
}

void Fetcher::finish(const QString &sha256, const Result &result)
{
    const auto waiting = m_waiting.take(sha256);
    for (const auto &[context, done] : waiting) {
        // A caller gone meanwhile (its window closed) is not called.
        if (context) done(result);
    }
}
}
