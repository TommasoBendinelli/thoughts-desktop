// SPDX-License-Identifier: GPL-2.0-or-later
// Page links (shell/thoughtspagelink.h): the parse the phone also makes
// (ThoughtsCitationLinks.kt parseThoughtsCitationLink), the page and note a link
// opens with, and the Library PDF it names.
#include "../shell/thoughtspagelink.h"

#include <QCryptographicHash>
#include <QTemporaryDir>
#include <QTest>

namespace
{
const QString Document = QStringLiteral("0b1c2d3e-4f50-4a61-8b72-93a4b5c6d7e8");
const QString Version = QString(64, QLatin1Char('a'));

QString link(const QString &query, const QString &document = Document)
{
    return QStringLiteral("thoughts://open/document/") + document + QLatin1Char('?') + query;
}

QString validQuery()
{
    return QStringLiteral("page=3&first=10&version=") + Version;
}

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class ThoughtsPageLinkTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void parsesValidLinks();
    void rejectsInvalidLinks_data();
    void rejectsInvalidLinks();
    void opensTheTargetPageWithThePhonesNotes();
    void findsTheLibraryPdfOfTheDocument();
};

void ThoughtsPageLinkTest::parsesValidLinks()
{
    const auto parsed = Thoughts::parsePageLink(QUrl(link(validQuery())));
    QVERIFY(parsed);
    QCOMPARE(parsed->documentId, Document);
    QCOMPARE(parsed->page, 3);
    QCOMPARE(parsed->first, 10);
    QCOMPARE(parsed->version, Version);
    QCOMPARE(parsed->targetPage(), 12);
    QVERIFY(Thoughts::isPageLinkUrl(QUrl(link(validQuery()))));

    // Any order of the three keys; leading zeros as the phone's toIntOrNull reads them.
    const auto reordered = Thoughts::parsePageLink(QUrl(link(QStringLiteral("version=%1&first=007&page=1").arg(Version))));
    QVERIFY(reordered);
    QCOMPARE(reordered->targetPage(), 7);

    // The largest target page an int holds.
    const auto last = Thoughts::parsePageLink(QUrl(link(QStringLiteral("page=1&first=2147483647&version=") + Version)));
    QVERIFY(last);
    QCOMPARE(last->targetPage(), std::numeric_limits<int>::max());

    // Where the Mac cannot be as strict as the phone: QUrl decodes percent-encoded letters and
    // digits (RFC 3986 makes them the same URI) and folds the case of the scheme and the host
    // before the link reaches Thoughts, so these read as the plain link. The phone rejects them.
    QCOMPARE(Thoughts::parsePageLink(QUrl(link(QStringLiteral("%70age=%33&first=10&version=") + Version)))->targetPage(), 12);
    QVERIFY(Thoughts::parsePageLink(QUrl(QStringLiteral("THOUGHTS://OPEN/document/") + Document + QLatin1Char('?') + validQuery())));

    // Other UUID versions the phone accepts (1 to 5, RFC 4122 variant).
    QVERIFY(Thoughts::parsePageLink(QUrl(link(validQuery(), QStringLiteral("0b1c2d3e-4f50-1a61-ab72-93a4b5c6d7e8")))));
}

void ThoughtsPageLinkTest::rejectsInvalidLinks_data()
{
    QTest::addColumn<QString>("value");
    const QString base = QStringLiteral("thoughts://open/document/") + Document;
    // Scheme, host and authority.
    QTest::newRow("https") << QStringLiteral("https://thoughts.example.org/open/document/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("other scheme") << QStringLiteral("thought://open/document/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("other host") << QStringLiteral("thoughts://review-card/document/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("longer host") << QStringLiteral("thoughts://opened/document/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("user info") << QStringLiteral("thoughts://me@open/document/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("empty user info") << QStringLiteral("thoughts://@open/document/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("port") << QStringLiteral("thoughts://open:80/document/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("fragment") << base + QLatin1Char('?') + validQuery() + QStringLiteral("#page");
    QTest::newRow("empty fragment") << base + QLatin1Char('?') + validQuery() + QLatin1Char('#');
    // Path.
    QTest::newRow("no path") << QStringLiteral("thoughts://open?") + validQuery();
    QTest::newRow("other path") << QStringLiteral("thoughts://open/documents/") + Document + QLatin1Char('?') + validQuery();
    QTest::newRow("no document") << QStringLiteral("thoughts://open/document/?") + validQuery();
    QTest::newRow("extra segment") << base + QStringLiteral("/page?") + validQuery();
    QTest::newRow("trailing slash") << base + QStringLiteral("/?") + validQuery();
    QTest::newRow("uppercase id") << link(validQuery(), Document.toUpper());
    QTest::newRow("braced id") << link(validQuery(), QLatin1Char('{') + Document + QLatin1Char('}'));
    QTest::newRow("id without dashes") << link(validQuery(), QString(Document).remove(QLatin1Char('-')));
    QTest::newRow("id version 0") << link(validQuery(), QStringLiteral("0b1c2d3e-4f50-0a61-8b72-93a4b5c6d7e8"));
    QTest::newRow("id version 6") << link(validQuery(), QStringLiteral("0b1c2d3e-4f50-6a61-8b72-93a4b5c6d7e8"));
    QTest::newRow("id variant c") << link(validQuery(), QStringLiteral("0b1c2d3e-4f50-4a61-cb72-93a4b5c6d7e8"));
    QTest::newRow("short id") << link(validQuery(), Document.chopped(1));
    // Query shape.
    QTest::newRow("no query") << base;
    QTest::newRow("empty query") << base + QLatin1Char('?');
    QTest::newRow("missing version") << link(QStringLiteral("page=3&first=10"));
    QTest::newRow("missing page") << link(QStringLiteral("first=10&version=") + Version);
    QTest::newRow("missing first") << link(QStringLiteral("page=3&version=") + Version);
    QTest::newRow("extra key") << link(validQuery() + QStringLiteral("&x=1"));
    QTest::newRow("duplicate key") << link(validQuery() + QStringLiteral("&page=4"));
    QTest::newRow("empty value") << link(QStringLiteral("page=&first=10&version=") + Version);
    QTest::newRow("key without value") << link(QStringLiteral("page&first=10&version=") + Version);
    QTest::newRow("value without key") << link(QStringLiteral("=3&first=10&version=") + Version);
    QTest::newRow("trailing ampersand") << link(validQuery() + QLatin1Char('&'));
    QTest::newRow("doubled ampersand") << link(QStringLiteral("page=3&&first=10&version=") + Version);
    QTest::newRow("two equals signs") << link(QStringLiteral("page=3=3&first=10&version=") + Version);
    QTest::newRow("semicolon") << link(QStringLiteral("page=3;first=10&version=") + Version);
    QTest::newRow("percent encoded value") << link(QStringLiteral("page=3%26first%3D10&version=") + Version);
    QTest::newRow("uppercase key") << link(QStringLiteral("Page=3&first=10&version=") + Version);
    // Page and first.
    QTest::newRow("page zero") << link(QStringLiteral("page=0&first=10&version=") + Version);
    QTest::newRow("first zero") << link(QStringLiteral("page=3&first=0&version=") + Version);
    QTest::newRow("negative page") << link(QStringLiteral("page=-3&first=10&version=") + Version);
    QTest::newRow("plus sign") << link(QStringLiteral("page=%2B3&first=10&version=") + Version);
    QTest::newRow("fraction") << link(QStringLiteral("page=3.0&first=10&version=") + Version);
    QTest::newRow("letters") << link(QStringLiteral("page=three&first=10&version=") + Version);
    QTest::newRow("page beyond int") << link(QStringLiteral("page=2147483648&first=1&version=") + Version);
    QTest::newRow("target beyond int") << link(QStringLiteral("page=2&first=2147483647&version=") + Version);
    // Version.
    QTest::newRow("uppercase version") << link(QStringLiteral("page=3&first=10&version=") + QString(64, QLatin1Char('A')));
    QTest::newRow("short version") << link(QStringLiteral("page=3&first=10&version=") + QString(63, QLatin1Char('a')));
    QTest::newRow("long version") << link(QStringLiteral("page=3&first=10&version=") + QString(65, QLatin1Char('a')));
    QTest::newRow("non-hex version") << link(QStringLiteral("page=3&first=10&version=") + QString(64, QLatin1Char('g')));
}

void ThoughtsPageLinkTest::rejectsInvalidLinks()
{
    QFETCH(QString, value);
    QVERIFY2(!Thoughts::parsePageLink(QUrl(value)), qPrintable(value));
}

void ThoughtsPageLinkTest::opensTheTargetPageWithThePhonesNotes()
{
    auto opening = Thoughts::pageLinkOpening(5, 10, false);
    QCOMPARE(opening.page, 5);
    QVERIFY(opening.message.isEmpty());

    opening = Thoughts::pageLinkOpening(5, 10, true);
    QCOMPARE(opening.page, 5);
    QCOMPARE(opening.message, QStringLiteral("Opened page 5 in the current PDF. Its pagination may have changed since this link was created."));

    opening = Thoughts::pageLinkOpening(12, 10, false);
    QCOMPARE(opening.page, 10);
    QCOMPARE(opening.message, QStringLiteral("Page 12 is not available. Opened page 10 instead."));

    opening = Thoughts::pageLinkOpening(12, 10, true);
    QCOMPARE(opening.page, 10);
    QCOMPARE(opening.message,
             QStringLiteral("Page 12 is not available in the current PDF, so page 10 was opened. Its pagination may have changed since this link was created."));

    // The last page is still there.
    opening = Thoughts::pageLinkOpening(10, 10, false);
    QCOMPARE(opening.page, 10);
    QVERIFY(opening.message.isEmpty());
}

void ThoughtsPageLinkTest::findsTheLibraryPdfOfTheDocument()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString currentPdfs = QDir(root.path()).filePath(QStringLiteral("current_pdfs"));
    QVERIFY(QDir().mkpath(currentPdfs));
    const QByteArray bytes("%PDF-1.4 paper\n");
    QVERIFY(writeFile(QDir(currentPdfs).filePath(QStringLiteral("paper.pdf")), bytes));

    const QString missing = QStringLiteral("1b1c2d3e-4f50-4a61-8b72-93a4b5c6d7e8");
    const QString twice = QStringLiteral("2b1c2d3e-4f50-4a61-8b72-93a4b5c6d7e8");
    const QString nested = QStringLiteral("3b1c2d3e-4f50-4a61-8b72-93a4b5c6d7e8");
    const auto writeIndex = [&](const QString &paperSha256) {
        const QJsonArray pdfs {
            QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("document_id"), Document}, {QStringLiteral("pdf_sha256"), paperSha256}},
            QJsonObject {{QStringLiteral("filename"), QStringLiteral("gone.pdf")}, {QStringLiteral("document_id"), missing}, {QStringLiteral("pdf_sha256"), Version}},
            QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("document_id"), twice}, {QStringLiteral("pdf_sha256"), Version}},
            QJsonObject {{QStringLiteral("filename"), QStringLiteral("other.pdf")}, {QStringLiteral("document_id"), twice}, {QStringLiteral("pdf_sha256"), Version}},
            QJsonObject {{QStringLiteral("filename"), QStringLiteral("../paper.pdf")}, {QStringLiteral("document_id"), nested}, {QStringLiteral("pdf_sha256"), Version}},
        };
        return writeFile(QDir(currentPdfs).filePath(QStringLiteral(".thoughts-library-index.json")),
                         QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), pdfs}}).toJson());
    };
    QVERIFY(writeIndex(sha256(bytes)));

    const QString path = Thoughts::pageLinkDocumentPath(root.path(), Document);
    QCOMPARE(path, QDir(currentPdfs).filePath(QStringLiteral("paper.pdf")));
    QCOMPARE(Thoughts::pageLinkPdfSha256(root.path(), path), sha256(bytes));

    QVERIFY(Thoughts::pageLinkDocumentPath(root.path(), QStringLiteral("4b1c2d3e-4f50-4a61-8b72-93a4b5c6d7e8")).isEmpty());
    QVERIFY(Thoughts::pageLinkDocumentPath(root.path(), missing).isEmpty());
    QVERIFY(Thoughts::pageLinkDocumentPath(root.path(), twice).isEmpty());
    QVERIFY(Thoughts::pageLinkDocumentPath(root.path(), nested).isEmpty());

    // A copy that does not match the Library's hash is compared as it is.
    QVERIFY(writeIndex(Version));
    QCOMPARE(Thoughts::pageLinkPdfSha256(root.path(), path), sha256(bytes));

    // No Library index: nothing is synchronized.
    QVERIFY(QFile::remove(QDir(currentPdfs).filePath(QStringLiteral(".thoughts-library-index.json"))));
    QVERIFY(Thoughts::pageLinkDocumentPath(root.path(), Document).isEmpty());
}

QTEST_GUILESS_MAIN(ThoughtsPageLinkTest)
#include "thoughtspagelinktest.moc"
