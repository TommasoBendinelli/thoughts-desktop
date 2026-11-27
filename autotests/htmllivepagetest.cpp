// SPDX-License-Identifier: GPL-2.0-or-later
// Live HTML pages and snapshots (thoughts-server protocol/html-live-pages.md): the stub and its
// address rules, the origin asked about, where a live page may navigate, the snapshot file made
// from the page's serialized HTML, and the versions this computer wrote over a page.
#include "core/htmllivepage.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>

namespace
{
// The stub of html-live-pages.md §1, as the server and agents write it.
const QByteArray SpecStub = "<!doctype html>\n"
                            "<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
                            "<meta name=\"thoughts-live-page\" content=\"http://localhost:18770/\">\n"
                            "<title>Training curves</title></head>\n"
                            "<body><p>Live page: <a href=\"http://localhost:18770/\">http://localhost:18770/</a>.\n"
                            "It opens in Thoughts, on a device that can reach this address.</p></body></html>\n";
const QString Path = QStringLiteral("docs-runs/tex/support/html/curves.html");

QByteArray stubWith(const QByteArray &meta)
{
    return QByteArray("<!doctype html><html><head>" + meta + "<title>T</title></head><body>Live.</body></html>");
}

QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool write(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class HtmlLivePageTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void theSpecStubIsLive();
    void malformedStubsAreEmbeddedPages();
    void addressRules();
    void originsAskedAbout();
    void navigationStaysWithinThePage();
    void stubRoundTrip();
    void snapshotFromTheSerializedPage();
    void snapshotWithoutHeadOrBody();
    void consentIsRememberedPerOrigin();
    void writtenPagesUntilTheBundleHasThem();
    void libraryPageWrite();
};

void HtmlLivePageTest::theSpecStubIsLive()
{
    QCOMPARE(Thoughts::LivePage::stubAddress(SpecStub), QUrl(QStringLiteral("http://localhost:18770/")));
    QCOMPARE(Thoughts::LivePage::title(SpecStub), QStringLiteral("Training curves"));
    // A stub is no snapshot.
    QVERIFY(!Thoughts::LivePage::snapshotSource(SpecStub).isValid());
    QTemporaryDir dir;
    QVERIFY(write(dir.filePath(QStringLiteral("stub.html")), SpecStub));
    QCOMPARE(Thoughts::LivePage::stubAddressOfFile(dir.filePath(QStringLiteral("stub.html"))), QUrl(QStringLiteral("http://localhost:18770/")));
    // Attribute order, quotes, case and entities as HTML allows them.
    QCOMPARE(Thoughts::LivePage::stubAddress(stubWith("<META content='https://runs.example.org/a?x=1&amp;y=2' NAME=Thoughts-Live-Page>")),
             QUrl(QStringLiteral("https://runs.example.org/a?x=1&y=2")));
    QCOMPARE(Thoughts::LivePage::stubAddress(stubWith("<meta name=thoughts-live-page content=http://10.0.0.7:6006/#scalars>")),
             QUrl(QStringLiteral("http://10.0.0.7:6006/#scalars")));
    // The first meta of the name counts.
    QCOMPARE(Thoughts::LivePage::stubAddress(stubWith("<meta name=\"thoughts-live-page\" content=\"http://a.test/\"><meta name=\"thoughts-live-page\" content=\"http://b.test/\">")),
             QUrl(QStringLiteral("http://a.test/")));
}

void HtmlLivePageTest::malformedStubsAreEmbeddedPages()
{
    using Thoughts::LivePage::stubAddress;
    // Shown sealed off as the embedded page they are, never loaded.
    QVERIFY(!stubAddress(QByteArray()).isValid());
    QVERIFY(!stubAddress("<html><body>An ordinary page.</body></html>").isValid());
    for (const QByteArray &address :
         {QByteArray("ftp://files.example.org/"), QByteArray("localhost:18770"), QByteArray("/relative/page.html"), QByteArray("javascript:alert(1)"),
          QByteArray("http://user:secret@localhost:18770/"), QByteArray("http://user@localhost/"), QByteArray("http:///nohost"), QByteArray(""),
          QByteArray("http://local host/"), QByteArray(QByteArray("http://localhost/") + QByteArray(2048, 'a')), QByteArray("file:///etc/passwd"),
          QByteArray("data:text/html,hi")}) {
        QVERIFY2(!stubAddress(stubWith(QByteArray("<meta name=\"thoughts-live-page\" content=\"" + address + "\">"))).isValid(), address.constData());
    }
    // A meta without content, or only in a comment.
    QVERIFY(!stubAddress(stubWith("<meta name=\"thoughts-live-page\">")).isValid());
    QVERIFY(!stubAddress(stubWith("<!-- <meta name=\"thoughts-live-page\" content=\"http://localhost:1/\"> -->")).isValid());
    // Something a reader runs, more than 8 KiB, not UTF-8.
    QByteArray scripted = SpecStub;
    scripted.replace("</body>", "<script>fetch('/')</script></body>");
    QVERIFY(!stubAddress(scripted).isValid());
    QByteArray large = SpecStub;
    large.replace("</body>", QByteArray("<p>" + QByteArray(int(Thoughts::LivePage::StubLimit), 'x') + "</p></body>"));
    QVERIFY(large.size() > Thoughts::LivePage::StubLimit);
    QVERIFY(!stubAddress(large).isValid());
    QByteArray latin = SpecStub;
    latin.replace("Training curves", "Kurven \xe4");
    QVERIFY(!stubAddress(latin).isValid());
}

void HtmlLivePageTest::addressRules()
{
    QString error;
    QCOMPARE(Thoughts::LivePage::validAddress(QStringLiteral(" https://thoughts.example.org/runs/7 "), &error), QUrl(QStringLiteral("https://thoughts.example.org/runs/7")));
    QVERIFY(error.isEmpty());
    QVERIFY(Thoughts::LivePage::validAddress(QStringLiteral("http://[::1]:8080/"), &error).isValid());
    QVERIFY(Thoughts::LivePage::validAddress(QStringLiteral("http://192.168.1.20/"), &error).isValid());
    QVERIFY(!Thoughts::LivePage::validAddress(QStringLiteral("http://user:pw@thoughts.example.org/"), &error).isValid());
    QVERIFY(error.contains(QStringLiteral("user name or password")));
    QVERIFY(!Thoughts::LivePage::validAddress(QStringLiteral("ws://localhost:1/"), &error).isValid());
    QVERIFY(error.contains(QStringLiteral("http or https")));
    QString longest = QStringLiteral("http://localhost/");
    longest += QString(Thoughts::LivePage::AddressLimit - longest.size(), QLatin1Char('a'));
    QVERIFY(Thoughts::LivePage::validAddress(longest).isValid());
    QVERIFY(!Thoughts::LivePage::validAddress(longest + QLatin1Char('a'), &error).isValid());
    QVERIFY(error.contains(QStringLiteral("2048")));
}

void HtmlLivePageTest::originsAskedAbout()
{
    using Thoughts::LivePage::origin;
    QCOMPARE(origin(QUrl(QStringLiteral("http://localhost:18770/a/b?c#d"))), QStringLiteral("http://localhost:18770"));
    QCOMPARE(origin(QUrl(QStringLiteral("HTTP://LocalHost:80/"))), QStringLiteral("http://localhost"));
    QCOMPARE(origin(QUrl(QStringLiteral("https://thoughts.example.org:443/x"))), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(origin(QUrl(QStringLiteral("https://thoughts.example.org:8443/x"))), QStringLiteral("https://thoughts.example.org:8443"));
    QCOMPARE(origin(QUrl(QStringLiteral("http://[::1]:6006/"))), QStringLiteral("http://[::1]:6006"));
    QVERIFY(origin(QUrl(QStringLiteral("file:///tmp/a.html"))).isEmpty());
}

void HtmlLivePageTest::navigationStaysWithinThePage()
{
    using Thoughts::LivePage::isBrowserLink;
    using Thoughts::LivePage::navigationStays;
    const QUrl page(QStringLiteral("http://localhost:18770/"));
    // Within itself: its other paths and routes, about:blank.
    QVERIFY(navigationStays(page, QUrl(QStringLiteral("http://localhost:18770/run/3#loss")), true));
    QVERIFY(navigationStays(page, QUrl(QStringLiteral("about:blank")), true));
    // The main frame never leaves the origin (another port, scheme or host is another site).
    QVERIFY(!navigationStays(page, QUrl(QStringLiteral("http://localhost:18771/")), true));
    // An http page its server moves to https on the same host stays; never the other way.
    QVERIFY(navigationStays(page, QUrl(QStringLiteral("https://localhost:18770/")), true));
    QVERIFY(navigationStays(page, QUrl(QStringLiteral("https://localhost/")), true));
    QVERIFY(!navigationStays(QUrl(QStringLiteral("https://localhost/")), QUrl(QStringLiteral("http://localhost/")), true));
    QVERIFY(!navigationStays(page, QUrl(QStringLiteral("https://other.localhost/")), true));
    QVERIFY(!navigationStays(page, QUrl(QStringLiteral("https://thoughts.example.org/")), true));
    QVERIFY(!navigationStays(page, QUrl(QStringLiteral("file:///tmp/secret.txt")), true));
    QVERIFY(!navigationStays(page, QUrl(QStringLiteral("data:text/html,<p>x</p>")), true));
    // Frames inside the page may load the web, never files or other schemes.
    QVERIFY(navigationStays(page, QUrl(QStringLiteral("https://cdn.example.org/embed")), false));
    QVERIFY(navigationStays(page, QUrl(QStringLiteral("blob:http://localhost:18770/0d1f")), false));
    QVERIFY(!navigationStays(page, QUrl(QStringLiteral("file:///etc/hosts")), false));
    QVERIFY(!navigationStays(page, QUrl(QStringLiteral("thoughts-html:docs-runs/tex/support/html/a.html")), false));
    // What the system browser may get.
    QVERIFY(isBrowserLink(QUrl(QStringLiteral("https://thoughts.example.org/"))));
    QVERIFY(isBrowserLink(QUrl(QStringLiteral("mailto:someone@example.org"))));
    QVERIFY(!isBrowserLink(QUrl(QStringLiteral("file:///etc/hosts"))));
    QVERIFY(!isBrowserLink(QUrl(QStringLiteral("x-apple.systempreferences:com.apple"))));
}

void HtmlLivePageTest::stubRoundTrip()
{
    const QUrl address(QStringLiteral("http://localhost:18770/?run=a&b=\"c\""));
    const QByteArray stub = Thoughts::LivePage::stubHtml(address, QStringLiteral("Loss <& accuracy>"));
    QVERIFY(!stub.isEmpty());
    QVERIFY(stub.size() <= Thoughts::LivePage::StubLimit);
    QVERIFY(!stub.toLower().contains("<script"));
    QCOMPARE(Thoughts::LivePage::stubAddress(stub), address);
    QCOMPARE(Thoughts::LivePage::title(stub), QStringLiteral("Loss <& accuracy>"));
    QVERIFY(stub.contains("It opens in Thoughts, on a device that can reach this address."));
    // Without a title: the host names it.
    QCOMPARE(Thoughts::LivePage::title(Thoughts::LivePage::stubHtml(QUrl(QStringLiteral("http://localhost:18770/")), QString())), QStringLiteral("localhost"));
    // Only a valid address makes a stub.
    QVERIFY(Thoughts::LivePage::stubHtml(QUrl(QStringLiteral("ftp://localhost/")), QStringLiteral("x")).isEmpty());
}

void HtmlLivePageTest::snapshotFromTheSerializedPage()
{
    // As the page's serializer (part/html-live/snapshot.js) returns the root element, with what a
    // live page may carry that a snapshot must not.
    const QString serialized = QStringLiteral(
        "<html lang=\"en\" class=\"dark\"><head><meta charset=\"windows-1252\"><meta http-equiv=\"refresh\" content=\"5\">"
        "<meta name=\"thoughts-live-page\" content=\"http://elsewhere.example.org/\"><meta name=\"viewport\" content=\"width=device-width\">"
        "<base href=\"http://localhost:18770/\"><title>TensorBoard</title><script src=\"app.js\"></script><style>body{color:red}</style></head>"
        "<body class=\"a\" data-x='<body>'><div title=\"a <script> b\">Loss 0.12</div><svg><title>tooltip</title><circle r=\"2\"/></svg>"
        "<script>window.leak()</script><img src=\"data:image/png;base64,iVBORw0KGgo=\" alt=\"\"><textarea><script>x</script></textarea></body></html>");
    const QUrl address(QStringLiteral("http://localhost:18770/"));
    const QDateTime takenAt(QDate(2026, 11, 19), QTime(12, 34, 56), QTimeZone::UTC);
    QString error;
    const QByteArray snapshot = Thoughts::LivePage::finishSnapshot(serialized, address, QStringLiteral("Training curves"), takenAt, QStringLiteral("19 November 2026, 13:34"), &error);
    QVERIFY2(!snapshot.isEmpty(), qPrintable(error));
    const QString text = QString::fromUtf8(snapshot);
    QVERIFY2(text.startsWith(QStringLiteral("<!doctype html>\n<html lang=\"en\" class=\"dark\"><head>\n<meta charset=\"utf-8\">\n"
                                            "<meta name=\"thoughts-live-source\" content=\"http://localhost:18770/\">\n"
                                            "<meta name=\"thoughts-snapshot-at\" content=\"2026-11-19T12:34:56Z\">\n<title>Training curves</title>\n")),
             qPrintable(text.left(400)));
    // What the reader reads of it: a snapshot of the address, an embedded page (no stub).
    QCOMPARE(Thoughts::LivePage::snapshotSource(snapshot), address);
    QVERIFY(!Thoughts::LivePage::stubAddress(snapshot).isValid());
    QCOMPARE(Thoughts::LivePage::metaContent(snapshot, QStringLiteral("thoughts-snapshot-at")), QStringLiteral("2026-11-19T12:34:56Z"));
    QCOMPARE(Thoughts::LivePage::title(snapshot), QStringLiteral("Training curves"));
    // The live page's own marks and instructions are gone; its scripts too, but not text that
    // only looks like one (an attribute, a textarea) or an SVG's tooltip title.
    for (const QString &gone : {QStringLiteral("windows-1252"), QStringLiteral("refresh"), QStringLiteral("elsewhere.example.org"), QStringLiteral("<base"),
                                QStringLiteral("TensorBoard"), QStringLiteral("app.js"), QStringLiteral("window.leak")}) {
        QVERIFY2(!text.contains(gone), qPrintable(gone));
    }
    for (const QString &kept : {QStringLiteral("<meta name=\"viewport\" content=\"width=device-width\">"), QStringLiteral("<style>body{color:red}</style>"),
                                QStringLiteral("<div title=\"a <script> b\">Loss 0.12</div>"), QStringLiteral("<svg><title>tooltip</title>"),
                                QStringLiteral("<textarea><script>x</script></textarea>"), QStringLiteral("data:image/png;base64,iVBORw0KGgo=")}) {
        QVERIFY2(text.contains(kept), qPrintable(kept));
    }
    // The visible line at the top of the body (not in the attribute that only looks like a tag).
    const qsizetype body = text.indexOf(QStringLiteral("<body class=\"a\" data-x='<body>'>"));
    QVERIFY(body > 0);
    const QString after = text.mid(body + QStringLiteral("<body class=\"a\" data-x='<body>'>").size());
    QVERIFY2(after.startsWith(QStringLiteral("\n<div data-thoughts-snapshot=\"\"")), qPrintable(after.left(200)));
    QVERIFY(after.contains(QStringLiteral(">Snapshot of <a href=\"http://localhost:18770/\" style=\"color:inherit\">http://localhost:18770/</a>, 19 November 2026, 13:34</div>")));
    QCOMPARE(text.count(QStringLiteral("Snapshot of ")), 1);
    QCOMPARE(text.count(QStringLiteral("<meta charset")), 1);

    // A snapshot of a snapshot keeps one of each mark, the newer.
    const QDateTime later = takenAt.addSecs(3600);
    const QByteArray again = Thoughts::LivePage::finishSnapshot(QString::fromUtf8(snapshot).mid(QStringLiteral("<!doctype html>\n").size()), address, QString(), later,
                                                                QStringLiteral("later"), &error);
    QCOMPARE(QString::fromUtf8(again).count(QStringLiteral("thoughts-live-source")), 1);
    QCOMPARE(Thoughts::LivePage::metaContent(again, QStringLiteral("thoughts-snapshot-at")), QStringLiteral("2026-11-19T13:34:56Z"));
    // Without a title of its own the page keeps its <title>.
    QCOMPARE(Thoughts::LivePage::title(again), QStringLiteral("Training curves"));
}

void HtmlLivePageTest::snapshotWithoutHeadOrBody()
{
    const QUrl address(QStringLiteral("https://thoughts.example.org/board"));
    const QDateTime takenAt(QDate(2026, 11, 19), QTime(8, 0), QTimeZone::UTC);
    const QString noHead = QString::fromUtf8(Thoughts::LivePage::finishSnapshot(QStringLiteral("<html><p>Only text</p></html>"), address, QStringLiteral("Board"), takenAt, QStringLiteral("now")));
    QVERIFY2(noHead.startsWith(QStringLiteral("<!doctype html>\n<html><head>\n<meta charset=\"utf-8\">")), qPrintable(noHead));
    QVERIFY(noHead.contains(QStringLiteral("</head><div data-thoughts-snapshot")));
    QCOMPARE(Thoughts::LivePage::snapshotSource(noHead.toUtf8()), address);
    const QString bare = QString::fromUtf8(Thoughts::LivePage::finishSnapshot(QStringLiteral("<p>Fragment</p>"), address, QString(), takenAt, QStringLiteral("now")));
    QVERIFY2(bare.startsWith(QStringLiteral("<!doctype html>\n<head>\n<meta charset=\"utf-8\">")), qPrintable(bare));
    QVERIFY(bare.contains(QStringLiteral("</head><div data-thoughts-snapshot")));
    QVERIFY(bare.endsWith(QStringLiteral("<p>Fragment</p>")));
}

void HtmlLivePageTest::initTestCase()
{
    static QTemporaryDir settings;
    QVERIFY(settings.isValid());
    // Settings in a file of the test's own: without an organization name QSettings
    // cannot write on Windows (the registry), and the user's settings stay untouched.
    QCoreApplication::setOrganizationName(QStringLiteral("ThoughtsAutotests"));
    QCoreApplication::setApplicationName(QStringLiteral("htmllivepagetest"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
}

void HtmlLivePageTest::consentIsRememberedPerOrigin()
{
    Thoughts::LivePage::forgetOrigins();
    const QString origin = Thoughts::LivePage::origin(QUrl(QStringLiteral("http://localhost:18770/")));
    QVERIFY(!Thoughts::LivePage::originAllowed(origin));
    Thoughts::LivePage::allowOrigin(origin);
    Thoughts::LivePage::allowOrigin(origin);
    QVERIFY(Thoughts::LivePage::originAllowed(origin));
    QCOMPARE(QSettings().value(QStringLiteral("html-live-pages/allowed-origins")).toStringList(), QStringList{origin});
    // Another port is another origin, asked about again.
    QVERIFY(!Thoughts::LivePage::originAllowed(Thoughts::LivePage::origin(QUrl(QStringLiteral("http://localhost:18771/")))));
    QVERIFY(!Thoughts::LivePage::originAllowed(QString()));
    Thoughts::LivePage::forgetOrigins();
    QVERIFY(!Thoughts::LivePage::originAllowed(origin));
}

void HtmlLivePageTest::writtenPagesUntilTheBundleHasThem()
{
    Thoughts::LivePage::forgetWrites();
    QTemporaryDir docs;
    QVERIFY(docs.isValid());
    const QByteArray snapshot = "<!doctype html><html><head><meta name=\"thoughts-live-source\" content=\"http://localhost:18770/\"></head><body>Loss</body></html>";
    const QString library = QDir(docs.path()).filePath(Path);
    QVERIFY(write(library, snapshot));
    // The PDF's bundle still carries the stub.
    Thoughts::HtmlAttachmentTarget built;
    built.path = Path;
    built.file = docs.filePath(QStringLiteral("bundle-stub"));
    built.sha256 = sha(SpecStub);
    QVERIFY(write(built.file, SpecStub));
    QVERIFY(!Thoughts::LivePage::writtenPage(Path, built, docs.path()).isValid());
    QString error;
    QVERIFY2(Thoughts::LivePage::rememberWrite(Path, snapshot, &error), qPrintable(error));
    auto written = Thoughts::LivePage::writtenPage(Path, built, docs.path());
    QVERIFY(written.isValid());
    QCOMPARE(written.path, Path);
    QCOMPARE(written.sha256, sha(snapshot));
    QVERIFY(written.file != library); // a copy of its own, not the Library's mutable file
    QFile copy(written.file);
    QVERIFY(copy.open(QIODevice::ReadOnly));
    QCOMPARE(copy.readAll(), snapshot);
    copy.close();
    // Also when the PDF has no bundle copy at all.
    QVERIFY(Thoughts::LivePage::writtenPage(Path, {}, docs.path()).isValid());
    // The bundle caught up: its version is shown, the record and the copy are gone.
    Thoughts::HtmlAttachmentTarget rebuilt = built;
    rebuilt.sha256 = sha(snapshot);
    QVERIFY(!Thoughts::LivePage::writtenPage(Path, rebuilt, docs.path()).isValid());
    QVERIFY(!QFileInfo::exists(written.file));
    QVERIFY(!Thoughts::LivePage::writtenPage(Path, built, docs.path()).isValid());
    // The Library's page changed after (another device's version came with Sync): the bundle's.
    QVERIFY(Thoughts::LivePage::rememberWrite(Path, snapshot, &error));
    QVERIFY(Thoughts::LivePage::writtenPage(Path, built, docs.path()).isValid());
    QVERIFY(write(library, "<html>another version</html>"));
    QVERIFY(!Thoughts::LivePage::writtenPage(Path, built, docs.path()).isValid());
    QVERIFY(!Thoughts::LivePage::writtenPage(Path, built, docs.path()).isValid());
    // Written again (Go Live Again): the newer write replaces the older and its copy.
    QVERIFY(write(library, snapshot));
    QVERIFY(Thoughts::LivePage::rememberWrite(Path, snapshot, &error));
    const QString first = Thoughts::LivePage::writtenPage(Path, built, docs.path()).file;
    QVERIFY(write(library, SpecStub));
    QVERIFY(Thoughts::LivePage::rememberWrite(Path, SpecStub, &error));
    QVERIFY(!QFileInfo::exists(first));
    // ...which is the bundle's version again here: nothing of its own to show.
    QVERIFY(!Thoughts::LivePage::writtenPage(Path, built, docs.path()).isValid());
    Thoughts::LivePage::forgetWrites();
}

void HtmlLivePageTest::libraryPageWrite()
{
    QTemporaryDir docs;
    QString error;
    // The page's folder must be on this computer (Sync publishes only connected folders).
    QVERIFY(!Thoughts::LivePage::writeLibraryPage(docs.path(), Path, SpecStub, &error));
    QVERIFY2(error.contains(QStringLiteral("docs-runs")), qPrintable(error));
    QVERIFY(QDir(docs.path()).mkpath(QStringLiteral("docs-runs")));
    QVERIFY2(Thoughts::LivePage::writeLibraryPage(docs.path(), Path, SpecStub, &error), qPrintable(error));
    QFile file(QDir(docs.path()).filePath(Path));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), SpecStub);
    // Only a docs folder's page, UTF-8, not empty.
    QVERIFY(!Thoughts::LivePage::writeLibraryPage(docs.path(), QStringLiteral("docs-runs/tex/support/html/../../x.html"), SpecStub, &error));
    QVERIFY(!Thoughts::LivePage::writeLibraryPage(docs.path(), QStringLiteral("elsewhere/x.html"), SpecStub, &error));
    QVERIFY(!Thoughts::LivePage::writeLibraryPage(docs.path(), Path, QByteArray("\xff\xfe"), &error));
    QVERIFY(!Thoughts::LivePage::writeLibraryPage(docs.path(), Path, QByteArray(), &error));
}

QTEST_GUILESS_MAIN(HtmlLivePageTest)
#include "htmllivepagetest.moc"
