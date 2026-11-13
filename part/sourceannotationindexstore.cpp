/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sourceannotationindex.h"
#include "sourceowner.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>
#include <QSaveFile>

namespace Thoughts {
namespace {
QMutex indexMutex;
QHash<QString, QJsonObject> indexes, sourceEvidence;
QString indexPath(const QString &root) { return QDir(root).filePath(QStringLiteral(".runtime/source-location-index.json")); }
}

QJsonObject sourceLocationIndex(const QString &root)
{
    QMutexLocker lock(&indexMutex);
    if (!indexes.contains(root)) {
        QFile file(indexPath(root));
        auto index = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
        if (index.value(QStringLiteral("schema_version")).toInt() != 1) index = {};
        // Loaded locations are unverified until the first complete scan.
        if (!index.isEmpty()) index.insert(QStringLiteral("refreshing"), true);
        index.insert(QStringLiteral("projects"), QJsonObject());
        indexes.insert(root, index);
    }
    return indexes.value(root);
}


void installSourceLocationIndex(const QString &root, const QJsonObject &index, const QJsonObject &scannedFiles)
{
    QMutexLocker lock(&indexMutex);
    auto verified = index;
    verified.remove(QStringLiteral("refreshing"));
    indexes.insert(root, verified);
    sourceEvidence.insert(root, scannedFiles);
    QDir().mkpath(QFileInfo(indexPath(root)).absolutePath());
    QSaveFile file(indexPath(root));
    const auto bytes = QJsonDocument(index).toJson(QJsonDocument::Compact);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
}

QJsonArray reviewSourceEvidence(const QString &root, const QString &id, const QString &project, const QString &sourceFile)
{
    QMutexLocker lock(&indexMutex);
    QJsonArray result;
    const auto evidence = sourceEvidence.value(root);
    const bool hasPreferred = evidence.value(sourceFile).toObject().value(QStringLiteral("reviews")).toObject().contains(id);
    for (auto it = evidence.begin(); it != evidence.end(); ++it) {
        if (hasPreferred && it.key() != sourceFile) continue;
        if (SourceOwner::of(it.key()) != project) continue;
        for (const auto &value : it.value().toObject().value(QStringLiteral("reviews")).toObject().value(id).toArray()) result.append(value);
    }
    return result;
}

void discardSourceLocationIndex(const QString &root)
{
    QMutexLocker lock(&indexMutex);
    indexes.remove(root);
    sourceEvidence.remove(root);
}

void invalidateSourceLocationIndex(const QString &root)
{
    sourceLocationIndex(root);
    QMutexLocker lock(&indexMutex);
    auto index = indexes.value(root);
    // Keep mutation checks conservative, but distinguish an in-flight scan from
    // a completed scan that actually found missing source locations.
    index.insert(QStringLiteral("projects"), QJsonObject());
    index.insert(QStringLiteral("refreshing"), true);
    indexes.insert(root, index);
}
}
