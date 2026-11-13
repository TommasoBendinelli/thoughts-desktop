// SPDX-License-Identifier: GPL-2.0-or-later
// Answer feedback records, merging and re-anchoring (thoughts-server protocol/review-card-feedback.md).
#include "../part/reviewcardfeedback.h"
#include "../part/feedbackversion.h"
#include "../part/desktopsyncstore.h"

#include <QCryptographicHash>
#include <QDir>
#include <QTemporaryDir>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUuid>

#include <functional>

using namespace Thoughts;
using ReviewCardFeedback::AnswerText;
using ReviewCardFeedback::Glyph;

class ReviewCardFeedbackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSpecExampleRoundTripsWithUnknownFields();
    void testValidationMatchesTheServer();
    void testMergeFollowsTheServerTable_data();
    void testMergeFollowsTheServerTable();
    void testDeleteWinsInEitherOrderThenUndoRevives();
    void testImmutableFieldsComeFromTheFirstWrite();
    void testSharedAnchoringVectors_data();
    void testSharedAnchoringVectors();
    void testAnswerTextAndDrawnRange();
    void testSelectionQualification();
    void testCreateEditDeleteUndo();
    void testVersionStamps();
    void testHtmlViewFeedback();
    void testAgentText();
    void testAgentJson();
    void testSharedFeedback();
    void testChangePassage();
    void testChangePassageUndoAfterSync();
    void testChangePassageRefusals();
};

namespace
{
QJsonObject specExample()
{
    // §2, plus fields this client does not know at every level (C1).
    return QJsonDocument::fromJson(R"({
      "schema_version": 1,
      "feedback_id": "5b0f6c1e-2a47-4d1b-9c35-7e0a4f2b9d13",
      "card_id": "0072b138-d3fe-416a-83e7-1407058f0e2b",
      "polarity": "negative",
      "comment": "This sign is wrong: the KL term is subtracted.",
      "anchor": {
        "version": 1,
        "quote": {"exact": "the KL term keeps the policy close", "prefix": "In practice, ", "suffix": " to the reference model."},
        "pdf": {
          "document_id": "c1e62ecc-2dfb-4259-97cc-e319b1186848",
          "pdf_sha256": "f4c8827551095a63ce58c6b1e2c5946014b7b3f62d0f96a780a2931f601a561e",
          "regions": [
            {"page": 7, "rect": [0.1765, 0.4116, 0.8256, 0.4290], "future_region_field": true},
            {"page": 7, "rect": [0.1765, 0.4290, 0.3537, 0.4460]}
          ]
        },
        "segment_id": "0",
        "segment_content_sha256": "3775b1627bce1bbc61bdd810a212daab7db3fbfc7932002aacc578a9954d87cd",
        "future_anchor_field": {"x": 1}
      },
      "revision": 1,
      "created_at": "2026-10-20T10:14:03.512Z",
      "updated_at": "2026-10-20T10:14:03.512Z",
      "created_origin_id": "android-pixel",
      "origin_id": "android-pixel",
      "deleted": false,
      "future_field": [1, 2]
    })").object();
}

QJsonObject withWrite(QJsonObject record, int revision, const QString &updatedAt, const QString &origin, bool deleted = false)
{
    record.insert(QStringLiteral("revision"), revision);
    record.insert(QStringLiteral("updated_at"), updatedAt);
    record.insert(QStringLiteral("origin_id"), origin);
    record.insert(QStringLiteral("deleted"), deleted);
    if (deleted) record.insert(QStringLiteral("deleted_at"), updatedAt);
    else record.remove(QStringLiteral("deleted_at"));
    return record;
}

Glyph glyph(int page, const QString &text, double left, double top, double width = 0.01, double height = 0.02)
{
    return Glyph{page, text, QRectF(left, top, width, height)};
}
}

void ReviewCardFeedbackTest::testSpecExampleRoundTripsWithUnknownFields()
{
    const QJsonObject record = specExample();
    QCOMPARE(ReviewCardFeedback::validationError(record), QString());
    // Through JSON and back, byte-equivalent.
    const QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    QCOMPARE(QJsonDocument::fromJson(bytes).toJson(QJsonDocument::Compact), bytes);
    // A rewrite keeps every field it does not change, unknown ones included.
    const QJsonObject edited = ReviewCardFeedback::edit(record, ReviewCardFeedback::positive(), QStringLiteral("  Fine after all.  "), QStringLiteral("mac-okular"),
                                                        QDateTime::fromString(QStringLiteral("2026-10-20T11:00:00.000Z"), Qt::ISODateWithMs));
    QCOMPARE(edited.value(QStringLiteral("future_field")), record.value(QStringLiteral("future_field")));
    QCOMPARE(edited.value(QStringLiteral("anchor")), record.value(QStringLiteral("anchor")));
    QCOMPARE(edited.value(QStringLiteral("comment")).toString(), QStringLiteral("Fine after all."));
    QCOMPARE(edited.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(edited.value(QStringLiteral("created_at")), record.value(QStringLiteral("created_at")));
    QCOMPARE(edited.value(QStringLiteral("created_origin_id")).toString(), QStringLiteral("android-pixel"));
    QCOMPARE(edited.value(QStringLiteral("origin_id")).toString(), QStringLiteral("mac-okular"));
    QCOMPARE(ReviewCardFeedback::validationError(edited), QString());
}

void ReviewCardFeedbackTest::testValidationMatchesTheServer()
{
    const QJsonObject valid = specExample();
    const auto broken = [&](const std::function<void(QJsonObject &)> &change) {
        QJsonObject record = valid;
        change(record);
        return !ReviewCardFeedback::validationError(record).isEmpty();
    };
    const auto anchorChange = [](QJsonObject &record, const std::function<void(QJsonObject &)> &change) {
        QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
        change(anchor);
        record.insert(QStringLiteral("anchor"), anchor);
    };
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("feedback_id"), QStringLiteral("5B0F6C1E-2A47-4D1B-9C35-7E0A4F2B9D13")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("feedback_id"), QStringLiteral("5b0f6c1e-2a47-1d1b-9c35-7e0a4f2b9d13")); })); // not v4
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("polarity"), QStringLiteral("neutral")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("updated_at"), QStringLiteral("2026-10-20T10:14:03")); })); // no timezone
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("deleted"), true); }));                                     // no deleted_at
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("deleted_at"), QStringLiteral("2026-10-20T10:14:03Z")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("comment"), QString(4001, QLatin1Char('x'))); }));
    QVERIFY(broken([](QJsonObject &r) { r.remove(QStringLiteral("created_origin_id")); }));
    QVERIFY(broken([&](QJsonObject &r) { anchorChange(r, [](QJsonObject &a) { a.insert(QStringLiteral("version"), 2); }); }));
    QVERIFY(broken([&](QJsonObject &r) { anchorChange(r, [](QJsonObject &a) { a.insert(QStringLiteral("quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("  ")}}); }); }));
    QVERIFY(broken([&](QJsonObject &r) { anchorChange(r, [](QJsonObject &a) { a.insert(QStringLiteral("segment_content_sha256"), QString(64, QLatin1Char('A'))); }); }));
    QVERIFY(broken([&](QJsonObject &r) {
        anchorChange(r, [](QJsonObject &a) {
            QJsonObject pdf = a.value(QStringLiteral("pdf")).toObject();
            pdf.insert(QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QJsonArray{0.5, 0.1, 0.4, 0.2}}}});
            a.insert(QStringLiteral("pdf"), pdf);
        });
    }));
    QVERIFY(broken([&](QJsonObject &r) {
        anchorChange(r, [](QJsonObject &a) {
            QJsonObject pdf = a.value(QStringLiteral("pdf")).toObject();
            pdf.insert(QStringLiteral("regions"), QJsonArray{});
            a.insert(QStringLiteral("pdf"), pdf);
        });
    }));
}

void ReviewCardFeedbackTest::testMergeFollowsTheServerTable_data()
{
    QTest::addColumn<QJsonObject>("stored");
    QTest::addColumn<QJsonObject>("incoming");
    QTest::addColumn<bool>("applies");
    const QJsonObject base = specExample();
    const QJsonObject a2 = withWrite(base, 2, QStringLiteral("2026-10-20T11:00:00.000Z"), QStringLiteral("mac-okular"));
    const QJsonObject b2 = withWrite(base, 2, QStringLiteral("2026-10-20T11:30:00.000Z"), QStringLiteral("android-pixel"));
    const QJsonObject deleted2 = withWrite(base, 2, QStringLiteral("2026-10-20T11:00:00.000Z"), QStringLiteral("mac-okular"), true);
    const QJsonObject editLater2 = withWrite(base, 2, QStringLiteral("2026-10-20T12:00:00.000Z"), QStringLiteral("android-pixel"));
    const QJsonObject undo3 = withWrite(base, 3, QStringLiteral("2026-10-20T11:00:05.000Z"), QStringLiteral("mac-okular"));
    const QJsonObject edit3 = withWrite(base, 3, QStringLiteral("2026-10-20T10:59:00.000Z"), QStringLiteral("mac-okular"));
    // The §5 table, in either arrival order.
    QTest::newRow("later updated_at wins at equal revision") << a2 << b2 << true;
    QTest::newRow("earlier updated_at loses at equal revision") << b2 << a2 << false;
    QTest::newRow("delete is sticky against a later edit") << deleted2 << editLater2 << false;
    QTest::newRow("a delete beats an edit of the same revision and time") << a2 << deleted2 << true;
    // A delete wins an equal revision whatever the times and the arrival order (server 244092c).
    const QJsonObject editEarlier2 = withWrite(base, 2, QStringLiteral("2026-10-20T10:00:00.000Z"), QStringLiteral("android-pixel"));
    const QJsonObject editSame2 = withWrite(base, 2, QStringLiteral("2026-10-20T11:00:00.000Z"), QStringLiteral("zz-origin"));
    QTest::newRow("delete arrives after a later edit") << editLater2 << deleted2 << true;
    QTest::newRow("delete arrives after an earlier edit") << editEarlier2 << deleted2 << true;
    QTest::newRow("delete arrives after an edit at the same time") << editSame2 << deleted2 << true;
    QTest::newRow("later edit arrives after the delete") << deleted2 << editLater2 << false;
    QTest::newRow("earlier edit arrives after the delete") << deleted2 << editEarlier2 << false;
    QTest::newRow("same-time edit arrives after the delete") << deleted2 << editSame2 << false;
    const QJsonObject deletedLater2 = withWrite(base, 2, QStringLiteral("2026-10-20T11:45:00.000Z"), QStringLiteral("android-pixel"), true);
    QTest::newRow("two deletes: the later wins") << deleted2 << deletedLater2 << true;
    QTest::newRow("two deletes: the earlier loses") << deletedLater2 << deleted2 << false;
    QTest::newRow("undo restores") << deleted2 << undo3 << true;
    QTest::newRow("a higher revision always applies") << deleted2 << edit3 << true;
    QTest::newRow("a stale revision is superseded") << edit3 << b2 << false;
    QTest::newRow("a new record applies") << QJsonObject() << base << true;
}

void ReviewCardFeedbackTest::testMergeFollowsTheServerTable()
{
    QFETCH(QJsonObject, stored);
    QFETCH(QJsonObject, incoming);
    QFETCH(bool, applies);
    QCOMPARE(ReviewCardFeedback::applies(incoming, stored), applies);
    QCOMPARE(ReviewCardFeedback::merge(stored, incoming).value(QStringLiteral("revision")), (applies ? incoming : stored).value(QStringLiteral("revision")));
}

void ReviewCardFeedbackTest::testDeleteWinsInEitherOrderThenUndoRevives()
{
    // Either arrival order of an equal-revision edit and delete ends deleted; Undo revives it.
    const QJsonObject base = specExample();
    const QJsonObject deleted = withWrite(base, 2, QStringLiteral("2026-10-20T11:00:00.000Z"), QStringLiteral("mac-okular"), true);
    for (const QString &time : {QStringLiteral("2026-10-20T10:00:00.000Z"), QStringLiteral("2026-10-20T11:00:00.000Z"), QStringLiteral("2026-10-20T12:00:00.000Z")}) {
        const QJsonObject edit = withWrite(base, 2, time, QStringLiteral("android-pixel"));
        for (const bool deleteFirst : {true, false}) {
            const QJsonObject result = deleteFirst ? ReviewCardFeedback::merge(ReviewCardFeedback::merge(base, deleted), edit)
                                                   : ReviewCardFeedback::merge(ReviewCardFeedback::merge(base, edit), deleted);
            QVERIFY2(result.value(QStringLiteral("deleted")).toBool(), qPrintable(time));
            const QJsonObject restored = ReviewCardFeedback::merge(result, ReviewCardFeedback::undo(result, QStringLiteral("android-pixel")));
            QVERIFY(!restored.value(QStringLiteral("deleted")).toBool());
            QCOMPARE(restored.value(QStringLiteral("revision")).toInt(), 3);
        }
    }
}

void ReviewCardFeedbackTest::testImmutableFieldsComeFromTheFirstWrite()
{
    const QJsonObject first = specExample();
    QJsonObject later = withWrite(first, 2, QStringLiteral("2026-10-20T11:00:00.000Z"), QStringLiteral("mac-okular"));
    later.insert(QStringLiteral("card_id"), QStringLiteral("another-card"));
    later.insert(QStringLiteral("created_at"), QStringLiteral("2026-10-21T00:00:00.000Z"));
    later.insert(QStringLiteral("created_origin_id"), QStringLiteral("mac-okular"));
    later.insert(QStringLiteral("anchor"), QJsonObject{{QStringLiteral("version"), 1}});
    later.insert(QStringLiteral("comment"), QStringLiteral("changed"));
    const QJsonObject merged = ReviewCardFeedback::merge(first, later);
    QCOMPARE(merged.value(QStringLiteral("comment")).toString(), QStringLiteral("changed"));
    for (const QString &key : {QStringLiteral("card_id"), QStringLiteral("anchor"), QStringLiteral("created_at"), QStringLiteral("created_origin_id")})
        QCOMPARE(merged.value(key), first.value(key));
}

void ReviewCardFeedbackTest::testSharedAnchoringVectors_data()
{
    QTest::addColumn<QJsonObject>("vector");
    // A copy of thoughts-server protocol/fixtures/review-card-feedback-anchoring.json (§9).
    QFile file(QStringLiteral(KDESRCDIR "data/review-card-feedback-anchoring.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonArray cases = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("cases")).toArray();
    QVERIFY(cases.size() >= 12);
    for (const QJsonValue &value : cases) {
        const QJsonObject vector = value.toObject();
        QTest::newRow(qPrintable(vector.value(QStringLiteral("name")).toString())) << vector;
    }
}

void ReviewCardFeedbackTest::testSharedAnchoringVectors()
{
    QFETCH(QJsonObject, vector);
    const QJsonObject quote = vector.value(QStringLiteral("quote")).toObject();
    const QJsonObject expected = vector.value(QStringLiteral("expected")).toObject();
    const auto resolution = ReviewCardFeedback::resolve(vector.value(QStringLiteral("answer_text")).toString(), quote.value(QStringLiteral("exact")).toString(),
                                                        quote.value(QStringLiteral("prefix")).toString(), quote.value(QStringLiteral("suffix")).toString(),
                                                        vector.value(QStringLiteral("same_pdf")).toBool());
    QCOMPARE(resolution.state, expected.value(QStringLiteral("state")).toString());
    QCOMPARE(resolution.start, expected.value(QStringLiteral("start")).isNull() ? -1 : expected.value(QStringLiteral("start")).toInt());
    QCOMPARE(resolution.end, expected.value(QStringLiteral("end")).isNull() ? -1 : expected.value(QStringLiteral("end")).toInt());
}

void ReviewCardFeedbackTest::testAnswerTextAndDrawnRange()
{
    // Two regions: "ab cd" on two lines of page 1, then "ef" on page 2.
    const QList<QList<Glyph>> regions{
        {glyph(1, QStringLiteral("a"), 0.10, 0.10), glyph(1, QStringLiteral("b"), 0.11, 0.10), glyph(1, QStringLiteral(" "), 0.12, 0.10),
         glyph(1, QStringLiteral("c"), 0.10, 0.20), glyph(1, QStringLiteral("d\n"), 0.11, 0.20)},
        {glyph(2, QStringLiteral("e"), 0.30, 0.50), glyph(2, QStringLiteral("f"), 0.31, 0.50)}};
    const AnswerText answer = ReviewCardFeedback::answerText(regions);
    QCOMPARE(answer.text, QStringLiteral("ab cd\n\nef"));
    QCOMPARE(answer.source.size(), answer.text.size());
    QCOMPARE(answer.source.at(6), -1); // the region joiner
    // C4 (g): a quote outside the card's regions never matches.
    QCOMPARE(ReviewCardFeedback::resolve(answer.text, QStringLiteral("xyz")).state, QStringLiteral("orphaned"));
    // "bcde" spans both lines of page 1 and page 2: one rectangle per line, blanks left out.
    const auto hit = ReviewCardFeedback::resolve(answer.text, QStringLiteral("b cd e"));
    QCOMPARE(hit.state, QStringLiteral("relocated"));
    const auto rects = ReviewCardFeedback::rangeRects(answer, hit.start, hit.end);
    QCOMPARE(rects.size(), 3);
    QCOMPARE(rects.at(0).first, 1);
    QCOMPARE(rects.at(0).second, QRectF(0.11, 0.10, 0.01, 0.02));
    QCOMPARE(rects.at(1).second, QRectF(0.10, 0.20, 0.02, 0.02));
    QCOMPARE(rects.at(2), (QPair<int, QRectF>(2, QRectF(0.30, 0.50, 0.01, 0.02))));
}

void ReviewCardFeedbackTest::testSelectionQualification()
{
    const QList<QPair<int, QRectF>> answer{{3, QRectF(QPointF(0.1, 0.2), QPointF(0.9, 0.4))}, {4, QRectF(QPointF(0.1, 0.1), QPointF(0.9, 0.2))}};
    // C6: inside (also across pages), within the 0.01 margin, partly outside, on another page.
    QVERIFY(ReviewCardFeedback::qualifies({{3, QRectF(QPointF(0.12, 0.21), QPointF(0.8, 0.23))}, {4, QRectF(QPointF(0.1, 0.1), QPointF(0.5, 0.12))}}, answer));
    QVERIFY(ReviewCardFeedback::qualifies({{3, QRectF(QPointF(0.1, 0.40), QPointF(0.5, 0.418))}}, answer));
    QVERIFY(!ReviewCardFeedback::qualifies({{3, QRectF(QPointF(0.12, 0.21), QPointF(0.8, 0.23))}, {3, QRectF(QPointF(0.1, 0.45), QPointF(0.8, 0.47))}}, answer));
    QVERIFY(!ReviewCardFeedback::qualifies({{5, QRectF(QPointF(0.12, 0.21), QPointF(0.8, 0.23))}}, answer));
    QVERIFY(!ReviewCardFeedback::qualifies({}, answer));
}

void ReviewCardFeedbackTest::testCreateEditDeleteUndo()
{
    const QJsonObject anchor = ReviewCardFeedback::anchor(QStringLiteral("  the passage \n"), QStringLiteral("Before "), QStringLiteral(" after"),
                                                          QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), QString(64, QLatin1Char('f')),
                                                          ReviewCardFeedback::regionsJson({{2, QRectF(QPointF(0.1, 0.2), QPointF(0.5, 0.25))}}),
                                                          QStringLiteral("0"), QString(64, QLatin1Char('a')));
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-20T10:00:00.123Z"), Qt::ISODateWithMs);
    // C7: Like creates a positive record without a comment.
    const QJsonObject liked = ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QString(), anchor, QStringLiteral("mac-okular"), now);
    QCOMPARE(ReviewCardFeedback::validationError(liked), QString());
    QCOMPARE(liked.value(QStringLiteral("polarity")).toString(), QStringLiteral("positive"));
    QCOMPARE(liked.value(QStringLiteral("comment")).toString(), QString());
    QCOMPARE(liked.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(liked.value(QStringLiteral("created_at")).toString(), QStringLiteral("2026-10-20T10:00:00.123Z"));
    QCOMPARE(liked.value(QStringLiteral("updated_at")), liked.value(QStringLiteral("created_at")));
    QCOMPARE(liked.value(QStringLiteral("created_origin_id")).toString(), QStringLiteral("mac-okular"));
    QCOMPARE(liked.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), QStringLiteral("the passage"));
    QCOMPARE(liked.value(QStringLiteral("feedback_id")).toString(), QUuid(liked.value(QStringLiteral("feedback_id")).toString()).toString(QUuid::WithoutBraces));
    // An edit: revision + 1, created_at kept, and never at or before the stored updated_at.
    const QJsonObject edited = ReviewCardFeedback::edit(liked, ReviewCardFeedback::negative(), QStringLiteral("Wrong sign."), QStringLiteral("mac-okular"), now.addSecs(-60));
    QCOMPARE(edited.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(edited.value(QStringLiteral("created_at")), liked.value(QStringLiteral("created_at")));
    QVERIFY(edited.value(QStringLiteral("updated_at")).toString() > liked.value(QStringLiteral("updated_at")).toString());
    QVERIFY(ReviewCardFeedback::applies(edited, liked));
    // Delete and Undo: revision n + 2, not deleted, no deleted_at; the tombstone keeps the record.
    const QJsonObject tombstone = ReviewCardFeedback::remove(edited, QStringLiteral("mac-okular"), now.addSecs(10));
    QCOMPARE(tombstone.value(QStringLiteral("deleted")).toBool(), true);
    QCOMPARE(tombstone.value(QStringLiteral("deleted_at")), tombstone.value(QStringLiteral("updated_at")));
    QCOMPARE(tombstone.value(QStringLiteral("comment")).toString(), QStringLiteral("Wrong sign."));
    QCOMPARE(ReviewCardFeedback::validationError(tombstone), QString());
    const QJsonObject restored = ReviewCardFeedback::undo(tombstone, QStringLiteral("mac-okular"), now.addSecs(11));
    QCOMPARE(restored.value(QStringLiteral("revision")).toInt(), edited.value(QStringLiteral("revision")).toInt() + 2);
    QCOMPARE(restored.value(QStringLiteral("deleted")).toBool(), false);
    QVERIFY(!restored.contains(QStringLiteral("deleted_at")));
    QVERIFY(ReviewCardFeedback::applies(restored, tombstone));
    QCOMPARE(ReviewCardFeedback::validationError(restored), QString());
    // The comment is trimmed and cut to 4000 characters (code points).
    QCOMPARE(ReviewCardFeedback::cleanComment(QStringLiteral("  x  ")), QStringLiteral("x"));
    QCOMPARE(ReviewCardFeedback::cleanComment(QString(5000, QLatin1Char('y'))).size(), 4000);

    // Projections: only the card's records, in created_at order; deleted ones on request.
    QJsonObject projections;
    QJsonObject other = liked;
    other.insert(QStringLiteral("feedback_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    other.insert(QStringLiteral("card_id"), QStringLiteral("card-2"));
    QJsonObject earlier = liked;
    earlier.insert(QStringLiteral("feedback_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    earlier.insert(QStringLiteral("created_at"), QStringLiteral("2026-10-19T10:00:00.000Z"));
    for (const QJsonObject &record : {tombstone, other, earlier})
        projections.insert(ReviewCardFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
    QCOMPARE(ReviewCardFeedback::recordsOfCard(projections, QStringLiteral("card-1")), QList<QJsonObject>{earlier});
    QCOMPARE(ReviewCardFeedback::recordsOfCard(projections, QStringLiteral("card-1"), true), (QList<QJsonObject>{earlier, tombstone}));
}

void ReviewCardFeedbackTest::testAgentText()
{
    // C9: the exact §6.5 text for liked + disliked + not found, in created_at order.
    QJsonObject liked = specExample();
    liked.insert(QStringLiteral("feedback_id"), QStringLiteral("0c4d1f7a-9b2e-4c5d-8e6f-1a2b3c4d5e6f"));
    liked.insert(QStringLiteral("polarity"), QStringLiteral("positive"));
    liked.insert(QStringLiteral("comment"), QStringLiteral("Clear\nexplanation."));
    liked.insert(QStringLiteral("created_at"), QStringLiteral("2026-10-20T09:00:00.000Z"));
    QJsonObject disliked = specExample();
    QJsonObject anchor = disliked.value(QStringLiteral("anchor")).toObject();
    anchor.insert(QStringLiteral("quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("β is set\n  to 0.1")}});
    disliked.insert(QStringLiteral("anchor"), anchor);
    disliked.insert(QStringLiteral("comment"), QStringLiteral("Should be 0.05 in our setup."));
    QJsonObject plain = liked;
    plain.insert(QStringLiteral("feedback_id"), QStringLiteral("7e8f9a0b-1c2d-4e3f-9a5b-6c7d8e9f0a1b"));
    plain.insert(QStringLiteral("comment"), QString());
    plain.insert(QStringLiteral("created_at"), QStringLiteral("2026-10-20T12:00:00.000Z"));
    anchor.insert(QStringLiteral("quote"), QJsonObject{{QStringLiteral("exact"), QString(310, QLatin1Char('q'))}});
    plain.insert(QStringLiteral("anchor"), anchor);
    QJsonObject gone = plain;
    gone.insert(QStringLiteral("deleted"), true);
    gone.insert(QStringLiteral("feedback_id"), QStringLiteral("9e8f9a0b-1c2d-4e3f-9a5b-6c7d8e9f0a1b"));
    QCOMPARE(ReviewCardFeedback::agentSection({disliked, plain, liked, gone}, {disliked.value(QStringLiteral("feedback_id")).toString()}),
             QStringLiteral("Answer feedback:\n"
                            "- Liked: \"the KL term keeps the policy close\" — Clear explanation.\n"
                            "- Disliked: \"β is set to 0.1\" — Should be 0.05 in our setup. (not found in the current answer)\n"
                            "- Liked: \"%1…\"").arg(QString(300, QLatin1Char('q'))));
    QCOMPARE(ReviewCardFeedback::agentSection({gone}, {}), QString());
}

void ReviewCardFeedbackTest::testAgentJson()
{
    // C10's fields (§7).
    const QJsonObject record = specExample();
    QJsonObject card{{QStringLiteral("card_id"), record.value(QStringLiteral("card_id"))},
                     {QStringLiteral("primary"), QJsonObject{{QStringLiteral("pdf_sha256"), record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256"))},
                                                            {QStringLiteral("source_anchor"), QJsonObject{{QStringLiteral("segments"), QJsonArray{QJsonObject{
                                                                {QStringLiteral("segment_id"), QStringLiteral("0")}, {QStringLiteral("content_sha256"), QString(64, QLatin1Char('b'))}}}}}}}}};
    QJsonObject json = ReviewCardFeedback::agentJson(record, card);
    QCOMPARE(json, (QJsonObject{{QStringLiteral("feedback_id"), record.value(QStringLiteral("feedback_id"))}, {QStringLiteral("polarity"), QStringLiteral("negative")},
                                {QStringLiteral("quote"), QStringLiteral("the KL term keeps the policy close")}, {QStringLiteral("comment"), record.value(QStringLiteral("comment"))},
                                {QStringLiteral("created_at"), record.value(QStringLiteral("created_at"))}, {QStringLiteral("updated_at"), record.value(QStringLiteral("updated_at"))},
                                {QStringLiteral("origin"), QStringLiteral("android-pixel")}, {QStringLiteral("anchor_state"), QStringLiteral("exact")},
                                {QStringLiteral("page"), 7}, {QStringLiteral("segment_id"), QStringLiteral("0")}, {QStringLiteral("source_changed"), true},
                                {QStringLiteral("card_id"), record.value(QStringLiteral("card_id"))}, {QStringLiteral("shared"), false}}));
    // Shown on another card of the document: the card it was made on, and shared.
    QJsonObject otherCard = card;
    otherCard.insert(QStringLiteral("card_id"), QStringLiteral("card-2"));
    const QJsonObject shared = ReviewCardFeedback::agentJson(record, otherCard, QStringLiteral("relocated"));
    QCOMPARE(shared.value(QStringLiteral("card_id")), record.value(QStringLiteral("card_id")));
    QCOMPARE(shared.value(QStringLiteral("shared")), QJsonValue(true));
    QCOMPARE(shared.value(QStringLiteral("anchor_state")).toString(), QStringLiteral("relocated"));
    QCOMPARE(ReviewCardFeedback::agentArray({record}, otherCard, false, {{record.value(QStringLiteral("feedback_id")).toString(), QStringLiteral("relocated")}})
                 .first().toObject().value(QStringLiteral("anchor_state")).toString(),
             QStringLiteral("relocated"));
    // Another PDF now; the segment unchanged; no segment known.
    QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('e')));
    primary.insert(QStringLiteral("source_anchor"), QJsonObject{{QStringLiteral("segments"), QJsonArray{QJsonObject{
        {QStringLiteral("segment_id"), QStringLiteral("0")}, {QStringLiteral("content_sha256"), QStringLiteral("3775b1627bce1bbc61bdd810a212daab7db3fbfc7932002aacc578a9954d87cd")}}}}});
    card.insert(QStringLiteral("primary"), primary);
    json = ReviewCardFeedback::agentJson(record, card);
    QCOMPARE(json.value(QStringLiteral("anchor_state")).toString(), QStringLiteral("pdf_changed"));
    QCOMPARE(json.value(QStringLiteral("source_changed")), QJsonValue(false));
    primary.remove(QStringLiteral("source_anchor"));
    card.insert(QStringLiteral("primary"), primary);
    QVERIFY(ReviewCardFeedback::agentJson(record, card).value(QStringLiteral("source_changed")).isNull());
    // Deleted records only on request, with deleted_at.
    const QJsonObject tombstone = ReviewCardFeedback::remove(record, QStringLiteral("mac-okular"));
    QCOMPARE(ReviewCardFeedback::agentArray({tombstone}, card), QJsonArray());
    const QJsonArray all = ReviewCardFeedback::agentArray({tombstone}, card, true);
    QCOMPARE(all.size(), 1);
    QCOMPARE(all.first().toObject().value(QStringLiteral("deleted_at")), tombstone.value(QStringLiteral("deleted_at")));
}

void ReviewCardFeedbackTest::testSharedFeedback()
{
    // Feedback is shared by the cards of a document, where its passage lies in their answer.
    const QString document = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848");
    const QString shown(64, QLatin1Char('a')), earlier(64, QLatin1Char('b'));
    const auto make = [&](const QString &cardId, const QString &quote, const QString &pdfSha256, const QList<QPair<int, QRectF>> &lines,
                          const QString &documentId = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848")) {
        return ReviewCardFeedback::create(cardId, ReviewCardFeedback::positive(), QString(),
                                          ReviewCardFeedback::anchor(quote, QString(), QString(), documentId, pdfSha256, ReviewCardFeedback::regionsJson(lines)),
                                          QStringLiteral("phone"));
    };
    // The card's document: the shown PDF's ID and its own primary IDs, and its PDF stem.
    const auto cardRecord = [](const QString &id, const QString &documentId, const QString &stem = QStringLiteral("notes")) {
        return QJsonObject{{QStringLiteral("card_id"), id},
                           {QStringLiteral("primary"), QJsonObject{{QStringLiteral("pdf_document_id"), documentId}, {QStringLiteral("document_id"), QString()},
                                                                   {QStringLiteral("pdf_stem"), stem}}}};
    };
    const QJsonObject card = cardRecord(QStringLiteral("card-a"), document);
    QCOMPARE(ReviewCardFeedback::documentOf(card).ids, QSet<QString>{document});
    QCOMPARE(ReviewCardFeedback::documentOf(card).pdfStem, QStringLiteral("notes"));
    QCOMPARE(ReviewCardFeedback::documentOf(card, {QStringLiteral("x"), QString()}).ids, (QSet<QString>{document, QStringLiteral("x")}));
    QVERIFY(ReviewCardFeedback::documentOf(QJsonObject{}).ids.isEmpty());
    // The cards that exist: B and C (another document); a record of an unknown or deleted card is never shared.
    const ReviewCardFeedback::HomeCards homeCards{{QStringLiteral("card-b"), cardRecord(QStringLiteral("card-b"), document)},
                                                  {QStringLiteral("card-c"), cardRecord(QStringLiteral("card-c"), QStringLiteral("5b0f6c1e-2a47-4d1b-9c35-7e0a4f2b9d13"), QStringLiteral("other"))}};

    const QList<QPair<int, QRectF>> answer{{1, QRectF(QPointF(0.1, 0.1), QPointF(0.9, 0.3))}};
    const QJsonObject own = make(QStringLiteral("card-a"), QStringLiteral("own"), shown, {{1, QRectF(QPointF(0.2, 0.5), QPointF(0.4, 0.52))}}, QStringLiteral("other"));
    const QJsonObject inside = make(QStringLiteral("card-b"), QStringLiteral("inside"), shown, {{1, QRectF(QPointF(0.2, 0.2), QPointF(0.4, 0.22))}});
    const QJsonObject outside = make(QStringLiteral("card-b"), QStringLiteral("the KL term"), shown, {{1, QRectF(QPointF(0.2, 0.5), QPointF(0.4, 0.52))}});
    const QJsonObject moved = make(QStringLiteral("card-b"), QStringLiteral("the KL term"), earlier, {{2, QRectF(QPointF(0.2, 0.5), QPointF(0.4, 0.52))}});
    const QJsonObject missing = make(QStringLiteral("card-b"), QStringLiteral("not in this answer"), earlier, {{1, QRectF(QPointF(0.2, 0.2), QPointF(0.4, 0.22))}});
    const QJsonObject otherDocument = make(QStringLiteral("card-c"), QStringLiteral("inside"), shown, {{1, QRectF(QPointF(0.2, 0.2), QPointF(0.4, 0.22))}},
                                           QStringLiteral("5b0f6c1e-2a47-4d1b-9c35-7e0a4f2b9d13"));
    const QJsonObject deleted = ReviewCardFeedback::remove(inside, QStringLiteral("mac"));
    QVERIFY(!ReviewCardFeedback::isShared(own, QStringLiteral("card-a")));
    QVERIFY(ReviewCardFeedback::isShared(inside, QStringLiteral("card-a")));

    // Candidates: its own records whatever their document, and the other cards' of its document.
    const ReviewCardFeedback::CardDocument documents = ReviewCardFeedback::documentOf(card);
    QJsonObject deletedOther = deleted;
    deletedOther.insert(QStringLiteral("feedback_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QJsonObject unknownCard = make(QStringLiteral("card-gone"), QStringLiteral("inside"), shown, {{1, QRectF(QPointF(0.2, 0.2), QPointF(0.4, 0.22))}});
    const QList<QJsonObject> all{own, inside, outside, moved, missing, otherDocument, deletedOther, unknownCard};
    QCOMPARE(ReviewCardFeedback::recordsForCard(all, QStringLiteral("card-a"), documents, homeCards).size(), 5);
    QCOMPARE(ReviewCardFeedback::recordsForCard(all, QStringLiteral("card-a"), documents, homeCards, true).size(), 6);
    QVERIFY(!ReviewCardFeedback::recordsForCard(all, QStringLiteral("card-a"), documents, homeCards).contains(otherDocument));
    QVERIFY(!ReviewCardFeedback::recordsForCard(all, QStringLiteral("card-a"), documents, homeCards).contains(unknownCard));
    QCOMPARE(ReviewCardFeedback::recordsForCard(all, QStringLiteral("card-a"), documents, {}), QList<QJsonObject>{own});
    QCOMPARE(ReviewCardFeedback::recordsForCard(all, QStringLiteral("card-a"), {}, homeCards), QList<QJsonObject>{own});
    QJsonObject projections;
    for (const QJsonObject &record : all) projections.insert(ReviewCardFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
    QCOMPARE(ReviewCardFeedback::recordsForCard(projections, QStringLiteral("card-a"), documents, homeCards, true),
             ReviewCardFeedback::recordsForCard(all, QStringLiteral("card-a"), documents, homeCards, true));
    // The document: the anchor's ID; without a valid one, or a card without IDs, the home card's
    // IDs; without those, the home card's PDF stem.
    const QJsonObject homeB = homeCards.value(QStringLiteral("card-b"));
    QVERIFY(ReviewCardFeedback::inDocument(inside, homeB, documents));
    QVERIFY(!ReviewCardFeedback::inDocument(otherDocument, homeB, documents));
    QJsonObject noAnchorDocument = inside;
    QJsonObject anchor = noAnchorDocument.value(QStringLiteral("anchor")).toObject();
    QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
    pdf.insert(QStringLiteral("document_id"), QStringLiteral("not-a-uuid"));
    anchor.insert(QStringLiteral("pdf"), pdf);
    noAnchorDocument.insert(QStringLiteral("anchor"), anchor);
    QVERIFY(ReviewCardFeedback::inDocument(noAnchorDocument, homeB, documents));
    QVERIFY(!ReviewCardFeedback::inDocument(noAnchorDocument, homeCards.value(QStringLiteral("card-c")), documents));
    const ReviewCardFeedback::CardDocument stemOnly{{}, QStringLiteral("notes")};
    QVERIFY(ReviewCardFeedback::inDocument(inside, homeB, stemOnly));
    QVERIFY(!ReviewCardFeedback::inDocument(inside, homeCards.value(QStringLiteral("card-c")), stemOnly));
    QVERIFY(!ReviewCardFeedback::inDocument(inside, QJsonObject{}, ReviewCardFeedback::CardDocument{}));

    // Same PDF: inside the answer regions (grown by 0.01) or not; the text is not read.
    int reads = 0;
    const QString text = QStringLiteral("In practice, the KL\nterm keeps the policy close.");
    const std::function<QString()> answerText = [&] {
        ++reads;
        return text;
    };
    QCOMPARE(ReviewCardFeedback::resolveShared(inside, answerText, shown, answer).state, QStringLiteral("exact"));
    QCOMPARE(ReviewCardFeedback::resolveShared(outside, answerText, shown, answer).state, QStringLiteral("orphaned"));
    QCOMPARE(reads, 0);
    QJsonObject edge = make(QStringLiteral("card-b"), QStringLiteral("edge"), shown, {{1, QRectF(QPointF(0.2, 0.305), QPointF(0.4, 0.3149))}});
    QCOMPARE(ReviewCardFeedback::resolveShared(edge, answerText, shown, answer).state, QStringLiteral("exact"));
    // Another PDF: found in the answer text (relocated) or not.
    const auto relocated = ReviewCardFeedback::resolveShared(moved, answerText, shown, answer);
    QCOMPARE(relocated.state, QStringLiteral("relocated"));
    QCOMPARE(text.mid(relocated.start, relocated.end - relocated.start), QStringLiteral("the KL\nterm"));
    QCOMPARE(ReviewCardFeedback::resolveShared(missing, answerText, shown, answer).state, QStringLiteral("orphaned"));

    // sharedStates: only other cards' records of its document found in its answer, not deleted;
    // the answer text read once.
    reads = 0;
    const auto states = ReviewCardFeedback::sharedStates(QStringLiteral("card-a"), documents, homeCards, shown, answer, all + QList<QJsonObject>{deleted}, answerText);
    QCOMPARE(states, (QHash<QString, QString>{{inside.value(QStringLiteral("feedback_id")).toString(), QStringLiteral("exact")},
                                               {moved.value(QStringLiteral("feedback_id")).toString(), QStringLiteral("relocated")}}));
    QCOMPARE(reads, 1);
    reads = 0;
    QVERIFY(ReviewCardFeedback::sharedStates(QStringLiteral("card-a"), documents, homeCards, shown, answer, {own, inside, outside}, answerText).size() == 1);
    QCOMPARE(reads, 0);
    // Another PDF and no answer text to search: not shown.
    QVERIFY(ReviewCardFeedback::sharedStates(QStringLiteral("card-a"), documents, homeCards, shown, answer, {moved}, [] { return QString(); }).isEmpty());

    // Copy for agent: a shared line names its card's question, before any "not found" (never for
    // shared records), collapsed and cut at 300 characters; without a question just the card.
    QJsonObject liked = inside;
    liked.insert(QStringLiteral("comment"), QStringLiteral("Clear."));
    QJsonObject second = moved;
    second.insert(QStringLiteral("created_at"), QStringLiteral("2099-01-01T00:00:00.000Z"));
    QJsonObject third = missing;
    third.insert(QStringLiteral("card_id"), QStringLiteral("card-c"));
    third.insert(QStringLiteral("created_at"), QStringLiteral("2099-01-02T00:00:00.000Z"));
    QJsonObject ownLater = own;
    ownLater.insert(QStringLiteral("created_at"), QStringLiteral("2099-01-03T00:00:00.000Z"));
    const QHash<QString, QString> questions{{QStringLiteral("card-b"), QStringLiteral("What does\n the  KL term do?")},
                                            {QStringLiteral("card-c"), QString()}};
    liked.insert(QStringLiteral("created_at"), QStringLiteral("2098-01-01T00:00:00.000Z"));
    QCOMPARE(ReviewCardFeedback::agentSection({liked, second, third, ownLater}, {ownLater.value(QStringLiteral("feedback_id")).toString()}, QStringLiteral("card-a"), questions),
             QStringLiteral("Answer feedback:\n"
                            "- Liked: \"inside\" — Clear. (from another card: \"What does the KL term do?\")\n"
                            "- Liked: \"the KL term\" (from another card: \"What does the KL term do?\")\n"
                            "- Liked: \"not in this answer\" (from another card)\n"
                            "- Liked: \"own\" (not found in the current answer)"));
    QCOMPARE(ReviewCardFeedback::agentSection({liked}, {}, QStringLiteral("card-a"), {{QStringLiteral("card-b"), QString(310, QLatin1Char('q'))}}),
             QStringLiteral("Answer feedback:\n- Liked: \"inside\" — Clear. (from another card: \"%1…\")").arg(QString(300, QLatin1Char('q'))));
    // Without a card, as before.
    QCOMPARE(ReviewCardFeedback::agentSection({liked}, {}), QStringLiteral("Answer feedback:\n- Liked: \"inside\" — Clear."));
}

void ReviewCardFeedbackTest::testHtmlViewFeedback()
{
    // Given in the HTML view: view "html", the edition, no PDF regions; found by its quote.
    const QString pdf = QString(64, QLatin1Char('f'));
    const QJsonObject anchor = ReviewCardFeedback::anchor(QStringLiteral("second passage"), QStringLiteral("The first and "), QStringLiteral(" end."),
                                                          QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), pdf, QJsonArray());
    QJsonObject record = ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QString(), anchor, QStringLiteral("mac-okular"));
    // Without view "html" a record needs PDF regions.
    QVERIFY(!ReviewCardFeedback::validationError(record).isEmpty());
    record.insert(QStringLiteral("view"), QStringLiteral("html"));
    QVERIFY(!ReviewCardFeedback::validationError(record).isEmpty()); // and the edition
    record.insert(QStringLiteral("html_edition_sha256"), QString(64, QLatin1Char('e')));
    QCOMPARE(ReviewCardFeedback::validationError(record), QString());
    record.insert(QStringLiteral("view"), QStringLiteral("epub"));
    QVERIFY(!ReviewCardFeedback::validationError(record).isEmpty());
    record.insert(QStringLiteral("view"), QStringLiteral("html"));
    // In the PDF view of the same PDF it is found by its quote (it has no regions there).
    const auto found = ReviewCardFeedback::resolve(record, QStringLiteral("The first and second passage end."), pdf);
    QCOMPARE(found.state, QStringLiteral("relocated"));
    QCOMPARE(QStringLiteral("The first and second passage end.").mid(found.start, found.end - found.start), QStringLiteral("second passage"));
    QCOMPARE(ReviewCardFeedback::resolve(record, QStringLiteral("Nothing like it."), pdf).state, QStringLiteral("orphaned"));
}

void ReviewCardFeedbackTest::testVersionStamps()
{
    // thoughts-feedback-version-stamps-SPEC.md: the shared stamp, and answer feedback carrying it.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir current(QDir(root.path()).filePath(QStringLiteral("current_pdfs")));
    QVERIFY(current.mkpath(QStringLiteral(".")));
    const auto writeFile = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    const auto sha = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QByteArray notes = "%PDF-1.4\n% built notes\n";
    const QByteArray paper = "%PDF-1.4\n% imported paper\n";
    QVERIFY(writeFile(current.filePath(QStringLiteral("notes.pdf")), notes));
    QVERIFY(writeFile(current.filePath(QStringLiteral("paper.pdf")), paper));
    const QString notesId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString paperId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QFile index(current.filePath(QStringLiteral(".thoughts-library-index.json")));
    QVERIFY(index.open(QIODevice::WriteOnly));
    index.write(QJsonDocument(QJsonObject{{QStringLiteral("pdfs"), QJsonArray{
        QJsonObject{{QStringLiteral("document_id"), notesId}, {QStringLiteral("filename"), QStringLiteral("notes.pdf")}, {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), sha(notes)}},
        QJsonObject{{QStringLiteral("document_id"), paperId}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("kind"), QStringLiteral("unlinked")}, {QStringLiteral("pdf_sha256"), sha(paper)}}}}}).toJson());
    index.close();

    QCOMPARE(FeedbackVersion::field(), QStringLiteral("review_version"));
    QCOMPARE(FeedbackVersion::make(sha(notes), true), (QJsonObject{{QStringLiteral("pdf_sha256"), sha(notes)}, {QStringLiteral("source_built"), true}}));
    QVERIFY(FeedbackVersion::make(sha(notes).toUpper(), true).isEmpty());
    QVERIFY(FeedbackVersion::make(QStringLiteral("abc"), false).isEmpty());
    // The document's current PDF, else the one named by the stem.
    QCOMPARE(FeedbackVersion::documentPdfPath(root.path(), notesId), current.filePath(QStringLiteral("notes.pdf")));
    QCOMPARE(FeedbackVersion::documentPdfPath(root.path(), QUuid::createUuid().toString(QUuid::WithoutBraces), QStringLiteral("paper")), current.filePath(QStringLiteral("paper.pdf")));
    QVERIFY(FeedbackVersion::documentPdfPath(root.path(), QString(), QStringLiteral("missing")).isEmpty());
    QVERIFY(FeedbackVersion::documentPdfPath(root.path(), QString(), QStringLiteral("../notes")).isEmpty());
    // The file's hash (cached per file identity, a rewrite is a new version); source_built from the index.
    const QString notesPath = current.filePath(QStringLiteral("notes.pdf"));
    const QString paperPath = current.filePath(QStringLiteral("paper.pdf"));
    QCOMPARE(FeedbackVersion::forPdf(root.path(), notesPath), FeedbackVersion::make(sha(notes), true));
    QCOMPARE(FeedbackVersion::forPdf(root.path(), paperPath), FeedbackVersion::make(sha(paper), false));
    QCOMPARE(FeedbackVersion::forPdf(root.path(), paperPath, true), FeedbackVersion::make(sha(paper), true));
    const QByteArray rebuilt = notes + "% rebuilt with more text\n";
    QVERIFY(writeFile(notesPath, rebuilt));
    QCOMPARE(FeedbackVersion::cachedFileSha256(notesPath), sha(rebuilt));
    QCOMPARE(FeedbackVersion::forPdf(root.path(), notesPath), FeedbackVersion::make(sha(rebuilt), true));
    // The reader's hash of the open PDF wins over the file (it may have been replaced meanwhile).
    QCOMPARE(FeedbackVersion::forPdf(root.path(), notesPath, false, sha(notes)), FeedbackVersion::make(sha(notes), true));
    QVERIFY(FeedbackVersion::forPdf(root.path(), current.filePath(QStringLiteral("gone.pdf"))).isEmpty());
    // stamped(): set, replace, and left out when unknown.
    const QJsonObject stamp = FeedbackVersion::make(sha(paper), false);
    QJsonObject record{{QStringLiteral("x"), 1}};
    record = FeedbackVersion::stamped(record, stamp);
    QCOMPARE(record.value(QStringLiteral("review_version")).toObject(), stamp);
    record = FeedbackVersion::stamped(record, {});
    QVERIFY(!record.contains(QStringLiteral("review_version")));
    QCOMPARE(record.value(QStringLiteral("x")).toInt(), 1);

    // Answer feedback: the stamp is valid for the server, an edit's stamp replaces the earlier one,
    // Delete and Undo keep it, and merging keeps the winner's.
    const QJsonObject anchor = ReviewCardFeedback::anchor(QStringLiteral("the passage"), QString(), QString(), paperId, sha(paper),
                                                          ReviewCardFeedback::regionsJson({{1, QRectF(QPointF(0.1, 0.2), QPointF(0.5, 0.25))}}));
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-22T10:00:00.000Z"), Qt::ISODateWithMs);
    const QJsonObject created = FeedbackVersion::stamped(
        ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QString(), anchor, QStringLiteral("mac-okular"), now), stamp);
    QCOMPARE(ReviewCardFeedback::validationError(created), QString());
    QCOMPARE(created.value(QStringLiteral("review_version")).toObject(), stamp);
    const QJsonObject later = FeedbackVersion::make(sha(rebuilt), true);
    QJsonObject withUnknown = created;
    withUnknown.insert(QStringLiteral("future_field"), 7);
    const QJsonObject edited = FeedbackVersion::stamped(ReviewCardFeedback::edit(withUnknown, ReviewCardFeedback::negative(), QStringLiteral("No."), QStringLiteral("mac-okular"), now.addSecs(1)), later);
    QCOMPARE(ReviewCardFeedback::validationError(edited), QString());
    QCOMPARE(edited.value(QStringLiteral("review_version")).toObject(), later);
    QCOMPARE(edited.value(QStringLiteral("future_field")).toInt(), 7);
    const QJsonObject tombstone = ReviewCardFeedback::remove(edited, QStringLiteral("phone"), now.addSecs(2));
    QCOMPARE(tombstone.value(QStringLiteral("review_version")).toObject(), later);
    const QJsonObject revived = ReviewCardFeedback::undo(tombstone, QStringLiteral("mac-okular"), now.addSecs(3));
    QCOMPARE(revived.value(QStringLiteral("review_version")).toObject(), later);
    QCOMPARE(ReviewCardFeedback::merge(created, edited).value(QStringLiteral("review_version")).toObject(), later);
    QCOMPARE(ReviewCardFeedback::merge(edited, created).value(QStringLiteral("review_version")).toObject(), later);
    QCOMPARE(ReviewCardFeedback::merge(edited, created).value(QStringLiteral("future_field")).toInt(), 7);
    // An old record without a stamp stays valid, and is not given one by Delete.
    const QJsonObject old = ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QString(), anchor, QStringLiteral("phone"), now);
    QVERIFY(!ReviewCardFeedback::remove(old, QStringLiteral("mac-okular"), now.addSecs(1)).contains(QStringLiteral("review_version")));
}

QTEST_GUILESS_MAIN(ReviewCardFeedbackTest)
namespace
{
QJsonObject passageAnchor(const QString &exact, const QString &pdfSha256, double top)
{
    return ReviewCardFeedback::anchor(exact, QStringLiteral("Before "), QStringLiteral(" after"), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), pdfSha256,
                                      ReviewCardFeedback::regionsJson({{2, QRectF(QPointF(0.1, top), QPointF(0.5, top + 0.05))}}), QStringLiteral("0"),
                                      QString(64, QLatin1Char('a')));
}
QDateTime at(const QString &text)
{
    return QDateTime::fromString(text, Qt::ISODateWithMs);
}
const QString origin = QStringLiteral("mac-okular");
}

void ReviewCardFeedbackTest::testChangePassage()
{
    const QString pdf = QString(64, QLatin1Char('f'));
    const QJsonObject old = ReviewCardFeedback::edit(ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QStringLiteral("Nice."),
                                                                                passageAnchor(QStringLiteral("the old passage"), pdf, 0.2), origin,
                                                                                at(QStringLiteral("2026-10-20T10:00:00.000Z"))),
                                                     ReviewCardFeedback::positive(), QStringLiteral("Nice."), origin, at(QStringLiteral("2026-10-20T10:05:00.000Z")));
    QCOMPARE(old.value(QStringLiteral("revision")).toInt(), 2);
    // The same quote on the same PDF is the same passage: only an edit then.
    QVERIFY(ReviewCardFeedback::samePassage(old, passageAnchor(QStringLiteral("the old passage"), pdf, 0.6)));
    QVERIFY(!ReviewCardFeedback::samePassage(old, passageAnchor(QStringLiteral("the old passage"), QString(64, QLatin1Char('e')), 0.2)));
    QVERIFY(!ReviewCardFeedback::samePassage(old, passageAnchor(QStringLiteral("a new passage"), pdf, 0.2)));

    const QJsonObject anchor = passageAnchor(QStringLiteral("a new passage"), pdf, 0.4);
    const QDateTime now = at(QStringLiteral("2026-10-21T09:00:00.250Z"));
    QString error;
    const auto replacement = ReviewCardFeedback::replacePassage(old, ReviewCardFeedback::negative(), QStringLiteral("  Too vague.\n"), anchor, origin, now, &error);
    QVERIFY2(replacement, qPrintable(error));
    const QJsonObject &created = replacement->created;
    QCOMPARE(ReviewCardFeedback::validationError(created), QString());
    // A new record on the same card: new id, the new passage, the editor's polarity and comment.
    QVERIFY(created.value(QStringLiteral("feedback_id")).toString() != old.value(QStringLiteral("feedback_id")).toString());
    QVERIFY(!QUuid(created.value(QStringLiteral("feedback_id")).toString()).isNull());
    QCOMPARE(created.value(QStringLiteral("card_id")).toString(), QStringLiteral("card-1"));
    QCOMPARE(created.value(QStringLiteral("anchor")).toObject(), anchor);
    QCOMPARE(created.value(QStringLiteral("polarity")).toString(), QStringLiteral("negative"));
    QCOMPARE(created.value(QStringLiteral("comment")).toString(), QStringLiteral("Too vague."));
    QCOMPARE(created.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(created.value(QStringLiteral("created_at")).toString(), QStringLiteral("2026-10-21T09:00:00.250Z"));
    QCOMPARE(created.value(QStringLiteral("deleted")).toBool(), false);
    // The old one's tombstone: the next revision, deleted, the record kept.
    const QJsonObject &tombstone = replacement->tombstone;
    QCOMPARE(tombstone.value(QStringLiteral("feedback_id")), old.value(QStringLiteral("feedback_id")));
    QCOMPARE(tombstone.value(QStringLiteral("revision")).toInt(), 3);
    QVERIFY(tombstone.value(QStringLiteral("deleted")).toBool());
    QCOMPARE(tombstone.value(QStringLiteral("deleted_at")), tombstone.value(QStringLiteral("updated_at")));
    QCOMPARE(tombstone.value(QStringLiteral("anchor")), old.value(QStringLiteral("anchor")));
    QCOMPARE(tombstone.value(QStringLiteral("comment")).toString(), QStringLiteral("Nice."));
    QCOMPARE(ReviewCardFeedback::validationError(tombstone), QString());

    // Both in one store write: one batch, consecutive device sequences.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    DesktopSyncOutbox outbox(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")), QDir(temporary.path()).filePath(QStringLiteral("outbox.json")));
    QVERIFY2(ReviewCardFeedback::enqueueAll(outbox, {created, tombstone}, &error), qPrintable(error));
    QList<QJsonObject> operations = outbox.operations(&error);
    QCOMPARE(operations.size(), 2);
    QCOMPARE(operations.at(0).value(QStringLiteral("payload")).toObject(), created);
    QCOMPARE(operations.at(0).value(QStringLiteral("base_revision")).toInteger(), 0);
    QCOMPARE(operations.at(1).value(QStringLiteral("payload")).toObject(), tombstone);
    QCOMPARE(operations.at(1).value(QStringLiteral("base_revision")).toInteger(), 2);
    QCOMPARE(operations.at(1).value(QStringLiteral("device_sequence")).toInteger(), operations.at(0).value(QStringLiteral("device_sequence")).toInteger() + 1);
    for (const QJsonObject &operation : std::as_const(operations)) {
        QCOMPARE(operation.value(QStringLiteral("entity_type")).toString(), ReviewCardFeedback::entityType());
        QCOMPARE(operation.value(QStringLiteral("collection")).toString(), ReviewCardFeedback::collection());
    }
    // All or none: an invalid record writes neither.
    QJsonObject broken = created;
    broken.remove(QStringLiteral("card_id"));
    QVERIFY(!ReviewCardFeedback::enqueueAll(outbox, {tombstone, broken}, &error));
    QCOMPARE(outbox.operations().size(), 2);

    // Undo (nothing synced yet): the new record deleted, the old one back, in one write.
    const QList<QJsonObject> undo = ReviewCardFeedback::undoReplacement(*replacement, created, tombstone, origin, now.addSecs(2));
    QCOMPARE(undo.size(), 2);
    QCOMPARE(undo.at(0).value(QStringLiteral("feedback_id")), created.value(QStringLiteral("feedback_id")));
    QCOMPARE(undo.at(0).value(QStringLiteral("revision")).toInt(), 2);
    QVERIFY(undo.at(0).value(QStringLiteral("deleted")).toBool());
    QCOMPARE(undo.at(1).value(QStringLiteral("feedback_id")), old.value(QStringLiteral("feedback_id")));
    QCOMPARE(undo.at(1).value(QStringLiteral("revision")).toInt(), 4);
    QVERIFY(!undo.at(1).value(QStringLiteral("deleted")).toBool());
    QVERIFY(!undo.at(1).contains(QStringLiteral("deleted_at")));
    QCOMPARE(undo.at(1).value(QStringLiteral("anchor")), old.value(QStringLiteral("anchor")));
    QVERIFY2(ReviewCardFeedback::enqueueAll(outbox, undo, &error), qPrintable(error));
    QCOMPARE(outbox.operations().size(), 4);
    // Without the stored records the replacement's own are used.
    QCOMPARE(ReviewCardFeedback::undoReplacement(*replacement, {}, {}, origin, now.addSecs(2)).size(), 2);
}

void ReviewCardFeedbackTest::testChangePassageUndoAfterSync()
{
    const QString pdf = QString(64, QLatin1Char('f'));
    const QJsonObject old = ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QString(), passageAnchor(QStringLiteral("old"), pdf, 0.2),
                                                       origin, at(QStringLiteral("2026-10-20T10:00:00.000Z")));
    const auto replacement = ReviewCardFeedback::replacePassage(old, ReviewCardFeedback::positive(), QString(), passageAnchor(QStringLiteral("new"), pdf, 0.4), origin,
                                                                at(QStringLiteral("2026-10-21T09:00:00.000Z")));
    QVERIFY(replacement);
    // Sync settled both writes: the projections hold the server's copies (same revisions).
    const QJsonObject createdSynced = ReviewCardFeedback::merge({}, replacement->created);
    const QJsonObject oldSynced = ReviewCardFeedback::merge(old, replacement->tombstone);
    QVERIFY(oldSynced.value(QStringLiteral("deleted")).toBool());
    QList<QJsonObject> undo = ReviewCardFeedback::undoReplacement(*replacement, createdSynced, oldSynced, origin, at(QStringLiteral("2026-10-21T09:00:03.000Z")));
    QCOMPARE(undo.size(), 2);
    QCOMPARE(undo.at(0).value(QStringLiteral("revision")).toInt(), 2);
    QVERIFY(undo.at(0).value(QStringLiteral("deleted")).toBool());
    QCOMPARE(undo.at(1).value(QStringLiteral("revision")).toInt(), 3);
    QVERIFY(!undo.at(1).value(QStringLiteral("deleted")).toBool());
    for (const QJsonObject &record : std::as_const(undo)) QCOMPARE(ReviewCardFeedback::validationError(record), QString());
    // The new record was edited elsewhere meanwhile (revision 2): its delete follows that revision.
    const QJsonObject createdEdited = ReviewCardFeedback::edit(createdSynced, ReviewCardFeedback::negative(), QStringLiteral("Phone"), QStringLiteral("android"),
                                                               at(QStringLiteral("2026-10-21T09:00:01.000Z")));
    undo = ReviewCardFeedback::undoReplacement(*replacement, createdEdited, oldSynced, origin, at(QStringLiteral("2026-10-21T09:00:03.000Z")));
    QCOMPARE(undo.at(0).value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(undo.at(0).value(QStringLiteral("comment")).toString(), QStringLiteral("Phone"));
    QVERIFY(ReviewCardFeedback::applies(undo.at(0), createdEdited));
    // Already undone elsewhere (new record deleted, old one restored): nothing to write.
    const QJsonObject createdDeleted = ReviewCardFeedback::remove(createdSynced, QStringLiteral("android"), at(QStringLiteral("2026-10-21T09:00:02.000Z")));
    const QJsonObject oldRestored = ReviewCardFeedback::undo(oldSynced, QStringLiteral("android"), at(QStringLiteral("2026-10-21T09:00:02.000Z")));
    QVERIFY(ReviewCardFeedback::undoReplacement(*replacement, createdDeleted, oldRestored, origin, at(QStringLiteral("2026-10-21T09:00:03.000Z"))).isEmpty());
}

void ReviewCardFeedbackTest::testChangePassageRefusals()
{
    const QString pdf = QString(64, QLatin1Char('f'));
    const QJsonObject anchor = passageAnchor(QStringLiteral("new"), pdf, 0.4);
    const QJsonObject old = ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QString(), passageAnchor(QStringLiteral("old"), pdf, 0.2),
                                                       origin, at(QStringLiteral("2026-10-20T10:00:00.000Z")));
    QString error;
    // Missing.
    QVERIFY(!ReviewCardFeedback::replacePassage({}, ReviewCardFeedback::positive(), QString(), anchor, origin, QDateTime::currentDateTimeUtc(), &error));
    QVERIFY(!error.isEmpty());
    // Deleted.
    error.clear();
    const QJsonObject deleted = ReviewCardFeedback::remove(old, origin, at(QStringLiteral("2026-10-20T11:00:00.000Z")));
    QVERIFY(!ReviewCardFeedback::replacePassage(deleted, ReviewCardFeedback::positive(), QString(), anchor, origin, QDateTime::currentDateTimeUtc(), &error));
    QVERIFY(error.contains(QStringLiteral("deleted")));
    // Invalid anchors: no quote, no regions, a bad PDF hash.
    const QList<QJsonObject> invalid{
        ReviewCardFeedback::anchor(QString(), QString(), QString(), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), pdf,
                                   ReviewCardFeedback::regionsJson({{2, QRectF(0.1, 0.2, 0.3, 0.05)}})),
        ReviewCardFeedback::anchor(QStringLiteral("new"), QString(), QString(), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), pdf, QJsonArray()),
        ReviewCardFeedback::anchor(QStringLiteral("new"), QString(), QString(), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), QStringLiteral("nothex"),
                                   ReviewCardFeedback::regionsJson({{2, QRectF(0.1, 0.2, 0.3, 0.05)}})),
        QJsonObject(),
    };
    for (const QJsonObject &bad : invalid) {
        error.clear();
        QVERIFY(!ReviewCardFeedback::replacePassage(old, ReviewCardFeedback::positive(), QString(), bad, origin, QDateTime::currentDateTimeUtc(), &error));
        QVERIFY(!error.isEmpty());
    }
    // A record without a card cannot be card feedback.
    QJsonObject noCard = old;
    noCard.remove(QStringLiteral("card_id"));
    QVERIFY(!ReviewCardFeedback::replacePassage(noCard, ReviewCardFeedback::positive(), QString(), anchor, origin, QDateTime::currentDateTimeUtc(), &error));
}

#include "reviewcardfeedbacktest.moc"
