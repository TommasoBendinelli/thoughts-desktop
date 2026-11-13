// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/markdownfolderimportdialog.h"
#include <QBuffer>
#include "../part/libraryprojectstore.h"
#include "../part/thoughtignore.h"
#include "../part/librarysourceinventory.h"
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <QTimer>
#include <QApplication>
#include <QProgressDialog>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslKey>
#include <QSslServer>
#include <QSslSocket>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QElapsedTimer>
#include <QMessageAuthenticationCode>
#include <QtEndian>
#include <git2.h>
#include <thread>

namespace {
// Fixture repositories are written with libgit2 itself: the tests need no git program.
struct LibGit2 {
    LibGit2() { git_libgit2_init(); }
    ~LibGit2() { git_libgit2_shutdown(); }
};

bool writeTree(git_repository *repository, const QMap<QString, QByteArray> &files, git_oid *tree)
{
    git_treebuilder *builder = nullptr;
    if (git_treebuilder_new(&builder, repository, nullptr) != 0) return false;
    QMap<QString, QMap<QString, QByteArray>> folders;
    bool ok = true;
    for (auto it = files.cbegin(); ok && it != files.cend(); ++it) {
        const auto slash = it.key().indexOf(QLatin1Char('/'));
        if (slash >= 0) { folders[it.key().left(slash)].insert(it.key().mid(slash + 1), it.value()); continue; }
        git_oid blob;
        ok = git_blob_create_from_buffer(&blob, repository, it.value().constData(), size_t(it.value().size())) == 0
            && git_treebuilder_insert(nullptr, builder, it.key().toUtf8().constData(), &blob, GIT_FILEMODE_BLOB) == 0;
    }
    for (auto it = folders.cbegin(); ok && it != folders.cend(); ++it) {
        git_oid folder;
        ok = writeTree(repository, it.value(), &folder)
            && git_treebuilder_insert(nullptr, builder, it.key().toUtf8().constData(), &folder, GIT_FILEMODE_TREE) == 0;
    }
    ok = ok && git_treebuilder_write(tree, builder) == 0;
    git_treebuilder_free(builder);
    return ok;
}

// Commits files to ref, on top of parentRef (default: ref itself, if it exists).
bool commitFiles(git_repository *repository, const char *ref, const QMap<QString, QByteArray> &files, const char *message, const char *parentRef = nullptr)
{
    git_oid treeId, parentId, commitId;
    git_tree *tree = nullptr;
    git_signature *signature = nullptr;
    git_commit *parent = nullptr;
    bool ok = writeTree(repository, files, &treeId) && git_tree_lookup(&tree, repository, &treeId) == 0
        && git_signature_new(&signature, "Test", "test@example.invalid", 1700000000, 0) == 0;
    if (ok && git_reference_name_to_id(&parentId, repository, parentRef ? parentRef : ref) == 0) ok = git_commit_lookup(&parent, repository, &parentId) == 0;
    if (ok) ok = (parent ? git_commit_create_v(&commitId, repository, ref, signature, signature, nullptr, message, tree, 1, parent)
                         : git_commit_create_v(&commitId, repository, ref, signature, signature, nullptr, message, tree, 0)) == 0;
    git_commit_free(parent);
    git_signature_free(signature);
    git_tree_free(tree);
    return ok;
}

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QString originUrl(const QString &checkout)
{
    git_repository *repository = nullptr;
    git_remote *remote = nullptr;
    QString url;
    if (git_repository_open(&repository, QFile::encodeName(checkout).constData()) == 0 && git_remote_lookup(&remote, repository, "origin") == 0)
        url = QString::fromUtf8(git_remote_url(remote));
    git_remote_free(remote);
    git_repository_free(repository);
    return url;
}

class RecordingPrompts : public Thoughts::GitClonePrompts
{
public:
    bool httpsCredentials(const QString &url, QString *username, QString *password) override
    {
        ++signIns; signInUrl = url; signInName = *username;
        if (!answerSignIn) return false;
        *username = QStringLiteral("reader"); *password = QStringLiteral("token-123");
        return true;
    }
    bool keyPassphrase(const QString &, bool, QString *) override { ++passphrases; return false; }
    bool trustUnknownHost(const QString &, const QString &, const QString &, bool) override { ++hostQuestions; return false; }
    std::atomic_int signIns{0}, passphrases{0}, hostQuestions{0};
    bool answerSignIn = true;
    QString signInUrl, signInName;
};

Thoughts::GitCloneOutcome cloneOnThread(const Thoughts::GitCloneRequest &request, Thoughts::GitClonePrompts *prompts)
{
    // The fixture servers answer from this thread's event loop.
    Thoughts::GitCloneOutcome outcome;
    std::atomic_bool done{false};
    std::thread worker([&] { outcome = Thoughts::gitClone(request, prompts, nullptr); done = true; });
    QElapsedTimer timer; timer.start();
    while (!done && timer.elapsed() < 60000) QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    worker.join();
    return outcome;
}

// Self-signed for 127.0.0.1 and localhost, valid until 2126.
const char testCertificate[] = R"(-----BEGIN CERTIFICATE-----
MIIBsTCCAVegAwIBAgIUXBR3nMX7WCU041RfIKZtvzxiuX4wCgYIKoZIzj0EAwIw
HzEdMBsGA1UEAwwUVGhvdWdodHMgdGVzdCBzZXJ2ZXIwIBcNMjYxMDIyMTQwNjE4
WhgPMjEyNjA5MjgxNDA2MThaMB8xHTAbBgNVBAMMFFRob3VnaHRzIHRlc3Qgc2Vy
dmVyMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEoOm9sPZR1id4nz6p3hRTC4Ro
+0NFaHdHjtbYWPtjyahnSPX1oohVgZ+Tg8nzSEirfnzjt8zuXx/QDC+l1xbt5aNv
MG0wHQYDVR0OBBYEFPj2gX6kekObEjcEDBNQ4AkJq9Y4MB8GA1UdIwQYMBaAFPj2
gX6kekObEjcEDBNQ4AkJq9Y4MA8GA1UdEwEB/wQFMAMBAf8wGgYDVR0RBBMwEYcE
fwAAAYIJbG9jYWxob3N0MAoGCCqGSM49BAMCA0gAMEUCIQD+M3m8Vh+WBXEGDcVI
vyBU5XQmmCSsyGlWUMdOtuWtTAIgN5QijzTPx4TGtN6Op2EvLDI/rAA9L8g0247b
9M7rZ+A=
-----END CERTIFICATE-----
)";
const char testPrivateKey[] = R"(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgo+4/SXApEJ0DrQNa
h/BxqprcnK94IO5Neoo12MHXdFShRANCAASg6b2w9lHWJ3ifPqneFFMLhGj7Q0Vo
d0eO1thY+2PJqGdI9fWiiFWBn5ODyfNISKt+fOO3zO5fH9AML6XXFu3l
-----END PRIVATE KEY-----
)";

// Answers every HTTPS request with 401 and remembers the Authorization headers.
struct SignInServer {
    QSslServer server;
    QList<QByteArray> authorizations;
    int requests = 0;
    // As Overleaf: a refused sign-in gets a 401 that offers no sign-in again.
    bool silentRefusal = false;
    bool start()
    {
        QSslConfiguration configuration = QSslConfiguration::defaultConfiguration();
        configuration.setLocalCertificate(QSslCertificate(QByteArray(testCertificate)));
        configuration.setPrivateKey(QSslKey(QByteArray(testPrivateKey), QSsl::Ec));
        server.setSslConfiguration(configuration);
        QObject::connect(&server, &QSslServer::pendingConnectionAvailable, &server, [this] {
            while (auto *socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    socket->setProperty("request", QByteArray(socket->property("request").toByteArray() + socket->readAll()));
                    const QByteArray request = socket->property("request").toByteArray();
                    const auto end = request.indexOf("\r\n\r\n");
                    if (end < 0) return;
                    socket->setProperty("request", request.mid(end + 4));
                    ++requests;
                    bool signedIn = false;
                    for (const auto &line : request.left(end).split('\n'))
                        if (line.toLower().startsWith("authorization:")) {
                            authorizations.append(line.mid(14).trimmed());
                            signedIn = true;
                        }
                    socket->write(silentRefusal && signedIn ? "HTTP/1.1 401 Unauthorized\r\nContent-Length: 0\r\n\r\n"
                                                            : "HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"test\"\r\nContent-Length: 0\r\n\r\n");
                });
                QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
        return server.listen(QHostAddress::LocalHost);
    }
};
}

class MarkdownFolderImportTest : public QObject {
    Q_OBJECT
    // Library projects also list their default source folder; find a connected one.
    static QJsonObject sourceLocation(const QJsonObject &project, const QString &owner) {
        for (const auto &value : project.value(QStringLiteral("sources")).toArray())
            if (value.toObject().value(QStringLiteral("id")).toString() == owner) return value.toObject();
        return {};
    }
private Q_SLOTS:
    void markdownCandidatesIncludeRootAndNestedFilesRespectingIgnore() {
        QTemporaryDir root;
        const auto owner = QStringLiteral("docs-review");
        const auto project = root.filePath(owner);
        const auto put = [&](const QString &path, const QByteArray &data) {
            const auto target = QDir(project).filePath(path);
            QDir().mkpath(QFileInfo(target).path());
            QFile file(target);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        QVERIFY(put(QStringLiteral("reviews.md"), "# Reviews\n"));
        QVERIFY(put(QStringLiteral("nested/reviews.markdown"), "# Nested\n"));
        QVERIFY(put(QStringLiteral("markdown/source/reviews.md"), "# Existing\n"));
        QVERIFY(put(QStringLiteral("ignored/hidden.md"), "# Hidden\n"));
        QVERIFY(put(QStringLiteral(".ThoughtIgnore"), ".git/\nignored/\n"));
#ifndef Q_OS_WIN
        // On Windows QFile::link makes a .lnk shortcut, an ordinary file, not a symlink.
        QVERIFY(QFile::link(QDir(project).filePath(QStringLiteral("reviews.md")), QDir(project).filePath(QStringLiteral("linked.md"))));
#endif
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject manifest{{QStringLiteral("schema_version"), 4}, {QStringLiteral("document_id"), id}, {QStringLiteral("documents"), QJsonArray{}}};
        const auto projectId = QStringLiteral("new:markdown-paths");
        auto configuration = Thoughts::ProjectConfiguration::empty(root.path(), projectId, QStringLiteral("Markdown paths"));
        auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
        folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("path"), owner}, {QStringLiteral("document_id"), id}});
        configuration.insert(QStringLiteral("source_folders"), folders);
        QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), projectId), configuration).isEmpty());
        QString error;
        const auto entries = Thoughts::DocumentBuildSelection::candidates(root.path(), owner, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(entries.size(), 3);
        QSet<QString> sources, outputs;
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            sources.insert(entry.value(QStringLiteral("source")).toString());
            outputs.insert(entry.value(QStringLiteral("output")).toString());
        }
        QCOMPARE(outputs.size(), 3);
        QVERIFY(sources.contains(QStringLiteral("reviews.md")));
        QVERIFY(sources.contains(QStringLiteral("nested/reviews.markdown")));
        auto saved = entries;
        for (qsizetype i = 0; i < saved.size(); ++i) {
            auto entry = saved[i].toObject();
            entry.insert(QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            entry.insert(QStringLiteral("included"), true);
            saved[i] = entry;
        }
        auto configured = manifest;
        configured.insert(QStringLiteral("documents"), saved);
        const auto storage = Thoughts::DocumentBuildSelection::uniformStorage(configured);
        QVERIFY(!Thoughts::DocumentBuildSelection::readUniform(storage, owner, &error).isEmpty());
        auto bad = storage;
        auto documents = bad.value(QStringLiteral("documents")).toArray();
        auto document = documents[0].toObject();
        document.insert(QStringLiteral("source"), QStringLiteral("../outside.md"));
        documents[0] = document;
        bad.insert(QStringLiteral("documents"), documents);
        QVERIFY(Thoughts::DocumentBuildSelection::readUniform(bad, owner, &error).isEmpty());
    }
    // A Project's default folder offers its Markdown and standalone LaTeX
    // files, named by spec Rules 2 and 3; never its configuration or PDFs.
    void defaultFolderCandidatesFollowTheSharedRules() {
        QTemporaryDir root;
        const auto projectId = QStringLiteral("new:default-documents");
        const auto configuration = Thoughts::ProjectConfiguration::empty(root.path(), projectId, QStringLiteral("Default documents"));
        QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), projectId), configuration).isEmpty());
        const auto owner = Thoughts::DocumentBuildSelection::defaultFolder(configuration);
        QVERIFY(Thoughts::SourceOwner::isDefaultFolder(owner));
        const auto put = [&](const QString &path, const QByteArray &data) {
            const auto target = QDir(root.filePath(owner)).filePath(path);
            QDir().mkpath(QFileInfo(target).path());
            QFile file(target);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        const QByteArray standalone = "\\documentclass{article}\n\\begin{document}Paper\\end{document}\n";
        QVERIFY(put(QStringLiteral("notes.md"), "# Notes\n"));
        QVERIFY(put(QStringLiteral("paper.tex"), standalone));
        QVERIFY(put(QStringLiteral("chapters/long.markdown"), "# Long\n"));
        QVERIFY(put(QStringLiteral("chapters/book.tex"), standalone));
        QVERIFY(put(QStringLiteral("tex/source/report.tex"), standalone));
        QVERIFY(put(QStringLiteral("chapters/fragment.tex"), "Only a fragment\n"));
        QVERIFY(put(QStringLiteral("commented.tex"), "% \\documentclass{article}\n% \\begin{document}\n"));
        QVERIFY(put(QStringLiteral(".drafts/hidden.md"), "# Hidden\n"));
        QVERIFY(put(QStringLiteral(".hidden.md"), "# Hidden\n"));
        QVERIFY(put(QStringLiteral("build/leftover.md"), "# Build\n"));
        QVERIFY(put(QStringLiteral("pdf/leftover.md"), "# Build\n"));
        QVERIFY(put(QStringLiteral("imported.pdf"), "%PDF-1.4\n"));
        QString error;
        const auto entries = Thoughts::DocumentBuildSelection::candidates(root.path(), owner, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QHash<QString, QJsonObject> bySource;
        for (const auto &value : entries) bySource.insert(value.toObject().value(QStringLiteral("source")).toString(), value.toObject());
        QCOMPARE(QSet<QString>(bySource.keyBegin(), bySource.keyEnd()), (QSet<QString>{QStringLiteral("notes.md"), QStringLiteral("paper.tex"),
            QStringLiteral("chapters/long.markdown"), QStringLiteral("chapters/book.tex"), QStringLiteral("tex/source/report.tex")}));
        for (auto it = bySource.cbegin(); it != bySource.cend(); ++it) {
            QCOMPARE(it->value(QStringLiteral("id")).toString(), owner + QLatin1Char('/') + it.key());
            QCOMPARE(it->value(QStringLiteral("owner")).toString(), owner);
            QCOMPARE(it->value(QStringLiteral("output")).toString(), Thoughts::SourceOwner::defaultFolderOutput(owner + QLatin1Char('/') + it.key()));
        }
        QCOMPARE(bySource.value(QStringLiteral("notes.md")).value(QStringLiteral("format")).toString(), QStringLiteral("markdown"));
        QCOMPARE(bySource.value(QStringLiteral("paper.tex")).value(QStringLiteral("format")).toString(), QStringLiteral("latex"));
        // Rule 3: a LaTeX document compiles from its own directory.
        QVERIFY(!bySource.value(QStringLiteral("paper.tex")).contains(QStringLiteral("working_directory")));
        QCOMPARE(bySource.value(QStringLiteral("chapters/book.tex")).value(QStringLiteral("working_directory")).toString(), QStringLiteral("chapters"));
        QCOMPARE(bySource.value(QStringLiteral("tex/source/report.tex")).value(QStringLiteral("working_directory")).toString(), QStringLiteral("tex/source"));

        // The folder's own .ThoughtIgnore replaces the built-in rules.
        QVERIFY(put(QStringLiteral(".ThoughtIgnore"), "chapters/\n"));
        QSet<QString> sources;
        for (const auto &value : Thoughts::DocumentBuildSelection::candidates(root.path(), owner, &error)) sources.insert(value.toObject().value(QStringLiteral("source")).toString());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!sources.contains(QStringLiteral("chapters/book.tex")));
        QVERIFY(sources.contains(QStringLiteral("build/leftover.md")) && sources.contains(QStringLiteral("pdf/leftover.md")));
    }
    void defaultFoldersOfTwoProjectsStayIsolated() {
        QTemporaryDir root;
        QStringList owners, outputs;
        for (const auto &id : {QStringLiteral("new:first"), QStringLiteral("new:second")}) {
            auto configuration = Thoughts::ProjectConfiguration::empty(root.path(), id, id);
            const auto owner = Thoughts::DocumentBuildSelection::defaultFolder(configuration);
            QDir().mkpath(root.filePath(owner));
            QFile notes(QDir(root.filePath(owner)).filePath(QStringLiteral("notes.md")));
            QVERIFY(notes.open(QIODevice::WriteOnly) && notes.write("# Notes\n") > 0);
            notes.close();
            QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), id), configuration).isEmpty());
            QString error;
            const auto entries = Thoughts::DocumentBuildSelection::candidates(root.path(), owner, &error);
            QVERIFY2(error.isEmpty(), qPrintable(error));
            QCOMPARE(entries.size(), 1);
            const auto entry = entries.first().toObject();
            // Declare it as Manage documents does, then organize the Project.
            const auto documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            configuration.insert(QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("document_id"), documentId},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), configuration.value(QStringLiteral("default_folder_id"))}, {QStringLiteral("path"), QStringLiteral("notes.md")}}},
                {QStringLiteral("format"), QStringLiteral("markdown")}, {QStringLiteral("included"), true}}});
            configuration.insert(QStringLiteral("rendering"), QJsonObject{{documentId, QJsonObject{{QStringLiteral("output"), entry.value(QStringLiteral("output"))}}}});
            QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), id), configuration).isEmpty());
            const auto project = Thoughts::ProjectConfiguration::organize(Thoughts::ProjectConfiguration::read(Thoughts::ProjectConfiguration::path(root.path(), id)), QJsonObject{{QStringLiteral("id"), id}});
            const auto document = project.value(QStringLiteral("documents")).toArray().first().toObject();
            QCOMPARE(document.value(QStringLiteral("source_project")).toString(), owner);
            QCOMPARE(document.value(QStringLiteral("source_file")).toString(), owner + QStringLiteral("/notes.md"));
            QStringList files;
            for (const auto &file : project.value(QStringLiteral("sources")).toArray().first().toObject().value(QStringLiteral("files")).toArray())
                files.append(file.toObject().value(QStringLiteral("id")).toString());
            QVERIFY(files.contains(owner + QStringLiteral("/notes.md")));
            QVERIFY(Thoughts::DocumentBuildSelection::owners(QJsonObject{{QStringLiteral("project_configuration"), configuration}}).contains(owner));
            // The declared document is offered once, with its configured output.
            const auto declared = Thoughts::DocumentBuildSelection::candidates(root.path(), owner, &error);
            QCOMPARE(declared.size(), 1);
            QCOMPARE(declared.first().toObject().value(QStringLiteral("document_id")).toString(), documentId);
            QCOMPARE(declared.first().toObject().value(QStringLiteral("output")).toString(), entry.value(QStringLiteral("output")).toString());
            owners.append(owner);
            outputs.append(entry.value(QStringLiteral("output")).toString());
        }
        QVERIFY(owners.first() != owners.last());
        QVERIFY(outputs.first() != outputs.last());
    }
    void latexEntryPointsAnywhereAreCandidates() {
        QTemporaryDir root;
        const auto owner = QStringLiteral("docs-paper");
        const auto project = root.filePath(owner);
        const auto put = [&](const QString &path, const QByteArray &data) {
            const auto target = QDir(project).filePath(path);
            QDir().mkpath(QFileInfo(target).path());
            QFile file(target);
            return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
        };
        const QByteArray entry = "\\documentclass{article}\n\\begin{document}\n\\input{sections/a}\n\\end{document}\n";
        QVERIFY(put(QStringLiteral("paper/main.tex"), entry));
        QVERIFY(put(QStringLiteral("paper/sections/a.tex"), "A\n"));
        QVERIFY(put(QStringLiteral("main.tex"), entry));
        QVERIFY(put(QStringLiteral("notes.tex"), "% \\documentclass{article}\n\\begin{document}\n"));
        QVERIFY(put(QStringLiteral("build/old.tex"), entry));
        QVERIFY(put(QStringLiteral(".hidden/h.tex"), entry));
        QVERIFY(put(QStringLiteral("ignored/z.tex"), entry));
        QVERIFY(put(QStringLiteral("tex/source/old.tex"), "Legacy fragment.\n"));
        QVERIFY(put(QStringLiteral("tex/source/sub/nested.tex"), entry));
        QVERIFY(put(QStringLiteral(".ThoughtIgnore"), "ignored/\n"));
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto projectId = QStringLiteral("new:latex-paths");
        auto configuration = Thoughts::ProjectConfiguration::empty(root.path(), projectId, QStringLiteral("LaTeX paths"));
        auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
        folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("path"), owner}, {QStringLiteral("document_id"), id}});
        configuration.insert(QStringLiteral("source_folders"), folders);
        QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), projectId), configuration).isEmpty());
        QString error;
        const auto entries = Thoughts::DocumentBuildSelection::candidates(root.path(), owner, &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QMap<QString, QString> directories;
        for (const auto &value : entries) {
            const auto candidate = value.toObject();
            if (candidate.value(QStringLiteral("format")).toString() == QLatin1String("latex"))
                directories.insert(candidate.value(QStringLiteral("source")).toString(), candidate.value(QStringLiteral("working_directory")).toString(QStringLiteral("<none>")));
        }
        // Files directly in tex/source keep building from the folder root.
        QCOMPARE(directories, (QMap<QString, QString>{{QStringLiteral("paper/main.tex"), QStringLiteral("paper")},
            {QStringLiteral("main.tex"), QStringLiteral("<none>")}, {QStringLiteral("tex/source/sub/nested.tex"), QStringLiteral("tex/source/sub")},
            {QStringLiteral("tex/source/old.tex"), QStringLiteral("<none>")}}));
        auto saved = entries;
        for (qsizetype i = 0; i < saved.size(); ++i) {
            auto candidate = saved[i].toObject();
            candidate.insert(QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            candidate.insert(QStringLiteral("included"), true);
            saved[i] = candidate;
        }
        const QJsonObject manifest{{QStringLiteral("schema_version"), 4}, {QStringLiteral("document_id"), id}, {QStringLiteral("documents"), saved}};
        const auto storage = Thoughts::DocumentBuildSelection::uniformStorage(manifest);
        const auto read = Thoughts::DocumentBuildSelection::readUniform(storage, owner, &error);
        QVERIFY2(!read.isEmpty(), qPrintable(error));
        bool nested = false;
        for (const auto &value : read.value(QStringLiteral("documents")).toArray())
            nested = nested || (value.toObject().value(QStringLiteral("source")).toString() == QLatin1String("paper/main.tex")
                                && value.toObject().value(QStringLiteral("working_directory")).toString() == QLatin1String("paper"));
        QVERIFY(nested);
        auto bad = storage;
        auto rendering = bad.value(QStringLiteral("rendering")).toObject();
        const auto key = rendering.keys().first();
        rendering.insert(key, QJsonObject{{QStringLiteral("working_directory"), QStringLiteral("../outside")}});
        bad.insert(QStringLiteral("rendering"), rendering);
        QVERIFY(Thoughts::DocumentBuildSelection::readUniform(bad, owner, &error).isEmpty());
    }
    void repositoryEntryPointsMayBeNested() {
        QTemporaryDir repository;
        const QMap<QString, QByteArray> files{
            {QStringLiteral(".git/HEAD"), "ref: refs/heads/main\n"},
            {QStringLiteral("paper/main.tex"), "\\documentclass{article}\n\\begin{document}\n\\input{sections/body}\n\\end{document}\n"},
            {QStringLiteral("paper/sections/body.tex"), "Body.\n"},
            {QStringLiteral("slides/main.tex"), "\\documentclass{beamer}\n\\begin{document}\n\\end{document}\n"},
            {QStringLiteral("shared/macros.tex"), "\\newcommand{\\example}{Example}\n"},
            {QStringLiteral("build/stale.tex"), "\\documentclass{article}\n\\begin{document}\n\\end{document}\n"}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const auto path = repository.filePath(it.key());
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(it.value());
        }
        const auto snapshot = Thoughts::MarkdownFolderImport::scan(repository.path());
        QVERIFY2(snapshot.error.isEmpty(), qPrintable(snapshot.error));
        QVERIFY(snapshot.repositoryLayout);
        QCOMPARE(snapshot.documents, (QStringList{QStringLiteral("tex/source/paper/main.tex"), QStringLiteral("tex/source/slides/main.tex")}));
        QVERIFY(snapshot.files.contains(QStringLiteral("tex/source/shared/macros.tex")));
        const auto documents = snapshot.projectDocuments(QStringLiteral("docs-import"));
        QCOMPARE(documents.size(), 2);
        const auto paper = documents.at(0).toObject(), slides = documents.at(1).toObject();
        QCOMPARE(paper.value(QStringLiteral("working_directory")).toString(), QStringLiteral("tex/source/paper"));
        QCOMPARE(slides.value(QStringLiteral("working_directory")).toString(), QStringLiteral("tex/source/slides"));
        QVERIFY(paper.value(QStringLiteral("output")).toString() != slides.value(QStringLiteral("output")).toString());
        QVERIFY(paper.value(QStringLiteral("output")).toString().startsWith(QStringLiteral("import_main_")));
    }

    void sourceSizeInventoryTracksChangesWithoutReadingContents() {
        QTemporaryDir root;
        const QString owner = QStringLiteral("docs-sizes");
        QVERIFY(QDir().mkpath(root.filePath(owner + QStringLiteral("/nested"))));
        const auto put = [&](const QString &name, const QByteArray &bytes) {
            QFile file(root.filePath(owner + QLatin1Char('/') + name));
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const QByteArray rules(".git/\nexcluded.bin\n");
        QVERIFY(put(QStringLiteral(".ThoughtIgnore"), rules));
        QVERIFY(put(QStringLiteral("nested/input.tex"), QByteArray(10, 'a')));
        QVERIFY(put(QStringLiteral("unused.dat"), QByteArray(20, 'b')));
        QVERIFY(put(QStringLiteral("excluded.bin"), QByteArray(1000, 'x')));
        Thoughts::LibrarySourceInventory first(root.path());
        const auto initial = first.refresh(owner);
        QCOMPARE(initial.value(QStringLiteral("size_state")).toString(), QStringLiteral("current"));
        QCOMPARE(initial.value(QStringLiteral("size_bytes")).toInteger(), rules.size() + 30);
        QCOMPARE(first.directoriesEnumerated, 2);
        first.save({owner});
        Thoughts::LibrarySourceInventory warm(root.path());
        QCOMPARE(warm.cached().value(owner).value(QStringLiteral("size_state")).toString(), QStringLiteral("cached"));
        QCOMPARE(warm.refresh(owner), initial);
        QCOMPARE(warm.directoriesEnumerated, 0);
        QVERIFY(put(QStringLiteral("unused.dat"), QByteArray(40, 'b')));
        QCOMPARE(warm.refresh(owner).value(QStringLiteral("size_bytes")).toInteger(), rules.size() + 50);
        QCOMPARE(warm.directoriesEnumerated, 0);
        QTest::qWait(5);
        QVERIFY(put(QStringLiteral("nested/new.txt"), QByteArray(7, 'n')));
        QCOMPARE(warm.refresh(owner).value(QStringLiteral("size_bytes")).toInteger(), rules.size() + 57);
        QCOMPARE(warm.directoriesEnumerated, 1);
        QTest::qWait(5);
        QVERIFY(QFile::remove(root.filePath(owner + QStringLiteral("/nested/input.tex"))));
        QCOMPARE(warm.refresh(owner).value(QStringLiteral("size_bytes")).toInteger(), rules.size() + 47);
        QTest::qWait(5);
        const QByteArray newRules(".git/\nexcluded.bin\nunused.dat\n");
        QVERIFY(put(QStringLiteral(".ThoughtIgnore"), newRules));
        QCOMPARE(warm.refresh(owner).value(QStringLiteral("size_bytes")).toInteger(), newRules.size() + 7);
        warm.save({owner});
        QVERIFY(QDir(root.path()).rename(owner, QStringLiteral("docs-moved")));
        QCOMPARE(warm.refresh(owner).value(QStringLiteral("size_state")).toString(), QStringLiteral("unavailable"));
        QCOMPARE(warm.refresh(QStringLiteral("docs-moved")).value(QStringLiteral("size_bytes")).toInteger(), newRules.size() + 7);
        warm.save({QStringLiteral("docs-moved")});
        QVERIFY(!Thoughts::LibrarySourceInventory(root.path()).cached().contains(owner));
    }

    void singleProjectManifestPreservesOtherFoldersAndDocumentIdentity() {
        QTemporaryDir root;
        const auto id = QStringLiteral("new:central-test");
        auto value = Thoughts::ProjectConfiguration::empty(root.path(), id, QStringLiteral("Central"));
        auto folders = value.value(QStringLiteral("source_folders")).toArray();
        QJsonArray documents;
        QStringList identifiers;
        for (const auto &owner : {QStringLiteral("docs-a"), QStringLiteral("docs-b")}) {
            QDir().mkpath(root.filePath(owner));
            const auto folder = QUuid::createUuid().toString(QUuid::WithoutBraces);
            folders.append(QJsonObject{{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), owner},
                {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
            const auto identifier = QUuid::createUuid().toString(QUuid::WithoutBraces); identifiers.append(identifier);
            documents.append(QJsonObject{{QStringLiteral("document_id"), identifier}, {QStringLiteral("format"), QStringLiteral("latex")},
                {QStringLiteral("included"), true}, {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), QStringLiteral("tex/source/main.tex")}}}});
        }
        value.insert(QStringLiteral("source_folders"), folders); value.insert(QStringLiteral("documents"), documents);
        const auto path = Thoughts::ProjectConfiguration::path(root.path(), id);
        QVERIFY(Thoughts::ProjectConfiguration::save(path, value).isEmpty());
        QString error;
        auto projection = Thoughts::DocumentBuildSelection::effectiveManifest(root.path(), QStringLiteral("docs-a"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(projection.value(QStringLiteral("documents")).toArray().size(), 1);
        auto entries = projection.value(QStringLiteral("documents")).toArray();
        auto first = entries[0].toObject(); first.insert(QStringLiteral("included"), false); entries[0] = first;
        projection.insert(QStringLiteral("documents"), entries);
        QVERIFY(Thoughts::DocumentBuildSelection::rawWrite(root.filePath(QStringLiteral("docs-a/thoughts-project.json")), projection).isEmpty());
        const auto saved = Thoughts::ProjectConfiguration::read(path, &error);
        QCOMPARE(saved.value(QStringLiteral("documents")).toArray()[1], documents[1]);
        QCOMPARE(saved.value(QStringLiteral("documents")).toArray()[0].toObject().value(QStringLiteral("document_id")).toString(), identifiers[0]);
        QVERIFY(!saved.value(QStringLiteral("documents")).toArray()[0].toObject().value(QStringLiteral("included")).toBool());
        QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral("docs-a/thoughts-project.json"))));
        // Catalogue projection must preserve interleaved global order across folders.
        Thoughts::LibraryProjectStore store(root.path());
        QJsonObject project{{QStringLiteral("id"), id}, {QStringLiteral("name"), QStringLiteral("Central")},
            {QStringLiteral("source_project"), QStringLiteral("docs-a")},
            {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), QStringLiteral("docs-b")}}}},
            {QStringLiteral("documents"), QJsonArray{}}};
        QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
        auto ordered = saved;
        ordered.insert(QStringLiteral("documents"), QJsonArray{documents[1], documents[0]});
        QVERIFY(Thoughts::ProjectConfiguration::save(path, ordered).isEmpty());
        const auto visible = store.projects().first().toObject().value(QStringLiteral("documents")).toArray();
        QCOMPARE(visible.size(), 2);
        QCOMPARE(visible.first().toObject().value(QStringLiteral("document_id")).toString(), identifiers[1]);
        auto moved = saved; folders = moved.value(QStringLiteral("source_folders")).toArray();
        auto folder = folders[1].toObject(); folder.insert(QStringLiteral("path"), QStringLiteral("docs-relocated")); folders[1] = folder;
        moved.insert(QStringLiteral("source_folders"), folders);
        QVERIFY(Thoughts::ProjectConfiguration::save(path, moved).isEmpty());
        QCOMPARE(Thoughts::ProjectConfiguration::projection(moved, QStringLiteral("docs-relocated")).value(QStringLiteral("documents")).toArray()[0].toObject().value(QStringLiteral("document_id")).toString(), identifiers[0]);
    }
    void uniformPdfManifestRoundTripAndProjectIsolation() {
        const auto aggregate = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const auto paper = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QTemporaryDir root;
        for (const auto &owner : {QStringLiteral("docs-one"), QStringLiteral("docs-two")}) {
            const QString projectId = QStringLiteral("source:") + owner;
            auto configuration = Thoughts::ProjectConfiguration::empty(root.path(), projectId, owner);
            auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
            folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("path"), owner}, {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
            configuration.insert(QStringLiteral("source_folders"), folders);
            QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), projectId), configuration).isEmpty());
        }
        const auto first = Thoughts::DocumentBuildSelection::managedPdfSource(root.path(), QStringLiteral("docs-one"), QStringLiteral("paper.pdf"));
        const auto second = Thoughts::DocumentBuildSelection::managedPdfSource(root.path(), QStringLiteral("docs-two"), QStringLiteral("paper.pdf"));
        QVERIFY(first != second);
        QDir().mkpath(root.filePath(QStringLiteral(".thoughts")));
        QFile catalogue(root.filePath(QStringLiteral(".thoughts/library-projects.json")));
        QVERIFY(catalogue.open(QIODevice::WriteOnly));
        catalogue.write(R"({"projects":[{"id":"source:docs-one","source_project":"docs-empty"},{"id":"new:destination","source_folders":[{"project":"docs-one"}]}]})");
        catalogue.close();
        const auto sourcePath = Thoughts::ProjectConfiguration::path(root.path(), QStringLiteral("source:docs-one"));
        QString configurationError;
        auto original = Thoughts::ProjectConfiguration::read(sourcePath, &configurationError);
        QVERIFY(configurationError.isEmpty());
        auto bindings = original.value(QStringLiteral("source_folders")).toArray();
        const auto transferred = bindings[1].toObject(); bindings.removeAt(1);
        original.insert(QStringLiteral("source_folders"), bindings);
        QVERIFY(Thoughts::ProjectConfiguration::save(sourcePath, original).isEmpty());
        auto destination = Thoughts::ProjectConfiguration::empty(root.path(), QStringLiteral("new:destination"), QStringLiteral("Destination"));
        bindings = destination.value(QStringLiteral("source_folders")).toArray(); bindings.append(transferred);
        destination.insert(QStringLiteral("source_folders"), bindings);
        QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), QStringLiteral("new:destination")), destination).isEmpty());
        const auto moved = Thoughts::DocumentBuildSelection::managedPdfSource(root.path(), QStringLiteral("docs-one"), QStringLiteral("paper.pdf"));
        const auto destinationKey = QString::fromLatin1(QCryptographicHash::hash(QByteArray("new:destination"), QCryptographicHash::Sha256).toHex());
        QVERIFY(moved.contains(destinationKey));
        const QJsonObject document{{QStringLiteral("document_id"), paper}, {QStringLiteral("source"), first},
            {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), true}};
        const QJsonObject manifest{{QStringLiteral("schema_version"), 4}, {QStringLiteral("document_id"), aggregate},
            {QStringLiteral("documents"), QJsonArray{document}}, {QStringLiteral("rendering"), QJsonObject{{paper, QJsonObject{{QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}}}}}};
        QString error;
        const auto runtime = Thoughts::DocumentBuildSelection::readUniform(manifest, QStringLiteral("docs-one"), &error);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(Thoughts::DocumentBuildSelection::uniformStorage(runtime), manifest);
        const auto ordered = Thoughts::DocumentBuildSelection::reordered(runtime, QJsonArray{QStringLiteral("third_party/foreign.pdf")});
        QCOMPARE(ordered.value(QStringLiteral("documents")).toArray().size(), 1);
        auto invalid = manifest;
        auto withExtra = document; withExtra.insert(QStringLiteral("id"), QStringLiteral("third_party/paper.pdf"));
        invalid.insert(QStringLiteral("documents"), QJsonArray{withExtra});
        QVERIFY(Thoughts::DocumentBuildSelection::readUniform(invalid, QStringLiteral("docs-one"), &error).isEmpty());
        QVERIFY(!error.isEmpty());
    }
    void thoughtIgnoreUsesProjectRulesInsteadOfGitRules() {
        QTemporaryDir project;
        Thoughts::ThoughtIgnore defaults(project.path());
        QVERIFY(defaults.error().isEmpty());
        QVERIFY(defaults.excludes(QStringLiteral("tex/source/.git/config")));
        QVERIFY(!defaults.excludes(QStringLiteral("tex/source/main.tex")));
        QFile rules(project.filePath(QStringLiteral(".ThoughtIgnore")));
        QVERIFY(rules.open(QIODevice::WriteOnly));
        rules.write(".git/\nbuild/\n*.tmp\n!keep.tmp\n!build/keep.tmp\n/secret.txt\n**/cache/*.json\n");
        rules.close();
        Thoughts::ThoughtIgnore custom(project.path());
        QVERIFY(custom.error().isEmpty());
        QVERIFY(custom.excludes(QStringLiteral("build/output.pdf")));
        QVERIFY(custom.excludes(QStringLiteral("build/keep.tmp")));
        QVERIFY(custom.excludes(QStringLiteral("tex/source/draft.tmp")));
        QVERIFY(!custom.excludes(QStringLiteral("tex/source/keep.tmp")));
        QVERIFY(custom.excludes(QStringLiteral("secret.txt")));
        QVERIFY(!custom.excludes(QStringLiteral("nested/secret.txt")));
        QVERIFY(custom.excludes(QStringLiteral("nested/cache/data.json")));
    }
    void connectedRootsHaveIndependentInventories() {
        QTemporaryDir library;
        const QMap<QString, QByteArray> files {
            {QStringLiteral("docs-primary/.ThoughtIgnore"), ".git/\ntmp/\nbuild/\nprivate.txt\n"},
            {QStringLiteral("docs-primary/README.md"), "Read me.\n"},
            {QStringLiteral("docs-primary/tex/source/main.tex"), "Main.\n"},
            {QStringLiteral("docs-primary/build/copy.tex"), "Copy.\n"},
            {QStringLiteral("docs-primary/tmp/copy.tex"), "Copy.\n"},
            {QStringLiteral("docs-primary/private.txt"), "Private.\n"},
            {QStringLiteral("docs-secondary/.ThoughtIgnore"), ".git/\nREADME.md\n"},
            {QStringLiteral("docs-secondary/README.md"), "Hidden here.\n"},
            {QStringLiteral("docs-secondary/private.txt"), "Included here.\n"}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const QString path = library.filePath(it.key());
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(it.value());
        }
        const auto projectId = QStringLiteral("source:docs-primary");
        auto configuration = Thoughts::ProjectConfiguration::empty(library.path(), projectId, QStringLiteral("Primary"));
        auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
        for (const auto &owner : {QStringLiteral("docs-primary"), QStringLiteral("docs-secondary")})
            folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("path"), owner}, {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
        configuration.insert(QStringLiteral("source_folders"), folders);
        QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(library.path(), projectId), configuration).isEmpty());
        Thoughts::LibraryProjectStore store(library.path());
        QJsonArray locations;
        for (const auto &value : store.projects().first().toObject().value(QStringLiteral("sources")).toArray())
            if (value.toObject().value(QStringLiteral("id")).toString().startsWith(QLatin1String("docs-"))) locations.append(value); // Not the default folder.
        QCOMPARE(locations.size(), 2);
        QMap<QString, QSet<QString>> inventory;
        for (const auto &value : locations) {
            const auto location = value.toObject();
            const QString owner = location.value(QStringLiteral("id")).toString();
            QVERIFY(owner == QLatin1String("docs-primary") || owner == QLatin1String("docs-secondary"));
            for (const auto &entry : location.value(QStringLiteral("files")).toArray())
                inventory[owner].insert(entry.toObject().value(QStringLiteral("relative_path")).toString());
        }
        QCOMPARE(inventory.value(QStringLiteral("docs-primary")), (QSet<QString>{QStringLiteral(".ThoughtIgnore"), QStringLiteral("README.md"), QStringLiteral("tex/source/main.tex")}));
        QCOMPARE(inventory.value(QStringLiteral("docs-secondary")), (QSet<QString>{QStringLiteral(".ThoughtIgnore"), QStringLiteral("private.txt")}));
    }
    void rootLatexRepositoryPreservesLayoutAndDefersSelection() {
        QTemporaryDir library;
        const auto repository = library.filePath(QStringLiteral(".thoughts/git-imports/native/repository"));
        const QMap<QString, QByteArray> files{
            {QStringLiteral(".git/HEAD"), "ref: refs/heads/main\n"},
            {QStringLiteral("main.tex"), "\\documentclass{article}\n\\begin{document}\n\\input{sections/body}\n\\end{document}\n"},
            {QStringLiteral("macros.tex"), "\\newcommand{\\example}{Example}\n"},
            {QStringLiteral("sections/body.tex"), "Body.\n"},
            {QStringLiteral("figs/figure.pdf"), "%PDF-1.4\n"},
            {QStringLiteral("CLAUDE.md"), "[Main](main.tex)\n"}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const auto path = QDir(repository).filePath(it.key());
            QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(it.value());
        }
        const auto snapshot = Thoughts::MarkdownFolderImport::scan(repository);
        QVERIFY(snapshot.error.isEmpty()); QVERIFY(snapshot.repositoryLayout);
        QCOMPARE(snapshot.documents, QStringList{QStringLiteral("tex/source/main.tex")});
        QVERIFY(snapshot.files.contains(QStringLiteral("tex/source/figs/figure.pdf")));
        const auto values = Thoughts::prepareMarkdownFolderImport(library.path(), nullptr, repository);
        QVERIFY(!values.isEmpty());
        Thoughts::LibraryProjectStore store(library.path());
        QVERIFY(store.enqueue(QStringLiteral("create"), {{QStringLiteral("id"), QStringLiteral("new:native")}}, {{QStringLiteral("name"), QStringLiteral("Native")}}).isEmpty());
        const auto error = store.enqueue(QStringLiteral("import_folder"), store.projects().first().toObject(), values);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto op = store.pending().last().toObject();
        const auto owner = store.projects().first().toObject().value(QStringLiteral("source_folders")).toArray().first().toObject().value(QStringLiteral("project")).toString();
        const auto manifestPath = library.filePath(owner + QStringLiteral("/thoughts-project.json"));
        QFile manifestFile(manifestPath);
        QVERIFY(manifestFile.open(QIODevice::ReadOnly));
        const auto sourceManifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
        QCOMPARE(sourceManifest.value(QStringLiteral("schema_version")).toInt(), 4);
        const auto sourceDocument = sourceManifest.value(QStringLiteral("documents")).toArray().first().toObject();
        QCOMPARE(sourceDocument.size(), 4);
        QVERIFY(!sourceDocument.contains(QStringLiteral("id")));
        QVERIFY(!sourceDocument.contains(QStringLiteral("title")));
        QVERIFY(!QUuid(sourceDocument.value(QStringLiteral("document_id")).toString()).isNull());
        manifestFile.close();
        QFile defaultIgnore(library.filePath(owner + QStringLiteral("/.ThoughtIgnore")));
        QVERIFY(defaultIgnore.open(QIODevice::ReadOnly));
        QCOMPARE(defaultIgnore.readAll(), Thoughts::ThoughtIgnore::defaultRules());
        defaultIgnore.close();
        // Selection is deferred: the Project configuration lists the folder,
        // and so offers its documents, only once Sync has accepted the import.
        QString selectionError;
        QVERIFY(Thoughts::DocumentBuildSelection::candidates(library.path(), owner, &selectionError).isEmpty());
        QVERIFY(!selectionError.isEmpty());
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            QFile file(library.filePath(owner + QStringLiteral("/tex/source/") + it.key()));
            QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), it.value());
        }
        QFile ignore(library.filePath(owner + QStringLiteral("/.ThoughtIgnore")));
        QVERIFY(ignore.open(QIODevice::WriteOnly));
        ignore.write(".git/\ntex/source/figs/\n");
        ignore.close();
        const auto locations = store.projects().first().toObject().value(QStringLiteral("sources")).toArray();
        QStringList shown;
        for (const auto &location : locations) for (const auto &entry : location.toObject().value(QStringLiteral("files")).toArray())
            shown.append(entry.toObject().value(QStringLiteral("id")).toString());
        QVERIFY(shown.contains(owner + QStringLiteral("/.ThoughtIgnore")));
        QVERIFY(shown.contains(owner + QStringLiteral("/tex/source/main.tex")));
        QVERIFY(!shown.contains(owner + QStringLiteral("/tex/source/.git/HEAD")));
        QVERIFY(!shown.contains(owner + QStringLiteral("/tex/source/figs/figure.pdf")));
        QVERIFY(store.prepareFolderUpload(op.value(QStringLiteral("operation_id")).toString()).isEmpty());
        QFile upload(store.pending().last().toObject().value(QStringLiteral("local_path")).toString());
        QVERIFY(upload.open(QIODevice::ReadOnly));
        const auto bundle = QJsonDocument::fromJson(upload.readAll()).object();
        QVERIFY(bundle.value(QStringLiteral("included_documents")).toArray().isEmpty());
        QVERIFY(bundle.value(QStringLiteral("repository_layout")).toBool());
        for (const auto &entry : bundle.value(QStringLiteral("files")).toArray())
            QVERIFY(entry.toObject().value(QStringLiteral("path")).toString() != QStringLiteral("tex/source/figs/figure.pdf"));
        upload.close();
    }
    void htmlFilesAreIncludedWithoutBeingPdfDocuments() {
        QTemporaryDir folder;
        QFile markdown(folder.filePath(QStringLiteral("README.md"))); QVERIFY(markdown.open(QIODevice::WriteOnly)); markdown.write("# Page\n"); markdown.close();
        QFile html(folder.filePath(QStringLiteral("plot.html"))); QVERIFY(html.open(QIODevice::WriteOnly)); html.write("<!doctype html><p>Plot</p>"); html.close();
        const auto imported = Thoughts::MarkdownFolderImport::scan(folder.path());
        QVERIFY2(imported.error.isEmpty(), qPrintable(imported.error));
        QCOMPARE(imported.documents, QStringList{QStringLiteral("README.md")});
        QVERIFY(imported.includedFiles(imported.documents).contains(QStringLiteral("plot.html")));
        const auto bundle = QJsonDocument::fromJson(imported.bundle(imported.documents)).object();
        QCOMPARE(bundle.value(QStringLiteral("files")).toArray().size(), 2);
        QVERIFY(!Thoughts::MarkdownFolderImport::html(QStringLiteral("image.png")));
    }
    void gitImportFailureCleansStaging() {
        QTemporaryDir library;
        bool reportedFailure = false;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("importSourcesFromGit"))) button->click();
            else if (auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("gitImportUrl"))) {
                url->setText(library.filePath(QStringLiteral("missing-repository"))); dialog->accept();
            } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
                reportedFailure = message->text().contains(QStringLiteral("repository path does not exist")); message->accept();
            }
        });
        driver.start(10);
        QTimer timeout; timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        timeout.start(15000);
        QVERIFY(Thoughts::prepareSourcesImport(library.path(), nullptr).isEmpty());
        driver.stop(); timeout.stop();
        QVERIFY(reportedFailure);
        QVERIFY(QDir(library.filePath(QStringLiteral(".thoughts/git-imports"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
        QVERIFY(Thoughts::LibraryProjectStore(library.path()).pending().isEmpty());
    }
    void gitImportEmptyFolderExplainsProblem() {
        QTemporaryDir library;
        const auto emptyFolder = library.filePath(QStringLiteral("empty-folder"));
        QVERIFY(QDir().mkpath(emptyFolder));
        QString warning;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("importSourcesFromGit"))) button->click();
            else if (auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("gitImportUrl"))) {
                url->setText(emptyFolder); dialog->accept();
            } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
                warning = message->text(); message->accept();
            }
        });
        driver.start(10);
        QTimer timeout; timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        timeout.start(15000);
        QVERIFY(Thoughts::prepareSourcesImport(library.path(), nullptr).isEmpty());
        driver.stop(); timeout.stop();
        QVERIFY(warning.contains(QStringLiteral("not a Git repository")));
        QVERIFY(warning.contains(QStringLiteral("From folder")));
        QVERIFY(!QDir(library.filePath(QStringLiteral(".thoughts/git-imports"))).exists());
    }
    void gitImportUnreachableHostExplainsProblem() {
        // No git program is needed: PATH is empty.
        QTemporaryDir library;
        const QByteArray path = qgetenv("PATH");
        qputenv("PATH", QByteArray());
        QString warning;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("importSourcesFromGit"))) button->click();
            else if (auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("gitImportUrl"))) {
                url->setText(QStringLiteral("https://example.invalid/notes.git")); dialog->accept();
            } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
                warning = message->text(); message->accept();
            }
        });
        driver.start(10);
        QTimer timeout; timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        timeout.start(60000);
        const auto values = Thoughts::prepareSourcesImport(library.path(), nullptr);
        qputenv("PATH", path);
        driver.stop(); timeout.stop();
        QVERIFY(values.isEmpty());
        QVERIFY2(warning.contains(QStringLiteral("Could not clone the repository")), qPrintable(warning));
        QVERIFY2(warning.contains(QStringLiteral("could not be found")) || warning.contains(QStringLiteral("connect")), qPrintable(warning));
        QVERIFY(!warning.contains(QStringLiteral("Git is not installed")));
        QVERIFY(QDir(library.filePath(QStringLiteral(".thoughts/git-imports"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty());
    }
    void gitImportRejectsAddressesGitCannotClone() {
        QVERIFY(Thoughts::gitImportAddressSupported(QStringLiteral("https://github.com/octocat/Hello-World")));
        QVERIFY(Thoughts::gitImportAddressSupported(QStringLiteral("git@github.com:octocat/Hello-World.git")));
        QVERIFY(Thoughts::gitImportAddressSupported(QStringLiteral("ssh://git@example.org:2222/notes.git")));
        QVERIFY(Thoughts::gitImportAddressSupported(QStringLiteral("file:///srv/notes.git")));
        QVERIFY(Thoughts::gitImportAddressSupported(QDir::rootPath() + QStringLiteral("repos/notes")));
        QVERIFY(!Thoughts::gitImportAddressSupported(QStringLiteral("htps://github.com/octocat/Hello-World")));
        QVERIFY(!Thoughts::gitImportAddressSupported(QStringLiteral("notes")));
        QVERIFY(!Thoughts::gitImportAddressSupported(QStringLiteral("https://")));
        QString warning;
        QTemporaryDir library;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("importSourcesFromGit"))) button->click();
            else if (auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("gitImportUrl"))) {
                url->setText(QStringLiteral("htps://github.com/octocat/Hello-World")); dialog->accept();
            } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
                warning = message->text(); message->accept();
            }
        });
        driver.start(10);
        QVERIFY(Thoughts::prepareSourcesImport(library.path(), nullptr).isEmpty());
        driver.stop();
        QVERIFY2(warning.contains(QStringLiteral("not a repository address")), qPrintable(warning));
        QVERIFY(!QDir(library.filePath(QStringLiteral(".thoughts/git-imports"))).exists());
    }
    // A bare origin with main (default) and feature.
    static QString bareOrigin(const QString &root) {
        const auto path = QDir(root).filePath(QStringLiteral("origin.git"));
        git_repository *repository = nullptr;
        if (git_repository_init(&repository, QFile::encodeName(path).constData(), 1) != 0) return {};
        const bool ok = commitFiles(repository, "refs/heads/main", {{QStringLiteral("README.md"), "# Main\n"}, {QStringLiteral("notes/chapter.md"), "Main chapter\n"}}, "Main")
            && commitFiles(repository, "refs/heads/feature", {{QStringLiteral("README.md"), "# Feature\n"}, {QStringLiteral("notes/chapter.md"), "Feature chapter\n"},
                                                              {QStringLiteral("feature.md"), "Only on feature\n"}}, "Feature", "refs/heads/main")
            && git_repository_set_head(repository, "refs/heads/main") == 0;
        git_repository_free(repository);
        return ok ? path : QString();
    }
    void gitCloneChecksOutTheDefaultBranch() {
        LibGit2 libgit2;
        QTemporaryDir dir;
        const auto origin = bareOrigin(dir.path());
        QVERIFY(!origin.isEmpty());
        const auto checkout = dir.filePath(QStringLiteral("staging/repository"));
        RecordingPrompts prompts;
        const auto outcome = Thoughts::gitClone({origin, {}, checkout, {}}, &prompts, nullptr);
        QVERIFY2(outcome.status == Thoughts::GitCloneOutcome::Cloned, qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QVERIFY(QFileInfo(QDir(checkout).filePath(QStringLiteral(".git"))).isDir());
        QCOMPARE(fileContents(QDir(checkout).filePath(QStringLiteral(".git/HEAD"))), QByteArray("ref: refs/heads/main\n"));
        QCOMPARE(fileContents(QDir(checkout).filePath(QStringLiteral("notes/chapter.md"))), QByteArray("Main chapter\n"));
        QVERIFY(!QFileInfo::exists(QDir(checkout).filePath(QStringLiteral("feature.md"))));
        QCOMPARE(originUrl(checkout), origin);
        // Like git clone: every branch is fetched, the default one is tracked.
        git_repository *repository = nullptr;
        QCOMPARE(git_repository_open(&repository, QFile::encodeName(checkout).constData()), 0);
        git_oid id;
        QCOMPARE(git_reference_name_to_id(&id, repository, "refs/remotes/origin/feature"), 0);
        git_config *config = nullptr;
        git_buf remote = GIT_BUF_INIT;
        QCOMPARE(git_repository_config_snapshot(&config, repository), 0);
        QCOMPARE(git_config_get_string_buf(&remote, config, "branch.main.remote"), 0);
        QCOMPARE(QByteArray(remote.ptr), QByteArray("origin"));
        git_buf_dispose(&remote);
        git_config_free(config);
        git_repository_free(repository);
        QCOMPARE(int(prompts.signIns), 0);
    }
    void gitCloneChecksOutTheNamedBranch() {
        LibGit2 libgit2;
        QTemporaryDir dir;
        const auto origin = bareOrigin(dir.path());
        const auto checkout = dir.filePath(QStringLiteral("repository"));
        const auto outcome = Thoughts::gitClone({origin, QStringLiteral("feature"), checkout, {}}, nullptr, nullptr);
        QVERIFY2(outcome.status == Thoughts::GitCloneOutcome::Cloned, qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QCOMPARE(fileContents(QDir(checkout).filePath(QStringLiteral(".git/HEAD"))), QByteArray("ref: refs/heads/feature\n"));
        QCOMPARE(fileContents(QDir(checkout).filePath(QStringLiteral("feature.md"))), QByteArray("Only on feature\n"));
        QCOMPARE(originUrl(checkout), origin);
    }
    void gitCloneMissingBranchExplainsAndCleansUp() {
        LibGit2 libgit2;
        QTemporaryDir dir;
        const auto origin = bareOrigin(dir.path());
        const auto checkout = dir.filePath(QStringLiteral("repository"));
        const auto outcome = Thoughts::gitClone({origin, QStringLiteral("no-such-branch"), checkout, {}}, nullptr, nullptr);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Failed);
        QVERIFY2(outcome.message.contains(QStringLiteral("“no-such-branch” does not exist")), qPrintable(outcome.message));
        QVERIFY(!QFileInfo::exists(checkout));
    }
    void gitCloneInvalidAddressExplains() {
        LibGit2 libgit2;
        QTemporaryDir dir;
        const auto checkout = dir.filePath(QStringLiteral("repository"));
        auto outcome = Thoughts::gitClone({QStringLiteral("htps://example.org/notes.git"), {}, checkout, {}}, nullptr, nullptr);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Failed);
        QVERIFY2(outcome.message.contains(QStringLiteral("not a repository address")), qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QVERIFY(!QFileInfo::exists(checkout));
        outcome = Thoughts::gitClone({dir.filePath(QStringLiteral("missing")), {}, checkout, {}}, nullptr, nullptr);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Failed);
        QVERIFY2(outcome.message.contains(QStringLiteral("No Git repository was found")), qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QVERIFY(!QFileInfo::exists(checkout));
    }
    void gitCloneCancelStopsTheTransferAndCleansUp() {
        LibGit2 libgit2;
        QTemporaryDir dir;
        const auto origin = bareOrigin(dir.path());
        const auto checkout = dir.filePath(QStringLiteral("repository"));
        std::atomic_bool cancel{false};
        int calls = 0;
        // file:// fetches through the transport, which reports progress as objects arrive.
        const auto outcome = Thoughts::gitClone({QUrl::fromLocalFile(origin).toString(), {}, checkout, {}}, nullptr, &cancel,
                                                [&](const Thoughts::GitCloneProgress &) { ++calls; cancel = true; });
        QVERIFY(calls > 0);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Canceled);
        QVERIFY(!QFileInfo::exists(checkout));
    }
    void gitCloneOfAWorkingCopyLeavesOutUncommittedChanges() {
        LibGit2 libgit2;
        QTemporaryDir origin, dir;
        git_repository *repository = nullptr;
        QCOMPARE(git_repository_init(&repository, QFile::encodeName(origin.path()).constData(), 0), 0);
        QVERIFY(commitFiles(repository, "refs/heads/main", {{QStringLiteral("chapter.md"), "Committed\n"}}, "Commit"));
        QCOMPARE(git_repository_set_head(repository, "refs/heads/main"), 0);
        git_checkout_options force;
        git_checkout_options_init(&force, GIT_CHECKOUT_OPTIONS_VERSION);
        force.checkout_strategy = GIT_CHECKOUT_FORCE;
        QCOMPARE(git_checkout_head(repository, &force), 0);
        git_repository_free(repository);
        QCOMPARE(fileContents(origin.filePath(QStringLiteral("chapter.md"))), QByteArray("Committed\n"));
        QFile edited(origin.filePath(QStringLiteral("chapter.md")));
        QVERIFY(edited.open(QIODevice::WriteOnly)); edited.write("Edited, not committed\n"); edited.close();
        QFile untracked(origin.filePath(QStringLiteral("draft.md")));
        QVERIFY(untracked.open(QIODevice::WriteOnly)); untracked.write("Untracked\n"); untracked.close();
        const auto checkout = dir.filePath(QStringLiteral("repository"));
        const auto outcome = Thoughts::gitClone({origin.path(), {}, checkout, {}}, nullptr, nullptr);
        QVERIFY2(outcome.status == Thoughts::GitCloneOutcome::Cloned, qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QCOMPARE(fileContents(QDir(checkout).filePath(QStringLiteral("chapter.md"))), QByteArray("Committed\n"));
        QVERIFY(!QFileInfo::exists(QDir(checkout).filePath(QStringLiteral("draft.md"))));
        QCOMPARE(originUrl(checkout), origin.path());
    }
    void gitCloneRefusesAnUntrustedCertificate() {
        LibGit2 libgit2;
        SignInServer server;
        QVERIFY(server.start());
        QTemporaryDir dir;
        RecordingPrompts prompts;
        const auto checkout = dir.filePath(QStringLiteral("repository"));
        const auto url = QStringLiteral("https://127.0.0.1:%1/notes.git").arg(server.server.serverPort());
        const auto outcome = cloneOnThread({url, {}, checkout, {}}, &prompts);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Failed);
        QVERIFY2(outcome.message.contains(QStringLiteral("certificate")), qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QCOMPARE(int(prompts.signIns), 0);
        QVERIFY(server.authorizations.isEmpty());
        QVERIFY(!QFileInfo::exists(checkout));
    }
    void gitCloneAsksOnceForHttpsSignIn() {
        LibGit2 libgit2;
        QTemporaryDir dir;
        const auto certificate = dir.filePath(QStringLiteral("server.pem"));
        QFile file(certificate);
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write(testCertificate); file.close();
        // Only the OpenSSL backend takes extra trusted certificates (Linux).
        if (git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, QFile::encodeName(certificate).constData(), nullptr) != 0)
            QSKIP("libgit2's TLS backend cannot trust a test certificate here");
        SignInServer server;
        QVERIFY(server.start());
        RecordingPrompts prompts;
        const auto checkout = dir.filePath(QStringLiteral("repository"));
        const auto url = QStringLiteral("https://127.0.0.1:%1/private.git").arg(server.server.serverPort());
        auto outcome = cloneOnThread({url, {}, checkout, {}}, &prompts);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Failed);
        QVERIFY2(outcome.message.contains(QStringLiteral("did not accept")), qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QCOMPARE(int(prompts.signIns), 1);
        QVERIFY(prompts.signInUrl.startsWith(url.left(url.lastIndexOf(QLatin1Char('/')))));
        QCOMPARE(server.authorizations, QList<QByteArray>{"Basic " + QByteArray("reader:token-123").toBase64()});
        QVERIFY(!QFileInfo::exists(checkout));
        QVERIFY(prompts.signInName.isEmpty());
        // A user name without password in the URL (Overleaf's https://git@git.overleaf.com/<id>)
        // starts the question; the server never sees an empty password.
        server.silentRefusal = true;
        server.authorizations.clear();
        const auto withUser = QStringLiteral("https://git@127.0.0.1:%1/private.git").arg(server.server.serverPort());
        outcome = cloneOnThread({withUser, {}, checkout, {}}, &prompts);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Failed);
        QVERIFY2(outcome.message.contains(QStringLiteral("did not accept")), qPrintable(outcome.message + QLatin1Char('\n') + outcome.detail));
        QCOMPARE(int(prompts.signIns), 2);
        QCOMPARE(prompts.signInName, QStringLiteral("git"));
        QCOMPARE(server.authorizations, QList<QByteArray>{"Basic " + QByteArray("reader:token-123").toBase64()});
        server.silentRefusal = false;
        // Canceling the sign-in cancels the clone.
        prompts.answerSignIn = false;
        outcome = cloneOnThread({url, {}, checkout, {}}, &prompts);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Canceled);
        QCOMPARE(int(prompts.signIns), 3);
        // No password over plain http.
        QTcpServer plain;
        QVERIFY(plain.listen(QHostAddress::LocalHost));
        connect(&plain, &QTcpServer::newConnection, this, [&plain] {
            while (auto *socket = plain.nextPendingConnection())
                connect(socket, &QTcpSocket::readyRead, socket, [socket] {
                    socket->readAll();
                    socket->write("HTTP/1.1 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"test\"\r\nContent-Length: 0\r\n\r\n");
                });
        });
        prompts.answerSignIn = true;
        outcome = cloneOnThread({QStringLiteral("http://127.0.0.1:%1/private.git").arg(plain.serverPort()), {}, checkout, {}}, &prompts);
        QCOMPARE(outcome.status, Thoughts::GitCloneOutcome::Failed);
        QVERIFY2(outcome.message.contains(QStringLiteral("https://")), qPrintable(outcome.message));
        QCOMPARE(int(prompts.signIns), 3);
    }
    void gitImportCancelReturnsAndCleansStaging() {
        // A server that accepts and never answers: Cancel must not wait for it.
        QTcpServer silent;
        QVERIFY(silent.listen(QHostAddress::LocalHost));
        QList<QTcpSocket *> connections;
        connect(&silent, &QTcpServer::newConnection, this, [&] { while (auto *socket = silent.nextPendingConnection()) connections.append(socket); });
        QTemporaryDir library;
        bool warned = false, canceled = false;
        QString warning;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            for (auto *widget : QApplication::topLevelWidgets()) {
                if (auto *progress = qobject_cast<QProgressDialog *>(widget); progress && progress->objectName() == QLatin1String("gitCloneProgress")
                    && progress->isVisible() && !connections.isEmpty() && !canceled) {
                    canceled = true;
                    if (auto *button = progress->findChild<QPushButton *>()) button->click(); // as the user does
                    return;
                }
            }
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("importSourcesFromGit"))) button->click();
            else if (auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("gitImportUrl"))) {
                url->setText(QStringLiteral("http://127.0.0.1:%1/notes.git").arg(silent.serverPort())); dialog->accept();
            } else if (auto *message = qobject_cast<QMessageBox *>(dialog)) {
                warned = true; warning = message->text(); message->accept();
            }
        });
        driver.start(10);
        QElapsedTimer timer; timer.start();
        QVERIFY(Thoughts::prepareSourcesImport(library.path(), nullptr).isEmpty());
        driver.stop();
        QVERIFY2(canceled, qPrintable(QStringLiteral("connections %1, warning %2").arg(connections.size()).arg(warning)));
        QVERIFY2(!warned, qPrintable(warning));
        QVERIFY2(timer.elapsed() < 10000, qPrintable(QString::number(timer.elapsed())));
        // The clone ends when the connection does, and removes its checkout.
        qDeleteAll(connections);
        const auto staging = library.filePath(QStringLiteral(".thoughts/git-imports"));
        QTRY_VERIFY_WITH_TIMEOUT(QDir(staging).entryList(QDir::Dirs | QDir::NoDotAndDotDot).isEmpty(), 20000);
    }
    void knownHostsMatchAsOpenSshDoes() {
        const auto blob = [](const QByteArray &type, const QByteArray &body) {
            QByteArray key;
            const auto add = [&key](const QByteArray &value) { char length[4]; qToBigEndian<quint32>(quint32(value.size()), length); key.append(length, 4).append(value); };
            add(type); add(body);
            return key;
        };
        const auto ed = blob("ssh-ed25519", QByteArray(32, 'a'));
        const auto otherEd = blob("ssh-ed25519", QByteArray(32, 'b'));
        const auto rsa = blob("ssh-rsa", QByteArray(64, 'c'));
        QCOMPARE(Thoughts::sshKeyType(ed), QByteArray("ssh-ed25519"));
        QCOMPARE(Thoughts::sshKeyFingerprint(ed), QStringLiteral("SHA256:") + QString::fromLatin1(QCryptographicHash::hash(ed, QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals)));
        using Thoughts::KnownHost;
        const QByteArray plain = "# comment\ngithub.com,140.82.121.4 " + QByteArray("ssh-ed25519 ") + ed.toBase64() + "\n";
        QCOMPARE(Thoughts::checkKnownHosts(plain, QStringLiteral("GitHub.com"), 22, ed), KnownHost::Matches);
        QCOMPARE(Thoughts::checkKnownHosts(plain, QStringLiteral("github.com"), 22, otherEd), KnownHost::Mismatch);
        QCOMPARE(Thoughts::checkKnownHosts(plain, QStringLiteral("github.com"), 22, rsa), KnownHost::OtherKeyTypesKnown);
        QCOMPARE(Thoughts::checkKnownHosts(plain, QStringLiteral("gitlab.com"), 22, ed), KnownHost::Unknown);
        QCOMPARE(Thoughts::checkKnownHosts(plain, QStringLiteral("github.com"), 2222, ed), KnownHost::Unknown);
        // Hashed names (HashKnownHosts) and [host]:port.
        const QByteArray salt("0123456789abcdefghij");
        const auto hashed = [&salt](const QByteArray &name) {
            return "|1|" + salt.toBase64() + '|' + QMessageAuthenticationCode::hash(name, salt, QCryptographicHash::Sha1).toBase64();
        };
        const QByteArray hashedLines = hashed("[git.example.org]:2222") + " ssh-ed25519 " + ed.toBase64() + '\n' + hashed("example.org") + " ssh-ed25519 " + ed.toBase64() + '\n';
        QCOMPARE(Thoughts::checkKnownHosts(hashedLines, QStringLiteral("git.example.org"), 2222, ed), KnownHost::Matches);
        QCOMPARE(Thoughts::checkKnownHosts(hashedLines, QStringLiteral("git.example.org"), 22, ed), KnownHost::Unknown);
        QCOMPARE(Thoughts::checkKnownHosts(hashedLines, QStringLiteral("example.org"), 22, otherEd), KnownHost::Mismatch);
        // Wildcards, negation, revoked keys and certificate authorities.
        const QByteArray patterns = "*.example.net,!bad.example.net ssh-ed25519 " + ed.toBase64() + "\n@cert-authority *.example.net ssh-ed25519 " + otherEd.toBase64() + '\n';
        QCOMPARE(Thoughts::checkKnownHosts(patterns, QStringLiteral("git.example.net"), 22, ed), KnownHost::Matches);
        QCOMPARE(Thoughts::checkKnownHosts(patterns, QStringLiteral("bad.example.net"), 22, ed), KnownHost::Unknown);
        QCOMPARE(Thoughts::checkKnownHosts(patterns, QStringLiteral("git.example.net"), 22, otherEd), KnownHost::Mismatch);
        QCOMPARE(Thoughts::checkKnownHosts(plain + "@revoked * ssh-ed25519 " + ed.toBase64() + '\n', QStringLiteral("github.com"), 22, ed), KnownHost::Mismatch);
        // The line Thoughts appends is read back as a match.
        QCOMPARE(Thoughts::knownHostsLine(QStringLiteral("Git.Example.org"), 2222, ed), "[git.example.org]:2222 ssh-ed25519 " + ed.toBase64() + '\n');
        QCOMPARE(Thoughts::checkKnownHosts(Thoughts::knownHostsLine(QStringLiteral("git.example.org"), 2222, ed), QStringLiteral("git.example.org"), 2222, ed), KnownHost::Matches);
        QCOMPARE(Thoughts::checkKnownHosts(Thoughts::knownHostsLine(QStringLiteral("github.com"), 22, ed), QStringLiteral("github.com"), 22, ed), KnownHost::Matches);
    }
    void sshAddressesAndEncryptedKeys() {
        QString host; int port = 0;
        QVERIFY(Thoughts::sshEndpoint(QStringLiteral("git@github.com:octocat/Hello-World.git"), &host, &port));
        QCOMPARE(host, QStringLiteral("github.com")); QCOMPARE(port, 22);
        QVERIFY(Thoughts::sshEndpoint(QStringLiteral("ssh://git@example.org:2222/notes.git"), &host, &port));
        QCOMPARE(host, QStringLiteral("example.org")); QCOMPARE(port, 2222);
        QVERIFY(Thoughts::sshEndpoint(QStringLiteral("example.org:notes.git"), &host, &port));
        QVERIFY(!Thoughts::sshEndpoint(QStringLiteral("https://github.com/octocat/Hello-World"), &host, &port));
        QVERIFY(!Thoughts::sshEndpoint(QStringLiteral("C:\\repos\\notes"), &host, &port));
        QVERIFY(!Thoughts::sshEndpoint(QStringLiteral("/srv/a:b"), &host, &port));
        QVERIFY(!Thoughts::sshEndpoint(QStringLiteral("./a:b"), &host, &port));
        const auto openssh = [](const QByteArray &cipher) {
            QByteArray data("openssh-key-v1", 15);
            char length[4]; qToBigEndian<quint32>(quint32(cipher.size()), length);
            data.append(length, 4).append(cipher).append(QByteArray(40, 'x'));
            return "-----BEGIN OPENSSH PRIVATE KEY-----\n" + data.toBase64() + "\n-----END OPENSSH PRIVATE KEY-----\n";
        };
        QVERIFY(!Thoughts::sshPrivateKeyEncrypted(openssh("none")));
        QVERIFY(Thoughts::sshPrivateKeyEncrypted(openssh("aes256-ctr")));
        QVERIFY(Thoughts::sshPrivateKeyEncrypted("-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,00\n\nAAAA\n-----END RSA PRIVATE KEY-----\n"));
        QVERIFY(!Thoughts::sshPrivateKeyEncrypted("-----BEGIN RSA PRIVATE KEY-----\nAAAA\n-----END RSA PRIVATE KEY-----\n"));
        QVERIFY(Thoughts::sshPrivateKeyEncrypted("-----BEGIN ENCRYPTED PRIVATE KEY-----\nAAAA\n-----END ENCRYPTED PRIVATE KEY-----\n"));
    }
    void gitImportNameUsesTheRepositoryName() {
        QCOMPARE(Thoughts::gitImportName(QStringLiteral("/Users/alex/release-1.5-report/")), QStringLiteral("release-1.5-report"));
        QCOMPARE(Thoughts::gitImportName(QStringLiteral(" /Users/alex/Documents/docs-benchmarks// ")), QStringLiteral("docs-benchmarks"));
        QCOMPARE(Thoughts::gitImportName(QStringLiteral("https://github.com/example-org/release-1.5-report.git")), QStringLiteral("release-1.5-report"));
        QCOMPARE(Thoughts::gitImportName(QStringLiteral("git@github.com:sam-example/Lab-meeting-notes.git")), QStringLiteral("Lab-meeting-notes"));
        QCOMPARE(Thoughts::gitImportName(QStringLiteral("git@example.org:notes.git")), QStringLiteral("notes"));
        QCOMPARE(Thoughts::gitImportName(QStringLiteral("C:\\repos\\notes\\")), QStringLiteral("notes"));
        QCOMPARE(Thoughts::gitImportName(QStringLiteral("/")), QString());
    }
    void gitImportWithoutCommittedDocumentsStillClones() {
        QTemporaryDir origin, library;
        LibGit2 libgit2;
        git_repository *initialized = nullptr;
        QCOMPARE(git_repository_init(&initialized, QFile::encodeName(origin.path()).constData(), 0), 0);
        QCOMPARE(git_repository_set_head(initialized, "refs/heads/main"), 0);
        git_repository_free(initialized);
        QVERIFY(QDir(origin.path()).mkpath(QStringLiteral("tex/source")));
        QFile source(origin.filePath(QStringLiteral("tex/source/main.tex")));
        QVERIFY(source.open(QIODevice::WriteOnly));
        source.write("\\documentclass{article}\n\\begin{document}Uncommitted.\\end{document}\n");
        source.close();

        int step = 0;
        QString message;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (step == 0) {
                if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("importSourcesFromGit"))) {
                    ++step; button->click();
                }
            } else if (step == 1) {
                if (auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("gitImportUrl"))) {
                    // A trailing slash still names the import after the repository.
                    url->setText(origin.path() + QLatin1Char('/'));
                    ++step; dialog->accept();
                }
            } else if (step == 2) {
                if (auto *warning = qobject_cast<QMessageBox *>(dialog)) {
                    message = warning->text();
                    ++step; warning->accept();
                }
            }
        });
        driver.start(10);
        QTimer timeout; timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        timeout.start(15000);
        const auto values = Thoughts::prepareSourcesImport(library.path(), nullptr);
        QVERIFY(!values.isEmpty());
        driver.stop(); timeout.stop();
        QCOMPARE(step, 2);
        QVERIFY(message.isEmpty());
        QCOMPARE(values.value(QStringLiteral("name")).toString(), QFileInfo(origin.path()).fileName());
    }
    void gitCloneBranchSubfolderAndEditableSources() {
        QTemporaryDir origin, library;
        LibGit2 libgit2;
        git_repository *fixture = nullptr;
        QCOMPARE(git_repository_init(&fixture, QFile::encodeName(origin.path()).constData(), 0), 0);
        QVERIFY(commitFiles(fixture, "refs/heads/selected-branch", {{QStringLiteral("notes/chapter.md"), "# Original\n"}, {QStringLiteral("outside.md"), "# Original\n"}}, "Fixture"));
        QVERIFY(commitFiles(fixture, "refs/heads/default-branch", {{QStringLiteral("notes/chapter.md"), "# Default branch content\n"}, {QStringLiteral("outside.md"), "# Original\n"}},
                            "Default branch", "refs/heads/selected-branch"));
        QCOMPARE(git_repository_set_head(fixture, "refs/heads/default-branch"), 0);
        git_repository_free(fixture);
        int step = 0; bool previewCorrect = false; bool localBrowseAvailable = false;
        QTimer driver;
        connect(&driver, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            if (step == 0) {
                if (auto *button = dialog->findChild<QPushButton *>(QStringLiteral("importSourcesFromGit"))) { ++step; button->click(); }
            } else if (step == 1) {
                if (auto *url = dialog->findChild<QLineEdit *>(QStringLiteral("gitImportUrl"))) {
                    localBrowseAvailable = dialog->findChild<QPushButton *>(QStringLiteral("gitImportBrowse")) != nullptr;
                    url->setText(origin.path());
                    dialog->findChild<QLineEdit *>(QStringLiteral("gitImportBranch"))->setText(QStringLiteral("selected-branch"));
                    dialog->findChild<QLineEdit *>(QStringLiteral("gitImportSubfolder"))->setText(QStringLiteral("notes"));
                    ++step; dialog->accept();
                }
            } else if (step == 2) {
                if (auto *tree = dialog->findChild<QTreeWidget *>(QStringLiteral("markdownFolderImportPreview"))) {
                    previewCorrect = Thoughts::selectedMarkdownImportDocuments(tree) == QStringList{QStringLiteral("notes/chapter.md")};
                    ++step; dialog->accept();
                }
            }
        });
        driver.start(10);
        QTimer timeout; timeout.setSingleShot(true);
        connect(&timeout, &QTimer::timeout, this, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
        timeout.start(15000);
        const auto values = Thoughts::prepareSourcesImport(library.path(), nullptr);
        driver.stop(); timeout.stop();
        QCOMPARE(step, 2); QVERIFY(localBrowseAvailable); QVERIFY(!previewCorrect); QVERIFY(!values.isEmpty());
        Thoughts::LibraryProjectStore store(library.path());
        QVERIFY(store.enqueue(QStringLiteral("create"), {{QStringLiteral("id"), QStringLiteral("new:git")}}, {{QStringLiteral("name"), QStringLiteral("Git")}}).isEmpty());
        const auto error = store.enqueue(QStringLiteral("import_folder"), store.projects().first().toObject(), values);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto project = store.projects().first().toObject();
        const auto owner = project.value(QStringLiteral("source_folders")).toArray().first().toObject().value(QStringLiteral("project")).toString();
        const auto checkout = library.filePath(owner + QStringLiteral("/markdown/source"));
        QVERIFY(QFileInfo(QDir(checkout).filePath(QStringLiteral(".git"))).isDir());
        QVERIFY(QFileInfo::exists(QDir(checkout).filePath(QStringLiteral("outside.md"))));
        QCOMPARE(fileContents(QDir(checkout).filePath(QStringLiteral(".git/HEAD"))), QByteArray("ref: refs/heads/selected-branch\n"));
        QCOMPARE(originUrl(checkout), origin.path());
        QFile chapter(QDir(checkout).filePath(QStringLiteral("notes/chapter.md")));
        QVERIFY(chapter.open(QIODevice::ReadOnly)); QCOMPARE(chapter.readAll(), QByteArray("# Original\n")); chapter.close();
        QVERIFY(chapter.open(QIODevice::WriteOnly)); chapter.write("# Edited in Sources\n"); chapter.close();
        QVERIFY(store.prepareFolderUpload(store.pending().last().toObject().value(QStringLiteral("operation_id")).toString()).isEmpty());
        const auto op = store.pending().last().toObject();
        const auto upload = Thoughts::MarkdownFolderImport::savedBundle(op.value(QStringLiteral("local_path")).toString(), op.value(QStringLiteral("sha256")).toString());
        // Git checkout documents start unchecked; the editable file still uploads.
        QVERIFY(!upload.documents.contains(QStringLiteral("notes/chapter.md")));
        QCOMPARE(upload.files.value(QStringLiteral("notes/chapter.md")), QByteArray("# Edited in Sources\n"));
        QVERIFY(upload.files.contains(QStringLiteral("outside.md")));
        QVERIFY(QFileInfo(QDir(checkout).filePath(QStringLiteral(".git"))).isDir());
    }
    void gitLatexCheckoutPreservesFilesAndMetadata() {
        QTemporaryDir library;
        const auto repository = library.filePath(QStringLiteral(".thoughts/git-imports/fixture/repository"));
        const QMap<QString, QByteArray> files{
            {QStringLiteral(".git/HEAD"), QByteArrayLiteral("ref: refs/heads/main\n")},
            {QStringLiteral("thoughts-project.json"), QByteArrayLiteral("{\"original\":true}\n")},
            {QStringLiteral("tex/source/main.tex"), QByteArrayLiteral("\\documentclass{article}\n\\begin{document}Text.\\end{document}\n")},
            {QStringLiteral("unselected.txt"), QByteArrayLiteral("Retain the entire checkout.\n")}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const auto path = QDir(repository).filePath(it.key()); QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(it.value());
        }
        const auto snapshot = Thoughts::MarkdownFolderImport::scan(repository);
        QVERIFY(snapshot.latex); QVERIFY(snapshot.error.isEmpty());
        const auto bytes = snapshot.bundle(snapshot.documents);
        const auto path = library.filePath(QStringLiteral("bundle.json"));
        QFile bundle(path); QVERIFY(bundle.open(QIODevice::WriteOnly)); bundle.write(bytes); bundle.close();
        Thoughts::LibraryProjectStore store(library.path());
        QVERIFY(store.enqueue(QStringLiteral("create"), {{QStringLiteral("id"), QStringLiteral("new:git-latex")}}, {{QStringLiteral("name"), QStringLiteral("Paper")}}).isEmpty());
        const auto error = store.enqueue(QStringLiteral("import_folder"), store.projects().first().toObject(), {
            {QStringLiteral("git_checkout"), repository}, {QStringLiteral("local_path"), path},
            {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("relative_path"), QStringLiteral("tex/source/main.tex")}}}}});
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto project = store.projects().first().toObject();
        const auto owner = project.value(QStringLiteral("source_folders")).toArray().first().toObject().value(QStringLiteral("project")).toString();
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const auto relative = it.key() == QLatin1String("thoughts-project.json") ? QStringLiteral(".git/thoughts-import-originals/") + it.key() : it.key();
            QFile file(library.filePath(owner + QLatin1Char('/') + relative));
            QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), it.value());
        }
        QVERIFY(store.prepareFolderUpload(store.pending().last().toObject().value(QStringLiteral("operation_id")).toString()).isEmpty());
    }
    void latexCopyAndPendingUpload() {
        QTemporaryDir input, library;
        const QMap<QString, QByteArray> files{{QStringLiteral("tex/source/main.tex"), QByteArrayLiteral("\\documentclass{article}\n\\begin{document}Text.\\end{document}\n")},
            {QStringLiteral("tex/fragments/body.tex"), QByteArrayLiteral("Original fragment.\r\n")}, {QStringLiteral("tex/support/references.bib"), QByteArrayLiteral("@book{test,title={Test}}")},
            {QStringLiteral("tex/generated/figure.pdf"), QByteArrayLiteral("%PDF-example")}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) {
            const auto path = input.filePath(it.key()); QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
            QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(it.value());
        }
        const auto copy = Thoughts::MarkdownFolderImport::scan(input.path());
        QVERIFY2(copy.error.isEmpty(), qPrintable(copy.error)); QVERIFY(copy.latex);
        QCOMPARE(copy.documents, QStringList{QStringLiteral("tex/source/main.tex")});
        QCOMPARE(Thoughts::MarkdownFolderImport::scan(input.filePath(QStringLiteral("tex"))).files, files);
        QCOMPARE(copy.files, files);
        const auto raw = copy.bundle(copy.documents);
        const auto path = library.filePath(QStringLiteral("copy.json")); QFile bundle(path); QVERIFY(bundle.open(QIODevice::WriteOnly)); bundle.write(raw); bundle.close();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
        const auto saved = Thoughts::MarkdownFolderImport::savedBundle(path, sha);
        QVERIFY2(saved.error.isEmpty(), qPrintable(saved.error)); QVERIFY(saved.latex); QCOMPARE(saved.files, files);
        Thoughts::LibraryProjectStore store(library.path());
        QVERIFY(store.enqueue(QStringLiteral("create"), {{QStringLiteral("id"), QStringLiteral("new:latex")}, {QStringLiteral("revision"), 0}}, {{QStringLiteral("name"), QStringLiteral("Followup")}}).isEmpty());
        QVERIFY(store.enqueue(QStringLiteral("import_folder"), store.projects().first().toObject(), {{QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("local_path"), path}, {QStringLiteral("sha256"), sha},
            {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("main")}, {QStringLiteral("relative_path"), QStringLiteral("tex/source/main.tex")}}}}}).isEmpty());
        const auto project = store.projects().first().toObject();
        const auto owner = project.value(QStringLiteral("source_folders")).toArray().first().toObject().value(QStringLiteral("project")).toString();
        // Rule N: docs-<slug of the import name>-<6 hex of the operation id>.
        const auto importOperation = store.pending().last().toObject();
        QCOMPARE(owner, QStringLiteral("docs-paper-") + QUuid(importOperation.value(QStringLiteral("operation_id")).toString()).toString(QUuid::Id128).left(6));
        QCOMPARE(importOperation.value(QStringLiteral("source_project")).toString(), owner);
        const auto location = sourceLocation(project, owner);
        QCOMPARE(location.value(QStringLiteral("id")).toString(), owner);
        QSet<QString> shown;
        for (const auto &entry : location.value(QStringLiteral("files")).toArray()) shown.insert(entry.toObject().value(QStringLiteral("relative_path")).toString());
        QVERIFY(shown.contains(QStringLiteral(".ThoughtIgnore")));
        for (auto file = files.cbegin(); file != files.cend(); ++file) QVERIFY(shown.contains(file.key()));
        QFile source(library.filePath(owner + QStringLiteral("/tex/fragments/body.tex"))); QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), files.value(QStringLiteral("tex/fragments/body.tex"))); source.close();
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write(QByteArrayLiteral("Edited before Sync.\n")); source.close();
        const auto error = store.prepareFolderUpload(location.value(QStringLiteral("import_operation_id")).toString());
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto op = store.pending().last().toObject();
        const auto upload = Thoughts::MarkdownFolderImport::savedBundle(op.value(QStringLiteral("local_path")).toString(), op.value(QStringLiteral("sha256")).toString());
        QVERIFY(upload.latex); QCOMPARE(upload.files.value(QStringLiteral("tex/fragments/body.tex")), QByteArrayLiteral("Edited before Sync.\n"));
    }
    void latexOutputsAreValidLibraryFilenames() {
        Thoughts::MarkdownFolderImport import;
        import.latex = true;
        import.documents = {QStringLiteral("tex/source/main.tex"), QStringLiteral("tex/source/My Paper (final).tex")};
        const auto documents = import.projectDocuments(QStringLiteral("docs-paper"));
        QCOMPARE(documents.at(0).toObject().value(QStringLiteral("output")).toString(), QStringLiteral("paper_main.pdf"));
        QCOMPARE(documents.at(1).toObject().value(QStringLiteral("output")).toString(), QStringLiteral("paper_My_Paper_final.pdf"));
    }
    void capturedCopyAndLinks() {
        QTemporaryDir dir;
        QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("section")));
        QFile root(dir.filePath(QStringLiteral("README.md"))); QVERIFY(root.open(QIODevice::WriteOnly)); root.write("# Root\n\n[Section](section/)\n"); root.close();
        QFile note(dir.filePath(QStringLiteral("section/README.md"))); QVERIFY(note.open(QIODevice::WriteOnly)); note.write("# Section\n\nOriginal.\n"); note.close();
        const auto copy = Thoughts::MarkdownFolderImport::scan(dir.path());
        QVERIFY2(copy.error.isEmpty(), qPrintable(copy.error)); QCOMPARE(copy.documents.size(), 2);
        QVERIFY(copy.issues(QStringLiteral("README.md"), copy.documents).isEmpty());
        QVERIFY(!copy.issues(QStringLiteral("README.md"), {QStringLiteral("README.md")}).isEmpty());
        QVERIFY(note.open(QIODevice::WriteOnly)); note.write("Changed externally."); note.close();
        const auto data = QJsonDocument::fromJson(copy.bundle(copy.documents)).object();
        QCOMPARE(data.value(QStringLiteral("files")).toArray().size(), 2);
        QVERIFY(copy.files.value(QStringLiteral("section/README.md")).contains("Original."));
    }
    void sizesIncludeOnlySelectedImages() {
        QTemporaryDir dir; QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("section")));
        const QMap<QString, QByteArray> files{
            {QStringLiteral("section/a.md"), "# A\n\n![Shared][pic]\n\n[pic]: <../shared image.png>\n"},
            {QStringLiteral("section/b.md"), "# B\n\n[Shared](../shared%20image.png)\n"},
            {QStringLiteral("other.md"), "# Other\n\n![Own](own.png)\n"},
            {QStringLiteral("shared image.png"), QByteArray(100, 'x')},
            {QStringLiteral("own.png"), QByteArray(200, 'y')},
            {QStringLiteral("unused.png"), QByteArray(500, 'z')}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) { QFile f(dir.filePath(it.key())); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(it.value()), it.value().size()); }
        const auto copy = Thoughts::MarkdownFolderImport::scan(dir.path()); QVERIFY(copy.error.isEmpty());
        const QStringList section{QStringLiteral("section/a.md"), QStringLiteral("section/b.md")};
        const auto expected = files.value(section[0]).size() + files.value(section[1]).size() + 100;
        QCOMPARE(copy.selectionBytes(section), expected);
        QCOMPARE(copy.includedFiles(section).size(), 3);
        QTreeWidget tree; QLabel summary; QPushButton import;
        Thoughts::populateMarkdownImportPreview(copy, &tree, &summary, &import);
        QVERIFY(import.isEnabled());
        QTreeWidgetItem *other = nullptr; QTreeWidgetItem *group = nullptr;
        for (QTreeWidgetItemIterator it(&tree); *it; ++it) {
            if ((*it)->data(0, Qt::UserRole).toString() == QLatin1String("other.md")) other = *it;
            if ((*it)->text(0) == QLatin1String("section")) group = *it;
        }
        QVERIFY(other); QVERIFY(group);
        QCOMPARE(group->data(2, Qt::UserRole).toLongLong(), expected);
        other->setCheckState(0, Qt::Unchecked);
        QCOMPARE(summary.property("selectedBytes").toLongLong(), expected);
        QVERIFY(import.isEnabled());
        QFile changed(dir.filePath(QStringLiteral("shared image.png"))); QVERIFY(changed.open(QIODevice::WriteOnly)); changed.write("External change"); changed.close();
        const auto entries = QJsonDocument::fromJson(copy.bundle(section)).object().value(QStringLiteral("files")).toArray();
        QCOMPARE(entries.size(), 3);
        for (const auto &entry : entries) if (entry.toObject().value(QStringLiteral("path")).toString() == QLatin1String("shared image.png")) QCOMPARE(QByteArray::fromBase64(entry.toObject().value(QStringLiteral("content")).toString().toLatin1()), QByteArray(100, 'x'));
        group->setCheckState(0, Qt::Unchecked);
        QCOMPARE(summary.property("selectedBytes").toLongLong(), 0); QVERIFY(!import.isEnabled());
    }
    void oversizedFilesCanBeExcluded() {
        QTemporaryDir dir;
        const QMap<QString, QByteArray> notes{{QStringLiteral("small.md"), "# Small\n"}, {QStringLiteral("large-image.md"), "# Large image\n\n![Large](large.png)\n"}};
        for (auto it = notes.cbegin(); it != notes.cend(); ++it) { QFile f(dir.filePath(it.key())); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(it.value()); }
        QFile image(dir.filePath(QStringLiteral("large.png"))); QVERIFY(image.open(QIODevice::WriteOnly)); QVERIFY(image.resize(Thoughts::MarkdownFolderImport::ImageLimit + 1)); image.close();
        QFile huge(dir.filePath(QStringLiteral("large.md"))); QVERIFY(huge.open(QIODevice::WriteOnly)); QVERIFY(huge.resize(Thoughts::MarkdownFolderImport::DocumentLimit + 1)); huge.close();
        auto copy = Thoughts::MarkdownFolderImport::scan(dir.path()); QVERIFY(copy.error.isEmpty()); QCOMPARE(copy.documents.size(), 3);
        QVERIFY(!copy.issues(QStringLiteral("large.md"), copy.documents).isEmpty());
        QVERIFY(!copy.issues(QStringLiteral("large-image.md"), copy.documents).isEmpty());
        QVERIFY(!copy.bundle({QStringLiteral("small.md")}).isEmpty());
        QVERIFY(copy.bundle(copy.documents).isEmpty());
        QTreeWidget tree; QLabel summary; QPushButton import;
        Thoughts::populateMarkdownImportPreview(copy, &tree, &summary, &import); QVERIFY(!import.isEnabled());
        for (QTreeWidgetItemIterator it(&tree); *it; ++it) if ((*it)->data(0, Qt::UserRole).toString() != QLatin1String("small.md")) (*it)->setCheckState(0, Qt::Unchecked);
        QVERIFY(import.isEnabled()); QCOMPARE(summary.property("selectedBytes").toLongLong(), notes.value(QStringLiteral("small.md")).size());
        copy.sizes[QStringLiteral("small.md")] = Thoughts::MarkdownFolderImport::SelectionLimit + 1;
        QVERIFY(!copy.selectionIssues({QStringLiteral("small.md")}).isEmpty());
        QStringList many;
        for (int i = 0; i < 1001; ++i) { const auto path = QString::number(i) + QStringLiteral(".md"); many.append(path); copy.sizes[path] = 0; }
        QVERIFY(!copy.selectionIssues(many).isEmpty());
        QVERIFY(copy.selectionIssues(many.mid(0, 1000)).isEmpty());
    }
    void referenceImagesAndCode() {
        QTemporaryDir dir; QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("figures")));
        const QByteArray markdown = "# Links\n\n![Inline](figure(1).png)\n\n![Reference][ref]\n\n[ref]: <with space.png>\n\n[Folder](figures/)\n\n`![Code](ignored.png)`\n\n```md\n![Fenced](also-ignored.png)\n```\n";
        const QMap<QString, QByteArray> files{{QStringLiteral("a.md"), markdown}, {QStringLiteral("figure(1).png"), "a"}, {QStringLiteral("with space.png"), "bb"}, {QStringLiteral("figures/figure.png"), "ccc"}};
        for (auto it = files.cbegin(); it != files.cend(); ++it) { QFile f(dir.filePath(it.key())); QVERIFY(f.open(QIODevice::WriteOnly)); f.write(it.value()); }
        const auto copy = Thoughts::MarkdownFolderImport::scan(dir.path()); QVERIFY(copy.error.isEmpty());
        QVERIFY2(copy.issues(copy.documents.first(), copy.documents).isEmpty(), qPrintable(copy.issues(copy.documents.first(), copy.documents).join(QLatin1Char('\n'))));
        QCOMPARE(copy.selectionBytes(copy.documents), markdown.size() + 6);
        const auto entries = QJsonDocument::fromJson(copy.bundle(copy.documents)).object().value(QStringLiteral("files")).toArray();
        qint64 bytes = 0;
        for (const auto &entry : entries) bytes += QByteArray::fromBase64(entry.toObject().value(QStringLiteral("content")).toString().toLatin1()).size();
        QCOMPARE(bytes, copy.selectionBytes(copy.documents));
    }
    void realFolderPreview() {
        const auto directory = qEnvironmentVariable("THOUGHTS_FOLDER_SIZE_FIXTURE");
        if (directory.isEmpty()) QSKIP("Optional real-folder preview validation");
        const auto copy = Thoughts::MarkdownFolderImport::scan(directory); QVERIFY2(copy.error.isEmpty(), qPrintable(copy.error));
        QDialog dialog; dialog.resize(1100, 650); auto *layout = new QVBoxLayout(&dialog);
        auto *label = new QLabel(QStringLiteral("Select Markdown documents. Sizes include linked images; shared images count once in totals.")); layout->addWidget(label);
        auto *tree = new QTreeWidget; auto *summary = new QLabel; auto *import = new QPushButton(QStringLiteral("Import"));
        layout->addWidget(tree); layout->addWidget(summary); layout->addWidget(import);
        Thoughts::populateMarkdownImportPreview(copy, tree, summary, import);
        QVERIFY2(import->isEnabled(), qPrintable(summary->text()));
        const auto content = copy.bundle(copy.documents); QVERIFY(!content.isEmpty());
        const auto entries = QJsonDocument::fromJson(content).object().value(QStringLiteral("files")).toArray(); qint64 bytes = 0;
        for (const auto &entry : entries) bytes += QByteArray::fromBase64(entry.toObject().value(QStringLiteral("content")).toString().toLatin1()).size();
        QCOMPARE(bytes, summary->property("selectedBytes").toLongLong());
        qInfo() << "Documents:" << copy.documents.size() << "files including images:" << entries.size() << "selected bytes:" << bytes;
        const auto output = qEnvironmentVariable("THOUGHTS_FOLDER_SIZE_OUTPUT");
        if (!output.isEmpty()) { QFile file(output + QStringLiteral(".json")); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(content), content.size()); dialog.show(); QTest::qWait(100); QVERIFY(dialog.grab().save(output + QStringLiteral(".png"))); }
    }
    void pendingImportRetainsProjectIdentity() {
        QTemporaryDir dir; Thoughts::LibraryProjectStore store(dir.path());
        const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:lab")}, {QStringLiteral("revision"), 0}};
        QVERIFY(store.enqueue(QStringLiteral("create"), project, {{QStringLiteral("name"), QStringLiteral("Lab")}}).isEmpty());
        const auto current = store.projects().first().toObject();
        QJsonArray docs{QJsonObject{{QStringLiteral("name"), QStringLiteral("Intro")}, {QStringLiteral("relative_path"), QStringLiteral("motors/README.md")}}};
        const QByteArray raw = QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("documents"), QJsonArray{QStringLiteral("motors/README.md")}},
            {QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("motors/README.md")}, {QStringLiteral("content"), QString::fromLatin1(QByteArrayLiteral("# Original\n").toBase64())}}}}}).toJson();
        const auto bundlePath = dir.filePath(QStringLiteral("copy.json")); QFile bundle(bundlePath); QVERIFY(bundle.open(QIODevice::WriteOnly)); bundle.write(raw); bundle.close();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
        QVERIFY(store.enqueue(QStringLiteral("import_folder"), current, {{QStringLiteral("name"), QStringLiteral("Knowledge")}, {QStringLiteral("documents"), docs}, {QStringLiteral("local_path"), bundlePath}, {QStringLiteral("sha256"), sha}}).isEmpty());
        const auto visible = store.projects(); QCOMPARE(visible.size(), 1);
        const auto owner = visible.first().toObject(); QCOMPARE(owner.value(QStringLiteral("id")).toString(), QStringLiteral("new:lab"));
        QCOMPARE(owner.value(QStringLiteral("documents")).toArray().size(), 0);
        const auto location = sourceLocation(owner, owner.value(QStringLiteral("source_folders")).toArray().first().toObject().value(QStringLiteral("project")).toString());
        QJsonObject file;
        for (const auto &entry : location.value(QStringLiteral("files")).toArray())
            if (entry.toObject().value(QStringLiteral("id")).toString().endsWith(QStringLiteral("/motors/README.md"))) file = entry.toObject();
        QVERIFY(!file.isEmpty());
        const auto localPath = QDir(dir.path()).filePath(file.value(QStringLiteral("id")).toString());
        QFile source(localPath); QVERIFY(source.open(QIODevice::WriteOnly)); source.write("# Edited before Sync\n"); source.close();
        const auto operationId = location.value(QStringLiteral("import_operation_id")).toString();
        QVERIFY(store.prepareFolderUpload(operationId).isEmpty());
        auto operation = store.pending().last().toObject();
        auto upload = Thoughts::MarkdownFolderImport::savedBundle(operation.value(QStringLiteral("local_path")).toString(), operation.value(QStringLiteral("sha256")).toString());
        QCOMPARE(upload.files.value(QStringLiteral("motors/README.md")), QByteArrayLiteral("# Edited before Sync\n"));
        const auto frozenSha = operation.value(QStringLiteral("sha256"));
        QVERIFY(source.open(QIODevice::WriteOnly)); source.write("# Edited during Sync\n"); source.close();
        QVERIFY(store.prepareFolderUpload(operationId).isEmpty());
        QCOMPARE(store.pending().last().toObject().value(QStringLiteral("sha256")), frozenSha);
        QVERIFY(store.releaseRejectedFolderUpload(operationId).isEmpty());
        QVERIFY(store.prepareFolderUpload(operationId).isEmpty());
        QVERIFY(store.pending().last().toObject().value(QStringLiteral("sha256")) != frozenSha);
        QCOMPARE(store.projects().first().toObject().value(QStringLiteral("documents")).toArray().size(), 0);
        QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), QByteArrayLiteral("# Edited during Sync\n")); source.close();
        QVERIFY(store.cancelFolderImport(operationId).isEmpty());
        QVERIFY(sourceLocation(store.projects().first().toObject(), location.value(QStringLiteral("id")).toString()).isEmpty());
        QVERIFY(QFileInfo::exists(localPath)); // Cancellation must not delete edited source files.
        QCOMPARE(store.pending().size(), 1);
    }
    // Source folder names spec: an import folder is named by rule N when it
    // is queued, follows Fedora when Fedora refuses or renames it, and an
    // import queued before the change gets its rule-N name before upload.
    void importFolderNamesFollowRuleN() {
        QTemporaryDir dir; Thoughts::LibraryProjectStore store(dir.path());
        const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:names")}, {QStringLiteral("revision"), 0}};
        QVERIFY(store.enqueue(QStringLiteral("create"), project, {{QStringLiteral("name"), QStringLiteral("Names")}}).isEmpty());
        const QByteArray raw = QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("documents"), QJsonArray{QStringLiteral("notes/README.md")}},
            {QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("notes/README.md")}, {QStringLiteral("content"), QString::fromLatin1(QByteArrayLiteral("# Notes\n").toBase64())}}}}}).toJson();
        const auto bundlePath = dir.filePath(QStringLiteral("copy.json")); QFile bundle(bundlePath); QVERIFY(bundle.open(QIODevice::WriteOnly)); bundle.write(raw); bundle.close();
        const auto sha = QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
        const QJsonArray docs{QJsonObject{{QStringLiteral("name"), QStringLiteral("Notes")}, {QStringLiteral("relative_path"), QStringLiteral("notes/README.md")}}};
        auto pendingImport = [&store] { for (const auto &v : store.pending()) if (v.toObject().value(QStringLiteral("action")).toString() == QLatin1String("import_folder")) return v.toObject(); return QJsonObject(); };
        QVERIFY(store.enqueue(QStringLiteral("import_folder"), store.projects().first().toObject(), {{QStringLiteral("name"), QStringLiteral("Lab meeting notes!")}, {QStringLiteral("documents"), docs}, {QStringLiteral("local_path"), bundlePath}, {QStringLiteral("sha256"), sha}}).isEmpty());
        QCOMPARE(store.projects().size(), 1); // Shows (and materializes) the pending import.
        const auto operationId = pendingImport().value(QStringLiteral("operation_id")).toString();
        const auto hex = QUuid(operationId).toString(QUuid::Id128);
        const QString first = QStringLiteral("docs-lab-meeting-notes-") + hex.left(6);
        QCOMPARE(pendingImport().value(QStringLiteral("source_project")).toString(), first);
        QVERIFY(QFileInfo(dir.filePath(first + QStringLiteral("/.thoughts-import.json"))).isFile());
        QCOMPARE(Thoughts::LibraryProjectStore::sourceProject(pendingImport()), first);

        // Fedora refuses that name (409 source_folder_exists): the next one is taken locally.
        QString renamed;
        QVERIFY(store.renameRefusedImportFolder(operationId, first, &renamed).isEmpty());
        const QString second = QStringLiteral("docs-lab-meeting-notes-") + hex.left(8);
        QCOMPARE(renamed, second);
        QCOMPARE(pendingImport().value(QStringLiteral("source_project")).toString(), second);
        QVERIFY(!QFileInfo::exists(dir.filePath(first)));
        QVERIFY(QFileInfo(dir.filePath(second + QStringLiteral("/markdown/source/notes/README.md"))).isFile());

        // Fedora (before rule N) created docs-import-<32 hex>: this computer follows.
        const QString legacy = QStringLiteral("docs-import-") + hex;
        QJsonObject declared; declared.insert(QStringLiteral("project"), legacy); declared.insert(QStringLiteral("folder"), QStringLiteral("Lab meeting notes!"));
        QJsonObject fedoraProject; fedoraProject.insert(QStringLiteral("id"), QStringLiteral("new:names")); fedoraProject.insert(QStringLiteral("source_folders"), QJsonArray{declared});
        QJsonObject snapshot; snapshot.insert(QStringLiteral("projects"), QJsonArray{fedoraProject});
        QVERIFY(store.adoptImportFolderName(snapshot, pendingImport()).isEmpty());
        QCOMPARE(pendingImport().value(QStringLiteral("source_project")).toString(), legacy);
        QVERIFY(QFileInfo(dir.filePath(legacy + QStringLiteral("/.thoughts-import.json"))).isFile());
        // Fedora declaring the name that was sent changes nothing.
        QVERIFY(store.adoptImportFolderName(snapshot, pendingImport()).isEmpty());
        QCOMPARE(pendingImport().value(QStringLiteral("source_project")).toString(), legacy);

        // An import queued by an older Thoughts (no source_project) is named before its upload.
        QFile catalogue(store.path()); QVERIFY(catalogue.open(QIODevice::ReadOnly));
        auto state = QJsonDocument::fromJson(catalogue.readAll()).object(); catalogue.close();
        auto pending = state.value(QStringLiteral("pending")).toArray();
        for (int i = 0; i < pending.size(); ++i) { auto op = pending[i].toObject(); op.remove(QStringLiteral("source_project")); pending[i] = op; }
        state.insert(QStringLiteral("pending"), pending);
        QVERIFY(catalogue.open(QIODevice::WriteOnly | QIODevice::Truncate)); catalogue.write(QJsonDocument(state).toJson()); catalogue.close();
        QCOMPARE(Thoughts::LibraryProjectStore::sourceProject(pendingImport()), legacy);
        QVERIFY(store.prepareFolderUpload(operationId).isEmpty());
        QCOMPARE(pendingImport().value(QStringLiteral("source_project")).toString(), first);
        QVERIFY(!QFileInfo::exists(dir.filePath(legacy)));
        QVERIFY(QFileInfo(dir.filePath(first + QStringLiteral("/markdown/source/notes/README.md"))).isFile());

        // Cancel finds the folder by the operation, whatever its name.
        QVERIFY(store.cancelFolderImport(operationId).isEmpty());
        QVERIFY(pendingImport().isEmpty());
        QVERIFY(QFileInfo::exists(dir.filePath(first))); // Cancellation never deletes source files.
    }

};
QTEST_MAIN(MarkdownFolderImportTest)
#include "markdownfolderimporttest.moc"
