// SPDX-License-Identifier: GPL-2.0-or-later
#include "part/thoughtsplatform.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace
{
bool writeBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
}

class ThoughtsPlatformTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    void cleanSecretDropsByteOrderMarkAndWhitespace()
    {
        // Windows PowerShell piped "\xEF\xBB\xBF" + token + "\r\n" into store-secret:
        // 44 bytes stored, and Fedora refused the 43-character token with 401.
        QCOMPARE(Thoughts::Platform::cleanSecret("\xEF\xBB\xBFtoken-43\r\n"), QByteArray("token-43"));
        QCOMPARE(Thoughts::Platform::cleanSecret(" token-43 \n"), QByteArray("token-43"));
        QCOMPARE(Thoughts::Platform::cleanSecret("token-43"), QByteArray("token-43"));
        QCOMPARE(Thoughts::Platform::cleanSecret("\xEF\xBB\xBF"), QByteArray());
    }

    void dataRootsNameOneThoughtsFolder()
    {
        const QString thoughts = Thoughts::Platform::thoughtsDataRoot();
        QCOMPARE(QFileInfo(thoughts).fileName(), QStringLiteral("Thoughts"));
        QVERIFY(QDir::isAbsolutePath(thoughts));
        const QString application = Thoughts::Platform::applicationDataRoot();
        QVERIFY(QDir::isAbsolutePath(application));
#ifdef Q_OS_WIN
        // Windows would otherwise split state between the roaming and local profiles.
        QCOMPARE(application, thoughts);
#else
        QCOMPARE(application, QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
#endif
    }

    void fileStampChangesWithContentsAndIdentityStaysWithTheFile()
    {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("source.tex"));
        QVERIFY(writeBytes(path, "first\n"));
        const QString identity = Thoughts::Platform::physicalFileIdentity(path);
        const QString stamp = Thoughts::Platform::fileStamp(path);
        QVERIFY(!identity.isEmpty());
        QVERIFY(stamp.startsWith(path + QLatin1Char(':')));
        QTest::qWait(20);
        QVERIFY(writeBytes(path, "second, longer\n"));
        QCOMPARE(Thoughts::Platform::physicalFileIdentity(path), identity);
        QVERIFY(Thoughts::Platform::fileStamp(path) != stamp);
        // Directories and missing files have no stamp.
        QVERIFY(Thoughts::Platform::fileStamp(root.path()).isEmpty());
        QVERIFY(Thoughts::Platform::fileStamp(root.filePath(QStringLiteral("missing"))).isEmpty());
        QVERIFY(Thoughts::Platform::physicalFileIdentity(root.filePath(QStringLiteral("missing"))).isEmpty());
    }

    void clonedFileKeepsItsBytes()
    {
        QTemporaryDir root;
        const QString source = root.filePath(QStringLiteral("a.pdf"));
        const QString destination = root.filePath(QStringLiteral("b.pdf"));
        QVERIFY(writeBytes(source, "%PDF-1.7 fixture"));
        if (!Thoughts::Platform::cloneOrLinkFile(source, destination)) {
            QSKIP("This file system shares no bytes between files; callers copy instead.");
        }
        QFile copy(destination);
        QVERIFY(copy.open(QIODevice::ReadOnly));
        QCOMPARE(copy.readAll(), QByteArray("%PDF-1.7 fixture"));
    }

    void directoryLinkIsRetargetedInPlace()
    {
        QTemporaryDir root;
        QDir dir(root.path());
        QVERIFY(dir.mkpath(QStringLiteral(".thoughts-library-stage-a")));
        QVERIFY(dir.mkpath(QStringLiteral(".thoughts-library-stage-b")));
        QVERIFY(writeBytes(dir.filePath(QStringLiteral(".thoughts-library-stage-a/paper.pdf")), "old"));
        QVERIFY(writeBytes(dir.filePath(QStringLiteral(".thoughts-library-stage-b/paper.pdf")), "new"));
        const QString link = dir.filePath(QStringLiteral("current_pdfs"));
        QString error;

        QVERIFY2(Thoughts::Platform::replaceDirectoryLink(link, dir.filePath(QStringLiteral(".thoughts-library-stage-a")), &error), qPrintable(error));
        QVERIFY(Thoughts::Platform::isDirectoryLink(QFileInfo(link)));
        QCOMPARE(QFileInfo(Thoughts::Platform::directoryLinkTarget(QFileInfo(link))).canonicalFilePath(),
                 QFileInfo(dir.filePath(QStringLiteral(".thoughts-library-stage-a"))).canonicalFilePath());
        QFile opened(QDir(link).filePath(QStringLiteral("paper.pdf")));
        QVERIFY(opened.open(QIODevice::ReadOnly));

        QVERIFY2(Thoughts::Platform::replaceDirectoryLink(link, dir.filePath(QStringLiteral(".thoughts-library-stage-b")), &error), qPrintable(error));
        QFile current(QDir(link).filePath(QStringLiteral("paper.pdf")));
        QVERIFY(current.open(QIODevice::ReadOnly));
        QCOMPARE(current.readAll(), QByteArray("new"));
        // A PDF opened through the old target keeps reading the old bytes.
        QCOMPARE(opened.readAll(), QByteArray("old"));
        QVERIFY(!Thoughts::Platform::isDirectoryLink(QFileInfo(dir.filePath(QStringLiteral(".thoughts-library-stage-b")))));
        // A real folder is never replaced by a link.
        QVERIFY(!Thoughts::Platform::replaceDirectoryLink(dir.filePath(QStringLiteral(".thoughts-library-stage-a")), dir.filePath(QStringLiteral(".thoughts-library-stage-b")), &error));
    }

    void openFilesKeepTheirGenerationOnWindowsOnly()
    {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("paper.pdf"));
        QVERIFY(writeBytes(path, "%PDF"));
        QVERIFY(!Thoughts::Platform::directoryHasFilesInUse(root.path()));
        QFile reader(path);
        QVERIFY(reader.open(QIODevice::ReadOnly));
#ifdef Q_OS_WIN
        QVERIFY(Thoughts::Platform::directoryHasFilesInUse(root.path()));
#else
        QVERIFY(!Thoughts::Platform::directoryHasFilesInUse(root.path()));
#endif
        reader.close();
        QVERIFY(!Thoughts::Platform::directoryHasFilesInUse(root.path()));
    }

    void durabilityHelpersAcceptOrdinaryFiles()
    {
        QTemporaryDir root;
        QFile file(root.filePath(QStringLiteral("journal.jsonl")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{}\n") == 3 && file.flush());
        QVERIFY(Thoughts::Platform::flushToDisk(file.handle()));
        QVERIFY(Thoughts::Platform::flushDirectory(root.path()));
    }

    void nativeServerToolsAreUsedWhenInstalled()
    {
        // The build puts the native tools next to the test programs.
        const QString native = Thoughts::Platform::nativeServerTool(QStringLiteral("thoughts-publish-sources"));
        if (native.isEmpty()) {
            QSKIP("The native thoughts-publish-sources is not built next to this test.");
        }
        QVERIFY(QFileInfo(native).isExecutable());
        QVERIFY(Thoughts::Platform::nativeServerTool(QStringLiteral("no-such-tool")).isEmpty());
    }

    void runningProcessesAreRecognised()
    {
        QVERIFY(Thoughts::Platform::processIsRunning(QString::number(QCoreApplication::applicationPid())));
        QVERIFY(!Thoughts::Platform::processIsRunning(QStringLiteral("not-a-pid")));
        QVERIFY(!Thoughts::Platform::processIsRunning(QStringLiteral("0")));
    }

#ifdef Q_OS_MACOS
    // Opt-in (THOUGHTS_TEST_KEYCHAIN=1): it writes one throwaway item to the
    // login Keychain and deletes it again.
    void secretsRoundTripThroughKeychain()
    {
        if (qEnvironmentVariable("THOUGHTS_TEST_KEYCHAIN") != QLatin1String("1")) {
            QSKIP("Set THOUGHTS_TEST_KEYCHAIN=1 to use the login Keychain.");
        }
        // testhome.cpp moved HOME, where `security` finds no keychain (and would
        // ask to create one): give it the account's own home for this test.
        const QByteArray home = qgetenv("HOME"), fixedHome = qgetenv("CFFIXED_USER_HOME");
        const auto restoreHome = qScopeGuard([home, fixedHome] {
            qputenv("HOME", home);
            qputenv("CFFIXED_USER_HOME", fixedHome);
        });
        qputenv("HOME", QFile::encodeName(Thoughts::Platform::accountHomeDirectory()));
        qunsetenv("CFFIXED_USER_HOME");
        const QString service = QStringLiteral("thoughts-test");
        const QString account = QStringLiteral("thoughtsplatformtest");
        QString error;
        // Plain token characters go through `security -i`'s standard input.
        QVERIFY2(Thoughts::Platform::writeSecret(service, account, "sk-or-v1-0123abcd", &error), qPrintable(error));
        QCOMPARE(Thoughts::Platform::readSecret(service, account), QByteArray("sk-or-v1-0123abcd"));
        // Overwritten in place.
        QVERIFY2(Thoughts::Platform::writeSecret(service, account, "sk-or-v1-4567efgh", &error), qPrintable(error));
        QCOMPARE(Thoughts::Platform::readSecret(service, account), QByteArray("sk-or-v1-4567efgh"));
        // Other characters still work, as an argument.
        QVERIFY2(Thoughts::Platform::writeSecret(service, account, "with space \"quote\"", &error), qPrintable(error));
        QCOMPARE(Thoughts::Platform::readSecret(service, account), QByteArray("with space \"quote\""));
        QVERIFY2(Thoughts::Platform::deleteSecret(service, account, &error), qPrintable(error));
        QVERIFY(Thoughts::Platform::readSecret(service, account).isEmpty());
        // Deleting what is not there is not an error.
        QVERIFY2(Thoughts::Platform::deleteSecret(service, account, &error), qPrintable(error));
    }
#endif

#ifdef Q_OS_WIN
    void secretsRoundTripThroughCredentialManager()
    {
        // One fixed entry, overwritten by every run.
        const QString account = QStringLiteral("thoughtsplatformtest");
        QString error;
        QVERIFY2(Thoughts::Platform::writeSecret(QStringLiteral("thoughts-test"), account, "s3cret-token", &error), qPrintable(error));
        QCOMPARE(Thoughts::Platform::readSecret(QStringLiteral("thoughts-test"), account), QByteArray("s3cret-token"));
    }
#endif
};

QTEST_GUILESS_MAIN(ThoughtsPlatformTest)
#include "thoughtsplatformtest.moc"
