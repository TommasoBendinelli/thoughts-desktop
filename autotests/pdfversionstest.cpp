// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/documentidentity.h"
#include "../part/pdfversions.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

using namespace Thoughts;

namespace
{
QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray pdfBytes(const QString &name)
{
    QFile file(QStringLiteral(KDESRCDIR "data/") + name);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

// GET /v1/objects/<sha256>: the objects it has, else 404. Counts requests.
class ObjectServer : public QTcpServer
{
public:
    QHash<QString, QByteArray> objects;
    QStringList requests;

    ObjectServer()
    {
        listen(QHostAddress::LocalHost);
        connect(this, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                    const QByteArray request = socket->readAll();
                    const QByteArray target = request.split(' ').value(1);
                    requests.append(QString::fromLatin1(target));
                    const QString digest = QString::fromLatin1(target).section(QLatin1Char('/'), -1);
                    QByteArray response;
                    if (target.startsWith("/v1/objects/") && objects.contains(digest)) {
                        const QByteArray body = objects.value(digest);
                        response = "HTTP/1.1 200 OK\r\nContent-Type: application/pdf\r\nContent-Length: " + QByteArray::number(body.size())
                            + "\r\nConnection: close\r\n\r\n" + body;
                    } else {
                        response = "HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 21\r\nConnection: close\r\n\r\n{\"detail\":\"missing\"}\n";
                    }
                    socket->write(response);
                    socket->disconnectFromHost();
                });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }
    QUrl url(const QString &digest) const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1/v1/objects/%2").arg(serverPort()).arg(digest));
    }
};

PdfVersions::Result fetchNow(PdfVersions::Fetcher &fetcher, const QString &digest)
{
    PdfVersions::Result result;
    bool done = false;
    fetcher.fetch(digest, &fetcher, [&](const PdfVersions::Result &value) {
        result = value;
        done = true;
    });
    if (!QTest::qWaitFor([&] { return done; }, 20000)) result.detail = QStringLiteral("timed out");
    return result;
}
}

class PdfVersionsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void recordedVersion();
    void cachedVersionPaths();
    void storeVerifiesAndPrunes();
    void fetchUsesCacheAndLocalCopies();
    void fetchDownloadsAndVerifies();
    void fetchFailures();
    void cachedVersionIsNotALibraryDocument();
};

void PdfVersionsTest::initTestCase()
{
    // The default cache is written below: never the real application data.
    QStandardPaths::setTestModeEnabled(true);
}

void PdfVersionsTest::recordedVersion()
{
    const QString stamped(64, QLatin1Char('a')), anchored(64, QLatin1Char('b'));
    const QJsonObject anchor{{QStringLiteral("pdf"), QJsonObject{{QStringLiteral("pdf_sha256"), anchored}}}};
    // A review event (or a re-stamped edit) names its version; answer feedback also by its anchor.
    QCOMPARE(PdfVersions::recordedSha256({{QStringLiteral("review_version"), QJsonObject{{QStringLiteral("pdf_sha256"), stamped}, {QStringLiteral("source_built"), true}}}}), stamped);
    QCOMPARE(PdfVersions::recordedSha256({{QStringLiteral("review_version"), QJsonObject{{QStringLiteral("pdf_sha256"), stamped}}}, {QStringLiteral("anchor"), anchor}}), stamped);
    QCOMPARE(PdfVersions::recordedSha256({{QStringLiteral("anchor"), anchor}}), anchored);
    // Older events have none; a malformed hash is none.
    QVERIFY(PdfVersions::recordedSha256({{QStringLiteral("rating"), QStringLiteral("good")}}).isEmpty());
    QVERIFY(PdfVersions::recordedSha256({{QStringLiteral("review_version"), QJsonObject{{QStringLiteral("pdf_sha256"), QStringLiteral("ABC")}}}}).isEmpty());
    QVERIFY(PdfVersions::recordedSha256({{QStringLiteral("review_version"), QJsonObject{{QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('A'))}}}}).isEmpty());
}

void PdfVersionsTest::cachedVersionPaths()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("pdf-versions"));
    const QString digest(64, QLatin1Char('c'));
    QVERIFY(PdfVersions::isCachedVersion(PdfVersions::cachePath(root, digest), root));
    QVERIFY(!PdfVersions::isCachedVersion(QDir(root).filePath(QStringLiteral("paper.pdf")), root));
    QVERIFY(!PdfVersions::isCachedVersion(QDir(root).filePath(QStringLiteral("nested/") + digest + QStringLiteral(".pdf")), root));
    QVERIFY(!PdfVersions::isCachedVersion(temporary.filePath(digest + QStringLiteral(".pdf")), root));
    QVERIFY(!PdfVersions::isCachedVersion(QString(), root));
    // The application's own cache by default.
    QVERIFY(PdfVersions::isCachedVersion(PdfVersions::cachePath(PdfVersions::defaultCacheRoot(), digest)));
    QVERIFY(!PdfVersions::isCachedVersion(PdfVersions::cachePath(root, digest)));
}

void PdfVersionsTest::storeVerifiesAndPrunes()
{
    QTemporaryDir temporary;
    const QString root = temporary.filePath(QStringLiteral("pdf-versions"));
    const QByteArray bytes = pdfBytes(QStringLiteral("file1.pdf"));
    QVERIFY(!bytes.isEmpty());
    QString error;
    // Bytes that are not the version are never cached.
    QVERIFY(PdfVersions::store(root, sha256(bytes + "x"), bytes, &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(!QFileInfo::exists(PdfVersions::cachePath(root, sha256(bytes + "x"))));
    const QString path = PdfVersions::store(root, sha256(bytes), bytes, &error);
    QCOMPARE(path, PdfVersions::cachePath(root, sha256(bytes)));
    QFile stored(path);
    QVERIFY(stored.open(QIODevice::ReadOnly));
    QCOMPARE(stored.readAll(), bytes);
    PdfVersions::setTitle(path, QStringLiteral("Paper — version of 2026-10-22 16:06"));
    QCOMPARE(PdfVersions::title(path), QStringLiteral("Paper — version of 2026-10-22 16:06"));
    // At most maximumCachedVersions stay.
    for (int i = 0; i < PdfVersions::maximumCachedVersions + 3; ++i) {
        const QByteArray other = "%PDF-1.4 " + QByteArray::number(i);
        QVERIFY(!PdfVersions::store(root, sha256(other), other).isEmpty());
    }
    QCOMPARE(QDir(root).entryList({QStringLiteral("*.pdf")}, QDir::Files).size(), PdfVersions::maximumCachedVersions);
}

void PdfVersionsTest::fetchUsesCacheAndLocalCopies()
{
    QTemporaryDir temporary;
    const QString docsRoot = temporary.filePath(QStringLiteral("docs"));
    const QString cacheRoot = temporary.filePath(QStringLiteral("cache"));
    const QByteArray current = pdfBytes(QStringLiteral("file1.pdf")), earlier = pdfBytes(QStringLiteral("file2.pdf"));
    // An earlier Library generation kept next to the current one lists the earlier PDF.
    const QString stage = QDir(docsRoot).filePath(QStringLiteral(".thoughts-library-stage-") + QString(64, QLatin1Char('1')));
    QVERIFY(writeFile(stage + QStringLiteral("/paper.pdf"), earlier));
    QVERIFY(writeFile(stage + QStringLiteral("/.thoughts-library-index.json"),
                      QJsonDocument(QJsonObject{{QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                                                                              {QStringLiteral("pdf_sha256"), sha256(earlier)}}}}}).toJson()));
    QCOMPARE(PdfVersions::localCopy(docsRoot, sha256(earlier)), stage + QStringLiteral("/paper.pdf"));
    QVERIFY(PdfVersions::localCopy(docsRoot, sha256(current)).isEmpty());

    int downloads = 0;
    PdfVersions::Fetcher fetcher(docsRoot, cacheRoot, [&](const QString &, PdfVersions::Failure *failure) -> QNetworkReply * {
        ++downloads;
        *failure = PdfVersions::Failure::NotConnected;
        return nullptr;
    });
    auto result = fetchNow(fetcher, sha256(earlier));
    QCOMPARE(int(result.failure), int(PdfVersions::Failure::None));
    QCOMPARE(result.path, PdfVersions::cachePath(cacheRoot, sha256(earlier)));
    QCOMPARE(downloads, 0);
    // Gone from the Library folder: the cache has it.
    QVERIFY(QDir(stage).removeRecursively());
    result = fetchNow(fetcher, sha256(earlier));
    QCOMPARE(result.path, PdfVersions::cachePath(cacheRoot, sha256(earlier)));
    QCOMPARE(downloads, 0);
    // A damaged cache file is not used.
    QVERIFY(writeFile(PdfVersions::cachePath(cacheRoot, sha256(earlier)), "%PDF-damaged"));
    result = fetchNow(fetcher, sha256(earlier));
    QCOMPARE(int(result.failure), int(PdfVersions::Failure::NotConnected));
    QVERIFY(result.path.isEmpty());
    QCOMPARE(downloads, 1);
    QVERIFY(!QFileInfo::exists(PdfVersions::cachePath(cacheRoot, sha256(earlier))));
}

void PdfVersionsTest::fetchDownloadsAndVerifies()
{
    QTemporaryDir temporary;
    ObjectServer server;
    QVERIFY(server.isListening());
    const QByteArray earlier = pdfBytes(QStringLiteral("file2.pdf"));
    const QString digest = sha256(earlier);
    server.objects.insert(digest, earlier);
    QNetworkAccessManager network;
    PdfVersions::Fetcher fetcher(temporary.filePath(QStringLiteral("docs")), temporary.filePath(QStringLiteral("cache")),
                                 [&](const QString &sha, PdfVersions::Failure *) { return network.get(QNetworkRequest(server.url(sha))); });
    // Two requests for one version share one download.
    int calls = 0;
    PdfVersions::Result first, second;
    fetcher.fetch(digest, &fetcher, [&](const PdfVersions::Result &result) { first = result; ++calls; });
    fetcher.fetch(digest, &fetcher, [&](const PdfVersions::Result &result) { second = result; ++calls; });
    QTRY_COMPARE_WITH_TIMEOUT(calls, 2, 20000);
    QCOMPARE(int(first.failure), int(PdfVersions::Failure::None));
    QCOMPARE(first.path, PdfVersions::cachePath(fetcher.cacheRoot(), digest));
    QCOMPARE(second.path, first.path);
    QCOMPARE(server.requests, QStringList{QStringLiteral("/v1/objects/") + digest});
    QFile cached(first.path);
    QVERIFY(cached.open(QIODevice::ReadOnly));
    QCOMPARE(cached.readAll(), earlier);
    // Then from the cache, without the server.
    const auto again = fetchNow(fetcher, digest);
    QCOMPARE(again.path, first.path);
    QCOMPARE(server.requests.size(), 1);

    // Bytes that are not the version asked for: refused, not cached.
    const QString wanted = sha256("%PDF-1.4 the real version");
    server.objects.insert(wanted, "%PDF-1.4 something else");
    const auto mismatch = fetchNow(fetcher, wanted);
    QCOMPARE(int(mismatch.failure), int(PdfVersions::Failure::Mismatch));
    QVERIFY(mismatch.path.isEmpty());
    QVERIFY(!QFileInfo::exists(PdfVersions::cachePath(fetcher.cacheRoot(), wanted)));

    // A caller gone meanwhile is not called.
    auto *gone = new QObject;
    bool called = false;
    const QByteArray third = pdfBytes(QStringLiteral("file1.pdf"));
    server.objects.insert(sha256(third), third);
    fetcher.fetch(sha256(third), gone, [&](const PdfVersions::Result &) { called = true; });
    delete gone;
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(PdfVersions::cachePath(fetcher.cacheRoot(), sha256(third))), 20000);
    QTest::qWait(50);
    QVERIFY(!called);
}

void PdfVersionsTest::fetchFailures()
{
    QTemporaryDir temporary;
    ObjectServer server;
    QNetworkAccessManager network;
    // A version the server no longer keeps.
    PdfVersions::Fetcher pruned(temporary.filePath(QStringLiteral("docs")), temporary.filePath(QStringLiteral("cache")),
                                [&](const QString &sha, PdfVersions::Failure *) { return network.get(QNetworkRequest(server.url(sha))); });
    const QString digest(64, QLatin1Char('d'));
    auto result = fetchNow(pruned, digest);
    QCOMPARE(int(result.failure), int(PdfVersions::Failure::NotFound));
    QVERIFY(result.path.isEmpty());
    // Offline: nothing listens.
    QTcpServer closed;
    QVERIFY(closed.listen(QHostAddress::LocalHost));
    const quint16 port = closed.serverPort();
    closed.close();
    PdfVersions::Fetcher offline(temporary.filePath(QStringLiteral("docs")), temporary.filePath(QStringLiteral("cache")),
                                 [&](const QString &sha, PdfVersions::Failure *) {
                                     return network.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/v1/objects/%2").arg(port).arg(sha))));
                                 });
    result = fetchNow(offline, digest);
    QCOMPARE(int(result.failure), int(PdfVersions::Failure::Unreachable));
    QVERIFY(!result.detail.isEmpty());
    // Not a version at all.
    result = fetchNow(offline, QStringLiteral("not-a-hash"));
    QCOMPARE(int(result.failure), int(PdfVersions::Failure::Failed));
    QVERIFY(QDir(temporary.filePath(QStringLiteral("cache"))).entryList(QDir::Files).isEmpty());
}

void PdfVersionsTest::cachedVersionIsNotALibraryDocument()
{
    // Even a cached version with the very bytes of a Library PDF is no Library document: it is
    // outside the Library folder and named by its hash, which no Library file is.
    QTemporaryDir temporary;
    const QString docsRoot = temporary.filePath(QStringLiteral("docs"));
    const QByteArray bytes = pdfBytes(QStringLiteral("file1.pdf"));
    const QString digest = sha256(bytes);
    const QString library = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/paper.pdf"));
    QVERIFY(writeFile(library, bytes));
    QVERIFY(writeFile(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")),
                      QJsonDocument(QJsonObject{{QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                                                                              {QStringLiteral("document_id"), QStringLiteral("8d3c2a0e-5b0e-4d43-9b55-3f0f8d5c6a11")},
                                                                                              {QStringLiteral("pdf_sha256"), digest}}}}}).toJson()));
    const DocumentIdentityStore identities(docsRoot);
    QVERIFY(!identities.forFile(library).isEmpty());
    const QString cached = PdfVersions::store(PdfVersions::defaultCacheRoot(), digest, bytes);
    QVERIFY(!cached.isEmpty());
    QVERIFY(!cached.startsWith(QFileInfo(docsRoot).absoluteFilePath()));
    QVERIFY(PdfVersions::isCachedVersion(cached));
    QVERIFY(identities.forFile(cached).isEmpty());
    QVERIFY(identities.forFile(cached, digest).isEmpty());
    QVERIFY(identities.byFilename(QFileInfo(cached).fileName()).isEmpty());
    QVERIFY(QFile::remove(cached));
}

QTEST_GUILESS_MAIN(PdfVersionsTest)
#include "pdfversionstest.moc"
