// SPDX-License-Identifier: GPL-2.0-or-later
#include "../part/desktopsyncstore.h"
#include "../part/linkoverridestore.h"

#include <QDir>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace Thoughts;

namespace
{
const QString source = QStringLiteral("3f0c8a9e-6a43-4c1f-9d8a-2b1c3d4e5f60");
const QString target = QStringLiteral("7a1b2c3d-4e5f-4a6b-8c7d-9e0f1a2b3c4d");
const QString otherTarget = QStringLiteral("0b9c8d7e-6f5a-4b3c-9d2e-1f0a9b8c7d6e");

PdfLinkOverrides::Link gotoLink(const QString &name, int page, std::optional<double> x, std::optional<double> y)
{
    PdfLinkOverrides::Link link;
    link.kind = QStringLiteral("goto");
    link.destName = name;
    link.destPage = page;
    link.destX = x;
    link.destY = y;
    return link;
}

PdfLinkOverrides::Link uriLink(const QString &uri)
{
    PdfLinkOverrides::Link link;
    link.kind = QStringLiteral("uri");
    link.uri = uri;
    return link;
}

QJsonObject record(const PdfLinkOverrides::Link &link, const QString &updatedAt, const QString &to = target, const QString &from = source)
{
    return {{QStringLiteral("override_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("source_document_id"), from},
            {QStringLiteral("link"), link.toJson()},
            {QStringLiteral("target_document_id"), to},
            {QStringLiteral("revision"), 1},
            {QStringLiteral("updated_at"), updatedAt},
            {QStringLiteral("origin_id"), QStringLiteral("phone")},
            {QStringLiteral("deleted"), false}};
}

QJsonObject projections(const QList<QJsonObject> &records)
{
    QJsonObject result;
    for (const QJsonObject &value : records) result.insert(PdfLinkOverrides::projectionKey(value.value(QStringLiteral("override_id")).toString()), value);
    // Other record types share the projections.
    result.insert(QStringLiteral("logical_comment\ncomments\nc1"), QJsonObject{{QStringLiteral("comment_id"), QStringLiteral("c1")}});
    return result;
}
}

class PdfLinkOverrideTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testNamedDestinationsMatchByName();
    void testPositionsMatchWithinTolerance();
    void testPageAloneNeverMatches();
    void testUriMatchesExactly();
    void testLatestUpdateWins();
    void testTombstonesAndOtherDocumentsAreIgnored();
    void testLinkJsonRoundTrip();
    void testCreateRetargetRemove();
    void testValidation();
    void testEnqueue();
};

void PdfLinkOverrideTest::testNamedDestinationsMatchByName()
{
    const auto stored = record(gotoLink(QStringLiteral("cite.pertsch2025fast"), 41, 72.0, 640.5), QStringLiteral("2026-10-15T10:00:00.000Z"));
    // Both named: the name decides, whatever the position.
    QVERIFY(PdfLinkOverrides::matches(stored, gotoLink(QStringLiteral("cite.pertsch2025fast"), 3, 0.0, 0.0)));
    QVERIFY(!PdfLinkOverrides::matches(stored, gotoLink(QStringLiteral("cite.other2024"), 41, 72.0, 640.5)));
    // A phone link (no name) matches the desktop record by page and position.
    QVERIFY(PdfLinkOverrides::matches(stored, gotoLink(QString(), 41, 72.0, 640.5)));
    QVERIFY(!PdfLinkOverrides::matches(stored, gotoLink(QString(), 40, 72.0, 640.5)));
    // A phone record (no name) matches a named desktop link by page and position.
    const auto phone = record(gotoLink(QString(), 41, 72.0, 640.5), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QVERIFY(PdfLinkOverrides::matches(phone, gotoLink(QStringLiteral("cite.pertsch2025fast"), 41, 71.0, 642.0)));
    QVERIFY(!PdfLinkOverrides::matches(phone, gotoLink(QStringLiteral("cite.pertsch2025fast"), 41, 71.0, 600.0)));
}

void PdfLinkOverrideTest::testPositionsMatchWithinTolerance()
{
    const auto stored = record(gotoLink(QString(), 5, 100.0, 500.0), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QVERIFY(PdfLinkOverrides::matches(stored, gotoLink(QString(), 5, 100.0, 500.0)));
    QVERIFY(PdfLinkOverrides::matches(stored, gotoLink(QString(), 5, 104.0, 496.0)));
    QVERIFY(!PdfLinkOverrides::matches(stored, gotoLink(QString(), 5, 100.0, 504.5)));
    QVERIFY(!PdfLinkOverrides::matches(stored, gotoLink(QString(), 5, 95.5, 500.0)));
    // x only counts when both know it.
    QVERIFY(PdfLinkOverrides::matches(stored, gotoLink(QString(), 5, std::nullopt, 502.0)));
    const auto withoutX = record(gotoLink(QString(), 5, std::nullopt, 500.0), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QVERIFY(PdfLinkOverrides::matches(withoutX, gotoLink(QString(), 5, 300.0, 499.0)));
    // A stored y needs a clicked y.
    QVERIFY(!PdfLinkOverrides::matches(stored, gotoLink(QString(), 5, 100.0, std::nullopt)));
}

void PdfLinkOverrideTest::testPageAloneNeverMatches()
{
    const auto pageOnly = record(gotoLink(QString(), 5, std::nullopt, std::nullopt), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QVERIFY(!PdfLinkOverrides::matches(pageOnly, gotoLink(QString(), 5, std::nullopt, std::nullopt)));
    QVERIFY(!PdfLinkOverrides::matches(pageOnly, gotoLink(QString(), 5, 10.0, 10.0)));
    QVERIFY(!PdfLinkOverrides::matches(pageOnly, gotoLink(QStringLiteral("name"), 5, 10.0, 10.0)));
    // A named record without y still matches a nameless link on its page.
    const auto namedWithoutY = record(gotoLink(QStringLiteral("section.2"), 5, std::nullopt, std::nullopt), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QVERIFY(PdfLinkOverrides::matches(namedWithoutY, gotoLink(QString(), 5, 10.0, 10.0)));
    QVERIFY(!gotoLink(QString(), 5, 10.0, std::nullopt).isValid());
    QVERIFY(gotoLink(QString(), 5, std::nullopt, 10.0).isValid());
    QVERIFY(gotoLink(QStringLiteral("n"), 5, std::nullopt, std::nullopt).isValid());
    QVERIFY(!gotoLink(QStringLiteral("n"), -1, std::nullopt, std::nullopt).isValid());
}

void PdfLinkOverrideTest::testUriMatchesExactly()
{
    const auto stored = record(uriLink(QStringLiteral("https://arxiv.org/abs/2501.09747")), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QVERIFY(PdfLinkOverrides::matches(stored, uriLink(QStringLiteral("https://arxiv.org/abs/2501.09747"))));
    QVERIFY(!PdfLinkOverrides::matches(stored, uriLink(QStringLiteral("https://arxiv.org/abs/2501.09747/"))));
    QVERIFY(!PdfLinkOverrides::matches(stored, uriLink(QStringLiteral("HTTPS://arxiv.org/abs/2501.09747"))));
    QVERIFY(!PdfLinkOverrides::matches(stored, gotoLink(QStringLiteral("https://arxiv.org/abs/2501.09747"), 0, 0.0, 0.0)));
    QVERIFY(!uriLink(QString()).isValid());
}

void PdfLinkOverrideTest::testLatestUpdateWins()
{
    const auto link = gotoLink(QStringLiteral("cite.a"), 9, 72.0, 700.0);
    const auto older = record(link, QStringLiteral("2026-10-15T10:00:00.000Z"), target);
    // The phone writes another RFC 3339 form; 12:30+02:00 is later than 10:00Z.
    const auto phone = record(gotoLink(QString(), 9, 72.0, 701.0), QStringLiteral("2026-10-15T12:30:00+02:00"), otherTarget);
    QCOMPARE(PdfLinkOverrides::findOverride(projections({older, phone}), source, link), phone);
    const auto newest = record(link, QStringLiteral("2026-10-15T10:45:00.5Z"), target);
    const auto matching = PdfLinkOverrides::matchingOverrides(projections({older, newest, phone}), source, link);
    QCOMPARE(matching, (QList<QJsonObject>{newest, phone, older}));
    // Equal times: the higher revision wins.
    auto revised = older;
    revised.insert(QStringLiteral("override_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    revised.insert(QStringLiteral("revision"), 2);
    QCOMPARE(PdfLinkOverrides::findOverride(projections({older, revised}), source, link), revised);
}

void PdfLinkOverrideTest::testTombstonesAndOtherDocumentsAreIgnored()
{
    const auto link = uriLink(QStringLiteral("https://example.org/paper"));
    auto deleted = record(link, QStringLiteral("2026-10-15T11:00:00.000Z"));
    deleted.insert(QStringLiteral("deleted"), true);
    const auto live = record(link, QStringLiteral("2026-10-15T10:00:00.000Z"), otherTarget);
    QCOMPARE(PdfLinkOverrides::findOverride(projections({deleted, live}), source, link), live);
    QVERIFY(PdfLinkOverrides::findOverride(projections({deleted}), source, link).isEmpty());
    QVERIFY(!PdfLinkOverrides::matches(deleted, link));
    const auto elsewhere = record(link, QStringLiteral("2026-10-15T10:00:00.000Z"), target, otherTarget);
    QVERIFY(PdfLinkOverrides::findOverride(projections({elsewhere}), source, link).isEmpty());
    QCOMPARE(PdfLinkOverrides::overridesFor(projections({deleted, live, elsewhere}), source), QList<QJsonObject>{live});
    QVERIFY(PdfLinkOverrides::overridesFor(projections({live}), QString()).isEmpty());
    QVERIFY(PdfLinkOverrides::anyOverrides(projections({live})));
    QVERIFY(!PdfLinkOverrides::anyOverrides(projections({})));
}

void PdfLinkOverrideTest::testLinkJsonRoundTrip()
{
    const auto named = gotoLink(QStringLiteral("cite.x"), 41, 72.0, 640.5);
    QCOMPARE(named.toJson(), (QJsonObject{{QStringLiteral("kind"), QStringLiteral("goto")}, {QStringLiteral("dest_name"), QStringLiteral("cite.x")},
                                          {QStringLiteral("dest_page"), 41}, {QStringLiteral("dest_x"), 72.0}, {QStringLiteral("dest_y"), 640.5}}));
    const auto parsed = PdfLinkOverrides::Link::fromJson(named.toJson());
    QCOMPARE(parsed.destName, named.destName);
    QCOMPARE(parsed.destPage, 41);
    QCOMPARE(*parsed.destX, 72.0);
    QCOMPARE(*parsed.destY, 640.5);
    QCOMPARE(gotoLink(QString(), 2, std::nullopt, 10.0).toJson(),
             (QJsonObject{{QStringLiteral("kind"), QStringLiteral("goto")}, {QStringLiteral("dest_page"), 2}, {QStringLiteral("dest_y"), 10.0}}));
    QCOMPARE(uriLink(QStringLiteral("https://a.b/c")).toJson(), (QJsonObject{{QStringLiteral("kind"), QStringLiteral("uri")}, {QStringLiteral("uri"), QStringLiteral("https://a.b/c")}}));
    // Null positions are unknown positions.
    const auto nulls = PdfLinkOverrides::Link::fromJson({{QStringLiteral("kind"), QStringLiteral("goto")}, {QStringLiteral("dest_page"), 1}, {QStringLiteral("dest_y"), QJsonValue()}});
    QVERIFY(!nulls.destY);
}

void PdfLinkOverrideTest::testCreateRetargetRemove()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-15T10:00:00.000Z"), Qt::ISODateWithMs);
    const auto link = gotoLink(QStringLiteral("cite.pertsch2025fast"), 41, 72.0, 640.5);
    const QString sha = QString(64, QLatin1Char('a'));
    const auto created = PdfLinkOverrides::create(source, sha, link, target, QStringLiteral("desktop-1"), now);
    QVERIFY(PdfLinkOverrides::validationError(created).isEmpty());
    QCOMPARE(created.value(QStringLiteral("source_document_id")).toString(), source);
    QCOMPARE(created.value(QStringLiteral("source_pdf_sha256")).toString(), sha);
    QCOMPARE(created.value(QStringLiteral("target_document_id")).toString(), target);
    QCOMPARE(created.value(QStringLiteral("link")).toObject(), link.toJson());
    QCOMPARE(created.value(QStringLiteral("revision")).toInteger(), qint64(1));
    QCOMPARE(created.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QCOMPARE(created.value(QStringLiteral("origin_id")).toString(), QStringLiteral("desktop-1"));
    QCOMPARE(created.value(QStringLiteral("deleted")).toBool(true), false);
    QCOMPARE(QUuid(created.value(QStringLiteral("override_id")).toString()).version(), QUuid::Random);

    // A record from the phone with a field this version does not know, and a clock ahead of ours.
    auto phone = created;
    phone.insert(QStringLiteral("updated_at"), QStringLiteral("2026-10-15T11:00:00.000Z"));
    phone.insert(QStringLiteral("future_field"), QJsonObject{{QStringLiteral("keep"), true}});
    phone.insert(QStringLiteral("revision"), 3);
    const auto changed = PdfLinkOverrides::retarget(phone, otherTarget, QStringLiteral("desktop-1"), now);
    QCOMPARE(changed.value(QStringLiteral("override_id")), phone.value(QStringLiteral("override_id")));
    QCOMPARE(changed.value(QStringLiteral("target_document_id")).toString(), otherTarget);
    QCOMPARE(changed.value(QStringLiteral("revision")).toInteger(), qint64(4));
    QCOMPARE(changed.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-15T11:00:00.001Z"));
    QCOMPARE(changed.value(QStringLiteral("future_field")).toObject(), (QJsonObject{{QStringLiteral("keep"), true}}));
    QCOMPARE(changed.value(QStringLiteral("link")), phone.value(QStringLiteral("link")));
    QVERIFY(PdfLinkOverrides::newer(changed, phone));

    const auto removed = PdfLinkOverrides::remove(changed, QStringLiteral("desktop-1"), now.addSecs(7200));
    QCOMPARE(removed.value(QStringLiteral("deleted")).toBool(), true);
    QCOMPARE(removed.value(QStringLiteral("revision")).toInteger(), qint64(5));
    QCOMPARE(removed.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-10-15T12:00:00.000Z"));
    QCOMPARE(removed.value(QStringLiteral("target_document_id")).toString(), otherTarget);
    QCOMPARE(removed.value(QStringLiteral("future_field")).toObject(), (QJsonObject{{QStringLiteral("keep"), true}}));
    QVERIFY(PdfLinkOverrides::validationError(removed).isEmpty());

    // Timestamps never fall behind the record they replace.
    QCOMPARE(PdfLinkOverrides::timestamp(now, QStringLiteral("2026-10-15T09:00:00Z")), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QCOMPARE(PdfLinkOverrides::timestamp(now, QStringLiteral("2026-10-15T10:00:00.000Z")), QStringLiteral("2026-10-15T10:00:00.001Z"));
}

void PdfLinkOverrideTest::testValidation()
{
    const auto valid = record(uriLink(QStringLiteral("https://example.org")), QStringLiteral("2026-10-15T10:00:00.000Z"));
    QVERIFY(PdfLinkOverrides::validationError(valid).isEmpty());
    auto broken = valid;
    broken.insert(QStringLiteral("target_document_id"), QStringLiteral("not-a-uuid"));
    QVERIFY(!PdfLinkOverrides::validationError(broken).isEmpty());
    broken = valid;
    broken.insert(QStringLiteral("link"), gotoLink(QString(), 3, std::nullopt, std::nullopt).toJson());
    QVERIFY(!PdfLinkOverrides::validationError(broken).isEmpty());
    broken = valid;
    broken.insert(QStringLiteral("revision"), 0);
    QVERIFY(!PdfLinkOverrides::validationError(broken).isEmpty());
    broken = valid;
    broken.remove(QStringLiteral("override_id"));
    QVERIFY(!PdfLinkOverrides::validationError(broken).isEmpty());
}

void PdfLinkOverrideTest::testEnqueue()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    DesktopSyncOutbox outbox(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")), QDir(temporary.path()).filePath(QStringLiteral("outbox.json")));
    const auto created = PdfLinkOverrides::create(source, QString(), gotoLink(QStringLiteral("cite.a"), 2, 10.0, 20.0), target, QStringLiteral("desktop-1"));
    QString error;
    QVERIFY2(PdfLinkOverrides::enqueue(outbox, created, &error), qPrintable(error));
    const auto changed = PdfLinkOverrides::retarget(created, otherTarget, QStringLiteral("desktop-1"));
    QVERIFY2(PdfLinkOverrides::enqueue(outbox, changed, &error), qPrintable(error));
    const auto removed = PdfLinkOverrides::remove(changed, QStringLiteral("desktop-1"));
    QVERIFY2(PdfLinkOverrides::enqueue(outbox, removed, &error), qPrintable(error));

    const QList<QJsonObject> operations = outbox.operations(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(operations.size(), 3);
    const QString id = created.value(QStringLiteral("override_id")).toString();
    const QList<QJsonObject> payloads{created, changed, removed};
    for (int index = 0; index < operations.size(); ++index) {
        const QJsonObject &operation = operations.at(index);
        QCOMPARE(operation.value(QStringLiteral("entity_type")).toString(), QStringLiteral("pdf_link_override"));
        QCOMPARE(operation.value(QStringLiteral("collection")).toString(), QStringLiteral("link-overrides"));
        QCOMPARE(operation.value(QStringLiteral("entity_id")).toString(), id);
        QCOMPARE(operation.value(QStringLiteral("base_revision")).toInteger(-1), qint64(index));
        QCOMPARE(operation.value(QStringLiteral("client_timestamp")).toString(), payloads.at(index).value(QStringLiteral("updated_at")).toString());
        QCOMPARE(operation.value(QStringLiteral("payload")).toObject(), payloads.at(index));
    }

    // Invalid records never reach the queue.
    auto invalid = created;
    invalid.remove(QStringLiteral("target_document_id"));
    QVERIFY(!PdfLinkOverrides::enqueue(outbox, invalid, &error));
    QVERIFY(!error.isEmpty());
    QCOMPARE(outbox.operations().size(), 3);
}

QTEST_GUILESS_MAIN(PdfLinkOverrideTest)
#include "pdflinkoverridetest.moc"
