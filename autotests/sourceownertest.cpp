// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/sourceowner.h"
#include <QTemporaryDir>
#include <QTest>

using Thoughts::SourceOwner;

// Spec Rule 1 and 2 vectors, shared by the desktop and tools/publisher, which
// include the same helper.
class SourceOwnerTest : public QObject {
    Q_OBJECT
    static QString hex(char digit = 'a') { return QString(64, QLatin1Char(digit)); }
private Q_SLOTS:
    void owners_data() {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("owner");
        QTest::addColumn<QString>("relative");
        const QString folder = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents");
        QTest::newRow("connected") << QStringLiteral("docs-paper/tex/source/main.tex") << QStringLiteral("docs-paper") << QStringLiteral("tex/source/main.tex");
        QTest::newRow("connected folder itself") << QStringLiteral("docs-paper") << QStringLiteral("docs-paper") << QString();
        QTest::newRow("connected punctuation") << QStringLiteral("docs-A_b.c-d/x.md") << QStringLiteral("docs-A_b.c-d") << QStringLiteral("x.md");
        QTest::newRow("default") << folder + QStringLiteral("/notes/a.md") << folder << QStringLiteral("notes/a.md");
        QTest::newRow("default folder itself") << folder << folder << QString();
        QTest::newRow("default configuration") << folder + QStringLiteral("/thoughts-project.json") << folder << QStringLiteral("thoughts-project.json");
        const QString canonical = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/default");
        QTest::newRow("canonical") << canonical + QStringLiteral("/notes/a.md") << canonical << QStringLiteral("notes/a.md");
        QTest::newRow("canonical folder itself") << canonical << canonical << QString();
        QTest::newRow("canonical configuration") << canonical + QStringLiteral("/thoughts-project.json") << canonical << QStringLiteral("thoughts-project.json");
        QTest::newRow("canonical other segment") << QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/defaults/a.md") << QString() << QString();
        QTest::newRow("canonical 63 hex") << QStringLiteral(".thoughts/projects/") + QString(63, QLatin1Char('a')) + QStringLiteral("/default/a.md") << QString() << QString();
        QTest::newRow("documents2") << QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents2/a.md") << QString() << QString();
        QTest::newRow("docs- alone") << QStringLiteral("docs-/x.md") << QString() << QString();
        QTest::newRow("docs space") << QStringLiteral("docs-my paper/x.md") << QString() << QString();
        QTest::newRow("uppercase hex") << QStringLiteral(".thoughts/projects/") + hex('A') + QStringLiteral("/documents/a.md") << QString() << QString();
        QTest::newRow("63 hex") << QStringLiteral(".thoughts/projects/") + QString(63, QLatin1Char('a')) + QStringLiteral("/documents/a.md") << QString() << QString();
        QTest::newRow("65 hex") << QStringLiteral(".thoughts/projects/") + QString(65, QLatin1Char('a')) + QStringLiteral("/documents/a.md") << QString() << QString();
        QTest::newRow("missing segment") << QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/a.md") << QString() << QString();
        QTest::newRow("other segment") << QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documentsx/a.md") << QString() << QString();
        QTest::newRow("extra leading segment") << QStringLiteral("x/.thoughts/projects/") + hex() + QStringLiteral("/documents/a.md") << QString() << QString();
        QTest::newRow("thoughts settings") << QStringLiteral(".thoughts/library-projects.json") << QString() << QString();
        QTest::newRow("unowned") << QStringLiteral("third_party/a.pdf") << QString() << QString();
    }
    void owners() {
        QFETCH(QString, path);
        QFETCH(QString, owner);
        QFETCH(QString, relative);
        QCOMPARE(SourceOwner::of(path), owner);
        QCOMPARE(SourceOwner::relative(path), relative);
        QCOMPARE(SourceOwner::isOwner(path), !owner.isEmpty() && owner == path);
    }
    void defaultFolderRootAndOutput() {
        const QString folder = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents");
        QVERIFY(SourceOwner::isDefaultFolder(folder) && !SourceOwner::isConnectedFolder(folder));
        QCOMPARE(SourceOwner::defaultFolderRoot(QStringLiteral("/docs/") + folder), QStringLiteral("/docs"));
        QCOMPARE(SourceOwner::defaultFolderRoot(QStringLiteral("/docs/docs-paper")), QString());
        QCOMPARE(SourceOwner::ofDirectory(QStringLiteral("/docs"), QStringLiteral("/docs/") + folder), folder);
        // Rule 2: the hash covers the full source id, so equal relative paths
        // in two Projects' default folders never share a Library PDF.
        const QString id = folder + QStringLiteral("/notes/a.md");
        const QString expected = QStringLiteral("document_")
            + QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex().left(20)) + QStringLiteral(".pdf");
        QCOMPARE(SourceOwner::defaultFolderOutput(id), expected);
        const QString other = QStringLiteral(".thoughts/projects/") + hex('b') + QStringLiteral("/documents/notes/a.md");
        QVERIFY(SourceOwner::defaultFolderOutput(other) != expected);
        QCOMPARE(SourceOwner::defaultFolderOutput(QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents/notes.md")).size(), 33);
    }
    // Rename Rule 4: the canonical spelling hashes as the legacy one, so the
    // Library PDF keeps its name when documents/ becomes default/.
    void outputNameInvariance() {
        const QString legacy = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents/notes/a.md");
        const QString canonical = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/default/notes/a.md");
        QCOMPARE(SourceOwner::defaultFolderOutput(canonical), SourceOwner::defaultFolderOutput(legacy));
        QCOMPARE(SourceOwner::defaultFolderOutput(legacy), QStringLiteral("document_")
            + QString::fromLatin1(QCryptographicHash::hash(legacy.toUtf8(), QCryptographicHash::Sha256).toHex().left(20)) + QStringLiteral(".pdf"));
        QCOMPARE(SourceOwner::legacySpelling(canonical), legacy);
        QCOMPARE(SourceOwner::legacySpelling(legacy), legacy);
        QCOMPARE(SourceOwner::legacySpelling(QStringLiteral("docs-x/a.md")), QStringLiteral("docs-x/a.md"));
        // Only the leading owner is respelled.
        const QString nested = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/default/default/a.md");
        QCOMPARE(SourceOwner::legacySpelling(nested), QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents/default/a.md"));
    }
    void bothSegments() {
        const QString canonical = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/default");
        QVERIFY(SourceOwner::isDefaultFolder(canonical));
        QVERIFY(!SourceOwner::isLegacyDefaultFolder(canonical));
        QVERIFY(SourceOwner::isLegacyDefaultFolder(QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents")));
        QCOMPARE(canonical.size(), 91);
        QCOMPARE(SourceOwner::defaultFolderRoot(QStringLiteral("/docs/") + canonical), QStringLiteral("/docs"));
        QCOMPARE(SourceOwner::ofDirectory(QStringLiteral("/docs"), QStringLiteral("/docs/") + canonical), canonical);
        QVERIFY(SourceOwner::isManifestPath(canonical + QStringLiteral("/thoughts-project.json")));
        QVERIFY(SourceOwner::isManifestPath(QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents/thoughts-project.json")));
        QVERIFY(!SourceOwner::isManifestPath(canonical + QStringLiteral("/x/thoughts-project.json")));
        QVERIFY(!SourceOwner::isManifestPath(QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/other/thoughts-project.json")));
        QCOMPARE(SourceOwner::projectHash(QStringLiteral("p")).size(), 64);
        QCOMPARE(SourceOwner::defaultFolder(hex(), SourceOwner::DefaultSegment), canonical);
    }
    void ruleR_data() {
        QTest::addColumn<QString>("input");
        QTest::addColumn<QString>("expected");
        const QString legacy = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents");
        const QString canonical = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/default");
        QTest::newRow("relative") << legacy + QStringLiteral("/a.tex") << canonical + QStringLiteral("/a.tex");
        QTest::newRow("folder at end") << legacy << canonical;
        QTest::newRow("absolute") << QStringLiteral("/Users/x/docs/") + legacy + QStringLiteral("/x.pdf") << QStringLiteral("/Users/x/docs/") + canonical + QStringLiteral("/x.pdf");
        QTest::newRow("dotdot") << QStringLiteral("../") + legacy + QStringLiteral("/x.pdf") << QStringLiteral("../") + canonical + QStringLiteral("/x.pdf");
        QTest::newRow("json quote") << QStringLiteral("{\"path\": \"") + legacy + QStringLiteral("\"}") << QStringLiteral("{\"path\": \"") + canonical + QStringLiteral("\"}");
        QTest::newRow("single quote") << QStringLiteral("'") + legacy + QStringLiteral("'") << QStringLiteral("'") + canonical + QStringLiteral("'");
        QTest::newRow("backslash") << legacy + QStringLiteral("\\a") << canonical + QStringLiteral("\\a");
        QTest::newRow("space") << legacy + QStringLiteral(" x") << canonical + QStringLiteral(" x");
        QTest::newRow("newline") << legacy + QStringLiteral("\n") << canonical + QStringLiteral("\n");
        QTest::newRow("tab") << legacy + QStringLiteral("\tx") << canonical + QStringLiteral("\tx");
        QTest::newRow("several") << legacy + QStringLiteral("/a ") + legacy + QStringLiteral("/b") << canonical + QStringLiteral("/a ") + canonical + QStringLiteral("/b");
        QTest::newRow("documents2") << legacy + QStringLiteral("2/a") << legacy + QStringLiteral("2/a");
        QTest::newRow("documentsx") << legacy + QStringLiteral("x") << legacy + QStringLiteral("x");
        QTest::newRow("colon") << legacy + QStringLiteral(":1") << legacy + QStringLiteral(":1");
        QTest::newRow("63 hex") << QStringLiteral(".thoughts/projects/") + QString(63, QLatin1Char('a')) + QStringLiteral("/documents/a")
                                << QStringLiteral(".thoughts/projects/") + QString(63, QLatin1Char('a')) + QStringLiteral("/documents/a");
        QTest::newRow("65 hex") << QStringLiteral(".thoughts/projects/") + QString(65, QLatin1Char('a')) + QStringLiteral("/documents/a")
                                << QStringLiteral(".thoughts/projects/") + QString(65, QLatin1Char('a')) + QStringLiteral("/documents/a");
        QTest::newRow("uppercase hex") << QStringLiteral(".thoughts/projects/") + hex('A') + QStringLiteral("/documents/a")
                                       << QStringLiteral(".thoughts/projects/") + hex('A') + QStringLiteral("/documents/a");
        QTest::newRow("unescaped dot required") << QStringLiteral("xthoughts/projects/") + hex() + QStringLiteral("/documents/a")
                                                << QStringLiteral("xthoughts/projects/") + hex() + QStringLiteral("/documents/a");
        // Apps here never escape '/' in JSON; an escaped path stays as is.
        QTest::newRow("escaped slashes") << QStringLiteral(".thoughts\\/projects\\/") + hex() + QStringLiteral("\\/documents")
                                         << QStringLiteral(".thoughts\\/projects\\/") + hex() + QStringLiteral("\\/documents");
        QTest::newRow("documents elsewhere") << QStringLiteral("docs-x/documents/a.md") << QStringLiteral("docs-x/documents/a.md");
        QTest::newRow("already canonical") << canonical + QStringLiteral("/a") << canonical + QStringLiteral("/a");
        QTest::newRow("non-ASCII around") << QStringLiteral("é") + legacy + QStringLiteral("/ü.md") << QStringLiteral("é") + canonical + QStringLiteral("/ü.md");
    }
    void ruleR() {
        QFETCH(QString, input);
        QFETCH(QString, expected);
        QCOMPARE(SourceOwner::rewriteLegacyDefaultFolder(input), expected);
        QCOMPARE(SourceOwner::rewriteLegacyDefaultFolder(input.toUtf8()), expected.toUtf8());
        QCOMPARE(SourceOwner::containsLegacyDefaultFolder(input.toUtf8()), input != expected);
        // Idempotent.
        QCOMPARE(SourceOwner::rewriteLegacyDefaultFolder(expected), expected);
    }
    // As in Python: \s is Unicode whitespace for text, ASCII for bytes.
    void ruleRWhitespaceFollowsPython() {
        const QString legacy = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents");
        const QString canonical = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/default");
        for (const char16_t space : {char16_t(0xa0), char16_t(0x2003), char16_t(0x1c)}) {
            QCOMPARE(SourceOwner::rewriteLegacyDefaultFolder(legacy + QChar(space)), canonical + QChar(space));
            QCOMPARE(SourceOwner::rewriteLegacyDefaultFolder(QString(legacy + QChar(space)).toUtf8()), QString(legacy + QChar(space)).toUtf8());
        }
        QCOMPARE(SourceOwner::rewriteLegacyDefaultFolder(QByteArray(legacy.toUtf8() + "\x0b")), QByteArray(canonical.toUtf8() + "\x0b"));
    }
    void ruleRJsonNeverEscapesSlash() {
        const QString path = QStringLiteral(".thoughts/projects/") + hex() + QStringLiteral("/documents/a.tex");
        const QByteArray json = QJsonDocument(QJsonObject{{QStringLiteral("path"), path}}).toJson(QJsonDocument::Compact);
        QVERIFY(json.contains(path.toUtf8()));
        QVERIFY(SourceOwner::containsLegacyDefaultFolder(json));
    }
    void location() {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString a = SourceOwner::projectHash(QStringLiteral("a"));
        const QString b = SourceOwner::projectHash(QStringLiteral("b"));
        QCOMPARE(Thoughts::DefaultFolderLocation::librarySegment(root.path()), SourceOwner::LegacyDefaultSegment);
        QCOMPARE(Thoughts::DefaultFolderLocation::ofProject(root.path(), QStringLiteral("a")), SourceOwner::defaultFolder(a, SourceOwner::LegacyDefaultSegment));
        QVERIFY(QDir(root.path()).mkpath(SourceOwner::defaultFolder(a, SourceOwner::LegacyDefaultSegment)));
        QCOMPARE(Thoughts::DefaultFolderLocation::existing(root.path(), a), SourceOwner::defaultFolder(a, SourceOwner::LegacyDefaultSegment));
        // A new Project follows the Library: default/ once any Project uses it.
        QVERIFY(QDir(root.path()).mkpath(SourceOwner::defaultFolder(b, SourceOwner::DefaultSegment)));
        QCOMPARE(Thoughts::DefaultFolderLocation::librarySegment(root.path()), SourceOwner::DefaultSegment);
        QCOMPARE(Thoughts::DefaultFolderLocation::ofProject(root.path(), QStringLiteral("c")),
                 SourceOwner::defaultFolder(SourceOwner::projectHash(QStringLiteral("c")), SourceOwner::DefaultSegment));
        QCOMPARE(Thoughts::DefaultFolderLocation::ofProject(root.path(), QStringLiteral("a")), SourceOwner::defaultFolder(a, SourceOwner::LegacyDefaultSegment));
        // Both on disk: default/ wins.
        QVERIFY(QDir(root.path()).mkpath(SourceOwner::defaultFolder(a, SourceOwner::DefaultSegment)));
        QCOMPARE(Thoughts::DefaultFolderLocation::existing(root.path(), a), SourceOwner::defaultFolder(a, SourceOwner::DefaultSegment));
        // Discovery: default/ before documents/ per hash.
        auto write = [&](const QString &relative) {
            QFile file(QDir(root.path()).filePath(relative));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("{}");
        };
        write(SourceOwner::defaultFolder(a, SourceOwner::LegacyDefaultSegment) + QStringLiteral("/thoughts-project.json"));
        write(SourceOwner::defaultFolder(b, SourceOwner::DefaultSegment) + QStringLiteral("/thoughts-project.json"));
        QStringList expected{QDir(root.path()).filePath(SourceOwner::defaultFolder(a, SourceOwner::LegacyDefaultSegment) + QStringLiteral("/thoughts-project.json")),
                             QDir(root.path()).filePath(SourceOwner::defaultFolder(b, SourceOwner::DefaultSegment) + QStringLiteral("/thoughts-project.json"))};
        std::sort(expected.begin(), expected.end());
        QCOMPARE(Thoughts::DefaultFolderLocation::manifestPaths(root.path()), expected);
        write(SourceOwner::defaultFolder(a, SourceOwner::DefaultSegment) + QStringLiteral("/thoughts-project.json"));
        QVERIFY(Thoughts::DefaultFolderLocation::manifestPaths(root.path()).contains(
            QDir(root.path()).filePath(SourceOwner::defaultFolder(a, SourceOwner::DefaultSegment) + QStringLiteral("/thoughts-project.json"))));
    }
    void librarySegmentFromCatalogue() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath(QStringLiteral(".thoughts")));
        QFile file(QDir(root.path()).filePath(QStringLiteral(".thoughts/library-projects.json")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QString manifest = SourceOwner::defaultFolder(hex(), SourceOwner::DefaultSegment) + QStringLiteral("/thoughts-project.json");
        file.write(QJsonDocument(QJsonObject{{QStringLiteral("projects"), QJsonArray{QJsonObject{{QStringLiteral("source_manifests"),
            QJsonArray{QJsonObject{{QStringLiteral("path"), manifest}}}}}}}}).toJson());
        file.close();
        QCOMPARE(Thoughts::DefaultFolderLocation::librarySegment(root.path()), SourceOwner::DefaultSegment);
    }
};

QTEST_GUILESS_MAIN(SourceOwnerTest)
#include "sourceownertest.moc"
