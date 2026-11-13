/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_PROPOSALHISTORY_H
#define THOUGHTS_PROPOSALHISTORY_H

#include "currentannotationstore.h"
#include "thoughtsplatform.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <memory>

namespace Thoughts::ProposalDecisions
{
// shell/proposaldecisions.cpp: the decision claims of protocol/agent-proposals.md §10.4.2.
bool recordClaims(const QString &database, const QList<QJsonObject> &claims, QString *error);
bool forgetClaims(const QString &database, const QStringList &decisionIds, QString *error);
}

namespace Thoughts::ProposalHistory
{
inline QString directory(const QString &root) { return QDir(root).filePath(QStringLiteral("comments/.history")); }
inline QString digest(const QByteArray &raw) { return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex()); }
// Retired archive of every observation, kept only so old workspaces can be cleaned up.
inline QString filename(const QString &root) { return QDir(directory(root)).filePath(QStringLiteral("proposal_events.jsonl")); }
inline QString agentRequestsFilename(const QString &root) { return QDir(directory(root)).filePath(QStringLiteral("agent-requests.json")); }

// Only the current state of each open proposal is kept (CurrentAnnotationStore
// type "proposal"); resolved proposals are dropped and no history is stored.
inline bool latest(const QString &root, QHash<QString, QJsonObject> *result, QString *error = nullptr)
{
    CurrentAnnotationStore store(root);
    if (store.read(QStringLiteral("proposal"), result)) return true;
    if (error) *error = store.error();
    return false;
}

struct Dates {
    QDateTime firstSeen;
    QDateTime updated;
};

inline QJsonObject agentRequests(const QString &root)
{
    QFile file(agentRequestsFilename(root));
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// An agent tool may retry a request; its result is kept for a month so the retry
// returns the same answer instead of editing the source twice.
inline bool rememberAgentRequests(const QString &root, const QJsonArray &events, QString *error = nullptr)
{
    QJsonObject requests = agentRequests(root);
    bool changed = false;
    for (const auto &value : events) {
        const auto event = value.toObject();
        const QString id = event.value(QStringLiteral("agent_request_id")).toString();
        if (id.isEmpty()) continue;
        requests.insert(id, QJsonObject{{QStringLiteral("sha256"), event.value(QStringLiteral("agent_request_sha256"))},
                                        {QStringLiteral("result"), event.value(QStringLiteral("agent_result"))},
                                        {QStringLiteral("at"), event.value(QStringLiteral("observed_at"))}});
        changed = true;
    }
    if (!changed) return true;
    const QString cutoff = QDateTime::currentDateTimeUtc().addDays(-30).toString(Qt::ISODateWithMs);
    for (auto it = requests.begin(); it != requests.end();)
        it = it.value().toObject().value(QStringLiteral("at")).toString() < cutoff ? requests.erase(it) : std::next(it);
    QSaveFile file(agentRequestsFilename(root));
    const QByteArray bytes = QJsonDocument(requests).toJson(QJsonDocument::Compact);
    if (QDir().mkpath(directory(root)) && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit()) return true;
    if (error) *error = QStringLiteral("Cannot save the agent request record.");
    return false;
}

// Apply observed proposal changes to the current state.
inline bool append(const QString &root, const QJsonArray &incoming, QString *error = nullptr)
{
    if (incoming.isEmpty()) return true;
    if (!rememberAgentRequests(root, incoming, error)) return false;
    CurrentAnnotationStore store(root);
    const bool saved = store.applyProposalEvents(incoming);
    if (!saved && error) *error = store.error();
    return saved;
}

inline QJsonObject event(const QString &kind, const QJsonObject &snapshot, const QString &before, const QString &after)
{
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("event_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("proposal_id"), snapshot.value(QStringLiteral("proposal_id"))}, {QStringLiteral("kind"), kind},
            {QStringLiteral("observed_at"), now}, {QStringLiteral("updated_at"), now}, {QStringLiteral("authority"), QStringLiteral("local_source")},
            {QStringLiteral("source_operation_id"), QString()}, {QStringLiteral("source_before_sha256"), before},
            {QStringLiteral("source_after_sha256"), after}, {QStringLiteral("snapshot"), snapshot},
            {QStringLiteral("revision"), 1}, {QStringLiteral("deleted"), false}};
}


inline QString sourceLockPath(const QString &root, const QString &path)
{
    return QDir(directory(root)).filePath(digest(path.toUtf8()) + QStringLiteral(".source.lock"));
}

// One write of several sources (a move pair across files, protocol/agent-proposals.md
// §12): every file gets its new text or none does. The journal holds each file's
// new text, so a crash part-way is rolled forward by recoverSources().
struct SourceWrite {
    QString path;   // canonical
    QByteArray before;
    QByteArray after;
};

// Several-source journals have their own name: an older build, which reads
// proposal_pending_*.json as single-source journals, leaves them alone.
inline bool recoverSources(const QString &root, QString *error = nullptr)
{
    const QDir dir(directory(root));
    for (const auto &name : dir.entryList({QStringLiteral("proposal_move_pending_*.json")}, QDir::Files)) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
        const auto pending = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
        struct Entry { QString path; QString before; QString after; QByteArray text; QString current; };
        QList<Entry> entries;
        bool valid = !canonicalRoot.isEmpty();
        for (const auto &value : pending.value(QStringLiteral("sources")).toArray()) {
            const auto source = value.toObject();
            Entry entry{source.value(QStringLiteral("source_path")).toString(), source.value(QStringLiteral("before_sha256")).toString(),
                        source.value(QStringLiteral("after_sha256")).toString(),
                        QByteArray::fromBase64(source.value(QStringLiteral("after")).toString().toLatin1()), {}};
            valid = valid && entry.path.startsWith(canonicalRoot + QLatin1Char('/')) && digest(entry.text) == entry.after;
            entries.append(entry);
        }
        if (!valid || entries.isEmpty()) { if (error) *error = QStringLiteral("Invalid pending proposal history transaction."); return false; }
        // A writer still holds the source locks while it works: leave its journal alone.
        QList<std::shared_ptr<QLockFile>> locks;
        bool busy = false;
        for (const auto &entry : std::as_const(entries)) {
            auto lock = std::make_shared<QLockFile>(sourceLockPath(root, entry.path));
            if (!lock->tryLock(0)) { busy = true; break; }
            locks.append(lock);
        }
        if (busy) continue;
        bool foreign = false;
        for (auto &entry : entries) {
            QFile source(entry.path);
            entry.current = source.open(QIODevice::ReadOnly) ? digest(source.readAll()) : QString();
            foreign = foreign || (entry.current != entry.before && entry.current != entry.after);
        }
        if (foreign) {
            // Someone changed a file since: finishing would overwrite their text.
            qWarning("A proposal move could not be completed because a source changed: %s", qPrintable(name));
        } else {
            for (const auto &entry : std::as_const(entries)) {
                if (entry.current != entry.after && !Platform::replaceFileContents(entry.path, entry.text)) {
                    if (error) *error = QStringLiteral("A proposal move could not be completed in %1.").arg(entry.path);
                    return false;
                }
            }
            if (!append(root, pending.value(QStringLiteral("events")).toArray(), error)) return false;
        }
        if (!QFile::remove(dir.filePath(name))) { if (error) *error = QStringLiteral("Cannot remove %1.").arg(name); return false; }
    }
    return true;
}

// Recover a source/history transaction only when the intended source write is verified.
inline bool recover(const QString &root, QString *error = nullptr)
{
    const QDir dir(directory(root));
    if (!dir.exists()) return true;
    QLockFile recoveryLock(dir.filePath(QStringLiteral("proposal_recovery.lock")));
    if (!recoveryLock.tryLock(5000)) { if (error) *error = QStringLiteral("Proposal history recovery is busy."); return false; }
    if (!recoverSources(root, error)) return false;
    for (const auto &name : dir.entryList({QStringLiteral("proposal_pending_*.json")}, QDir::Files)) {
        QFile file(dir.filePath(name));
        if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return false; }
        const auto pending = QJsonDocument::fromJson(file.readAll()).object();
        const auto sourcePath = pending.value(QStringLiteral("source_path")).toString();
        const auto canonicalRoot = QFileInfo(root).canonicalFilePath();
        const auto canonicalSource = QFileInfo(sourcePath).canonicalFilePath();
        if (sourcePath.isEmpty() || canonicalRoot.isEmpty() || (!canonicalSource.isEmpty() && !canonicalSource.startsWith(canonicalRoot + QLatin1Char('/')))) {
            if (error) *error = QStringLiteral("Invalid pending proposal history transaction."); return false;
        }
        if (!QFileInfo(sourcePath).isFile()) continue;
        QFile source(sourcePath);
        if (!source.open(QIODevice::ReadOnly)) continue;
        if (digest(source.readAll()) != pending.value(QStringLiteral("after_sha256")).toString()) continue;
        if (!append(root, pending.value(QStringLiteral("events")).toArray(), error)) return false;
        file.close();
        if (!file.remove()) { if (error) *error = file.errorString(); return false; }
    }
    return true;
}

// Decision claims (§10.5) are recorded in `claimDatabase` together with the
// journal and before the source is replaced (write-ahead): a crash leaves no
// claim or a claim whose wrapper is still present, which Sync drops.
inline bool writeSource(const QString &root, const QString &path, const QByteArray &before,
                        const QByteArray &after, const QJsonArray &events, QString *error,
                        const QList<QJsonObject> &claims = {}, const QString &claimDatabase = {})
{
    if (!recover(root, error) || !QDir().mkpath(directory(root))) return false;
    QLockFile sourceLock(QDir(directory(root)).filePath(digest(path.toUtf8()) + QStringLiteral(".source.lock")));
    if (!sourceLock.tryLock(5000)) { if (error) *error = QStringLiteral("Another proposal edit is saving this source."); return false; }
    const auto journalPath = QDir(directory(root)).filePath(QStringLiteral("proposal_pending_%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QSaveFile journal(journalPath);
    const auto pending = QJsonDocument(QJsonObject{{QStringLiteral("source_path"), path}, {QStringLiteral("after_sha256"), digest(after)},
                                                  {QStringLiteral("events"), events}}).toJson(QJsonDocument::Compact);
    if (!journal.open(QIODevice::WriteOnly) || journal.write(pending) != pending.size() || !journal.commit()) {
        if (error) *error = QStringLiteral("Cannot save proposal history before changing the source."); return false;
    }
    QFile current(path);
    if (!current.open(QIODevice::ReadOnly) || current.readAll() != before) {
        QFile::remove(journalPath);
        if (error) *error = QStringLiteral("The proposal source changed before it could be saved."); return false;
    }
    current.close();
    const bool claimed = !claims.isEmpty() && !claimDatabase.isEmpty();
    QString claimError;
    if (claimed && !ProposalDecisions::recordClaims(claimDatabase, claims, &claimError)) {
        QFile::remove(journalPath);
        if (error) *error = QStringLiteral("Cannot record the proposal decision before changing the source: %1").arg(claimError);
        return false;
    }
    if (!Platform::replaceFileContents(path, after)) {
        QFile::remove(journalPath);
        if (claimed) {
            QStringList ids;
            for (const auto &claim : claims) ids.append(claim.value(QStringLiteral("decision_id")).toString());
            QString ignored;
            // Left behind, they would be dropped at upload: the wrapper is still there.
            ProposalDecisions::forgetClaims(claimDatabase, ids, &ignored);
        }
        if (error) *error = QStringLiteral("The proposal source could not be saved."); return false;
    }
    // The durable journal survives an interrupted history promotion; source success
    // must not be reported as failure after the TeX has already been committed.
    QString recoveryError;
    if (!recover(root, &recoveryError)) qWarning("Proposal history promotion pending: %s", qPrintable(recoveryError));
    return true;
}

// writeSource for several files at once (§12): the journal, the claims, then the
// files; a failed write puts back the files already written. After a crash the
// next recover() writes the remaining files from the journal.
inline bool writeSources(const QString &root, QList<SourceWrite> writes, const QJsonArray &events, QString *error,
                         const QList<QJsonObject> &claims = {}, const QString &claimDatabase = {})
{
    if (writes.size() == 1) return writeSource(root, writes.first().path, writes.first().before, writes.first().after, events, error, claims, claimDatabase);
    if (writes.isEmpty() || !recover(root, error) || !QDir().mkpath(directory(root))) return false;
    // A fixed order, so two writers of the same files cannot wait for each other.
    std::sort(writes.begin(), writes.end(), [](const SourceWrite &a, const SourceWrite &b) { return a.path < b.path; });
    QList<std::shared_ptr<QLockFile>> locks;
    for (const auto &write : std::as_const(writes)) {
        auto lock = std::make_shared<QLockFile>(sourceLockPath(root, write.path));
        if (!lock->tryLock(5000)) { if (error) *error = QStringLiteral("Another proposal edit is saving this source."); return false; }
        locks.append(lock);
    }
    QJsonArray sources;
    for (const auto &write : std::as_const(writes))
        sources.append(QJsonObject{{QStringLiteral("source_path"), write.path}, {QStringLiteral("before_sha256"), digest(write.before)},
                                   {QStringLiteral("after_sha256"), digest(write.after)}, {QStringLiteral("after"), QString::fromLatin1(write.after.toBase64())}});
    const auto journalPath = QDir(directory(root)).filePath(QStringLiteral("proposal_move_pending_%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QSaveFile journal(journalPath);
    const auto pending = QJsonDocument(QJsonObject{{QStringLiteral("sources"), sources}, {QStringLiteral("events"), events}}).toJson(QJsonDocument::Compact);
    if (!journal.open(QIODevice::WriteOnly) || journal.write(pending) != pending.size() || !journal.commit()) {
        if (error) *error = QStringLiteral("Cannot save proposal history before changing the sources."); return false;
    }
    for (const auto &write : std::as_const(writes)) {
        QFile current(write.path);
        if (!current.open(QIODevice::ReadOnly) || current.readAll() != write.before) {
            QFile::remove(journalPath);
            if (error) *error = QStringLiteral("The proposal source %1 changed before it could be saved.").arg(QFileInfo(write.path).fileName()); return false;
        }
    }
    const bool claimed = !claims.isEmpty() && !claimDatabase.isEmpty();
    const auto forget = [&] {
        if (!claimed) return;
        QStringList ids;
        for (const auto &claim : claims) ids.append(claim.value(QStringLiteral("decision_id")).toString());
        QString ignored;
        ProposalDecisions::forgetClaims(claimDatabase, ids, &ignored);
    };
    QString claimError;
    if (claimed && !ProposalDecisions::recordClaims(claimDatabase, claims, &claimError)) {
        QFile::remove(journalPath);
        if (error) *error = QStringLiteral("Cannot record the proposal decision before changing the sources: %1").arg(claimError);
        return false;
    }
    for (qsizetype i = 0; i < writes.size(); ++i) {
        if (Platform::replaceFileContents(writes[i].path, writes[i].after)) continue;
        bool restored = true;
        for (qsizetype j = 0; j < i; ++j) restored = Platform::replaceFileContents(writes[j].path, writes[j].before) && restored;
        if (!restored) {
            // Keep the journal: recovery finishes the write instead of leaving half of it.
            if (error) *error = QStringLiteral("The proposal sources could not be saved together; Thoughts completes the write when it next saves a proposal.");
            return false;
        }
        QFile::remove(journalPath);
        forget();
        if (error) *error = QStringLiteral("The proposal source %1 could not be saved.").arg(QFileInfo(writes[i].path).fileName()); return false;
    }
    // Every file is written: promote the history and drop the journal while still holding the locks.
    QString promotion;
    if (append(root, events, &promotion)) QFile::remove(journalPath);
    else qWarning("Proposal history promotion pending: %s", qPrintable(promotion));
    return true;
}
}
#endif
