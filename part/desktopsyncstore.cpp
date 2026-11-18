// SPDX-License-Identifier: GPL-2.0-or-later
#include "desktopsyncstore.h"
#include "docschangemonitor.h"
#include "sourceowner.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <thoughts/durableoutbox.h>

#include <algorithm>
#include <limits>
#include <tuple>

namespace Thoughts
{

namespace
{

void clearError(QString *error)
{
    if (error) {
        error->clear();
    }
}

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

QString normalizedPath(const QString &path)
{
    const QString clean = QDir::cleanPath(path.trimmed());
    if (clean.isEmpty() || clean == QLatin1String(".") || QDir::isAbsolutePath(clean) || clean == QLatin1String("..") || clean.startsWith(QLatin1String("../"))) {
        return {};
    }
    return clean;
}

bool pathIsWithin(const QString &path, const QString &directory)
{
    return path == directory || path.startsWith(directory + QLatin1Char('/'));
}

bool directoryBaseline(const DesktopTrackedPath &path)
{
    return path.metadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory");
}

bool discoveryDirectoryBaseline(const DesktopTrackedPath &path)
{
    return directoryBaseline(path)
        && path.metadata.value(QStringLiteral("_monitoring_mode")).toString()
            == QLatin1String("discovery");
}

QStringList normalizedTargets(QStringList targets)
{
    for (QString &target : targets) {
        target = target.trimmed();
    }
    targets.removeAll(QString());
    targets.removeDuplicates();
    std::sort(targets.begin(), targets.end());
    return targets;
}

const QSet<QString> &ignoredDesktopSourceDirectories()
{
    static const QSet<QString> ignoredDirectories {
        QStringLiteral(".cache"),  QStringLiteral(".git"),       QStringLiteral(".hg"),          QStringLiteral(".mypy_cache"), QStringLiteral(".pytest_cache"), QStringLiteral(".ruff_cache"),
        QStringLiteral(".svn"),    QStringLiteral(".tox"),       QStringLiteral(".venv"),        QStringLiteral("__pycache__"), QStringLiteral("build"),         QStringLiteral("dist"),
        QStringLiteral("log"),     QStringLiteral("logs"),       QStringLiteral("node_modules"), QStringLiteral("out"),         QStringLiteral("output"),        QStringLiteral("pdf"),
        QStringLiteral("scripts"), QStringLiteral("submission"), QStringLiteral("target"),       QStringLiteral("temp"),        QStringLiteral("tmp"),           QStringLiteral("venv"),
    };
    return ignoredDirectories;
}

// Inside a source folder (a docs-* folder or a Project's default folder) its
// .ThoughtIgnore is the only source policy, and the scanner applies it. The
// name heuristics below cover everything else.
bool isWithinDocsProject(const QStringList &components)
{
    const QString path = components.join(QLatin1Char('/'));
    const QString owner = Thoughts::SourceOwner::of(path);
    return !owner.isEmpty() && path.size() > owner.size();
}

bool isEligibleDesktopSourceDirectoryImpl(const QString &path)
{
    const QString clean = normalizedPath(path);
    if (clean.isEmpty()) {
        return false;
    }
    const QStringList components = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (isWithinDocsProject(components)) {
        return true;
    }
    return !components.isEmpty()
        && std::none_of(components.cbegin(), components.cend(), [](const QString &component) {
               return ignoredDesktopSourceDirectories().contains(component.toCaseFolded());
           });
}

bool isPotentialDesktopSourcePathImpl(const QString &path)
{
    const QString clean = normalizedPath(path);
    if (clean.isEmpty()) {
        return false;
    }
    const QStringList components = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (components.isEmpty()) {
        return false;
    }
    // A default folder's configuration and imported PDFs keep their own
    // handling; they are never discovered as sources.
    const QString owner = Thoughts::SourceOwner::of(clean);
    if (Thoughts::SourceOwner::isDefaultFolder(owner) && components.size() == 5
        && (components.constLast() == QLatin1String("thoughts-project.json") || components.constLast().endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))) {
        return false;
    }
    if (isWithinDocsProject(components)) {
        return true;
    }
    for (qsizetype index = 0; index + 1 < components.size(); ++index) {
        if (ignoredDesktopSourceDirectories().contains(components.at(index).toCaseFolded())) {
            return false;
        }
    }

    const QString foldedName = components.constLast().toCaseFolded();
    if (foldedName.isEmpty() || foldedName.startsWith(QLatin1Char('.')) || foldedName == QLatin1String("main.pdf") || foldedName == QLatin1String("agents.md") || foldedName == QLatin1String("cmakelists.txt") ||
        foldedName == QLatin1String("makefile") || foldedName.startsWith(QLatin1String("readme")) || foldedName.startsWith(QLatin1String("license")) || foldedName.startsWith(QLatin1String("requirements"))) {
        return false;
    }
    static const QSet<QString> ignoredSuffixes {
        QStringLiteral("aux"), QStringLiteral("bak"), QStringLiteral("bbl"), QStringLiteral("bcf"),  QStringLiteral("blg"), QStringLiteral("dvi"),  QStringLiteral("fdb_latexmk"), QStringLiteral("fls"),
        QStringLiteral("lof"), QStringLiteral("log"), QStringLiteral("lot"), QStringLiteral("nav"),  QStringLiteral("out"), QStringLiteral("part"), QStringLiteral("rej"),         QStringLiteral("snm"),
        QStringLiteral("swn"), QStringLiteral("swo"), QStringLiteral("swp"), QStringLiteral("temp"), QStringLiteral("tmp"), QStringLiteral("toc"),  QStringLiteral("vrb"),         QStringLiteral("xdv"),
    };
    const QString suffix = QFileInfo(foldedName).suffix();
    if (suffix.isEmpty() || ignoredSuffixes.contains(suffix) || foldedName.endsWith(QLatin1String(".synctex.gz")) || foldedName.endsWith(QLatin1String(".run.xml"))) {
        return false;
    }
    static const QSet<QString> strongSourceOrAssetSuffixes {
        QStringLiteral("html"), QStringLiteral("htm"),
        QStringLiteral("asy"), QStringLiteral("bbx"), QStringLiteral("bib"),     QStringLiteral("bst"),  QStringLiteral("cbx"),  QStringLiteral("cls"), QStringLiteral("def"), QStringLiteral("dot"),
        QStringLiteral("eps"), QStringLiteral("gif"), QStringLiteral("gnuplot"), QStringLiteral("gp"),   QStringLiteral("jpeg"), QStringLiteral("jpg"), QStringLiteral("ltx"), QStringLiteral("mat"),
        QStringLiteral("m"),   QStringLiteral("pdf"), QStringLiteral("pgf"),     QStringLiteral("plot"), QStringLiteral("png"),  QStringLiteral("ps"),  QStringLiteral("sty"), QStringLiteral("svg"),
        QStringLiteral("tex"), QStringLiteral("tif"), QStringLiteral("tiff"),    QStringLiteral("tikz"), QStringLiteral("webp"),
    };
    if (strongSourceOrAssetSuffixes.contains(suffix)) {
        return true;
    }
    // Accepted only inside a source directory below. Markdown documents,
    // their fragments, and their stylesheets are sources exactly like TeX;
    // without them a file created while Thoughts was closed was never found.
    static const QSet<QString> contextualDataSuffixes {
        QStringLiteral("css"),
        // Fonts a stylesheet loads (markdown/support/fonts) are build inputs.
        QStringLiteral("otf"),
        QStringLiteral("ttf"),
        QStringLiteral("woff"),
        QStringLiteral("woff2"),
        QStringLiteral("markdown"),
        QStringLiteral("md"),
        QStringLiteral("csv"),
        QStringLiteral("dat"),
        QStringLiteral("json"),
        QStringLiteral("tsv"),
        QStringLiteral("txt"),
        QStringLiteral("xml"),
        QStringLiteral("yaml"),
        QStringLiteral("yml"),
    };
    if (!contextualDataSuffixes.contains(suffix)) {
        return false;
    }
    static const QSet<QString> sourceDirectories {
        QStringLiteral("assets"),
        QStringLiteral("data"),
        QStringLiteral("figures"),
        QStringLiteral("fragments"),
        QStringLiteral("generated"),
        QStringLiteral("plots"),
        QStringLiteral("source"),
        QStringLiteral("sources"),
        QStringLiteral("support"),
        QStringLiteral("tex"),
    };
    return std::any_of(components.cbegin(), std::prev(components.cend()), [](const QString &component) { return sourceDirectories.contains(component.toCaseFolded()); });
}

QString jsonText(const QJsonObject &object)
{
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString targetsText(const QStringList &targets)
{
    return QString::fromUtf8(QJsonDocument(QJsonArray::fromStringList(normalizedTargets(targets))).toJson(QJsonDocument::Compact));
}

QJsonObject parsedObject(const QVariant &value)
{
    const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8());
    return document.isObject() ? document.object() : QJsonObject();
}

QStringList parsedTargets(const QVariant &value)
{
    const QJsonDocument document = QJsonDocument::fromJson(value.toString().toUtf8());
    QStringList result;
    if (document.isArray()) {
        for (const QJsonValue &target : document.array()) {
            if (target.isString()) {
                result.append(target.toString());
            }
        }
    }
    return normalizedTargets(result);
}

bool validSha256(const QString &sha256)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(sha256).hasMatch();
}

QSqlDatabase databaseFor(const QString &connectionName)
{
    return QSqlDatabase::contains(connectionName) ? QSqlDatabase::database(connectionName, false) : QSqlDatabase();
}

// Rows changed on each open connection when observers were last told.
QMutex announcedChangesMutex;
QHash<QString, qint64> announcedChanges;

void forgetAnnouncedChanges(const QString &connectionName)
{
    QMutexLocker locker(&announcedChangesMutex);
    announcedChanges.remove(connectionName);
}

// A transaction that reads and then writes. A deferred BEGIN takes SQLite's
// write lock only at its first write; in WAL mode that upgrade fails at once
// with "database is locked" while another connection (the change tracker, a
// thoughtsctl decision) writes, whatever busy_timeout says. BEGIN IMMEDIATE
// takes the lock first and waits busy_timeout for it.
bool beginWriteTransaction(QSqlDatabase &database, QString *failure)
{
    if (!database.isValid() || !database.isOpen()) {
        *failure = QStringLiteral("desktop synchronization store is not open");
        return false;
    }
    QSqlQuery begin(database);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) {
        *failure = begin.lastError().text().isEmpty() ? QStringLiteral("the desktop synchronization store could not start a transaction") : begin.lastError().text();
        return false;
    }
    return true;
}

// Commits and, when the transaction modified rows, tells in-process
// observers. Consumers learn about Thoughts' own writes from this rather than
// by watching the database files, and no-op transactions stay silent.
bool commitAndAnnounce(QSqlDatabase &database)
{
    if (!database.commit()) {
        return false;
    }
    QSqlQuery changes(database);
    if (!changes.exec(QStringLiteral("SELECT total_changes()")) || !changes.next()) {
        return true;
    }
    const qint64 total = changes.value(0).toLongLong();
    {
        QMutexLocker locker(&announcedChangesMutex);
        qint64 &last = announcedChanges[database.connectionName()];
        if (total == last) {
            return true;
        }
        last = total;
    }
    notifyDesktopSyncStoreChanged(database.databaseName());
    return true;
}

bool execStatement(QSqlDatabase &database, const QString &statement, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(statement)) {
        return fail(error, query.lastError().text());
    }
    return true;
}

bool ensureColumn(QSqlDatabase &database, const QString &table, const QString &column, const QString &definition, QString *error)
{
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
        return fail(error, query.lastError().text());
    }
    while (query.next()) {
        if (query.value(1).toString() == column) {
            return true;
        }
    }
    return execStatement(database, QStringLiteral("ALTER TABLE %1 ADD COLUMN %2 %3").arg(table, column, definition), error);
}

bool setMetadata(QSqlDatabase &database, const QString &key, const QString &value, QString *error)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("INSERT INTO desktop_sync_metadata(key, value) VALUES(?, ?) ON CONFLICT(key) DO UPDATE SET value=excluded.value"));
    query.addBindValue(key);
    query.addBindValue(value);
    return query.exec() || fail(error, query.lastError().text());
}

QString metadataValue(QSqlDatabase &database, const QString &key)
{
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT value FROM desktop_sync_metadata WHERE key = ?"));
    query.addBindValue(key);
    return query.exec() && query.next() ? query.value(0).toString() : QString();
}

quint64 nextGeneration(QSqlDatabase &database, QString *error)
{
    bool ok = false;
    quint64 current = metadataValue(database, QStringLiteral("last_generation")).toULongLong(&ok);
    if (!ok) {
        QSqlQuery query(database);
        if (!query.exec(QStringLiteral("SELECT MAX(value) FROM ("
                                       "SELECT COALESCE(MAX(generation), 0) AS value FROM desktop_pending_paths "
                                       "UNION ALL "
                                       "SELECT COALESCE(MAX(generation), 0) AS value FROM desktop_library_operations)")) ||
            !query.next()) {
            fail(error, query.lastError().text());
            return 0;
        }
        current = query.value(0).toULongLong();
    }
    if (current >= static_cast<quint64>(std::numeric_limits<qint64>::max())) {
        fail(error, QStringLiteral("desktop synchronization generation is exhausted"));
        return 0;
    }
    const quint64 next = current + 1;
    return setMetadata(database, QStringLiteral("last_generation"), QString::number(next), error) ? next : 0;
}

DesktopTrackedPath trackedPathFromQuery(QSqlQuery &query)
{
    DesktopTrackedPath path;
    path.path = query.value(0).toString();
    path.exists = query.value(1).toBool();
    path.sizeBytes = query.value(2).toLongLong();
    path.modifiedAtMs = query.value(3).toLongLong();
    path.sha256 = query.value(4).toString();
    path.metadata = parsedObject(query.value(5));
    path.affectedTargets = parsedTargets(query.value(6));
    return path;
}

QHash<QString, DesktopTrackedPath> readTrackedPaths(QSqlDatabase &database, const QString &table, QString *error)
{
    QHash<QString, DesktopTrackedPath> result;
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT path, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json FROM %1").arg(table))) {
        fail(error, query.lastError().text());
        return {};
    }
    while (query.next()) {
        const DesktopTrackedPath path = trackedPathFromQuery(query);
        result.insert(path.path, path);
    }
    return result;
}

bool insertTrackedPath(QSqlQuery &query, const DesktopTrackedPath &path, QString *error)
{
    query.bindValue(0, path.path);
    query.bindValue(1, path.exists);
    query.bindValue(2, path.sizeBytes);
    query.bindValue(3, path.modifiedAtMs);
    query.bindValue(4, path.sha256.isEmpty() ? QStringLiteral("") : path.sha256);
    query.bindValue(5, jsonText(path.metadata));
    query.bindValue(6, targetsText(path.affectedTargets));
    return query.exec() || fail(error, query.lastError().text());
}

bool trackedPathsEqual(const DesktopTrackedPath &baseline, const DesktopTrackedPath &observed)
{
    QJsonObject baselineMetadata = baseline.metadata;
    QJsonObject observedMetadata = observed.metadata;
    const QJsonValue baselineChangedAt = baselineMetadata.take(QStringLiteral("_local_ctime_ms"));
    const QJsonValue observedChangedAt = observedMetadata.take(QStringLiteral("_local_ctime_ms"));
    const QString baselineIdentity = baselineMetadata.take(QStringLiteral("_local_file_identity")).toString();
    const QString observedIdentity = observedMetadata.take(QStringLiteral("_local_file_identity")).toString();
    const bool directorySentinels = baselineMetadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory") && observedMetadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory");
    if (directorySentinels) {
        // Directory rows are coverage sensors. Their physical identity only
        // schedules exact file discovery; it is never publication work.
        return true;
    }
    if (baseline.exists != observed.exists) {
        return false;
    }
    if (!baseline.exists) {
        return baselineMetadata == observedMetadata;
    }
    bool contentsEqual = false;
    if (validSha256(baseline.sha256) && validSha256(observed.sha256)) {
        contentsEqual = baseline.sha256 == observed.sha256;
    } else {
        contentsEqual = baseline.sizeBytes == observed.sizeBytes && baseline.modifiedAtMs == observed.modifiedAtMs;
        if (contentsEqual && !baselineIdentity.isEmpty()) {
            contentsEqual = baselineIdentity == observedIdentity;
        }
        if (contentsEqual && baselineChangedAt.isDouble()) {
            contentsEqual = observedChangedAt.isDouble() && baselineChangedAt.toInteger() == observedChangedAt.toInteger();
        }
    }
    return contentsEqual && baselineMetadata == observedMetadata;
}

bool trackedPathsExactlyEqual(const DesktopTrackedPath &left, const DesktopTrackedPath &right)
{
    return left.path == right.path && left.exists == right.exists && left.sizeBytes == right.sizeBytes && left.modifiedAtMs == right.modifiedAtMs && left.sha256 == right.sha256 && left.metadata == right.metadata &&
        normalizedTargets(left.affectedTargets) == normalizedTargets(right.affectedTargets);
}

bool coalescePendingPath(QSqlDatabase &database, const QString &path, const QString &action, const QJsonObject &metadata, const QStringList &affectedTargets, const DesktopTrackedPath *trackedState, bool mergeExisting, QString *error)
{
    QJsonObject resolvedMetadata = metadata;
    QStringList resolvedTargets = normalizedTargets(affectedTargets);
    QString existingAction;
    QJsonObject existingMetadata;
    QStringList existingTargets;
    bool existingHasTrackedState = false;
    DesktopTrackedPath existingTrackedState;
    QSqlQuery existing(database);
    existing.prepare(QStringLiteral("SELECT action, metadata_json, affected_targets_json, has_tracked_state, exists_flag, size_bytes, modified_at_ms, sha256 FROM desktop_pending_paths WHERE path = ?"));
    existing.addBindValue(path);
    if (!existing.exec()) {
        return fail(error, existing.lastError().text());
    }
    if (existing.next()) {
        existingAction = existing.value(0).toString();
        existingMetadata = parsedObject(existing.value(1));
        existingTargets = parsedTargets(existing.value(2));
        existingHasTrackedState = existing.value(3).toBool();
        existingTrackedState.path = path;
        existingTrackedState.exists = existing.value(4).toBool();
        existingTrackedState.sizeBytes = existing.value(5).toLongLong();
        existingTrackedState.modifiedAtMs = existing.value(6).toLongLong();
        existingTrackedState.sha256 = existing.value(7).toString();
        existingTrackedState.metadata = existingMetadata;
        existingTrackedState.affectedTargets = existingTargets;
        if (mergeExisting) {
            for (auto iterator = metadata.begin(); iterator != metadata.end(); ++iterator) {
                existingMetadata.insert(iterator.key(), iterator.value());
            }
            resolvedMetadata = existingMetadata;
        }
        resolvedTargets.append(existingTargets);
        resolvedTargets = normalizedTargets(resolvedTargets);
        const bool trackedStateUnchanged = trackedState ? existingHasTrackedState && trackedPathsEqual(existingTrackedState, *trackedState) : !existingHasTrackedState;
        if (existingAction == action && existingMetadata == resolvedMetadata && existingTargets == resolvedTargets && trackedStateUnchanged) {
            return true;
        }
    }

    const quint64 generation = nextGeneration(database, error);
    if (generation == 0) {
        return false;
    }
    QSqlQuery upsert(database);
    upsert.prepare(QStringLiteral(
        "INSERT INTO desktop_pending_paths(path, generation, action, metadata_json, affected_targets_json, updated_at_ms, has_tracked_state, exists_flag, size_bytes, modified_at_ms, sha256) VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(path) DO UPDATE SET generation=excluded.generation, action=excluded.action, metadata_json=excluded.metadata_json, affected_targets_json=excluded.affected_targets_json, updated_at_ms=excluded.updated_at_ms, "
        "has_tracked_state=excluded.has_tracked_state, exists_flag=excluded.exists_flag, size_bytes=excluded.size_bytes, modified_at_ms=excluded.modified_at_ms, sha256=excluded.sha256"));
    upsert.addBindValue(path);
    upsert.addBindValue(static_cast<qint64>(generation));
    upsert.addBindValue(action);
    upsert.addBindValue(jsonText(resolvedMetadata));
    upsert.addBindValue(targetsText(resolvedTargets));
    upsert.addBindValue(QDateTime::currentMSecsSinceEpoch());
    upsert.addBindValue(trackedState != nullptr);
    upsert.addBindValue(trackedState ? trackedState->exists : action != QLatin1String("delete"));
    upsert.addBindValue(trackedState ? trackedState->sizeBytes : -1);
    upsert.addBindValue(trackedState ? trackedState->modifiedAtMs : -1);
    upsert.addBindValue(trackedState && !trackedState->sha256.isEmpty() ? trackedState->sha256 : QStringLiteral(""));
    return upsert.exec() || fail(error, upsert.lastError().text());
}

QList<DesktopPendingFile> readPendingFiles(QSqlDatabase &database, quint64 maximumGeneration, QString *error)
{
    QList<DesktopPendingFile> files;
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT path, generation, action, metadata_json, affected_targets_json, has_tracked_state, exists_flag, size_bytes, modified_at_ms, sha256 FROM desktop_pending_paths WHERE generation <= ? ORDER BY path"));
    query.addBindValue(static_cast<qint64>(std::min<quint64>(maximumGeneration, static_cast<quint64>(std::numeric_limits<qint64>::max()))));
    if (!query.exec()) {
        fail(error, query.lastError().text());
        return {};
    }
    while (query.next()) {
        DesktopPendingFile file;
        file.path = query.value(0).toString();
        file.generation = query.value(1).toULongLong();
        file.action = query.value(2).toString();
        file.metadata = parsedObject(query.value(3));
        file.affectedTargets = parsedTargets(query.value(4));
        file.hasTrackedState = query.value(5).toBool();
        file.trackedState.path = file.path;
        file.trackedState.exists = query.value(6).toBool();
        file.trackedState.sizeBytes = query.value(7).toLongLong();
        file.trackedState.modifiedAtMs = query.value(8).toLongLong();
        file.trackedState.sha256 = query.value(9).toString();
        file.trackedState.metadata = file.metadata;
        file.trackedState.affectedTargets = file.affectedTargets;
        files.append(file);
    }
    return files;
}

QSet<QString> pathsWithOutstandingClaims(QSqlDatabase &database, QString *error)
{
    QSet<QString> paths;
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT DISTINCT path FROM desktop_pending_claims"))) {
        fail(error, query.lastError().text());
        return {};
    }
    while (query.next()) {
        paths.insert(query.value(0).toString());
    }
    return paths;
}

bool claimSnapshot(QSqlDatabase &database, const DesktopSyncSnapshot &snapshot, const QString &claimId, QString *error)
{
    const QString persistedClaimId = claimId.isNull() ? QStringLiteral("") : claimId;
    QSqlQuery claim(database);
    claim.prepare(
        QStringLiteral("INSERT INTO desktop_pending_claims(path, generation, claimed_at_ms, claim_id) "
                       "VALUES(?, ?, ?, ?) "
                       "ON CONFLICT(path, generation) DO UPDATE SET "
                       "claimed_at_ms=MIN(desktop_pending_claims.claimed_at_ms, excluded.claimed_at_ms), "
                       "claim_id=CASE WHEN desktop_pending_claims.claim_id = '' "
                       "THEN excluded.claim_id ELSE desktop_pending_claims.claim_id END"));
    const qint64 claimedAt = QDateTime::currentMSecsSinceEpoch();
    for (const DesktopPendingFile &file : snapshot.files) {
        claim.bindValue(0, file.path);
        claim.bindValue(1, static_cast<qint64>(file.generation));
        claim.bindValue(2, claimedAt);
        claim.bindValue(3, persistedClaimId);
        if (!claim.exec()) {
            return fail(error, claim.lastError().text());
        }
    }
    return true;
}

bool clearClaimsThrough(QSqlDatabase &database, const QString &path, quint64 generation, QString *error)
{
    QSqlQuery clear(database);
    clear.prepare(QStringLiteral("DELETE FROM desktop_pending_claims WHERE path = ? AND generation <= ?"));
    clear.addBindValue(path);
    clear.addBindValue(static_cast<qint64>(generation));
    return clear.exec() || fail(error, clear.lastError().text());
}

bool safeLibraryFilename(const QString &filename)
{
    return !filename.isEmpty() && QFileInfo(filename).fileName() == filename && filename.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) && filename.compare(QStringLiteral(".pdf"), Qt::CaseInsensitive) != 0 &&
        !filename.contains(QLatin1Char('\\')) && !filename.contains(QChar::Null);
}

QString libraryOperationId(const QJsonObject &mutation)
{
    const QString requestId = mutation.value(QStringLiteral("request_id")).toString().trimmed();
    if (!requestId.isEmpty()) {
        return requestId;
    }
    return QStringLiteral("legacy-library-%1").arg(QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(mutation).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex()));
}

bool validateLibraryMutation(const QJsonObject &mutation, QString *error)
{
    const QString type = mutation.value(QStringLiteral("type")).toString();
    const QString filename = mutation.value(QStringLiteral("filename")).toString();
    const QString sha256 = mutation.value(QStringLiteral("sha256")).toString();
    const QString operationId = libraryOperationId(mutation);
    if (!QSet<QString> {QStringLiteral("add"), QStringLiteral("rename"), QStringLiteral("archive"), QStringLiteral("remove")}.contains(type) || !safeLibraryFilename(filename) || !validSha256(sha256) || operationId.isEmpty() ||
        operationId.size() > 512 || std::any_of(operationId.cbegin(), operationId.cend(), [](QChar character) { return character == QChar::Null || character == QLatin1Char('\r') || character == QLatin1Char('\n'); })) {
        return fail(error, QStringLiteral("invalid desktop Library operation"));
    }
    if (type == QLatin1String("add")) {
        if (mutation.value(QStringLiteral("local_path")).toString().isEmpty()) {
            return fail(error, QStringLiteral("desktop Library add operation has no local PDF"));
        }
    } else if (type == QLatin1String("rename")) {
        const QString displayName = mutation.value(QStringLiteral("display_name")).toString().trimmed();
        if (displayName.isEmpty() || displayName.size() > 200) {
            return fail(error, QStringLiteral("desktop Library rename operation has an invalid display name"));
        }
    } else if (type == QLatin1String("archive") && !mutation.value(QStringLiteral("archived")).isBool()) {
        return fail(error, QStringLiteral("desktop Library archive operation has no state"));
    }
    return true;
}

DesktopLibraryOperation libraryOperationFromQuery(QSqlQuery &query)
{
    DesktopLibraryOperation operation;
    operation.operationId = query.value(0).toString();
    operation.generation = query.value(1).toULongLong();
    operation.type = query.value(2).toString();
    operation.filename = query.value(3).toString();
    operation.mutation = parsedObject(query.value(4));
    operation.state = query.value(5).toString();
    operation.createdAtMs = query.value(6).toLongLong();
    operation.completedAtMs = query.value(7).toLongLong();
    return operation;
}

bool setLibraryOperationState(QSqlDatabase &database, const QString &operationId, quint64 generation, const QString &state, QString *error)
{
    QSqlQuery update(database);
    update.prepare(
        QStringLiteral("UPDATE desktop_library_operations "
                       "SET state = ?, completed_at_ms = ? "
                       "WHERE operation_id = ? AND generation = ?"));
    update.addBindValue(state);
    update.addBindValue(state == QLatin1String("pending") || state == QLatin1String("in_flight") ? 0 : QDateTime::currentMSecsSinceEpoch());
    update.addBindValue(operationId);
    update.addBindValue(static_cast<qint64>(generation));
    return (update.exec() && update.numRowsAffected() == 1) || fail(error, update.lastError().text().isEmpty() ? QStringLiteral("desktop Library operation generation changed") : update.lastError().text());
}

DesktopLibraryEnqueueResult enqueueLibraryOperationInTransaction(QSqlDatabase &database, const QJsonObject &mutation, const QString &importedFrom)
{
    DesktopLibraryEnqueueResult result;
    if (!validateLibraryMutation(mutation, &result.error)) {
        return result;
    }
    const QString operationId = libraryOperationId(mutation);
    const QString type = mutation.value(QStringLiteral("type")).toString();
    const QString filename = mutation.value(QStringLiteral("filename")).toString();

    QSqlQuery sameId(database);
    sameId.prepare(
        QStringLiteral("SELECT operation_id, generation, type, filename, mutation_json, state, created_at_ms, completed_at_ms "
                       "FROM desktop_library_operations WHERE operation_id = ?"));
    sameId.addBindValue(operationId);
    if (!sameId.exec()) {
        result.error = sameId.lastError().text();
        return result;
    }
    if (sameId.next()) {
        result.operation = libraryOperationFromQuery(sameId);
        if (result.operation.mutation != mutation) {
            result.error = QStringLiteral("desktop Library operation ID has conflicting content");
            return result;
        }
        result.success = true;
        result.queued = result.operation.active();
        result.cancelledPendingAdd = result.operation.state == QLatin1String("cancelled");
        return result;
    }

    QList<DesktopLibraryOperation> active;
    QSqlQuery pending(database);
    pending.prepare(
        QStringLiteral("SELECT operation_id, generation, type, filename, mutation_json, state, created_at_ms, completed_at_ms "
                       "FROM desktop_library_operations "
                       "WHERE filename = ? AND state IN ('pending', 'in_flight') ORDER BY generation"));
    pending.addBindValue(filename);
    if (!pending.exec()) {
        result.error = pending.lastError().text();
        return result;
    }
    while (pending.next()) {
        active.append(libraryOperationFromQuery(pending));
    }
    if (type == QLatin1String("add") && !active.isEmpty()) {
        result.error = QStringLiteral("a desktop Library operation is already pending for this PDF");
        return result;
    }

    bool pendingAdd = false;
    bool inFlightAdd = false;
    for (const DesktopLibraryOperation &operation : std::as_const(active)) {
        if (operation.type == QLatin1String("add")) {
            pendingAdd = pendingAdd || operation.state == QLatin1String("pending");
            inFlightAdd = inFlightAdd || operation.state == QLatin1String("in_flight");
        }
        const bool supersedePending =
            operation.state == QLatin1String("pending") && ((type == QLatin1String("rename") && operation.type == type) || (type == QLatin1String("archive") && operation.type == type) || type == QLatin1String("remove"));
        if (supersedePending && !setLibraryOperationState(database, operation.operationId, operation.generation, QStringLiteral("superseded"), &result.error)) {
            return result;
        }
    }

    const quint64 generation = nextGeneration(database, &result.error);
    if (generation == 0) {
        return result;
    }
    const bool cancelRemove = type == QLatin1String("remove") && pendingAdd && !inFlightAdd;
    const QString state = cancelRemove ? QStringLiteral("cancelled") : QStringLiteral("pending");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSqlQuery insert(database);
    insert.prepare(
        QStringLiteral("INSERT INTO desktop_library_operations("
                       "operation_id, generation, type, filename, mutation_json, state, "
                       "created_at_ms, completed_at_ms, imported_from) "
                       "VALUES(?, ?, ?, ?, ?, ?, ?, ?, ?)"));
    insert.addBindValue(operationId);
    insert.addBindValue(static_cast<qint64>(generation));
    insert.addBindValue(type);
    insert.addBindValue(filename);
    insert.addBindValue(jsonText(mutation));
    insert.addBindValue(state);
    insert.addBindValue(now);
    insert.addBindValue(cancelRemove ? now : 0);
    insert.addBindValue(importedFrom.isEmpty() ? QStringLiteral("") : importedFrom);
    if (!insert.exec()) {
        result.error = insert.lastError().text();
        return result;
    }
    result.success = true;
    result.queued = !cancelRemove;
    result.cancelledPendingAdd = cancelRemove;
    result.operation = DesktopLibraryOperation {operationId, generation, type, filename, mutation, state, now, cancelRemove ? now : 0};
    return result;
}

}

bool isPotentialDesktopSourcePath(const QString &path)
{
    return isPotentialDesktopSourcePathImpl(path);
}

bool isEligibleDesktopSourceDirectory(const QString &path)
{
    return isEligibleDesktopSourceDirectoryImpl(path);
}

bool DesktopSyncSnapshot::isEmpty() const
{
    return files.isEmpty();
}

bool DesktopLibraryOperation::active() const
{
    return state == QLatin1String("pending") || state == QLatin1String("in_flight");
}

DesktopSyncStore::DesktopSyncStore(QString databasePath)
    : m_databasePath(std::move(databasePath))
    , m_connectionName(QStringLiteral("thoughts-desktop-sync-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)))
{
}

DesktopSyncStore::~DesktopSyncStore()
{
    if (QSqlDatabase::contains(m_connectionName)) {
        {
            QSqlDatabase database = QSqlDatabase::database(m_connectionName, false);
            database.close();
        }
        QSqlDatabase::removeDatabase(m_connectionName);
    }
    forgetAnnouncedChanges(m_connectionName);
}

QString DesktopSyncStore::databasePath() const
{
    return m_databasePath;
}

bool DesktopSyncStore::open(QString *error)
{
    clearError(error);
    if (initialized()) {
        return true;
    }
    if (m_databasePath.isEmpty()) {
        return fail(error, QStringLiteral("desktop synchronization database path is empty"));
    }
    if (m_databasePath != QLatin1String(":memory:") && !QDir().mkpath(QFileInfo(m_databasePath).absolutePath())) {
        return fail(error, QStringLiteral("could not create the desktop synchronization database directory"));
    }

    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_connectionName);
    database.setDatabaseName(m_databasePath);
    if (!database.open()) {
        const QString message = database.lastError().text();
        database = QSqlDatabase();
        QSqlDatabase::removeDatabase(m_connectionName);
        return fail(error, message);
    }
    const QStringList pragmas {QStringLiteral("PRAGMA journal_mode=WAL"), QStringLiteral("PRAGMA synchronous=FULL"), QStringLiteral("PRAGMA foreign_keys=ON"), QStringLiteral("PRAGMA busy_timeout=5000")};
    for (const QString &pragma : pragmas) {
        if (!execStatement(database, pragma, error)) {
            return false;
        }
    }
    const QStringList schema {
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_sync_metadata(key TEXT PRIMARY KEY, value TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_pending_paths(path TEXT PRIMARY KEY, generation INTEGER NOT NULL, action TEXT NOT NULL CHECK(action IN ('upsert', 'delete')), metadata_json TEXT NOT NULL DEFAULT '{}', "
                       "affected_targets_json TEXT NOT NULL DEFAULT '[]', updated_at_ms INTEGER NOT NULL, has_tracked_state INTEGER NOT NULL DEFAULT 0, exists_flag INTEGER NOT NULL DEFAULT 1, size_bytes INTEGER NOT NULL DEFAULT -1, "
                       "modified_at_ms INTEGER NOT NULL DEFAULT -1, sha256 TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS desktop_pending_generation ON desktop_pending_paths(generation)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_pending_claims(path TEXT NOT NULL, generation INTEGER NOT NULL, claimed_at_ms INTEGER NOT NULL, claim_id TEXT NOT NULL DEFAULT '', PRIMARY KEY(path, generation))"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS desktop_pending_claims_generation ON desktop_pending_claims(generation)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_source_baselines(path TEXT PRIMARY KEY, exists_flag INTEGER NOT NULL, size_bytes INTEGER NOT NULL, modified_at_ms INTEGER NOT NULL, sha256 TEXT NOT NULL DEFAULT '', metadata_json "
                       "TEXT NOT NULL DEFAULT '{}', affected_targets_json TEXT NOT NULL DEFAULT '[]')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_observed_paths(path TEXT PRIMARY KEY, exists_flag INTEGER NOT NULL, size_bytes INTEGER NOT NULL, modified_at_ms INTEGER NOT NULL, sha256 TEXT NOT NULL DEFAULT '', metadata_json "
                       "TEXT NOT NULL DEFAULT '{}', affected_targets_json TEXT NOT NULL DEFAULT '[]')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_sync_frontier(singleton INTEGER PRIMARY KEY CHECK(singleton = 1), library_revision TEXT NOT NULL DEFAULT '', source_frontier INTEGER NOT NULL DEFAULT -1, comment_frontier INTEGER "
                       "NOT NULL DEFAULT -1, annotation_frontier INTEGER NOT NULL DEFAULT -1, cursor INTEGER NOT NULL DEFAULT -1, metadata_json TEXT NOT NULL DEFAULT '{}')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_verified_objects(sha256 TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL, kind TEXT NOT NULL, semantic_role TEXT NOT NULL, owning_publication TEXT NOT NULL, verified_at_ms INTEGER "
                       "NOT NULL, metadata_json TEXT NOT NULL DEFAULT '{}')"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_logical_operations(operation_id TEXT PRIMARY KEY, device_sequence INTEGER NOT NULL, operation_json TEXT NOT NULL, acknowledged INTEGER NOT NULL DEFAULT 0, created_at_ms INTEGER "
                       "NOT NULL, acknowledged_at_ms INTEGER NOT NULL DEFAULT 0, imported_from TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS desktop_logical_operations_pending ON desktop_logical_operations(acknowledged, device_sequence)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS desktop_library_operations(operation_id TEXT PRIMARY KEY, generation INTEGER NOT NULL UNIQUE, type TEXT NOT NULL CHECK(type IN ('add', 'rename', 'archive', 'remove')), filename TEXT NOT "
                       "NULL, mutation_json TEXT NOT NULL, state TEXT NOT NULL CHECK(state IN ('pending', 'in_flight', 'superseded', 'cancelled', 'acknowledged')), created_at_ms INTEGER NOT NULL, completed_at_ms INTEGER NOT NULL DEFAULT "
                       "0, imported_from TEXT NOT NULL DEFAULT '')"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS desktop_library_operations_active ON desktop_library_operations(state, generation)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS desktop_library_operations_filename ON desktop_library_operations(filename, state, generation)"),
    };
    for (const QString &statement : schema) {
        if (!execStatement(database, statement, error)) {
            return false;
        }
    }
    const QList<std::tuple<QString, QString, QString>> pendingColumns {
        {QStringLiteral("has_tracked_state"), QStringLiteral("INTEGER"), QStringLiteral("NOT NULL DEFAULT 0")},
        {QStringLiteral("exists_flag"), QStringLiteral("INTEGER"), QStringLiteral("NOT NULL DEFAULT 1")},
        {QStringLiteral("size_bytes"), QStringLiteral("INTEGER"), QStringLiteral("NOT NULL DEFAULT -1")},
        {QStringLiteral("modified_at_ms"), QStringLiteral("INTEGER"), QStringLiteral("NOT NULL DEFAULT -1")},
        {QStringLiteral("sha256"), QStringLiteral("TEXT"), QStringLiteral("NOT NULL DEFAULT ''")},
    };
    for (const auto &[column, type, constraint] : pendingColumns) {
        if (!ensureColumn(database, QStringLiteral("desktop_pending_paths"), column, type + QLatin1Char(' ') + constraint, error)) {
            return false;
        }
    }
    if (!ensureColumn(database, QStringLiteral("desktop_pending_claims"), QStringLiteral("claim_id"), QStringLiteral("TEXT NOT NULL DEFAULT ''"), error)) {
        return false;
    }
    return true;
}

bool DesktopSyncStore::initialized() const
{
    const QSqlDatabase database = databaseFor(m_connectionName);
    return database.isValid() && database.isOpen();
}

DesktopSyncSummary DesktopSyncStore::summary(QString *error) const
{
    clearError(error);
    DesktopSyncSummary result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return result;
    }
    const quint64 maximum = std::numeric_limits<quint64>::max();
    const QList<DesktopPendingFile> files = readPendingFiles(database, maximum, error);
    if (error && !error->isEmpty()) {
        return result;
    }
    QSet<QString> targets;
    for (const DesktopPendingFile &file : files) {
        result.dirtyPaths.append(file.path);
        const bool unresolved = file.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved");
        if (file.affectedTargets.isEmpty() || unresolved) {
            result.filesByTarget[QStringLiteral("Shared sources")].append(file);
            continue;
        }
        for (const QString &target : file.affectedTargets) {
            targets.insert(target);
            result.filesByTarget[target].append(file);
        }
    }
    result.pendingFileCount = files.size();
    result.dirtyTargets = targets.values();
    std::sort(result.dirtyPaths.begin(), result.dirtyPaths.end());
    std::sort(result.dirtyTargets.begin(), result.dirtyTargets.end());
    const bool reconciled = metadataValue(database, QStringLiteral("baseline_reconciled")) == QLatin1String("1");
    result.status = !reconciled ? DesktopSyncStatus::Unknown : (files.isEmpty() ? DesktopSyncStatus::Clean : DesktopSyncStatus::Pending);
    return result;
}

QStringList DesktopSyncStore::dirtyPaths(QString *error) const
{
    return summary(error).dirtyPaths;
}

QStringList DesktopSyncStore::dirtyTargets(QString *error) const
{
    return summary(error).dirtyTargets;
}

QHash<QString, QList<DesktopPendingFile>> DesktopSyncStore::pendingFilesByTarget(QString *error) const
{
    return summary(error).filesByTarget;
}

bool DesktopSyncStore::markReconciliationRequired(QString *error)
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    return database.isValid() && database.isOpen() ? setMetadata(database, QStringLiteral("baseline_reconciled"), QStringLiteral("0"), error) : fail(error, QStringLiteral("desktop synchronization store is not open"));
}

QList<DesktopTrackedPath> DesktopSyncStore::baselines(QString *error) const
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return {};
    }
    const QHash<QString, DesktopTrackedPath> byPath = readTrackedPaths(database, QStringLiteral("desktop_source_baselines"), error);
    if (error && !error->isEmpty()) {
        return {};
    }
    QList<DesktopTrackedPath> result = byPath.values();
    std::sort(result.begin(), result.end(), [](const DesktopTrackedPath &left, const DesktopTrackedPath &right) { return left.path < right.path; });
    return result;
}

bool DesktopSyncStore::readMonitoringSnapshot(const QString &databasePath, QList<DesktopTrackedPath> *baselines,
                                             DesktopSyncSnapshot *pending, QString *error)
{
    QString localError;
    if (!error) { error = &localError; }
    clearError(error);
    const QString connection = QStringLiteral("thoughts-monitoring-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool success = false;
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        database.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY;QSQLITE_BUSY_TIMEOUT=1000"));
        database.setDatabaseName(databasePath);
        if (!database.open() || !database.transaction()) {
            fail(error, database.lastError().text());
        } else {
            *baselines = readTrackedPaths(database, QStringLiteral("desktop_source_baselines"), error).values();
            std::sort(baselines->begin(), baselines->end(), [](const DesktopTrackedPath &left, const DesktopTrackedPath &right) { return left.path < right.path; });
            if (!error || error->isEmpty()) {
                pending->throughGeneration = metadataValue(database, QStringLiteral("last_generation")).toULongLong();
                pending->files = readPendingFiles(database, pending->throughGeneration, error);
            }
            success = (!error || error->isEmpty()) && database.commit();
            if (!success) {
                if (!error || error->isEmpty()) { fail(error, database.lastError().text()); }
                database.rollback();
            }
        }
        database.close();
    }
    QSqlDatabase::removeDatabase(connection);
    if (!success) { baselines->clear(); *pending = {}; }
    return success;
}

QList<DesktopTrackedPath> DesktopSyncStore::trackingPaths(QString *error) const
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return {};
    }
    const QHash<QString, DesktopTrackedPath> baselinesByPath = readTrackedPaths(database, QStringLiteral("desktop_source_baselines"), error);
    const QHash<QString, DesktopTrackedPath> observedByPath = readTrackedPaths(database, QStringLiteral("desktop_observed_paths"), error);
    if (error && !error->isEmpty()) {
        return {};
    }
    QHash<QString, DesktopTrackedPath> tracked;
    for (auto iterator = baselinesByPath.cbegin(); iterator != baselinesByPath.cend(); ++iterator) {
        DesktopTrackedPath value = iterator.value();
        const auto observed = observedByPath.constFind(iterator.key());
        if (observed != observedByPath.cend()) {
            // Accepted baselines own semantic identity while observations own
            // the current cheap filesystem identity. This lets publication
            // resolve an Unknown path without discarding a newer local save.
            value.exists = observed->exists;
            value.sizeBytes = observed->sizeBytes;
            value.modifiedAtMs = observed->modifiedAtMs;
            value.sha256 = observed->sha256;
            for (const QString &key : {QStringLiteral("_local_ctime_ms"), QStringLiteral("_local_file_identity"), QStringLiteral("_tracking_bootstrap")}) {
                if (observed->metadata.contains(key)) {
                    value.metadata.insert(key, observed->metadata.value(key));
                }
            }
        }
        tracked.insert(iterator.key(), value);
    }
    QStringList activeDirectories;
    for (auto iterator = baselinesByPath.cbegin(); iterator != baselinesByPath.cend(); ++iterator) {
        if (discoveryDirectoryBaseline(iterator.value())) {
            activeDirectories.append(iterator.key());
        }
    }
    for (auto iterator = observedByPath.cbegin(); iterator != observedByPath.cend(); ++iterator) {
        const QJsonObject metadata = iterator->metadata;
        const bool covered = std::any_of(activeDirectories.cbegin(), activeDirectories.cend(), [&iterator](const QString &directory) { return pathIsWithin(iterator.key(), directory); });
        const bool directory = metadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory");
        if (!baselinesByPath.contains(iterator.key()) && covered
            && ((directory && isEligibleDesktopSourceDirectoryImpl(iterator.key()))
                || (!directory && isPotentialDesktopSourcePathImpl(iterator.key())))) {
            // Coverage descendants are durable observations rather than
            // publication baselines. Keep both directory sensors and
            // plausible source files across restarts so an unchanged accepted
            // Library cannot rediscover them as additions.
            tracked.insert(iterator.key(), iterator.value());
        }
    }
    const QList<DesktopPendingFile> pending = readPendingFiles(database, std::numeric_limits<quint64>::max(), error);
    if (error && !error->isEmpty()) {
        return {};
    }
    for (const DesktopPendingFile &file : pending) {
        DesktopTrackedPath path = file.hasTrackedState ? file.trackedState : observedByPath.value(file.path, baselinesByPath.value(file.path));
        path.path = file.path;
        if (path.metadata.isEmpty()) {
            path.metadata = file.metadata;
        }
        if (path.affectedTargets.isEmpty()) {
            path.affectedTargets = file.affectedTargets;
        }
        tracked.insert(file.path, path);
    }
    QList<DesktopTrackedPath> result = tracked.values();
    std::sort(result.begin(), result.end(), [](const DesktopTrackedPath &left, const DesktopTrackedPath &right) { return left.path < right.path; });
    return result;
}

bool DesktopSyncStore::replaceBaselines(const QList<DesktopTrackedPath> &baselines, QString *error)
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    QSqlQuery clear(database);
    if (!clear.exec(QStringLiteral("DELETE FROM desktop_source_baselines"))) {
        database.rollback();
        return fail(error, clear.lastError().text());
    }
    QSet<QString> seen;
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO desktop_source_baselines(path, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json) VALUES(?, ?, ?, ?, ?, ?, ?)"));
    for (DesktopTrackedPath baseline : baselines) {
        baseline.path = normalizedPath(baseline.path);
        baseline.affectedTargets = normalizedTargets(baseline.affectedTargets);
        if (baseline.path.isEmpty() || seen.contains(baseline.path) || (!baseline.sha256.isEmpty() && !validSha256(baseline.sha256))) {
            database.rollback();
            return fail(error, QStringLiteral("invalid or duplicate desktop source baseline"));
        }
        seen.insert(baseline.path);
        if (!insertTrackedPath(insert, baseline, error)) {
            database.rollback();
            return false;
        }
    }
    if (!setMetadata(database, QStringLiteral("baseline_reconciled"), QStringLiteral("0"), error) || !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, error && !error->isEmpty() ? *error : database.lastError().text());
    }
    return true;
}

bool DesktopSyncStore::reconcileStartupMetadata(const QList<DesktopTrackedPath> &observedPaths, bool observationBootstrapComplete, QString *error)
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    QHash<QString, DesktopTrackedPath> baselines = readTrackedPaths(database, QStringLiteral("desktop_source_baselines"), error);
    QHash<QString, DesktopTrackedPath> previous = readTrackedPaths(database, QStringLiteral("desktop_observed_paths"), error);
    if (error && !error->isEmpty()) {
        database.rollback();
        return false;
    }
    QHash<QString, DesktopTrackedPath> observed;
    for (DesktopTrackedPath path : observedPaths) {
        path.path = normalizedPath(path.path);
        path.affectedTargets = normalizedTargets(path.affectedTargets);
        if (path.path.isEmpty() || observed.contains(path.path) || (!path.sha256.isEmpty() && !validSha256(path.sha256))) {
            database.rollback();
            return fail(error, QStringLiteral("invalid or duplicate observed desktop source path"));
        }
        observed.insert(path.path, path);
    }

    QStringList activeDirectories;
    for (auto iterator = baselines.cbegin(); iterator != baselines.cend(); ++iterator) {
        if (discoveryDirectoryBaseline(iterator.value())) {
            activeDirectories.append(iterator.key());
        }
    }
    const auto coveredByActiveDirectory = [&activeDirectories](const QString &path) { return std::any_of(activeDirectories.cbegin(), activeDirectories.cend(), [&path](const QString &directory) { return pathIsWithin(path, directory); }); };
    for (auto iterator = previous.begin(); iterator != previous.end();) {
        const bool directory = directoryBaseline(iterator.value());
        if (!baselines.contains(iterator.key())
            && (!coveredByActiveDirectory(iterator.key())
                || (directory ? !isEligibleDesktopSourceDirectoryImpl(iterator.key())
                              : !isPotentialDesktopSourcePathImpl(iterator.key())))) {
            iterator = previous.erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = observed.begin(); iterator != observed.end();) {
        const bool directory = directoryBaseline(iterator.value());
        if (!baselines.contains(iterator.key())
            && (!coveredByActiveDirectory(iterator.key())
                || (directory ? !isEligibleDesktopSourceDirectoryImpl(iterator.key())
                              : !isPotentialDesktopSourcePathImpl(iterator.key())))) {
            iterator = observed.erase(iterator);
        } else {
            ++iterator;
        }
    }

    QStringList bootstrappingDirectories;
    for (const QString &directory : std::as_const(activeDirectories)) {
        const auto previousDirectory = previous.constFind(directory);
        if (previousDirectory == previous.cend() || previousDirectory->metadata.value(QStringLiteral("_tracking_bootstrap")).toBool()) {
            bootstrappingDirectories.append(directory);
        }
    }
    const auto coveredByBootstrap = [&bootstrappingDirectories](const QString &path) {
        return std::any_of(bootstrappingDirectories.cbegin(), bootstrappingDirectories.cend(), [&path](const QString &directory) { return pathIsWithin(path, directory); });
    };
    for (const QString &directory : std::as_const(bootstrappingDirectories)) {
        auto current = observed.find(directory);
        if (current == observed.end()) {
            continue;
        }
        if (observationBootstrapComplete) {
            current->metadata.remove(QStringLiteral("_tracking_bootstrap"));
        } else {
            current->metadata.insert(QStringLiteral("_tracking_bootstrap"), true);
        }
    }

    QSet<QString> allPaths(baselines.keyBegin(), baselines.keyEnd());
    allPaths.unite(QSet<QString>(previous.keyBegin(), previous.keyEnd()));
    allPaths.unite(QSet<QString>(observed.keyBegin(), observed.keyEnd()));
    QStringList orderedPaths = allPaths.values();
    std::sort(orderedPaths.begin(), orderedPaths.end());
    QSet<QString> pendingPaths;
    QSet<QString> pendingDeletions;
    for (const DesktopPendingFile &pending : readPendingFiles(database,
                                                              std::numeric_limits<quint64>::max(),
                                                              error)) {
        pendingPaths.insert(pending.path);
        if (pending.action == QLatin1String("delete")) pendingDeletions.insert(pending.path);
    }
    if (error && !error->isEmpty()) {
        database.rollback();
        return false;
    }
    const QSet<QString> claimedPaths = pathsWithOutstandingClaims(database, error);
    if (error && !error->isEmpty()) {
        database.rollback();
        return false;
    }
    for (const QString &path : std::as_const(orderedPaths)) {
        const bool hasBaseline = baselines.contains(path);
        DesktopTrackedPath current = observed.value(path);
        if (!observed.contains(path)) {
            current.path = path;
            current.exists = false;
            current.sizeBytes = -1;
            current.modifiedAtMs = -1;
            current.metadata = previous.value(path, baselines.value(path)).metadata;
            current.affectedTargets = previous.value(path).affectedTargets;
            current.affectedTargets.append(baselines.value(path).affectedTargets);
            current.affectedTargets = normalizedTargets(current.affectedTargets);
        }
        const bool directorySentinel = directoryBaseline(current);
        const bool bootstrappingObservation = !hasBaseline && coveredByBootstrap(path);
        if (bootstrappingObservation && !directorySentinel) {
            current.metadata.insert(QStringLiteral("_tracking_observation"), QStringLiteral("coverage_seed"));
        } else if (hasBaseline) {
            current.metadata.remove(QStringLiteral("_tracking_observation"));
        }
        // An observed path without a publication baseline is clean only when
        // it was already part of the initial coverage census. Once a newly
        // discovered file has entered the durable outbox, an unchanged later
        // scan must not silently clear it merely because it now matches the
        // preceding observation.
        const bool previousObservationMatches = !hasBaseline
            && !pendingPaths.contains(path)
            && previous.contains(path)
            && trackedPathsEqual(previous.value(path), current);
        const bool ignoredObservation = !hasBaseline && current.metadata.value(QStringLiteral("source_kind")).toString() == QLatin1String("ignored");
        // A file never published that is gone again leaves nothing to publish:
        // Fedora has no copy to delete.
        const bool matches =
            hasBaseline ? trackedPathsEqual(baselines.value(path), current) : (directorySentinel || bootstrappingObservation || previousObservationMatches || ignoredObservation || !current.exists);
        if (matches) {
            if (claimedPaths.contains(path)) {
                // A frozen generation may already be publishing bytes which no
                // longer match the local file. Preserve the current state as a
                // newer generation even when it equals the old baseline. Once
                // the frozen generation is accepted, this row restores the
                // server to the user's latest bytes.
                if (!coalescePendingPath(database, path, current.exists ? QStringLiteral("upsert") : QStringLiteral("delete"), current.metadata, current.affectedTargets, &current, false, error)) {
                    database.rollback();
                    return false;
                }
            } else {
                QSqlQuery remove(database);
                remove.prepare(QStringLiteral("DELETE FROM desktop_pending_paths WHERE path = ?"));
                remove.addBindValue(path);
                if (!remove.exec()) {
                    database.rollback();
                    return fail(error, remove.lastError().text());
                }
            }
        } else {
            current.metadata.remove(QStringLiteral("_tracking_observation"));
            QStringList targets = current.affectedTargets;
            targets.append(baselines.value(path).affectedTargets);
            targets.append(previous.value(path).affectedTargets);
            if (!coalescePendingPath(database, path, current.exists ? QStringLiteral("upsert") : QStringLiteral("delete"), current.metadata, normalizedTargets(targets), &current, false, error)) {
                database.rollback();
                return false;
            }
        }
        observed.insert(path, current);
    }

    // Earlier versions recorded such deletions as pending; they could never be
    // published and kept every later scan unauthoritative. A frozen claim on
    // one protects nothing either: there are no bytes and no Fedora copy, and
    // since its publication can never accept it, the claim would never clear.
    for (const QString &path : std::as_const(pendingDeletions)) {
        if (baselines.contains(path) || (observed.contains(path) && observed.value(path).exists)) continue;
        for (const QString &statement : {QStringLiteral("DELETE FROM desktop_pending_paths WHERE path = ? AND action = 'delete'"),
                                         QStringLiteral("DELETE FROM desktop_pending_claims WHERE path = ?")}) {
            QSqlQuery remove(database);
            remove.prepare(statement);
            remove.addBindValue(path);
            if (!remove.exec()) {
                database.rollback();
                return fail(error, remove.lastError().text());
            }
        }
    }

    QSqlQuery clearObserved(database);
    if (!clearObserved.exec(QStringLiteral("DELETE FROM desktop_observed_paths"))) {
        database.rollback();
        return fail(error, clearObserved.lastError().text());
    }
    QSqlQuery insertObserved(database);
    insertObserved.prepare(QStringLiteral("INSERT INTO desktop_observed_paths(path, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json) VALUES(?, ?, ?, ?, ?, ?, ?)"));
    for (const QString &path : std::as_const(orderedPaths)) {
        if (!insertTrackedPath(insertObserved, observed.value(path), error)) {
            database.rollback();
            return false;
        }
    }
    if (!setMetadata(database, QStringLiteral("baseline_reconciled"), observationBootstrapComplete ? QStringLiteral("1") : QStringLiteral("0"), error) || !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, error && !error->isEmpty() ? *error : database.lastError().text());
    }
    return true;
}

bool DesktopSyncStore::adoptLegacyCoverageBootstrapObservations(int expectedCount, int *adoptedCount, QString *error)
{
    clearError(error);
    if (adoptedCount) {
        *adoptedCount = 0;
    }
    if (expectedCount < 0) {
        return fail(error, QStringLiteral("legacy coverage repair requires an audited expected count"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    QSqlQuery candidates(database);
    if (!candidates.exec(QStringLiteral("SELECT p.path, p.generation, o.metadata_json "
                                        "FROM desktop_pending_paths p "
                                        "JOIN desktop_observed_paths o ON o.path = p.path "
                                        "LEFT JOIN desktop_source_baselines b ON b.path = p.path "
                                        "WHERE b.path IS NULL AND p.has_tracked_state = 1 "
                                        "AND NOT EXISTS (SELECT 1 FROM desktop_pending_claims c WHERE c.path = p.path) "
                                        "AND p.exists_flag = o.exists_flag AND p.size_bytes = o.size_bytes "
                                        "AND p.modified_at_ms = o.modified_at_ms AND p.sha256 = o.sha256 "
                                        "AND p.metadata_json = o.metadata_json "
                                        "AND p.affected_targets_json = o.affected_targets_json "
                                        "ORDER BY p.path"))) {
        database.rollback();
        return fail(error, candidates.lastError().text());
    }
    struct Candidate {
        QString path;
        quint64 generation = 0;
        QJsonObject metadata;
    };
    QList<Candidate> selected;
    while (candidates.next()) {
        selected.append(Candidate {candidates.value(0).toString(), candidates.value(1).toULongLong(), parsedObject(candidates.value(2))});
    }
    if (selected.size() != expectedCount) {
        database.rollback();
        return fail(error, QStringLiteral("legacy coverage repair found %1 exact observation(s), expected %2").arg(selected.size()).arg(expectedCount));
    }
    QSqlQuery markObserved(database);
    markObserved.prepare(QStringLiteral("UPDATE desktop_observed_paths SET metadata_json = ? WHERE path = ?"));
    QSqlQuery removePending(database);
    removePending.prepare(QStringLiteral("DELETE FROM desktop_pending_paths WHERE path = ? AND generation = ?"));
    for (Candidate candidate : std::as_const(selected)) {
        candidate.metadata.insert(QStringLiteral("_tracking_observation"), QStringLiteral("coverage_seed"));
        markObserved.bindValue(0, jsonText(candidate.metadata));
        markObserved.bindValue(1, candidate.path);
        if (!markObserved.exec()) {
            database.rollback();
            return fail(error, markObserved.lastError().text());
        }
        removePending.bindValue(0, candidate.path);
        removePending.bindValue(1, static_cast<qint64>(candidate.generation));
        if (!removePending.exec() || removePending.numRowsAffected() != 1) {
            database.rollback();
            return fail(error, removePending.lastError().text().isEmpty() ? QStringLiteral("legacy coverage observation changed during repair") : removePending.lastError().text());
        }
    }
    if (!commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, database.lastError().text());
    }
    if (adoptedCount) {
        *adoptedCount = selected.size();
    }
    return true;
}

bool DesktopSyncStore::recordPathChange(const QString &path, const QString &action, const QJsonObject &metadata, const QStringList &affectedTargets, QString *error)
{
    clearError(error);
    const QString cleanPath = normalizedPath(path);
    if (cleanPath.isEmpty() || (action != QLatin1String("upsert") && action != QLatin1String("delete"))) {
        return fail(error, QStringLiteral("invalid desktop source change"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    if (!coalescePendingPath(database, cleanPath, action, metadata, affectedTargets, nullptr, true, error) || !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, error && !error->isEmpty() ? *error : database.lastError().text());
    }
    return true;
}

DesktopSyncSnapshot DesktopSyncStore::freeze(QString *error) const
{
    return freezeForPublication(QString(), error);
}

DesktopSyncSnapshot DesktopSyncStore::freezeForPublication(const QString &claimId, QString *error) const
{
    clearError(error);
    DesktopSyncSnapshot snapshot;
    if (claimId.size() > 64 || claimId.contains(QLatin1Char('\r')) || claimId.contains(QLatin1Char('\n')) || claimId.contains(QChar::Null)) {
        fail(error, QStringLiteral("invalid desktop source publication claim ID"));
        return snapshot;
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    QString busy;
    if (!beginWriteTransaction(database, &busy)) {
        fail(error, busy);
        return snapshot;
    }
    bool ok = false;
    snapshot.throughGeneration = metadataValue(database, QStringLiteral("last_generation")).toULongLong(&ok);
    if (!ok) {
        snapshot.throughGeneration = 0;
    }
    snapshot.files = readPendingFiles(database, snapshot.throughGeneration, error);
    if ((error && !error->isEmpty()) || !claimSnapshot(database, snapshot, claimId, error) || !commitAndAnnounce(database)) {
        database.rollback();
        if (error && error->isEmpty()) {
            *error = database.lastError().text();
        }
        return {};
    }
    return snapshot;
}

bool DesktopSyncStore::acknowledge(const DesktopSyncSnapshot &snapshot, QString *error)
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    QString busy;
    if (!beginWriteTransaction(database, &busy)) {
        return fail(error, busy);
    }
    QSet<QString> seen;
    QSqlQuery pending(database);
    pending.prepare(QStringLiteral("SELECT has_tracked_state, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json FROM desktop_pending_paths WHERE path = ? AND generation = ?"));
    QSqlQuery updateBaseline(database);
    updateBaseline.prepare(
        QStringLiteral("INSERT INTO desktop_source_baselines(path, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json) VALUES(?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(path) DO UPDATE SET exists_flag=excluded.exists_flag, size_bytes=excluded.size_bytes, modified_at_ms=excluded.modified_at_ms, sha256=excluded.sha256, metadata_json=excluded.metadata_json, "
                       "affected_targets_json=excluded.affected_targets_json"));
    QSqlQuery remove(database);
    remove.prepare(QStringLiteral("DELETE FROM desktop_pending_paths WHERE path = ? AND generation = ?"));
    for (const DesktopPendingFile &file : snapshot.files) {
        const QString path = normalizedPath(file.path);
        if (path.isEmpty() || seen.contains(path) || file.generation == 0 || file.generation > snapshot.throughGeneration) {
            database.rollback();
            return fail(error, QStringLiteral("invalid frozen desktop synchronization snapshot"));
        }
        seen.insert(path);
        pending.bindValue(0, path);
        pending.bindValue(1, static_cast<qint64>(file.generation));
        if (!pending.exec()) {
            database.rollback();
            return fail(error, pending.lastError().text());
        }
        if (pending.next()) {
            DesktopTrackedPath persisted;
            persisted.path = path;
            persisted.exists = pending.value(1).toBool();
            persisted.sizeBytes = pending.value(2).toLongLong();
            persisted.modifiedAtMs = pending.value(3).toLongLong();
            persisted.sha256 = pending.value(4).toString();
            persisted.metadata = parsedObject(pending.value(5));
            persisted.affectedTargets = parsedTargets(pending.value(6));
            const bool hasTrackedState = pending.value(0).toBool();
            if (hasTrackedState != file.hasTrackedState || (hasTrackedState && !trackedPathsExactlyEqual(persisted, file.trackedState))) {
                database.rollback();
                return fail(error, QStringLiteral("frozen desktop synchronization state was modified"));
            }
        }
        // The remote publication has accepted these frozen bytes. Advance the
        // accepted baseline even when a newer local generation replaced the
        // exact row while the request was in flight.
        if (file.hasTrackedState) {
            if (!insertTrackedPath(updateBaseline, file.trackedState, error)) {
                database.rollback();
                return false;
            }
        }
        remove.bindValue(0, path);
        remove.bindValue(1, static_cast<qint64>(file.generation));
        if (!remove.exec()) {
            database.rollback();
            return fail(error, remove.lastError().text());
        }
        if (!clearClaimsThrough(database, path, file.generation, error)) {
            database.rollback();
            return false;
        }
    }
    if (!commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, database.lastError().text());
    }
    return true;
}

bool DesktopSyncStore::acceptBaselinesAndAcknowledge(const QList<DesktopTrackedPath> &baselines, const DesktopSyncSnapshot &snapshot, QString *error, const QString &publicationId)
{
    clearError(error);
    if (!publicationId.isEmpty() && (publicationId.size() > 64 || publicationId.contains(QLatin1Char('\r')) || publicationId.contains(QLatin1Char('\n')) || publicationId.contains(QChar::Null))) {
        return fail(error, QStringLiteral("invalid source publication ID"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    QString busy;
    if (!beginWriteTransaction(database, &busy)) {
        return fail(error, busy);
    }

    QHash<QString, DesktopTrackedPath> accepted;
    for (DesktopTrackedPath baseline : baselines) {
        baseline.path = normalizedPath(baseline.path);
        baseline.affectedTargets = normalizedTargets(baseline.affectedTargets);
        if (baseline.path.isEmpty() || accepted.contains(baseline.path) || (!baseline.sha256.isEmpty() && !validSha256(baseline.sha256))) {
            database.rollback();
            return fail(error, QStringLiteral("invalid or duplicate desktop source baseline"));
        }
        accepted.insert(baseline.path, baseline);
    }

    QSet<QString> seen;
    QSet<QString> acknowledged;
    QSqlQuery pending(database);
    pending.prepare(QStringLiteral("SELECT has_tracked_state, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json FROM desktop_pending_paths WHERE path = ? AND generation = ?"));
    for (const DesktopPendingFile &file : snapshot.files) {
        const QString path = normalizedPath(file.path);
        if (path.isEmpty() || seen.contains(path) || file.generation == 0 || file.generation > snapshot.throughGeneration) {
            database.rollback();
            return fail(error, QStringLiteral("invalid frozen desktop synchronization snapshot"));
        }
        seen.insert(path);
        pending.bindValue(0, path);
        pending.bindValue(1, static_cast<qint64>(file.generation));
        if (!pending.exec()) {
            database.rollback();
            return fail(error, pending.lastError().text());
        }
        if (pending.next()) {
            DesktopTrackedPath persisted;
            persisted.path = path;
            persisted.exists = pending.value(1).toBool();
            persisted.sizeBytes = pending.value(2).toLongLong();
            persisted.modifiedAtMs = pending.value(3).toLongLong();
            persisted.sha256 = pending.value(4).toString();
            persisted.metadata = parsedObject(pending.value(5));
            persisted.affectedTargets = parsedTargets(pending.value(6));
            const bool hasTrackedState = pending.value(0).toBool();
            if (hasTrackedState != file.hasTrackedState || (hasTrackedState && !trackedPathsExactlyEqual(persisted, file.trackedState))) {
                database.rollback();
                return fail(error, QStringLiteral("frozen desktop synchronization state was modified"));
            }
            acknowledged.insert(path);
        }
        // `accepted` is the authoritative result of the completed
        // publication. Keep it as the new baseline even if a newer local row
        // replaced the frozen generation; that row remains pending relative to
        // these server bytes.
        if (!clearClaimsThrough(database, path, file.generation, error)) {
            database.rollback();
            return false;
        }
    }

    QSqlQuery clear(database);
    if (!clear.exec(QStringLiteral("DELETE FROM desktop_source_baselines"))) {
        database.rollback();
        return fail(error, clear.lastError().text());
    }
    QStringList paths = accepted.keys();
    std::sort(paths.begin(), paths.end());
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO desktop_source_baselines(path, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json) VALUES(?, ?, ?, ?, ?, ?, ?)"));
    for (const QString &path : std::as_const(paths)) {
        if (!insertTrackedPath(insert, accepted.value(path), error)) {
            database.rollback();
            return false;
        }
    }
    // Do not replace the observation index with the canonical closure. It also
    // contains durable coverage sensors for pre-existing, nondependency paths.
    // Losing those rows makes the next restart report the whole project tree as
    // newly added. trackingPaths() merges accepted semantics with the retained
    // physical observations, and the next reconciliation prunes stale roots.
    QSqlQuery remove(database);
    remove.prepare(QStringLiteral("DELETE FROM desktop_pending_paths WHERE path = ? AND generation = ?"));
    for (const DesktopPendingFile &file : snapshot.files) {
        const QString path = normalizedPath(file.path);
        if (!acknowledged.contains(path)) {
            continue;
        }
        remove.bindValue(0, path);
        remove.bindValue(1, static_cast<qint64>(file.generation));
        if (!remove.exec()) {
            database.rollback();
            return fail(error, remove.lastError().text());
        }
    }
    if ((!publicationId.isEmpty() && !setMetadata(database, QStringLiteral("last_handled_source_publication_id"), publicationId, error)) || !setMetadata(database, QStringLiteral("baseline_reconciled"), QStringLiteral("0"), error) ||
        !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, error && !error->isEmpty() ? *error : database.lastError().text());
    }
    return true;
}

int DesktopSyncStore::reconcileAppliedSourcePublication(const QJsonObject &publication, QString *error)
{
    clearError(error);
    const QString publicationId = publication.value(QStringLiteral("publication_id")).toString().trimmed();
    const QString traceId = publication.value(QStringLiteral("trace_id")).toString().trimmed();
    const qint64 sourceFrontier = publication.value(QStringLiteral("source_frontier")).toInteger(-1);
    if (publicationId.isEmpty() || publicationId.size() > 64 || publicationId.contains(QLatin1Char('\r')) || publicationId.contains(QLatin1Char('\n')) || publicationId.contains(QChar::Null) || traceId.isEmpty() || traceId.size() > 64 ||
        traceId.contains(QLatin1Char('\r')) || traceId.contains(QLatin1Char('\n')) || traceId.contains(QChar::Null) || sourceFrontier < 0 || !publication.value(QStringLiteral("results")).isArray()) {
        return 0;
    }

    QHash<QString, QString> terminalShaByPath;
    for (const QJsonValue &value : publication.value(QStringLiteral("results")).toArray()) {
        const QJsonObject result = value.toObject();
        const QString outcome = result.value(QStringLiteral("outcome")).toString();
        if (outcome != QLatin1String("applied") && outcome != QLatin1String("merged") && outcome != QLatin1String("duplicate")) {
            continue;
        }
        const QString originalPath = result.value(QStringLiteral("path")).toString();
        const QString path = normalizedPath(originalPath);
        const QString sha256 = result.value(QStringLiteral("sha256")).toString().toLower();
        if (path.isEmpty() || path != originalPath || !validSha256(sha256) || terminalShaByPath.contains(path)) {
            fail(error, QStringLiteral("invalid terminal source publication result"));
            return -1;
        }
        terminalShaByPath.insert(path, sha256);
    }
    if (terminalShaByPath.isEmpty()) {
        return 0;
    }

    QSqlDatabase database = databaseFor(m_connectionName);
    QString busy;
    if (!beginWriteTransaction(database, &busy)) {
        fail(error, busy);
        return -1;
    }
    if (metadataValue(database, QStringLiteral("last_handled_source_publication_id")) == publicationId) {
        database.rollback();
        return 0;
    }
    const QList<DesktopPendingFile> pending = readPendingFiles(database, std::numeric_limits<quint64>::max(), error);
    if (error && !error->isEmpty()) {
        database.rollback();
        return -1;
    }

    const QString deletedSha256 = QString::fromLatin1(QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex());
    QList<DesktopPendingFile> matched;
    QSqlQuery claimed(database);
    claimed.prepare(QStringLiteral("SELECT claim_id FROM desktop_pending_claims WHERE path = ? AND generation = ?"));
    for (const DesktopPendingFile &file : pending) {
        const auto terminal = terminalShaByPath.constFind(file.path);
        if (terminal == terminalShaByPath.cend() || !file.hasTrackedState) {
            continue;
        }
        claimed.bindValue(0, file.path);
        claimed.bindValue(1, static_cast<qint64>(file.generation));
        if (!claimed.exec()) {
            database.rollback();
            fail(error, claimed.lastError().text());
            return -1;
        }
        if (!claimed.next() || claimed.value(0).toString() != traceId) {
            continue;
        }
        const bool exactUpsert = file.action == QLatin1String("upsert") && file.trackedState.exists && validSha256(file.trackedState.sha256) && file.trackedState.sha256 == terminal.value();
        const bool exactDeletion = file.action == QLatin1String("delete") && !file.trackedState.exists && terminal.value() == deletedSha256;
        if (!exactUpsert && !exactDeletion) {
            continue;
        }
        matched.append(file);
    }
    QSqlQuery updateBaseline(database);
    updateBaseline.prepare(
        QStringLiteral("INSERT INTO desktop_source_baselines(path, exists_flag, size_bytes, modified_at_ms, sha256, metadata_json, affected_targets_json) "
                       "VALUES(?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(path) DO UPDATE SET exists_flag=excluded.exists_flag, size_bytes=excluded.size_bytes, modified_at_ms=excluded.modified_at_ms, "
                       "sha256=excluded.sha256, metadata_json=excluded.metadata_json, affected_targets_json=excluded.affected_targets_json"));
    QSqlQuery remove(database);
    remove.prepare(QStringLiteral("DELETE FROM desktop_pending_paths WHERE path = ? AND generation = ?"));
    for (const DesktopPendingFile &file : std::as_const(matched)) {
        if (!insertTrackedPath(updateBaseline, file.trackedState, error)) {
            database.rollback();
            return -1;
        }
        remove.bindValue(0, file.path);
        remove.bindValue(1, static_cast<qint64>(file.generation));
        if (!remove.exec() || remove.numRowsAffected() != 1) {
            database.rollback();
            fail(error, remove.lastError().text().isEmpty() ? QStringLiteral("terminal source publication generation changed") : remove.lastError().text());
            return -1;
        }
        if (!clearClaimsThrough(database, file.path, file.generation, error)) {
            database.rollback();
            return -1;
        }
    }

    if (matched.isEmpty()) {
        database.rollback();
        return 0;
    }

    DesktopSyncFrontier recovered;
    QSqlQuery currentFrontier(database);
    if (!currentFrontier.exec(QStringLiteral("SELECT library_revision, source_frontier, comment_frontier, annotation_frontier, cursor, metadata_json "
                                             "FROM desktop_sync_frontier WHERE singleton = 1"))) {
        database.rollback();
        fail(error, currentFrontier.lastError().text());
        return -1;
    }
    if (currentFrontier.next()) {
        recovered.libraryRevision = currentFrontier.value(0).toString();
        recovered.sourceFrontier = currentFrontier.value(1).toLongLong();
        recovered.commentFrontier = currentFrontier.value(2).toLongLong();
        recovered.annotationFrontier = currentFrontier.value(3).toLongLong();
        recovered.cursor = currentFrontier.value(4).toLongLong();
        recovered.metadata = parsedObject(currentFrontier.value(5));
    }
    recovered.sourceFrontier = std::max(recovered.sourceFrontier, sourceFrontier);
    recovered.metadata.remove(QStringLiteral("source_manifest"));
    recovered.metadata.insert(QStringLiteral("last_recovered_source_publication_id"), publicationId);
    QSqlQuery updateFrontier(database);
    updateFrontier.prepare(
        QStringLiteral("INSERT INTO desktop_sync_frontier(singleton, library_revision, source_frontier, comment_frontier, annotation_frontier, cursor, metadata_json) "
                       "VALUES(1, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(singleton) DO UPDATE SET library_revision=excluded.library_revision, source_frontier=excluded.source_frontier, "
                       "comment_frontier=excluded.comment_frontier, annotation_frontier=excluded.annotation_frontier, cursor=excluded.cursor, metadata_json=excluded.metadata_json"));
    updateFrontier.addBindValue(recovered.libraryRevision.isEmpty() ? QStringLiteral("") : recovered.libraryRevision);
    updateFrontier.addBindValue(recovered.sourceFrontier);
    updateFrontier.addBindValue(recovered.commentFrontier);
    updateFrontier.addBindValue(recovered.annotationFrontier);
    updateFrontier.addBindValue(recovered.cursor);
    updateFrontier.addBindValue(jsonText(recovered.metadata));
    if (!updateFrontier.exec() || !setMetadata(database, QStringLiteral("last_handled_source_publication_id"), publicationId, error) || !setMetadata(database, QStringLiteral("baseline_reconciled"), QStringLiteral("0"), error) ||
        !commitAndAnnounce(database)) {
        const QString detail = error && !error->isEmpty() ? *error : (updateFrontier.lastError().text().isEmpty() ? database.lastError().text() : updateFrontier.lastError().text());
        database.rollback();
        fail(error, detail);
        return -1;
    }
    return matched.size();
}

DesktopSyncFrontier DesktopSyncStore::frontier(QString *error) const
{
    clearError(error);
    DesktopSyncFrontier result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return result;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT library_revision, source_frontier, comment_frontier, annotation_frontier, cursor, metadata_json FROM desktop_sync_frontier WHERE singleton = 1"))) {
        fail(error, query.lastError().text());
        return result;
    }
    if (query.next()) {
        result.libraryRevision = query.value(0).toString();
        result.sourceFrontier = query.value(1).toLongLong();
        result.commentFrontier = query.value(2).toLongLong();
        result.annotationFrontier = query.value(3).toLongLong();
        result.cursor = query.value(4).toLongLong();
        result.metadata = parsedObject(query.value(5));
    }
    return result;
}

bool DesktopSyncStore::setFrontier(const DesktopSyncFrontier &frontier, QString *error)
{
    clearError(error);
    if (frontier.sourceFrontier < -1 || frontier.commentFrontier < -1 || frontier.annotationFrontier < -1 || frontier.cursor < -1) {
        return fail(error, QStringLiteral("invalid desktop synchronization frontier"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        return fail(error, QStringLiteral("desktop synchronization store is not open"));
    }
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("INSERT INTO desktop_sync_frontier(singleton, library_revision, source_frontier, comment_frontier, annotation_frontier, cursor, metadata_json) VALUES(1, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(singleton) DO UPDATE SET library_revision=excluded.library_revision, source_frontier=excluded.source_frontier, comment_frontier=excluded.comment_frontier, "
                       "annotation_frontier=excluded.annotation_frontier, cursor=excluded.cursor, metadata_json=excluded.metadata_json"));
    query.addBindValue(frontier.libraryRevision.isEmpty() ? QStringLiteral("") : frontier.libraryRevision);
    query.addBindValue(frontier.sourceFrontier);
    query.addBindValue(frontier.commentFrontier);
    query.addBindValue(frontier.annotationFrontier);
    query.addBindValue(frontier.cursor);
    query.addBindValue(jsonText(frontier.metadata));
    return query.exec() || fail(error, query.lastError().text());
}

QHash<QString, DesktopVerifiedObject> DesktopSyncStore::verifiedObjects(QString *error) const
{
    clearError(error);
    QHash<QString, DesktopVerifiedObject> result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return result;
    }
    QSqlQuery query(database);
    query.setForwardOnly(true);
    if (!query.exec(QStringLiteral("SELECT sha256, size_bytes, kind, semantic_role, owning_publication, verified_at_ms, metadata_json FROM desktop_verified_objects"))) {
        fail(error, query.lastError().text());
        return result;
    }
    while (query.next()) {
        DesktopVerifiedObject object;
        object.sha256 = query.value(0).toString();
        object.sizeBytes = query.value(1).toLongLong();
        object.kind = query.value(2).toString();
        object.semanticRole = query.value(3).toString();
        object.owningPublication = query.value(4).toString();
        object.verifiedAtMs = query.value(5).toLongLong();
        object.metadata = parsedObject(query.value(6));
        result.insert(object.sha256, object);
    }
    return result;
}

QHash<QString, DesktopVerifiedObject> DesktopSyncStore::verifiedObjects(const QStringList &sha256s, QString *error) const
{
    clearError(error);
    QHash<QString, DesktopVerifiedObject> result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return result;
    }
    QStringList wanted;
    QSet<QString> seen;
    for (const QString &sha256 : sha256s) {
        if (validSha256(sha256) && !seen.contains(sha256)) {
            seen.insert(sha256);
            wanted.append(sha256);
        }
    }
    constexpr qsizetype chunk = 500;
    for (qsizetype start = 0; start < wanted.size(); start += chunk) {
        const QStringList part = wanted.mid(start, chunk);
        QStringList marks;
        marks.fill(QStringLiteral("?"), part.size());
        QSqlQuery query(database);
        query.setForwardOnly(true);
        query.prepare(QStringLiteral("SELECT sha256, size_bytes, kind, semantic_role, owning_publication, verified_at_ms, metadata_json "
                                     "FROM desktop_verified_objects WHERE sha256 IN (%1)").arg(marks.join(QLatin1Char(','))));
        for (const QString &sha256 : part) query.addBindValue(sha256);
        if (!query.exec()) {
            fail(error, query.lastError().text());
            return {};
        }
        while (query.next()) {
            DesktopVerifiedObject object;
            object.sha256 = query.value(0).toString();
            object.sizeBytes = query.value(1).toLongLong();
            object.kind = query.value(2).toString();
            object.semanticRole = query.value(3).toString();
            object.owningPublication = query.value(4).toString();
            object.verifiedAtMs = query.value(5).toLongLong();
            object.metadata = parsedObject(query.value(6));
            result.insert(object.sha256, object);
        }
    }
    return result;
}

std::optional<DesktopVerifiedObject> DesktopSyncStore::verifiedObject(const QString &sha256, QString *error) const
{
    clearError(error);
    if (!validSha256(sha256)) {
        fail(error, QStringLiteral("invalid verified object SHA-256"));
        return std::nullopt;
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return std::nullopt;
    }
    QSqlQuery query(database);
    query.prepare(QStringLiteral("SELECT size_bytes, kind, semantic_role, owning_publication, verified_at_ms, metadata_json FROM desktop_verified_objects WHERE sha256 = ?"));
    query.addBindValue(sha256);
    if (!query.exec()) {
        fail(error, query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        return std::nullopt;
    }
    DesktopVerifiedObject object;
    object.sha256 = sha256;
    object.sizeBytes = query.value(0).toLongLong();
    object.kind = query.value(1).toString();
    object.semanticRole = query.value(2).toString();
    object.owningPublication = query.value(3).toString();
    object.verifiedAtMs = query.value(4).toLongLong();
    object.metadata = parsedObject(query.value(5));
    return object;
}

bool DesktopSyncStore::upsertVerifiedObject(const DesktopVerifiedObject &object, QString *error)
{
    clearError(error);
    if (!validSha256(object.sha256) || object.sizeBytes < 0 || object.kind.trimmed().isEmpty()) {
        return fail(error, QStringLiteral("invalid verified desktop object"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        return fail(error, QStringLiteral("desktop synchronization store is not open"));
    }
    QSqlQuery query(database);
    query.prepare(
        QStringLiteral("INSERT INTO desktop_verified_objects(sha256, size_bytes, kind, semantic_role, owning_publication, verified_at_ms, metadata_json) VALUES(?, ?, ?, ?, ?, ?, ?) "
                       "ON CONFLICT(sha256) DO UPDATE SET size_bytes=excluded.size_bytes, kind=excluded.kind, semantic_role=excluded.semantic_role, owning_publication=excluded.owning_publication, verified_at_ms=excluded.verified_at_ms, "
                       "metadata_json=excluded.metadata_json"));
    query.addBindValue(object.sha256);
    query.addBindValue(object.sizeBytes);
    query.addBindValue(object.kind.trimmed());
    query.addBindValue(object.semanticRole.isNull() ? QStringLiteral("") : object.semanticRole.trimmed());
    query.addBindValue(object.owningPublication.isNull() ? QStringLiteral("") : object.owningPublication.trimmed());
    query.addBindValue(object.verifiedAtMs > 0 ? object.verifiedAtMs : QDateTime::currentMSecsSinceEpoch());
    query.addBindValue(jsonText(object.metadata));
    return query.exec() || fail(error, query.lastError().text());
}

QList<DesktopLogicalOperation> DesktopSyncStore::logicalOperations(bool includeAcknowledged, QString *error) const
{
    clearError(error);
    QList<DesktopLogicalOperation> result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return result;
    }
    QSqlQuery query(database);
    const QString predicate = includeAcknowledged ? QString() : QStringLiteral(" WHERE acknowledged = 0");
    if (!query.exec(QStringLiteral("SELECT operation_id, device_sequence, operation_json, acknowledged, created_at_ms, acknowledged_at_ms FROM desktop_logical_operations%1 ORDER BY device_sequence, operation_id").arg(predicate))) {
        fail(error, query.lastError().text());
        return {};
    }
    while (query.next()) {
        const QJsonDocument document = QJsonDocument::fromJson(query.value(2).toString().toUtf8());
        if (!document.isObject()) {
            fail(error, QStringLiteral("stored desktop logical operation is malformed"));
            return {};
        }
        DesktopLogicalOperation operation;
        operation.operationId = query.value(0).toString();
        operation.deviceSequence = query.value(1).toLongLong();
        operation.operation = document.object();
        operation.acknowledged = query.value(3).toBool();
        operation.createdAtMs = query.value(4).toLongLong();
        operation.acknowledgedAtMs = query.value(5).toLongLong();
        result.append(operation);
    }
    return result;
}

bool DesktopSyncStore::reconcileDeviceSequence(qint64 remoteNext, QString *error)
{
    clearError(error);
    if (remoteNext < 1 || remoteNext == std::numeric_limits<qint64>::max()) {
        return fail(error, QStringLiteral("invalid server device sequence"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, QStringLiteral("desktop synchronization store could not start sequence reconciliation"));
    }
    const auto rollback = [&](const QString &message) {
        database.rollback();
        return fail(error, message);
    };
    QSqlQuery maximum(database);
    if (!maximum.exec(QStringLiteral("SELECT COALESCE(MAX(device_sequence), 0) FROM desktop_logical_operations")) || !maximum.next()) {
        return rollback(maximum.lastError().text());
    }
    const qint64 highest = maximum.value(0).toLongLong();
    if (highest == std::numeric_limits<qint64>::max()) {
        return rollback(QStringLiteral("desktop logical operation sequence is exhausted"));
    }
    qint64 next = qMax(remoteNext, qMax(highest + 1, metadataValue(database, QStringLiteral("next_device_sequence")).toLongLong()));
    maximum.finish();
    QString readError;
    const QList<DesktopLogicalOperation> pending = logicalOperations(false, &readError);
    if (!readError.isEmpty()) {
        return rollback(readError);
    }
    // Rebase the entire pending suffix if any entry collides, preserving its
    // order and operation IDs. Fedora deduplicates IDs after a lost response.
    const bool rebase = std::any_of(pending.cbegin(), pending.cend(), [remoteNext](const auto &operation) { return operation.deviceSequence < remoteNext; });
    if (rebase) {
        for (const DesktopLogicalOperation &stored : pending) {
            if (next == std::numeric_limits<qint64>::max()) {
                return rollback(QStringLiteral("desktop logical operation sequence is exhausted"));
            }
            QJsonObject operation = stored.operation;
            if (operation.value(QStringLiteral("operation_id")).toString() != stored.operationId
                || operation.value(QStringLiteral("device_sequence")).toInteger(-1) != stored.deviceSequence) {
                return rollback(QStringLiteral("stored desktop logical operation identity is inconsistent"));
            }
            operation.insert(QStringLiteral("device_sequence"), next);
            QSqlQuery update(database);
            update.prepare(QStringLiteral("UPDATE desktop_logical_operations SET device_sequence = ?, operation_json = ? WHERE operation_id = ? AND acknowledged = 0"));
            update.addBindValue(next++);
            update.addBindValue(jsonText(operation));
            update.addBindValue(stored.operationId);
            if (!update.exec() || update.numRowsAffected() != 1) {
                return rollback(QStringLiteral("could not reconcile pending desktop operation: ") + update.lastError().text());
            }
        }
    }
    if (!setMetadata(database, QStringLiteral("next_device_sequence"), QString::number(next), error)) {
        return rollback(error ? *error : QStringLiteral("could not store reconciled device sequence"));
    }
    if (!commitAndAnnounce(database)) {
        return rollback(database.lastError().text());
    }
    return true;
}

QJsonObject DesktopSyncStore::enqueueLogicalOperation(QJsonObject operation, QString *error)
{
    const auto operations = enqueueLogicalOperations({operation}, error);
    return operations.isEmpty() ? QJsonObject() : operations.constFirst();
}

QList<QJsonObject> DesktopSyncStore::enqueueLogicalOperations(QList<QJsonObject> operations, QString *error)
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
        return {};
    }
    bool sequenceOk = false;
    qint64 sequence = metadataValue(database, QStringLiteral("next_device_sequence")).toLongLong(&sequenceOk);
    if (!sequenceOk) {
        QSqlQuery maximum(database);
        if (!maximum.exec(QStringLiteral("SELECT COALESCE(MAX(device_sequence), 0) + 1 FROM desktop_logical_operations")) || !maximum.next()) {
            database.rollback();
            fail(error, maximum.lastError().text());
            return {};
        }
        sequence = maximum.value(0).toLongLong();
    }
    if (sequence < 1 || sequence > std::numeric_limits<qint64>::max() - operations.size()) {
        database.rollback();
        fail(error, QStringLiteral("desktop logical operation sequence is exhausted"));
        return {};
    }
    for (QJsonObject &operation : operations) {
        operation.insert(QStringLiteral("operation_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        operation.insert(QStringLiteral("device_sequence"), sequence++);
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral("INSERT INTO desktop_logical_operations(operation_id, device_sequence, operation_json, created_at_ms) VALUES(?, ?, ?, ?)"));
        insert.addBindValue(operation.value(QStringLiteral("operation_id")).toString());
        insert.addBindValue(operation.value(QStringLiteral("device_sequence")).toInteger());
        insert.addBindValue(jsonText(operation));
        insert.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!insert.exec()) {
            database.rollback(); fail(error, insert.lastError().text()); return {};
        }
    }
    if (!setMetadata(database, QStringLiteral("next_device_sequence"), QString::number(sequence), error) || !commitAndAnnounce(database)) {
        database.rollback(); fail(error, database.lastError().text()); return {};
    }
    return operations;
}

bool DesktopSyncStore::appendLogicalOperation(const QJsonObject &operation, QString *error)
{
    clearError(error);
    const QString operationId = operation.value(QStringLiteral("operation_id")).toString().trimmed();
    const qint64 deviceSequence = operation.value(QStringLiteral("device_sequence")).toInteger(-1);
    if (operationId.isEmpty() || deviceSequence < 0 || deviceSequence == std::numeric_limits<qint64>::max()) {
        return fail(error, QStringLiteral("invalid desktop logical operation"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    QSqlQuery existing(database);
    existing.prepare(QStringLiteral("SELECT operation_json FROM desktop_logical_operations WHERE operation_id = ?"));
    existing.addBindValue(operationId);
    if (!existing.exec()) {
        database.rollback();
        return fail(error, existing.lastError().text());
    }
    if (existing.next()) {
        const QJsonDocument stored = QJsonDocument::fromJson(existing.value(0).toString().toUtf8());
        if (!stored.isObject() || stored.object() != operation) {
            database.rollback();
            return fail(error, QStringLiteral("desktop logical operation ID has conflicting content"));
        }
    } else {
        QSqlQuery insert(database);
        insert.prepare(QStringLiteral("INSERT INTO desktop_logical_operations(operation_id, device_sequence, operation_json, created_at_ms) VALUES(?, ?, ?, ?)"));
        insert.addBindValue(operationId);
        insert.addBindValue(deviceSequence);
        insert.addBindValue(jsonText(operation));
        insert.addBindValue(QDateTime::currentMSecsSinceEpoch());
        if (!insert.exec()) {
            database.rollback();
            return fail(error, insert.lastError().text());
        }
    }
    bool nextOk = false;
    const qint64 storedNext = metadataValue(database, QStringLiteral("next_device_sequence")).toLongLong(&nextOk);
    const qint64 next = std::max(deviceSequence + 1, nextOk ? storedNext : qint64(1));
    if (!setMetadata(database, QStringLiteral("next_device_sequence"), QString::number(next), error) || !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, error && !error->isEmpty() ? *error : database.lastError().text());
    }
    return true;
}

bool DesktopSyncStore::acknowledgeLogicalOperations(const QStringList &operationIds, QString *error)
{
    clearError(error);
    QStringList uniqueIds = operationIds;
    uniqueIds.removeDuplicates();
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE desktop_logical_operations SET acknowledged = 1, acknowledged_at_ms = CASE WHEN acknowledged = 0 THEN ? ELSE acknowledged_at_ms END WHERE operation_id = ?"));
    const qint64 acknowledgedAt = QDateTime::currentMSecsSinceEpoch();
    for (const QString &operationId : std::as_const(uniqueIds)) {
        update.bindValue(0, acknowledgedAt);
        update.bindValue(1, operationId);
        if (!update.exec()) {
            database.rollback();
            return fail(error, update.lastError().text());
        }
    }
    if (!commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, database.lastError().text());
    }
    return true;
}

bool DesktopSyncStore::removeTombstones(const QStringList &paths, int *removed, QString *error)
{
    clearError(error);
    if (removed) *removed = 0;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    int count = 0;
    for (const QString &table : {QStringLiteral("desktop_source_baselines"), QStringLiteral("desktop_observed_paths")}) {
        QSqlQuery remove(database);
        remove.prepare(QStringLiteral("DELETE FROM %1 WHERE path = ? AND exists_flag = 0 "
                                      "AND NOT EXISTS (SELECT 1 FROM desktop_pending_paths WHERE desktop_pending_paths.path = %1.path)").arg(table));
        for (const QString &path : paths) {
            remove.bindValue(0, path);
            if (!remove.exec()) {
                database.rollback();
                return fail(error, remove.lastError().text());
            }
            if (table == QLatin1String("desktop_source_baselines")) count += remove.numRowsAffected();
        }
    }
    if (!commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, database.lastError().text());
    }
    if (removed) *removed = count;
    return true;
}

QList<DesktopLibraryOperation> DesktopSyncStore::libraryOperations(bool includeHistory, QString *error) const
{
    clearError(error);
    QList<DesktopLibraryOperation> result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        fail(error, QStringLiteral("desktop synchronization store is not open"));
        return result;
    }
    const QString predicate = includeHistory ? QString() : QStringLiteral(" WHERE state IN ('pending', 'in_flight')");
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT operation_id, generation, type, filename, mutation_json, state, created_at_ms, completed_at_ms "
                                   "FROM desktop_library_operations%1 ORDER BY generation")
                        .arg(predicate))) {
        fail(error, query.lastError().text());
        return {};
    }
    while (query.next()) {
        DesktopLibraryOperation operation = libraryOperationFromQuery(query);
        if (!validateLibraryMutation(operation.mutation, error) || operation.operationId != libraryOperationId(operation.mutation) || operation.generation == 0 ||
            operation.type != operation.mutation.value(QStringLiteral("type")).toString() || operation.filename != operation.mutation.value(QStringLiteral("filename")).toString()) {
            fail(error, QStringLiteral("stored desktop Library operation is malformed"));
            return {};
        }
        result.append(operation);
    }
    return result;
}

DesktopLibraryEnqueueResult DesktopSyncStore::enqueueLibraryOperation(const QJsonObject &mutation)
{
    DesktopLibraryEnqueueResult result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        result.error = database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text();
        return result;
    }
    result = enqueueLibraryOperationInTransaction(database, mutation, QString());
    if (!result.success || !commitAndAnnounce(database)) {
        database.rollback();
        if (result.error.isEmpty()) {
            result.error = database.lastError().text();
        }
        result.success = false;
    }
    return result;
}

std::optional<DesktopLibraryOperation> DesktopSyncStore::claimNextLibraryOperation(QString *error)
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
        return std::nullopt;
    }
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT operation_id, generation, type, filename, mutation_json, state, created_at_ms, completed_at_ms "
                                   "FROM desktop_library_operations "
                                   "WHERE state IN ('pending', 'in_flight') "
                                   "ORDER BY CASE state WHEN 'in_flight' THEN 0 ELSE 1 END, generation LIMIT 1"))) {
        database.rollback();
        fail(error, query.lastError().text());
        return std::nullopt;
    }
    if (!query.next()) {
        commitAndAnnounce(database);
        return std::nullopt;
    }
    DesktopLibraryOperation operation = libraryOperationFromQuery(query);
    if (!validateLibraryMutation(operation.mutation, error) || operation.operationId != libraryOperationId(operation.mutation) || operation.generation == 0) {
        database.rollback();
        fail(error, QStringLiteral("stored desktop Library operation is malformed"));
        return std::nullopt;
    }
    if (operation.state == QLatin1String("pending")) {
        if (!setLibraryOperationState(database, operation.operationId, operation.generation, QStringLiteral("in_flight"), error)) {
            database.rollback();
            return std::nullopt;
        }
        operation.state = QStringLiteral("in_flight");
    }
    if (!commitAndAnnounce(database)) {
        database.rollback();
        fail(error, database.lastError().text());
        return std::nullopt;
    }
    return operation;
}

bool DesktopSyncStore::acknowledgeLibraryOperation(const QString &operationId, quint64 generation, QString *error)
{
    clearError(error);
    if (operationId.isEmpty() || generation == 0) {
        return fail(error, QStringLiteral("invalid desktop Library operation acknowledgement"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    QSqlQuery current(database);
    current.prepare(QStringLiteral("SELECT state FROM desktop_library_operations WHERE operation_id = ? AND generation = ?"));
    current.addBindValue(operationId);
    current.addBindValue(static_cast<qint64>(generation));
    if (!current.exec() || !current.next()) {
        database.rollback();
        return fail(error, current.lastError().text().isEmpty() ? QStringLiteral("desktop Library operation generation changed") : current.lastError().text());
    }
    const QString state = current.value(0).toString();
    if (state != QLatin1String("in_flight") && state != QLatin1String("superseded")) {
        database.rollback();
        return fail(error, QStringLiteral("desktop Library operation is not in flight"));
    }
    if (!setLibraryOperationState(database, operationId, generation, QStringLiteral("acknowledged"), error) || !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, error && !error->isEmpty() ? *error : database.lastError().text());
    }
    return true;
}

bool DesktopSyncStore::releaseLibraryOperation(const QString &operationId, quint64 generation, QString *error)
{
    clearError(error);
    QSqlDatabase database = databaseFor(m_connectionName);
    if (operationId.isEmpty() || generation == 0 || !database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("invalid desktop Library operation release") : database.lastError().text());
    }
    QSqlQuery current(database);
    current.prepare(QStringLiteral("SELECT type, filename, state FROM desktop_library_operations WHERE operation_id = ? AND generation = ?"));
    current.addBindValue(operationId);
    current.addBindValue(static_cast<qint64>(generation));
    if (!current.exec() || !current.next() || current.value(2).toString() != QLatin1String("in_flight")) {
        database.rollback();
        return fail(error, current.lastError().text().isEmpty() ? QStringLiteral("desktop Library operation is not in flight") : current.lastError().text());
    }
    const QString type = current.value(0).toString();
    const QString filename = current.value(1).toString();
    bool superseded = false;
    if (type == QLatin1String("rename") || type == QLatin1String("archive")) {
        QSqlQuery newer(database);
        newer.prepare(
            QStringLiteral("SELECT 1 FROM desktop_library_operations "
                           "WHERE filename = ? AND type = ? AND state = 'pending' AND generation > ? LIMIT 1"));
        newer.addBindValue(filename);
        newer.addBindValue(type);
        newer.addBindValue(static_cast<qint64>(generation));
        if (!newer.exec()) {
            database.rollback();
            return fail(error, newer.lastError().text());
        }
        superseded = newer.next();
    }
    if (!setLibraryOperationState(database, operationId, generation, superseded ? QStringLiteral("superseded") : QStringLiteral("pending"), error) || !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, error && !error->isEmpty() ? *error : database.lastError().text());
    }
    return true;
}

DesktopLegacyLibraryImportResult DesktopSyncStore::importLegacyLibraryOperations(const QString &legacyStatePath, const QJsonArray &operations)
{
    DesktopLegacyLibraryImportResult result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        result.error = QStringLiteral("desktop synchronization store is not open");
        return result;
    }
    const QString absolutePath = QFileInfo(legacyStatePath).absoluteFilePath();
    if (absolutePath.isEmpty()) {
        result.error = QStringLiteral("legacy desktop Library queue path is invalid");
        return result;
    }
    const QByteArray encoded = QJsonDocument(operations).toJson(QJsonDocument::Compact);
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(encoded, QCryptographicHash::Sha256).toHex());
    const QString pathDigest = QString::fromLatin1(QCryptographicHash::hash(absolutePath.toUtf8(), QCryptographicHash::Sha256).toHex());
    const QString importKey = QStringLiteral("legacy_library_queue_import_%1").arg(pathDigest);
    if (!database.transaction()) {
        result.error = database.lastError().text();
        return result;
    }
    const QString importedDigest = metadataValue(database, importKey);
    if (!importedDigest.isEmpty()) {
        if (importedDigest != digest) {
            database.rollback();
            result.error = QStringLiteral("legacy desktop Library queue changed after its one-time import");
            return result;
        }
        if (!commitAndAnnounce(database)) {
            database.rollback();
            result.error = database.lastError().text();
            return result;
        }
        result.success = true;
        result.alreadyImported = true;
        return result;
    }
    for (const QJsonValue &value : operations) {
        if (!value.isObject()) {
            database.rollback();
            result.error = QStringLiteral("legacy desktop Library queue contains a malformed operation");
            return result;
        }
        DesktopLibraryEnqueueResult imported = enqueueLibraryOperationInTransaction(database, value.toObject(), absolutePath);
        if (!imported.success) {
            database.rollback();
            result.error = imported.error;
            return result;
        }
        ++result.importedOperations;
    }
    if (!setMetadata(database, importKey, digest, &result.error) || !commitAndAnnounce(database)) {
        database.rollback();
        if (result.error.isEmpty()) {
            result.error = database.lastError().text();
        }
        result.importedOperations = 0;
        return result;
    }
    result.success = true;
    return result;
}

bool DesktopSyncStore::advanceCursor(qint64 expectedPrevious, qint64 next, QString *error)
{
    clearError(error);
    if (expectedPrevious < 0 || next < expectedPrevious) {
        return fail(error, QStringLiteral("invalid desktop synchronization cursor advance"));
    }
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen() || !database.transaction()) {
        return fail(error, database.lastError().text().isEmpty() ? QStringLiteral("desktop synchronization store is not open") : database.lastError().text());
    }
    QSqlQuery current(database);
    if (!current.exec(QStringLiteral("SELECT cursor FROM desktop_sync_frontier WHERE singleton = 1"))) {
        database.rollback();
        return fail(error, current.lastError().text());
    }
    const qint64 actual = current.next() ? current.value(0).toLongLong() : 0;
    if (actual != expectedPrevious) {
        database.rollback();
        return fail(error, QStringLiteral("desktop synchronization cursor changed concurrently"));
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral("INSERT INTO desktop_sync_frontier(singleton, cursor) VALUES(1, ?) ON CONFLICT(singleton) DO UPDATE SET cursor=excluded.cursor"));
    update.addBindValue(next);
    if (!update.exec() || !commitAndAnnounce(database)) {
        database.rollback();
        return fail(error, update.lastError().text().isEmpty() ? database.lastError().text() : update.lastError().text());
    }
    return true;
}

AnnotationSubtypeRepairResult DesktopSyncStore::repairBlankAnnotationSubtypes()
{
    AnnotationSubtypeRepairResult result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        result.error = QStringLiteral("desktop synchronization store is not open");
        return result;
    }
    struct Repair {
        QString operationId;
        QString originalJson;
        QJsonObject replacement;
    };
    QList<Repair> repairs;
    QJsonArray recoveryOperations;
    QSqlQuery query(database);
    if (!query.exec(QStringLiteral("SELECT operation_id, operation_json FROM desktop_logical_operations WHERE acknowledged = 0 ORDER BY device_sequence, operation_id"))) {
        result.error = query.lastError().text();
        return result;
    }
    while (query.next()) {
        const QString originalJson = query.value(1).toString();
        const QJsonDocument document = QJsonDocument::fromJson(originalJson.toUtf8());
        if (!document.isObject()) {
            result.error = QStringLiteral("stored desktop logical operation is malformed");
            return result;
        }
        QJsonObject operation = document.object();
        if (operation.value(QStringLiteral("entity_type")).toString() != QLatin1String("annotation")) {
            continue;
        }
        QJsonObject payload = operation.value(QStringLiteral("payload")).toObject();
        const QJsonValue subtype = payload.value(QStringLiteral("subtype"));
        if (!subtype.isString() || !subtype.toString().trimmed().isEmpty()) {
            continue;
        }
        recoveryOperations.append(operation);
        payload.insert(QStringLiteral("subtype"), QStringLiteral("Text"));
        operation.insert(QStringLiteral("payload"), payload);
        repairs.append({query.value(0).toString(), originalJson, operation});
    }
    if (repairs.isEmpty()) {
        result.success = true;
        return result;
    }

    const QByteArray recoveryBytes = QJsonDocument(QJsonObject {
                                                       {QStringLiteral("schema_version"), 1},
                                                       {QStringLiteral("operations"), recoveryOperations},
                                                   })
                                         .toJson(QJsonDocument::Indented);
    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmsszzz'Z'"));
    result.backupPath = m_databasePath + QStringLiteral(".annotation-repair-") + stamp + QStringLiteral(".json");
    for (int suffix = 1; QFileInfo::exists(result.backupPath); ++suffix) {
        result.backupPath = m_databasePath + QStringLiteral(".annotation-repair-") + stamp + QStringLiteral("-%1.json").arg(suffix);
    }
    QSaveFile recovery(result.backupPath);
    recovery.setDirectWriteFallback(false);
    if (!recovery.open(QIODevice::WriteOnly) || recovery.write(recoveryBytes) != recoveryBytes.size() || !recovery.commit()) {
        result.error = recovery.errorString();
        result.backupPath.clear();
        return result;
    }
    result.backupSha256 = QCryptographicHash::hash(recoveryBytes, QCryptographicHash::Sha256).toHex();

    if (!database.transaction()) {
        result.error = database.lastError().text();
        return result;
    }
    QSqlQuery update(database);
    update.prepare(QStringLiteral("UPDATE desktop_logical_operations SET operation_json = ? WHERE operation_id = ? AND operation_json = ? AND acknowledged = 0"));
    for (const Repair &repair : std::as_const(repairs)) {
        update.bindValue(0, jsonText(repair.replacement));
        update.bindValue(1, repair.operationId);
        update.bindValue(2, repair.originalJson);
        if (!update.exec() || update.numRowsAffected() != 1) {
            database.rollback();
            result.error = update.lastError().text().isEmpty() ? QStringLiteral("desktop logical operation changed during annotation repair") : update.lastError().text();
            return result;
        }
    }
    if (!commitAndAnnounce(database)) {
        database.rollback();
        result.error = database.lastError().text();
        return result;
    }
    result.repaired = repairs.size();
    result.success = true;
    return result;
}

DesktopLegacyImportResult DesktopSyncStore::importLegacyOutbox(const QString &legacyPath)
{
    DesktopLegacyImportResult result;
    QSqlDatabase database = databaseFor(m_connectionName);
    if (!database.isValid() || !database.isOpen()) {
        result.error = QStringLiteral("desktop synchronization store is not open");
        return result;
    }
    QFile source(legacyPath);
    if (!source.open(QIODevice::ReadOnly)) {
        result.error = source.errorString();
        return result;
    }
    const QByteArray bytes = source.readAll();
    source.close();
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.error = parseError.error != QJsonParseError::NoError ? parseError.errorString() : QStringLiteral("legacy desktop outbox root is not an object");
        return result;
    }
    const QJsonObject legacy = document.object();
    const QJsonValue operationsValue = legacy.value(QStringLiteral("operations"));
    if (!operationsValue.isArray()) {
        result.error = QStringLiteral("legacy desktop outbox operations are malformed");
        return result;
    }
    QList<QJsonObject> operations;
    QSet<QString> operationIds;
    qint64 maximumDeviceSequence = 0;
    for (const QJsonValue &value : operationsValue.toArray()) {
        if (!value.isObject()) {
            result.error = QStringLiteral("legacy desktop outbox contains a malformed operation");
            return result;
        }
        const QJsonObject operation = value.toObject();
        const QString operationId = operation.value(QStringLiteral("operation_id")).toString().trimmed();
        const qint64 deviceSequence = operation.value(QStringLiteral("device_sequence")).toInteger(-1);
        if (operationId.isEmpty() || deviceSequence < 0 || deviceSequence == std::numeric_limits<qint64>::max() || operationIds.contains(operationId)) {
            result.error = QStringLiteral("legacy desktop outbox contains an invalid or duplicate operation");
            return result;
        }
        operationIds.insert(operationId);
        maximumDeviceSequence = std::max(maximumDeviceSequence, deviceSequence);
        operations.append(operation);
    }
    const qint64 cursor = legacy.value(QStringLiteral("server_cursor")).toInteger(-1);
    const qint64 nextDeviceSequence = legacy.value(QStringLiteral("next_device_sequence")).toInteger(1);
    if (cursor < 0 || nextDeviceSequence < 1) {
        result.error = QStringLiteral("legacy desktop outbox cursor is invalid");
        return result;
    }
    const QByteArray sha256 = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
    result.recoveryCopyPath = legacyPath + QStringLiteral(".desktop-sync-import-") + QString::fromLatin1(sha256.left(16)) + QStringLiteral(".json");
    if (QFileInfo::exists(result.recoveryCopyPath)) {
        QFile recovery(result.recoveryCopyPath);
        if (!recovery.open(QIODevice::ReadOnly) || recovery.readAll() != bytes) {
            result.error = QStringLiteral("legacy desktop outbox recovery copy conflicts with the source");
            return result;
        }
    } else {
        if (!QFile::copy(legacyPath, result.recoveryCopyPath)) {
            result.error = QStringLiteral("could not preserve a legacy desktop outbox recovery copy");
            return result;
        }
        QFile recovery(result.recoveryCopyPath);
        if (!recovery.open(QIODevice::ReadOnly) || recovery.readAll() != bytes) {
            result.error = QStringLiteral("legacy desktop outbox changed while its recovery copy was created");
            return result;
        }
    }

    const QString absoluteLegacyPath = QFileInfo(legacyPath).absoluteFilePath();
    const QByteArray pathDigest = QCryptographicHash::hash(absoluteLegacyPath.toUtf8(), QCryptographicHash::Sha256).toHex();
    const QString pathImportKey = QStringLiteral("legacy_outbox_path_import_%1").arg(QString::fromLatin1(pathDigest));
    const QString importKey = QStringLiteral("legacy_outbox_import_%1").arg(QString::fromLatin1(sha256));
    if (!database.transaction()) {
        result.error = database.lastError().text();
        return result;
    }
    const QString importedPathSha = metadataValue(database, pathImportKey);
    if (!importedPathSha.isEmpty() && importedPathSha != QString::fromLatin1(sha256)) {
        database.rollback();
        result.error = QStringLiteral("legacy desktop outbox changed after its one-time import");
        return result;
    }
    if (metadataValue(database, importKey) == QLatin1String("1")) {
        if (!setMetadata(database, pathImportKey, QString::fromLatin1(sha256), &result.error) || !commitAndAnnounce(database)) {
            database.rollback();
            if (result.error.isEmpty()) {
                result.error = database.lastError().text();
            }
            return result;
        }
        result.success = true;
        result.alreadyImported = true;
        result.cursor = cursor;
        return result;
    }
    QSqlQuery existing(database);
    existing.prepare(QStringLiteral("SELECT operation_json FROM desktop_logical_operations WHERE operation_id = ?"));
    QSqlQuery insert(database);
    insert.prepare(QStringLiteral("INSERT INTO desktop_logical_operations(operation_id, device_sequence, operation_json, created_at_ms, imported_from) VALUES(?, ?, ?, ?, ?)"));
    for (const QJsonObject &operation : std::as_const(operations)) {
        const QString operationId = operation.value(QStringLiteral("operation_id")).toString();
        existing.bindValue(0, operationId);
        if (!existing.exec()) {
            database.rollback();
            result.error = existing.lastError().text();
            return result;
        }
        if (existing.next()) {
            const QJsonDocument stored = QJsonDocument::fromJson(existing.value(0).toString().toUtf8());
            if (!stored.isObject() || stored.object() != operation) {
                database.rollback();
                result.error = QStringLiteral("legacy desktop operation conflicts with stored content");
                return result;
            }
            continue;
        }
        insert.bindValue(0, operationId);
        insert.bindValue(1, operation.value(QStringLiteral("device_sequence")).toInteger());
        insert.bindValue(2, jsonText(operation));
        insert.bindValue(3, QDateTime::currentMSecsSinceEpoch());
        insert.bindValue(4, absoluteLegacyPath);
        if (!insert.exec()) {
            database.rollback();
            result.error = insert.lastError().text();
            return result;
        }
        ++result.importedOperations;
    }
    bool storedNextOk = false;
    const qint64 storedNext = metadataValue(database, QStringLiteral("next_device_sequence")).toLongLong(&storedNextOk);
    const qint64 importedNextDeviceSequence = std::max(nextDeviceSequence, maximumDeviceSequence + 1);
    const qint64 mergedNextDeviceSequence = storedNextOk ? std::max(storedNext, importedNextDeviceSequence) : importedNextDeviceSequence;
    QSqlQuery frontier(database);
    frontier.prepare(QStringLiteral("INSERT INTO desktop_sync_frontier(singleton, cursor) VALUES(1, ?) ON CONFLICT(singleton) DO UPDATE SET cursor=MAX(cursor, excluded.cursor)"));
    frontier.addBindValue(cursor);
    if (!frontier.exec() || !setMetadata(database, QStringLiteral("next_device_sequence"), QString::number(mergedNextDeviceSequence), &result.error) || !setMetadata(database, pathImportKey, QString::fromLatin1(sha256), &result.error) ||
        !setMetadata(database, importKey, QStringLiteral("1"), &result.error) || !commitAndAnnounce(database)) {
        database.rollback();
        if (result.error.isEmpty()) {
            result.error = frontier.lastError().text().isEmpty() ? database.lastError().text() : frontier.lastError().text();
        }
        result.importedOperations = 0;
        return result;
    }
    result.success = true;
    result.cursor = cursor;
    return result;
}

DesktopSyncOutbox::DesktopSyncOutbox(QString databasePath, QString legacyPath)
    : m_store(std::move(databasePath))
    , m_legacyPath(std::move(legacyPath))
{
}

bool DesktopSyncOutbox::ready(QString *error) const
{
    return prepare(error);
}

QJsonObject DesktopSyncOutbox::enqueue(QString entityType, QString entityId, QString collection, qint64 baseRevision, QJsonObject payload, QString clientTimestamp)
{
    if (!prepare(nullptr)) {
        return {};
    }
    QJsonObject operation {
        {QStringLiteral("entity_type"), std::move(entityType)},
        {QStringLiteral("entity_id"), std::move(entityId)},
        {QStringLiteral("collection"), std::move(collection)},
        {QStringLiteral("client_timestamp"), std::move(clientTimestamp)},
        {QStringLiteral("payload"), std::move(payload)},
    };
    if (baseRevision >= 0) {
        operation.insert(QStringLiteral("base_revision"), baseRevision);
    }
    return m_store.enqueueLogicalOperation(std::move(operation));
}

QJsonObject DesktopSyncOutbox::enqueueAgentProposalResolution(QString artifactSha256, QString action, QJsonArray proposals)
{
    if (!prepare(nullptr)) {
        return {};
    }
    return m_store.enqueueLogicalOperation({
        {QStringLiteral("outbox_kind"), QStringLiteral("agent_proposal_resolution")},
        {QStringLiteral("artifact_sha256"), std::move(artifactSha256)},
        {QStringLiteral("action"), std::move(action)},
        {QStringLiteral("proposals"), std::move(proposals)},
    });
}

QJsonObject DesktopSyncOutbox::enqueueAgentProposalUndo(QString operationId)
{
    if (!prepare(nullptr)) {
        return {};
    }
    return m_store.enqueueLogicalOperation({
        {QStringLiteral("outbox_kind"), QStringLiteral("agent_proposal_undo")},
        {QStringLiteral("undo_of"), std::move(operationId)},
    });
}

QJsonObject DesktopSyncOutbox::enqueueSourceConflictResolution(QString conflictId,
                                                               qint64 expectedRevision,
                                                               QString expectedSha256,
                                                               QByteArray contents,
                                                               QString clientTimestamp,
                                                               QJsonArray proposalDecisions)
{
    if (!prepare(nullptr)) {
        return {};
    }
    const QString contentSha256 = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    QJsonObject operation {
        {QStringLiteral("outbox_kind"), QStringLiteral("source_conflict_resolution")},
        {QStringLiteral("conflict_id"), std::move(conflictId)},
        {QStringLiteral("expected_revision"), expectedRevision},
        {QStringLiteral("expected_sha256"), std::move(expectedSha256)},
        {QStringLiteral("content_sha256"), contentSha256},
        {QStringLiteral("content_base64"), QString::fromLatin1(contents.toBase64())},
        {QStringLiteral("client_timestamp"), std::move(clientTimestamp)},
    };
    if (!proposalDecisions.isEmpty()) {
        operation.insert(QStringLiteral("proposal_decisions"), proposalDecisions);
    }
    return m_store.enqueueLogicalOperation(std::move(operation));
}

QList<QJsonObject> DesktopSyncOutbox::operations(QString *error) const
{
    if (!prepare(error)) {
        return {};
    }
    const QList<DesktopLogicalOperation> stored = m_store.logicalOperations(false, error);
    QList<QJsonObject> result;
    result.reserve(stored.size());
    for (const DesktopLogicalOperation &operation : stored) {
        result.append(operation.operation);
    }
    return result;
}

bool DesktopSyncOutbox::reconcileDeviceSequence(qint64 remoteNext, QString *error)
{
    return prepare(error) && m_store.reconcileDeviceSequence(remoteNext, error);
}

AnnotationSubtypeRepairResult DesktopSyncOutbox::repairBlankAnnotationSubtypes()
{
    QString error;
    if (!prepare(&error)) {
        AnnotationSubtypeRepairResult result;
        result.error = error;
        return result;
    }
    return m_store.repairBlankAnnotationSubtypes();
}

bool DesktopSyncOutbox::acknowledge(const QList<QString> &operationIds, QString *error)
{
    return prepare(error) && m_store.acknowledgeLogicalOperations(operationIds, error);
}

qint64 DesktopSyncOutbox::cursor(QString *error) const
{
    if (!prepare(error)) {
        return -1;
    }
    return m_store.frontier(error).cursor;
}

bool DesktopSyncOutbox::advanceCursor(qint64 expectedPrevious, qint64 next, QString *error)
{
    return prepare(error) && m_store.advanceCursor(expectedPrevious, next, error);
}

QString DesktopSyncOutbox::databasePath() const
{
    return m_store.databasePath();
}

QString DesktopSyncOutbox::legacyPath() const
{
    return m_legacyPath;
}

bool DesktopSyncOutbox::prepare(QString *error) const
{
    clearError(error);
    if (m_ready) {
        return true;
    }
    if (!m_store.open(error)) {
        return false;
    }
    if (!m_legacyPath.isEmpty() && QFileInfo::exists(m_legacyPath)) {
        const DesktopLegacyImportResult imported = m_store.importLegacyOutbox(m_legacyPath);
        if (!imported.success) {
            return fail(error, imported.error);
        }
    }
    DesktopSyncFrontier storedFrontier = m_store.frontier(error);
    if (error && !error->isEmpty()) {
        return false;
    }
    if (storedFrontier.cursor < 0) {
        storedFrontier.cursor = 0;
        if (!m_store.setFrontier(storedFrontier, error)) {
            return false;
        }
    }
    m_ready = true;
    return true;
}

}

QList<QJsonObject> Thoughts::DesktopSyncOutbox::enqueueBatch(QList<QJsonObject> operations, QString *error)
{
    if (!ready(error)) return {};
    return m_store.enqueueLogicalOperations(std::move(operations), error);
}
