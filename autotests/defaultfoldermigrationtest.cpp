// SPDX-License-Identifier: GPL-2.0-or-later
// The client local migration of the default-folder rename (documents/ ->
// default/) over a fixture Library and state directory.
#include "../part/defaultfoldermigration.h"
#include "../part/libraryprojectstore.h"
#include "../part/projectconfiguration.h"
#include "../part/repositoryprojectionstore.h"
#include "../part/sourceowner.h"

#include <QElapsedTimer>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using Thoughts::DefaultFolderMigration;
using Thoughts::SourceOwner;

namespace
{
QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

void writeBytes(const QString &path, const QByteArray &bytes)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write(bytes), bytes.size());
}

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

void execute(QSqlDatabase &db, const QString &statement)
{
    QSqlQuery query(db);
    QVERIFY2(query.exec(statement), qPrintable(statement));
}
}

class DefaultFolderMigrationTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_temp;
    DefaultFolderMigration::Paths m_paths;
    QString m_hash;
    QString m_legacy; // .thoughts/projects/<hash>/documents
    QString m_canonical; // .thoughts/projects/<hash>/default

    QString docs(const QString &relative) const
    {
        return QDir(m_paths.docsRoot).filePath(relative);
    }
    QString state(const QString &relative) const
    {
        return QDir(m_paths.stateRoot).filePath(relative);
    }
    QByteArray manifestBytes(const QString &folder) const
    {
        const QUuid urlNamespace(QStringLiteral("6ba7b811-9dad-11d1-80b4-00c04fd430c8"));
        const QString folderId = QUuid::createUuidV5(urlNamespace, QStringLiteral("thoughts:default-folder:p")).toString(QUuid::WithoutBraces);
        const QString aggregate = QUuid::createUuidV5(urlNamespace, QStringLiteral("thoughts:default-document:p")).toString(QUuid::WithoutBraces);
        const QJsonObject value{
            {QStringLiteral("schema_version"), 5},
            {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
            {QStringLiteral("id"), QStringLiteral("p")},
            {QStringLiteral("name"), QStringLiteral("P")},
            {QStringLiteral("default_folder_id"), folderId},
            {QStringLiteral("source_folders"),
             QJsonArray{QJsonObject{{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), folder}, {QStringLiteral("document_id"), aggregate}}}},
            {QStringLiteral("documents"),
             QJsonArray{QJsonObject{{QStringLiteral("document_id"), QStringLiteral("8b0f5a52-79a4-4c3e-9d2e-0e0f8f6f7a11")},
                                    {QStringLiteral("format"), QStringLiteral("latex")},
                                    {QStringLiteral("included"), true},
                                    {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), QStringLiteral("notes.tex")}}}},
                        QJsonObject{{QStringLiteral("document_id"), QStringLiteral("9c1f6b63-8ab5-4d4f-8e3f-1f1a9a7a8b22")},
                                    {QStringLiteral("format"), QStringLiteral("pdf")},
                                    {QStringLiteral("included"), true},
                                    {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), QStringLiteral("imported.pdf")}}}}}},
            {QStringLiteral("rendering"), QJsonObject{{QStringLiteral("9c1f6b63-8ab5-4d4f-8e3f-1f1a9a7a8b22"), QJsonObject{{QStringLiteral("sha256"), sha256Hex("%PDF imported")}}}}},
            {QStringLiteral("document_names"), QJsonObject()}};
        // Python's json.dumps(indent=2) layout, like Fedora's manifests.
        return QJsonDocument(value).toJson();
    }

    // A legacy Library with an unsynced edit, an imported PDF and state that
    // names the old folder everywhere this computer records source paths.
    void buildFixture()
    {
        QVERIFY(m_temp.isValid());
        const QString base = m_temp.path() + QStringLiteral("/") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_paths = {base + QStringLiteral("/docs"), base + QStringLiteral("/app/thoughts-server"), base + QStringLiteral("/app"), base + QStringLiteral("/data")};
        m_hash = SourceOwner::projectHash(QStringLiteral("p"));
        m_legacy = SourceOwner::defaultFolder(m_hash, SourceOwner::LegacyDefaultSegment);
        m_canonical = SourceOwner::defaultFolder(m_hash, SourceOwner::DefaultSegment);
        const QString notes = m_legacy + QStringLiteral("/notes.tex");
        writeBytes(docs(m_legacy + QStringLiteral("/thoughts-project.json")), manifestBytes(m_legacy));
        writeBytes(docs(notes), "\\documentclass{article}\\begin{document}edited here, not yet synced\\end{document}\n");
        writeBytes(docs(m_legacy + QStringLiteral("/imported.pdf")), "%PDF imported");
        writeBytes(docs(m_legacy + QStringLiteral("/build/notes.aux")), "aux\n");
        writeBytes(docs(QStringLiteral("docs-x/tex/source/a.tex")), "a\n");

        // The publisher's state (Python json layout).
        const QByteArray publication = QStringLiteral(
                                           "{\"bases\": {\"%1\": {\"revision\": 3, \"sha256\": \"%2\"}, \"docs-x/tex/source/a.tex\": {\"revision\": 1, \"sha256\": \"%2\"}},"
                                           " \"pending_publication\": {\"mutations\": [{\"path\": \"%1\", \"action\": \"upsert\"}]},"
                                           " \"conflicts\": {\"c1\": {\"path\": \"%1\"}}, \"next_device_sequence\": 7}\n")
                                           .arg(notes, QString(64, QLatin1Char('0')))
                                           .toUtf8();
        writeBytes(state(QStringLiteral("source-publication.json")), publication);
        QFile::setPermissions(state(QStringLiteral("source-publication.json")), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        writeBytes(state(QStringLiteral("source-comment-snapshot.json")),
                   QJsonDocument(QJsonObject{{QStringLiteral("docs_root"), m_paths.docsRoot},
                                             {QStringLiteral("entries"), QJsonObject{{notes, QJsonObject{{QStringLiteral("line"), 1}}}}}})
                       .toJson(QJsonDocument::Compact));
        writeBytes(state(QStringLiteral("source-merge-recoveries/") + sha256Hex(notes.toUtf8()) + QStringLiteral(".json")),
                   QStringLiteral("{\"path\": \"%1\", \"base_sha256\": \"x\"}").arg(notes).toUtf8());
        writeBytes(state(QStringLiteral("source-merge-recoveries/") + sha256Hex(notes.toUtf8()) + QStringLiteral(".local")), "local bytes mention " + notes.toUtf8());
        writeBytes(state(QStringLiteral("source-conflicts/c1/") + notes + QStringLiteral(".local")), "conflicting bytes\n");
        writeBytes(docs(QStringLiteral(".runtime/source-location-index.json")), "{\"schema_version\": 1}");
        writeBytes(QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-manifest.json")),
                   QStringLiteral("{\"schema_version\": 3, \"shared_files\": [{\"path\": \"%1/thoughts-project.json\"}]}").arg(m_legacy).toUtf8());

        // The Library catalogue and identities: relative keys and absolute paths.
        const QString absolutePdf = docs(m_legacy + QStringLiteral("/imported.pdf"));
        writeBytes(docs(QStringLiteral(".thoughts/library-projects.json")),
                   QJsonDocument(QJsonObject{{QStringLiteral("projects"), QJsonArray()},
                                             {QStringLiteral("pending"),
                                              QJsonArray{QJsonObject{{QStringLiteral("action"), QStringLiteral("import")},
                                                                     {QStringLiteral("project_id"), QStringLiteral("p")},
                                                                     {QStringLiteral("local_path"), absolutePdf}}}},
                                             {QStringLiteral("generated_manifests"),
                                              QJsonObject{{m_legacy + QStringLiteral("/thoughts-project.json"), sha256Hex(manifestBytes(m_legacy))},
                                                          {QStringLiteral("docs-x/thoughts-project.json"), QStringLiteral("abc")}}}})
                       .toJson());
        writeBytes(docs(QStringLiteral(".thoughts/document-identities.json")),
                   QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
                                             {QStringLiteral("documents"), QJsonObject{{QStringLiteral("d"), QJsonObject{{QStringLiteral("local_path"), absolutePdf}}}}}})
                       .toJson());
        writeBytes(docs(QStringLiteral("comments/.history/proposal_events.jsonl")), QStringLiteral("{\"source_file\": \"%1\"}\n").arg(notes).toUtf8());

        // desktop-sync.sqlite: pending edit, baseline, an observation of the
        // new path that already exists, and a queued operation.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("fixture"));
            db.setDatabaseName(state(QStringLiteral("desktop-sync.sqlite")));
            QVERIFY(db.open());
            execute(db, QStringLiteral("CREATE TABLE desktop_pending_paths(path TEXT PRIMARY KEY, generation INTEGER NOT NULL, action TEXT NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE desktop_source_baselines(path TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE desktop_observed_paths(path TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE desktop_logical_operations(operation_id TEXT PRIMARY KEY, operation_json TEXT NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE blobs(id INTEGER PRIMARY KEY, data BLOB)"));
            execute(db, QStringLiteral("INSERT INTO desktop_pending_paths VALUES('%1', 4, 'upsert')").arg(notes));
            execute(db, QStringLiteral("INSERT INTO desktop_source_baselines VALUES('%1', 10), ('docs-x/tex/source/a.tex', 2)").arg(notes));
            execute(db, QStringLiteral("INSERT INTO desktop_observed_paths VALUES('%1', 10), ('%2', 10)").arg(notes, m_canonical + QStringLiteral("/notes.tex")));
            execute(db, QStringLiteral("INSERT INTO desktop_logical_operations VALUES('o1', '{\"payload\":{\"source_file\":\"%1\"}}')").arg(notes));
            QSqlQuery blob(db);
            blob.prepare(QStringLiteral("INSERT INTO blobs(data) VALUES(?)"));
            blob.addBindValue(QByteArray(QByteArray("{\"p\":\"") + notes.toUtf8() + "\"}"));
            QVERIFY(blob.exec());
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("fixture"));
        // Repository projections verify every payload's SHA-256.
        {
            Thoughts::RepositoryProjectionStore store(m_paths.stateRoot);
            qint64 revision = 0;
            QVERIFY(store.write(0, {},
                                QJsonObject{{QStringLiteral("review_card\ncards\nk1"), QJsonObject{{QStringLiteral("card_id"), QStringLiteral("k1")}, {QStringLiteral("source_file"), notes}}},
                                            {QStringLiteral("logical_comment\ncomments\nc1"), QJsonObject{{QStringLiteral("comment_id"), QStringLiteral("c1")}}}},
                                &revision));
            QCOMPARE(revision, 1);
            writeBytes(state(QStringLiteral("repository.json")), "{\"projection_store_revision\": 1}");
        }
        // The Library search index: legacy rows are dropped and rebuilt.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("search"));
            db.setDatabaseName(QDir(m_paths.applicationDataRoot).filePath(QStringLiteral("library-search.sqlite")));
            QVERIFY(db.open());
            execute(db, QStringLiteral("CREATE TABLE indexed_sources(path TEXT PRIMARY KEY, identity TEXT NOT NULL)"));
            execute(db, QStringLiteral("INSERT INTO indexed_sources VALUES('%1', 'x'), ('docs-x/tex/source/a.tex', 'y')").arg(notes));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("search"));
    }

    QJsonObject canonicalSnapshot() const
    {
        return QJsonObject{{QStringLiteral("projects"),
                            QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("p")},
                                                   {QStringLiteral("source_manifests"),
                                                    QJsonArray{QJsonObject{{QStringLiteral("path"), QString(m_canonical + QStringLiteral("/thoughts-project.json"))}}}}}}}};
    }
    QJsonObject legacySnapshot() const
    {
        return QJsonObject{{QStringLiteral("projects"),
                            QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("p")},
                                                   {QStringLiteral("source_manifests"),
                                                    QJsonArray{QJsonObject{{QStringLiteral("path"), QString(m_legacy + QStringLiteral("/thoughts-project.json"))}}}}}}}};
    }

    void verifyMigrated()
    {
        const QString notes = m_canonical + QStringLiteral("/notes.tex");
        // The folder moved as a whole: the unsynced edit and the PDF with it.
        QVERIFY(!QFileInfo::exists(docs(m_legacy)));
        QCOMPARE(readBytes(docs(notes)), QByteArray("\\documentclass{article}\\begin{document}edited here, not yet synced\\end{document}\n"));
        QCOMPARE(readBytes(docs(m_canonical + QStringLiteral("/imported.pdf"))), QByteArray("%PDF imported"));
        QVERIFY(QFileInfo::exists(docs(m_canonical + QStringLiteral("/build/notes.aux"))));
        // Step 4: the manifest equals Fedora's Rule-R bytes.
        QCOMPARE(readBytes(docs(m_canonical + QStringLiteral("/thoughts-project.json"))), SourceOwner::rewriteLegacyDefaultFolder(manifestBytes(m_legacy)));
        QCOMPARE(readBytes(docs(m_canonical + QStringLiteral("/thoughts-project.json"))), manifestBytes(m_canonical));
        // Step 3, textually: identical bytes apart from the folder name.
        const QByteArray publication = readBytes(state(QStringLiteral("source-publication.json")));
        QVERIFY(publication.contains(notes.toUtf8()));
        QVERIFY(!SourceOwner::containsLegacyDefaultFolder(publication));
        QVERIFY(publication.startsWith("{\"bases\": {\""));
        QCOMPARE(QFile::permissions(state(QStringLiteral("source-publication.json"))) & (QFileDevice::ReadGroup | QFileDevice::ReadOther), QFileDevice::Permissions());
        for (const QString &path : {state(QStringLiteral("source-comment-snapshot.json")), QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-manifest.json")),
                                    docs(QStringLiteral(".thoughts/library-projects.json")), docs(QStringLiteral(".thoughts/document-identities.json")),
                                    docs(QStringLiteral("comments/.history/proposal_events.jsonl"))}) {
            const QByteArray bytes = readBytes(path);
            QVERIFY2(!bytes.isEmpty() && !SourceOwner::containsLegacyDefaultFolder(bytes), qPrintable(path));
            QVERIFY2(bytes.contains(m_canonical.toUtf8()), qPrintable(path));
        }
        const QJsonObject catalogue = QJsonDocument::fromJson(readBytes(docs(QStringLiteral(".thoughts/library-projects.json")))).object();
        // This computer's own copy stays recorded as its own, at its new bytes.
        QCOMPARE(catalogue.value(QStringLiteral("generated_manifests")).toObject().value(m_canonical + QStringLiteral("/thoughts-project.json")).toString(),
                 sha256Hex(manifestBytes(m_canonical)));
        QCOMPARE(catalogue.value(QStringLiteral("generated_manifests")).toObject().value(QStringLiteral("docs-x/thoughts-project.json")).toString(), QStringLiteral("abc"));
        QCOMPARE(catalogue.value(QStringLiteral("pending")).toArray().at(0).toObject().value(QStringLiteral("local_path")).toString(),
                 docs(m_canonical + QStringLiteral("/imported.pdf")));
        // Files named sha256(path) follow their path; the recovery's source bytes stay as they were.
        const QString recovery = state(QStringLiteral("source-merge-recoveries/") + sha256Hex(notes.toUtf8()));
        QVERIFY(QFileInfo::exists(recovery + QStringLiteral(".json")));
        QCOMPARE(readBytes(recovery + QStringLiteral(".local")), QByteArray("local bytes mention ") + (m_legacy + QStringLiteral("/notes.tex")).toUtf8());
        QVERIFY(!QFileInfo::exists(state(QStringLiteral("source-merge-recoveries/") + sha256Hex((m_legacy + QStringLiteral("/notes.tex")).toUtf8()) + QStringLiteral(".json"))));
        QCOMPARE(readBytes(state(QStringLiteral("source-conflicts/c1/") + notes + QStringLiteral(".local"))), QByteArray("conflicting bytes\n"));
        QVERIFY(!QFileInfo::exists(state(QStringLiteral("source-conflicts/c1/") + m_legacy)));
        QVERIFY(!QFileInfo::exists(docs(QStringLiteral(".runtime/source-location-index.json"))));
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("check"));
            db.setDatabaseName(state(QStringLiteral("desktop-sync.sqlite")));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("SELECT path, generation FROM desktop_pending_paths")) && query.next());
            QCOMPARE(query.value(0).toString(), notes);
            QCOMPARE(query.value(1).toInt(), 4);
            QVERIFY(query.exec(QStringLiteral("SELECT path FROM desktop_source_baselines ORDER BY path")) && query.next());
            QCOMPARE(query.value(0).toString(), notes);
            QVERIFY(query.exec(QStringLiteral("SELECT COUNT(*) FROM desktop_observed_paths")) && query.next());
            QCOMPARE(query.value(0).toInt(), 1);
            QVERIFY(query.exec(QStringLiteral("SELECT operation_json FROM desktop_logical_operations")) && query.next());
            QVERIFY(query.value(0).toString().contains(notes));
            QVERIFY(query.exec(QStringLiteral("SELECT data, typeof(data) FROM blobs")) && query.next());
            QCOMPARE(query.value(1).toString(), QStringLiteral("blob"));
            QVERIFY(query.value(0).toByteArray().contains(notes.toUtf8()));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("check"));
        {
            Thoughts::RepositoryProjectionStore store(m_paths.stateRoot);
            QJsonObject projections;
            QVERIFY(store.read(1, &projections));
            QCOMPARE(projections.value(QStringLiteral("review_card\ncards\nk1")).toObject().value(QStringLiteral("source_file")).toString(), notes);
        }
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("search-check"));
            db.setDatabaseName(QDir(m_paths.applicationDataRoot).filePath(QStringLiteral("library-search.sqlite")));
            QVERIFY(db.open());
            QSqlQuery query(db);
            QVERIFY(query.exec(QStringLiteral("SELECT path FROM indexed_sources")) && query.next());
            QCOMPARE(query.value(0).toString(), QStringLiteral("docs-x/tex/source/a.tex"));
            QVERIFY(!query.next());
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("search-check"));
        DefaultFolderMigration migration(m_paths);
        QCOMPARE(migration.journal().value(QStringLiteral("phase")).toString(), QStringLiteral("done"));
        QCOMPARE(migration.journal().value(QStringLiteral("report")).toObject().value(QStringLiteral("remaining_legacy_matches")).toInt(), 0);
        QVERIFY(!migration.interrupted());
        QVERIFY(!migration.needed(canonicalSnapshot()));
    }

private Q_SLOTS:
    void triggerNeedsFedoraCanonicalAndLocalLegacy()
    {
        buildFixture();
        DefaultFolderMigration migration(m_paths);
        QCOMPARE(migration.legacyFolders(), QStringList{m_hash});
        QVERIFY(migration.legacyStateRemains());
        QVERIFY(!DefaultFolderMigration::fedoraUsesCanonicalSegment(legacySnapshot()));
        QVERIFY(DefaultFolderMigration::fedoraUsesCanonicalSegment(canonicalSnapshot()));
        QVERIFY(!migration.needed(legacySnapshot()));
        QVERIFY(migration.needed(canonicalSnapshot()));
        QVERIFY(!migration.interrupted());
    }

    void migratesEverythingOnce()
    {
        buildFixture();
        DefaultFolderMigration migration(m_paths);
        const auto result = migration.run();
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        QVERIFY(result.migrated);
        verifyMigrated();
        // The PDF name a default-folder document builds to never changes.
        QCOMPARE(SourceOwner::defaultFolderOutput(m_canonical + QStringLiteral("/notes.tex")),
                 SourceOwner::defaultFolderOutput(m_legacy + QStringLiteral("/notes.tex")));
        // The Library reads the renamed Project.
        const auto projects = Thoughts::LibraryProjectStore(m_paths.docsRoot).projects(false);
        QCOMPARE(projects.size(), 1);
        QCOMPARE(Thoughts::ProjectConfiguration::path(m_paths.docsRoot, QStringLiteral("p")), docs(m_canonical + QStringLiteral("/thoughts-project.json")));
        // A repeat is a no-op.
        const QByteArray before = readBytes(state(QStringLiteral("source-publication.json")));
        const auto again = DefaultFolderMigration(m_paths).run();
        QVERIFY(again.success);
        QVERIFY(!again.migrated);
        QCOMPARE(readBytes(state(QStringLiteral("source-publication.json"))), before);
    }

    void resumesAnInterruptedRun()
    {
        buildFixture();
        // A crash after the rename, before any record was rewritten.
        QVERIFY(QDir().rename(docs(m_legacy), docs(m_canonical)));
        writeBytes(state(QStringLiteral("default-folder-migration.json")), "{\"schema_version\": 1, \"phase\": \"rewriting\"}");
        DefaultFolderMigration migration(m_paths);
        QVERIFY(migration.interrupted());
        QVERIFY(migration.legacyFolders().isEmpty());
        QVERIFY(migration.needed(legacySnapshot()));
        const auto result = migration.run();
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        verifyMigrated();
    }

    void bothFoldersMergeOrStop()
    {
        buildFixture();
        // default/ already holds an identical copy and a different file.
        writeBytes(docs(m_canonical + QStringLiteral("/imported.pdf")), "%PDF imported");
        writeBytes(docs(m_canonical + QStringLiteral("/notes.tex")), "different\n");
        DefaultFolderMigration migration(m_paths);
        const auto stopped = migration.run();
        QVERIFY(!stopped.success);
        QVERIFY(stopped.details.contains(QStringLiteral("notes.tex")));
        QVERIFY(migration.interrupted());
        QCOMPARE(migration.journal().value(QStringLiteral("phase")).toString(), QStringLiteral("needs_attention"));
        // Nothing differing was changed; nothing was rewritten.
        QCOMPARE(readBytes(docs(m_canonical + QStringLiteral("/notes.tex"))), QByteArray("different\n"));
        QVERIFY(QFileInfo::exists(docs(m_legacy + QStringLiteral("/notes.tex"))));
        QVERIFY(SourceOwner::containsLegacyDefaultFolder(readBytes(state(QStringLiteral("source-publication.json")))));
        // Once the user resolves it, the rerun completes.
        QVERIFY(QFile::remove(docs(m_canonical + QStringLiteral("/notes.tex"))));
        const auto result = migration.run();
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        verifyMigrated();
    }

    void catalogueInstallsInEitherSpelling()
    {
        buildFixture();
        // Before the migration, Fedora's canonical manifest does not match the
        // legacy folder here: that needs the migration, never a second folder.
        const QByteArray canonicalBytes = manifestBytes(m_canonical);
        const QJsonObject snapshot{
            {QStringLiteral("projects"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("p")},
                                    {QStringLiteral("name"), QStringLiteral("P")},
                                    {QStringLiteral("source_manifests"),
                                     QJsonArray{QJsonObject{{QStringLiteral("path"), QString(m_canonical + QStringLiteral("/thoughts-project.json"))},
                                                            {QStringLiteral("sha256"), sha256Hex(canonicalBytes)},
                                                            {QStringLiteral("content"), QString::fromLatin1(canonicalBytes.toBase64())}}}}}}}};
        Thoughts::LibraryProjectStore store(m_paths.docsRoot);
        QVERIFY(!store.receive(snapshot).isEmpty());
        QVERIFY(!QFileInfo::exists(docs(m_canonical)));
        QVERIFY(DefaultFolderMigration(m_paths).run().success);
        QCOMPARE(store.receive(snapshot), QString());
        QCOMPARE(readBytes(docs(m_canonical + QStringLiteral("/thoughts-project.json"))), canonicalBytes);
        // A Library new to this computer installs Fedora's spelling directly.
        const QString fresh = m_temp.path() + QStringLiteral("/fresh");
        QVERIFY(QDir().mkpath(fresh));
        QCOMPARE(Thoughts::LibraryProjectStore(fresh).receive(snapshot), QString());
        QCOMPARE(readBytes(QDir(fresh).filePath(m_canonical + QStringLiteral("/thoughts-project.json"))), canonicalBytes);
        // New default folders follow the Library's spelling.
        QCOMPARE(Thoughts::DefaultFolderLocation::librarySegment(fresh), SourceOwner::DefaultSegment);
        QCOMPARE(Thoughts::ProjectConfiguration::path(fresh, QStringLiteral("new")),
                 QDir(fresh).filePath(SourceOwner::defaultFolder(SourceOwner::projectHash(QStringLiteral("new")), SourceOwner::DefaultSegment)
                                      + QStringLiteral("/thoughts-project.json")));
    }

    void validationAcceptsBothSpellingsInTheirOwnFolder()
    {
        buildFixture();
        // Legacy, unchanged.
        QString error;
        QVERIFY(!Thoughts::ProjectConfiguration::find(m_paths.docsRoot, m_legacy, nullptr, &error).isEmpty());
        QVERIFY(error.isEmpty());
        // A canonical manifest stored in the legacy folder belongs elsewhere.
        writeBytes(docs(m_legacy + QStringLiteral("/thoughts-project.json")), manifestBytes(m_canonical));
        QVERIFY(Thoughts::ProjectConfiguration::find(m_paths.docsRoot, m_canonical, nullptr, &error).isEmpty());
        QVERIFY(!error.isEmpty());
        // Another Project's hash is invalid in either spelling.
        const QString other = SourceOwner::defaultFolder(SourceOwner::projectHash(QStringLiteral("other")), SourceOwner::DefaultSegment);
        QByteArray foreign = manifestBytes(other);
        writeBytes(docs(m_legacy + QStringLiteral("/thoughts-project.json")), foreign);
        error.clear();
        QVERIFY(Thoughts::ProjectConfiguration::read(docs(m_legacy + QStringLiteral("/thoughts-project.json")), &error).isEmpty());
        QVERIFY(!error.isEmpty());
        // Canonical in its own folder: valid, with canonical projections.
        QVERIFY(QDir().rename(docs(m_legacy), docs(m_canonical)));
        writeBytes(docs(m_canonical + QStringLiteral("/thoughts-project.json")), manifestBytes(m_canonical));
        error.clear();
        const auto value = Thoughts::ProjectConfiguration::find(m_paths.docsRoot, m_canonical, nullptr, &error);
        QVERIFY2(!value.isEmpty(), qPrintable(error));
        const auto projection = Thoughts::ProjectConfiguration::projection(value, m_canonical);
        bool sawPdf = false;
        for (const auto &document : projection.value(QStringLiteral("documents")).toArray()) {
            if (document.toObject().value(QStringLiteral("format")).toString() == QLatin1String("pdf")) {
                QCOMPARE(document.toObject().value(QStringLiteral("source")).toString(), QStringLiteral("../") + m_canonical + QStringLiteral("/imported.pdf"));
                sawPdf = true;
            }
        }
        QVERIFY(sawPdf);
    }

    // Opt-in rehearsal on a COPY of a real computer's Library and state:
    // THOUGHTS_MIGRATION_REHEARSAL=<dir> holding docs/ (the Library) and
    // appdata/ (the application data root, with thoughts-server/ inside).
    // Prints the report and the time the run took.
    void rehearsalOnACopy()
    {
        const QString root = qEnvironmentVariable("THOUGHTS_MIGRATION_REHEARSAL");
        if (root.isEmpty()) {
            QSKIP("Set THOUGHTS_MIGRATION_REHEARSAL to a copied Library and state");
        }
        const DefaultFolderMigration::Paths paths{QDir(root).filePath(QStringLiteral("docs")),
                                                  QDir(root).filePath(QStringLiteral("appdata/thoughts-server")),
                                                  QDir(root).filePath(QStringLiteral("appdata")),
                                                  QDir(root).filePath(QStringLiteral("appdata"))};
        DefaultFolderMigration migration(paths);
        qInfo().noquote() << "legacy folders:" << migration.legacyFolders().size() << "legacy state:" << migration.legacyStateRemains();
        QElapsedTimer timer;
        timer.start();
        const auto result = migration.run();
        qInfo().noquote() << "run took" << timer.elapsed() << "ms";
        qInfo().noquote() << QJsonDocument(result.report).toJson(QJsonDocument::Indented);
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        timer.restart();
        const auto again = DefaultFolderMigration(paths).run();
        qInfo().noquote() << "repeat took" << timer.elapsed() << "ms, migrated:" << again.migrated;
        QVERIFY(again.success);
        QVERIFY(!again.migrated);
        QVERIFY(DefaultFolderMigration(paths).legacyFolders().isEmpty());
        QVERIFY(!DefaultFolderMigration(paths).legacyStateRemains());
    }
};

QTEST_GUILESS_MAIN(DefaultFolderMigrationTest)
#include "defaultfoldermigrationtest.moc"
