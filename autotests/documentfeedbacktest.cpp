// SPDX-License-Identifier: GPL-2.0-or-later
// Document feedback records, merging and finding the passage again
// (thoughts-server protocol/document-feedback.md, extending review-card-feedback.md).
#include "../part/documentfeedback.h"
#include "../part/desktopsyncstore.h"

#include <QDir>
#include <QTemporaryDir>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QUuid>

#include <functional>

using namespace Thoughts;
using ReviewCardFeedback::AnswerText;
using ReviewCardFeedback::Glyph;

class DocumentFeedbackTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testSpecExampleRoundTripsWithUnknownFields();
    void testValidationMatchesTheServer();
    void testMergeFollowsTheServerTable_data();
    void testMergeFollowsTheServerTable();
    void testDeleteIsStickyThenUndoRevives();
    void testImmutableFieldsComeFromTheFirstWrite();
    void testCreateEditDeleteUndo();
    void testRecordsOfDocument();
    void testSearchPages();
    void testPlacement();
    void testSelectionContext();
    void testDrawing();
    void testDrawingColours();
    void testTextBox();
    void testChangePassage();
};

namespace
{
const QString documentId = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848");
const QString pdfSha = QStringLiteral("f4c8827551095a63ce58c6b1e2c5946014b7b3f62d0f96a780a2931f601a561e");

QJsonObject specExample()
{
    // §1, plus fields this client does not know at every level.
    return QJsonDocument::fromJson(R"json({
      "schema_version": 1,
      "feedback_id": "9d2f4a51-7c0e-4b8a-a6f1-3e5b2c7d8e90",
      "polarity": "positive",
      "comment": "",
      "anchor": {
        "version": 1,
        "quote": {
          "exact": "The picture draws each route in the plane of the two weights",
          "prefix": "",
          "suffix": ": the horizontal position is a(t)"
        },
        "pdf": {
          "document_id": "c1e62ecc-2dfb-4259-97cc-e319b1186848",
          "pdf_sha256": "f4c8827551095a63ce58c6b1e2c5946014b7b3f62d0f96a780a2931f601a561e",
          "regions": [
            {"page": 1, "rect": [0.0330, 0.5090, 0.8930, 0.5200], "future_region_field": 3}
          ]
        },
        "future_anchor_field": {"x": 1}
      },
      "revision": 1,
      "created_at": "2026-10-28T08:52:03.512Z",
      "updated_at": "2026-10-28T08:52:03.512Z",
      "created_origin_id": "android-cph2719",
      "origin_id": "android-cph2719",
      "deleted": false,
      "future_field": [1, 2]
    })json").object();
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

// One glyph per character of @p word on @p page, left to right from @p left on the line at @p top,
// then a space.
QList<Glyph> line(int page, const QString &words, double left, double top)
{
    QList<Glyph> glyphs;
    double x = left;
    for (const QChar character : words) {
        glyphs.append(Glyph{page, QString(character), QRectF(x, top, 0.01, 0.02)});
        x += 0.01;
    }
    glyphs.append(Glyph{page, QStringLiteral("\n"), QRectF()});
    return glyphs;
}

QJsonObject recordOn(const QString &quote, const QString &sha, int page, const QString &prefix = {}, const QString &suffix = {})
{
    const QJsonObject anchor = ReviewCardFeedback::anchor(quote, prefix, suffix, documentId, sha,
                                                          ReviewCardFeedback::regionsJson({{page, QRectF(QPointF(0.1, 0.1), QPointF(0.4, 0.12))}}));
    return DocumentFeedback::create(ReviewCardFeedback::positive(), QString(), anchor, QStringLiteral("mac-okular"),
                                    QDateTime::fromString(QStringLiteral("2026-10-28T09:00:00.000Z"), Qt::ISODateWithMs));
}
}

void DocumentFeedbackTest::testSpecExampleRoundTripsWithUnknownFields()
{
    const QJsonObject record = specExample();
    QCOMPARE(DocumentFeedback::validationError(record), QString());
    QCOMPARE(DocumentFeedback::entityType(), QStringLiteral("document_feedback"));
    QCOMPARE(DocumentFeedback::collection(), QStringLiteral("document-feedback"));
    QCOMPARE(DocumentFeedback::capability(), QStringLiteral("document_feedback_v1"));
    QCOMPARE(DocumentFeedback::projectionKey(QStringLiteral("x")), QStringLiteral("document_feedback\ndocument-feedback\nx"));
    const QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    QCOMPARE(QJsonDocument::fromJson(bytes).toJson(QJsonDocument::Compact), bytes);
    // A rewrite keeps every field it does not change, unknown ones included, and never adds a card.
    const QJsonObject edited = DocumentFeedback::edit(record, ReviewCardFeedback::negative(), QStringLiteral("  Unclear.  "), QStringLiteral("mac-okular"),
                                                      QDateTime::fromString(QStringLiteral("2026-10-28T09:00:00.000Z"), Qt::ISODateWithMs));
    QCOMPARE(edited.value(QStringLiteral("future_field")), record.value(QStringLiteral("future_field")));
    QCOMPARE(edited.value(QStringLiteral("anchor")), record.value(QStringLiteral("anchor")));
    QCOMPARE(edited.value(QStringLiteral("comment")).toString(), QStringLiteral("Unclear."));
    QCOMPARE(edited.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(edited.value(QStringLiteral("created_origin_id")).toString(), QStringLiteral("android-cph2719"));
    QCOMPARE(edited.value(QStringLiteral("origin_id")).toString(), QStringLiteral("mac-okular"));
    QVERIFY(!edited.contains(QStringLiteral("card_id")));
    QCOMPARE(DocumentFeedback::validationError(edited), QString());
    QCOMPARE(DocumentFeedback::documentIdOf(record), documentId);
}

void DocumentFeedbackTest::testValidationMatchesTheServer()
{
    // Server conformance test 5: each of these is refused.
    const QJsonObject valid = specExample();
    const auto broken = [&](const std::function<void(QJsonObject &)> &change) {
        QJsonObject record = valid;
        change(record);
        return !DocumentFeedback::validationError(record).isEmpty();
    };
    const auto withPdf = [](QJsonObject &record, const std::function<void(QJsonObject &)> &change) {
        QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
        QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
        change(pdf);
        anchor.insert(QStringLiteral("pdf"), pdf);
        record.insert(QStringLiteral("anchor"), anchor);
    };
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("card_id"), QStringLiteral("0072b138-d3fe-416a-83e7-1407058f0e2b")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("card_id"), QString()); }));
    QVERIFY(broken([](QJsonObject &r) {
        QJsonObject anchor = r.value(QStringLiteral("anchor")).toObject();
        anchor.remove(QStringLiteral("pdf"));
        r.insert(QStringLiteral("anchor"), anchor);
    }));
    QVERIFY(broken([&](QJsonObject &r) { withPdf(r, [](QJsonObject &pdf) { pdf.insert(QStringLiteral("regions"), QJsonArray{}); }); }));
    QVERIFY(broken([&](QJsonObject &r) {
        withPdf(r, [](QJsonObject &pdf) {
            QJsonArray many;
            for (int i = 0; i < 201; ++i) many.append(QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QJsonArray{0.1, 0.1, 0.2, 0.2}}});
            pdf.insert(QStringLiteral("regions"), many);
        });
    }));
    QVERIFY(broken([&](QJsonObject &r) { withPdf(r, [](QJsonObject &pdf) { pdf.insert(QStringLiteral("document_id"), QStringLiteral("not-a-uuid")); }); }));
    QVERIFY(broken([](QJsonObject &r) {
        r.insert(QStringLiteral("view"), QStringLiteral("html"));
        r.insert(QStringLiteral("html_edition_sha256"), QString(64, QLatin1Char('a')));
    }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("polarity"), QStringLiteral("neutral")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("created_at"), QStringLiteral("2026-10-28T08:52:03")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("updated_at"), QStringLiteral("yesterday")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("deleted"), true); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("deleted_at"), QStringLiteral("2026-10-28T08:52:03Z")); }));
    QVERIFY(broken([](QJsonObject &r) { r.insert(QStringLiteral("feedback_id"), QStringLiteral("9D2F4A51-7C0E-4B8A-A6F1-3E5B2C7D8E90")); }));
    // view "pdf" and the informational segment fields are accepted.
    QVERIFY(!broken([](QJsonObject &r) { r.insert(QStringLiteral("view"), QStringLiteral("pdf")); }));
    QVERIFY(!broken([](QJsonObject &r) {
        QJsonObject anchor = r.value(QStringLiteral("anchor")).toObject();
        anchor.insert(QStringLiteral("segment_id"), QStringLiteral("3"));
        anchor.insert(QStringLiteral("segment_content_sha256"), QString(64, QLatin1Char('b')));
        r.insert(QStringLiteral("anchor"), anchor);
    }));
    QVERIFY(!broken([](QJsonObject &r) {
        r.insert(QStringLiteral("review_version"), QJsonObject{{QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("source_built"), false}});
    }));
}

void DocumentFeedbackTest::testMergeFollowsTheServerTable_data()
{
    QTest::addColumn<QJsonObject>("stored");
    QTest::addColumn<QJsonObject>("incoming");
    QTest::addColumn<bool>("applies");
    const QJsonObject base = specExample();
    const QJsonObject a2 = withWrite(base, 2, QStringLiteral("2026-10-28T11:00:00.000Z"), QStringLiteral("mac-okular"));
    const QJsonObject b2 = withWrite(base, 2, QStringLiteral("2026-10-28T11:30:00.000Z"), QStringLiteral("android-cph2719"));
    const QJsonObject deleted2 = withWrite(base, 2, QStringLiteral("2026-10-28T11:00:00.000Z"), QStringLiteral("mac-okular"), true);
    const QJsonObject editLater2 = withWrite(base, 2, QStringLiteral("2026-10-28T12:00:00.000Z"), QStringLiteral("android-cph2719"));
    const QJsonObject editEarlier2 = withWrite(base, 2, QStringLiteral("2026-10-28T10:00:00.000Z"), QStringLiteral("android-cph2719"));
    const QJsonObject undo3 = withWrite(base, 3, QStringLiteral("2026-10-28T11:00:05.000Z"), QStringLiteral("mac-okular"));
    const QJsonObject edit3 = withWrite(base, 3, QStringLiteral("2026-10-28T10:59:00.000Z"), QStringLiteral("mac-okular"));
    // Server conformance tests 2 and 3, in either arrival order.
    QTest::newRow("higher revision wins") << base << a2 << true;
    QTest::newRow("later updated_at wins at equal revision") << a2 << b2 << true;
    QTest::newRow("earlier updated_at loses at equal revision") << b2 << a2 << false;
    QTest::newRow("a stale revision is superseded") << edit3 << b2 << false;
    QTest::newRow("delete arrives after a later edit") << editLater2 << deleted2 << true;
    QTest::newRow("delete arrives after an earlier edit") << editEarlier2 << deleted2 << true;
    QTest::newRow("later edit arrives after the delete") << deleted2 << editLater2 << false;
    QTest::newRow("earlier edit arrives after the delete") << deleted2 << editEarlier2 << false;
    QTest::newRow("undo restores") << deleted2 << undo3 << true;
    QTest::newRow("a new record applies") << QJsonObject() << base << true;
}

void DocumentFeedbackTest::testMergeFollowsTheServerTable()
{
    QFETCH(QJsonObject, stored);
    QFETCH(QJsonObject, incoming);
    QFETCH(bool, applies);
    QCOMPARE(DocumentFeedback::applies(incoming, stored), applies);
    const QJsonObject merged = DocumentFeedback::merge(stored, incoming);
    QCOMPARE(merged.value(QStringLiteral("revision")), (applies ? incoming : stored).value(QStringLiteral("revision")));
    QCOMPARE(merged.value(QStringLiteral("deleted")), (applies ? incoming : stored).value(QStringLiteral("deleted")));
}

void DocumentFeedbackTest::testDeleteIsStickyThenUndoRevives()
{
    const QJsonObject base = specExample();
    const QJsonObject deleted = withWrite(base, 2, QStringLiteral("2026-10-28T11:00:00.000Z"), QStringLiteral("mac-okular"), true);
    for (const QString &time : {QStringLiteral("2026-10-28T10:00:00.000Z"), QStringLiteral("2026-10-28T11:00:00.000Z"), QStringLiteral("2026-10-28T12:00:00.000Z")}) {
        const QJsonObject edit = withWrite(base, 2, time, QStringLiteral("android-cph2719"));
        for (const bool deleteFirst : {true, false}) {
            const QJsonObject result = deleteFirst ? DocumentFeedback::merge(DocumentFeedback::merge(base, deleted), edit)
                                                   : DocumentFeedback::merge(DocumentFeedback::merge(base, edit), deleted);
            QVERIFY2(result.value(QStringLiteral("deleted")).toBool(), qPrintable(time));
            const QJsonObject restored = DocumentFeedback::merge(result, DocumentFeedback::undo(result, QStringLiteral("android-cph2719")));
            QVERIFY(!restored.value(QStringLiteral("deleted")).toBool());
            QVERIFY(!restored.contains(QStringLiteral("deleted_at")));
            QCOMPARE(restored.value(QStringLiteral("revision")).toInt(), 3);
        }
    }
}

void DocumentFeedbackTest::testImmutableFieldsComeFromTheFirstWrite()
{
    // Server conformance test 4 (and view, fixed by the first write).
    const QJsonObject first = specExample();
    QJsonObject later = withWrite(first, 2, QStringLiteral("2026-10-28T11:00:00.000Z"), QStringLiteral("mac-okular"));
    later.insert(QStringLiteral("created_at"), QStringLiteral("2026-10-29T00:00:00.000Z"));
    later.insert(QStringLiteral("created_origin_id"), QStringLiteral("mac-okular"));
    later.insert(QStringLiteral("anchor"), QJsonObject{{QStringLiteral("version"), 1}});
    later.insert(QStringLiteral("view"), QStringLiteral("pdf"));
    later.insert(QStringLiteral("comment"), QStringLiteral("changed"));
    const QJsonObject merged = DocumentFeedback::merge(first, later);
    QCOMPARE(merged.value(QStringLiteral("comment")).toString(), QStringLiteral("changed"));
    for (const QString &key : {QStringLiteral("anchor"), QStringLiteral("created_at"), QStringLiteral("created_origin_id")}) QCOMPARE(merged.value(key), first.value(key));
    // The first write had no view: it stays without one.
    QVERIFY(!merged.contains(QStringLiteral("view")));
    QJsonObject withView = first;
    withView.insert(QStringLiteral("view"), QStringLiteral("pdf"));
    later.remove(QStringLiteral("view"));
    QCOMPARE(DocumentFeedback::merge(withView, later).value(QStringLiteral("view")).toString(), QStringLiteral("pdf"));
}

void DocumentFeedbackTest::testCreateEditDeleteUndo()
{
    const QJsonObject anchor = ReviewCardFeedback::anchor(QStringLiteral("  the passage \n"), QStringLiteral("Before "), QStringLiteral(" after"), documentId, pdfSha,
                                                          ReviewCardFeedback::regionsJson({{2, QRectF(QPointF(0.1, 0.2), QPointF(0.5, 0.25))}}));
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-10-28T10:00:00.123Z"), Qt::ISODateWithMs);
    // Like: a positive record at once, without a comment and without a card.
    const QJsonObject liked = DocumentFeedback::create(ReviewCardFeedback::positive(), QString(), anchor, QStringLiteral("mac-okular"), now);
    QCOMPARE(DocumentFeedback::validationError(liked), QString());
    QVERIFY(!liked.contains(QStringLiteral("card_id")));
    QCOMPARE(liked.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(liked.value(QStringLiteral("polarity")).toString(), QStringLiteral("positive"));
    QCOMPARE(liked.value(QStringLiteral("comment")).toString(), QString());
    QCOMPARE(liked.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(liked.value(QStringLiteral("deleted")).toBool(), false);
    QCOMPARE(liked.value(QStringLiteral("created_at")).toString(), QStringLiteral("2026-10-28T10:00:00.123Z"));
    QCOMPARE(liked.value(QStringLiteral("updated_at")), liked.value(QStringLiteral("created_at")));
    QCOMPARE(liked.value(QStringLiteral("created_origin_id")).toString(), QStringLiteral("mac-okular"));
    QCOMPARE(liked.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), QStringLiteral("the passage"));
    const QString id = liked.value(QStringLiteral("feedback_id")).toString();
    QCOMPARE(QUuid(id).version(), QUuid::Random);
    QCOMPARE(id, id.toLower());
    // An edit: revision + 1, created_at kept.
    const QJsonObject edited = DocumentFeedback::edit(liked, ReviewCardFeedback::negative(), QStringLiteral("Too vague."), QStringLiteral("mac-okular"), now.addSecs(-60));
    QCOMPARE(edited.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(edited.value(QStringLiteral("created_at")), liked.value(QStringLiteral("created_at")));
    QVERIFY(edited.value(QStringLiteral("updated_at")).toString() > liked.value(QStringLiteral("updated_at")).toString());
    // Delete and Undo: revision n + 2, not deleted, no deleted_at.
    const QJsonObject tombstone = DocumentFeedback::remove(edited, QStringLiteral("mac-okular"), now.addSecs(10));
    QCOMPARE(tombstone.value(QStringLiteral("deleted")).toBool(), true);
    QCOMPARE(tombstone.value(QStringLiteral("deleted_at")), tombstone.value(QStringLiteral("updated_at")));
    QCOMPARE(DocumentFeedback::validationError(tombstone), QString());
    const QJsonObject restored = DocumentFeedback::undo(tombstone, QStringLiteral("mac-okular"), now.addSecs(11));
    QCOMPARE(restored.value(QStringLiteral("revision")).toInt(), edited.value(QStringLiteral("revision")).toInt() + 2);
    QCOMPARE(restored.value(QStringLiteral("deleted")).toBool(), false);
    QVERIFY(!restored.contains(QStringLiteral("deleted_at")));
    QVERIFY(DocumentFeedback::applies(restored, tombstone));
    QCOMPARE(DocumentFeedback::validationError(restored), QString());
}

void DocumentFeedbackTest::testRecordsOfDocument()
{
    QJsonObject projections;
    const QJsonObject first = recordOn(QStringLiteral("first"), pdfSha, 1);
    QJsonObject earlier = recordOn(QStringLiteral("earlier"), pdfSha, 2);
    earlier.insert(QStringLiteral("created_at"), QStringLiteral("2026-10-27T09:00:00.000Z"));
    const QJsonObject deleted = DocumentFeedback::remove(recordOn(QStringLiteral("gone"), pdfSha, 1), QStringLiteral("phone"));
    QJsonObject otherDocument = recordOn(QStringLiteral("other"), pdfSha, 1);
    QJsonObject anchor = otherDocument.value(QStringLiteral("anchor")).toObject();
    QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
    pdf.insert(QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    anchor.insert(QStringLiteral("pdf"), pdf);
    otherDocument.insert(QStringLiteral("anchor"), anchor);
    for (const QJsonObject &record : {first, earlier, deleted, otherDocument})
        projections.insert(DocumentFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
    // Card feedback on the same document is never document feedback.
    const QJsonObject card = ReviewCardFeedback::create(QStringLiteral("card-1"), ReviewCardFeedback::positive(), QString(), first.value(QStringLiteral("anchor")).toObject(),
                                                        QStringLiteral("mac-okular"));
    projections.insert(ReviewCardFeedback::projectionKey(card.value(QStringLiteral("feedback_id")).toString()), card);
    QCOMPARE(DocumentFeedback::recordsOfDocument(projections, documentId), (QList<QJsonObject>{earlier, first}));
    QCOMPARE(DocumentFeedback::recordsOfDocument(projections, documentId.toUpper()).size(), 2);
    QCOMPARE(DocumentFeedback::recordsOfDocument(projections, documentId, true).size(), 3);
    QVERIFY(DocumentFeedback::recordsOfDocument(projections, QString()).isEmpty());
}

void DocumentFeedbackTest::testSearchPages()
{
    QCOMPARE(DocumentFeedback::searchPages({5}, 20, false), QList<int>({5}));
    QCOMPARE(DocumentFeedback::searchPages({5}, 20, true), QList<int>({2, 3, 4, 5, 6, 7, 8}));
    QCOMPARE(DocumentFeedback::searchPages({2, 1}, 4, true), QList<int>({1, 2, 3, 4}));
    QCOMPARE(DocumentFeedback::searchPages({9}, 4, false), QList<int>());
    QCOMPARE(DocumentFeedback::searchPages({9}, 7, true), QList<int>({6, 7}));
}

void DocumentFeedbackTest::testPlacement()
{
    // A displayed PDF of 10 pages: page n reads "page n words", page 7 also "the moved passage".
    QHash<int, QList<Glyph>> pages;
    for (int page = 1; page <= 10; ++page) pages.insert(page, line(page, QStringLiteral("page %1 words").arg(page), 0.1, 0.1));
    pages[7].append(line(7, QStringLiteral("the moved passage"), 0.2, 0.5));
    pages[2].append(line(2, QStringLiteral("hyph-enated text"), 0.2, 0.5));
    QList<QList<int>> asked;
    const auto pageText = [&](const QList<int> &wanted) {
        asked.append(wanted);
        QList<QList<Glyph>> regions;
        for (const int page : wanted) regions.append(pages.value(page));
        return ReviewCardFeedback::answerText(regions);
    };
    const QString otherSha = QString(64, QLatin1Char('d'));

    // (1) Same PDF: the regions as they are, no text read.
    const QJsonObject same = recordOn(QStringLiteral("anything"), pdfSha, 4);
    auto placement = DocumentFeedback::place(same, pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("exact"));
    QCOMPARE(placement.rects, ReviewCardFeedback::regionsOf(same));
    QVERIFY(asked.isEmpty());

    // (2) Another PDF, the quote on its anchor page: relocated over its own glyphs.
    placement = DocumentFeedback::place(recordOn(QStringLiteral("page 4 words"), otherSha, 4), pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("relocated"));
    QCOMPARE(asked, QList<QList<int>>({{4}}));
    QCOMPARE(placement.rects.size(), 1);
    QCOMPARE(placement.rects.first().first, 4);
    QVERIFY(qAbs(placement.rects.first().second.left() - 0.1) < 1e-9);
    QVERIFY(qAbs(placement.rects.first().second.right() - 0.22) < 1e-9);

    // (3) Moved to a page within 3 of the anchor page: found on the widened search.
    asked.clear();
    placement = DocumentFeedback::place(recordOn(QStringLiteral("the moved passage"), otherSha, 5), pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("relocated"));
    QCOMPARE(asked, QList<QList<int>>({{5}, {2, 3, 4, 5, 6, 7, 8}}));
    QCOMPARE(placement.rects.size(), 1);
    QCOMPARE(placement.rects.first().first, 7);
    QVERIFY(qAbs(placement.rects.first().second.left() - 0.2) < 1e-9);

    // (4) Further away than 3 pages: orphaned (kept, not drawn).
    asked.clear();
    placement = DocumentFeedback::place(recordOn(QStringLiteral("the moved passage"), otherSha, 2), pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("orphaned"));
    QVERIFY(placement.rects.isEmpty());
    QCOMPARE(asked, QList<QList<int>>({{2}, {1, 2, 3, 4, 5}}));

    // (5) Whitespace and the hyphen retry, as for card feedback.
    placement = DocumentFeedback::place(recordOn(QStringLiteral("hyphenated  text"), otherSha, 2), pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("relocated"));
    QCOMPARE(placement.rects.first().first, 2);

    // (6) Prefix and suffix pick between two hits.
    pages[3] = line(3, QStringLiteral("alpha beta gamma beta delta"), 0.1, 0.3);
    placement = DocumentFeedback::place(recordOn(QStringLiteral("beta"), otherSha, 3, QStringLiteral("gamma "), QStringLiteral(" delta")), pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("relocated"));
    QVERIFY(qAbs(placement.rects.first().second.left() - 0.27) < 1e-9);

    // (7) An edited quote is orphaned; so is one whose pages are not in this PDF.
    QCOMPARE(DocumentFeedback::place(recordOn(QStringLiteral("not in the PDF"), otherSha, 4), pdfSha, 10, pageText).state, QStringLiteral("orphaned"));
    QCOMPARE(DocumentFeedback::place(recordOn(QStringLiteral("page 4 words"), otherSha, 40), pdfSha, 10, pageText).state, QStringLiteral("orphaned"));
}

void DocumentFeedbackTest::testSelectionContext()
{
    const AnswerText text = ReviewCardFeedback::answerText({line(1, QStringLiteral("This is some random text put here because it is"), 0.1, 0.1)});
    // "some random text" is selected: glyphs 8..23.
    const auto selected = [](int page, const QPointF &centre) { return page == 1 && centre.x() > 0.18 && centre.x() < 0.34; };
    const auto [prefix, suffix] = DocumentFeedback::context(text, selected);
    QCOMPARE(prefix, QStringLiteral("This is "));
    QCOMPARE(suffix, QStringLiteral(" put here because it is\n"));
    QCOMPARE(DocumentFeedback::context(text, [](int, const QPointF &) { return false; }), (QPair<QString, QString>()));
}

void DocumentFeedbackTest::testDrawing()
{
    const QList<DocumentFeedback::Stroke> first{{{QPointF(0.123456, 0.2), QPointF(0.3, 0.4)}, QString()}};
    const QJsonObject record = DocumentFeedback::createDrawing(2, first, documentId, pdfSha, QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(record), QString());
    QVERIFY(DocumentFeedback::isDrawing(record));
    QVERIFY(!DocumentFeedback::canResolve(record));
    QVERIFY(DocumentFeedback::canResolve(DocumentFeedback::withKind(record, DocumentFeedback::question())));
    QCOMPARE(record.value(QStringLiteral("polarity")).toString(), ReviewCardFeedback::neutral());
    // Rounded to four decimals.
    QCOMPARE(DocumentFeedback::strokesOf(record).constFirst().points.constFirst(), QPointF(0.1235, 0.2));

    // It grows without touching what the server keeps as first written.
    const QList<DocumentFeedback::Stroke> both = first + QList<DocumentFeedback::Stroke>{{{QPointF(0.9, 0.9)}, QString()}};
    const QJsonObject next = DocumentFeedback::withStrokes(record, both, QStringLiteral("mac"));
    QCOMPARE(next.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(next.value(QStringLiteral("anchor")), record.value(QStringLiteral("anchor")));
    QCOMPARE(DocumentFeedback::strokesOf(next).size(), 2);
    QCOMPARE(DocumentFeedback::validationError(next), QString());
    const QJsonObject merged = DocumentFeedback::merge(record, next);
    QCOMPARE(DocumentFeedback::strokesOf(merged).size(), 2);
    // A later edit of its comment keeps the strokes.
    const QJsonObject commented = DocumentFeedback::edit(next, ReviewCardFeedback::neutral(), QStringLiteral("See this"), QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::strokesOf(commented).size(), 2);
    QCOMPARE(DocumentFeedback::validationError(commented), QString());

    // On the PDF it was drawn on, where its place is its strokes, and nowhere else.
    const QString other(64, QLatin1Char('e'));
    QVERIFY(DocumentFeedback::shownOn(next, pdfSha));
    QVERIFY(!DocumentFeedback::shownOn(next, other));
    const auto placed = DocumentFeedback::place(next, pdfSha, 3, {});
    QCOMPARE(placed.state, QStringLiteral("exact"));
    QCOMPARE(placed.rects.size(), 1);
    QCOMPARE(placed.rects.constFirst().first, 2);
    QVERIFY(placed.rects.constFirst().second.contains(QPointF(0.5, 0.5)));
    QVERIFY(placed.rects.constFirst().second != QRectF(0.0, 0.0, 1.0, 1.0));
    // "Drawing" could be anywhere in the text of another version; it is never looked for.
    const auto text = [](const QList<int> &) { return AnswerText{QStringLiteral("A Drawing here"), {}}; };
    QCOMPARE(DocumentFeedback::place(next, other, 3, text).state, QStringLiteral("orphaned"));

    // What the server would refuse is never queued.
    const auto changed = [&record](const QJsonValue &drawing) {
        QJsonObject result = record;
        result.insert(QStringLiteral("drawing"), drawing);
        return DocumentFeedback::validationError(result);
    };
    const auto strokes = [](const QJsonArray &points) {
        return QJsonObject{{QStringLiteral("strokes"), QJsonArray{QJsonObject{{QStringLiteral("points"), points}}}}};
    };
    QVERIFY(changed(QJsonObject{}).contains(QStringLiteral("drawing")));
    QVERIFY(!changed(QJsonObject{{QStringLiteral("strokes"), QJsonArray{}}}).isEmpty());
    QVERIFY(!changed(strokes({})).isEmpty());
    QVERIFY(!changed(strokes({QJsonArray{0.5}})).isEmpty());
    QVERIFY(!changed(strokes({QJsonArray{1.5, 0.5}})).isEmpty());
    QVERIFY(!changed(strokes({QJsonArray{0.5, -0.1}})).isEmpty());
    QJsonObject withoutStrokes = record;
    withoutStrokes.remove(QStringLiteral("drawing"));
    QVERIFY(!DocumentFeedback::validationError(withoutStrokes).isEmpty());
    QJsonObject likeWithStrokes = DocumentFeedback::withKind(record, QString());
    likeWithStrokes.insert(QStringLiteral("polarity"), ReviewCardFeedback::positive());
    QVERIFY(!DocumentFeedback::validationError(likeWithStrokes).isEmpty());
    QJsonObject resolved = record;
    resolved.insert(QStringLiteral("resolved_at"), QStringLiteral("2026-11-05T10:00:00.000Z"));
    QVERIFY(!DocumentFeedback::validationError(resolved).isEmpty());
}

void DocumentFeedbackTest::testDrawingColours()
{
    // A stroke keeps the pencil's colour ("color", "#rrggbb"); the pencil's own blue keeps none, as
    // a drawing made before colours did.
    const QList<DocumentFeedback::Stroke> drawn{{{QPointF(0.1, 0.1), QPointF(0.2, 0.1)}, QStringLiteral("#d32f2f")}, {{QPointF(0.5, 0.5)}, QString()}};
    const QJsonObject record = DocumentFeedback::createDrawing(1, drawn, documentId, pdfSha, QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(record), QString());
    const QJsonArray json = record.value(QStringLiteral("drawing")).toObject().value(QStringLiteral("strokes")).toArray();
    QCOMPARE(json.at(0).toObject().value(QStringLiteral("color")).toString(), QStringLiteral("#d32f2f"));
    QVERIFY(!json.at(1).toObject().contains(QStringLiteral("color")));
    QList<DocumentFeedback::Stroke> read = DocumentFeedback::strokesOf(record);
    QCOMPARE(read.at(0).colour, QStringLiteral("#d32f2f"));
    QVERIFY(read.at(1).colour.isEmpty());

    // A later stroke and an erased one leave the colours of the others as they were.
    read.append({{QPointF(0.9, 0.9)}, QStringLiteral("#2e7d32")});
    read.removeAt(1);
    const QJsonObject next = DocumentFeedback::withStrokes(record, read, QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(next), QString());
    const auto kept = DocumentFeedback::strokesOf(next);
    QCOMPARE(kept.size(), 2);
    QCOMPARE(kept.at(0).colour, QStringLiteral("#d32f2f"));
    QCOMPARE(kept.at(1).colour, QStringLiteral("#2e7d32"));

    // Anything but "#rrggbb" is the pencil's own colour; upper case is the same colour.
    QCOMPARE(DocumentFeedback::strokeColour(QStringLiteral("#D32F2F")), QStringLiteral("#d32f2f"));
    for (const QString &other : {QStringLiteral("red"), QStringLiteral("#fff"), QStringLiteral("#d32f2f80"), QStringLiteral("d32f2f0"), QString()}) {
        QVERIFY2(DocumentFeedback::strokeColour(other).isEmpty(), qPrintable(other));
    }
    QJsonObject odd = record;
    QJsonObject oddDrawing = odd.value(QStringLiteral("drawing")).toObject();
    QJsonArray oddStrokes = oddDrawing.value(QStringLiteral("strokes")).toArray();
    QJsonObject oddStroke = oddStrokes.at(0).toObject();
    oddStroke.insert(QStringLiteral("color"), 42);
    oddStrokes.replace(0, oddStroke);
    oddDrawing.insert(QStringLiteral("strokes"), oddStrokes);
    odd.insert(QStringLiteral("drawing"), oddDrawing);
    QVERIFY(DocumentFeedback::strokesOf(odd).constFirst().colour.isEmpty());

    // The eraser: a stroke is near a segment that crosses it or passes within the radius, on the
    // page as shown (a page 1000 x 2000 pixels at 100, 100).
    const QRectF page(100.0, 100.0, 1000.0, 2000.0);
    const DocumentFeedback::Stroke line{{QPointF(0.1, 0.1), QPointF(0.5, 0.1)}, QString()}; // (200, 300) to (600, 300)
    QVERIFY(DocumentFeedback::strokeNear(line, page, QPointF(400.0, 250.0), QPointF(400.0, 350.0), 8.0)); // crosses
    QVERIFY(DocumentFeedback::strokeNear(line, page, QPointF(400.0, 306.0), QPointF(450.0, 306.0), 8.0)); // alongside
    QVERIFY(DocumentFeedback::strokeNear(line, page, QPointF(605.0, 300.0), QPointF(605.0, 300.0), 8.0)); // a press past its end
    QVERIFY(!DocumentFeedback::strokeNear(line, page, QPointF(400.0, 310.0), QPointF(450.0, 330.0), 8.0));
    QVERIFY(!DocumentFeedback::strokeNear(line, page, QPointF(620.0, 250.0), QPointF(620.0, 350.0), 8.0));
    const DocumentFeedback::Stroke dot{{QPointF(0.5, 0.5)}, QString()}; // (600, 1100)
    QVERIFY(DocumentFeedback::strokeNear(dot, page, QPointF(590.0, 1095.0), QPointF(610.0, 1095.0), 8.0));
    QVERIFY(!DocumentFeedback::strokeNear(dot, page, QPointF(590.0, 1090.0), QPointF(610.0, 1090.0), 8.0));
    QVERIFY(!DocumentFeedback::strokeNear({}, page, QPointF(600.0, 1100.0), QPointF(600.0, 1100.0), 8.0));
}

void DocumentFeedbackTest::testChangePassage()
{
    using Thoughts::ReviewCardFeedback::regionsJson;
    const QString documentId = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848");
    const QString pdf = QString(64, QLatin1Char('f'));
    const auto anchorAt = [&](const QString &exact, double top) {
        return Thoughts::ReviewCardFeedback::anchor(exact, QString(), QString(), documentId, pdf, regionsJson({{3, QRectF(QPointF(0.1, top), QPointF(0.6, top + 0.04))}}));
    };
    const QString origin = QStringLiteral("mac-okular");
    const QDateTime first = QDateTime::fromString(QStringLiteral("2026-11-01T08:00:00.000Z"), Qt::ISODateWithMs);
    QJsonObject question = Thoughts::DocumentFeedback::withKind(Thoughts::DocumentFeedback::create(Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("Why?"),
                                                                                                   anchorAt(QStringLiteral("old passage"), 0.2), origin, first),
                                                                Thoughts::DocumentFeedback::question());
    question = Thoughts::DocumentFeedback::resolve(question, true, origin, first.addSecs(60));
    QVERIFY(question.contains(QStringLiteral("resolved_at")));
    const QDateTime now = first.addDays(1);
    QString error;
    // A question keeps its kind and resolution; the new record has no card.
    auto replacement = Thoughts::DocumentFeedback::replacePassage(question, Thoughts::ReviewCardFeedback::neutral(), Thoughts::DocumentFeedback::question(),
                                                                  QStringLiteral("Why this?"), anchorAt(QStringLiteral("new passage"), 0.5), origin, now, &error);
    QVERIFY2(replacement, qPrintable(error));
    const QJsonObject created = replacement->created;
    QCOMPARE(Thoughts::DocumentFeedback::validationError(created), QString());
    QVERIFY(!created.contains(QStringLiteral("card_id")));
    QVERIFY(created.value(QStringLiteral("feedback_id")) != question.value(QStringLiteral("feedback_id")));
    QCOMPARE(created.value(QStringLiteral("kind")).toString(), Thoughts::DocumentFeedback::question());
    QCOMPARE(created.value(QStringLiteral("resolved_at")), question.value(QStringLiteral("resolved_at")));
    QCOMPARE(created.value(QStringLiteral("comment")).toString(), QStringLiteral("Why this?"));
    QCOMPARE(created.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(created.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), QStringLiteral("new passage"));
    QCOMPARE(replacement->tombstone.value(QStringLiteral("revision")).toInt(), question.value(QStringLiteral("revision")).toInt() + 1);
    QVERIFY(replacement->tombstone.value(QStringLiteral("deleted")).toBool());
    QCOMPARE(Thoughts::DocumentFeedback::validationError(replacement->tombstone), QString());

    // In one store write.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::DesktopSyncOutbox outbox(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")), QDir(temporary.path()).filePath(QStringLiteral("outbox.json")));
    QVERIFY2(Thoughts::DocumentFeedback::enqueueAll(outbox, {created, replacement->tombstone}, &error), qPrintable(error));
    const QList<QJsonObject> operations = outbox.operations();
    QCOMPARE(operations.size(), 2);
    QCOMPARE(operations.at(0).value(QStringLiteral("entity_type")).toString(), Thoughts::DocumentFeedback::entityType());
    QCOMPARE(operations.at(1).value(QStringLiteral("payload")).toObject(), replacement->tombstone);

    // Undo, as for cards.
    const QList<QJsonObject> undo = Thoughts::ReviewCardFeedback::undoReplacement(*replacement, created, replacement->tombstone, origin, now.addSecs(3));
    QCOMPARE(undo.size(), 2);
    QVERIFY(undo.at(0).value(QStringLiteral("deleted")).toBool());
    QVERIFY(!undo.at(1).value(QStringLiteral("deleted")).toBool());
    for (const QJsonObject &record : undo) QCOMPARE(Thoughts::DocumentFeedback::validationError(record), QString());

    // A like becoming a like: no kind, no resolved_at.
    const QJsonObject like = Thoughts::DocumentFeedback::create(Thoughts::ReviewCardFeedback::positive(), QString(), anchorAt(QStringLiteral("liked"), 0.3), origin, first);
    replacement = Thoughts::DocumentFeedback::replacePassage(like, Thoughts::ReviewCardFeedback::negative(), QString(), QString(), anchorAt(QStringLiteral("disliked"), 0.6), origin, now, &error);
    QVERIFY(replacement);
    QVERIFY(!replacement->created.contains(QStringLiteral("kind")));
    QVERIFY(!replacement->created.contains(QStringLiteral("resolved_at")));
    QCOMPARE(replacement->created.value(QStringLiteral("polarity")).toString(), Thoughts::ReviewCardFeedback::negative());

    // Refusals: a drawing, a deleted record, a missing one, an invalid anchor.
    const QJsonObject drawing = Thoughts::DocumentFeedback::createDrawing(1, {{{QPointF(0.1, 0.1), QPointF(0.2, 0.2)}}}, documentId, pdf, origin, first);
    QVERIFY(!Thoughts::DocumentFeedback::replacePassage(drawing, Thoughts::ReviewCardFeedback::neutral(), Thoughts::DocumentFeedback::drawing(), QString(), anchorAt(QStringLiteral("x"), 0.5), origin, now, &error));
    QVERIFY(!Thoughts::DocumentFeedback::replacePassage(Thoughts::DocumentFeedback::remove(like, origin, now), Thoughts::ReviewCardFeedback::positive(), QString(), QString(), anchorAt(QStringLiteral("x"), 0.5), origin, now, &error));
    QVERIFY(!Thoughts::DocumentFeedback::replacePassage({}, Thoughts::ReviewCardFeedback::positive(), QString(), QString(), anchorAt(QStringLiteral("x"), 0.5), origin, now, &error));
    QVERIFY(!Thoughts::DocumentFeedback::replacePassage(like, Thoughts::ReviewCardFeedback::positive(), QString(), QString(),
                                                        Thoughts::ReviewCardFeedback::anchor(QStringLiteral("x"), QString(), QString(), documentId, pdf, QJsonArray()), origin, now, &error));
}

void DocumentFeedbackTest::testTextBox()
{
    const QRectF box(0.12346, 0.3, 0.2, 0.02);
    const QJsonObject record = DocumentFeedback::createTextBox(2, box, 0.0145, QStringLiteral("  Why not the median?\nOr the mode.  "), documentId, pdfSha, QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(record), QString());
    QVERIFY(DocumentFeedback::isTextBox(record));
    QVERIFY(DocumentFeedback::isPageMark(record));
    QVERIFY(!DocumentFeedback::isDrawing(record));
    QVERIFY(!DocumentFeedback::canResolve(record));
    QCOMPARE(DocumentFeedback::kindOf(record), QStringLiteral("text"));
    QCOMPARE(record.value(QStringLiteral("polarity")).toString(), ReviewCardFeedback::neutral());
    // The text is the comment, so every list of comments shows it; line breaks stay.
    QCOMPARE(record.value(QStringLiteral("comment")).toString(), QStringLiteral("Why not the median?\nOr the mode."));
    // The anchor only names the page; the place is [left, top, right, bottom], to four decimals.
    QCOMPARE(record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), QStringLiteral("Text"));
    QCOMPARE(ReviewCardFeedback::regionsOf(record), (QList<QPair<int, QRectF>>{{2, QRectF(0.0, 0.0, 1.0, 1.0)}}));
    QCOMPARE(record.value(QStringLiteral("text_box")).toObject().value(QStringLiteral("rect")).toArray(), (QJsonArray{0.1235, 0.3, 0.3235, 0.32}));
    QCOMPARE(DocumentFeedback::textBoxFontSize(record), 0.0145);
    QVERIFY(qAbs(DocumentFeedback::textBoxRect(record).width() - 0.2) < 1e-9);

    // It moves and changes without touching what the server keeps as first written; a field another
    // client added to its place stays.
    QJsonObject withColour = record;
    QJsonObject place = record.value(QStringLiteral("text_box")).toObject();
    place.insert(QStringLiteral("colour"), QStringLiteral("kept"));
    withColour.insert(QStringLiteral("text_box"), place);
    const QJsonObject moved = DocumentFeedback::withTextBox(withColour, QRectF(0.5, 0.6, 0.3, 0.05), 0.02, QStringLiteral("Or the mode."), QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(moved), QString());
    QCOMPARE(moved.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(moved.value(QStringLiteral("anchor")), record.value(QStringLiteral("anchor")));
    QCOMPARE(moved.value(QStringLiteral("comment")).toString(), QStringLiteral("Or the mode."));
    QCOMPARE(DocumentFeedback::textBoxRect(moved).topLeft(), QPointF(0.5, 0.6));
    QCOMPARE(DocumentFeedback::textBoxFontSize(moved), 0.02);
    QCOMPARE(moved.value(QStringLiteral("text_box")).toObject().value(QStringLiteral("colour")).toString(), QStringLiteral("kept"));
    QCOMPARE(DocumentFeedback::textBoxRect(DocumentFeedback::merge(record, moved)), DocumentFeedback::textBoxRect(moved));
    // Never off its page, and never a font out of bounds.
    const QJsonObject pushed = DocumentFeedback::withTextBox(record, QRectF(0.9, 0.99, 0.3, 0.05), 1.0, QStringLiteral("x"), QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(pushed), QString());
    QVERIFY(DocumentFeedback::textBoxRect(pushed).right() <= 1.0 && DocumentFeedback::textBoxRect(pushed).bottom() <= 1.0);
    QCOMPARE(DocumentFeedback::textBoxFontSize(pushed), DocumentFeedback::maximumFontSize);

    // On the PDF it was typed on, at its box, and nowhere else; it is never looked for by its quote.
    const QString other(64, QLatin1Char('e'));
    QVERIFY(DocumentFeedback::shownOn(moved, pdfSha));
    QVERIFY(!DocumentFeedback::shownOn(moved, other));
    const auto placed = DocumentFeedback::place(moved, pdfSha, 3, {});
    QCOMPARE(placed.state, QStringLiteral("exact"));
    QCOMPARE(placed.rects.size(), 1);
    QCOMPARE(placed.rects.constFirst().first, 2);
    QCOMPARE(placed.rects.constFirst().second, DocumentFeedback::textBoxRect(moved));
    const auto text = [](const QList<int> &) { return AnswerText{QStringLiteral("A Text here"), {}}; };
    QCOMPARE(DocumentFeedback::place(moved, other, 3, text).state, QStringLiteral("orphaned"));
    QVERIFY(!DocumentFeedback::replacePassage(moved, ReviewCardFeedback::positive(), QString(), QString(), record.value(QStringLiteral("anchor")).toObject(), QStringLiteral("mac")));

    // What the server would refuse is never queued.
    const auto changed = [&record](const QJsonValue &place) {
        QJsonObject result = record;
        result.insert(QStringLiteral("text_box"), place);
        return DocumentFeedback::validationError(result);
    };
    const auto at = [](const QJsonArray &rect, double fontSize = 0.0145) { return QJsonObject{{QStringLiteral("rect"), rect}, {QStringLiteral("font_size"), fontSize}}; };
    QVERIFY(changed(QJsonObject{}).contains(QStringLiteral("text box")));
    QVERIFY(!changed(at({0.5, 0.3, 0.4, 0.4})).isEmpty());
    QVERIFY(!changed(at({0.1, 0.3, 0.4, 0.3})).isEmpty());
    QVERIFY(!changed(at({-0.1, 0.3, 0.4, 0.4})).isEmpty());
    QVERIFY(!changed(at({0.1, 0.3, 1.2, 0.4})).isEmpty());
    QVERIFY(!changed(at({0.1, 0.3, 0.4})).isEmpty());
    QVERIFY(!changed(at({0.1, 0.3, 0.4, 0.4}, 0.0)).isEmpty());
    QVERIFY(!changed(at({0.1, 0.3, 0.4, 0.4}, 0.5)).isEmpty());
    QCOMPARE(changed(at({0.1, 0.3, 0.4, 0.4})), QString());
    QJsonObject blank = record;
    blank.insert(QStringLiteral("comment"), QStringLiteral(" \n "));
    QVERIFY(!DocumentFeedback::validationError(blank).isEmpty());
    QJsonObject withoutPlace = record;
    withoutPlace.remove(QStringLiteral("text_box"));
    QVERIFY(!DocumentFeedback::validationError(withoutPlace).isEmpty());
    QJsonObject commentWithPlace = DocumentFeedback::withKind(record, DocumentFeedback::other());
    QVERIFY(!DocumentFeedback::validationError(commentWithPlace).isEmpty());
    QJsonObject resolved = record;
    resolved.insert(QStringLiteral("resolved_at"), QStringLiteral("2026-11-09T10:00:00.000Z"));
    QVERIFY(!DocumentFeedback::validationError(resolved).isEmpty());
    // Deleted, it is still valid, so the tombstone syncs.
    QCOMPARE(DocumentFeedback::validationError(DocumentFeedback::remove(record, QStringLiteral("mac"))), QString());
}

QTEST_GUILESS_MAIN(DocumentFeedbackTest)
#include "documentfeedbacktest.moc"
