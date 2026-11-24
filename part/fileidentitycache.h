// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef THOUGHTS_FILEIDENTITYCACHE_H
#define THOUGHTS_FILEIDENTITYCACHE_H

#include "verifiedartifactcache.h"

#include <QByteArray>
#include <QCache>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QMutex>
#include <QMutexLocker>
#include <QString>

#include <optional>

namespace Thoughts
{
// ---- Files read again only when they changed.
//
// A Library load reads several thousand small files (the Library index, the
// comment sidecars, the source pointers, project settings, sources). Most are
// the same as the last time, and on a computer that scans every file opened
// each of those reads costs far more than the read itself. The caches below
// keep what was read from each file, keyed by its path, and use it again while
// the file's identity (artifactFileIdentity(): device, inode, size,
// modification and status-change time in nanoseconds) is unchanged: an atomic
// replacement, an edit in place and a copy over it all change the identity. A
// file that changed while it was read is not kept.

// The files read and the reads saved are counted in fileReadCounts()
// (verifiedartifactcache.h).

// Values derived from one file each, at most `maximumCost` of them by cost.
template<typename T>
class FileIdentityCache
{
public:
    explicit FileIdentityCache(qsizetype maximumCost)
        : m_cache(maximumCost)
    {
    }

    // The value for the file at `path`: kept, or `read(path)` when the file is
    // new or changed. `cost(value)` weighs a value against the maximum; one
    // that costs more than the maximum is not kept. A missing file is never
    // kept: `read` is called for it each time, or `missing` is returned when
    // given (what `read` returns for a missing file, without trying to open it).
    template<typename Read, typename Cost>
    T value(const QString &path, Read read, Cost cost, bool *kept = nullptr, const std::optional<T> &missing = std::nullopt)
    {
        const QString identity = artifactFileIdentity(path);
        if (identity.isEmpty() && missing && !QFileInfo::exists(path) && !QFileInfo(path).isSymLink()) {
            if (kept) *kept = false;
            return *missing;
        }
        if (!identity.isEmpty()) {
            QMutexLocker lock(&m_mutex);
            if (const Entry *entry = m_cache.object(path); entry && entry->identity == identity) {
                ++fileReadCounts().hits;
                if (kept) *kept = true;
                return entry->value;
            }
        }
        if (kept) *kept = false;
        if (!identity.isEmpty()) ++fileReadCounts().reads;
        T result = read(path);
        const bool unchanged = !identity.isEmpty() && artifactFileIdentity(path) == identity;
        QMutexLocker lock(&m_mutex);
        if (unchanged) {
            m_cache.insert(path, new Entry {identity, result}, qMax<qsizetype>(1, cost(result)));
        } else {
            m_cache.remove(path);
        }
        return result;
    }

    template<typename Read>
    T value(const QString &path, Read read, bool *kept = nullptr)
    {
        return value(path, read, [](const T &) { return qsizetype(1); }, kept);
    }

    void clear()
    {
        QMutexLocker lock(&m_mutex);
        m_cache.clear();
    }

private:
    struct Entry {
        QString identity;
        T value;
    };
    QMutex m_mutex;
    QCache<QString, Entry> m_cache;
};

// A JSON file as read: whether it could be opened, its parse status, and the
// document.
struct CachedJsonFile {
    bool opened = false;
    QJsonParseError::ParseError error = QJsonParseError::NoError;
    QJsonDocument document;
    qint64 bytes = 0;

    bool valid() const
    {
        return opened && error == QJsonParseError::NoError;
    }
    // The top-level object; empty when the file is missing or not an object.
    QJsonObject object() const
    {
        return document.object();
    }
};

// Parsed JSON takes about ParsedJsonHeapFactor times its text; 64 MiB keeps
// every small JSON file a Library load reads (measured: 11 MiB of text).
inline constexpr qsizetype CachedJsonFilesLimit = 64 * 1024 * 1024;
// Raw bytes kept, e.g. the sources whose agent proposals are observed.
inline constexpr qsizetype CachedFileBytesLimit = 48 * 1024 * 1024;
// A file larger than this is read each time instead of pushing out many small ones.
inline constexpr qint64 CachedFileLargestKept = 8 * 1024 * 1024;

THOUGHTS_ARTIFACT_CACHE_SHARED inline FileIdentityCache<CachedJsonFile> &cachedJsonFiles()
{
    static FileIdentityCache<CachedJsonFile> cache(CachedJsonFilesLimit);
    return cache;
}

THOUGHTS_ARTIFACT_CACHE_SHARED inline FileIdentityCache<std::optional<QByteArray>> &cachedFileContents()
{
    static FileIdentityCache<std::optional<QByteArray>> cache(CachedFileBytesLimit);
    return cache;
}

// The JSON file at `path`, parsed; read again only when it changed.
inline CachedJsonFile cachedJsonFile(const QString &path)
{
    return cachedJsonFiles().value(
        path,
        [](const QString &file) {
            CachedJsonFile result;
            QFile input(file);
            if (!input.open(QIODevice::ReadOnly)) return result;
            result.opened = true;
            const QByteArray bytes = input.readAll();
            result.bytes = bytes.size();
            QJsonParseError error;
            result.document = QJsonDocument::fromJson(bytes, &error);
            result.error = error.error;
            return result;
        },
        [](const CachedJsonFile &result) {
            return result.bytes > CachedFileLargestKept ? CachedJsonFilesLimit + 1 : qsizetype(result.bytes) * ParsedJsonHeapFactor;
        },
        nullptr, CachedJsonFile {});
}

// The bytes of the file at `path`, or nothing when it cannot be read; read
// again only when it changed.
inline std::optional<QByteArray> cachedFileBytes(const QString &path)
{
    return cachedFileContents().value(
        path,
        [](const QString &file) -> std::optional<QByteArray> {
            QFile input(file);
            if (!input.open(QIODevice::ReadOnly)) return std::nullopt;
            return input.readAll();
        },
        [](const std::optional<QByteArray> &bytes) {
            const qsizetype size = bytes ? bytes->size() : 0;
            return size > CachedFileLargestKept ? CachedFileBytesLimit + 1 : size;
        },
        nullptr, std::optional<std::optional<QByteArray>>(std::optional<QByteArray>()));
}

// Forgets everything kept (tests).
inline void clearFileIdentityCaches()
{
    cachedJsonFiles().clear();
    cachedFileContents().clear();
}
}
#endif
