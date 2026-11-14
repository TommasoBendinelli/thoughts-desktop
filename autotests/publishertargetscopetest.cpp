/* SPDX-License-Identifier: GPL-2.0-or-later */
// One publication asks, for every PDF it covers, which folder builds it. The
// answer comes from the Project files of the whole Library; inside a
// RegisteredSourceTargetsScope they are read once (tools/publisher/buildinputs.cpp).
#undef QT_NO_CAST_FROM_ASCII
#undef QT_NO_CAST_TO_ASCII
#include "../tools/publisher/buildinputs.h"
#include "../tools/publisher/pyjson.h"
#include "../tools/publisher/sourcepublisher.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

using namespace Thoughts::Publisher;
namespace Py = Thoughts::Py;

namespace
{
bool write(const QString &path, const QByteArray &content)
{
    QFile file(path);
    return QDir().mkpath(QFileInfo(path).absolutePath()) && file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(content) == content.size();
}

// A Project "sam" with one folder and the given LaTeX documents.
bool writeProject(const QString &root, const QString &folder, const QStringList &documents)
{
    const QString projectId = QStringLiteral("new:") + folder;
    const QString hash = QString::fromLatin1(QCryptographicHash::hash(projectId.toUtf8(), QCryptographicHash::Sha256).toHex());
    int next = 10;
    // Synthetic ids; a document id has to be a version 4 UUID.
    const auto id = [&next, &folder] { return QStringLiteral("0190a000-0000-4000-8000-0000%1aa%2").arg(folder.size(), 4, 10, QLatin1Char('0')).arg(next++); };
    const QString defaultId = id();
    const QString folderId = id();
    QJsonArray entries;
    for (const QString &document : documents) {
        if (!write(QDir(root).filePath(folder + QLatin1Char('/') + document), "\\documentclass{article}\\begin{document}x\\end{document}\n")) {
            return false;
        }
        entries.append(QJsonObject {{QStringLiteral("document_id"), id()},
                                    {QStringLiteral("source"), QJsonObject {{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), document}}},
                                    {QStringLiteral("format"), QStringLiteral("latex")},
                                    {QStringLiteral("included"), true}});
    }
    const QJsonObject configuration {
        {QStringLiteral("schema_version"), 5},
        {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
        {QStringLiteral("id"), projectId},
        {QStringLiteral("name"), QStringLiteral("sam")},
        {QStringLiteral("default_folder_id"), defaultId},
        {QStringLiteral("source_folders"),
         QJsonArray {QJsonObject {{QStringLiteral("folder_id"), defaultId}, {QStringLiteral("path"), QStringLiteral(".thoughts/projects/%1/default").arg(hash)}, {QStringLiteral("document_id"), id()}},
                     QJsonObject {{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), folder}, {QStringLiteral("document_id"), id()}}}},
        {QStringLiteral("documents"), entries},
    };
    return write(QDir(root).filePath(QStringLiteral(".thoughts/projects/%1/default/thoughts-project.json").arg(hash)), QJsonDocument(configuration).toJson());
}

QStringList outputs(const QList<SourceTarget> &targets)
{
    QStringList names;
    for (const SourceTarget &target : targets) {
        names.append(target.project + QLatin1Char('/') + target.target);
    }
    return names;
}
}

class PublisherTargetScopeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void registryIsReadOncePerScope()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString root = QDir(directory.path()).canonicalPath();
        QVERIFY(writeProject(root, QStringLiteral("docs-paper"), {QStringLiteral("tex/source/one.tex"), QStringLiteral("tex/source/two.tex"), QStringLiteral("tex/source/three.tex")}));
        QVERIFY(writeProject(root, QStringLiteral("docs-other"), {QStringLiteral("tex/source/other.tex")}));
        const Py::Path docsRoot(root);

        // Without a scope every question reads the Project files again.
        qint64 scans = RegisteredSourceTargetsScope::scans();
        const QList<SourceTarget> unscoped = registeredSourceProjectTargets(docsRoot);
        QCOMPARE(unscoped.size(), 4);
        QList<QPair<QString, Py::Path>> unscopedProjects;
        for (const SourceTarget &target : unscoped) {
            unscopedProjects.append(targetProject(docsRoot, target.target, std::nullopt, true));
        }
        QCOMPARE(RegisteredSourceTargetsScope::scans() - scans, 5);

        // Inside one, the same questions (as a publication of every PDF asks them, twice)
        // read them once and get the same answers.
        scans = RegisteredSourceTargetsScope::scans();
        {
            const RegisteredSourceTargetsScope scope;
            QCOMPARE(outputs(registeredSourceProjectTargets(docsRoot)), outputs(unscoped));
            for (int pass = 0; pass < 2; ++pass) {
                for (qsizetype index = 0; index < unscoped.size(); ++index) {
                    const QPair<QString, Py::Path> project = targetProject(docsRoot, unscoped.at(index).target, std::nullopt, true);
                    QCOMPARE(project.first, unscopedProjects.at(index).first);
                    QCOMPARE(project.second.str(), unscopedProjects.at(index).second.str());
                    QCOMPARE(targetProject(docsRoot, unscoped.at(index).target, unscoped.at(index).project, true).first, unscoped.at(index).project);
                }
            }
            QCOMPARE(RegisteredSourceTargetsScope::scans() - scans, 1);

            // Nested scopes share the reading; another docs root has its own.
            {
                const RegisteredSourceTargetsScope inner;
                QCOMPARE(outputs(registeredSourceProjectTargets(docsRoot)), outputs(unscoped));
            }
            QCOMPARE(outputs(registeredSourceProjectTargets(docsRoot)), outputs(unscoped));
            QCOMPARE(RegisteredSourceTargetsScope::scans() - scans, 1);
        }

        // A Project file that changes (the publication installs the server's version) is
        // read again by the next scope: the publisher ends one scope before publishing and
        // starts another after it.
        QVERIFY(writeProject(root, QStringLiteral("docs-paper"), {QStringLiteral("tex/source/one.tex")}));
        scans = RegisteredSourceTargetsScope::scans();
        {
            const RegisteredSourceTargetsScope scope;
            const QList<SourceTarget> after = registeredSourceProjectTargets(docsRoot);
            QCOMPARE(after.size(), 2);
            QCOMPARE(outputs(registeredSourceProjectTargets(docsRoot)), outputs(after));
            QCOMPARE(RegisteredSourceTargetsScope::scans() - scans, 1);
        }
        // Nothing is kept after the last scope ended.
        QCOMPARE(outputs(registeredSourceProjectTargets(docsRoot)).size(), 2);
        QCOMPARE(RegisteredSourceTargetsScope::scans() - scans, 2);
    }

    // After its publication the publisher reads the server's source tree once and says
    // whether it is the tree read before plus exactly that publication. Only then may the
    // app skip the installer pass that would find "only what was just sent".
    void treeAfterPublicationHoldsOnlyOwnChanges()
    {
        using Py::Json;
        const auto head = [](const QString &path, int revision, const QString &sha, bool deleted = false) {
            Json item = Json::object({{QStringLiteral("path"), Json(path)}, {QStringLiteral("revision"), Json(revision)}, {QStringLiteral("sha256"), Json(sha)}, {QStringLiteral("object_sha256"), Json(sha)}});
            if (deleted) {
                item.insert(QStringLiteral("deleted"), Json(true));
            }
            return item;
        };
        const auto tree = [](qint64 frontier, const QList<Json> &sources) {
            return Json::object({{QStringLiteral("source_frontier"), Json(frontier)}, {QStringLiteral("sources"), Json::array(sources)}});
        };
        const auto mutation = [](const QString &action, const QString &path) {
            return Json::object({{QStringLiteral("action"), Json(action)}, {QStringLiteral("path"), Json(path)}});
        };
        const auto answer = [](const QString &outcome, const QString &path, int revision, const QString &sha) {
            return Json::object({{QStringLiteral("outcome"), Json(outcome)}, {QStringLiteral("path"), Json(path)}, {QStringLiteral("revision"), Json(revision)}, {QStringLiteral("sha256"), Json(sha)}, {QStringLiteral("object_sha256"), Json(sha)}});
        };
        const QString a(64, QLatin1Char('a')), b(64, QLatin1Char('b')), c(64, QLatin1Char('c')), d(64, QLatin1Char('d')), e(64, QLatin1Char('e'));
        const Json before = tree(7, {head(QStringLiteral("docs-paper/one.tex"), 3, a), head(QStringLiteral("docs-paper/two.tex"), 1, b), head(QStringLiteral("docs-paper/old.tex"), 2, c)});
        const QList<Json> mutations {mutation(QStringLiteral("upsert"), QStringLiteral("docs-paper/one.tex")), mutation(QStringLiteral("upsert"), QStringLiteral("docs-paper/new.tex")),
                                     mutation(QStringLiteral("delete"), QStringLiteral("docs-paper/old.tex"))};
        const Json result = Json::object({{QStringLiteral("source_frontier"), Json(12)},
                                          {QStringLiteral("results"),
                                           Json::array({answer(QStringLiteral("applied"), QStringLiteral("docs-paper/one.tex"), 4, d), answer(QStringLiteral("merged"), QStringLiteral("docs-paper/new.tex"), 1, e),
                                                        answer(QStringLiteral("applied"), QStringLiteral("docs-paper/old.tex"), 3, c)})}});
        const QList<Json> own {head(QStringLiteral("docs-paper/new.tex"), 1, e), head(QStringLiteral("docs-paper/old.tex"), 3, c, true), head(QStringLiteral("docs-paper/one.tex"), 4, d),
                               head(QStringLiteral("docs-paper/two.tex"), 1, b)};
        QVERIFY(SourcePublisher::onlyOwnChanges(before, tree(12, own), mutations, result));

        // Another device changed a source in between: the installer still has work.
        QList<Json> foreignChange = own;
        foreignChange[3] = head(QStringLiteral("docs-paper/two.tex"), 2, a);
        QVERIFY(!SourcePublisher::onlyOwnChanges(before, tree(13, foreignChange), mutations, result));
        // ... or added one, or removed one.
        QList<Json> foreignAddition = own;
        foreignAddition.append(head(QStringLiteral("docs-paper/phone.tex"), 1, a));
        QVERIFY(!SourcePublisher::onlyOwnChanges(before, tree(13, foreignAddition), mutations, result));
        QList<Json> foreignDeletion = own;
        foreignDeletion[3] = head(QStringLiteral("docs-paper/two.tex"), 2, b, true);
        QVERIFY(!SourcePublisher::onlyOwnChanges(before, tree(13, foreignDeletion), mutations, result));
        // The published source itself was changed again before the tree was read.
        QList<Json> changedAgain = own;
        changedAgain[2] = head(QStringLiteral("docs-paper/one.tex"), 5, a);
        QVERIFY(!SourcePublisher::onlyOwnChanges(before, tree(13, changedAgain), mutations, result));
        // A deletion that is not in effect.
        QList<Json> notDeleted = own;
        notDeleted[1] = head(QStringLiteral("docs-paper/old.tex"), 3, c);
        QVERIFY(!SourcePublisher::onlyOwnChanges(before, tree(12, notDeleted), mutations, result));
        // A conflict is no accepted change, and an answer must cover every mutation.
        const Json conflicted = Json::object({{QStringLiteral("results"),
                                               Json::array({answer(QStringLiteral("conflict"), QStringLiteral("docs-paper/one.tex"), 4, d), answer(QStringLiteral("merged"), QStringLiteral("docs-paper/new.tex"), 1, e),
                                                            answer(QStringLiteral("applied"), QStringLiteral("docs-paper/old.tex"), 3, c)})}});
        QVERIFY(!SourcePublisher::onlyOwnChanges(before, tree(12, own), mutations, conflicted));
        const Json partial = Json::object({{QStringLiteral("results"), Json::array({answer(QStringLiteral("applied"), QStringLiteral("docs-paper/one.tex"), 4, d)})}});
        QVERIFY(!SourcePublisher::onlyOwnChanges(before, tree(12, own), mutations, partial));
        QVERIFY(!SourcePublisher::onlyOwnChanges(Json::object(), tree(12, own), mutations, result));
    }
};

QTEST_GUILESS_MAIN(PublisherTargetScopeTest)
#include "publishertargetscopetest.moc"
