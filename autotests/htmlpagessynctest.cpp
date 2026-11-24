/* SPDX-License-Identifier: GPL-2.0-or-later */
// A docs folder's HTML pages kept in step with Fedora's page list
// (tools/publisher/htmlpages.cpp, thoughts-server protocol/html-pages.md 4).
#undef QT_NO_CAST_FROM_ASCII
#undef QT_NO_CAST_TO_ASCII
#include "../tools/publisher/htmlpages.h"
#include "../tools/publisher/sourceapi.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <functional>
#include <utility>

using namespace Thoughts::Publisher;

namespace
{
QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// Fedora's objects and page list, with the compare-and-swap of POST /v1/html-pages.
class FakePages : public QObject
{
public:
    FakePages()
    {
        m_server.listen(QHostAddress::LocalHost);
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = m_server.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] { handle(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QString url() const { return QStringLiteral("http://127.0.0.1:%1").arg(m_server.serverPort()); }
    void put(const QString &path, const QByteArray &content)
    {
        objects.insert(sha(content), content);
        pages.insert(path, sha(content));
    }

    bool supported = true;
    std::function<void()> raceAfterListing;
    QHash<QString, QByteArray> objects;
    QMap<QString, QString> pages; // path -> sha256
    QList<QJsonObject> changes;

private:
    void handle(QTcpSocket *socket)
    {
        QByteArray &buffer = m_buffers[socket];
        buffer += socket->readAll();
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) return;
        const QList<QByteArray> lines = buffer.left(headerEnd).split('\n');
        const QList<QByteArray> requestLine = lines.value(0).trimmed().split(' ');
        qsizetype length = 0;
        for (const QByteArray &line : lines) {
            if (line.toLower().startsWith("content-length:")) length = line.mid(15).trimmed().toLongLong();
        }
        if (buffer.size() < headerEnd + 4 + length) return;
        const QByteArray body = buffer.mid(headerEnd + 4, length);
        m_buffers.remove(socket);
        const QByteArray method = requestLine.value(0);
        const QString path = QString::fromLatin1(requestLine.value(1));
        int status = 200;
        QByteArray response;
        const auto json = [](const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Compact); };
        if (path == QLatin1String("/v1/html-pages") && !supported) {
            status = 404;
            response = "{\"detail\":\"Not Found\"}";
        } else if (method == "GET" && path == QLatin1String("/v1/html-pages")) {
            QJsonArray listed;
            for (auto it = pages.cbegin(); it != pages.cend(); ++it)
                listed.append(QJsonObject{{"path", it.key()}, {"folder", it.key().section('/', 0, 0)}, {"sha256", it.value()},
                                          {"size_bytes", objects.value(it.value()).size()}});
            response = json({{"pages", listed}});
            if (raceAfterListing) std::exchange(raceAfterListing, nullptr)();
        } else if (method == "POST" && path == QLatin1String("/v1/html-pages")) {
            const QJsonObject request = QJsonDocument::fromJson(body).object();
            changes.append(request);
            QMap<QString, QString> next = pages;
            for (const QJsonValue &value : request.value("changes").toArray()) {
                const QJsonObject change = value.toObject();
                const QString page = change.value("path").toString();
                const QJsonValue wanted = change.value("sha256");
                const QString current = next.value(page);
                if ((wanted.isNull() && current.isEmpty()) || current == wanted.toString()) continue;
                if (current != change.value("base_sha256").toString()) {
                    status = 409;
                    QJsonObject refusal;
                    refusal.insert(QStringLiteral("detail"), QString(page + QStringLiteral(" changed on another device")));
                    refusal.insert(QStringLiteral("error_code"), QStringLiteral("html_page_changed"));
                    refusal.insert(QStringLiteral("path"), page);
                    response = json(refusal);
                    break;
                }
                if (wanted.isNull()) next.remove(page);
                else next.insert(page, wanted.toString());
            }
            if (status == 200) {
                pages = next;
                response = json({{"folder", request.value("folder")}, {"changed", QJsonArray()}, {"pages", QJsonArray()}});
            }
        } else if (method == "POST" && path == QLatin1String("/v1/objects")) {
            objects.insert(sha(body), body);
            response = json({{"sha256", sha(body)}});
        } else if (method == "GET" && path.startsWith(QLatin1String("/v1/objects/"))) {
            status = objects.contains(path.mid(12)) ? 200 : 404;
            response = objects.value(path.mid(12));
        } else {
            status = 404;
            response = "{\"detail\":\"not found\"}";
        }
        QByteArray reply = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Error")
            + "\r\nContent-Type: application/json\r\nContent-Length: " + QByteArray::number(response.size()) + "\r\nConnection: close\r\n\r\n" + response;
        socket->write(reply);
        socket->disconnectFromHost();
    }

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
};

const QString folder = QStringLiteral("docs-concepts");
const QString page = QStringLiteral("docs-concepts/tex/support/html/irt-middle-slope.html");
const QString other = QStringLiteral("docs-concepts/tex/support/html/sram-cell.html");
}

class HtmlPagesSyncTest : public QObject
{
    Q_OBJECT

    QTemporaryDir m_directory;
    QString m_root;

    QString file(const QString &relative) const { return m_root + QLatin1Char('/') + relative; }
    void write(const QString &relative, const QByteArray &content)
    {
        QVERIFY(QDir().mkpath(QFileInfo(file(relative)).path()));
        QFile output(file(relative));
        QVERIFY(output.open(QIODevice::WriteOnly));
        output.write(content);
    }
    QByteArray read(const QString &relative) const
    {
        QFile input(file(relative));
        return input.open(QIODevice::ReadOnly) ? input.readAll() : QByteArray();
    }
    HtmlPageSync sync(FakePages &fedora, const QString &which = folder)
    {
        SourceApi api(fedora.url(), QStringLiteral("token"));
        return syncHtmlPages(api, Thoughts::Py::Path(m_root), Thoughts::Py::Path(m_root + QStringLiteral("/state/source-state.json")), {which},
                             QStringLiteral("20261109T120000Z"));
    }

private Q_SLOTS:
    void init()
    {
        QVERIFY(m_directory.isValid());
        static int run = 0;
        m_root = m_directory.path() + QStringLiteral("/run-") + QString::number(++run);
        QVERIFY(QDir().mkpath(file(folder + QStringLiteral("/tex/source"))));
    }

    void pathsAndFolders()
    {
        QVERIFY(isHtmlPagePath(page));
        QVERIFY(!isHtmlPagePath(QStringLiteral("docs-concepts/tex/support/html/sub/a.html")));
        QVERIFY(!isHtmlPagePath(QStringLiteral("docs-concepts/tex/support/html/.a.html")));
        QVERIFY(!isHtmlPagePath(QStringLiteral(".thoughts/projects/x/default/tex/support/html/a.html")));
        const QString project = QStringLiteral(".thoughts/projects/") + QString(64, QLatin1Char('a')) + QStringLiteral("/default");
        QVERIFY(isHtmlPagePath(project + QStringLiteral("/tex/support/html/a.html")));
        QVERIFY(!isHtmlPagePath(project + QStringLiteral("/tex/support/html/sub/a.html")));
        QVERIFY(!isHtmlPagePath(project + QStringLiteral("/a.html")));
        QCOMPARE(htmlPageFolders({QStringLiteral("docs-b/tex/a.tex"), QStringLiteral("docs-a/x.md"), QStringLiteral(".thoughts/projects/p/default/a.md"),
                                  project + QStringLiteral("/note.tex"), QStringLiteral("docs-a/y.md")}),
                 QStringList({project, QStringLiteral("docs-a"), QStringLiteral("docs-b")}));
    }

    // 24 Nov 2026: a Project's default folder has pages too (a prompts page beside its note).
    void aDefaultFolderPageIsInstalledAndSent()
    {
        const QString project = QStringLiteral(".thoughts/projects/") + QString(64, QLatin1Char('c')) + QStringLiteral("/default");
        const QString fromFedora = project + QStringLiteral("/tex/support/html/prompts.html");
        const QString made = project + QStringLiteral("/tex/support/html/local.html");
        write(project + QStringLiteral("/notes.tex"), "\\documentclass{article}");
        write(made, "<!doctype html><p>made here");
        FakePages fedora;
        fedora.put(fromFedora, "<!doctype html><p>prompts");
        fedora.put(page, "<!doctype html><p>another folder");
        const HtmlPageSync report = sync(fedora, project);
        QVERIFY2(report.errors.isEmpty(), qPrintable(report.errors.join(QLatin1Char('\n'))));
        QCOMPARE(report.installed, QStringList{fromFedora});
        QCOMPARE(read(fromFedora), QByteArray("<!doctype html><p>prompts"));
        QCOMPARE(report.uploaded, QStringList{made});
        QCOMPARE(fedora.changes.size(), 1);
        QCOMPARE(read(page), QByteArray()); // another folder's page stays where it is
        QCOMPARE(listedHtmlPages(Thoughts::Py::Path(m_root), project), QStringList());
    }

    void aServerWithoutPagesIsLeftAlone()
    {
        FakePages fedora;
        fedora.supported = false;
        write(page, "<!doctype html><p>local");
        const HtmlPageSync report = sync(fedora);
        QVERIFY(!report.supported);
        QVERIFY(report.errors.isEmpty() && !report.changed());
        QVERIFY(fedora.changes.isEmpty());
    }

    // The agent on Fedora added a page; the Mac installs it into the folder.
    void aPageFedoraAddedIsInstalled()
    {
        FakePages fedora;
        fedora.put(page, "<!doctype html><p>from Fedora");
        const HtmlPageSync report = sync(fedora);
        QCOMPARE(report.installed, QStringList{page});
        QCOMPARE(read(page), QByteArray("<!doctype html><p>from Fedora"));
        QVERIFY(fedora.changes.isEmpty());
        // Nothing to do the next time.
        QVERIFY(!sync(fedora).changed());
    }

    // Pages made on this Mac (the ones that today only travel as build inputs) join the list in one request.
    void aPageIsNeverWrittenThroughASymbolicLink()
    {
        // Somewhere outside the Library that a link inside the folder points to.
        const QString outside = m_root + QStringLiteral("-outside");
        QVERIFY(QDir().mkpath(outside + QStringLiteral("/html")));
        QVERIFY(QDir().mkpath(outside + QStringLiteral("/support/html")));
        const auto listing = [](const QString &directory) { return QDir(directory).entryList(QDir::Files | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot); };
        FakePages fedora;
        fedora.put(page, "<p>from the server</p>");
        const auto refused = [&] {
            const HtmlPageSync result = sync(fedora);
            QVERIFY(result.installed.isEmpty());
            QCOMPARE(result.errors.size(), 1);
            QVERIFY2(result.errors.first().startsWith(page + QStringLiteral(": not installed")), qPrintable(result.errors.first()));
            QVERIFY(listing(outside + QStringLiteral("/html")).isEmpty());
            QVERIFY(listing(outside + QStringLiteral("/support/html")).isEmpty());
            QVERIFY(listing(outside).isEmpty());
        };
        // The page directory itself is a link.
        QVERIFY(QDir().mkpath(file(folder + QStringLiteral("/tex/support"))));
        QVERIFY(QFile::link(outside + QStringLiteral("/html"), file(folder + QStringLiteral("/tex/support/html"))));
        refused();
        // A directory above it is one.
        QVERIFY(QFile::remove(file(folder + QStringLiteral("/tex/support/html"))));
        QVERIFY(QDir().rmdir(file(folder + QStringLiteral("/tex/support"))));
        QVERIFY(QFile::link(outside + QStringLiteral("/support"), file(folder + QStringLiteral("/tex/support"))));
        refused();
        // The page's own name is one (dangling, so nothing "exists" there yet).
        QVERIFY(QFile::remove(file(folder + QStringLiteral("/tex/support"))));
        QVERIFY(QDir().mkpath(file(folder + QStringLiteral("/tex/support/html"))));
        QVERIFY(QFile::link(outside + QStringLiteral("/planted.html"), file(page)));
        refused();
        QVERIFY(!QFileInfo::exists(outside + QStringLiteral("/planted.html")));
        // Without links the same page is installed where it belongs.
        QVERIFY(QFile::remove(file(page)));
        const HtmlPageSync result = sync(fedora);
        QCOMPARE(result.installed, QStringList{page});
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(read(page), QByteArray("<p>from the server</p>"));
        QDir(outside).removeRecursively();
    }

    void localPagesAreAddedInOneRequest()
    {
        FakePages fedora;
        write(page, "<!doctype html><p>one");
        write(other, "<!doctype html><p>two");
        write(folder + QStringLiteral("/tex/support/html/bad name.html"), "<p>not a page name");
        write(folder + QStringLiteral("/tex/support/html/notes.txt"), "not a page");
        const HtmlPageSync report = sync(fedora);
        QCOMPARE(report.uploaded, QStringList({page, other}));
        QCOMPARE(fedora.changes.size(), 1);
        const QJsonArray changes = fedora.changes.first().value("changes").toArray();
        QCOMPARE(changes.size(), 2);
        QVERIFY(changes.first().toObject().value("base_sha256").isNull());
        QCOMPARE(fedora.pages.value(page), sha("<!doctype html><p>one"));
        QCOMPARE(fedora.objects.value(sha("<!doctype html><p>two")), QByteArray("<!doctype html><p>two"));
        QVERIFY(!sync(fedora).changed());
    }

    void changesTravelTheWayTheyWereMade()
    {
        FakePages fedora;
        fedora.put(page, "<!doctype html><p>v1");
        sync(fedora);
        // Changed here: replaced on Fedora, naming the version it replaces.
        write(page, "<!doctype html><p>v2 here");
        HtmlPageSync report = sync(fedora);
        QCOMPARE(report.uploaded, QStringList{page});
        QCOMPARE(fedora.changes.last().value("changes").toArray().first().toObject().value("base_sha256").toString(), sha("<!doctype html><p>v1"));
        // Changed on Fedora: installed here.
        fedora.put(page, "<!doctype html><p>v3 there");
        report = sync(fedora);
        QCOMPARE(report.installed, QStringList{page});
        QCOMPARE(read(page), QByteArray("<!doctype html><p>v3 there"));
        // Deleted here: removed on Fedora.
        QVERIFY(QFile::remove(file(page)));
        report = sync(fedora);
        QCOMPARE(report.removed, QStringList{page});
        QVERIFY(!fedora.pages.contains(page));
        QVERIFY(!QFileInfo::exists(file(page)));
    }

    void bothChangedKeepsTheLocalPageAside()
    {
        FakePages fedora;
        fedora.put(page, "<!doctype html><p>v1");
        sync(fedora);
        write(page, "<!doctype html><p>mine");
        fedora.put(page, "<!doctype html><p>theirs");
        const HtmlPageSync report = sync(fedora);
        QCOMPARE(report.conflicts, QStringList{page});
        QCOMPARE(read(page), QByteArray("<!doctype html><p>theirs"));
        QCOMPARE(read(QStringLiteral(".thoughts/html-page-conflicts/20261109T120000Z/") + page), QByteArray("<!doctype html><p>mine"));
        QVERIFY(fedora.changes.isEmpty());
    }

    // A page this Mac has never agreed on and that differs from Fedora's is a conflict too: nothing is lost.
    void aFirstSyncWithDifferentPagesKeepsBoth()
    {
        FakePages fedora;
        write(page, "<!doctype html><p>old local");
        fedora.put(page, "<!doctype html><p>agent's replacement");
        const HtmlPageSync report = sync(fedora);
        QCOMPARE(report.conflicts, QStringList{page});
        QCOMPARE(read(page), QByteArray("<!doctype html><p>agent's replacement"));
    }

    // Another device replaces the page between the listing and the change: Fedora refuses, nothing is
    // recorded, and the next Sync sees both changes.
    void aRefusedChangeIsDecidedAgainNextTime()
    {
        FakePages fedora;
        fedora.put(page, "<!doctype html><p>v1");
        sync(fedora);
        write(page, "<!doctype html><p>mine");
        fedora.raceAfterListing = [&fedora] { fedora.put(page, "<!doctype html><p>raced"); };
        HtmlPageSync report = sync(fedora);
        QVERIFY(report.uploaded.isEmpty());
        QCOMPARE(report.errors.size(), 1);
        QVERIFY2(report.errors.first().contains(QStringLiteral("html_page_changed")), qPrintable(report.errors.first()));
        QCOMPARE(fedora.pages.value(page), sha("<!doctype html><p>raced"));
        report = sync(fedora);
        QCOMPARE(report.conflicts, QStringList{page});
        QCOMPARE(read(page), QByteArray("<!doctype html><p>raced"));
        QCOMPARE(read(QStringLiteral(".thoughts/html-page-conflicts/20261109T120000Z/") + page), QByteArray("<!doctype html><p>mine"));
    }
};

QTEST_GUILESS_MAIN(HtmlPagesSyncTest)

#include "htmlpagessynctest.moc"
