// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/projectconfiguration.h"
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrentRun>

using Thoughts::ProjectConfiguration;
class ProjectConfigurationCacheTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void originalSourceFolderHasNoSpecialStatus() {
        QTemporaryDir root;
        auto configuration = ProjectConfiguration::empty(root.path(), QStringLiteral("research"), QStringLiteral("Research"));
        auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
        folders.append(QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("first")}, {QStringLiteral("path"), QStringLiteral("docs-first")}});
        folders.append(QJsonObject{{QStringLiteral("folder_id"), QStringLiteral("second")}, {QStringLiteral("path"), QStringLiteral("docs-second")}});
        configuration.insert(QStringLiteral("source_folders"), folders);
        QJsonObject old;
        old.insert(QStringLiteral("source_project"), QStringLiteral("docs-first"));
        QJsonObject second;
        second.insert(QStringLiteral("project"), QStringLiteral("docs-second"));
        second.insert(QStringLiteral("folder"), QStringLiteral("Second"));
        old.insert(QStringLiteral("source_folders"), QJsonArray{second});
        const auto project = ProjectConfiguration::organize(configuration, old);
        QVERIFY(!project.contains(QStringLiteral("source_project")));
        const auto connected = project.value(QStringLiteral("source_folders")).toArray();
        QCOMPARE(connected.size(), 2);
        QCOMPARE(connected[0].toObject().value(QStringLiteral("project")).toString(), QStringLiteral("docs-first"));
        QCOMPARE(connected[1].toObject().value(QStringLiteral("folder")).toString(), QStringLiteral("Second"));
    }
    void unbuiltDocumentIsNamedByItsFile() {
        // A checked document without a PDF yet must not show as an empty row.
        QTemporaryDir root;
        auto configuration = ProjectConfiguration::empty(root.path(), QStringLiteral("eth"), QStringLiteral("ETH"));
        const auto folder = configuration.value(QStringLiteral("default_folder_id")).toString();
        const auto declare = [&](const QString &id, const QString &path) {
            return QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("format"), QStringLiteral("latex")}, {QStringLiteral("included"), true},
                               {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), path}}}};
        };
        configuration.insert(QStringLiteral("documents"), QJsonArray{declare(QStringLiteral("unnamed"), QStringLiteral("report/final_report_time_series_comparison.tex")),
                                                                     declare(QStringLiteral("named"), QStringLiteral("cv.tex"))});
        configuration.insert(QStringLiteral("document_names"), QJsonObject{{QStringLiteral("named"), QStringLiteral("My CV")}});
        const auto documents = ProjectConfiguration::organize(configuration, QJsonObject()).value(QStringLiteral("documents")).toArray();
        QCOMPARE(documents.size(), 2);
        QCOMPARE(documents[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("final_report_time_series_comparison"));
        QVERIFY(documents[0].toObject().value(QStringLiteral("pending_build")).toBool());
        QCOMPARE(documents[1].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("My CV"));
    }
    void unpublishedDocumentsStayWhenTheServerListsThem() {
        // protocol/project-document-publication.md: an unchecked declaration stays in its
        // Project, unpublished, once the catalogue says the server can publish it again.
        QTemporaryDir root;
        auto configuration = ProjectConfiguration::empty(root.path(), QStringLiteral("eth"), QStringLiteral("ETH"));
        const auto folder = configuration.value(QStringLiteral("default_folder_id")).toString();
        const auto declare = [&](const QString &id, const QString &path, bool included) {
            return QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), included},
                               {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), path}}}};
        };
        configuration.insert(QStringLiteral("documents"), QJsonArray{declare(QStringLiteral("shown"), QStringLiteral("shown.pdf"), true),
                                                                     declare(QStringLiteral("hidden"), QStringLiteral("hidden.pdf"), false)});
        const auto older = ProjectConfiguration::organize(configuration, QJsonObject());
        QCOMPARE(older.value(QStringLiteral("documents")).toArray().size(), 1);
        QVERIFY(!older.contains(QStringLiteral("unpublished_documents")));

        const auto project = ProjectConfiguration::organize(configuration, QJsonObject{
            {QStringLiteral("unpublished_documents"), QJsonArray{QJsonObject{{QStringLiteral("document_id"), QStringLiteral("hidden")},
                {QStringLiteral("name"), QStringLiteral("Hidden paper")}, {QStringLiteral("filename"), QStringLiteral("hidden.pdf")}}}}});
        const auto documents = project.value(QStringLiteral("documents")).toArray();
        QCOMPARE(documents.size(), 1);
        QVERIFY(documents[0].toObject().value(QStringLiteral("published")).toBool());
        const auto unpublished = project.value(QStringLiteral("unpublished_documents")).toArray();
        QCOMPARE(unpublished.size(), 1);
        QCOMPARE(unpublished[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Hidden paper"));
        QVERIFY(!unpublished[0].toObject().value(QStringLiteral("published")).toBool(true));
        QVERIFY(!unpublished[0].toObject().contains(QStringLiteral("pending_build")));
    }
    void unpublishedDocumentIdsLetAnotherProjectPublish() {
        const auto declaration = [](const QString &id, bool included) {
            return QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("included"), included}};
        };
        const QJsonObject first{{QStringLiteral("documents"), QJsonArray{declaration(QStringLiteral("shared"), false), declaration(QStringLiteral("hidden"), false),
                                                                         declaration(QStringLiteral("shown"), true)}}};
        const QJsonObject second{{QStringLiteral("documents"), QJsonArray{declaration(QStringLiteral("shared"), true)}}};
        QCOMPARE(ProjectConfiguration::unpublishedDocumentIds(QList<QJsonObject>{first, second}), QSet<QString>{QStringLiteral("hidden")});
    }
    void reuseAndSelectiveInvalidation() {
        QTemporaryDir root;
        auto a = ProjectConfiguration::empty(root.path(), QStringLiteral("a"), QStringLiteral("Alpha"));
        const auto b = ProjectConfiguration::empty(root.path(), QStringLiteral("b"), QStringLiteral("Beta"));
        const auto pa = ProjectConfiguration::path(root.path(), QStringLiteral("a"));
        const auto pb = ProjectConfiguration::path(root.path(), QStringLiteral("b"));
        QVERIFY(ProjectConfiguration::save(pa, a).isEmpty());
        QVERIFY(ProjectConfiguration::save(pb, b).isEmpty());
        const auto before = ProjectConfiguration::diskReadCount();
        QCOMPARE(ProjectConfiguration::read(pa), a);
        QCOMPARE(ProjectConfiguration::read(pb), b);
        for (int i = 0; i < 10; ++i) {
            QCOMPARE(ProjectConfiguration::read(pa), a);
            QCOMPARE(ProjectConfiguration::read(pb), b);
        }
        QCOMPARE(ProjectConfiguration::diskReadCount() - before, quint64(2));
        const auto oldIdentity = ProjectConfiguration::snapshotIdentity(root.path());
        a.insert(QStringLiteral("name"), QStringLiteral("Gamma"));
        QVERIFY(ProjectConfiguration::save(pa, a).isEmpty()); // Atomic Sync/local replacement.
        QVERIFY(ProjectConfiguration::snapshotIdentity(root.path()) != oldIdentity);
        QCOMPARE(ProjectConfiguration::read(pa), a);
        QCOMPARE(ProjectConfiguration::read(pb), b);
        QCOMPARE(ProjectConfiguration::diskReadCount() - before, quint64(3));

        // A same-size in-place edit remains visible even with a preserved mtime.
        const auto modified = QFileInfo(pa).lastModified();
        QFile file(pa); QVERIFY(file.open(QIODevice::ReadWrite));
        auto bytes = file.readAll(); QVERIFY(bytes.contains("Gamma"));
        bytes.replace("Gamma", "Delta"); QVERIFY(file.seek(0));
        QCOMPARE(file.write(bytes), bytes.size()); QVERIFY(file.flush());
        QVERIFY(file.setFileTime(modified, QFileDevice::FileModificationTime)); file.close();
        QCOMPARE(ProjectConfiguration::read(pa).value(QStringLiteral("name")).toString(), QStringLiteral("Delta"));
        QCOMPARE(ProjectConfiguration::diskReadCount() - before, quint64(4));
    }
    void concurrentReadersShareOneLoad() {
        QTemporaryDir root;
        const auto value = ProjectConfiguration::empty(root.path(), QStringLiteral("parallel"), QStringLiteral("Parallel"));
        const auto path = ProjectConfiguration::path(root.path(), QStringLiteral("parallel"));
        QVERIFY(ProjectConfiguration::save(path, value).isEmpty());
        const auto before = ProjectConfiguration::diskReadCount();
        QList<QFuture<QJsonObject>> jobs;
        for (int i = 0; i < 12; ++i) jobs.append(QtConcurrent::run([path] { return ProjectConfiguration::read(path); }));
        for (auto &job : jobs) QCOMPARE(job.result(), value);
        QCOMPARE(ProjectConfiguration::diskReadCount() - before, quint64(1));
    }
    void invalidDeletedAndSymlinkSettingsDoNotReuseOldValues() {
        QTemporaryDir root;
        const auto value = ProjectConfiguration::empty(root.path(), QStringLiteral("safe"), QStringLiteral("Safe"));
        const auto path = ProjectConfiguration::path(root.path(), QStringLiteral("safe"));
        QVERIFY(ProjectConfiguration::save(path, value).isEmpty());
        QCOMPARE(ProjectConfiguration::read(path), value);
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("broken JSON"); file.close();
        QString error;
        QVERIFY(ProjectConfiguration::read(path, &error).isEmpty()); QVERIFY(!error.isEmpty());
        const auto reads = ProjectConfiguration::diskReadCount();
        error.clear(); QVERIFY(ProjectConfiguration::read(path, &error).isEmpty()); QVERIFY(!error.isEmpty());
        QCOMPARE(ProjectConfiguration::diskReadCount(), reads);
        QVERIFY(ProjectConfiguration::save(path, value).isEmpty());
        QCOMPARE(ProjectConfiguration::read(path), value);
        const auto identity = ProjectConfiguration::snapshotIdentity(root.path());
        QVERIFY(QFile::remove(path)); QVERIFY(ProjectConfiguration::read(path).isEmpty());
        QVERIFY(identity != ProjectConfiguration::snapshotIdentity(root.path()));
        const auto target = root.filePath(QStringLiteral("elsewhere.json"));
        QVERIFY(ProjectConfiguration::save(target, value).isEmpty());
        QVERIFY(QFile::link(target, path));
        error.clear(); QVERIFY(ProjectConfiguration::read(path, &error).isEmpty()); QVERIFY(!error.isEmpty());
    }
    void savingAnUnchangedValueKeepsFedoraBytes() {
        // Sync compares bytes; rewriting the same Project in Qt's layout would look like an edit.
        QTemporaryDir root;
        const auto value = ProjectConfiguration::empty(root.path(), QStringLiteral("pdf:reviews.pdf"), QStringLiteral("reviews"));
        const auto path = ProjectConfiguration::path(root.path(), QStringLiteral("pdf:reviews.pdf"));
        const QByteArray fedora = QJsonDocument(value).toJson(QJsonDocument::Compact) + "\n";
        QVERIFY(ProjectConfiguration::saveBytes(path, fedora).isEmpty());
        QVERIFY(ProjectConfiguration::save(path, value).isEmpty());
        QFile unchanged(path); QVERIFY(unchanged.open(QIODevice::ReadOnly)); QCOMPARE(unchanged.readAll(), fedora); unchanged.close();
        auto renamed = value; renamed.insert(QStringLiteral("name"), QStringLiteral("peer reviews"));
        QVERIFY(ProjectConfiguration::save(path, renamed).isEmpty());
        QCOMPARE(ProjectConfiguration::read(path).value(QStringLiteral("name")).toString(), QStringLiteral("peer reviews"));
    }
};
QTEST_GUILESS_MAIN(ProjectConfigurationCacheTest)
#include "projectconfigurationcachetest.moc"
