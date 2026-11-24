// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUuid>
#include "verifiedartifactcache.h"

namespace Thoughts {

// Paths locate bytes; only a persisted UUID identifies a document.
class DocumentIdentityStore {
    QString m_root;
    QString registryPath() const { return QDir(m_root).filePath(QStringLiteral(".thoughts/document-identities.json")); }
    static QJsonObject read(const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
    }
public:
    explicit DocumentIdentityStore(const QString &root) : m_root(root) {}
    static bool validId(const QString &id) {
        const QUuid uuid(id);
        return !uuid.isNull() && uuid.version() == QUuid::Random && uuid.toString(QUuid::WithoutBraces) == id;
    }
    QJsonObject byId(const QString &id) const {
        if (!validId(id)) return {};
        for (const auto &value : read(QDir(m_root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json"))).value(QStringLiteral("pdfs")).toArray()) {
            if (value.toObject().value(QStringLiteral("document_id")).toString() == id) return value.toObject();
        }
        return read(registryPath()).value(QStringLiteral("documents")).toObject().value(id).toObject();
    }
    QString readablePath(const QString &id) const {
        const auto entry = byId(id);
        if (entry.isEmpty()) return {};
        const auto published = QDir(m_root).filePath(QStringLiteral("current_pdfs/") + entry.value(QStringLiteral("filename")).toString());
        if (!forFile(published).isEmpty()) return published;
        const auto local = read(registryPath()).value(QStringLiteral("documents")).toObject().value(id).toObject().value(QStringLiteral("local_path")).toString();
        return !local.isEmpty() && !forFile(local).isEmpty() ? local : QString();
    }
    QJsonObject byFilename(const QString &filename) const {
        for (const auto &value : read(QDir(m_root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json"))).value(QStringLiteral("pdfs")).toArray()) {
            const auto entry = value.toObject();
            if (entry.value(QStringLiteral("filename")).toString() == filename && validId(entry.value(QStringLiteral("document_id")).toString())) return entry;
        }
        const auto documents = read(registryPath()).value(QStringLiteral("documents")).toObject();
        for (auto it = documents.begin(); it != documents.end(); ++it) {
            const auto entry = it.value().toObject();
            if (entry.value(QStringLiteral("filename")).toString() == filename && validId(it.key())) return entry;
        }
        return {};
    }
    QJsonObject forFile(const QString &path, const QString &verifiedSha = {}) const {
        const QFileInfo info(path);
        const auto entry = byFilename(info.fileName());
        if (entry.isEmpty() || !info.isFile()) return {};
        const auto parent = QDir::cleanPath(info.absolutePath());
        const auto root = QDir::cleanPath(QFileInfo(m_root).absoluteFilePath());
        const auto local = entry.value(QStringLiteral("local_path")).toString();
        const bool registeredPath = !local.isEmpty() && QFileInfo(local).canonicalFilePath() == info.canonicalFilePath();
        const bool publishedPath = parent == QDir(root).filePath(QStringLiteral("current_pdfs"))
            || (QFileInfo(parent).absolutePath() == root && QFileInfo(parent).fileName().startsWith(QLatin1String(".thoughts-library-stage-")));
        // A published copy can coexist with the original registered import copy.
        bool importedPath = registeredPath;
        if (!importedPath) {
            const auto documents = read(registryPath()).value(QStringLiteral("documents")).toObject();
            const auto stored = documents.value(entry.value(QStringLiteral("document_id")).toString()).toObject();
            const auto original = stored.value(QStringLiteral("local_path")).toString();
            importedPath = !original.isEmpty() && QFileInfo(original).canonicalFilePath() == info.canonicalFilePath();
        }
        if (!publishedPath && !importedPath) return {};
        const auto expected = entry.value(QStringLiteral("pdf_sha256")).toString();
        if (!verifiedSha.isEmpty()) return verifiedSha == expected ? entry : QJsonObject();
        return hasBytes(path, expected) ? entry : QJsonObject();
    }
    // The identity of @p path when its bytes are a version the Library published: the current one
    // (forFile), or an earlier one still in its Library generation folder
    // (.thoughts-library-stage-<revision> in the root), as that generation's own index lists it,
    // with that version's pdf_sha256. A reader tab keeps an earlier version on screen while a newer
    // one is ready. Empty when the document is no longer in the Library.
    QJsonObject forVersion(const QString &path, const QString &verifiedSha = {}) const {
        if (const auto current = forFile(path, verifiedSha); !current.isEmpty()) return current;
        const QFileInfo info(path);
        const QFileInfo generation(info.absolutePath());
        const auto root = QFileInfo(m_root).canonicalFilePath();
        if (!info.isFile() || root.isEmpty() || !generation.fileName().startsWith(QLatin1String(".thoughts-library-stage-"))
            || QFileInfo(generation.absolutePath()).canonicalFilePath() != root) return {};
        const auto documentId = byFilename(info.fileName()).value(QStringLiteral("document_id")).toString();
        if (!validId(documentId)) return {};
        for (const auto &value : read(QDir(generation.absoluteFilePath()).filePath(QStringLiteral(".thoughts-library-index.json"))).value(QStringLiteral("pdfs")).toArray()) {
            const auto entry = value.toObject();
            if (entry.value(QStringLiteral("filename")).toString() != info.fileName() || entry.value(QStringLiteral("document_id")).toString() != documentId) continue;
            const auto expected = entry.value(QStringLiteral("pdf_sha256")).toString();
            if (!verifiedSha.isEmpty()) return verifiedSha == expected ? entry : QJsonObject();
            return hasBytes(path, expected) ? entry : QJsonObject();
        }
        return {};
    }
    // Whether the file at @p path has the SHA-256 @p expected (remembered per file identity).
    static bool hasBytes(const QString &path, const QString &expected) {
        static QMutex mutex;
        static QCache<QString, bool> verified(1024);
        const auto identity = artifactFileIdentity(path);
        const auto key = identity + QLatin1Char(':') + expected;
        if (identity.isEmpty()) return false;
        { QMutexLocker guard(&mutex); if (verified.contains(key)) return true; }
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!hash.addData(&file) || artifactFileIdentity(path) != identity
            || QString::fromLatin1(hash.result().toHex()) != expected) return false;
        { QMutexLocker guard(&mutex); verified.insert(key, new bool(true)); }
        return true;
    }

    QString registerImport(const QString &filename, const QString &sha, const QString &path, QString *error) const {
        QDir().mkpath(QFileInfo(registryPath()).absolutePath());
        QLockFile lock(registryPath() + QStringLiteral(".lock"));
        if (!lock.tryLock(5000)) { if (error) *error = QStringLiteral("Document registration is busy."); return {}; }
        auto state = read(registryPath());
        if (QFileInfo::exists(registryPath()) && (state.value(QStringLiteral("schema_version")).toInt() != 1 || !state.value(QStringLiteral("documents")).isObject())) {
            if (error) *error = QStringLiteral("The saved document identities could not be read. Restore the registry before importing.");
            return {};
        }
        auto documents = state.value(QStringLiteral("documents")).toObject();
        const auto existing = byFilename(filename);
        if (!existing.isEmpty() && existing.value(QStringLiteral("pdf_sha256")).toString() != sha) {
            if (error) *error = QStringLiteral("This filename belongs to another PDF version.");
            return {};
        }
        const auto id = existing.isEmpty() ? QUuid::createUuid().toString(QUuid::WithoutBraces) : existing.value(QStringLiteral("document_id")).toString();
        documents.insert(id, QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("filename"), filename},
            {QStringLiteral("pdf_sha256"), sha}, {QStringLiteral("local_path"), QFileInfo(path).absoluteFilePath()}, {QStringLiteral("kind"), QStringLiteral("unlinked")}});
        state.insert(QStringLiteral("schema_version"), 1);
        state.insert(QStringLiteral("documents"), documents);
        const auto bytes = QJsonDocument(state).toJson();
        QSaveFile file(registryPath());
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
            if (error) *error = QStringLiteral("The document identifier could not be saved.");
            return {};
        }
        return id;
    }
};
}
