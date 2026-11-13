// SPDX-License-Identifier: GPL-2.0-or-later
#include "docscommentmutationstore.h"

#include "docscommentsynchronizer.h"
#include "thoughtsplatform.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QUuid>
#include <QDateTime>
#include <QJsonParseError>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>

namespace Okular
{

namespace
{

bool safePdfStem(const QString &pdfStem)
{
    return !pdfStem.isEmpty() && QFileInfo(pdfStem).fileName() == pdfStem && !pdfStem.contains(QLatin1Char('/')) && !pdfStem.contains(QLatin1Char('\\'));
}

bool syncDirectory(const QString &path, QString *error)
{
    const bool success = Thoughts::Platform::flushDirectory(path);
    if (!success && error) {
        *error = QStringLiteral("could not durability-sync the journal directory");
    }
    return success;
}

bool syncFile(QFile *file, QString *error)
{
    if (!file->flush()) {
        if (error) {
            *error = file->errorString();
        }
        return false;
    }
    if (!Thoughts::Platform::flushToDisk(file->handle())) {
        if (error) {
            *error = QStringLiteral("could not durability-sync the pending comment journal");
        }
        return false;
    }
    return true;
}

QStringList readJournalLines(const QString &path, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        return {};
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = file.errorString();
        }
        return {};
    }
    QStringList lines;
    int lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QByteArray encoded = file.readLine().trimmed();
        if (encoded.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(encoded, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (error) {
                *error = QStringLiteral("invalid pending comment mutation at line %1: %2").arg(lineNumber).arg(parseError.errorString());
            }
            return {};
        }
        lines.append(QString::fromUtf8(encoded));
    }
    return lines;
}

bool committedBatchLines(const QString &journal, QStringList *lines, const QStringList *applied, QString *error)
{
    const QDir directory(QFileInfo(journal).absoluteDir().filePath(QStringLiteral("batches")));
    if (!directory.exists()) return true;
    QLockFile lock(directory.filePath(QStringLiteral("writer.lock")));
    if (!lock.tryLock(2000)) { if (error) *error = QStringLiteral("The project comment journal is busy."); return false; }
    const QString key = QFileInfo(journal).fileName();
    for (const auto &name : directory.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile file(directory.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (!document.isObject()) { if (error) *error = QStringLiteral("Invalid project comment journal."); return false; }
        auto batches = document.object();
        const auto entries = batches.value(key).toArray();
        QJsonArray remainder;
        for (const auto &entry : entries) {
            const auto line = QString::fromUtf8(QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact));
            if (lines) lines->append(line);
            if (!applied || !applied->contains(line)) remainder.append(entry);
        }
        file.close();
        if (!applied || remainder.size() == entries.size()) continue;
        if (remainder.isEmpty()) batches.remove(key); else batches.insert(key, remainder);
        if (batches.isEmpty()) {
            if (!QFile::remove(file.fileName())) { if (error) *error = QStringLiteral("Cannot acknowledge project comment journal."); return false; }
        } else {
            QSaveFile output(file.fileName());
            const auto bytes = QJsonDocument(batches).toJson(QJsonDocument::Compact);
            if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size() || !output.commit()) {
                if (error) *error = output.errorString(); return false;
            }
        }
    }
    return !applied || syncDirectory(directory.path(), error);
}

}

DocsCommentMutationStore::DocsCommentMutationStore(QString stateRoot)
    : m_stateRoot(std::move(stateRoot))
{
}

QString DocsCommentMutationStore::journalPath(const QString &docsRoot, const QString &pdfStem) const
{
    if (docsRoot.isEmpty() || !safePdfStem(pdfStem)) {
        return {};
    }
    const QFileInfo rootInfo(docsRoot);
    const QString normalizedRoot = rootInfo.canonicalFilePath().isEmpty() ? rootInfo.absoluteFilePath() : rootInfo.canonicalFilePath();
    const QString key = QString::fromLatin1(QCryptographicHash::hash((normalizedRoot + QLatin1Char('\n') + pdfStem).toUtf8(), QCryptographicHash::Sha256).toHex());
    return QDir(m_stateRoot).filePath(QStringLiteral("comment-sidecar-pending/%1.journal").arg(key));
}

bool DocsCommentMutationStore::queue(const QString &docsRoot, const QString &pdfStem, const QList<QJsonObject> &mutations, QString *error) const
{
    const QString path = journalPath(docsRoot, pdfStem);
    if (path.isEmpty() || mutations.isEmpty()) {
        if (error) {
            *error = QStringLiteral("invalid pending comment mutation");
        }
        return false;
    }
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error) {
            *error = QStringLiteral("could not create the pending comment journal directory");
        }
        return false;
    }
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(2000)) {
        if (error) {
            *error = QStringLiteral("another writer is updating the pending comment journal");
        }
        return false;
    }

    QByteArray encoded;
    for (const QJsonObject &mutation : mutations) {
        encoded += QJsonDocument(mutation).toJson(QJsonDocument::Compact);
        encoded += '\n';
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    if (file.write(encoded) != encoded.size() || !syncFile(&file, error)) {
        return false;
    }
    file.close();
    return syncDirectory(directory, error);
}

bool DocsCommentMutationStore::queueBatches(const QString &docsRoot, const QMap<QString, QList<QJsonObject>> &batches, QString *error) const
{
    if (batches.size() == 1) return queue(docsRoot, batches.firstKey(), batches.first(), error);
    QJsonObject payload;
    QString parent;
    for (auto batch = batches.cbegin(); batch != batches.cend(); ++batch) {
        const auto path = journalPath(docsRoot, batch.key());
        if (path.isEmpty() || batch.value().isEmpty()) { if (error) *error = QStringLiteral("Invalid project comment batch."); return false; }
        parent = QFileInfo(path).absoluteDir().filePath(QStringLiteral("batches"));
        QJsonArray entries;
        for (const auto &mutation : batch.value()) entries.append(mutation);
        payload.insert(QFileInfo(path).fileName(), entries);
    }
    if (parent.isEmpty() || !QDir().mkpath(parent)) return false;
    QLockFile lock(QDir(parent).filePath(QStringLiteral("writer.lock")));
    if (!lock.tryLock(2000)) { if (error) *error = QStringLiteral("The project comment journal is busy."); return false; }
    const auto existing = QDir(parent).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    const qint64 sequence = qMax(QDateTime::currentMSecsSinceEpoch(), existing.isEmpty() ? qint64(0) : existing.last().section(QLatin1Char('-'), 0, 0).toLongLong() + 1);
    const QString name = QStringLiteral("%1-%2.json").arg(sequence, 20, 10, QLatin1Char('0')).arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSaveFile file(QDir(parent).filePath(name));
    const auto encoded = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(encoded) != encoded.size() || !file.commit()) {
        if (error) *error = file.errorString(); return false;
    }
    return syncDirectory(parent, error);
}

DocsCommentMutationFlushResult DocsCommentMutationStore::flush(const QString &docsRoot, const QString &pdfStem, int lockTimeoutMs) const
{
    DocsCommentMutationFlushResult result;
    result.docsRoot = QFileInfo(docsRoot).absoluteFilePath();
    result.pdfStem = pdfStem;
    const QString path = journalPath(docsRoot, pdfStem);
    if (path.isEmpty()) {
        result.failureReason = QStringLiteral("invalid PDF stem");
        return result;
    }

    QString error;
    QLockFile journalLock(path + QStringLiteral(".lock"));
    journalLock.setStaleLockTime(30000);
    if (!journalLock.tryLock(lockTimeoutMs)) {
        result.failureReason = QStringLiteral("another writer is updating the pending comment journal");
        return result;
    }
    QStringList lines = readJournalLines(path, &error);
    journalLock.unlock();
    if (error.isEmpty()) committedBatchLines(path, &lines, nullptr, &error);
    if (!error.isEmpty()) {
        result.failureReason = error;
        return result;
    }
    if (lines.isEmpty()) {
        result.success = true;
        return result;
    }

    QList<QJsonObject> mutations;
    mutations.reserve(lines.size());
    for (const QString &line : lines) {
        const auto mutation = QJsonDocument::fromJson(line.toUtf8()).object();
        mutations.append(mutation);
        if (mutation.value(QStringLiteral("kind")).toString() == QLatin1String("upsert")
            && mutation.value(QStringLiteral("create_logical_comment")).toBool()) {
            result.createdCommentIds.insert(mutation.value(QStringLiteral("id")).toString());
        }
    }
    const DocsCommentSynchronizer::CommentMutationBatchResult applied =
        DocsCommentSynchronizer::applySidecarMutations(docsRoot, pdfStem, mutations, lockTimeoutMs);
    result.success = applied.success;
    result.failureReason = applied.failureReason;
    result.appliedLines = lines;
    result.changedRecords = applied.changedRecords;
    result.visibleFingerprint = applied.visibleFingerprint;
    return result;
}

bool DocsCommentMutationStore::acknowledge(const QString &docsRoot, const QString &pdfStem, const QStringList &appliedLines, QString *error) const
{
    if (appliedLines.isEmpty()) {
        return true;
    }
    const QString path = journalPath(docsRoot, pdfStem);
    if (path.isEmpty()) {
        if (error) {
            *error = QStringLiteral("invalid PDF stem");
        }
        return false;
    }
    QLockFile lock(path + QStringLiteral(".lock"));
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(2000)) {
        if (error) {
            *error = QStringLiteral("another writer is updating the pending comment journal");
        }
        return false;
    }
    const QStringList current = readJournalLines(path, error);
    if (error && !error->isEmpty()) {
        return false;
    }
    QStringList remainder;
    if (current.mid(0, appliedLines.size()) == appliedLines) {
        remainder = current.mid(appliedLines.size());
    } else {
        const QSet<QString> appliedSet(appliedLines.cbegin(), appliedLines.cend());
        for (const QString &line : current) {
            if (!appliedSet.contains(line)) {
                remainder.append(line);
            }
        }
    }

    const QString directory = QFileInfo(path).absolutePath();
    if (remainder.isEmpty()) {
        if (!QFile::remove(path) && QFileInfo::exists(path)) {
            if (error) {
                *error = QStringLiteral("could not remove the applied pending comment journal");
            }
            return false;
        }
        return syncDirectory(directory, error) && committedBatchLines(path, nullptr, &appliedLines, error);
    }

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    for (const QString &line : remainder) {
        const QByteArray encoded = line.toUtf8() + '\n';
        if (file.write(encoded) != encoded.size()) {
            if (error) {
                *error = file.errorString();
            }
            return false;
        }
    }
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return syncDirectory(directory, error) && committedBatchLines(path, nullptr, &appliedLines, error);
}

bool DocsCommentMutationStore::hasPending(const QString &docsRoot, const QString &pdfStem) const
{
    const QString path = journalPath(docsRoot, pdfStem);
    if (path.isEmpty()) return false;
    if (QFileInfo(path).isFile() && QFileInfo(path).size() > 0) return true;
    QStringList lines;
    return !committedBatchLines(path, &lines, nullptr, nullptr) || !lines.isEmpty();
}

}
