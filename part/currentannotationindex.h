/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_CURRENTANNOTATIONINDEX_H
#define THOUGHTS_CURRENTANNOTATIONINDEX_H

#include "verifiedartifactcache.h"
#include "currentannotationstore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

namespace Thoughts::CurrentAnnotationIndex
{
inline QString filename(const QString &stateRoot)
{
    return QDir(stateRoot).filePath(QStringLiteral("current-annotation-index.json"));
}

struct CachedIndex {
    QMutex mutex;
    QString repositoryIdentity;
    QString indexIdentity;
    QJsonObject contents;
    // Serialized projections of the last write; most state writes leave them unchanged.
    QJsonObject projectionsSource;
    QByteArray projectionsJson;
};


inline CachedIndex &cache()
{
    static CachedIndex value;
    return value;
}

// The exact Compact JSON Qt produces for the complete index, reusing the serialized
// projections when they are unchanged (~4 MB per state write otherwise).
inline QByteArray indexJson(const QJsonObject &indexWithoutProjections, const QJsonObject &projections)
{
    QByteArray projectionsJson;
    {
        QMutexLocker guard(&cache().mutex);
        // Equal-and-shared objects compare in O(1).
        if (!cache().projectionsJson.isNull() && cache().projectionsSource == projections) projectionsJson = cache().projectionsJson;
    }
    if (projectionsJson.isNull()) {
        projectionsJson = QJsonDocument(projections).toJson(QJsonDocument::Compact);
        QMutexLocker guard(&cache().mutex);
        cache().projectionsSource = projections;
        cache().projectionsJson = projectionsJson;
    }
    QJsonObject placeholder = indexWithoutProjections;
    placeholder.insert(QStringLiteral("projections"), QJsonObject());
    QByteArray json = QJsonDocument(placeholder).toJson(QJsonDocument::Compact);
    // String values escape their quotes, so this marker can only be the key.
    static const QByteArray marker("\"projections\":{}");
    const qsizetype at = json.indexOf(marker);
    if (at < 0) {
        placeholder.insert(QStringLiteral("projections"), projections);
        return QJsonDocument(placeholder).toJson(QJsonDocument::Compact);
    }
    json.replace(at + marker.size() - 2, 2, projectionsJson);
    return json;
}

inline bool read(const QString &stateRoot, QJsonObject *current)
{
    const QString repository = QDir(stateRoot).filePath(QStringLiteral("repository.json"));
    const QString identity = artifactFileIdentity(repository);
    if (identity.isEmpty()) return false;
    const QString indexPath = filename(stateRoot);
    const QString indexIdentity = artifactFileIdentity(indexPath);
    {
        QMutexLocker guard(&cache().mutex);
        if (!indexIdentity.isEmpty() && cache().repositoryIdentity == identity && cache().indexIdentity == indexIdentity) {
            *current = cache().contents;
            return true;
        }
    }
    QFile file(indexPath);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const auto document = QJsonDocument::fromJson(file.readAll());
    if (!document.isObject()) return false;
    QJsonObject index = document.object();
    if (index.value(QStringLiteral("schema_version")).toInt() != 2 || index.contains(QStringLiteral("projections"))) return false;
    CurrentAnnotationStore store(stateRoot);
    QHash<QString, QJsonObject> records;
    if (!store.read(QStringLiteral("projection"), &records)) return false;
    QJsonObject projections;
    for (auto it = records.cbegin(); it != records.cend(); ++it) projections.insert(it.key(), it.value());
    index.insert(QStringLiteral("projections"), projections);
    QJsonObject unsignedIndex = index;
    unsignedIndex.remove(QStringLiteral("index_sha256"));
    if (index.value(QStringLiteral("schema_version")).toInt() != 2
        || index.value(QStringLiteral("repository_identity")).toString() != identity
        || index.value(QStringLiteral("index_sha256")).toString() != QString::fromLatin1(artifactBytesSha256(
               QJsonDocument(unsignedIndex).toJson(QJsonDocument::Compact)))
        || !index.value(QStringLiteral("projections")).isObject()
        || !index.value(QStringLiteral("resolved_agent_proposals")).isObject()
        || artifactFileIdentity(repository) != identity || artifactFileIdentity(indexPath) != indexIdentity) return false;
    *current = index;
    QMutexLocker guard(&cache().mutex);
    cache().repositoryIdentity = identity;
    cache().indexIdentity = indexIdentity;
    cache().contents = index;
    return true;
}

inline bool write(const QString &stateRoot, const QJsonObject &repositoryState, bool replaceProjectionRecords = true)
{
    const QString repository = QDir(stateRoot).filePath(QStringLiteral("repository.json"));
    const QString identity = artifactFileIdentity(repository);
    if (identity.isEmpty()) return false;
    const QJsonObject projections = repositoryState.value(QStringLiteral("projections")).toObject();
    // Share the caller's projections (no copy) unless retired history must be filtered out.
    static const QLatin1String retired("proposal_event\nproposals\n");
    QJsonObject currentProjections = projections;
    bool hasRetired = false;
    for (auto it = projections.constBegin(); it != projections.constEnd() && !hasRetired; ++it) hasRetired = it.key().startsWith(retired);
    if (hasRetired) {
        for (auto it = currentProjections.begin(); it != currentProjections.end();) {
            if (it.key().startsWith(retired)) it = currentProjections.erase(it);
            else ++it;
        }
    }
    CurrentAnnotationStore store(stateRoot);
    if (replaceProjectionRecords && !store.replace(QStringLiteral("projection"), currentProjections)) return false;
    QJsonObject index {{QStringLiteral("schema_version"), 2},
        {QStringLiteral("repository_identity"), identity},
        {QStringLiteral("bootstrapped"), repositoryState.value(QStringLiteral("bootstrapped"))},
        {QStringLiteral("resolved_agent_proposals"), repositoryState.value(QStringLiteral("resolved_agent_proposals")).toObject()}};
    index.insert(QStringLiteral("index_sha256"), QString::fromLatin1(artifactBytesSha256(indexJson(index, currentProjections))));
    QJsonObject completeIndex = index;
    completeIndex.insert(QStringLiteral("projections"), currentProjections);
    QSaveFile file(filename(stateRoot));
    const QByteArray bytes = QJsonDocument(index).toJson(QJsonDocument::Compact);
    if (!(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()
        && artifactFileIdentity(repository) == identity && file.commit())) return false;
    QMutexLocker guard(&cache().mutex);
    cache().repositoryIdentity = identity;
    cache().indexIdentity = artifactFileIdentity(filename(stateRoot));
    cache().contents = completeIndex;
    return true;
}
}

#endif
