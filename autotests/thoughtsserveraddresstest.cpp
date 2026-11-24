// SPDX-License-Identifier: GPL-2.0-or-later
#include "part/thoughtsplatform.h"
#include "part/thoughtsserveraddress.h"

#include "config-thoughts.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace
{
// Stand-ins for a build's default and legacy addresses.
const QString DefaultAddress = QStringLiteral("https://thoughts.example.org");
const QString LegacyAddress = QStringLiteral("https://server.example.ts.net");

// THOUGHTS_SERVER_URL is unset for the duration and restored afterwards.
auto withoutEnvironmentAddress()
{
    const bool had = qEnvironmentVariableIsSet("THOUGHTS_SERVER_URL");
    const QByteArray previous = qgetenv("THOUGHTS_SERVER_URL");
    qunsetenv("THOUGHTS_SERVER_URL");
    return qScopeGuard([had, previous] {
        if (had) {
            qputenv("THOUGHTS_SERVER_URL", previous);
        } else {
            qunsetenv("THOUGHTS_SERVER_URL");
        }
    });
}
}

class ThoughtsServerAddressTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QStandardPaths::setTestModeEnabled(true);
    }

    // The default and legacy addresses are the build's (THOUGHTS_DEFAULT_SERVER_URL,
    // THOUGHTS_LEGACY_SERVER_URL), and the default is a valid address.
    void addressesComeFromTheBuild()
    {
        QCOMPARE(Thoughts::ServerAddress::defaultUrl(), QStringLiteral(THOUGHTS_DEFAULT_SERVER_URL));
        QCOMPARE(Thoughts::ServerAddress::legacyUrl(), QStringLiteral(THOUGHTS_LEGACY_SERVER_URL));
        QVERIFY(!Thoughts::ServerAddress::normalized(Thoughts::ServerAddress::defaultUrl()).isEmpty());
        QVERIFY(Thoughts::ServerAddress::legacyUrl().isEmpty() || !Thoughts::ServerAddress::normalized(Thoughts::ServerAddress::legacyUrl()).isEmpty());
        QCOMPARE(Thoughts::ServerAddress::firstRunUrl(false), Thoughts::ServerAddress::defaultUrl());
        QCOMPARE(Thoughts::ServerAddress::firstRunUrl(true),
                 Thoughts::ServerAddress::legacyUrl().isEmpty() ? Thoughts::ServerAddress::defaultUrl() : Thoughts::ServerAddress::legacyUrl());
        QCOMPARE(QFileInfo(Thoughts::ServerAddress::storePath()).dir().absolutePath(), QDir(Thoughts::Platform::thoughtsDataRoot()).absolutePath());
    }

    void addressesAreValidated_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("expected");
        QTest::newRow("public") << QStringLiteral("https://thoughts.example.org") << QStringLiteral("https://thoughts.example.org");
        QTest::newRow("Tailscale name") << QStringLiteral("https://server.example.ts.net") << QStringLiteral("https://server.example.ts.net");
        QTest::newRow("any https host") << QStringLiteral("https://example.org") << QStringLiteral("https://example.org");
        QTest::newRow("trailing slash and case") << QStringLiteral("  HTTPS://Thoughts.Example-Server.ORG/ ") << QStringLiteral("https://thoughts.example-server.org");
        QTest::newRow("port") << QStringLiteral("https://server.example.ts.net:8443") << QStringLiteral("https://server.example.ts.net:8443");
        QTest::newRow("path prefix") << QStringLiteral("https://example.org/thoughts/") << QStringLiteral("https://example.org/thoughts");
        QTest::newRow("empty") << QString() << QString();
        QTest::newRow("no scheme") << QStringLiteral("thoughts.example.org") << QString();
        QTest::newRow("http") << QStringLiteral("http://thoughts.example.org") << QString();
        QTest::newRow("other scheme") << QStringLiteral("ftp://thoughts.example.org") << QString();
        QTest::newRow("no host") << QStringLiteral("https:///v1") << QString();
        QTest::newRow("user") << QStringLiteral("https://me@thoughts.example.org") << QString();
        QTest::newRow("user and password") << QStringLiteral("https://me:secret@thoughts.example.org") << QString();
        QTest::newRow("empty user info") << QStringLiteral("https://@thoughts.example.org") << QString();
        QTest::newRow("query") << QStringLiteral("https://thoughts.example.org/?token=1") << QString();
        QTest::newRow("empty query") << QStringLiteral("https://thoughts.example.org?") << QString();
        QTest::newRow("fragment") << QStringLiteral("https://thoughts.example.org#top") << QString();
        QTest::newRow("invalid") << QStringLiteral("https://thoughts example.org") << QString();
    }

    void addressesAreValidated()
    {
        QFETCH(QString, address);
        QFETCH(QString, expected);
        QString error = QStringLiteral("stale");
        QCOMPARE(Thoughts::ServerAddress::normalized(address, &error), expected);
        QCOMPARE(error.isEmpty(), !expected.isEmpty());
    }

    void savedAddressRoundTrips()
    {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("nested/server.json"));
        bool present = true;
        QVERIFY(Thoughts::ServerAddress::saved(&present, path).isEmpty());
        QVERIFY(!present);

        QString error;
        QVERIFY(!Thoughts::ServerAddress::save(QStringLiteral("http://thoughts.example.org"), &error, path));
        QVERIFY(!error.isEmpty());
        QVERIFY(!QFile::exists(path));

        QVERIFY2(Thoughts::ServerAddress::save(QStringLiteral("https://Thoughts.example.org/"), &error, path), qPrintable(error));
        QCOMPARE(Thoughts::ServerAddress::saved(&present, path), QStringLiteral("https://thoughts.example.org"));
        QVERIFY(present);
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("server_url")).toString(), QStringLiteral("https://thoughts.example.org"));
    }

    void firstRunKeepsTheLegacyAddressForExistingInstallations()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        int checks = 0;
        const auto existing = [&checks] {
            ++checks;
            return true;
        };
        // An installation that synchronized before keeps the address it used,
        // and the choice is saved so the default never switches silently.
        QCOMPARE(Thoughts::ServerAddress::configured(existing, path, DefaultAddress, LegacyAddress), LegacyAddress);
        QCOMPARE(checks, 1);
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), LegacyAddress);
        QCOMPARE(Thoughts::ServerAddress::configured(existing, path, DefaultAddress, LegacyAddress), LegacyAddress);
        QCOMPARE(checks, 1);

        // Switching saves the new address, which later runs use.
        QVERIFY(Thoughts::ServerAddress::save(DefaultAddress, nullptr, path));
        QCOMPARE(Thoughts::ServerAddress::configured(existing, path, DefaultAddress, LegacyAddress), DefaultAddress);
        QCOMPARE(checks, 1);
    }

    void firstRunWithoutLegacyAddressUsesTheDefault()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        int checks = 0;
        const auto existing = [&checks] {
            ++checks;
            return true;
        };
        // A build without a legacy address has nothing to keep: even an
        // installation that synchronized starts with the default.
        QCOMPARE(Thoughts::ServerAddress::configured(existing, path, DefaultAddress, QString()), DefaultAddress);
        QCOMPARE(checks, 0);
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), DefaultAddress);
        QCOMPARE(Thoughts::ServerAddress::firstRunUrl(true, DefaultAddress, QString()), DefaultAddress);
    }

    void savedAddressWinsOverTheBuildAddresses()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const QString own = QStringLiteral("https://own.example.net:8443");
        QVERIFY(Thoughts::ServerAddress::save(own, nullptr, path));
        bool checked = false;
        const auto existing = [&checked] {
            checked = true;
            return true;
        };
        // Whatever the build's addresses are, an installation keeps the one it saved.
        QCOMPARE(Thoughts::ServerAddress::configured(existing, path, DefaultAddress, LegacyAddress), own);
        QCOMPARE(Thoughts::ServerAddress::configured(existing, path, DefaultAddress, QString()), own);
        QCOMPARE(Thoughts::ServerAddress::configured(existing, path), own);
        QVERIFY(!checked);
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), own);
    }

    void firstRunUsesThePublicServerForNewInstallations()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        QCOMPARE(Thoughts::ServerAddress::configured([] { return false; }, path, DefaultAddress, LegacyAddress), DefaultAddress);
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), DefaultAddress);
        // Once saved, the installation check no longer matters.
        QCOMPARE(Thoughts::ServerAddress::configured([] { return true; }, path, DefaultAddress, LegacyAddress), DefaultAddress);
        QCOMPARE(Thoughts::ServerAddress::firstRunUrl(true, DefaultAddress, LegacyAddress), LegacyAddress);
        QCOMPARE(Thoughts::ServerAddress::firstRunUrl(false, DefaultAddress, LegacyAddress), DefaultAddress);

        // The build's own addresses work the same way.
        QTemporaryDir buildRoot;
        const QString buildPath = buildRoot.filePath(QStringLiteral("server.json"));
        QCOMPARE(Thoughts::ServerAddress::configured([] { return false; }, buildPath), Thoughts::ServerAddress::defaultUrl());
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, buildPath), Thoughts::ServerAddress::defaultUrl());
    }

    void existingInstallationMeansSyncState()
    {
        const QDir stateRoot(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server")));
        const QString state = stateRoot.filePath(QStringLiteral("repository.json"));
        QFile::remove(state);
        const auto cleanup = qScopeGuard([state] { QFile::remove(state); });
        QVERIFY(!Thoughts::ServerAddress::existingInstallation());
        QVERIFY(QDir().mkpath(stateRoot.absolutePath()));
        QFile file(state);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{}") == 2);
        file.close();
        QVERIFY(Thoughts::ServerAddress::existingInstallation());
    }

    void unreadableSavedAddressIsNotReplaced()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{\"server_url\": \"http://thoughts.example.org\"}") > 0);
        file.close();
        bool checked = false;
        QVERIFY(Thoughts::ServerAddress::configured([&checked] { checked = true; return true; }, path, DefaultAddress, LegacyAddress).isEmpty());
        QVERIFY(!checked);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("http://thoughts.example.org"));
    }

    void environmentOverridesWithoutSaving()
    {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const bool had = qEnvironmentVariableIsSet("THOUGHTS_SERVER_URL");
        const QByteArray previous = qgetenv("THOUGHTS_SERVER_URL");
        const auto restore = qScopeGuard([had, previous] {
            if (had) {
                qputenv("THOUGHTS_SERVER_URL", previous);
            } else {
                qunsetenv("THOUGHTS_SERVER_URL");
            }
        });
        qputenv("THOUGHTS_SERVER_URL", "https://server.example.ts.net:8443");
        QVERIFY(Thoughts::ServerAddress::overriddenByEnvironment());
        bool checked = false;
        QCOMPARE(Thoughts::ServerAddress::configured([&checked] { checked = true; return true; }, path, DefaultAddress, LegacyAddress), QStringLiteral("https://server.example.ts.net:8443"));
        QVERIFY(!checked);
        QVERIFY(!QFile::exists(path));
    }

    void originIsSchemeHostAndPort_data()
    {
        QTest::addColumn<QString>("address");
        QTest::addColumn<QString>("expected");
        QTest::newRow("plain") << QStringLiteral("https://example.org") << QStringLiteral("https://example.org");
        QTest::newRow("path and case") << QStringLiteral("HTTPS://Example.ORG/thoughts/") << QStringLiteral("https://example.org");
        QTest::newRow("default port") << QStringLiteral("https://example.org:443/x") << QStringLiteral("https://example.org");
        QTest::newRow("other port") << QStringLiteral("https://example.org:8443") << QStringLiteral("https://example.org:8443");
        QTest::newRow("subdomain") << QStringLiteral("https://a.example.org") << QStringLiteral("https://a.example.org");
        QTest::newRow("http") << QStringLiteral("http://example.org") << QString();
        QTest::newRow("user") << QStringLiteral("https://example.org@other.example") << QString();
        QTest::newRow("empty") << QString() << QString();
    }

    void originIsSchemeHostAndPort()
    {
        QFETCH(QString, address);
        QFETCH(QString, expected);
        QCOMPARE(Thoughts::ServerAddress::origin(address), expected);
    }

    // The store an earlier version of Thoughts left: only the address.
    static void writeOldStore(const QString &path, const QString &address)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("server_url"), address}}).toJson());
    }

    void existingTokenIsBoundToTheSavedAddressOnFirstUse()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const QString first = QStringLiteral("https://first.example.org");
        const QString account = QStringLiteral("thoughts-device");
        const auto existing = [] { return true; };
        writeOldStore(path, first);
        QVERIFY(Thoughts::ServerAddress::tokenOrigin(account, path).isEmpty());

        // Migration: the installation keeps working with the address it has.
        QString issuedFor;
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", first, &issuedFor, existing, path));
        QCOMPARE(issuedFor, first);
        QCOMPARE(Thoughts::ServerAddress::tokenOrigin(account, path), first);
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), first);

        // The same origin under another spelling or path is the same server.
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", QStringLiteral("HTTPS://First.Example.org:443/prefix/"), &issuedFor, existing, path));
        // Any other origin is not: another host, a subdomain, another port.
        for (const QString &other : {QStringLiteral("https://second.example.org"), QStringLiteral("https://evil.first.example.org"), QStringLiteral("https://first.example.org.evil.example"),
                                     QStringLiteral("https://first.example.org:8443"), QStringLiteral("https://example.org")}) {
            issuedFor.clear();
            QVERIFY2(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", other, &issuedFor, existing, path), qPrintable(other));
            QCOMPARE(issuedFor, first);
        }
        // Nothing is sent to an address Thoughts would not use at all, and no token is no token.
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", QStringLiteral("http://first.example.org"), &issuedFor, existing, path));
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, QByteArray(), first, &issuedFor, existing, path));
        QVERIFY(issuedFor.isEmpty());

        // The record holds a fingerprint, never the token.
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray stored = file.readAll();
        QVERIFY(!stored.contains("token-one"));
        const QJsonObject entry = QJsonDocument::fromJson(stored).object().value(QStringLiteral("token_origins")).toObject().value(account).toObject();
        QCOMPARE(entry.value(QStringLiteral("origin")).toString(), first);
        // The agent notes tool (tools/mcp/agent_notes.py) computes the same fingerprint.
        QCOMPARE(entry.value(QStringLiteral("token_fingerprint")).toString(), QStringLiteral("5c8fb37591dc58fc3ab5fd7f8bbc15c4"));
    }

    void changedAddressDoesNotReceiveTheOldToken()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const QString first = QStringLiteral("https://first.example.org");
        const QString second = QStringLiteral("https://second.example.org");
        const QString device = QStringLiteral("thoughts-device");
        const QString publisher = QStringLiteral("thoughts-source-sync");
        const auto existing = [] { return true; };
        // An updated installation whose owner changes the address before any
        // token was read: Server… and thoughtsctl set-server both save().
        writeOldStore(path, first);
        QVERIFY(Thoughts::ServerAddress::save(second, nullptr, path, existing));
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), second);
        QCOMPARE(Thoughts::ServerAddress::tokenOrigin(device, path), first);
        QCOMPARE(Thoughts::ServerAddress::tokenOrigin(publisher, path), first);
        QString issuedFor;
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(device, "token-one", second, &issuedFor, existing, path));
        QCOMPARE(issuedFor, first);
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(publisher, "publisher-one", second, &issuedFor, existing, path));
        // Still good for the server that issued it (THOUGHTS_SERVER_URL back to it, say).
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(device, "token-one", first, &issuedFor, existing, path));
        const QString message = Thoughts::ServerAddress::tokenWithheldMessage(device, first, second + QStringLiteral("/prefix"));
        QVERIFY2(message.contains(QStringLiteral("was issued for https://first.example.org and is not sent to https://second.example.org.")), qPrintable(message));
        QVERIFY(message.contains(QStringLiteral("thoughtsctl store-secret --account thoughts-device")));

        // The owner stores the new server's token (in any way): it differs
        // from the one seen before and belongs to the address saved now.
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(device, "token-two", second, &issuedFor, existing, path));
        QCOMPARE(issuedFor, second);
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(device, "token-two", first, &issuedFor, existing, path));
        QCOMPARE(issuedFor, second);
        // Each account has its own token: the publisher's is still the first server's.
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(publisher, "publisher-one", second, &issuedFor, existing, path));
        QCOMPARE(issuedFor, first);

        // A later change binds nothing anew: the record is already there.
        QVERIFY(Thoughts::ServerAddress::save(QStringLiteral("https://third.example.org"), nullptr, path, existing));
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(device, "token-two", QStringLiteral("https://third.example.org"), &issuedFor, existing, path));
        QCOMPARE(issuedFor, second);
        QCOMPARE(Thoughts::ServerAddress::tokenOrigin(publisher, path), first);
    }

    void changedAddressAfterTheTokenWasSeen()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const QString first = QStringLiteral("https://first.example.org");
        const QString second = QStringLiteral("https://second.example.org/thoughts");
        const QString account = QStringLiteral("thoughts-device");
        const auto existing = [] { return true; };
        writeOldStore(path, first);
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", first, nullptr, existing, path));
        QVERIFY(Thoughts::ServerAddress::save(second, nullptr, path, existing));
        QString issuedFor;
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", second, &issuedFor, existing, path));
        QCOMPARE(issuedFor, first);

        // The same token really is the new server's (it moved): storing it
        // again through thoughtsctl or Server… says so.
        QString error;
        QVERIFY2(Thoughts::ServerAddress::bindToken(account, "token-one", second, &error, path), qPrintable(error));
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", second, &issuedFor, existing, path));
        QCOMPARE(issuedFor, QStringLiteral("https://second.example.org"));
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", first, &issuedFor, existing, path));
        // Saving keeps the address and the record apart.
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), second);
        QVERIFY(!Thoughts::ServerAddress::bindToken(account, "token-one", QStringLiteral("http://second.example.org"), &error, path));
        QVERIFY(!Thoughts::ServerAddress::bindToken(QStringLiteral("*"), "token-one", second, &error, path));
        QVERIFY(!Thoughts::ServerAddress::bindToken(account, QByteArray(), second, &error, path));
    }

    void environmentAddressDoesNotReceiveTheStoredToken()
    {
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const QString saved = QStringLiteral("https://first.example.org");
        const QString account = QStringLiteral("thoughts-device");
        const auto existing = [] { return true; };
        writeOldStore(path, saved);
        const bool had = qEnvironmentVariableIsSet("THOUGHTS_SERVER_URL");
        const QByteArray previous = qgetenv("THOUGHTS_SERVER_URL");
        const auto restore = qScopeGuard([had, previous] {
            if (had) {
                qputenv("THOUGHTS_SERVER_URL", previous);
            } else {
                qunsetenv("THOUGHTS_SERVER_URL");
            }
        });
        // The first run after the update is one with the override: the token
        // is bound to the saved address, not to the override.
        qputenv("THOUGHTS_SERVER_URL", "https://override.example.org");
        const QString configured = Thoughts::ServerAddress::configured(existing, path);
        QCOMPARE(configured, QStringLiteral("https://override.example.org"));
        QString issuedFor;
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", configured, &issuedFor, existing, path));
        QCOMPARE(issuedFor, saved);
        // An override naming the same server changes nothing.
        qputenv("THOUGHTS_SERVER_URL", "https://first.example.org/");
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", Thoughts::ServerAddress::configured(existing, path), &issuedFor, existing, path));
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), saved);
    }

    void installationBeingSetUpBindsItsTokenWhenFirstUsed()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const QString account = QStringLiteral("thoughts-device");
        const auto fresh = [] { return false; };
        // The first start saved the public address; the owner then names their own server.
        QCOMPARE(Thoughts::ServerAddress::configured(fresh, path), Thoughts::ServerAddress::defaultUrl());
        QVERIFY(Thoughts::ServerAddress::save(QStringLiteral("https://own.example.org"), nullptr, path, fresh));
        QVERIFY(Thoughts::ServerAddress::tokenOrigin(account, path).isEmpty());
        QString issuedFor;
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", QStringLiteral("https://own.example.org"), &issuedFor, fresh, path));
        QCOMPARE(issuedFor, QStringLiteral("https://own.example.org"));
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", Thoughts::ServerAddress::defaultUrl(), &issuedFor, fresh, path));
    }

    void tokenWithoutASavedAddressIsBoundToTheFirstRunAddress()
    {
        const auto restore = withoutEnvironmentAddress();
        QTemporaryDir root;
        const QString path = root.filePath(QStringLiteral("server.json"));
        const QString account = QStringLiteral("thoughts-device");
        // Nothing saved yet on a computer that synchronized before: the
        // address older versions always used is saved and the token bound to it.
        QString issuedFor;
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", LegacyAddress, &issuedFor, [] { return true; }, path, DefaultAddress, LegacyAddress));
        QCOMPARE(issuedFor, LegacyAddress);
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, path), LegacyAddress);
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", QStringLiteral("https://other.example.org"), &issuedFor, [] { return true; }, path, DefaultAddress, LegacyAddress));
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", DefaultAddress, &issuedFor, [] { return true; }, path, DefaultAddress, LegacyAddress));

        // A build without a legacy address has nothing to keep: the default
        // is saved and the token bound to it, as with the build's own addresses.
        const QString withoutLegacy = root.filePath(QStringLiteral("without-legacy.json"));
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", DefaultAddress, &issuedFor, [] { return true; }, withoutLegacy, DefaultAddress, QString()));
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, withoutLegacy), DefaultAddress);
        const QString buildPath = root.filePath(QStringLiteral("build.json"));
        const QString buildFirstRun = Thoughts::ServerAddress::firstRunUrl(true);
        QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", buildFirstRun, &issuedFor, [] { return true; }, buildPath));
        QCOMPARE(Thoughts::ServerAddress::saved(nullptr, buildPath), buildFirstRun);

        // A saved address that is no address binds nothing and sends nothing.
        const QString broken = root.filePath(QStringLiteral("broken.json"));
        QFile file(broken);
        QVERIFY(file.open(QIODevice::WriteOnly));
        QVERIFY(file.write("{\"server_url\": \"http://first.example.org\"}") > 0);
        file.close();
        QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", QStringLiteral("https://first.example.org"), &issuedFor, [] { return true; }, broken));
        QVERIFY(issuedFor.isEmpty());
    }

    void signingInMovesTheConnectionGeneration()
    {
        // What keeps a token compares this to the value it read the token at.
        const quint64 before = Thoughts::ServerAddress::connectionGeneration();
        QCOMPARE(Thoughts::ServerAddress::connectionGeneration(), before);
        Thoughts::ServerAddress::connectionChanged();
        QCOMPARE(Thoughts::ServerAddress::connectionGeneration(), before + 1);
    }
};

QTEST_GUILESS_MAIN(ThoughtsServerAddressTest)

#include "thoughtsserveraddresstest.moc"
