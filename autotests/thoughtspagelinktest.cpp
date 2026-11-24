// SPDX-License-Identifier: GPL-2.0-or-later
// Page links (shell/thoughtspagelink.h): the parse the phone also makes
// (ThoughtsCitationLinks.kt parseThoughtsCitationLink), the page and note a link
// opens with, and the Library PDF it names.
#include "../part/pdfplacelink.h"
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
    void readsTheServersDocumentPagesLeniently();
    void otherAddressesStayWebLinks_data();
    void otherAddressesStayWebLinks();
    void knowsTheOriginsOfItsServer();
    void parsesPdfPlaceLinks();
    void malformedPlacesKeepThePage_data();
    void malformedPlacesKeepThePage();
    void refusesOtherPdfLinks_data();
    void refusesOtherPdfLinks();
    void convertsPdfPointsToThePageAsShown();
    void parsesReadingOverlayLinks();
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

// 2026-11-19: an agent linked a note with the server's "Open in Thoughts" page
// (https://<server>/open/document/<id>?page=1&first=1&version=<sha256>), which the Mac handed to
// the browser. A page of this computer's server under /open/document/<id> is the page link it
// stands for, read leniently: agents wrote some with only ?page=N.
void ThoughtsPageLinkTest::readsTheServersDocumentPagesLeniently()
{
    const QStringList origins{QStringLiteral("https://thoughts.example.org")};
    const QString page = QStringLiteral("https://thoughts.example.org/open/document/") + Document;

    auto link = Thoughts::serverPageLink(QUrl(page + QLatin1Char('?') + validQuery()), origins);
    QVERIFY(link);
    QCOMPARE(link->documentId, Document);
    QCOMPARE(link->page, 3);
    QCOMPARE(link->first, 10);
    QCOMPARE(link->version, Version);
    QCOMPARE(link->targetPage(), 12);
    QVERIFY(Thoughts::isServerDocumentPageUrl(QUrl(page), origins));

    // Missing first: 1. Missing version: the current one (no version). Missing page: 1.
    link = Thoughts::serverPageLink(QUrl(page + QStringLiteral("?page=5")), origins);
    QVERIFY(link);
    QCOMPARE(link->page, 5);
    QCOMPARE(link->first, 1);
    QVERIFY(link->version.isEmpty());
    QCOMPARE(link->targetPage(), 5);
    link = Thoughts::serverPageLink(QUrl(page), origins);
    QVERIFY(link);
    QCOMPARE(link->targetPage(), 1);
    QVERIFY(link->version.isEmpty());
    link = Thoughts::serverPageLink(QUrl(page + QStringLiteral("?first=4&version=") + Version), origins);
    QVERIFY(link);
    QCOMPARE(link->targetPage(), 4);
    QCOMPARE(link->version, Version);

    // Unreadable values count as missing; other parameters and a fragment do not matter.
    link = Thoughts::serverPageLink(QUrl(page + QStringLiteral("?page=0&first=x&version=nope&utm=1#top")), origins);
    QVERIFY(link);
    QCOMPARE(link->targetPage(), 1);
    QVERIFY(link->version.isEmpty());
    link = Thoughts::serverPageLink(QUrl(page + QStringLiteral("?page=2&page=9&version=") + QString(64, QLatin1Char('A'))), origins);
    QVERIFY(link);
    QCOMPARE(link->page, 2);
    QCOMPARE(link->version, Version);

    // The host's case, the default port and an upper-case id or a trailing slash are the same page.
    link = Thoughts::serverPageLink(QUrl(QStringLiteral("https://Thoughts.Example.ORG:443/open/document/") + Document.toUpper() + QStringLiteral("/?page=7")), origins);
    QVERIFY(link);
    QCOMPARE(link->documentId, Document);
    QCOMPARE(link->targetPage(), 7);
}

void ThoughtsPageLinkTest::otherAddressesStayWebLinks_data()
{
    QTest::addColumn<QString>("value");
    QTest::addColumn<bool>("documentPage");
    const QString query = QLatin1Char('?') + validQuery();
    QTest::newRow("other host") << QStringLiteral("https://other.example.org/open/document/") + Document + query << false;
    QTest::newRow("subdomain") << QStringLiteral("https://www.thoughts.example.org/open/document/") + Document + query << false;
    QTest::newRow("http") << QStringLiteral("http://thoughts.example.org/open/document/") + Document + query << false;
    QTest::newRow("other port") << QStringLiteral("https://thoughts.example.org:8443/open/document/") + Document + query << false;
    QTest::newRow("user info") << QStringLiteral("https://me@thoughts.example.org/open/document/") + Document + query << false;
    QTest::newRow("web app") << QStringLiteral("https://thoughts.example.org/app/") << false;
    QTest::newRow("web app document") << QStringLiteral("https://thoughts.example.org/app/document/") + Document + query << false;
    QTest::newRow("server root") << QStringLiteral("https://thoughts.example.org/") << false;
    QTest::newRow("other path") << QStringLiteral("https://thoughts.example.org/open/documents/") + Document + query << false;
    QTest::newRow("prefixed path") << QStringLiteral("https://thoughts.example.org/x/open/document/") + Document + query << false;
    QTest::newRow("thoughts scheme") << link(validQuery()) << false;
    // A document page without a document: not opened here, but still the server's page.
    QTest::newRow("no document") << QStringLiteral("https://thoughts.example.org/open/document/") + query << true;
    QTest::newRow("not an id") << QStringLiteral("https://thoughts.example.org/open/document/item-response-theory") + query << true;
    QTest::newRow("extra segment") << QStringLiteral("https://thoughts.example.org/open/document/") + Document + QStringLiteral("/page") + query << true;
}

void ThoughtsPageLinkTest::otherAddressesStayWebLinks()
{
    QFETCH(QString, value);
    QFETCH(bool, documentPage);
    const QStringList origins{QStringLiteral("https://thoughts.example.org")};
    QVERIFY2(!Thoughts::serverPageLink(QUrl(value), origins), qPrintable(value));
    QCOMPARE(Thoughts::isServerDocumentPageUrl(QUrl(value), origins), documentPage);
    // Without a server, nothing is the server's.
    QVERIFY(!Thoughts::isServerDocumentPageUrl(QUrl(value), {}));
}

void ThoughtsPageLinkTest::knowsTheOriginsOfItsServer()
{
    QCOMPARE(Thoughts::ownServerOrigins(QStringLiteral("https://thoughts.example.org/")), QStringList{QStringLiteral("https://thoughts.example.org")});
    QCOMPARE(Thoughts::ownServerOrigins(QStringLiteral("https://Thoughts.Example.org:8443")), QStringList{QStringLiteral("https://thoughts.example.org:8443")});
    // The public server and its earlier private address are one server: each knows the other's pages.
    // A build without an earlier address (THOUGHTS_LEGACY_SERVER_URL empty) has only the public one.
    const bool legacy = !Thoughts::ServerAddress::legacyUrl().isEmpty();
    QStringList owner{Thoughts::ServerAddress::origin(Thoughts::ServerAddress::defaultUrl())};
    if (legacy) owner.append(Thoughts::ServerAddress::origin(Thoughts::ServerAddress::legacyUrl()));
    if (legacy) QCOMPARE(Thoughts::ownServerOrigins(Thoughts::ServerAddress::legacyUrl()), owner);
    QCOMPARE(Thoughts::ownServerOrigins(Thoughts::ServerAddress::defaultUrl()), owner);
    QVERIFY(Thoughts::serverPageLink(QUrl(Thoughts::ServerAddress::defaultUrl() + QStringLiteral("/open/document/") + Document),
                                     Thoughts::ownServerOrigins(legacy ? Thoughts::ServerAddress::legacyUrl() : Thoughts::ServerAddress::defaultUrl())));
    // An address Thoughts refuses (the tests' own http server, nothing) has no pages.
    QVERIFY(Thoughts::ownServerOrigins(QStringLiteral("http://127.0.0.1:9")).isEmpty());
    QVERIFY(Thoughts::ownServerOrigins(QString()).isEmpty());
    // The configured server: THOUGHTS_SERVER_URL in tests.
    const QByteArray saved = qgetenv("THOUGHTS_SERVER_URL");
    qputenv("THOUGHTS_SERVER_URL", "https://thoughts.example.org");
    QCOMPARE(Thoughts::ownServerOrigins(), QStringList{QStringLiteral("https://thoughts.example.org")});
    if (saved.isEmpty()) qunsetenv("THOUGHTS_SERVER_URL");
    else qputenv("THOUGHTS_SERVER_URL", saved);
}

// Agent note links to a place in a PDF (server protocol/agent-notes.md, "Links to a place in a
// PDF"): the page as before, and optionally a point and the passage, in PDF points.
void ThoughtsPageLinkTest::parsesPdfPlaceLinks()
{
    const auto place = Thoughts::parsePdfPlaceLink(QStringLiteral("thoughts-pdf-link:web_article.pdf?page=1&x=72.000&y=1387.437&rect=72,1370.5,523.25,1399"));
    QVERIFY(place);
    QCOMPARE(place->filename, QStringLiteral("web_article.pdf"));
    QCOMPARE(place->page, 1);
    QVERIFY(place->point);
    QCOMPARE(*place->point, QPointF(72.0, 1387.437));
    QVERIFY(place->rect);
    QCOMPARE(*place->rect, QRectF(QPointF(72.0, 1370.5), QPointF(523.25, 1399.0)));

    // The page alone, as notes linked it so far.
    const auto page = Thoughts::parsePdfPlaceLink(QStringLiteral("thoughts-pdf-link:notes.pdf?page=0"));
    QVERIFY(page);
    QCOMPARE(page->filename, QStringLiteral("notes.pdf"));
    QCOMPARE(page->page, 0);
    QVERIFY(!page->point && !page->rect);

    // The corners in any order make the same passage; a rect alone is the place too.
    const auto swapped = Thoughts::parsePdfPlaceLink(QStringLiteral("thoughts-pdf-link:notes.pdf?page=2&rect=300,410,60,380"));
    QVERIFY(swapped && swapped->rect && !swapped->point);
    QCOMPARE(*swapped->rect, QRectF(QPointF(60, 380), QPointF(300, 410)));
    // y without x: the left edge. Encoded commas (as a URL may carry them) and unknown parameters.
    const auto encoded = Thoughts::parsePdfPlaceLink(QStringLiteral("thoughts-pdf-link:notes.pdf?page=4&y=500&zoom=2&rect=10%2C20%2C30%2C40"));
    QVERIFY(encoded && encoded->point && encoded->rect);
    QCOMPARE(*encoded->point, QPointF(0, 500));
    QCOMPARE(*encoded->rect, QRectF(QPointF(10, 20), QPointF(30, 40)));
    // The first of repeated parameters counts.
    const auto repeated = Thoughts::parsePdfPlaceLink(QStringLiteral("thoughts-pdf-link:notes.pdf?page=4&y=500&y=10"));
    QVERIFY(repeated && repeated->point);
    QCOMPARE(repeated->point->y(), 500.0);
    // What QUrl gives back for the link of a PDF reads the same.
    const QString fromUrl = QUrl(QStringLiteral("thoughts-pdf-link:notes.pdf?page=3&x=72&y=400.5&rect=60,380,300,410")).toString();
    const auto read = Thoughts::parsePdfPlaceLink(fromUrl);
    QVERIFY2(read && read->point && read->rect, qPrintable(fromUrl));
    QCOMPARE(read->page, 3);
}

void ThoughtsPageLinkTest::malformedPlacesKeepThePage_data()
{
    QTest::addColumn<QString>("query");
    QTest::addColumn<bool>("point");
    QTest::addColumn<bool>("rect");
    QTest::newRow("y not a number") << QStringLiteral("&x=72&y=abc") << false << false;
    QTest::newRow("y nan") << QStringLiteral("&x=72&y=nan") << false << false;
    QTest::newRow("y infinite") << QStringLiteral("&y=inf") << false << false;
    QTest::newRow("y exponent") << QStringLiteral("&y=1e400") << false << false;
    QTest::newRow("y empty") << QStringLiteral("&y=") << false << false;
    QTest::newRow("x alone") << QStringLiteral("&x=72") << false << false;
    QTest::newRow("x malformed") << QStringLiteral("&x=7,2&y=400") << true << false;
    QTest::newRow("rect three numbers") << QStringLiteral("&y=400&rect=1,2,3") << true << false;
    QTest::newRow("rect five numbers") << QStringLiteral("&rect=1,2,3,4,5") << false << false;
    QTest::newRow("rect no width") << QStringLiteral("&rect=10,20,10,40") << false << false;
    QTest::newRow("rect no height") << QStringLiteral("&rect=10,20,30,20") << false << false;
    QTest::newRow("rect word") << QStringLiteral("&rect=a,b,c,d") << false << false;
    QTest::newRow("bare ampersands") << QStringLiteral("&&") << false << false;
    QTest::newRow("no value") << QStringLiteral("&rect") << false << false;
}

void ThoughtsPageLinkTest::malformedPlacesKeepThePage()
{
    QFETCH(QString, query);
    QFETCH(bool, point);
    QFETCH(bool, rect);
    const auto place = Thoughts::parsePdfPlaceLink(QStringLiteral("thoughts-pdf-link:notes.pdf?page=7") + query);
    QVERIFY(place);
    QCOMPARE(place->page, 7);
    QCOMPARE(bool(place->point), point);
    QCOMPARE(bool(place->rect), rect);
}

void ThoughtsPageLinkTest::refusesOtherPdfLinks_data()
{
    QTest::addColumn<QString>("href");
    QTest::newRow("no page") << QStringLiteral("thoughts-pdf-link:notes.pdf");
    QTest::newRow("page not first") << QStringLiteral("thoughts-pdf-link:notes.pdf?x=1&page=1");
    QTest::newRow("negative page") << QStringLiteral("thoughts-pdf-link:notes.pdf?page=-1");
    QTest::newRow("folder") << QStringLiteral("thoughts-pdf-link:docs/notes.pdf?page=1");
    QTest::newRow("parent") << QStringLiteral("thoughts-pdf-link:../notes.pdf?page=1");
    QTest::newRow("not a pdf") << QStringLiteral("thoughts-pdf-link:notes.txt?page=1");
    QTest::newRow("fragment") << QStringLiteral("thoughts-pdf-link:notes.pdf?page=1#quote=x");
    QTest::newRow("other scheme") << QStringLiteral("https://example.org/notes.pdf?page=1");
}

void ThoughtsPageLinkTest::refusesOtherPdfLinks()
{
    QFETCH(QString, href);
    QVERIFY(!Thoughts::parsePdfPlaceLink(href));
}

// PDF points from the bottom left of the crop box, in the PDF's own user space, to the page as
// the reader shows it (top left origin, normalized), turned by its /Rotate and the reader.
void ThoughtsPageLinkTest::convertsPdfPointsToThePageAsShown()
{
    const QSizeF crop(500, 2000); // a tall page
    const auto near = [](const QPointF &actual, const QPointF &expected) {
        return std::abs(actual.x() - expected.x()) < 1e-9 && std::abs(actual.y() - expected.y()) < 1e-9;
    };
    // The origin flip: y counts from the bottom.
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(50, 1500), crop, 0), QPointF(0.1, 0.25)));
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(0, 2000), crop, 0), QPointF(0, 0)));
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(500, 0), crop, 0), QPointF(1, 1)));
    // Outside the crop box: its edge.
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(-20, 2400), crop, 0), QPointF(0, 0)));
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(900, -5), crop, 0), QPointF(1, 1)));
    // Turned clockwise: the page's top left goes to the top right, and so on.
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(50, 1500), crop, 1), QPointF(0.75, 0.1)));
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(50, 1500), crop, 2), QPointF(0.9, 0.75)));
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(50, 1500), crop, 3), QPointF(0.25, 0.9)));
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(50, 1500), crop, 5), QPointF(0.75, 0.1)));
    QVERIFY(near(Thoughts::normalizedPdfPoint(QPointF(0, 2000), crop, 1), QPointF(1, 0)));
    // A passage: its lower edge in PDF points is its bottom on the page.
    const QRectF passage(QPointF(50, 1400), QPointF(250, 1500));
    const QRectF shown = Thoughts::normalizedPdfRect(passage, crop, 0);
    QVERIFY(near(shown.topLeft(), QPointF(0.1, 0.25)));
    QVERIFY(near(shown.bottomRight(), QPointF(0.5, 0.3)));
    const QRectF turned = Thoughts::normalizedPdfRect(passage, crop, 1);
    QVERIFY(near(turned.topLeft(), QPointF(0.7, 0.1)));
    QVERIFY(near(turned.bottomRight(), QPointF(0.75, 0.5)));
    const QRectF upsideDown = Thoughts::normalizedPdfRect(passage, crop, 2);
    QVERIFY(near(upsideDown.topLeft(), QPointF(0.5, 0.7)));
    QVERIFY(near(upsideDown.bottomRight(), QPointF(0.9, 0.75)));
    // No crop box: nothing to place.
    QCOMPARE(Thoughts::normalizedPdfPoint(QPointF(1, 1), QSizeF(), 0), QPointF());
}

void ThoughtsPageLinkTest::parsesReadingOverlayLinks()
{
    // add_reading_overlay's open_url (reading-overlays.md §3.1): thoughts://open/document?document_id=…&overlay=….
    const QString overlay = QStringLiteral("5b0c1f6e-2a47-4d1e-9c3b-7e8f2a6d4c10");
    const auto parsed = Thoughts::parseOverlayLink(QUrl(QStringLiteral("thoughts://open/document?document_id=%1&overlay=%2").arg(Document, overlay)));
    QVERIFY(parsed);
    QCOMPARE(parsed->documentId, Document);
    QCOMPARE(parsed->overlayId, overlay);
    // Either order, upper-case ids, other parameters ignored.
    const auto reordered = Thoughts::parseOverlayLink(QUrl(QStringLiteral("thoughts://open/document?overlay=%1&page=3&document_id=%2").arg(overlay.toUpper(), Document.toUpper())));
    QVERIFY(reordered);
    QCOMPARE(reordered->documentId, Document);
    QCOMPARE(reordered->overlayId, overlay);
    // The page link's form with an overlay.
    const auto pageForm = Thoughts::parseOverlayLink(QUrl(link(validQuery() + QStringLiteral("&overlay=") + overlay)));
    QVERIFY(pageForm);
    QCOMPARE(pageForm->documentId, Document);
    QCOMPARE(pageForm->overlayId, overlay);
    // It is no page link: those take exactly page, first and version.
    QVERIFY(!Thoughts::parsePageLink(QUrl(QStringLiteral("thoughts://open/document?document_id=%1&overlay=%2").arg(Document, overlay))));
    QVERIFY(Thoughts::isPageLinkUrl(QUrl(QStringLiteral("thoughts://open/document?document_id=%1&overlay=%2").arg(Document, overlay))));
    // Not an overlay link.
    for (const QString &value : {link(validQuery()), QStringLiteral("thoughts://open/document?document_id=%1").arg(Document),
                                 QStringLiteral("thoughts://open/document?document_id=%1&overlay=m1").arg(Document),
                                 QStringLiteral("thoughts://open/document?document_id=not-a-uuid&overlay=%1").arg(overlay),
                                 QStringLiteral("thoughts://open/document?overlay=%1").arg(overlay),
                                 QStringLiteral("thoughts://open/document?document_id=%1&overlay=%2&overlay=%2").arg(Document, overlay),
                                 QStringLiteral("thoughts://open/card?document_id=%1&overlay=%2").arg(Document, overlay),
                                 QStringLiteral("thoughts://elsewhere/document?document_id=%1&overlay=%2").arg(Document, overlay),
                                 QStringLiteral("https://open/document?document_id=%1&overlay=%2").arg(Document, overlay)})
        QVERIFY2(!Thoughts::parseOverlayLink(QUrl(value)), qPrintable(value));
}

QTEST_GUILESS_MAIN(ThoughtsPageLinkTest)
#include "thoughtspagelinktest.moc"
