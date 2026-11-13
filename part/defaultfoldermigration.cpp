// SPDX-License-Identifier: GPL-2.0-or-later
#include "defaultfoldermigration.h"

#include "sourceowner.h"

#include "../tools/publisher/pyerror.h"
#include "../tools/publisher/statelock.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QHash>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QUuid>

#include <functional>
#include <memory>
#include <optional>

namespace Thoughts
{
namespace
{
constexpr QLatin1StringView JournalName{"default-folder-migration.json"};
constexpr QLatin1StringView LikeFilter{"%.thoughts/projects/%/documents%"};

QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

std::optional<QByteArray> readFile(const QString &path)
{
    QFile file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return file.readAll();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    const QFileDevice::Permissions permissions = QFileInfo::exists(path) ? QFile::permissions(path) : QFileDevice::Permissions();
    QSaveFile file(path);
    if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return false;
    }
    if (permissions) {
        QFile::setPermissions(path, permissions);
    }
    return true;
}

// Rule R occurrences in bytes: each one shortens the text by two bytes
// ("documents" -> "default").
qsizetype legacyCount(const QByteArray &bytes)
{
    return (bytes.size() - SourceOwner::rewriteLegacyDefaultFolder(bytes).size()) / 2;
}

bool sameFileContent(const QString &left, const QString &right)
{
    QFile a(left);
    QFile b(right);
    if (!a.open(QIODevice::ReadOnly) || !b.open(QIODevice::ReadOnly) || a.size() != b.size()) {
        return false;
    }
    while (!a.atEnd()) {
        if (a.read(1 << 20) != b.read(1 << 20)) {
            return false;
        }
    }
    return true;
}

class Migrator
{
public:
    Migrator(const DefaultFolderMigration::Paths &paths, QJsonObject &report)
        : m_paths(paths)
        , m_report(report)
    {
    }

    QStringList stateRoots() const
    {
        QStringList roots{QDir::cleanPath(m_paths.stateRoot)};
        if (!m_paths.thoughtsDataRoot.isEmpty()) {
            const QString other = QDir::cleanPath(QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("thoughts-server")));
            if (!roots.contains(other) && QFileInfo(other).isDir()) {
                roots.append(other);
            }
        }
        return roots;
    }

    // Step 2: <hash>/documents -> <hash>/default.
    bool moveFolder(const QString &hash, QStringList *problems)
    {
        const QDir projects(DefaultFolderLocation::projectsDirectory(m_paths.docsRoot));
        const QString legacy = projects.filePath(hash + QLatin1Char('/') + SourceOwner::LegacyDefaultSegment);
        const QString canonical = projects.filePath(hash + QLatin1Char('/') + SourceOwner::DefaultSegment);
        if (!QFileInfo(legacy).isDir() || QFileInfo(legacy).isSymLink()) {
            return true;
        }
        if (!QFileInfo::exists(canonical) && !QFileInfo(canonical).isSymLink()) {
            if (!QDir().rename(legacy, canonical)) {
                problems->append(QStringLiteral("%1 could not be renamed to %2.").arg(legacy, canonical));
                return false;
            }
            appendMoved(hash, QStringLiteral("renamed"));
            return true;
        }
        if (!QFileInfo(canonical).isDir() || QFileInfo(canonical).isSymLink()) {
            problems->append(QStringLiteral("%1 exists and is not a folder.").arg(canonical));
            return false;
        }
        // Both exist: move what default/ lacks, drop identical copies, and stop
        // at any difference without changing it.
        QStringList differing;
        QDirIterator walk(legacy, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        QStringList files;
        while (walk.hasNext()) {
            files.append(walk.next());
        }
        std::sort(files.begin(), files.end());
        for (const QString &file : std::as_const(files)) {
            const QString relative = QDir(legacy).relativeFilePath(file);
            const QString target = QDir(canonical).filePath(relative);
            if (QFileInfo(file).isSymLink() || QFileInfo(target).isSymLink()) {
                differing.append(relative);
                continue;
            }
            if (!QFileInfo::exists(target)) {
                if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::rename(file, target)) {
                    problems->append(QStringLiteral("%1 could not be moved to %2.").arg(file, target));
                    return false;
                }
            } else if (QFileInfo(target).isFile() && sameFileContent(file, target)) {
                if (!QFile::remove(file)) {
                    problems->append(QStringLiteral("%1 could not be removed.").arg(file));
                    return false;
                }
            } else {
                differing.append(relative);
            }
        }
        if (!differing.isEmpty()) {
            for (const QString &relative : std::as_const(differing)) {
                problems->append(QStringLiteral("%1 differs from %2.").arg(QDir(legacy).filePath(relative), QDir(canonical).filePath(relative)));
            }
            return false;
        }
        // Only empty directories remain.
        QStringList directories;
        QDirIterator dirs(legacy, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirs.hasNext()) {
            directories.append(dirs.next());
        }
        std::sort(directories.begin(), directories.end(), [](const QString &a, const QString &b) {
            return a.size() > b.size();
        });
        for (const QString &directory : std::as_const(directories)) {
            QDir().rmdir(directory);
        }
        if (!QDir().rmdir(legacy)) {
            problems->append(QStringLiteral("%1 still contains files after moving them to %2.").arg(legacy, canonical));
            return false;
        }
        appendMoved(hash, QStringLiteral("merged"));
        return true;
    }

    // Rule R on one text file; returns false on a write failure.
    bool rewriteFile(const QString &path)
    {
        const auto bytes = readFile(path);
        if (!bytes || !bytes->contains(".thoughts/projects/")) {
            return true;
        }
        const QByteArray rewritten = SourceOwner::rewriteLegacyDefaultFolder(*bytes);
        if (rewritten == *bytes) {
            return true;
        }
        if (!writeFile(path, rewritten)) {
            m_errors.append(QStringLiteral("%1 could not be rewritten.").arg(path));
            return false;
        }
        m_rewrittenFiles.append(path);
        m_files.insert(path, legacyCount(*bytes));
        return true;
    }

    void rewriteFiles(const QString &directory, const QStringList &nameFilters, bool recursive, const QStringList &skippedDirectories = {})
    {
        if (!QFileInfo(directory).isDir()) {
            return;
        }
        QDirIterator walk(directory, nameFilters, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                          recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (walk.hasNext()) {
            const QString path = walk.next();
            const QString relative = QDir(directory).relativeFilePath(path);
            // The journal records what the migration did, old paths included.
            if (walk.fileName() == JournalName) {
                continue;
            }
            if (std::any_of(skippedDirectories.cbegin(), skippedDirectories.cend(), [&relative](const QString &skipped) {
                    return relative.startsWith(skipped + QLatin1Char('/'));
                })) {
                continue;
            }
            rewriteFile(path);
        }
    }

    // source-merge-recoveries/<sha256(path)>.{json,local}
    void renameMergeRecoveries(const QString &stateRoot)
    {
        const QDir directory(QDir(stateRoot).filePath(QStringLiteral("source-merge-recoveries")));
        for (const QString &name : directory.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            const QString recordPath = directory.filePath(name);
            const auto bytes = readFile(recordPath);
            if (!bytes) {
                continue;
            }
            const QString path = QJsonDocument::fromJson(*bytes).object().value(QStringLiteral("path")).toString();
            const QString rewrittenPath = SourceOwner::rewriteLegacyDefaultFolder(path);
            const QString stem = name.chopped(5);
            if (path.isEmpty() || rewrittenPath == path || stem != sha256Hex(path.toUtf8())) {
                // Already renamed (or not a legacy path): its content may still need Rule R.
                rewriteFile(recordPath);
                continue;
            }
            const QString newStem = sha256Hex(rewrittenPath.toUtf8());
            const QString local = directory.filePath(stem + QStringLiteral(".local"));
            const QString newLocal = directory.filePath(newStem + QStringLiteral(".local"));
            if (QFileInfo::exists(local)) {
                QFile::remove(newLocal);
                if (!QFile::rename(local, newLocal)) {
                    m_errors.append(QStringLiteral("%1 could not be renamed.").arg(local));
                    continue;
                }
            }
            if (!writeFile(directory.filePath(newStem + QStringLiteral(".json")), SourceOwner::rewriteLegacyDefaultFolder(*bytes)) || !QFile::remove(recordPath)) {
                m_errors.append(QStringLiteral("%1 could not be renamed.").arg(recordPath));
                continue;
            }
            m_renamed.append(QJsonObject{{QStringLiteral("from"), recordPath}, {QStringLiteral("to"), directory.filePath(newStem + QStringLiteral(".json"))}});
        }
    }

    // source-conflicts/<conflict id>/<source path>.local (and Thoughts' own
    // desktop-merge files below it): the source path is part of the file path.
    void moveConflictFiles(const QString &stateRoot)
    {
        const QString root = QDir(stateRoot).filePath(QStringLiteral("source-conflicts"));
        if (!QFileInfo(root).isDir()) {
            return;
        }
        QStringList files;
        QDirIterator walk(root, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            files.append(walk.next());
        }
        for (const QString &file : std::as_const(files)) {
            const QString relative = QDir(root).relativeFilePath(file);
            const QString rewritten = SourceOwner::rewriteLegacyDefaultFolder(relative);
            if (rewritten == relative) {
                if (file.endsWith(QLatin1String(".json"))) {
                    rewriteFile(file);
                }
                continue;
            }
            const QString target = QDir(root).filePath(rewritten);
            if (QFileInfo::exists(target)) {
                if (!sameFileContent(file, target)) {
                    m_errors.append(QStringLiteral("%1 and %2 both exist and differ.").arg(file, target));
                    continue;
                }
                QFile::remove(file);
            } else if (!QDir().mkpath(QFileInfo(target).absolutePath()) || !QFile::rename(file, target)) {
                m_errors.append(QStringLiteral("%1 could not be moved.").arg(file));
                continue;
            }
            m_renamed.append(QJsonObject{{QStringLiteral("from"), file}, {QStringLiteral("to"), target}});
            if (target.endsWith(QLatin1String(".json"))) {
                rewriteFile(target);
            }
        }
        // Drop the emptied legacy directories.
        QStringList directories;
        QDirIterator dirs(root, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirs.hasNext()) {
            const QString directory = dirs.next();
            if (SourceOwner::rewriteLegacyDefaultFolder(directory) != directory) {
                directories.append(directory);
            }
        }
        std::sort(directories.begin(), directories.end(), [](const QString &a, const QString &b) {
            return a.size() > b.size();
        });
        for (const QString &directory : std::as_const(directories)) {
            QDir().rmdir(directory);
        }
    }

    struct Database {
        QString connection = QStringLiteral("default-folder-migration-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        QSqlDatabase db;
        explicit Database(const QString &path)
        {
            db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
            db.setDatabaseName(path);
            db.setConnectOptions(QStringLiteral("QSQLITE_BUSY_TIMEOUT=10000"));
        }
        ~Database()
        {
            db.close();
            db = {};
            QSqlDatabase::removeDatabase(connection);
        }
    };

    // Rule R on every text and blob column of every ordinary table. Tables
    // that observe the file system (rebuilt by the next scan) drop a legacy
    // row whose rewritten key already exists; anywhere else that is an error.
    void rewriteDatabase(const QString &path)
    {
        if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
            return;
        }
        static const QSet<QString> observationTables{QStringLiteral("desktop_observed_paths"), QStringLiteral("desktop_pending_claims")};
        Database database(path);
        if (!database.db.open()) {
            m_errors.append(QStringLiteral("%1 could not be opened: %2").arg(path, database.db.lastError().text()));
            return;
        }
        QSqlQuery tables(database.db);
        if (!tables.exec(QStringLiteral("SELECT name, sql FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"))) {
            m_errors.append(QStringLiteral("%1 could not be read: %2").arg(path, tables.lastError().text()));
            return;
        }
        QList<QPair<QString, QString>> names;
        while (tables.next()) {
            names.append({tables.value(0).toString(), tables.value(1).toString()});
        }
        // FTS shadow tables belong to their virtual table.
        QSet<QString> virtualTables;
        for (const auto &[name, sql] : std::as_const(names)) {
            if (sql.startsWith(QLatin1String("CREATE VIRTUAL TABLE"), Qt::CaseInsensitive)) {
                virtualTables.insert(name);
            }
        }
        if (!database.db.transaction()) {
            m_errors.append(QStringLiteral("%1 is busy: %2").arg(path, database.db.lastError().text()));
            return;
        }
        QJsonObject counts;
        bool failed = false;
        for (const auto &[name, sql] : std::as_const(names)) {
            if (virtualTables.contains(name)
                || std::any_of(virtualTables.cbegin(), virtualTables.cend(), [&name](const QString &table) {
                       return name.startsWith(table + QLatin1Char('_'));
                   })) {
                continue;
            }
            const bool withoutRowid = sql.contains(QLatin1String("WITHOUT ROWID"), Qt::CaseInsensitive);
            QSqlQuery columns(database.db);
            if (!columns.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(name))) {
                continue;
            }
            QStringList columnNames;
            while (columns.next()) {
                columnNames.append(columns.value(1).toString());
            }
            const QString quotedTable = QLatin1Char('"') + name + QLatin1Char('"');
            qint64 rewrittenRows = 0;
            for (const QString &column : std::as_const(columnNames)) {
                const QString quoted = QLatin1Char('"') + column + QLatin1Char('"');
                QSqlQuery select(database.db);
                select.prepare(QStringLiteral("SELECT %1, %2, typeof(%2) FROM %3 WHERE CAST(%2 AS TEXT) LIKE ?")
                                   .arg(withoutRowid ? QStringLiteral("NULL") : QStringLiteral("rowid"), quoted, quotedTable));
                select.addBindValue(QString(LikeFilter));
                if (!select.exec()) {
                    continue;
                }
                while (select.next()) {
                    const QString type = select.value(2).toString();
                    const QByteArray before = type == QLatin1String("blob") ? select.value(1).toByteArray() : select.value(1).toString().toUtf8();
                    const QByteArray after = SourceOwner::rewriteLegacyDefaultFolder(before);
                    if (after == before) {
                        continue;
                    }
                    if (withoutRowid) {
                        m_errors.append(QStringLiteral("%1: table %2 has no row ids to rewrite.").arg(path, name));
                        failed = true;
                        break;
                    }
                    const qint64 rowid = select.value(0).toLongLong();
                    QSqlQuery update(database.db);
                    update.prepare(QStringLiteral("UPDATE %1 SET %2 = ? WHERE rowid = ?").arg(quotedTable, quoted));
                    if (type == QLatin1String("blob")) {
                        update.addBindValue(after);
                    } else {
                        update.addBindValue(QString::fromUtf8(after));
                    }
                    update.addBindValue(rowid);
                    if (!update.exec()) {
                        const QString message = update.lastError().text();
                        if (observationTables.contains(name) && message.contains(QLatin1String("UNIQUE"), Qt::CaseInsensitive)) {
                            QSqlQuery drop(database.db);
                            drop.prepare(QStringLiteral("DELETE FROM %1 WHERE rowid = ?").arg(quotedTable));
                            drop.addBindValue(rowid);
                            if (drop.exec()) {
                                ++rewrittenRows;
                                continue;
                            }
                        }
                        m_errors.append(QStringLiteral("%1: %2.%3 could not be rewritten: %4").arg(path, name, column, message));
                        failed = true;
                        break;
                    }
                    ++rewrittenRows;
                    // RepositoryProjectionStore verifies each payload's SHA-256.
                    if (name == QLatin1String("records") && column == QLatin1String("payload") && columnNames.contains(QStringLiteral("sha256"))) {
                        QSqlQuery digest(database.db);
                        digest.prepare(QStringLiteral("UPDATE records SET sha256 = ? WHERE rowid = ?"));
                        digest.addBindValue(QCryptographicHash::hash(after, QCryptographicHash::Sha256));
                        digest.addBindValue(rowid);
                        if (!digest.exec()) {
                            m_errors.append(QStringLiteral("%1: a projection digest could not be updated.").arg(path));
                            failed = true;
                            break;
                        }
                    }
                }
                if (failed) {
                    break;
                }
            }
            if (failed) {
                break;
            }
            if (rewrittenRows) {
                counts.insert(name, rewrittenRows);
            }
        }
        if (failed) {
            database.db.rollback();
            return;
        }
        if (!database.db.commit()) {
            m_errors.append(QStringLiteral("%1 could not be committed: %2").arg(path, database.db.lastError().text()));
            return;
        }
        if (!counts.isEmpty()) {
            m_databases.insert(path, counts);
        }
    }

    // Step 5: Library search rows keyed by a legacy path; the index rebuilds them.
    void dropSearchRows(const QString &path)
    {
        if (!QFileInfo(path).isFile()) {
            return;
        }
        Database database(path);
        if (!database.db.open()) {
            return;
        }
        qint64 dropped = 0;
        for (const auto &[table, column] : {QPair<QString, QString>{QStringLiteral("indexed_sources"), QStringLiteral("path")},
                                            {QStringLiteral("source_entries"), QStringLiteral("path")},
                                            {QStringLiteral("indexed_pdfs"), QStringLiteral("pdf_path")},
                                            {QStringLiteral("titles"), QStringLiteral("pdf_path")},
                                            {QStringLiteral("entries"), QStringLiteral("pdf_path")}}) {
            QSqlQuery query(database.db);
            query.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 LIKE ?").arg(table, column));
            query.addBindValue(QString(LikeFilter));
            if (query.exec()) {
                dropped += std::max(0, query.numRowsAffected());
            }
        }
        if (dropped) {
            m_report.insert(QStringLiteral("library_search_rows_dropped"), dropped);
        }
    }

    // Count what Rule R would still change in a text file or a database.
    qint64 scanFile(const QString &path) const
    {
        const auto bytes = readFile(path);
        return bytes ? legacyCount(*bytes) : 0;
    }
    qint64 scanDatabase(const QString &path) const
    {
        if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
            return 0;
        }
        Database database(path);
        if (!database.db.open()) {
            return 0;
        }
        QSqlQuery tables(database.db);
        if (!tables.exec(QStringLiteral("SELECT name, sql FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"))) {
            return 0;
        }
        QList<QPair<QString, QString>> names;
        while (tables.next()) {
            names.append({tables.value(0).toString(), tables.value(1).toString()});
        }
        qint64 remaining = 0;
        for (const auto &[name, sql] : std::as_const(names)) {
            if (sql.startsWith(QLatin1String("CREATE VIRTUAL TABLE"), Qt::CaseInsensitive)) {
                continue;
            }
            QSqlQuery columns(database.db);
            if (!columns.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(name))) {
                continue;
            }
            QStringList columnNames;
            while (columns.next()) {
                columnNames.append(columns.value(1).toString());
            }
            for (const QString &column : std::as_const(columnNames)) {
                QSqlQuery select(database.db);
                select.prepare(QStringLiteral("SELECT CAST(\"%1\" AS BLOB) FROM \"%2\" WHERE CAST(\"%1\" AS TEXT) LIKE ?").arg(column, name));
                select.addBindValue(QString(LikeFilter));
                if (!select.exec()) {
                    continue;
                }
                while (select.next()) {
                    remaining += legacyCount(select.value(0).toByteArray());
                }
            }
        }
        return remaining;
    }

    void finish()
    {
        if (!m_files.isEmpty()) {
            m_report.insert(QStringLiteral("files"), m_files);
        }
        if (!m_databases.isEmpty()) {
            m_report.insert(QStringLiteral("databases"), m_databases);
        }
        if (!m_renamed.isEmpty()) {
            m_report.insert(QStringLiteral("renamed"), m_renamed);
        }
        if (!m_moved.isEmpty()) {
            m_report.insert(QStringLiteral("folders"), m_moved);
        }
    }

    const QStringList &errors() const
    {
        return m_errors;
    }
    bool changed() const
    {
        return !m_files.isEmpty() || !m_databases.isEmpty() || !m_renamed.isEmpty() || !m_moved.isEmpty();
    }

private:
    void appendMoved(const QString &hash, const QString &how)
    {
        m_moved.append(QJsonObject{{QStringLiteral("hash"), hash}, {QStringLiteral("action"), how}});
    }

    const DefaultFolderMigration::Paths &m_paths;
    QJsonObject &m_report;
    QStringList m_errors;
    QStringList m_rewrittenFiles;
    QJsonObject m_files;
    QJsonObject m_databases;
    QJsonArray m_renamed;
    QJsonArray m_moved;
};

QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}
}

DefaultFolderMigration::DefaultFolderMigration(Paths paths)
    : m_paths(std::move(paths))
{
}

QString DefaultFolderMigration::journalPath() const
{
    return QDir(m_paths.stateRoot).filePath(JournalName);
}

QJsonObject DefaultFolderMigration::journal() const
{
    const auto bytes = readFile(journalPath());
    return bytes ? QJsonDocument::fromJson(*bytes).object() : QJsonObject();
}

bool DefaultFolderMigration::interrupted() const
{
    const QJsonObject value = journal();
    return !value.isEmpty() && value.value(QStringLiteral("phase")).toString() != QLatin1String("done");
}

QStringList DefaultFolderMigration::legacyFolders() const
{
    QStringList result;
    const QDir projects(DefaultFolderLocation::projectsDirectory(m_paths.docsRoot));
    for (const QString &name : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        if (name.size() == SourceOwner::HashLength && SourceOwner::isLowerHex(name)
            && QFileInfo(projects.filePath(name + QLatin1Char('/') + SourceOwner::LegacyDefaultSegment)).isDir()) {
            result.append(name);
        }
    }
    return result;
}

bool DefaultFolderMigration::legacyStateRemains() const
{
    for (const QString &path : {QDir(m_paths.stateRoot).filePath(QStringLiteral("source-publication.json")),
                                QDir(m_paths.docsRoot).filePath(QStringLiteral(".thoughts/library-projects.json"))}) {
        const auto bytes = readFile(path);
        if (bytes && SourceOwner::containsLegacyDefaultFolder(*bytes)) {
            return true;
        }
    }
    return false;
}

bool DefaultFolderMigration::fedoraUsesCanonicalSegment(const QJsonObject &librarySnapshot)
{
    for (const auto &projectValue : librarySnapshot.value(QStringLiteral("projects")).toArray()) {
        const QJsonObject project = projectValue.toObject();
        for (const auto &manifest : project.value(QStringLiteral("source_manifests")).toArray()) {
            const QString path = manifest.toObject().value(QStringLiteral("path")).toString();
            if (SourceOwner::isManifestPath(path) && SourceOwner::defaultFolderPrefixLength(path) == SourceOwner::DefaultFolderLength) {
                return true;
            }
        }
        const QJsonObject configuration = project.value(QStringLiteral("project_configuration")).toObject();
        for (const auto &folder : configuration.value(QStringLiteral("source_folders")).toArray()) {
            const QJsonObject value = folder.toObject();
            if (value.value(QStringLiteral("folder_id")) == configuration.value(QStringLiteral("default_folder_id"))
                && SourceOwner::isDefaultFolder(value.value(QStringLiteral("path")).toString())
                && !SourceOwner::isLegacyDefaultFolder(value.value(QStringLiteral("path")).toString())) {
                return true;
            }
        }
    }
    return false;
}

bool DefaultFolderMigration::needed(const QJsonObject &librarySnapshot) const
{
    if (interrupted()) {
        return true;
    }
    return fedoraUsesCanonicalSegment(librarySnapshot) && (!legacyFolders().isEmpty() || legacyStateRemains());
}

DefaultFolderMigration::Result DefaultFolderMigration::run()
{
    Result result;
    const QStringList folders = legacyFolders();
    QJsonObject value = journal();
    if (!interrupted() && folders.isEmpty() && !legacyStateRemains()) {
        return result;
    }
    const auto fail = [&](const QString &error, const QString &details) {
        result.success = false;
        result.error = error;
        result.details = details;
        value.insert(QStringLiteral("phase"), QStringLiteral("needs_attention"));
        value.insert(QStringLiteral("problem"), details.isEmpty() ? error : details);
        value.insert(QStringLiteral("updated_at"), now());
        writeFile(journalPath(), QJsonDocument(value).toJson());
        return result;
    };

    // The publisher, the editable-source installer and the review source
    // writers take this lock before touching the publication state.
    std::unique_ptr<Publisher::SourceStateLock> stateLock;
    try {
        stateLock = std::make_unique<Publisher::SourceStateLock>(Py::Path(QDir(m_paths.stateRoot).filePath(QStringLiteral("source-publication.json"))), 30);
    } catch (const Py::Exception &error) {
        result.success = false;
        result.error = QStringLiteral("The Project folders could not be renamed: another source writer is active.");
        result.details = error.line();
        return result;
    }
    const QString cataloguePath = QDir(m_paths.docsRoot).filePath(QStringLiteral(".thoughts/library-projects.json"));
    QDir().mkpath(QFileInfo(cataloguePath).absolutePath());
    QLockFile catalogueLock(cataloguePath + QStringLiteral(".lock"));
    if (!catalogueLock.tryLock(10000)) {
        result.success = false;
        result.error = QStringLiteral("The Project folders could not be renamed: the project catalogue is busy.");
        return result;
    }

    // Step 1: the journal.
    if (value.isEmpty() || value.value(QStringLiteral("phase")).toString() == QLatin1String("done")) {
        value = QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("started_at"), now()}};
    }
    value.insert(QStringLiteral("docs_root"), m_paths.docsRoot);
    QJsonArray journalFolders = value.value(QStringLiteral("folders")).toArray();
    for (const QString &hash : folders) {
        if (!journalFolders.contains(hash)) {
            journalFolders.append(hash);
        }
    }
    value.insert(QStringLiteral("folders"), journalFolders);
    value.insert(QStringLiteral("phase"), QStringLiteral("moving"));
    value.remove(QStringLiteral("problem"));
    value.remove(QStringLiteral("report"));
    value.insert(QStringLiteral("updated_at"), now());
    if (!QDir().mkpath(m_paths.stateRoot) || !writeFile(journalPath(), QJsonDocument(value).toJson())) {
        result.success = false;
        result.error = QStringLiteral("The Project folder rename could not be recorded.");
        return result;
    }

    QJsonObject report;
    Migrator migrator(m_paths, report);

    // Step 2: one rename per Project.
    QStringList problems;
    for (const QString &hash : folders) {
        migrator.moveFolder(hash, &problems);
    }
    if (!problems.isEmpty()) {
        return fail(QStringLiteral("A Project folder needs attention: both its old and its new default folder hold different files."), problems.join(QLatin1Char('\n')));
    }
    value.insert(QStringLiteral("phase"), QStringLiteral("rewriting"));
    value.insert(QStringLiteral("updated_at"), now());
    writeFile(journalPath(), QJsonDocument(value).toJson());

    // Step 3: Rule R on every local record of source paths.
    const QStringList stateRoots = migrator.stateRoots();
    for (const QString &stateRoot : stateRoots) {
        // source-publication.json (bases, pending_publication, materialized
        // sources, conflicts, authoring reconciliation), repository.json,
        // source-comment-snapshot.json, render statuses, the dependency graph...
        migrator.rewriteFiles(stateRoot, {QStringLiteral("*.json")}, false);
        migrator.rewriteFiles(QDir(stateRoot).filePath(QStringLiteral("source-comment-journal")), {QStringLiteral("*.json")}, true);
        migrator.rewriteFiles(QDir(stateRoot).filePath(QStringLiteral("comment-sidecar-pending")), {QStringLiteral("*.json"), QStringLiteral("*.jsonl")}, true);
        migrator.renameMergeRecoveries(stateRoot);
        migrator.moveConflictFiles(stateRoot);
        // desktop-sync.sqlite (pending paths, claims, baselines, observed
        // paths, the outbox, proposal decisions), repository projections,
        // current annotations, the review query index. sync-operations.sqlite
        // is the Sync history and keeps the paths it reported.
        const QDir directory(stateRoot);
        for (const QString &name : directory.entryList({QStringLiteral("*.sqlite")}, QDir::Files, QDir::Name)) {
            if (name != QLatin1String("sync-operations.sqlite")) {
                migrator.rewriteDatabase(directory.filePath(name));
            }
        }
    }
    if (!m_paths.thoughtsDataRoot.isEmpty()) {
        migrator.rewriteFile(QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-manifest.json")));
    }
    const QDir docs(m_paths.docsRoot);
    // The Library catalogue (pending operations, generated_manifests keys),
    // document identities (local_path), the Sources inventory, link records.
    migrator.rewriteFiles(docs.filePath(QStringLiteral(".thoughts")), {QStringLiteral("*.json")}, false);
    // Comment layouts, legacy sidecars and the proposal history.
    migrator.rewriteFiles(docs.filePath(QStringLiteral("comments")), {QStringLiteral("*.json"), QStringLiteral("*.jsonl")}, true);
    // Review records kept beside the Library; content-addressed objects keep their bytes.
    migrator.rewriteFiles(docs.filePath(QStringLiteral("reviews")), {QStringLiteral("*.json"), QStringLiteral("*.jsonl")}, true,
                          {QStringLiteral("objects"), QStringLiteral("source-objects")});

    // Step 4: the local Project manifests, byte-equal to Fedora's. A copy
    // this computer installed or wrote itself stays recorded as its own.
    const QDir projects(DefaultFolderLocation::projectsDirectory(m_paths.docsRoot));
    QHash<QString, QPair<QString, QString>> manifestDigests; // relative path -> (old, new) SHA-256
    for (const QString &name : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString manifest = projects.filePath(name + QLatin1Char('/') + SourceOwner::DefaultSegment + QStringLiteral("/thoughts-project.json"));
        const auto before = readFile(manifest);
        migrator.rewriteFile(manifest);
        const auto after = readFile(manifest);
        if (before && after && *before != *after) {
            manifestDigests.insert(docs.relativeFilePath(manifest), {sha256Hex(*before), sha256Hex(*after)});
        }
    }
    if (!manifestDigests.isEmpty()) {
        const auto bytes = readFile(cataloguePath);
        QJsonObject catalogue = bytes ? QJsonDocument::fromJson(*bytes).object() : QJsonObject();
        QJsonObject generated = catalogue.value(QStringLiteral("generated_manifests")).toObject();
        bool changed = false;
        for (auto it = manifestDigests.cbegin(); it != manifestDigests.cend(); ++it) {
            if (generated.value(it.key()).toString() == it.value().first) {
                generated.insert(it.key(), it.value().second);
                changed = true;
            }
        }
        if (changed) {
            catalogue.insert(QStringLiteral("generated_manifests"), generated);
            // LibraryProjectStore's own layout.
            if (!writeFile(cataloguePath, QJsonDocument(catalogue).toJson())) {
                return fail(QStringLiteral("The Project folder rename could not update the project catalogue."), cataloguePath);
            }
        }
    }

    // Step 5: rebuildable caches keyed by path.
    const QString locationIndex = docs.filePath(QStringLiteral(".runtime/source-location-index.json"));
    if (QFileInfo::exists(locationIndex) && QFile::remove(locationIndex)) {
        report.insert(QStringLiteral("source_location_index_dropped"), true);
    }
    if (!m_paths.applicationDataRoot.isEmpty()) {
        migrator.dropSearchRows(QDir(m_paths.applicationDataRoot).filePath(QStringLiteral("library-search.sqlite")));
    }

    if (!migrator.errors().isEmpty()) {
        migrator.finish();
        value.insert(QStringLiteral("report"), report);
        return fail(QStringLiteral("The Project folder rename could not update this computer's Sync records."), migrator.errors().join(QLatin1Char('\n')));
    }

    // Final scan: nothing outside the history may name a legacy folder now.
    QJsonObject remaining;
    qint64 remainingTotal = 0;
    const auto scanFiles = [&](const QString &directory, const QStringList &filters, bool recursive) {
        if (!QFileInfo(directory).isDir()) {
            return;
        }
        QDirIterator walk(directory, filters, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot,
                          recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (walk.hasNext()) {
            const QString path = walk.next();
            if (walk.fileName() == JournalName) {
                continue;
            }
            const qint64 count = migrator.scanFile(path);
            if (count) {
                remaining.insert(path, count);
                remainingTotal += count;
            }
        }
    };
    for (const QString &stateRoot : stateRoots) {
        scanFiles(stateRoot, {QStringLiteral("*.json")}, false);
        const QDir directory(stateRoot);
        for (const QString &name : directory.entryList({QStringLiteral("*.sqlite")}, QDir::Files, QDir::Name)) {
            if (name == QLatin1String("sync-operations.sqlite")) {
                continue;
            }
            const qint64 count = migrator.scanDatabase(directory.filePath(name));
            if (count) {
                remaining.insert(directory.filePath(name), count);
                remainingTotal += count;
            }
        }
    }
    scanFiles(docs.filePath(QStringLiteral(".thoughts")), {QStringLiteral("*.json")}, false);
    report.insert(QStringLiteral("remaining_legacy_matches"), remainingTotal);
    if (!remaining.isEmpty()) {
        report.insert(QStringLiteral("remaining"), remaining);
    }
    if (!legacyFolders().isEmpty()) {
        migrator.finish();
        value.insert(QStringLiteral("report"), report);
        return fail(QStringLiteral("A Project folder could not be renamed."), legacyFolders().join(QLatin1Char('\n')));
    }

    // Step 6.
    migrator.finish();
    result.migrated = migrator.changed();
    result.report = report;
    value.insert(QStringLiteral("phase"), QStringLiteral("done"));
    value.insert(QStringLiteral("finished_at"), now());
    value.insert(QStringLiteral("updated_at"), now());
    value.insert(QStringLiteral("report"), report);
    if (!writeFile(journalPath(), QJsonDocument(value).toJson())) {
        result.success = false;
        result.error = QStringLiteral("The Project folder rename could not be recorded as finished.");
    }
    return result;
}

}
