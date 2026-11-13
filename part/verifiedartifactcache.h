/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_VERIFIEDARTIFACTCACHE_H
#define THOUGHTS_VERIFIEDARTIFACTCACHE_H

#include <QCache>
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSet>
#ifdef Q_OS_WIN
#include "thoughtsplatform.h"
#else
#include <sys/stat.h>
#endif
#ifdef Q_OS_MACOS
#include <CommonCrypto/CommonDigest.h>
#endif

namespace Thoughts {
// Use the platform's accelerated SHA-256 while retaining full byte verification.
// Other platforms keep Qt's implementation and identical digest semantics.
inline QByteArray artifactFileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
#ifdef Q_OS_MACOS
    CC_SHA256_CTX context;
    if (CC_SHA256_Init(&context) != 1) return {};
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    for (;;) {
        const qint64 count = file.read(buffer.data(), buffer.size());
        if (count < 0) return {};
        if (count == 0) break;
        if (CC_SHA256_Update(&context, buffer.constData(), static_cast<CC_LONG>(count)) != 1) return {};
    }
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    if (CC_SHA256_Final(digest, &context) != 1) return {};
    return QByteArray(reinterpret_cast<const char *>(digest), sizeof(digest)).toHex();
#else
    QCryptographicHash hash(QCryptographicHash::Sha256);
    return hash.addData(&file) ? hash.result().toHex() : QByteArray();
#endif
}
// File identity includes ctime and inode: same-length rewrites and atomic
// replacements invalidate verification even if a writer preserves mtime.
inline QString artifactFileIdentity(const QString &path)
{
#ifdef Q_OS_WIN
    return Platform::fileStamp(path);
#else
    struct stat s;
    if (::stat(QFile::encodeName(path).constData(), &s) || !S_ISREG(s.st_mode)) return {};
#ifdef Q_OS_MACOS
    const auto mt = s.st_mtimespec;
    const auto ct = s.st_ctimespec;
#else
    const auto mt = s.st_mtim;
    const auto ct = s.st_ctim;
#endif
    return path + QStringLiteral(":%1:%2:%3:%4:%5:%6:%7")
        .arg(s.st_dev).arg(s.st_ino).arg(s.st_size).arg(mt.tv_sec).arg(mt.tv_nsec).arg(ct.tv_sec).arg(ct.tv_nsec);
#endif
}

inline QByteArray artifactBytesSha256(const QByteArray &bytes)
{
#ifdef Q_OS_MACOS
    unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_CTX context;
    if (CC_SHA256_Init(&context) != 1) return {};
    for (qsizetype offset = 0; offset < bytes.size();) {
        const auto count = static_cast<CC_LONG>(qMin<qsizetype>(bytes.size() - offset, 64 * 1024 * 1024));
        if (CC_SHA256_Update(&context, bytes.constData() + offset, count) != 1) return {};
        offset += count;
    }
    if (CC_SHA256_Final(digest, &context) != 1) return {};
    return QByteArray(reinterpret_cast<const char *>(digest), sizeof(digest)).toHex();
#else
    return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
#endif
}

inline bool readVerifiedArtifact(const QString &path, const QString &digest, QByteArray *contents = nullptr)
{
    static QMutex mutex;
    static QCache<QString, QByteArray> cache(128 * 1024); // KiB, bounded across projects
    // File identities whose bytes already matched their digest. The identity
    // changes on any rewrite, so an evicted or uncached artifact can be read
    // again without hashing it again.
    static QSet<QString> verified;
    const QString identity = artifactFileIdentity(path);
    if (identity.isEmpty() || digest.size() != 64) return false;
    const QString key = identity + QLatin1Char(':') + digest;
    bool knownGood = false;
    {
        QMutexLocker lock(&mutex);
        if (const auto *cached = cache.object(key)) {
            if (contents) *contents = *cached;
            return true;
        }
        knownGood = verified.contains(key);
    }
    if (knownGood && !contents) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024 * 1024) return false;
    const QByteArray raw = file.readAll();
    if (artifactFileIdentity(path) != identity || (!knownGood && artifactBytesSha256(raw) != digest.toLatin1())) return false;
    if (contents) *contents = raw;
    QMutexLocker lock(&mutex);
    if (verified.size() >= 65536) verified.clear();
    verified.insert(key);
    // Large artifacts would evict many small entries on every insert.
    if (raw.size() <= 4 * 1024 * 1024) cache.insert(key, new QByteArray(raw), qMax(1, int(raw.size() / 1024)));
    return true;
}

inline QJsonObject sourceArtifactJson(const QByteArray &verifiedContents, const QString &digest)
{
    static QMutex mutex;
    static QCache<QString, QJsonObject> cache(128 * 1024);
    QMutexLocker lock(&mutex);
    if (const auto *cached = cache.object(digest)) return *cached;
    const QJsonObject result = QJsonDocument::fromJson(verifiedContents).object();
    if (!result.isEmpty()) cache.insert(digest, new QJsonObject(result), qMax(1, int(verifiedContents.size() / 1024)));
    return result;
}
}
#endif
