/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_VERIFIEDARTIFACTCACHE_H
#define THOUGHTS_VERIFIEDARTIFACTCACHE_H

#include <QCache>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
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

// ---- What is kept in memory.
//
// Artifact bytes and parsed source artifacts are kept so that the Library can
// be listed again without reading and hashing every file. Each of the two
// caches is bounded in bytes and forgets the least recently used entries first.

// The bytes of verified artifacts that a caller asked for.
inline constexpr qint64 VerifiedArtifactBytesLimit = 32 * 1024 * 1024;
// An artifact larger than this is never kept: it would push out many small ones.
inline constexpr qint64 VerifiedArtifactLargestKept = 4 * 1024 * 1024;
// Parsed source artifacts, by the memory they take.
inline constexpr qint64 SourceArtifactJsonBytesLimit = 32 * 1024 * 1024;
// A parsed JSON object takes about this many times its compact text (measured
// on 386 source artifacts of one Library: 4.04).
inline constexpr int ParsedJsonHeapFactor = 4;
// File identities whose bytes already matched their digest.
inline constexpr int VerifiedArtifactIdentityLimit = 65536;

// The sections of a source artifact that sourceArtifactJson() leaves out: the
// plan of the basic review cards (pdf.basic_review) and the review card
// anchors. They are nine tenths of an artifact's size and nothing reads them
// from the parsed copy (ReviewDocumentTools reads the plan from the file).
// Kept, they made the parsed artifacts of one Library 186 MiB instead of 5.
inline constexpr char SourceArtifactOmittedPdfSection[] = "basic_review";
inline constexpr char SourceArtifactOmittedSection[] = "review_card_anchors";

// This header is compiled into the application and into the part's library.
// Their caches are one and the same: without this each binary kept its own.
#if defined(Q_OS_WIN)
#define THOUGHTS_ARTIFACT_CACHE_SHARED
#else
#define THOUGHTS_ARTIFACT_CACHE_SHARED __attribute__((visibility("default")))
#endif

struct THOUGHTS_ARTIFACT_CACHE_SHARED VerifiedArtifactCacheState {
    QMutex mutex;
    // Keyed by file identity and digest; the cost is the size in bytes.
    QCache<QString, QByteArray> bytes {VerifiedArtifactBytesLimit};
    QSet<QString> verified;
    // Keyed by digest; the cost is the estimated memory in bytes.
    QCache<QString, QJsonObject> json {SourceArtifactJsonBytesLimit};
    // What each owner (a PDF's filename) had cached last: a rebuilt PDF has
    // another artifact, and the one before it is of no use any more.
    QHash<QString, QString> bytesKeyOfOwner;
    QHash<QString, QString> jsonDigestOfOwner;
    qint64 bytesHits = 0;
    qint64 bytesMisses = 0;
    qint64 jsonHits = 0;
    qint64 jsonMisses = 0;
};

THOUGHTS_ARTIFACT_CACHE_SHARED inline VerifiedArtifactCacheState &verifiedArtifactCacheState()
{
    static VerifiedArtifactCacheState state;
    return state;
}

// What the caches hold now, for the developer log and for tests.
struct VerifiedArtifactCacheStatistics {
    qint64 bytesEntries = 0;
    qint64 bytesKept = 0;
    qint64 jsonEntries = 0;
    // Estimated memory of the parsed artifacts.
    qint64 jsonBytesKept = 0;
    qint64 verifiedIdentities = 0;
    qint64 bytesHits = 0;
    qint64 bytesMisses = 0;
    qint64 jsonHits = 0;
    qint64 jsonMisses = 0;
};

inline VerifiedArtifactCacheStatistics verifiedArtifactCacheStatistics()
{
    VerifiedArtifactCacheState &state = verifiedArtifactCacheState();
    QMutexLocker lock(&state.mutex);
    return {state.bytes.size(), state.bytes.totalCost(), state.json.size(), state.json.totalCost(), state.verified.size(), state.bytesHits, state.bytesMisses, state.jsonHits, state.jsonMisses};
}

// Forgets everything kept (tests). Files are verified again when next read.
inline void clearVerifiedArtifactCaches()
{
    VerifiedArtifactCacheState &state = verifiedArtifactCacheState();
    QMutexLocker lock(&state.mutex);
    state.bytes.clear();
    state.verified.clear();
    state.json.clear();
    state.bytesKeyOfOwner.clear();
    state.jsonDigestOfOwner.clear();
    state.bytesHits = state.bytesMisses = state.jsonHits = state.jsonMisses = 0;
}

namespace VerifiedArtifactCacheDetail
{
// The entry `owner` had before is dropped when it gets another one.
template<typename Cache>
inline void supersede(Cache &cache, QHash<QString, QString> &keyOfOwner, const QString &owner, const QString &key)
{
    if (owner.isEmpty()) {
        return;
    }
    const auto previous = keyOfOwner.constFind(owner);
    if (previous != keyOfOwner.cend() && *previous != key) {
        cache.remove(*previous);
    }
    if (keyOfOwner.size() >= 8192 && !keyOfOwner.contains(owner)) {
        keyOfOwner.clear();
    }
    keyOfOwner.insert(owner, key);
}
}

namespace VerifiedArtifactCacheDetail
{
inline bool readVerified(const QString &path, const QString &digest, QByteArray *contents, const QString &owner, bool keepBytes)
{
    VerifiedArtifactCacheState &state = verifiedArtifactCacheState();
    const QString identity = artifactFileIdentity(path);
    if (identity.isEmpty() || digest.size() != 64) return false;
    const QString key = identity + QLatin1Char(':') + digest;
    bool knownGood = false;
    {
        QMutexLocker lock(&state.mutex);
        if (const auto *cached = state.bytes.object(key)) {
            ++state.bytesHits;
            if (contents) *contents = *cached;
            return true;
        }
        // The identity changes on any rewrite, so an evicted or uncached
        // artifact can be read again without hashing it again.
        knownGood = state.verified.contains(key);
    }
    if (knownGood && !contents) return true;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024 * 1024) return false;
    const QByteArray raw = file.readAll();
    if (artifactFileIdentity(path) != identity || (!knownGood && artifactBytesSha256(raw) != digest.toLatin1())) return false;
    if (contents) *contents = raw;
    QMutexLocker lock(&state.mutex);
    ++state.bytesMisses;
    if (state.verified.size() >= VerifiedArtifactIdentityLimit) state.verified.clear();
    state.verified.insert(key);
    if (keepBytes && raw.size() <= VerifiedArtifactLargestKept) {
        supersede(state.bytes, state.bytesKeyOfOwner, owner, key);
        state.bytes.insert(key, new QByteArray(raw), qMax<qsizetype>(1, raw.size()));
    }
    return true;
}
}

// Whether the file at `path` has the SHA-256 `digest`, and its bytes when
// `contents` is given.
//
// Only a caller that asks for the bytes makes them stay in memory: a call that
// only verifies reads the file once, remembers that its identity is good, and
// keeps nothing else. `owner` names what the artifact belongs to (a PDF's
// filename); the bytes kept for the same owner before are dropped.
inline bool readVerifiedArtifact(const QString &path, const QString &digest, QByteArray *contents = nullptr, const QString &owner = {})
{
    return VerifiedArtifactCacheDetail::readVerified(path, digest, contents, owner, contents != nullptr);
}

namespace VerifiedArtifactCacheDetail
{
inline QJsonObject parsedSourceArtifact(const QByteArray &verifiedContents)
{
    QJsonObject result = QJsonDocument::fromJson(verifiedContents).object();
    result.remove(QLatin1String(SourceArtifactOmittedSection));
    const auto pdf = result.constFind(QLatin1String("pdf"));
    if (pdf != result.constEnd() && pdf->isObject() && pdf->toObject().contains(QLatin1String(SourceArtifactOmittedPdfSection))) {
        QJsonObject slim = pdf->toObject();
        slim.remove(QLatin1String(SourceArtifactOmittedPdfSection));
        result.insert(QLatin1String("pdf"), slim);
    }
    return result;
}

inline void keepSourceArtifact(VerifiedArtifactCacheState &state, const QString &digest, const QJsonObject &result, const QString &owner)
{
    if (result.isEmpty()) {
        return;
    }
    const qsizetype cost = qMax<qsizetype>(1, QJsonDocument(result).toJson(QJsonDocument::Compact).size() * ParsedJsonHeapFactor);
    QMutexLocker lock(&state.mutex);
    supersede(state.json, state.jsonDigestOfOwner, owner, digest);
    state.json.insert(digest, new QJsonObject(result), cost);
}
}

// The source artifact with the SHA-256 `digest`, parsed, without the sections
// named above (SourceArtifactOmitted...). `verifiedContents` must be its
// verified bytes. `owner` as for readVerifiedArtifact().
inline QJsonObject sourceArtifactJson(const QByteArray &verifiedContents, const QString &digest, const QString &owner = {})
{
    VerifiedArtifactCacheState &state = verifiedArtifactCacheState();
    {
        QMutexLocker lock(&state.mutex);
        if (const auto *cached = state.json.object(digest)) {
            ++state.jsonHits;
            return *cached;
        }
        ++state.jsonMisses;
    }
    // Parsed outside the lock: another artifact's reader can proceed.
    const QJsonObject result = VerifiedArtifactCacheDetail::parsedSourceArtifact(verifiedContents);
    VerifiedArtifactCacheDetail::keepSourceArtifact(state, digest, result, owner);
    return result;
}

// The same for the artifact file at `path`: verified and parsed in one step.
// Its bytes are read only when the parsed artifact is not kept already, and are
// not kept themselves. An empty object when the file does not verify.
inline QJsonObject verifiedSourceArtifactJson(const QString &path, const QString &digest, const QString &owner = {})
{
    VerifiedArtifactCacheState &state = verifiedArtifactCacheState();
    QJsonObject kept;
    {
        QMutexLocker lock(&state.mutex);
        if (const auto *cached = state.json.object(digest)) {
            kept = *cached;
        }
    }
    if (!kept.isEmpty()) {
        // Still there, and still the file that was verified.
        if (!readVerifiedArtifact(path, digest)) {
            return {};
        }
        QMutexLocker lock(&state.mutex);
        ++state.jsonHits;
        return kept;
    }
    QByteArray raw;
    if (!VerifiedArtifactCacheDetail::readVerified(path, digest, &raw, QString(), false)) {
        return {};
    }
    {
        QMutexLocker lock(&state.mutex);
        ++state.jsonMisses;
    }
    const QJsonObject result = VerifiedArtifactCacheDetail::parsedSourceArtifact(raw);
    VerifiedArtifactCacheDetail::keepSourceArtifact(state, digest, result, owner);
    return result;
}
}
#endif
