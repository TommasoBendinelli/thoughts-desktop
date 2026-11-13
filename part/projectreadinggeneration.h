/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_PROJECTREADINGGENERATION_H
#define THOUGHTS_PROJECTREADINGGENERATION_H
#include "projectreading.h"
#include "verifiedartifactcache.h"
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#ifdef Q_OS_MACOS
#include <sys/clonefile.h>
#endif

namespace Thoughts {
// retainedObjectDirectories are the objects/ folders of earlier, immutable
// Library generations. A reading may keep a document version that is no
// longer published or cached; those folders still hold its verified bytes.
inline bool prepareProjectReadings(const QJsonObject &library, const QString &directory,
                                  const QString &objectDirectory, bool stage, QString *error,
                                  const QStringList &retainedObjectDirectories = {})
{
    const auto fail = [error](const QString &message) { if (error) *error = message; return false; };
    QSet<QString> filenames, projects;
    const QDir root(directory), cache(objectDirectory);
    if (stage && !QDir().mkpath(root.filePath(QStringLiteral("objects")))) return fail(QStringLiteral("Cannot prepare project documents."));
    for (const auto &value : library.value(QStringLiteral("project_readings")).toArray()) {
        const auto entry = value.toObject();
        const auto filename = entry.value(QStringLiteral("filename")).toString();
        const auto digest = entry.value(QStringLiteral("sha256")).toString();
        if (!QRegularExpression(QStringLiteral("^project_[0-9a-f]{24}\\.thoughts-project$")).match(filename).hasMatch()
            || filenames.contains(filename) || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(digest).hasMatch())
            return fail(QStringLiteral("Invalid project reading identity."));
        filenames.insert(filename);
        QByteArray bytes;
        if (!readVerifiedArtifact(stage ? cache.filePath(digest) : root.filePath(filename), digest, &bytes)
            || bytes.size() != entry.value(QStringLiteral("size_bytes")).toInteger(-1))
            return fail(QStringLiteral("A required project reading manifest is missing or corrupt."));
        const auto reading = ProjectReading::parse(bytes);
        if (!reading || reading->id != entry.value(QStringLiteral("project_id")).toString()
            || reading->pages != entry.value(QStringLiteral("page_count")).toInt(-1) || projects.contains(reading->id))
            return fail(QStringLiteral("Project metadata does not match its reading manifest."));
        projects.insert(reading->id);
        const auto copyObject = [&](const QString &hash, qint64 size) {
            const auto destination = root.filePath(QStringLiteral("objects/") + hash);
            if (QFileInfo(destination).isFile() && (size < 0 || QFileInfo(destination).size() == size)
                && artifactFileSha256(destination) == hash.toLatin1()) return true;
            if (!stage) return false;
            auto source = cache.filePath(hash);
            for (const auto &retained : retainedObjectDirectories) {
                if (QFileInfo(source).isFile()) break;
                source = QDir(retained).filePath(hash);
            }
            if (!QFileInfo(source).isFile()) {
                for (const auto &raw : library.value(QStringLiteral("publications")).toArray()) {
                    const auto publication = raw.toObject();
                    const auto filename = publication.value(QStringLiteral("filename")).toString();
                    if (publication.value(QStringLiteral("sha256")).toString() == hash && QFileInfo(filename).fileName() == filename)
                        source = root.filePath(filename);
                }
            }
            if (!QFileInfo(source).isFile() || (size >= 0 && QFileInfo(source).size() != size)
                || artifactFileSha256(source) != hash.toLatin1()) return false;
            QFile::remove(destination);
            // These are private immutable generation objects. Never symlink a mutable path.
#ifdef Q_OS_MACOS
            // An APFS clone shares the unchanged blocks, so each Sync no longer rewrites every
            // project document into its new generation.
            if (::clonefile(QFile::encodeName(source).constData(), QFile::encodeName(destination).constData(), CLONE_NOFOLLOW) == 0) return true;
#endif
            return QFile::copy(source, destination);
        };
        for (const auto &document : reading->documents) {
            const bool pdf = copyObject(document.pdfSha256, document.size);
            if (!pdf || !copyObject(document.packageSha256, -1))
                return fail(QStringLiteral("A required project document or package is missing or corrupt: %1 of \"%2\" in %3 (%4).")
                                .arg(pdf ? QStringLiteral("package") : QStringLiteral("PDF"), document.title, filename,
                                     pdf ? document.packageSha256 : document.pdfSha256));
        }
        if (stage) {
            QSaveFile manifest(root.filePath(filename));
            if (!manifest.open(QIODevice::WriteOnly) || manifest.write(bytes) != bytes.size() || !manifest.commit())
                return fail(QStringLiteral("Cannot stage the verified project reading manifest."));
        }
    }
    QSet<QString> actual;
    for (const auto &name : root.entryList({QStringLiteral("*.thoughts-project")}, QDir::Files)) actual.insert(name);
    if (actual != filenames) return fail(QStringLiteral("The generation contains unexpected project reading manifests."));
    return true;
}
}
#endif
