// SPDX-License-Identifier: GPL-2.0-or-later
#include "sourcefoldermigration.h"

#include "sourceowner.h"
#include "thoughtsplatform.h"

#include "../tools/publisher/pyerror.h"
#include "../tools/publisher/statelock.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <memory>
#include <optional>
#include <tuple>

namespace Thoughts
{
namespace
{
// The map itself stays as Fedora sent it wherever it is stored.
const QStringList KeptKeys{QStringLiteral("source_folder_renames")};

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

bool looksLikeJson(const QByteArray &bytes)
{
    for (const char c : bytes) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            continue;
        }
        return c == '{' || c == '[';
    }
    return false;
}

// Rule P on a stored value: JSON keeps its contents fields and multi-line
// strings; other text is rewritten only when it is one line.
QByteArray rewriteStored(const QByteArray &bytes, const SourceFolderRenames &renames, qsizetype *count)
{
    if (looksLikeJson(bytes)) {
        return SourceFolderNames::rewriteJsonText(bytes, renames, count, KeptKeys);
    }
    if (bytes.contains('\n') || bytes.contains('\r')) {
        return bytes;
    }
    return SourceFolderNames::rewrite(bytes, renames, count);
}

qsizetype oldCount(const QByteArray &bytes, const SourceFolderRenames &renames)
{
    qsizetype count = 0;
    rewriteStored(bytes, renames, &count);
    return count;
}

QString now()
{
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

SourceFolderRenames merged(const SourceFolderRenames &first, const SourceFolderRenames &second)
{
    SourceFolderRenames result = first;
    QSet<QString> olds;
    for (const SourceFolderRename &rename : first) {
        olds.insert(rename.oldName);
    }
    for (const SourceFolderRename &rename : second) {
        if (!olds.contains(rename.oldName)) {
            olds.insert(rename.oldName);
            result.append(rename);
        }
    }
    return result;
}

class Migrator
{
public:
    Migrator(const SourceFolderMigration::Paths &paths, const SourceFolderRenames &renames)
        : m_paths(paths)
        , m_renames(renames)
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

    // Step 3: docs-<old> -> docs-<new>, one rename; or a merge into an
    // existing docs-<new> that stops at the first difference.
    bool moveFolder(const SourceFolderRename &rename, QStringList *problems)
    {
        const QDir docs(m_paths.docsRoot);
        const QString source = docs.filePath(rename.oldName);
        const QString target = docs.filePath(rename.newName);
        const QFileInfo sourceInfo(source);
        if (!sourceInfo.exists() && !sourceInfo.isSymLink()) {
            return true;
        }
        const bool targetExists = QFileInfo::exists(target) || QFileInfo(target).isSymLink();
        if (!targetExists) {
            // A symbolic link moves as the link itself.
            if (!(sourceInfo.isSymLink() ? QFile::rename(source, target) : QDir().rename(source, target))) {
                problems->append(QStringLiteral("%1 could not be renamed to %2.").arg(source, target));
                return false;
            }
            appendMoved(rename, QStringLiteral("renamed"));
            return true;
        }
        if (sourceInfo.isSymLink() || !sourceInfo.isDir() || !QFileInfo(target).isDir() || QFileInfo(target).isSymLink()) {
            problems->append(QStringLiteral("%1 and %2 both exist.").arg(source, target));
            return false;
        }
        QStringList files;
        QDirIterator walk(source, QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (walk.hasNext()) {
            files.append(walk.next());
        }
        std::sort(files.begin(), files.end());
        QStringList differing;
        // Check everything before moving anything: a difference leaves both folders as they were.
        for (const QString &file : std::as_const(files)) {
            const QString relative = QDir(source).relativeFilePath(file);
            const QString destination = QDir(target).filePath(relative);
            if (QFileInfo(file).isSymLink() || QFileInfo(destination).isSymLink()
                || (QFileInfo::exists(destination) && !(QFileInfo(destination).isFile() && sameFileContent(file, destination)))) {
                differing.append(relative);
            }
        }
        if (!differing.isEmpty()) {
            for (const QString &relative : std::as_const(differing)) {
                problems->append(QStringLiteral("%1 differs from %2.").arg(QDir(source).filePath(relative), QDir(target).filePath(relative)));
            }
            return false;
        }
        for (const QString &file : std::as_const(files)) {
            const QString destination = QDir(target).filePath(QDir(source).relativeFilePath(file));
            if (QFileInfo::exists(destination)) {
                if (!QFile::remove(file)) {
                    problems->append(QStringLiteral("%1 could not be removed.").arg(file));
                    return false;
                }
            } else if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::rename(file, destination)) {
                problems->append(QStringLiteral("%1 could not be moved to %2.").arg(file, destination));
                return false;
            }
        }
        QStringList directories;
        QDirIterator dirs(source, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirs.hasNext()) {
            directories.append(dirs.next());
        }
        std::sort(directories.begin(), directories.end(), [](const QString &a, const QString &b) {
            return a.size() > b.size();
        });
        for (const QString &directory : std::as_const(directories)) {
            QDir().rmdir(directory);
        }
        if (!QDir().rmdir(source)) {
            problems->append(QStringLiteral("%1 still contains files after moving them to %2.").arg(source, target));
            return false;
        }
        appendMoved(rename, QStringLiteral("merged"));
        return true;
    }

    // Rule P on one text file; false on a write failure.
    bool rewriteFile(const QString &path)
    {
        const auto bytes = readFile(path);
        if (!bytes || !bytes->contains("docs-")) {
            return true;
        }
        qsizetype count = 0;
        const QByteArray rewritten = rewriteStored(*bytes, m_renames, &count);
        if (!count || rewritten == *bytes) {
            return true;
        }
        if (!writeFile(path, rewritten)) {
            m_errors.append(QStringLiteral("%1 could not be rewritten.").arg(path));
            return false;
        }
        m_files.insert(path, count);
        return true;
    }

    void rewriteFiles(const QString &directory, const QStringList &nameFilters, bool recursive, const QStringList &skippedDirectories = {})
    {
        if (!QFileInfo(directory).isDir()) {
            return;
        }
        QDirIterator walk(directory, nameFilters, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags);
        while (walk.hasNext()) {
            const QString path = walk.next();
            const QString relative = QDir(directory).relativeFilePath(path);
            // The journals record what the migrations did, old names included.
            if (walk.fileName() == SourceFolderMigration::JournalName || walk.fileName() == QLatin1String("default-folder-migration.json")) {
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

    // source-merge-recoveries/<sha256(path)>.{json,local}: renamed to sha256(P(path)).
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
            const QString rewrittenPath = SourceFolderNames::rewrite(path, m_renames);
            const QString stem = name.chopped(5);
            if (path.isEmpty() || rewrittenPath == path || stem != sha256Hex(path.toUtf8())) {
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
            qsizetype count = 0;
            if (!writeFile(directory.filePath(newStem + QStringLiteral(".json")), rewriteStored(*bytes, m_renames, &count)) || !QFile::remove(recordPath)) {
                m_errors.append(QStringLiteral("%1 could not be renamed.").arg(recordPath));
                continue;
            }
            m_renamed.append(QJsonObject{{QStringLiteral("from"), recordPath}, {QStringLiteral("to"), directory.filePath(newStem + QStringLiteral(".json"))}});
        }
    }

    // source-conflicts/<conflict id>/<source path>.local (and desktop-merge files):
    // the source path is part of the file path.
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
            const QString rewritten = SourceFolderNames::rewrite(relative, m_renames);
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
        QStringList directories;
        QDirIterator dirs(root, QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (dirs.hasNext()) {
            const QString directory = dirs.next();
            if (SourceFolderNames::rewrite(QDir(root).relativeFilePath(directory), m_renames) != QDir(root).relativeFilePath(directory)) {
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
        QString connection = QStringLiteral("source-folder-migration-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
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

    struct TableInfo {
        QString name;
        bool withoutRowid = false;
        QStringList columns;
    };
    // Ordinary tables (not FTS and their shadow tables) and their columns.
    static QList<TableInfo> tables(QSqlDatabase &db)
    {
        QSqlQuery query(db);
        QList<QPair<QString, QString>> names;
        if (query.exec(QStringLiteral("SELECT name, sql FROM sqlite_master WHERE type = 'table' AND name NOT LIKE 'sqlite_%'"))) {
            while (query.next()) {
                names.append({query.value(0).toString(), query.value(1).toString()});
            }
        }
        QSet<QString> virtualTables;
        for (const auto &[name, sql] : std::as_const(names)) {
            if (sql.startsWith(QLatin1String("CREATE VIRTUAL TABLE"), Qt::CaseInsensitive)) {
                virtualTables.insert(name);
            }
        }
        QList<TableInfo> result;
        for (const auto &[name, sql] : std::as_const(names)) {
            if (virtualTables.contains(name) || std::any_of(virtualTables.cbegin(), virtualTables.cend(), [&name](const QString &table) {
                    return name.startsWith(table + QLatin1Char('_'));
                })) {
                continue;
            }
            TableInfo info{name, sql.contains(QLatin1String("WITHOUT ROWID"), Qt::CaseInsensitive), {}};
            QSqlQuery columns(db);
            if (columns.exec(QStringLiteral("PRAGMA table_info(\"%1\")").arg(name))) {
                while (columns.next()) {
                    const QString column = columns.value(1).toString();
                    // Free text keeps its bytes.
                    if (!SourceFolderNames::isContentsKey(column)) {
                        info.columns.append(column);
                    }
                }
            }
            result.append(info);
        }
        return result;
    }

    // Rule P on every text and blob column. A rewritten value that collides
    // with an existing row's key keeps that row (Fedora's, already under the
    // new name) and drops the old one.
    void rewriteDatabase(const QString &path)
    {
        if (!QFileInfo(path).isFile() || QFileInfo(path).size() == 0) {
            return;
        }
        Database database(path);
        if (!database.db.open()) {
            m_errors.append(QStringLiteral("%1 could not be opened: %2").arg(path, database.db.lastError().text()));
            return;
        }
        const QList<TableInfo> infos = tables(database.db);
        if (!database.db.transaction()) {
            m_errors.append(QStringLiteral("%1 is busy: %2").arg(path, database.db.lastError().text()));
            return;
        }
        QJsonObject counts;
        qint64 droppedTotal = 0;
        bool failed = false;
        for (const TableInfo &table : infos) {
            const QString quotedTable = QLatin1Char('"') + table.name + QLatin1Char('"');
            qint64 rewrittenRows = 0;
            for (const QString &column : table.columns) {
                const QString quoted = QLatin1Char('"') + column + QLatin1Char('"');
                QSqlQuery select(database.db);
                select.prepare(QStringLiteral("SELECT %1, %2, typeof(%2) FROM %3 WHERE CAST(%2 AS TEXT) LIKE '%docs-%'")
                                   .arg(table.withoutRowid ? QStringLiteral("NULL") : QStringLiteral("rowid"), quoted, quotedTable));
                if (!select.exec()) {
                    continue;
                }
                QList<std::tuple<qint64, QString, QByteArray>> changes;
                while (select.next()) {
                    const QString type = select.value(2).toString();
                    const QByteArray before = type == QLatin1String("blob") ? select.value(1).toByteArray() : select.value(1).toString().toUtf8();
                    qsizetype count = 0;
                    const QByteArray after = rewriteStored(before, m_renames, &count);
                    if (!count || after == before) {
                        continue;
                    }
                    if (table.withoutRowid) {
                        m_errors.append(QStringLiteral("%1: table %2 has no row ids to rewrite.").arg(path, table.name));
                        failed = true;
                        break;
                    }
                    changes.append({select.value(0).toLongLong(), type, after});
                }
                select.finish();
                for (const auto &[rowid, type, after] : std::as_const(changes)) {
                    if (failed) {
                        break;
                    }
                    QSqlQuery update(database.db);
                    update.prepare(QStringLiteral("UPDATE OR IGNORE %1 SET %2 = ? WHERE rowid = ?").arg(quotedTable, quoted));
                    if (type == QLatin1String("blob")) {
                        update.addBindValue(after);
                    } else {
                        update.addBindValue(QString::fromUtf8(after));
                    }
                    update.addBindValue(rowid);
                    if (!update.exec()) {
                        m_errors.append(QStringLiteral("%1: %2.%3 could not be rewritten: %4").arg(path, table.name, column, update.lastError().text()));
                        failed = true;
                        break;
                    }
                    if (update.numRowsAffected() == 0) {
                        // The rewritten key exists already: keep that row.
                        QSqlQuery drop(database.db);
                        drop.prepare(QStringLiteral("DELETE FROM %1 WHERE rowid = ?").arg(quotedTable));
                        drop.addBindValue(rowid);
                        if (!drop.exec()) {
                            m_errors.append(QStringLiteral("%1: a duplicate row of %2 could not be dropped: %3").arg(path, table.name, drop.lastError().text()));
                            failed = true;
                            break;
                        }
                        ++droppedTotal;
                        continue;
                    }
                    ++rewrittenRows;
                    // RepositoryProjectionStore verifies each payload's SHA-256.
                    if (table.name == QLatin1String("records") && column == QLatin1String("payload") && table.columns.contains(QStringLiteral("sha256"))) {
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
                counts.insert(table.name, rewrittenRows);
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
        if (droppedTotal) {
            counts.insert(QStringLiteral("_duplicate_rows_dropped"), droppedTotal);
        }
        if (!counts.isEmpty()) {
            m_databases.insert(path, counts);
        }
    }

    // Library search rows of an old folder; the index rebuilds them.
    qint64 dropSearchRows(const QString &path)
    {
        if (!QFileInfo(path).isFile()) {
            return 0;
        }
        Database database(path);
        if (!database.db.open()) {
            return 0;
        }
        qint64 dropped = 0;
        for (const auto &[table, column] : {QPair<QString, QString>{QStringLiteral("indexed_sources"), QStringLiteral("path")},
                                            {QStringLiteral("source_entries"), QStringLiteral("path")},
                                            {QStringLiteral("indexed_pdfs"), QStringLiteral("pdf_path")},
                                            {QStringLiteral("titles"), QStringLiteral("pdf_path")},
                                            {QStringLiteral("entries"), QStringLiteral("pdf_path")}}) {
            for (const SourceFolderRename &rename : m_renames) {
                QSqlQuery query(database.db);
                query.prepare(QStringLiteral("DELETE FROM %1 WHERE %2 LIKE ?").arg(table, column));
                query.addBindValue(QString(QStringLiteral("%") + rename.oldName + QStringLiteral("%")));
                if (query.exec()) {
                    dropped += std::max(0, query.numRowsAffected());
                }
            }
        }
        return dropped;
    }

    // What rule P would still change.
    qint64 scanFile(const QString &path) const
    {
        const auto bytes = readFile(path);
        return bytes && bytes->contains("docs-") ? oldCount(*bytes, m_renames) : 0;
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
        qint64 remaining = 0;
        for (const TableInfo &table : tables(database.db)) {
            for (const QString &column : table.columns) {
                QSqlQuery select(database.db);
                select.prepare(QStringLiteral("SELECT CAST(\"%1\" AS BLOB) FROM \"%2\" WHERE CAST(\"%1\" AS TEXT) LIKE '%docs-%'").arg(column, table.name));
                if (!select.exec()) {
                    continue;
                }
                while (select.next()) {
                    remaining += oldCount(select.value(0).toByteArray(), m_renames);
                }
            }
        }
        return remaining;
    }

    QJsonObject report() const
    {
        QJsonObject result;
        if (!m_files.isEmpty()) {
            result.insert(QStringLiteral("files"), m_files);
        }
        if (!m_databases.isEmpty()) {
            result.insert(QStringLiteral("databases"), m_databases);
        }
        if (!m_renamed.isEmpty()) {
            result.insert(QStringLiteral("renamed"), m_renamed);
        }
        if (!m_moved.isEmpty()) {
            result.insert(QStringLiteral("folders"), m_moved);
        }
        return result;
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
    void appendMoved(const SourceFolderRename &rename, const QString &how)
    {
        m_moved.append(QJsonObject{{QStringLiteral("old"), rename.oldName}, {QStringLiteral("new"), rename.newName}, {QStringLiteral("action"), how}});
    }

    const SourceFolderMigration::Paths &m_paths;
    const SourceFolderRenames &m_renames;
    QStringList m_errors;
    QJsonObject m_files;
    QJsonObject m_databases;
    QJsonArray m_renamed;
    QJsonArray m_moved;
};
}

SourceFolderMigration::SourceFolderMigration(Paths paths)
    : m_paths(std::move(paths))
{
}

QString SourceFolderMigration::journalPath() const
{
    return QDir(m_paths.stateRoot).filePath(JournalName);
}

QJsonObject SourceFolderMigration::journal() const
{
    const auto bytes = readFile(journalPath());
    return bytes ? QJsonDocument::fromJson(*bytes).object() : QJsonObject();
}

bool SourceFolderMigration::interrupted() const
{
    const QJsonObject value = journal();
    return !value.isEmpty() && value.value(QStringLiteral("phase")).toString() != QLatin1String("done");
}

SourceFolderRenames SourceFolderMigration::journalRenames() const
{
    return SourceFolderNames::fromJson(journal().value(QStringLiteral("renames")).toArray());
}

SourceFolderRenames SourceFolderMigration::renamesFrom(const QJsonObject &librarySnapshot)
{
    return SourceFolderNames::fromJson(librarySnapshot.value(QStringLiteral("source_folder_renames")).toArray());
}

SourceFolderRenames SourceFolderMigration::pending(const SourceFolderRenames &renames) const
{
    if (renames.isEmpty()) {
        return {};
    }
    const QDir docs(m_paths.docsRoot);
    // Publication state and queued Library changes: what Sync would send.
    QList<QByteArray> state;
    if (const auto publication = readFile(QDir(m_paths.stateRoot).filePath(QStringLiteral("source-publication.json")))) {
        state.append(*publication);
    }
    if (const auto catalogue = readFile(docs.filePath(QStringLiteral(".thoughts/library-projects.json")))) {
        state.append(QJsonDocument(QJsonDocument::fromJson(*catalogue).object().value(QStringLiteral("pending")).toArray()).toJson(QJsonDocument::Compact));
    }
    SourceFolderRenames result;
    for (const SourceFolderRename &rename : renames) {
        const QFileInfo old(docs.filePath(rename.oldName));
        bool remains = old.exists() || old.isSymLink();
        const QByteArray name = rename.oldName.toUtf8();
        for (const QByteArray &bytes : std::as_const(state)) {
            // A plain substring search first: the JSON-aware count only runs where the name occurs.
            remains = remains || (bytes.contains(name) && oldCount(bytes, {rename}) > 0);
        }
        if (remains) {
            result.append(rename);
        }
    }
    return result;
}

bool SourceFolderMigration::needed(const QJsonObject &librarySnapshot) const
{
    return interrupted() || !pending(renamesFrom(librarySnapshot)).isEmpty();
}

QStringList SourceFolderMigration::declaredFolders() const
{
    QStringList result;
    for (const QString &manifest : DefaultFolderLocation::manifestPaths(m_paths.docsRoot)) {
        const auto bytes = readFile(manifest);
        if (!bytes) {
            continue;
        }
        for (const QJsonValue &folder : QJsonDocument::fromJson(*bytes).object().value(QStringLiteral("source_folders")).toArray()) {
            const QString path = folder.toObject().value(QStringLiteral("path")).toString();
            if (SourceOwner::isConnectedFolder(path) && !result.contains(path)) {
                result.append(path);
            }
        }
    }
    result.sort();
    return result;
}

QStringList SourceFolderMigration::unresolvedOldFolders(const SourceFolderRenames &stored) const
{
    QStringList result;
    for (const QString &folder : declaredFolders()) {
        if (SourceFolderNames::isRuleNName(folder)) {
            continue;
        }
        const bool covered = std::any_of(stored.cbegin(), stored.cend(), [&folder](const SourceFolderRename &rename) {
            return rename.oldName == folder || rename.newName == folder;
        });
        if (!covered) {
            result.append(folder);
        }
    }
    return result;
}

QString SourceFolderMigration::catalogueKey(const QString &docsRoot)
{
    const auto bytes = readFile(QDir(docsRoot).filePath(QStringLiteral(".thoughts/library-projects.json")));
    if (!bytes) {
        return QStringLiteral("none");
    }
    const QJsonObject catalogue = QJsonDocument::fromJson(*bytes).object();
    QJsonArray projects;
    for (const QJsonValue &value : catalogue.value(QStringLiteral("projects")).toArray()) {
        const QJsonObject project = value.toObject();
        projects.append(QJsonArray{project.value(QStringLiteral("id")), project.value(QStringLiteral("revision"))});
    }
    const QJsonObject key{{QStringLiteral("projects"), projects}, {QStringLiteral("renames"), catalogue.value(QStringLiteral("source_folder_renames"))}};
    return sha256Hex(QJsonDocument(key).toJson(QJsonDocument::Compact));
}

SourceFolderMigration::DocumentSyncCheck SourceFolderMigration::documentSyncCheck(const SourceFolderRenames &stored, const QString &catalogueKey, QString *askedKey) const
{
    if (interrupted() || !pending(stored).isEmpty()) {
        return DocumentSyncCheck::ApplyStored;
    }
    if (askedKey && *askedKey == catalogueKey) {
        return DocumentSyncCheck::Nothing;
    }
    if (unresolvedOldFolders(stored).isEmpty()) {
        return DocumentSyncCheck::Nothing;
    }
    if (askedKey) {
        *askedKey = catalogueKey;
    }
    return DocumentSyncCheck::AskFedora;
}

SourceFolderMigration::Result SourceFolderMigration::run(const SourceFolderRenames &fedoraRenames)
{
    Result result;
    QJsonObject value = journal();
    const bool resumed = interrupted();
    // Every rename Fedora made, including those applied by an earlier run:
    // the publisher maps stale bases with the whole map.
    const SourceFolderRenames renames = merged(SourceFolderNames::fromJson(value.value(QStringLiteral("renames")).toArray()), fedoraRenames);
    const SourceFolderRenames todo = pending(renames);
    if (!resumed && todo.isEmpty()) {
        return result;
    }
    const QDir docs(m_paths.docsRoot);
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

    // Windows cannot rename a folder while another program has a file in it
    // open. Nothing moves until every folder is free.
    QStringList busy;
    for (const SourceFolderRename &rename : todo) {
        const QString folder = docs.filePath(rename.oldName);
        if (QFileInfo(folder).isDir() && !QFileInfo(folder).isSymLink() && Platform::directoryHasFilesInUse(folder)) {
            busy.append(folder);
        }
    }
    if (!busy.isEmpty()) {
        result.success = false;
        result.error = QStringLiteral("Source folders need new names, but files in them are open. Close them (PDFs, editors, terminals) and Sync again.");
        result.details = busy.join(QLatin1Char('\n'));
        return result;
    }

    std::unique_ptr<Publisher::SourceStateLock> stateLock;
    try {
        stateLock = std::make_unique<Publisher::SourceStateLock>(Py::Path(QDir(m_paths.stateRoot).filePath(QStringLiteral("source-publication.json"))), 30);
    } catch (const Py::Exception &error) {
        result.success = false;
        result.error = QStringLiteral("The source folders could not be renamed: another source writer is active.");
        result.details = error.line();
        return result;
    }
    const QString cataloguePath = docs.filePath(QStringLiteral(".thoughts/library-projects.json"));
    QDir().mkpath(QFileInfo(cataloguePath).absolutePath());
    QLockFile catalogueLock(cataloguePath + QStringLiteral(".lock"));
    if (!catalogueLock.tryLock(10000)) {
        result.success = false;
        result.error = QStringLiteral("The source folders could not be renamed: the project catalogue is busy.");
        return result;
    }

    // Step 1: the journal.
    if (value.isEmpty() || value.value(QStringLiteral("phase")).toString() == QLatin1String("done")) {
        value = QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("started_at"), now()}};
    }
    value.insert(QStringLiteral("docs_root"), m_paths.docsRoot);
    value.insert(QStringLiteral("renames"), SourceFolderNames::toJson(renames));
    value.insert(QStringLiteral("phase"), QStringLiteral("moving"));
    value.remove(QStringLiteral("problem"));
    value.remove(QStringLiteral("report"));
    value.insert(QStringLiteral("updated_at"), now());
    if (!QDir().mkpath(m_paths.stateRoot) || !writeFile(journalPath(), QJsonDocument(value).toJson())) {
        result.success = false;
        result.error = QStringLiteral("The source folder rename could not be recorded.");
        return result;
    }

    Migrator migrator(m_paths, renames);

    // Step 3: one rename per folder.
    QStringList problems;
    for (const SourceFolderRename &rename : renames) {
        migrator.moveFolder(rename, &problems);
    }
    if (!problems.isEmpty()) {
        return fail(QStringLiteral("A source folder could not be given its new name and needs attention (see the details). Sync tries again."), problems.join(QLatin1Char('\n')));
    }
    value.insert(QStringLiteral("phase"), QStringLiteral("rewriting"));
    value.insert(QStringLiteral("updated_at"), now());
    writeFile(journalPath(), QJsonDocument(value).toJson());

    // Step 4: rule P on every local record of source paths and folder names.
    const QStringList stateRoots = migrator.stateRoots();
    for (const QString &stateRoot : stateRoots) {
        // source-publication.json (bases, pending publication, materialized
        // sources, conflicts), repository.json, source-comment-snapshot.json,
        // render statuses, the dependency graph...
        migrator.rewriteFiles(stateRoot, {QStringLiteral("*.json")}, false);
        migrator.rewriteFiles(QDir(stateRoot).filePath(QStringLiteral("source-comment-journal")), {QStringLiteral("*.json")}, true);
        migrator.rewriteFiles(QDir(stateRoot).filePath(QStringLiteral("comment-sidecar-pending")),
                              {QStringLiteral("*.json"), QStringLiteral("*.jsonl"), QStringLiteral("*.journal")}, true);
        migrator.rewriteFiles(QDir(stateRoot).filePath(QStringLiteral("build-runs")), {QStringLiteral("*.json")}, true);
        migrator.renameMergeRecoveries(stateRoot);
        migrator.moveConflictFiles(stateRoot);
        // desktop-sync.sqlite, repository projections, the review query index,
        // current annotations. sync-operations.sqlite is the Sync history.
        const QDir directory(stateRoot);
        for (const QString &name : directory.entryList({QStringLiteral("*.sqlite")}, QDir::Files, QDir::Name)) {
            if (name != QLatin1String("sync-operations.sqlite")) {
                migrator.rewriteDatabase(directory.filePath(name));
            }
        }
        migrator.rewriteDatabase(directory.filePath(QStringLiteral(".runtime/current-annotations.sqlite")));
    }
    if (!m_paths.thoughtsDataRoot.isEmpty()) {
        migrator.rewriteFile(QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-manifest.json")));
        migrator.rewriteFile(QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-sync-state.json")));
    }
    // The Library catalogue (pending operations, generated_manifests keys;
    // source_folder_renames stays), document identities, the Sources
    // inventory, link records.
    migrator.rewriteFiles(docs.filePath(QStringLiteral(".thoughts")), {QStringLiteral("*.json")}, false);
    migrator.rewriteFiles(docs.filePath(QStringLiteral("comments")), {QStringLiteral("*.json"), QStringLiteral("*.jsonl")}, true);
    migrator.rewriteFiles(docs.filePath(QStringLiteral("reviews")), {QStringLiteral("*.json"), QStringLiteral("*.jsonl")}, true,
                          {QStringLiteral("objects"), QStringLiteral("source-objects")});
    migrator.rewriteFiles(docs.filePath(QStringLiteral("library")), {QStringLiteral("*.json")}, false);
    // current_pdfs/.thoughts-source-index.json is left alone: it belongs to
    // Fedora's published generation (a read-only stage whose SHA-256 the
    // generation records). Fedora rewrote its own index, and this Sync
    // installs that generation.

    // The local Project manifests, as Fedora rewrote its own. A copy this
    // computer installed or wrote itself stays recorded as its own.
    QHash<QString, QPair<QString, QString>> manifestDigests; // relative path -> (old, new) SHA-256
    for (const QString &manifest : DefaultFolderLocation::manifestPaths(m_paths.docsRoot)) {
        const auto before = readFile(manifest);
        migrator.rewriteFile(manifest);
        const auto after = readFile(manifest);
        if (before && after && *before != *after) {
            manifestDigests.insert(docs.relativeFilePath(manifest), {sha256Hex(*before), sha256Hex(*after)});
        }
    }
    // Folder manifests of the renamed folders, and the import receipt that
    // marks an untouched provisional one.
    for (const SourceFolderRename &rename : renames) {
        const QString folder = docs.filePath(rename.newName);
        const QString manifest = QDir(folder).filePath(QStringLiteral("thoughts-project.json"));
        const auto before = readFile(manifest);
        migrator.rewriteFile(manifest);
        const auto after = readFile(manifest);
        if (!before || !after || *before == *after) {
            continue;
        }
        const QString receiptPath = QDir(folder).filePath(QStringLiteral(".thoughts-import.json"));
        if (const auto receiptBytes = readFile(receiptPath)) {
            QJsonObject receipt = QJsonDocument::fromJson(*receiptBytes).object();
            if (receipt.value(QStringLiteral("manifest_sha256")).toString() == sha256Hex(*before)) {
                receipt.insert(QStringLiteral("manifest_sha256"), sha256Hex(*after));
                writeFile(receiptPath, QJsonDocument(receipt).toJson());
            }
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
            if (!writeFile(cataloguePath, QJsonDocument(catalogue).toJson())) {
                return fail(QStringLiteral("The source folder rename could not update the project catalogue."), cataloguePath);
            }
        }
    }

    // Step 5: rebuildable caches keyed by path.
    QJsonObject report;
    const QString locationIndex = docs.filePath(QStringLiteral(".runtime/source-location-index.json"));
    if (QFileInfo::exists(locationIndex) && QFile::remove(locationIndex)) {
        report.insert(QStringLiteral("source_location_index_dropped"), true);
    }
    // HTML editions (Fedora's, verified by SHA-256) list their PDF's source
    // files; the rebuild after the rename publishes new ones, and the HTML
    // view downloads them again.
    for (const QString &stateRoot : stateRoots) {
        QDir editions(QDir(stateRoot).filePath(QStringLiteral("html-editions")));
        if (editions.exists() && editions.removeRecursively()) {
            report.insert(QStringLiteral("html_editions_dropped"), true);
        }
    }
    if (!m_paths.applicationDataRoot.isEmpty()) {
        if (const qint64 dropped = migrator.dropSearchRows(QDir(m_paths.applicationDataRoot).filePath(QStringLiteral("library-search.sqlite")))) {
            report.insert(QStringLiteral("library_search_rows_dropped"), dropped);
        }
    }
    const QJsonObject changes = migrator.report();
    for (auto it = changes.constBegin(); it != changes.constEnd(); ++it) {
        report.insert(it.key(), it.value());
    }
    if (!migrator.errors().isEmpty()) {
        value.insert(QStringLiteral("report"), report);
        return fail(QStringLiteral("The source folder rename could not update this computer's Sync records."), migrator.errors().join(QLatin1Char('\n')));
    }

    // Step 6: the final scan.
    QJsonObject remaining;
    qint64 remainingTotal = 0;
    const auto scanFiles = [&](const QString &directory, const QStringList &filters) {
        if (!QFileInfo(directory).isDir()) {
            return;
        }
        QDirIterator walk(directory, filters, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot);
        while (walk.hasNext()) {
            const QString path = walk.next();
            if (walk.fileName() == JournalName || walk.fileName() == QLatin1String("default-folder-migration.json")) {
                continue;
            }
            if (const qint64 count = migrator.scanFile(path)) {
                remaining.insert(path, count);
                remainingTotal += count;
            }
        }
    };
    for (const QString &stateRoot : stateRoots) {
        scanFiles(stateRoot, {QStringLiteral("*.json")});
        const QDir directory(stateRoot);
        for (const QString &name : directory.entryList({QStringLiteral("*.sqlite")}, QDir::Files, QDir::Name)) {
            if (name == QLatin1String("sync-operations.sqlite")) {
                continue;
            }
            if (const qint64 count = migrator.scanDatabase(directory.filePath(name))) {
                remaining.insert(directory.filePath(name), count);
                remainingTotal += count;
            }
        }
    }
    scanFiles(docs.filePath(QStringLiteral(".thoughts")), {QStringLiteral("*.json")});
    report.insert(QStringLiteral("remaining_old_names"), remainingTotal);
    if (!remaining.isEmpty()) {
        report.insert(QStringLiteral("remaining"), remaining);
    }
    QStringList left;
    for (const SourceFolderRename &rename : renames) {
        const QFileInfo old(docs.filePath(rename.oldName));
        if (old.exists() || old.isSymLink()) {
            left.append(old.filePath());
        }
    }
    if (!left.isEmpty()) {
        value.insert(QStringLiteral("report"), report);
        return fail(QStringLiteral("A source folder could not be renamed."), left.join(QLatin1Char('\n')));
    }

    result.migrated = migrator.changed();
    result.report = report;
    value.insert(QStringLiteral("phase"), QStringLiteral("done"));
    value.insert(QStringLiteral("finished_at"), now());
    value.insert(QStringLiteral("updated_at"), now());
    value.insert(QStringLiteral("report"), report);
    if (!writeFile(journalPath(), QJsonDocument(value).toJson())) {
        result.success = false;
        result.error = QStringLiteral("The source folder rename could not be recorded as finished.");
    }
    return result;
}

}
