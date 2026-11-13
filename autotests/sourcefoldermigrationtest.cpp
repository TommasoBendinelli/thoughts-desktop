// SPDX-License-Identifier: GPL-2.0-or-later
// The client migration of the source folder naming change (docs-<old> ->
// docs-<slug>-<hex>) over a fixture Library and state directory holding every
// store that records a folder name.
#include "../part/libraryprojectstore.h"
#include "../part/projectconfiguration.h"
#include "../part/repositoryprojectionstore.h"
#include "../part/sourcefoldermigration.h"
#include "../part/sourcefoldernames.h"
#include "../part/sourceowner.h"
#include "../tools/publisher/pyerror.h"
#include "../tools/publisher/pyio.h"
#include "../tools/publisher/pyjson.h"
#include "../tools/publisher/sourcepublisher.h"

#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using Thoughts::SourceFolderMigration;
using Thoughts::SourceFolderNames;
using Thoughts::SourceFolderRenames;
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

QVariant scalar(const QString &database, const QString &statement)
{
    const QString connection = QUuid::createUuid().toString();
    QVariant result;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connection);
        db.setDatabaseName(database);
        if (db.open()) {
            QSqlQuery query(db);
            if (query.exec(statement) && query.next()) {
                result = query.value(0);
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(connection);
    return result;
}

const QString ImportHex = QStringLiteral("1ed79cc6aaaaaaaaaaaaaaaaaaaaaaaa");
const QString OldTemplate = QStringLiteral("docs-template");
const QString NewTemplate = QStringLiteral("docs-template-c8ee33");
const QString OldImport = QStringLiteral("docs-import-") + ImportHex;
const QString NewImport = QStringLiteral("docs-benchmarks-1ed79c");
}

class SourceFolderMigrationTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_temp;
    SourceFolderMigration::Paths m_paths;
    QString m_default; // .thoughts/projects/<hash>/default

    QString docs(const QString &relative) const
    {
        return QDir(m_paths.docsRoot).filePath(relative);
    }
    QString state(const QString &relative) const
    {
        return QDir(m_paths.stateRoot).filePath(relative);
    }
    static SourceFolderRenames renames()
    {
        return {{QStringLiteral("c8ee3312-0000-5000-8000-000000000001"), OldTemplate, NewTemplate},
                {QStringLiteral("f2f2f2f2-0000-5000-8000-000000000002"), OldImport, NewImport},
                // Fedora's map lists every declared folder; this one was never on this computer.
                {QStringLiteral("a6fe5d00-0000-5000-8000-000000000003"), QStringLiteral("docs-concepts"), QStringLiteral("docs-concepts-a6fe5d")}};
    }
    static QJsonObject snapshot()
    {
        return QJsonObject{{QStringLiteral("projects"), QJsonArray()}, {QStringLiteral("source_folder_renames"), SourceFolderNames::toJson(renames())}};
    }
    QByteArray manifestBytes(const QString &templateFolder, const QString &importFolder) const
    {
        const QJsonObject value{
            {QStringLiteral("schema_version"), 5},
            {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
            {QStringLiteral("id"), QStringLiteral("p")},
            {QStringLiteral("name"), QStringLiteral("P")},
            {QStringLiteral("default_folder_id"), QStringLiteral("d0000000-0000-5000-8000-000000000000")},
            {QStringLiteral("source_folders"),
             QJsonArray{QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("d0000000-0000-5000-8000-000000000000")}, {QStringLiteral("path"), m_default},
                                    {QStringLiteral("document_id"), QStringLiteral("d0000000-0000-5000-8000-0000000000aa")}},
                        QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("c8ee3312-0000-5000-8000-000000000001")}, {QStringLiteral("path"), templateFolder},
                                    {QStringLiteral("document_id"), QStringLiteral("c8ee3312-0000-5000-8000-0000000000aa")}},
                        QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("f2f2f2f2-0000-5000-8000-000000000002")}, {QStringLiteral("path"), importFolder},
                                    {QStringLiteral("document_id"), QStringLiteral("f2f2f2f2-0000-5000-8000-0000000000aa")}}}},
            {QStringLiteral("documents"),
             QJsonArray{QJsonObject{{QStringLiteral("document_id"), QStringLiteral("8b0f5a52-79a4-4c3e-9d2e-0e0f8f6f7a11")},
                                    {QStringLiteral("format"), QStringLiteral("latex")},
                                    {QStringLiteral("included"), true},
                                    {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("c8ee3312-0000-5000-8000-000000000001")},
                                                                           {QStringLiteral("path"), QStringLiteral("tex/source/a.tex")}}}}}},
            // Rule F: Fedora pinned the output before renaming.
            {QStringLiteral("rendering"), QJsonObject{{QStringLiteral("8b0f5a52-79a4-4c3e-9d2e-0e0f8f6f7a11"), QJsonObject{{QStringLiteral("output"), QStringLiteral("template_a.pdf")}}}}},
            {QStringLiteral("document_names"), QJsonObject()}};
        return QJsonDocument(value).toJson();
    }

    void buildFixture()
    {
        QVERIFY(m_temp.isValid());
        const QString base = m_temp.path() + QStringLiteral("/") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        m_paths = {base + QStringLiteral("/docs"), base + QStringLiteral("/app/thoughts-server"), base + QStringLiteral("/app"), base + QStringLiteral("/data")};
        m_default = SourceOwner::defaultFolder(SourceOwner::projectHash(QStringLiteral("p")), SourceOwner::DefaultSegment);
        const QString a = OldTemplate + QStringLiteral("/tex/source/a.tex");
        const QString b = OldImport + QStringLiteral("/markdown/source/b.md");
        writeBytes(docs(m_default + QStringLiteral("/thoughts-project.json")), manifestBytes(OldTemplate, OldImport));
        writeBytes(docs(a), "\\documentclass{article}\\begin{document}edited here, not yet synced; see docs-template/x\\end{document}\n");
        writeBytes(docs(OldTemplate + QStringLiteral("/pdf/template_a.pdf")), "%PDF a");
        writeBytes(docs(b), "# B\n\n[link](../../docs-template/tex/source/a.tex)\n");
        writeBytes(docs(OldImport + QStringLiteral("/.thoughts-import.json")), "{\"operation_id\": \"1ed79cc6-aaaa-aaaa-aaaa-aaaaaaaaaaaa\"}");
        // Not renamed: a longer name and an undeclared folder.
        writeBytes(docs(QStringLiteral("docs-templates/tex/source/c.tex")), "c\n");
        writeBytes(docs(QStringLiteral("docs-models/tex/source/m.tex")), "m\n");

        // The publisher's state (Python json layout): keys, values, a contents
        // field and a multi-line string that keep their bytes.
        const QByteArray publication = QStringLiteral(
                                           "{\"bases\": {\"%1\": {\"revision\": 3, \"sha256\": \"%3\"}, \"%2\": {\"revision\": 1, \"sha256\": \"%3\"},"
                                           " \"docs-templates/tex/source/c.tex\": {\"revision\": 2, \"sha256\": \"%3\"}},"
                                           " \"pending_publication\": {\"mutations\": [{\"path\": \"%1\", \"action\": \"upsert\", \"contents\": \"mentions %1\"}]},"
                                           " \"conflicts\": {\"c1\": {\"path\": \"%1\", \"message\": \"line one %1\\nline two\"}}, \"next_device_sequence\": 7,"
                                           " \"scope\": \"project:docs-template\"}\n")
                                           .arg(a, b, QString(64, QLatin1Char('0')))
                                           .toUtf8();
        writeBytes(state(QStringLiteral("source-publication.json")), publication);
        writeBytes(state(QStringLiteral("source-comment-snapshot.json")),
                   QJsonDocument(QJsonObject{{QStringLiteral("docs_root"), m_paths.docsRoot}, {QStringLiteral("entries"), QJsonObject{{a, QJsonObject{{QStringLiteral("line"), 1}}}}}})
                       .toJson(QJsonDocument::Compact));
        writeBytes(state(QStringLiteral("source-merge-recoveries/") + sha256Hex(a.toUtf8()) + QStringLiteral(".json")),
                   QStringLiteral("{\"path\": \"%1\", \"base_sha256\": \"x\"}").arg(a).toUtf8());
        writeBytes(state(QStringLiteral("source-merge-recoveries/") + sha256Hex(a.toUtf8()) + QStringLiteral(".local")), "local bytes mention " + a.toUtf8());
        writeBytes(state(QStringLiteral("source-conflicts/c1/") + a + QStringLiteral(".local")), "conflicting bytes\n");
        writeBytes(state(QStringLiteral("build-runs/r1/request.json")), "{\"project\": \"docs-template\", \"target\": \"template.pdf\"}");
        writeBytes(docs(QStringLiteral(".runtime/source-location-index.json")), "{\"schema_version\": 1}");
        // An HTML edition lists its PDF's source files (derived; dropped).
        writeBytes(state(QStringLiteral("html-editions/pdf/") + QString(64, QLatin1Char('a')) + QStringLiteral(".json")), "{\"filename\": \"template_a.pdf\"}");
        writeBytes(state(QStringLiteral("html-editions/editions/") + QString(64, QLatin1Char('b')) + QStringLiteral("/manifest.json")),
                   QStringLiteral("{\"source_files\": {\"%1\": \"x\"}}").arg(a).toUtf8());
        writeBytes(QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-manifest.json")),
                   QStringLiteral("{\"schema_version\": 3, \"shared_files\": [{\"path\": \"%1/thoughts-project.json\"}]}").arg(OldTemplate).toUtf8());
        writeBytes(QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-sync-state.json")), QStringLiteral("{\"files\": {\"%1\": {}}}").arg(a).toUtf8());

        // The catalogue: a queued Library change, this computer's own manifest
        // copy, and Fedora's map (which keeps its old names).
        const QString manifestRelative = m_default + QStringLiteral("/thoughts-project.json");
        writeBytes(docs(QStringLiteral(".thoughts/library-projects.json")),
                   QJsonDocument(QJsonObject{{QStringLiteral("projects"), QJsonArray()},
                                             {QStringLiteral("pending"),
                                              QJsonArray{QJsonObject{{QStringLiteral("action"), QStringLiteral("rename_source")},
                                                                     {QStringLiteral("project_id"), QStringLiteral("p")},
                                                                     {QStringLiteral("source_project"), OldTemplate},
                                                                     {QStringLiteral("name"), QStringLiteral("Template notes")}}}},
                                             {QStringLiteral("generated_manifests"), QJsonObject{{manifestRelative, sha256Hex(manifestBytes(OldTemplate, OldImport))}}},
                                             {QStringLiteral("source_folder_renames"), SourceFolderNames::toJson(renames())}})
                       .toJson());
        writeBytes(docs(QStringLiteral(".thoughts/document-identities.json")),
                   QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
                                             {QStringLiteral("documents"),
                                              QJsonObject{{QStringLiteral("d"), QJsonObject{{QStringLiteral("local_path"), docs(OldTemplate + QStringLiteral("/pdf/template_a.pdf"))}}}}}})
                       .toJson());
        writeBytes(docs(QStringLiteral("comments/template_comments.jsonl")), QStringLiteral("{\"source_file\": \"%1\", \"contents\": \"about docs-template\"}\n").arg(a).toUtf8());
        writeBytes(docs(QStringLiteral("library/source-project-policy.json")), "{\"excluded_projects\": [\"docs-template\", \"docs-templates\"]}\n");
        writeBytes(docs(QStringLiteral("current_pdfs/.thoughts-source-index.json")), QStringLiteral("{\"sources\": [\"%1\"]}").arg(b).toUtf8());

        // desktop-sync.sqlite: pending edit, baseline, an observation of the
        // new path that already exists, a queued operation, free text.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("fixture"));
            db.setDatabaseName(state(QStringLiteral("desktop-sync.sqlite")));
            QVERIFY(db.open());
            execute(db, QStringLiteral("CREATE TABLE desktop_pending_paths(path TEXT PRIMARY KEY, generation INTEGER NOT NULL, action TEXT NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE desktop_source_baselines(path TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE desktop_observed_paths(path TEXT PRIMARY KEY, size_bytes INTEGER NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE desktop_logical_operations(operation_id TEXT PRIMARY KEY, operation_json TEXT NOT NULL)"));
            execute(db, QStringLiteral("CREATE TABLE notes(id INTEGER PRIMARY KEY, contents TEXT, body TEXT)"));
            execute(db, QStringLiteral("INSERT INTO desktop_pending_paths VALUES('%1', 4, 'upsert')").arg(a));
            execute(db, QStringLiteral("INSERT INTO desktop_source_baselines VALUES('%1', 10), ('docs-templates/tex/source/c.tex', 2)").arg(a));
            execute(db, QStringLiteral("INSERT INTO desktop_observed_paths VALUES('%1', 10), ('%2', 11)").arg(a, SourceFolderNames::rewrite(a, renames())));
            execute(db, QStringLiteral("INSERT INTO desktop_logical_operations VALUES('o1', '{\"payload\":{\"source_file\":\"%1\",\"scope\":\"project:docs-template\"}}')").arg(a));
            execute(db, QStringLiteral("INSERT INTO notes VALUES(1, 'see docs-template', 'line docs-template\nsecond')"));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("fixture"));
        // review-query.sqlite (card_json source anchors), current annotations.
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("cards"));
            db.setDatabaseName(state(QStringLiteral("review-query.sqlite")));
            QVERIFY(db.open());
            execute(db, QStringLiteral("CREATE TABLE cards(card_id TEXT PRIMARY KEY, card_json TEXT NOT NULL)"));
            execute(db, QStringLiteral("INSERT INTO cards VALUES('k1', '{\"question\":\"Q?\",\"source_anchor\":{\"source_file\":\"%1\"},\"contents\":\"docs-template\"}')").arg(a));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("cards"));
        {
            QVERIFY(QDir().mkpath(state(QStringLiteral(".runtime"))));
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("annotations"));
            db.setDatabaseName(state(QStringLiteral(".runtime/current-annotations.sqlite")));
            QVERIFY(db.open());
            execute(db, QStringLiteral("CREATE TABLE annotations(id TEXT PRIMARY KEY, source_file TEXT, projects TEXT)"));
            execute(db, QStringLiteral("INSERT INTO annotations VALUES('x', '%1', '{\"project:docs-template\": 1}')").arg(a));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("annotations"));
        {
            Thoughts::RepositoryProjectionStore store(m_paths.stateRoot);
            qint64 revision = 0;
            QVERIFY(store.write(0, {},
                                QJsonObject{{QStringLiteral("review_card\ncards\nk1"), QJsonObject{{QStringLiteral("card_id"), QStringLiteral("k1")}, {QStringLiteral("source_file"), a}}}},
                                &revision));
            writeBytes(state(QStringLiteral("repository.json")), "{\"projection_store_revision\": 1}");
        }
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("search"));
            db.setDatabaseName(QDir(m_paths.applicationDataRoot).filePath(QStringLiteral("library-search.sqlite")));
            QVERIFY(db.open());
            execute(db, QStringLiteral("CREATE TABLE indexed_sources(path TEXT PRIMARY KEY, identity TEXT NOT NULL)"));
            execute(db, QStringLiteral("INSERT INTO indexed_sources VALUES('%1', 'x'), ('docs-models/tex/source/m.tex', 'y')").arg(a));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("search"));
    }

    void verifyMigrated()
    {
        const QString a = NewTemplate + QStringLiteral("/tex/source/a.tex");
        const QString oldA = OldTemplate + QStringLiteral("/tex/source/a.tex");
        // Folders moved whole; sources keep their bytes (links are Fedora's rule L).
        QVERIFY(!QFileInfo::exists(docs(OldTemplate)));
        QVERIFY(!QFileInfo::exists(docs(OldImport)));
        QCOMPARE(readBytes(docs(a)), QByteArray("\\documentclass{article}\\begin{document}edited here, not yet synced; see docs-template/x\\end{document}\n"));
        QCOMPARE(readBytes(docs(NewTemplate + QStringLiteral("/pdf/template_a.pdf"))), QByteArray("%PDF a"));
        QCOMPARE(readBytes(docs(NewImport + QStringLiteral("/markdown/source/b.md"))), QByteArray("# B\n\n[link](../../docs-template/tex/source/a.tex)\n"));
        QVERIFY(QFileInfo::exists(docs(QStringLiteral("docs-templates/tex/source/c.tex"))));
        QVERIFY(QFileInfo::exists(docs(QStringLiteral("docs-models/tex/source/m.tex"))));
        // The manifest names the new folders; the pinned output is unchanged.
        const QByteArray manifest = readBytes(docs(m_default + QStringLiteral("/thoughts-project.json")));
        QCOMPARE(manifest, manifestBytes(NewTemplate, NewImport));
        const auto projects = Thoughts::LibraryProjectStore(m_paths.docsRoot).projects(false);
        QCOMPARE(projects.size(), 1);
        // State: rewritten in place, free text kept.
        const QByteArray publication = readBytes(state(QStringLiteral("source-publication.json")));
        QVERIFY(publication.startsWith(QStringLiteral("{\"bases\": {\"%1\": {\"revision\": 3").arg(a).toUtf8()));
        QVERIFY(publication.contains(QStringLiteral("\"contents\": \"mentions %1\"").arg(oldA).toUtf8()));
        QVERIFY(publication.contains(QStringLiteral("\"message\": \"line one %1\\nline two\"").arg(oldA).toUtf8()));
        QVERIFY(publication.contains("\"docs-templates/tex/source/c.tex\""));
        QVERIFY(publication.contains("\"scope\": \"project:docs-template-c8ee33\""));
        for (const QString &path : {state(QStringLiteral("source-comment-snapshot.json")), state(QStringLiteral("build-runs/r1/request.json")),
                                    QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-manifest.json")),
                                    QDir(m_paths.thoughtsDataRoot).filePath(QStringLiteral("source-sync-state.json")), docs(QStringLiteral(".thoughts/document-identities.json")),
                                    docs(QStringLiteral("comments/template_comments.jsonl")), docs(QStringLiteral("library/source-project-policy.json"))}) {
            const QByteArray bytes = readBytes(path);
            QVERIFY2(bytes.contains(NewTemplate.toUtf8()) || bytes.contains(NewImport.toUtf8()), qPrintable(path));
        }
        QVERIFY(readBytes(docs(QStringLiteral("comments/template_comments.jsonl"))).contains("\"contents\": \"about docs-template\""));
        // The published generation's source index is Fedora's: the next generation replaces it.
        QCOMPARE(readBytes(docs(QStringLiteral("current_pdfs/.thoughts-source-index.json"))), QStringLiteral("{\"sources\": [\"%1\"]}").arg(OldImport + QStringLiteral("/markdown/source/b.md")).toUtf8());
        QCOMPARE(readBytes(docs(QStringLiteral("library/source-project-policy.json"))), QByteArray("{\"excluded_projects\": [\"docs-template-c8ee33\", \"docs-templates\"]}\n"));
        const QJsonObject catalogue = QJsonDocument::fromJson(readBytes(docs(QStringLiteral(".thoughts/library-projects.json")))).object();
        QCOMPARE(catalogue.value(QStringLiteral("pending")).toArray().at(0).toObject().value(QStringLiteral("source_project")).toString(), NewTemplate);
        QCOMPARE(SourceFolderNames::fromJson(catalogue.value(QStringLiteral("source_folder_renames")).toArray()), renames());
        QCOMPARE(catalogue.value(QStringLiteral("generated_manifests")).toObject().value(m_default + QStringLiteral("/thoughts-project.json")).toString(),
                 sha256Hex(manifestBytes(NewTemplate, NewImport)));
        // Files named sha256(path) follow their path.
        const QString recovery = state(QStringLiteral("source-merge-recoveries/") + sha256Hex(a.toUtf8()));
        QVERIFY(QFileInfo::exists(recovery + QStringLiteral(".json")));
        QCOMPARE(readBytes(recovery + QStringLiteral(".local")), QByteArray("local bytes mention ") + oldA.toUtf8());
        QVERIFY(!QFileInfo::exists(state(QStringLiteral("source-merge-recoveries/") + sha256Hex(oldA.toUtf8()) + QStringLiteral(".json"))));
        QCOMPARE(readBytes(state(QStringLiteral("source-conflicts/c1/") + a + QStringLiteral(".local"))), QByteArray("conflicting bytes\n"));
        QVERIFY(!QFileInfo::exists(state(QStringLiteral("source-conflicts/c1/") + OldTemplate)));
        QVERIFY(!QFileInfo::exists(docs(QStringLiteral(".runtime/source-location-index.json"))));
        QVERIFY(!QFileInfo::exists(state(QStringLiteral("html-editions"))));
        // SQLite: keys, JSON, the existing row kept on a key collision, free text kept.
        const QString sync = state(QStringLiteral("desktop-sync.sqlite"));
        QCOMPARE(scalar(sync, QStringLiteral("SELECT path FROM desktop_pending_paths")).toString(), a);
        QCOMPARE(scalar(sync, QStringLiteral("SELECT generation FROM desktop_pending_paths")).toInt(), 4);
        QCOMPARE(scalar(sync, QStringLiteral("SELECT COUNT(*) FROM desktop_source_baselines WHERE path = '%1'").arg(a)).toInt(), 1);
        QCOMPARE(scalar(sync, QStringLiteral("SELECT COUNT(*) FROM desktop_observed_paths")).toInt(), 1);
        QCOMPARE(scalar(sync, QStringLiteral("SELECT size_bytes FROM desktop_observed_paths")).toInt(), 11);
        QVERIFY(scalar(sync, QStringLiteral("SELECT operation_json FROM desktop_logical_operations")).toString().contains(QStringLiteral("project:docs-template-c8ee33")));
        QCOMPARE(scalar(sync, QStringLiteral("SELECT contents FROM notes")).toString(), QStringLiteral("see docs-template"));
        QCOMPARE(scalar(sync, QStringLiteral("SELECT body FROM notes")).toString(), QStringLiteral("line docs-template\nsecond"));
        const QString cardJson = scalar(state(QStringLiteral("review-query.sqlite")), QStringLiteral("SELECT card_json FROM cards")).toString();
        QVERIFY(cardJson.contains(a));
        QVERIFY(cardJson.contains(QStringLiteral("\"contents\":\"docs-template\"")));
        QCOMPARE(scalar(state(QStringLiteral(".runtime/current-annotations.sqlite")), QStringLiteral("SELECT projects FROM annotations")).toString(),
                 QStringLiteral("{\"project:docs-template-c8ee33\": 1}"));
        {
            Thoughts::RepositoryProjectionStore store(m_paths.stateRoot);
            QJsonObject projections;
            QVERIFY(store.read(1, &projections));
            QCOMPARE(projections.value(QStringLiteral("review_card\ncards\nk1")).toObject().value(QStringLiteral("source_file")).toString(), a);
        }
        QCOMPARE(scalar(QDir(m_paths.applicationDataRoot).filePath(QStringLiteral("library-search.sqlite")), QStringLiteral("SELECT group_concat(path) FROM indexed_sources")).toString(),
                 QStringLiteral("docs-models/tex/source/m.tex"));
        SourceFolderMigration migration(m_paths);
        QCOMPARE(migration.journal().value(QStringLiteral("phase")).toString(), QStringLiteral("done"));
        QCOMPARE(migration.journal().value(QStringLiteral("report")).toObject().value(QStringLiteral("remaining_old_names")).toInt(), 0);
        QCOMPARE(migration.journalRenames(), renames());
        QVERIFY(!migration.interrupted());
        QVERIFY(!migration.needed(snapshot()));
    }

    // The publisher's view of a state file after its own guard.
    Thoughts::Py::Json publisherState()
    {
        const Thoughts::Py::Path path(state(QStringLiteral("source-publication.json")));
        Thoughts::Py::Json value = Thoughts::Py::loadsText(QString::fromUtf8(readBytes(path.str())));
        Thoughts::Publisher::followSourceFolderRenames(path, value);
        return value;
    }

private Q_SLOTS:
    void detectsFromFedorasMap()
    {
        buildFixture();
        SourceFolderMigration migration(m_paths);
        QCOMPARE(SourceFolderMigration::renamesFrom(snapshot()), renames());
        // docs-concepts never was on this computer.
        QCOMPARE(migration.pending(renames()), renames().mid(0, 2));
        QVERIFY(migration.needed(snapshot()));
        QVERIFY(!migration.needed(QJsonObject{{QStringLiteral("projects"), QJsonArray()}}));
        QVERIFY(!migration.interrupted());
    }

    void migratesEverythingOnce()
    {
        buildFixture();
        SourceFolderMigration migration(m_paths);
        const auto result = migration.run(renames());
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        QVERIFY(result.migrated);
        verifyMigrated();
        // A repeat is a no-op.
        const QByteArray before = readBytes(state(QStringLiteral("source-publication.json")));
        const auto again = SourceFolderMigration(m_paths).run(renames());
        QVERIFY(again.success);
        QVERIFY(!again.migrated);
        QCOMPARE(readBytes(state(QStringLiteral("source-publication.json"))), before);
        // The publisher reads the migrated bases unchanged.
        const auto bases = publisherState().value(QStringLiteral("bases"));
        QVERIFY(bases.contains(NewTemplate + QStringLiteral("/tex/source/a.tex")));
        QVERIFY(!bases.contains(OldTemplate + QStringLiteral("/tex/source/a.tex")));
    }

    // Never a deletion of an old path: a base left under an old folder (a
    // store the migration missed, a straggler writer) follows the folder,
    // and nothing is published while the rename is unfinished.
    void publisherNeverPublishesOldPathDeletions()
    {
        buildFixture();
        QVERIFY(SourceFolderMigration(m_paths).run(renames()).success);
        const QString stale = QStringLiteral("{\"schema_version\": 1, \"next_device_sequence\": 9, \"bases\": {\"docs-template/tex/source/a.tex\": {\"revision\": 3},"
                                             " \"docs-import-%1/markdown/source/b.md\": {\"revision\": 1}, \"docs-template-c8ee33/tex/source/z.tex\": {\"revision\": 5},"
                                             " \"docs-template/tex/source/z.tex\": {\"revision\": 4}}}")
                                  .arg(ImportHex);
        writeBytes(state(QStringLiteral("source-publication.json")), stale.toUtf8());
        const auto bases = publisherState().value(QStringLiteral("bases"));
        QCOMPARE(bases.keys().size(), 3);
        QCOMPARE(bases.value(NewTemplate + QStringLiteral("/tex/source/a.tex")).value(QStringLiteral("revision")).toInt(), 3);
        QCOMPARE(bases.value(NewImport + QStringLiteral("/markdown/source/b.md")).value(QStringLiteral("revision")).toInt(), 1);
        // The migration's own base wins over a stale one.
        QCOMPARE(bases.value(NewTemplate + QStringLiteral("/tex/source/z.tex")).value(QStringLiteral("revision")).toInt(), 5);
        // Unfinished: the publisher refuses.
        QJsonObject journal = SourceFolderMigration(m_paths).journal();
        journal.insert(QStringLiteral("phase"), QStringLiteral("rewriting"));
        writeBytes(state(QString(SourceFolderMigration::JournalName)), QJsonDocument(journal).toJson());
        bool refused = false;
        try {
            publisherState();
        } catch (const Thoughts::Py::Exception &) {
            refused = true;
        }
        QVERIFY(refused);
    }

    void resumesAnInterruptedRun()
    {
        buildFixture();
        // A crash after the first folder moved, before any record was rewritten.
        QVERIFY(QDir().rename(docs(OldTemplate), docs(NewTemplate)));
        writeBytes(state(QString(SourceFolderMigration::JournalName)),
                   QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("phase"), QStringLiteral("moving")},
                                             {QStringLiteral("renames"), SourceFolderNames::toJson(renames())}})
                       .toJson());
        SourceFolderMigration migration(m_paths);
        QVERIFY(migration.interrupted());
        QVERIFY(migration.needed(QJsonObject()));
        // Startup resumes with the journal's map alone.
        const auto result = migration.run(migration.journalRenames());
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        verifyMigrated();
    }

    void stragglersMergeOrStop()
    {
        buildFixture();
        // The new folder already holds an identical copy and a different file.
        writeBytes(docs(NewTemplate + QStringLiteral("/pdf/template_a.pdf")), "%PDF a");
        writeBytes(docs(NewTemplate + QStringLiteral("/tex/source/a.tex")), "different\n");
        SourceFolderMigration migration(m_paths);
        const auto stopped = migration.run(renames());
        QVERIFY(!stopped.success);
        QVERIFY(stopped.details.contains(QStringLiteral("a.tex")));
        QCOMPARE(migration.journal().value(QStringLiteral("phase")).toString(), QStringLiteral("needs_attention"));
        // Nothing was half-moved or rewritten.
        QCOMPARE(readBytes(docs(NewTemplate + QStringLiteral("/tex/source/a.tex"))), QByteArray("different\n"));
        QVERIFY(QFileInfo::exists(docs(OldTemplate + QStringLiteral("/pdf/template_a.pdf"))));
        QVERIFY(readBytes(state(QStringLiteral("source-publication.json"))).contains("\"docs-template/tex/source/a.tex\""));
        // Once resolved, the rerun completes and merges.
        QVERIFY(QFile::remove(docs(NewTemplate + QStringLiteral("/tex/source/a.tex"))));
        const auto result = migration.run(renames());
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        verifyMigrated();
        // An editor that later recreates the old folder: Sync merges it in.
        writeBytes(docs(OldTemplate + QStringLiteral("/tex/source/new.tex")), "written by a stale editor\n");
        QVERIFY(SourceFolderMigration(m_paths).needed(snapshot()));
        QVERIFY(SourceFolderMigration(m_paths).run(renames()).success);
        QVERIFY(!QFileInfo::exists(docs(OldTemplate)));
        QCOMPARE(readBytes(docs(NewTemplate + QStringLiteral("/tex/source/new.tex"))), QByteArray("written by a stale editor\n"));
    }

    // A document Sync asks Fedora's catalogue only for declared folders of the
    // old form that the stored map does not cover, once per stored catalogue.
    void documentSyncAsksFedoraOnlyWhenNeeded()
    {
        using Check = SourceFolderMigration::DocumentSyncCheck;
        buildFixture();
        SourceFolderMigration migration(m_paths);
        QCOMPARE(migration.declaredFolders(), (QStringList{OldImport, OldTemplate}));
        const QString key = SourceFolderMigration::catalogueKey(m_paths.docsRoot);

        // The map is stored: apply it, no fetch.
        QString asked;
        QCOMPARE(migration.documentSyncCheck(renames(), key, &asked), Check::ApplyStored);
        QVERIFY(asked.isEmpty());
        QVERIFY(migration.unresolvedOldFolders(renames()).isEmpty());

        // An unmigrated Library (old names, empty map): one fetch per run and stored catalogue.
        QCOMPARE(migration.unresolvedOldFolders({}), (QStringList{OldImport, OldTemplate}));
        QCOMPARE(migration.documentSyncCheck({}, key, &asked), Check::AskFedora);
        QCOMPARE(asked, key);
        QCOMPARE(migration.documentSyncCheck({}, key, &asked), Check::Nothing);
        QCOMPARE(migration.documentSyncCheck({}, key, &asked), Check::Nothing);
        // A new stored catalogue (a full Sync received one) may be asked about once more.
        QCOMPARE(migration.documentSyncCheck({}, QStringLiteral("another catalogue"), &asked), Check::AskFedora);
        QCOMPARE(migration.documentSyncCheck({}, QStringLiteral("another catalogue"), &asked), Check::Nothing);

        // After the rename only orphans keep old names (docs-models, docs-templates): never a fetch.
        QVERIFY(migration.run(renames()).success);
        QVERIFY(QFileInfo(docs(QStringLiteral("docs-models"))).isDir());
        QCOMPARE(migration.declaredFolders(), (QStringList{NewImport, NewTemplate}));
        QString fresh;
        QCOMPARE(migration.documentSyncCheck(renames(), SourceFolderMigration::catalogueKey(m_paths.docsRoot), &fresh), Check::Nothing);
        QCOMPARE(migration.documentSyncCheck({}, SourceFolderMigration::catalogueKey(m_paths.docsRoot), &fresh), Check::Nothing);
        QVERIFY(fresh.isEmpty());
        // The catalogue key follows the stored map.
        QVERIFY(SourceFolderMigration::catalogueKey(m_paths.docsRoot) != SourceFolderMigration::catalogueKey(m_temp.path() + QStringLiteral("/nowhere")));
    }

    // A legacy Project id "source:docs-x" (its default folder is
    // sha256("source:docs-x")) is an identifier, never a folder path: after
    // the rename its id and default folder are unchanged, its docs-x moved.
    void legacyProjectIdsStay()
    {
        buildFixture();
        const QString id = QStringLiteral("source:docs-legacy");
        const QString defaultFolder = SourceOwner::defaultFolder(SourceOwner::projectHash(id), SourceOwner::DefaultSegment);
        const QJsonObject manifest{
            {QStringLiteral("schema_version"), 5},
            {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), QStringLiteral("docs-legacy")},
            {QStringLiteral("default_folder_id"), QStringLiteral("e0000000-0000-5000-8000-000000000000")},
            {QStringLiteral("source_folders"),
             QJsonArray{QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("e0000000-0000-5000-8000-000000000000")}, {QStringLiteral("path"), defaultFolder},
                                    {QStringLiteral("document_id"), QStringLiteral("e0000000-0000-5000-8000-0000000000aa")}},
                        QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("1e9ac700-0000-5000-8000-000000000004")}, {QStringLiteral("path"), QStringLiteral("docs-legacy")},
                                    {QStringLiteral("document_id"), QStringLiteral("1e9ac700-0000-5000-8000-0000000000aa")}}}},
            {QStringLiteral("documents"), QJsonArray()},
            {QStringLiteral("document_names"), QJsonObject()}};
        writeBytes(docs(defaultFolder + QStringLiteral("/thoughts-project.json")), QJsonDocument(manifest).toJson());
        writeBytes(docs(QStringLiteral("docs-legacy/tex/source/l.tex")), "l\n");
        // Ids recorded elsewhere: a queued change, a review scope, a sqlite row.
        QJsonObject catalogue = QJsonDocument::fromJson(readBytes(docs(QStringLiteral(".thoughts/library-projects.json")))).object();
        QJsonArray pending = catalogue.value(QStringLiteral("pending")).toArray();
        pending.append(QJsonObject{{QStringLiteral("action"), QStringLiteral("rename")}, {QStringLiteral("project_id"), id}, {QStringLiteral("name"), QStringLiteral("Legacy")}});
        catalogue.insert(QStringLiteral("pending"), pending);
        writeBytes(docs(QStringLiteral(".thoughts/library-projects.json")), QJsonDocument(catalogue).toJson());
        writeBytes(docs(QStringLiteral("reviews/scopes.json")), "{\"project_id\": \"source:docs-legacy\", \"source_file\": \"docs-legacy/tex/source/l.tex\"}\n");
        SourceFolderRenames map = renames();
        map.append({QStringLiteral("1e9ac700-0000-5000-8000-000000000004"), QStringLiteral("docs-legacy"), QStringLiteral("docs-legacy-1e9ac7")});

        const auto result = SourceFolderMigration(m_paths).run(map);
        QVERIFY2(result.success, qPrintable(result.error + QLatin1Char(' ') + result.details));
        // The folder moved; the id, its default folder and its hash did not.
        QVERIFY(!QFileInfo::exists(docs(QStringLiteral("docs-legacy"))));
        QVERIFY(QFileInfo::exists(docs(QStringLiteral("docs-legacy-1e9ac7/tex/source/l.tex"))));
        const QJsonObject after = QJsonDocument::fromJson(readBytes(docs(defaultFolder + QStringLiteral("/thoughts-project.json")))).object();
        QCOMPARE(after.value(QStringLiteral("id")).toString(), id);
        QCOMPARE(after.value(QStringLiteral("source_folders")).toArray().at(0).toObject().value(QStringLiteral("path")).toString(), defaultFolder);
        QCOMPARE(after.value(QStringLiteral("source_folders")).toArray().at(1).toObject().value(QStringLiteral("path")).toString(), QStringLiteral("docs-legacy-1e9ac7"));
        // The name is a label; rule P treats a single-line name like any other value.
        QCOMPARE(Thoughts::ProjectConfiguration::path(m_paths.docsRoot, id), docs(defaultFolder + QStringLiteral("/thoughts-project.json")));
        const auto projects = Thoughts::LibraryProjectStore(m_paths.docsRoot).projects(false);
        QVERIFY2(Thoughts::LibraryProjectStore(m_paths.docsRoot).error().isEmpty(), qPrintable(Thoughts::LibraryProjectStore(m_paths.docsRoot).error()));
        QCOMPARE(projects.size(), 2);
        const QByteArray scopes = readBytes(docs(QStringLiteral("reviews/scopes.json")));
        QVERIFY(scopes.contains("\"project_id\": \"source:docs-legacy\""));
        QVERIFY(scopes.contains("\"docs-legacy-1e9ac7/tex/source/l.tex\""));
        const QJsonObject catalogueAfter = QJsonDocument::fromJson(readBytes(docs(QStringLiteral(".thoughts/library-projects.json")))).object();
        QCOMPARE(catalogueAfter.value(QStringLiteral("pending")).toArray().last().toObject().value(QStringLiteral("project_id")).toString(), id);
    }

    // Rule F on the desktop: a renamed folder keeps the PDF short name of its old one.
    void pdfNamesFollowTheOldName()
    {
        buildFixture();
        QCOMPARE(SourceFolderNames::pdfShortName(NewTemplate, Thoughts::LibraryProjectStore::sourceFolderRenames(m_paths.docsRoot)), QStringLiteral("template"));
        QCOMPARE(SourceFolderNames::pdfShortName(NewTemplate, SourceFolderNames::knownRenames(m_paths.docsRoot)), QStringLiteral("template"));
        QCOMPARE(SourceFolderNames::pdfShortName(QStringLiteral("docs-other-123456"), SourceFolderNames::knownRenames(m_paths.docsRoot)), QStringLiteral("other-123456"));
        // A server worktree's own copy of the map.
        const QString server = m_temp.path() + QStringLiteral("/server");
        writeBytes(QDir(server).filePath(QStringLiteral("library/source-folder-renames.json")), QJsonDocument(SourceFolderNames::toJson(renames())).toJson());
        QCOMPARE(SourceFolderNames::pdfShortName(NewImport, SourceFolderNames::knownRenames(server)), QStringLiteral("import-") + ImportHex);
    }
};

QTEST_GUILESS_MAIN(SourceFolderMigrationTest)
#include "sourcefoldermigrationtest.moc"
