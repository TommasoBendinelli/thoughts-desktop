// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/updatecheck.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTest>

using namespace Thoughts::Updates;

class UpdateCheckTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void installerRunsOnlyWithAMatchingDigest()
    {
        const QByteArray installer("MZ installer bytes");
        const QByteArray digest = QCryptographicHash::hash(installer, QCryptographicHash::Sha256).toHex();
        QVERIFY(installerMatches(digest, installer));
        // A release without a digest matches nothing: its installer is never run.
        QVERIFY(!installerMatches(QByteArray(), installer));
        QVERIFY(!installerMatches(QByteArray(), QByteArray()));
        QVERIFY(!installerMatches(digest, installer + "x"));
        QVERIFY(!installerMatches(digest, QByteArray()));
        QVERIFY(!installerMatches(digest.toUpper(), installer));
        QVERIFY(!installerMatches(digest.left(63), installer));
        QVERIFY(!installerMatches("sha256:" + digest, installer));
        QVERIFY(!installerMatches(QCryptographicHash::hash(installer, QCryptographicHash::Sha1).toHex(), installer));
        QVERIFY(!installerMatches(QByteArray(64, 'z'), installer));
    }

    void installerIsSavedWhereNoOtherProgramCanPutIt()
    {
        const QByteArray installer("MZ installer bytes");
        const QString first = saveInstaller(installer, QStringLiteral("Thoughts-2026.11.12.1-setup.exe"));
        const QString second = saveInstaller(installer, QStringLiteral("Thoughts-2026.11.12.1-setup.exe"));
        const auto cleanup = qScopeGuard([first, second] {
            for (const QString &path : {first, second}) {
                if (!path.isEmpty()) {
                    QDir(QFileInfo(path).absolutePath()).removeRecursively();
                }
            }
        });
        QVERIFY(!first.isEmpty() && !second.isEmpty());
        QCOMPARE(QFileInfo(first).fileName(), QStringLiteral("Thoughts-2026.11.12.1-setup.exe"));
        // Each download gets a folder of its own, with a name that cannot be known beforehand.
        QVERIFY(QFileInfo(first).absolutePath() != QFileInfo(second).absolutePath());
        const QString temporary = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).canonicalPath();
        QCOMPARE(QFileInfo(QFileInfo(first).absolutePath()).dir().canonicalPath(), temporary);
        QVERIFY(QFileInfo(first).dir().dirName().startsWith(QStringLiteral("Thoughts-update-")));
        QVERIFY(QFileInfo(first).dir().dirName().size() > QStringLiteral("Thoughts-update-").size());
        QFile saved(first);
        QVERIFY(saved.open(QIODevice::ReadOnly));
        QCOMPARE(saved.readAll(), installer);
#ifdef Q_OS_UNIX
        // The folder is this user's alone.
        const QFile::Permissions permissions = QFileInfo(QFileInfo(first).absolutePath()).permissions();
        QVERIFY(!(permissions & (QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup | QFile::ReadOther | QFile::WriteOther | QFile::ExeOther)));
#endif
    }

    void installerNameIsNeverAPath()
    {
        QCOMPARE(installerFileName(QStringLiteral("Thoughts-2026.11.12.1-setup.exe")), QStringLiteral("Thoughts-2026.11.12.1-setup.exe"));
        for (const QString &name : {QStringLiteral("Thoughts-..\\..\\x-setup.exe"), QStringLiteral("Thoughts-../x-setup.exe"), QStringLiteral("Thoughts-a/b-setup.exe"),
                                    QStringLiteral("Thoughts-..-setup.exe"), QStringLiteral("C:\\Windows\\Thoughts-1-setup.exe"), QStringLiteral("Thoughts-1-setup.exe:stream"),
                                    QStringLiteral("evil.exe"), QStringLiteral("Thoughts-1 -setup.exe"), QString()}) {
            QCOMPARE(installerFileName(name), QStringLiteral("Thoughts-setup.exe"));
            const QString saved = saveInstaller("x", name);
            QVERIFY(!saved.isEmpty());
            QCOMPARE(QFileInfo(saved).fileName(), QStringLiteral("Thoughts-setup.exe"));
            QVERIFY(QFileInfo(saved).dir().dirName().startsWith(QStringLiteral("Thoughts-update-")));
            QDir(QFileInfo(saved).absolutePath()).removeRecursively();
        }
        // A Mac release's disk image keeps its name by the same rule.
        QCOMPARE(installerFileName(QStringLiteral("Thoughts-2026.11.12.1.dmg")), QStringLiteral("Thoughts-2026.11.12.1.dmg"));
        for (const QString &name : {QStringLiteral("Thoughts-../x.dmg"), QStringLiteral("Thoughts-a/b.dmg"), QStringLiteral("Thoughts-...dmg"), QStringLiteral("evil.dmg"), QStringLiteral("Thoughts-1 .dmg")}) {
            QCOMPARE(installerFileName(name), QStringLiteral("Thoughts.dmg"));
        }
    }

    void redirectsStayOnHttpsAndTheTokenOnItsOrigin()
    {
        const QUrl api(QStringLiteral("https://api.github.example/repos/o/r/releases/assets/1"));
        bool sendToken = true;
        QVERIFY(redirectAllowed(api, QUrl(QStringLiteral("https://api.github.example/other")), &sendToken));
        QVERIFY(sendToken);
        // The signed download link is on another host: followed, without the token.
        QVERIFY(redirectAllowed(api, QUrl(QStringLiteral("https://objects.example.net/file?signature=1")), &sendToken));
        QVERIFY(!sendToken);
        QVERIFY(redirectAllowed(api, QUrl(QStringLiteral("https://api.github.example:8443/other")), &sendToken));
        QVERIFY(!sendToken);
        QVERIFY(redirectAllowed(api, QUrl(QStringLiteral("https://api.github.example.evil.example/other")), &sendToken));
        QVERIFY(!sendToken);
        for (const QString &refused : {QStringLiteral("http://api.github.example/other"), QStringLiteral("file:///C:/Windows/x.exe"), QStringLiteral("ftp://objects.example.net/file"),
                                       QStringLiteral("https://user@objects.example.net/file"), QString()}) {
            sendToken = true;
            QVERIFY2(!redirectAllowed(api, QUrl(refused), &sendToken), qPrintable(refused));
            QVERIFY(!sendToken);
        }
    }
};

QTEST_MAIN(UpdateCheckTest)
#include "updatechecktest.moc"
