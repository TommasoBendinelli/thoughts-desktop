// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/recallpredictorconfig.h"

#include <QTest>

using namespace Thoughts::RecallPredictorConfig;

class RecallPredictorConfigTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void addresses_data()
    {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("allowed");
        QTest::newRow("https") << QStringLiteral("https://predictor.example.org/recall-prediction") << true;
        QTest::newRow("https with port") << QStringLiteral("https://predictor.example.org:8443/x") << true;
        QTest::newRow("http to 127.0.0.1") << QStringLiteral("http://127.0.0.1:8770/recall-prediction") << true;
        QTest::newRow("http to another loopback address") << QStringLiteral("http://127.8.9.10:8770/x") << true;
        QTest::newRow("http to localhost") << QStringLiteral("http://localhost:8770/x") << true;
        QTest::newRow("http to LOCALHOST") << QStringLiteral("http://LocalHost/x") << true;
        QTest::newRow("http to a .localhost name") << QStringLiteral("http://predictor.localhost:8770/x") << true;
        QTest::newRow("http to ::1") << QStringLiteral("http://[::1]:8770/x") << true;
        QTest::newRow("http to a public host") << QStringLiteral("http://predictor.example.org/x") << false;
        QTest::newRow("http to a private address") << QStringLiteral("http://192.168.1.20:8770/x") << false;
        QTest::newRow("http to a carrier-grade NAT address") << QStringLiteral("http://100.64.1.2:8770/x") << false;
        QTest::newRow("http to every interface") << QStringLiteral("http://0.0.0.0:8770/x") << false;
        QTest::newRow("http to a name that starts like loopback") << QStringLiteral("http://127.0.0.1.example.org/x") << false;
        QTest::newRow("http to a name that ends like localhost") << QStringLiteral("http://notlocalhost/x") << false;
        QTest::newRow("http to localhost as a subdomain") << QStringLiteral("http://localhost.example.org/x") << false;
        QTest::newRow("user in the address") << QStringLiteral("https://user:secret@predictor.example.org/x") << false;
        QTest::newRow("loopback behind a user name") << QStringLiteral("http://127.0.0.1@predictor.example.org/x") << false;
        QTest::newRow("other scheme") << QStringLiteral("ftp://127.0.0.1/x") << false;
        QTest::newRow("file") << QStringLiteral("file:///tmp/x") << false;
        QTest::newRow("no host") << QStringLiteral("https:///x") << false;
        QTest::newRow("empty") << QString() << false;
    }

    void addresses()
    {
        QFETCH(QString, url);
        QFETCH(bool, allowed);
        QCOMPARE(urlAllowed(QUrl(url)), allowed);
        // The configuration yields a service only for an allowed address, and says why not.
        QString refusal = QStringLiteral("stale");
        const QByteArray json = QJsonDocument(QJsonObject {{QStringLiteral("url"), url}, {QStringLiteral("token"), QStringLiteral("recall-secret")}}).toJson();
        const Service service = parse(json, &refusal);
        QCOMPARE(!service.url.isEmpty(), allowed);
        QCOMPARE(service.token.isEmpty(), !allowed);
        QCOMPARE(refusal.isEmpty(), allowed || url.isEmpty());
        QVERIFY(!refusal.contains(QStringLiteral("recall-secret")));
    }

    void incompleteConfigurationAsksForNothing()
    {
        QString refusal;
        QVERIFY(parse("{}", &refusal).url.isEmpty());
        QVERIFY(refusal.isEmpty());
        QVERIFY(parse("{\"url\": \"https://predictor.example.org\"}", &refusal).url.isEmpty());
        QVERIFY(parse("{\"token\": \"t\"}", &refusal).url.isEmpty());
        QVERIFY(parse("not json", &refusal).url.isEmpty());
        QVERIFY(refusal.isEmpty());
        const Service service = parse("{\"url\": \"https://predictor.example.org/p\", \"token\": \"t\"}");
        QCOMPARE(service.url, QUrl(QStringLiteral("https://predictor.example.org/p")));
        QCOMPARE(service.token, QByteArray("t"));
    }
};

QTEST_GUILESS_MAIN(RecallPredictorConfigTest)
#include "recallpredictorconfigtest.moc"
