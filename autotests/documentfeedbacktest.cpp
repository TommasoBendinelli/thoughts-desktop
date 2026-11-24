// SPDX-License-Identifier: GPL-2.0-or-later
// Document feedback records, merging and finding the passage again
// (thoughts-server protocol/document-feedback.md, extending review-card-feedback.md).
#include "../part/documentfeedback.h"
#include "../part/desktopsyncstore.h"
#include "../part/projectfeedback.h"
#include "../part/readingoverlays.h"

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
    void testProjectReadingMovesFeedbackToItsDocument();
    void testSearchPages();
    void testPlacement();
    void testPlacementAcrossPageBreak();
    void testSelectionContext();
    void testDrawing();
    void testDrawingColours();
    void testDrawingInAnHtmlAttachment();
    void testTextBox();
    void testPictures();
    void testChangePassage();
    // Reading overlays (reading-overlays.md): the record, the user's edits, merging and placement.
    void testReadingOverlayRecord();
    void testReadingOverlayDeleteMarkAndOverlay();
    void testReadingOverlayMerge();
    void testReadingOverlaysOfDocument();
    void testReadingOverlayMarksAreFoundAsFeedback();
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

void DocumentFeedbackTest::testPlacementAcrossPageBreak()
{
    // Pages 5 and 6 of a paper: a running head at the top, a page number at the foot, and a passage
    // that runs on from the last line of page 5 to the first of page 6.
    QHash<int, QList<Glyph>> pages;
    pages[5] = line(5, QStringLiteral("Paper Title"), 0.4, 0.03) + line(5, QStringLiteral("body of page 5"), 0.1, 0.3)
        + line(5, QStringLiteral("the passage runs"), 0.1, 0.85) + line(5, QStringLiteral("5"), 0.5, 0.95);
    pages[6] = line(6, QStringLiteral("Paper Title"), 0.4, 0.03) + line(6, QStringLiteral("on to the next page"), 0.1, 0.10)
        + line(6, QStringLiteral("body of page 6"), 0.1, 0.3) + line(6, QStringLiteral("6"), 0.5, 0.95);
    const auto pageText = [&](const QList<int> &wanted) {
        QList<QList<Glyph>> regions;
        for (const int page : wanted) regions.append(pages.value(page));
        return ReviewCardFeedback::answerText(regions);
    };
    const QString otherSha = QString(64, QLatin1Char('d'));
    const auto across = [&](const QString &quote) {
        const QJsonObject anchor = ReviewCardFeedback::anchor(quote, QString(), QString(), documentId, otherSha,
                                                              ReviewCardFeedback::regionsJson({{5, QRectF(QPointF(0.1, 0.85), QPointF(0.3, 0.87))},
                                                                                               {6, QRectF(QPointF(0.1, 0.1), QPointF(0.3, 0.12))}}));
        return DocumentFeedback::create(ReviewCardFeedback::positive(), QString(), anchor, QStringLiteral("mac-okular"),
                                        QDateTime::fromString(QStringLiteral("2026-10-28T09:00:00.000Z"), Qt::ISODateWithMs));
    };

    // The quote leaves out "5" and "Paper Title" between the pages (as the server places it).
    auto placement = DocumentFeedback::place(across(QStringLiteral("the passage runs on to the next page")), pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("relocated"));
    QCOMPARE(placement.rects.size(), 2);
    QCOMPARE(placement.rects.at(0).first, 5);
    QVERIFY(qAbs(placement.rects.at(0).second.top() - 0.85) < 1e-9);
    QCOMPARE(placement.rects.at(1).first, 6);
    QVERIFY(qAbs(placement.rects.at(1).second.top() - 0.10) < 1e-9);
    QVERIFY(qAbs(placement.rects.at(1).second.right() - 0.29) < 1e-9);
    // A quote that has them still matches the text as it is.
    placement = DocumentFeedback::place(across(QStringLiteral("the passage runs 5 Paper Title on to the next page")), pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("relocated"));
    QCOMPARE(placement.rects.size(), 4);

    // Only between the pages: page 5's head, page 6's foot and the furniture of a page searched
    // alone stay.
    const ReviewCardFeedback::AnswerText both = pageText({5, 6});
    const ReviewCardFeedback::AnswerText bare = DocumentFeedback::withoutPageFurniture(both);
    QCOMPARE(bare.glyphs.size(), both.glyphs.size());
    QCOMPARE(bare.text.simplified(), QStringLiteral("Paper Title body of page 5 the passage runs on to the next page body of page 6 6"));
    for (int i = 0; i < bare.text.size(); ++i) QCOMPARE(bare.text.at(i), bare.source.at(i) < 0 ? QLatin1Char('\n') : both.glyphs.at(bare.source.at(i)).text.at(0));
    QCOMPARE(DocumentFeedback::withoutPageFurniture(pageText({5})).text, pageText({5}).text);

    // A last line close to the text above it is text, not furniture; so is a page of one line.
    pages[7] = line(7, QStringLiteral("body of page 7"), 0.1, 0.90) + line(7, QStringLiteral("ends near the foot"), 0.1, 0.925);
    pages[8] = line(8, QStringLiteral("Paper Title"), 0.4, 0.03);
    const ReviewCardFeedback::AnswerText kept = pageText({7, 8});
    QCOMPARE(DocumentFeedback::withoutPageFurniture(kept).text, kept.text);
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

void DocumentFeedbackTest::testDrawingInAnHtmlAttachment()
{
    // §10.1 (18 November: the pencil on an HTML page of interactive plots did nothing): a drawing in an
    // HTML attachment is made on a snapshot of the page, kept with it.
    QCOMPARE(DocumentFeedback::htmlDrawingCapability(), QStringLiteral("document_feedback_html_drawing_v1"));
    const QString pageSha(64, QLatin1Char('a'));
    const QString pictureSha(64, QLatin1Char('c'));
    const QJsonObject placement{{QStringLiteral("created_in"), QStringLiteral("html_attachment")},
                                {QStringLiteral("html_attachment"), QJsonObject{{QStringLiteral("path"), QStringLiteral("docs-x/html/plots.html")}, {QStringLiteral("sha256"), pageSha}}},
                                {QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("selected words")}}},
                                {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("#plots")}}}};
    const QJsonObject snapshot = DocumentFeedback::snapshotJson(pictureSha.toUpper(), QStringLiteral("image/png"), 2400, 1500);
    QCOMPARE(snapshot.value(QStringLiteral("object_sha256")).toString(), pictureSha);
    const QList<DocumentFeedback::Stroke> strokes{{{QPointF(0.41, 0.22), QPointF(0.47, 0.25)}, QStringLiteral("#d32f2f")}};
    const QJsonObject record = DocumentFeedback::createAttachmentDrawing(placement, snapshot, strokes, documentId, pdfSha, QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(record), QString());
    // The record of the protocol's example.
    QVERIFY(DocumentFeedback::isDrawing(record));
    QVERIFY(DocumentFeedback::isAttachmentDrawing(record));
    QVERIFY(!DocumentFeedback::isAttachmentDrawing(DocumentFeedback::createDrawing(1, strokes, documentId, pdfSha, QStringLiteral("mac"))));
    QCOMPARE(record.value(QStringLiteral("view")).toString(), DocumentFeedback::attachmentView());
    QCOMPARE(record.value(QStringLiteral("polarity")).toString(), ReviewCardFeedback::neutral());
    QCOMPARE(record.value(QStringLiteral("kind")).toString(), DocumentFeedback::drawing());
    const QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
    QCOMPARE(anchor.value(QStringLiteral("quote")).toObject(),
             QJsonObject({{QStringLiteral("exact"), QStringLiteral("Drawing")}, {QStringLiteral("prefix"), QString()}, {QStringLiteral("suffix"), QString()}}));
    QCOMPARE(anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("document_id")).toString(), documentId);
    QCOMPARE(anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString(), pdfSha);
    QVERIFY(anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray().isEmpty());
    QCOMPARE(anchor.value(QStringLiteral("html_attachment")), placement.value(QStringLiteral("html_attachment")));
    QCOMPARE(anchor.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#plots"));
    QCOMPARE(DocumentFeedback::snapshotOf(record), snapshot);
    QCOMPARE(DocumentFeedback::snapshotSha256(record), pictureSha);
    QCOMPARE(DocumentFeedback::strokesOf(record).constFirst().points, strokes.constFirst().points);
    QCOMPARE(DocumentFeedback::strokesOf(record).constFirst().colour, QStringLiteral("#d32f2f"));
    // No passage, no resolution, and no place on a page of the PDF.
    QVERIFY(DocumentFeedback::isPageMark(record));
    QVERIFY(!DocumentFeedback::canResolve(record));
    QVERIFY(DocumentFeedback::pageMarkBounds(record).isEmpty());
    QVERIFY(!DocumentFeedback::strokeBounds(record).isEmpty());
    QVERIFY(!DocumentFeedback::replacePassage(record, ReviewCardFeedback::positive(), QString(), QString(), record.value(QStringLiteral("anchor")).toObject(), QStringLiteral("mac")));

    // It shows on the page version it was drawn on, never on the PDF, even the one that linked the page.
    QVERIFY(DocumentFeedback::shownOn(record, pdfSha, pageSha));
    QVERIFY(DocumentFeedback::shownOn(record, QString(64, QLatin1Char('e')), pageSha));
    QVERIFY(!DocumentFeedback::shownOn(record, pdfSha, QString(64, QLatin1Char('b'))));
    QVERIFY(!DocumentFeedback::shownOn(record, pdfSha));
    QCOMPARE(DocumentFeedback::place(record, pdfSha, 3, {}).state, QStringLiteral("orphaned"));
    QVERIFY(DocumentFeedback::place(record, pdfSha, 3, {}).rects.isEmpty());

    // More strokes later: the next revision keeps the anchor and the snapshot it was drawn on.
    const QList<DocumentFeedback::Stroke> more = strokes + QList<DocumentFeedback::Stroke>{{{QPointF(0.9, 0.95)}, QString()}};
    const QJsonObject next = DocumentFeedback::withStrokes(record, more, QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(next), QString());
    QCOMPARE(next.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(next.value(QStringLiteral("anchor")), record.value(QStringLiteral("anchor")));
    QCOMPARE(DocumentFeedback::snapshotOf(next), snapshot);
    QCOMPARE(DocumentFeedback::strokesOf(DocumentFeedback::merge(record, next)).size(), 2);
    // A field another client added to the drawing stays too.
    QJsonObject extended = next;
    QJsonObject extendedDrawing = extended.value(QStringLiteral("drawing")).toObject();
    extendedDrawing.insert(QStringLiteral("future_field"), 7);
    extended.insert(QStringLiteral("drawing"), extendedDrawing);
    QCOMPARE(DocumentFeedback::withStrokes(extended, strokes, QStringLiteral("mac")).value(QStringLiteral("drawing")).toObject().value(QStringLiteral("future_field")).toInt(), 7);
    QCOMPARE(DocumentFeedback::validationError(DocumentFeedback::remove(next, QStringLiteral("mac"))), QString());

    // What the server refuses is never queued: no snapshot, or a bad one, in a page; one on the PDF.
    const auto withSnapshot = [&record](const QJsonValue &value) {
        QJsonObject changed = record;
        QJsonObject drawn = changed.value(QStringLiteral("drawing")).toObject();
        if (value.isUndefined()) drawn.remove(QStringLiteral("snapshot"));
        else drawn.insert(QStringLiteral("snapshot"), value);
        changed.insert(QStringLiteral("drawing"), drawn);
        return DocumentFeedback::validationError(changed);
    };
    QVERIFY(withSnapshot(QJsonValue::Undefined).contains(QStringLiteral("drawing")));
    QVERIFY(!withSnapshot(QJsonObject{}).isEmpty());
    QVERIFY(!withSnapshot(QStringLiteral("picture")).isEmpty());
    QJsonObject bad = snapshot;
    bad.insert(QStringLiteral("object_sha256"), pictureSha.toUpper());
    QVERIFY(!withSnapshot(bad).isEmpty());
    bad = snapshot;
    bad.insert(QStringLiteral("object_sha256"), QStringLiteral("abc"));
    QVERIFY(!withSnapshot(bad).isEmpty());
    bad = snapshot;
    bad.insert(QStringLiteral("media_type"), QStringLiteral("image/gif"));
    QVERIFY(!withSnapshot(bad).isEmpty());
    bad.insert(QStringLiteral("media_type"), QStringLiteral("image/jpeg"));
    QVERIFY(withSnapshot(bad).isEmpty());
    bad = snapshot;
    bad.insert(QStringLiteral("width"), 0);
    QVERIFY(!withSnapshot(bad).isEmpty());
    bad = snapshot;
    bad.insert(QStringLiteral("height"), 1.5);
    QVERIFY(!withSnapshot(bad).isEmpty());
    bad = snapshot;
    bad.insert(QStringLiteral("height"), DocumentFeedback::maximumSnapshotSide + 1);
    QVERIFY(!withSnapshot(bad).isEmpty());
    bad = snapshot;
    bad.remove(QStringLiteral("width"));
    QVERIFY(!withSnapshot(bad).isEmpty());
    QJsonObject onPdf = DocumentFeedback::createDrawing(1, strokes, documentId, pdfSha, QStringLiteral("mac"));
    QJsonObject onPdfDrawing = onPdf.value(QStringLiteral("drawing")).toObject();
    onPdfDrawing.insert(QStringLiteral("snapshot"), snapshot);
    onPdf.insert(QStringLiteral("drawing"), onPdfDrawing);
    QVERIFY(!DocumentFeedback::validationError(onPdf).isEmpty());
    // Strokes on the snapshot as on a page: in the unit square, not empty, at most 200.
    const auto withStrokesJson = [&record](const QJsonArray &json) {
        QJsonObject changed = record;
        QJsonObject drawn = changed.value(QStringLiteral("drawing")).toObject();
        drawn.insert(QStringLiteral("strokes"), json);
        changed.insert(QStringLiteral("drawing"), drawn);
        return DocumentFeedback::validationError(changed);
    };
    // One stroke of the points @p xy (x, y, x, y, ...).
    const auto stroke = [](const QList<double> &xy) {
        QJsonArray points;
        for (qsizetype i = 0; i + 1 < xy.size(); i += 2) points.append(QJsonArray{xy.at(i), xy.at(i + 1)});
        return QJsonObject{{QStringLiteral("points"), points}};
    };
    QVERIFY(withStrokesJson(QJsonArray{stroke({0.0, 1.0, 1.0, 0.0})}).isEmpty());
    QVERIFY(!withStrokesJson(QJsonArray()).isEmpty());
    QVERIFY(!withStrokesJson(QJsonArray{stroke({})}).isEmpty());
    QVERIFY(!withStrokesJson(QJsonArray{stroke({1.01, 0.5})}).isEmpty());
    QVERIFY(!withStrokesJson(QJsonArray{stroke({0.5, -0.01})}).isEmpty());
    QJsonArray many;
    for (int i = 0; i <= DocumentFeedback::maximumStrokes; ++i) many.append(stroke({0.5, 0.5}));
    QVERIFY(!withStrokesJson(many).isEmpty());
    many.removeLast();
    QVERIFY(withStrokesJson(many).isEmpty());
    // A text box is typed on a page of the PDF, never in a page.
    QJsonObject typed = DocumentFeedback::createTextBox(1, QRectF(0.2, 0.2, 0.3, 0.03), 0.015, QStringLiteral("Why?"), documentId, pdfSha, QStringLiteral("mac"));
    QCOMPARE(DocumentFeedback::validationError(typed), QString());
    QJsonObject typedAnchor = typed.value(QStringLiteral("anchor")).toObject();
    typedAnchor.insert(QStringLiteral("html_attachment"), placement.value(QStringLiteral("html_attachment")));
    QJsonObject typedPdf = typedAnchor.value(QStringLiteral("pdf")).toObject();
    typedPdf.insert(QStringLiteral("regions"), QJsonArray());
    typedAnchor.insert(QStringLiteral("pdf"), typedPdf);
    typed.insert(QStringLiteral("anchor"), typedAnchor);
    typed.insert(QStringLiteral("view"), DocumentFeedback::attachmentView());
    QVERIFY(!DocumentFeedback::validationError(typed).isEmpty());

    // Queued as any feedback, with the snapshot in its payload.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::DesktopSyncOutbox outbox(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")), QDir(temporary.path()).filePath(QStringLiteral("outbox.json")));
    QString error;
    QVERIFY2(DocumentFeedback::enqueue(outbox, record, &error), qPrintable(error));
    QCOMPARE(outbox.operations().size(), 1);
    QCOMPARE(DocumentFeedback::snapshotSha256(outbox.operations().constFirst().value(QStringLiteral("payload")).toObject()), pictureSha);
    QJsonObject withoutPicture = record;
    withoutPicture.insert(QStringLiteral("drawing"), QJsonObject{{QStringLiteral("strokes"), record.value(QStringLiteral("drawing")).toObject().value(QStringLiteral("strokes"))}});
    QVERIFY(!DocumentFeedback::enqueue(outbox, withoutPicture, &error));
    QCOMPARE(outbox.operations().size(), 1);
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

void DocumentFeedbackTest::testPictures()
{
    // §12 (20 November): pictures on a comment, objects uploaded before the record.
    QCOMPARE(DocumentFeedback::picturesCapability(), QStringLiteral("document_feedback_pictures_v1"));
    const QJsonObject photo = DocumentFeedback::snapshotJson(QString(64, QLatin1Char('a')), QStringLiteral("image/jpeg"), 2048, 1536);
    const QJsonObject screenshot = DocumentFeedback::snapshotJson(QString(64, QLatin1Char('b')), QStringLiteral("image/png"), 800, 600);
    const QJsonObject record = DocumentFeedback::withPictures(specExample(), {photo, screenshot});
    QCOMPARE(DocumentFeedback::picturesOf(record), QJsonArray({photo, screenshot}));
    QCOMPARE(DocumentFeedback::pictureSha256s(record), QStringList({QString(64, QLatin1Char('a')), QString(64, QLatin1Char('b'))}));
    QVERIFY(DocumentFeedback::hasPictures(record));
    QVERIFY2(DocumentFeedback::validationError(record).isEmpty(), qPrintable(DocumentFeedback::validationError(record)));
    QCOMPARE(DocumentFeedback::agentPicturesNote(record), QStringLiteral(" [2 pictures: the Thoughts document_feedback tool shows them]"));
    QCOMPARE(DocumentFeedback::agentPicturesNote(DocumentFeedback::withPictures(record, {photo})), QStringLiteral(" [1 picture: the Thoughts document_feedback tool shows it]"));
    // None is no field.
    QVERIFY(!DocumentFeedback::withPictures(record, {}).contains(QStringLiteral("pictures")));
    QVERIFY(!DocumentFeedback::hasPictures(specExample()));
    QVERIFY(DocumentFeedback::agentPicturesNote(specExample()).isEmpty());
    // An edit keeps them.
    QCOMPARE(DocumentFeedback::picturesOf(DocumentFeedback::edit(record, ReviewCardFeedback::negative(), QStringLiteral("Like this"), QStringLiteral("mac"))),
             QJsonArray({photo, screenshot}));
    // The server's rules: a picture type, its size, no object twice, at most 8, a list.
    const auto invalid = [](const QJsonValue &pictures) {
        QJsonObject candidate = specExample();
        candidate.insert(QStringLiteral("pictures"), pictures);
        return !DocumentFeedback::validationError(candidate).isEmpty();
    };
    QVERIFY(!invalid(QJsonArray()));
    QVERIFY(invalid(QJsonArray{photo, photo}));
    QVERIFY(invalid(QJsonArray{DocumentFeedback::snapshotJson(QString(64, QLatin1Char('c')), QStringLiteral("image/gif"), 10, 10)}));
    QVERIFY(invalid(QJsonArray{DocumentFeedback::snapshotJson(QString(64, QLatin1Char('c')), QStringLiteral("image/png"), 0, 10)}));
    QVERIFY(invalid(QStringLiteral("photo.jpg")));
    QJsonArray nine;
    for (int i = 0; i < 9; ++i) nine.append(DocumentFeedback::snapshotJson(QString(64, QLatin1Char('0' + i)), QStringLiteral("image/png"), 1, 1));
    QVERIFY(invalid(nine));
    nine.removeLast();
    QVERIFY(!invalid(nine));
    // Never on a drawing: it is its strokes.
    QJsonObject drawing = DocumentFeedback::createDrawing(2, {{{QPointF(0.1, 0.1)}, QString()}}, documentId, pdfSha, QStringLiteral("mac"));
    drawing.insert(QStringLiteral("pictures"), QJsonArray{photo});
    QVERIFY(!DocumentFeedback::validationError(drawing).isEmpty());
    QVERIFY(DocumentFeedback::picturesOf(drawing).isEmpty());
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

void DocumentFeedbackTest::testProjectReadingMovesFeedbackToItsDocument()
{
    // A Project read as one PDF: two documents of 3 and 4 pages, the second starting at page 4.
    ProjectReading reading;
    ReadingDocument first;
    first.id = QStringLiteral("0190a000-0000-4000-8000-00000000000a");
    first.pdfSha256 = QString(64, QLatin1Char('a'));
    first.pages = 3;
    first.firstPage = 0;
    ReadingDocument second;
    second.id = QStringLiteral("0190a000-0000-4000-8000-00000000000b");
    second.pdfSha256 = QString(64, QLatin1Char('b'));
    second.pages = 4;
    second.firstPage = 3;
    reading.documents = {first, second};
    reading.pages = 7;
    const QString virtualSha = QString(64, QLatin1Char('c'));
    const QString virtualId = ProjectFeedback::virtualDocumentId(virtualSha);
    QVERIFY(QUuid(virtualId).version() == QUuid::Random);
    QCOMPARE(ProjectFeedback::virtualDocumentId(virtualSha), virtualId);

    const auto record = [](const QString &documentId, const QString &sha, QList<int> pages, const QString &versionSha) {
        QJsonArray regions;
        for (int page : pages) regions.append(QJsonObject{{QStringLiteral("page"), page}, {QStringLiteral("rect"), QJsonArray{0.1, 0.2, 0.9, 0.25}}});
        return QJsonObject{{QStringLiteral("feedback_id"), QStringLiteral("0190a000-0000-4000-8000-0000000000f1")},
                           {QStringLiteral("anchor"), QJsonObject{{QStringLiteral("pdf"), QJsonObject{{QStringLiteral("document_id"), documentId},
                                                                                                     {QStringLiteral("pdf_sha256"), sha},
                                                                                                     {QStringLiteral("regions"), regions}}}}},
                           {FeedbackVersion::field(), QJsonObject{{QStringLiteral("pdf_sha256"), versionSha}, {QStringLiteral("source_built"), true}}}};
    };
    const auto pages = [](const QJsonObject &value) {
        QList<int> result;
        for (const QJsonValue &region : value.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray())
            result.append(region.toObject().value(QStringLiteral("page")).toInt());
        return result;
    };
    const auto pdf = [](const QJsonObject &value) { return value.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject(); };

    // The second document's page 2 shows on the combined PDF's page 5, under the virtual document.
    const QJsonObject own = record(second.id, second.pdfSha256, {2}, second.pdfSha256);
    const QJsonObject shown = ProjectFeedback::toProject(own, second, virtualId, virtualSha);
    QCOMPARE(pages(shown), QList<int>{5});
    QCOMPARE(pdf(shown).value(QStringLiteral("document_id")).toString(), virtualId);
    QCOMPARE(pdf(shown).value(QStringLiteral("pdf_sha256")).toString(), virtualSha);
    QCOMPARE(DocumentFeedback::recordsOfDocument(QList<QJsonObject>{shown}, virtualId, true).size(), 1);
    // And back, exactly as it was.
    QCOMPARE(ProjectFeedback::fromProject(shown, second, virtualSha), own);
    // Feedback made on an earlier PDF of the document keeps its hash (it is placed by its text).
    const QJsonObject earlier = record(second.id, QString(64, QLatin1Char('e')), {1}, QString(64, QLatin1Char('e')));
    QCOMPARE(pdf(ProjectFeedback::toProject(earlier, second, virtualId, virtualSha)).value(QStringLiteral("pdf_sha256")).toString(), QString(64, QLatin1Char('e')));

    // New feedback given in the combined PDF goes to the document its passage is in.
    QCOMPARE(ProjectFeedback::documentOf(record(virtualId, virtualSha, {4, 5}, virtualSha), reading), &reading.documents[1]);
    QCOMPARE(ProjectFeedback::documentOf(record(virtualId, virtualSha, {3}, virtualSha), reading), &reading.documents[0]);
    // Across two documents, or outside the reading: none.
    QVERIFY(!ProjectFeedback::documentOf(record(virtualId, virtualSha, {3, 4}, virtualSha), reading));
    QVERIFY(!ProjectFeedback::documentOf(record(virtualId, virtualSha, {8}, virtualSha), reading));
    const QJsonObject given = ProjectFeedback::fromProject(record(virtualId, virtualSha, {4, 5}, virtualSha), second, virtualSha);
    QCOMPARE(pages(given), (QList<int>{1, 2}));
    QCOMPARE(pdf(given).value(QStringLiteral("document_id")).toString(), second.id);
    QCOMPARE(pdf(given).value(QStringLiteral("pdf_sha256")).toString(), second.pdfSha256);
    QCOMPARE(given.value(FeedbackVersion::field()).toObject().value(QStringLiteral("pdf_sha256")).toString(), second.pdfSha256);
}

namespace
{
// §1's example, with a second and a third mark and fields this client does not know.
QJsonObject overlayExample()
{
    return QJsonDocument::fromJson(R"json({
      "schema_version": 1,
      "overlay_id": "5b0c1f6e-2a47-4d1e-9c3b-7e8f2a6d4c10",
      "title": "What to read, from your mental model",
      "summary": "Sections 1-2 repeat what your KL cards cover; read Section 3 and Table 2 closely.",
      "document": {
        "document_id": "c1e62ecc-2dfb-4259-97cc-e319b1186848",
        "pdf_sha256": "f4c8827551095a63ce58c6b1e2c5946014b7b3f62d0f96a780a2931f601a561e",
        "filename": "paper.pdf"
      },
      "author": {"agent": "claude", "label": "Agent Study", "conversation_id": "1f0e2d3c-4b5a-4968-8776-655443322110",
                 "conversation_url": "https://claude.ai/code/session_01ExampleSessionAbcdEfgh"},
      "marks": [
        {"mark_id": "m1", "kind": "skip", "comment": "You know reverse KL from 6 cards (last reviewed 12 Nov).",
         "anchor": {"version": 1, "quote": {"exact": "Reverse KL has natural synergy with RL", "prefix": "", "suffix": ", which"},
                    "pdf": {"document_id": "c1e62ecc-2dfb-4259-97cc-e319b1186848",
                            "pdf_sha256": "f4c8827551095a63ce58c6b1e2c5946014b7b3f62d0f96a780a2931f601a561e",
                            "regions": [{"page": 3, "rect": [0.0830, 0.2010, 0.9120, 0.2150]}]}}},
        {"mark_id": "m2", "kind": "read", "comment": "",
         "anchor": {"version": 1, "quote": {"exact": "page 4 words", "prefix": "", "suffix": ""},
                    "pdf": {"document_id": "c1e62ecc-2dfb-4259-97cc-e319b1186848",
                            "pdf_sha256": "f4c8827551095a63ce58c6b1e2c5946014b7b3f62d0f96a780a2931f601a561e",
                            "regions": [{"page": 4, "rect": [0.1, 0.1, 0.4, 0.12]}]}}},
        {"mark_id": "m3", "kind": "future_kind", "comment": "A later kind.", "future_mark_field": true,
         "anchor": {"version": 1, "quote": {"exact": "a passage that was edited away", "prefix": "", "suffix": ""},
                    "pdf": {"document_id": "c1e62ecc-2dfb-4259-97cc-e319b1186848",
                            "pdf_sha256": "f4c8827551095a63ce58c6b1e2c5946014b7b3f62d0f96a780a2931f601a561e",
                            "regions": [{"page": 5, "rect": [0.1, 0.3, 0.4, 0.32]}]}}}
      ],
      "revision": 1,
      "created_at": "2026-11-19T17:30:00.000Z",
      "updated_at": "2026-11-19T17:30:00.000Z",
      "created_origin_id": "example-connector",
      "origin_id": "example-connector",
      "deleted": false,
      "future_field": [1, 2]
    })json").object();
}

QStringList markIds(const QJsonObject &record)
{
    QStringList ids;
    for (const QJsonObject &mark : ReadingOverlays::marksOf(record)) ids.append(ReadingOverlays::markIdOf(mark));
    return ids;
}
}

void DocumentFeedbackTest::testReadingOverlayRecord()
{
    const QJsonObject record = overlayExample();
    QCOMPARE(ReadingOverlays::entityType(), QStringLiteral("reading_overlay"));
    QCOMPARE(ReadingOverlays::collection(), QStringLiteral("reading-overlays"));
    QCOMPARE(ReadingOverlays::capability(), QStringLiteral("reading_overlays_v1"));
    QCOMPARE(ReadingOverlays::projectionKey(QStringLiteral("x")), QStringLiteral("reading_overlay\nreading-overlays\nx"));
    QCOMPARE(ReadingOverlays::overlayIdOf(record), QStringLiteral("5b0c1f6e-2a47-4d1e-9c3b-7e8f2a6d4c10"));
    QCOMPARE(ReadingOverlays::documentIdOf(record), documentId);
    QCOMPARE(ReadingOverlays::pdfSha256Of(record), pdfSha);
    QCOMPARE(ReadingOverlays::authorLabel(record), QStringLiteral("Agent Study"));
    QCOMPARE(ReadingOverlays::conversationUrl(record), QStringLiteral("https://claude.ai/code/session_01ExampleSessionAbcdEfgh"));
    QJsonObject unnamed = record;
    unnamed.insert(QStringLiteral("author"), QJsonObject{{QStringLiteral("agent"), QStringLiteral("codex")}, {QStringLiteral("label"), QString()}});
    QCOMPARE(ReadingOverlays::authorLabel(unnamed), QStringLiteral("codex"));
    QVERIFY(ReadingOverlays::conversationUrl(unnamed).isEmpty());
    // The marks in the record's order; a kind this client does not know is shown as a note.
    QCOMPARE(markIds(record), QStringList({QStringLiteral("m1"), QStringLiteral("m2"), QStringLiteral("m3")}));
    const QJsonObject skip = ReadingOverlays::markOf(record, QStringLiteral("m1"));
    QCOMPARE(ReadingOverlays::kindOf(skip), ReadingOverlays::skip());
    QCOMPARE(ReadingOverlays::kindOf(ReadingOverlays::markOf(record, QStringLiteral("m3"))), ReadingOverlays::note());
    QVERIFY(ReadingOverlays::markOf(record, QStringLiteral("m9")).isEmpty());
    QCOMPARE(ReadingOverlays::commentOf(skip), QStringLiteral("You know reverse KL from 6 cards (last reviewed 12 Nov)."));
    QCOMPARE(ReadingOverlays::quoteOf(skip), QStringLiteral("Reverse KL has natural synergy with RL"));
    // The Overlay tab lists the first 100 characters of a quote.
    QCOMPARE(ReadingOverlays::quoteStart(skip), QStringLiteral("Reverse KL has natural synergy with RL"));
    QJsonObject longMark = skip;
    QJsonObject anchor = longMark.value(QStringLiteral("anchor")).toObject();
    anchor.insert(QStringLiteral("quote"), QJsonObject{{QStringLiteral("exact"), QString(150, QLatin1Char('x'))}});
    longMark.insert(QStringLiteral("anchor"), anchor);
    QCOMPARE(ReadingOverlays::quoteStart(longMark), QString(100, QLatin1Char('x')) + QStringLiteral("…"));
    // A mark's passage is read exactly as a document feedback anchor.
    QCOMPARE(ReviewCardFeedback::regionsOf(ReadingOverlays::passageOf(skip)), (QList<QPair<int, QRectF>>{{3, QRectF(QPointF(0.0830, 0.2010), QPointF(0.9120, 0.2150))}}));
    QVERIFY(!ReadingOverlays::isDeleted(record));
}

void DocumentFeedbackTest::testReadingOverlayDeleteMarkAndOverlay()
{
    const QJsonObject record = overlayExample();
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-11-19T18:00:00.000Z"), Qt::ISODateWithMs);
    // Delete Mark: revision + 1 without that mark; the rest, unknown fields included, as it was.
    const QJsonObject withoutSkip = ReadingOverlays::withoutMark(record, QStringLiteral("m1"), QStringLiteral("mac-okular"), now);
    QCOMPARE(withoutSkip.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(markIds(withoutSkip), QStringList({QStringLiteral("m2"), QStringLiteral("m3")}));
    QCOMPARE(withoutSkip.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-11-19T18:00:00.000Z"));
    QCOMPARE(withoutSkip.value(QStringLiteral("origin_id")).toString(), QStringLiteral("mac-okular"));
    QCOMPARE(withoutSkip.value(QStringLiteral("created_origin_id")).toString(), QStringLiteral("example-connector"));
    QCOMPARE(withoutSkip.value(QStringLiteral("deleted")).toBool(), false);
    for (const QString &field : {QStringLiteral("future_field"), QStringLiteral("title"), QStringLiteral("summary"), QStringLiteral("document"), QStringLiteral("author"),
                                 QStringLiteral("created_at")})
        QCOMPARE(withoutSkip.value(field), record.value(field));
    QCOMPARE(ReadingOverlays::markOf(withoutSkip, QStringLiteral("m3")), ReadingOverlays::markOf(record, QStringLiteral("m3")));
    QCOMPARE(ReadingOverlays::validationError(withoutSkip), QString());
    // An unknown mark, or a deleted overlay: nothing to write.
    QVERIFY(ReadingOverlays::withoutMark(record, QStringLiteral("m9"), QStringLiteral("mac-okular"), now).isEmpty());
    // A clock behind the record's updated_at still writes a later time.
    const QJsonObject later = ReadingOverlays::withoutMark(withoutSkip, QStringLiteral("m2"), QStringLiteral("mac-okular"), now.addSecs(-3600));
    QCOMPARE(later.value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(later.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-11-19T18:00:00.001Z"));
    // The last mark: the overlay's tombstone instead, with that mark kept (the server refuses an
    // overlay without marks).
    const QJsonObject last = ReadingOverlays::withoutMark(later, QStringLiteral("m3"), QStringLiteral("mac-okular"), now.addSecs(60));
    QCOMPARE(last.value(QStringLiteral("revision")).toInt(), 4);
    QVERIFY(ReadingOverlays::isDeleted(last));
    QCOMPARE(markIds(last), QStringList{QStringLiteral("m3")});
    QCOMPARE(ReadingOverlays::validationError(last), QString());
    QVERIFY(ReadingOverlays::withoutMark(last, QStringLiteral("m3"), QStringLiteral("mac-okular"), now).isEmpty());
    // Delete Overlay: revision + 1, deleted, the rest kept.
    const QJsonObject tombstone = ReadingOverlays::remove(record, QStringLiteral("mac-okular"), now);
    QCOMPARE(tombstone.value(QStringLiteral("revision")).toInt(), 2);
    QVERIFY(ReadingOverlays::isDeleted(tombstone));
    QCOMPARE(markIds(tombstone), markIds(record));
    QCOMPARE(ReadingOverlays::validationError(tombstone), QString());

    // What the client may queue: a later revision of a valid overlay.
    QVERIFY(!ReadingOverlays::validationError(record).isEmpty()); // revision 1 is the agent's
    QJsonObject broken = withoutSkip;
    broken.insert(QStringLiteral("overlay_id"), QStringLiteral("not-a-uuid"));
    QVERIFY(!ReadingOverlays::validationError(broken).isEmpty());
    broken = withoutSkip;
    broken.insert(QStringLiteral("marks"), QJsonArray());
    QVERIFY(!ReadingOverlays::validationError(broken).isEmpty());
    broken = withoutSkip;
    broken.insert(QStringLiteral("document"), QJsonObject{{QStringLiteral("pdf_sha256"), pdfSha}});
    QVERIFY(!ReadingOverlays::validationError(broken).isEmpty());
    // Two marks with one id: the server would refuse the whole batch.
    broken = withoutSkip;
    QJsonArray twice = broken.value(QStringLiteral("marks")).toArray();
    twice.append(twice.first());
    broken.insert(QStringLiteral("marks"), twice);
    QVERIFY(!ReadingOverlays::validationError(broken).isEmpty());

    // Queued as the server takes it: collection reading-overlays, entity_id = overlay_id, base
    // revision = revision - 1.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    DesktopSyncOutbox outbox(QDir(temporary.path()).filePath(QStringLiteral("desktop-sync.sqlite")), QDir(temporary.path()).filePath(QStringLiteral("outbox.json")));
    QString error;
    QVERIFY2(ReadingOverlays::enqueue(outbox, withoutSkip, &error), qPrintable(error));
    QVERIFY(!ReadingOverlays::enqueue(outbox, broken, &error));
    const QList<QJsonObject> operations = outbox.operations();
    QCOMPARE(operations.size(), 1);
    QCOMPARE(operations.first().value(QStringLiteral("entity_type")).toString(), QStringLiteral("reading_overlay"));
    QCOMPARE(operations.first().value(QStringLiteral("collection")).toString(), QStringLiteral("reading-overlays"));
    QCOMPARE(operations.first().value(QStringLiteral("entity_id")).toString(), ReadingOverlays::overlayIdOf(record));
    QCOMPARE(operations.first().value(QStringLiteral("base_revision")).toInteger(), 1);
    QCOMPARE(operations.first().value(QStringLiteral("payload")).toObject(), withoutSkip);
}

void DocumentFeedbackTest::testReadingOverlayMerge()
{
    const QJsonObject record = overlayExample();
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-11-19T18:00:00.000Z"), Qt::ISODateWithMs);
    const QJsonObject withoutSkip = ReadingOverlays::withoutMark(record, QStringLiteral("m1"), QStringLiteral("mac-okular"), now);
    // Last revision wins, in both orders.
    QCOMPARE(ReadingOverlays::merge(record, withoutSkip), withoutSkip);
    QCOMPARE(ReadingOverlays::merge(withoutSkip, record), withoutSkip);
    // The agent's replacement and the user's Delete Mark of the same revision: the later write
    // wins; a delete beats an equal revision in every arrival order.
    QJsonObject replaced = record;
    replaced.insert(QStringLiteral("revision"), 2);
    replaced.insert(QStringLiteral("updated_at"), QStringLiteral("2026-11-19T17:45:00.000Z"));
    replaced.insert(QStringLiteral("title"), QStringLiteral("Revised"));
    QCOMPARE(ReadingOverlays::merge(replaced, withoutSkip), withoutSkip);
    QCOMPARE(ReadingOverlays::merge(withoutSkip, replaced), withoutSkip);
    const QJsonObject tombstone = ReadingOverlays::remove(record, QStringLiteral("mac-okular"), now.addSecs(-3600));
    QCOMPARE(ReadingOverlays::merge(replaced, tombstone), tombstone);
    QCOMPARE(ReadingOverlays::merge(tombstone, replaced), tombstone);
    QCOMPARE(ReadingOverlays::merge(tombstone, withoutSkip), tombstone);
    // overlay_id, document.document_id, author, created_at and created_origin_id never change; a
    // replacement on a newer PDF version changes the document's PDF and filename.
    QJsonObject rewritten = withoutSkip;
    rewritten.insert(QStringLiteral("revision"), 3);
    rewritten.insert(QStringLiteral("author"), QJsonObject{{QStringLiteral("agent"), QStringLiteral("codex")}});
    rewritten.insert(QStringLiteral("created_at"), QStringLiteral("2026-11-20T00:00:00.000Z"));
    rewritten.insert(QStringLiteral("created_origin_id"), QStringLiteral("elsewhere"));
    rewritten.insert(QStringLiteral("document"), QJsonObject{{QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                                             {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('d'))},
                                                             {QStringLiteral("filename"), QStringLiteral("renamed.pdf")}});
    const QJsonObject merged = ReadingOverlays::merge(withoutSkip, rewritten);
    QCOMPARE(merged.value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(merged.value(QStringLiteral("author")), record.value(QStringLiteral("author")));
    QCOMPARE(merged.value(QStringLiteral("created_at")), record.value(QStringLiteral("created_at")));
    QCOMPARE(merged.value(QStringLiteral("created_origin_id")), record.value(QStringLiteral("created_origin_id")));
    QCOMPARE(ReadingOverlays::documentIdOf(merged), documentId);
    QCOMPARE(ReadingOverlays::pdfSha256Of(merged), QString(64, QLatin1Char('d')));
    QCOMPARE(merged.value(QStringLiteral("document")).toObject().value(QStringLiteral("filename")).toString(), QStringLiteral("renamed.pdf"));
}

void DocumentFeedbackTest::testReadingOverlaysOfDocument()
{
    const QJsonObject older = overlayExample();
    QJsonObject newer = overlayExample();
    newer.insert(QStringLiteral("overlay_id"), QStringLiteral("0c1d2e3f-4a5b-4c6d-8e7f-8091a2b3c4d5"));
    newer.insert(QStringLiteral("created_at"), QStringLiteral("2026-11-19T18:30:00.000Z"));
    const QJsonObject deleted = ReadingOverlays::remove(
        [&] {
            QJsonObject record = overlayExample();
            record.insert(QStringLiteral("overlay_id"), QStringLiteral("9e8d7c6b-5a49-4382-a716-05f4e3d2c1b0"));
            return record;
        }(),
        QStringLiteral("mac-okular"));
    QJsonObject elsewhere = overlayExample();
    elsewhere.insert(QStringLiteral("overlay_id"), QStringLiteral("1a2b3c4d-5e6f-4a7b-8c9d-0e1f2a3b4c5d"));
    elsewhere.insert(QStringLiteral("document"), QJsonObject{{QStringLiteral("document_id"), QStringLiteral("0b9c8d7e-6f5a-4b3c-9d2e-1f0a9b8c7d6e")}});
    QJsonObject projections;
    for (const QJsonObject &record : {older, newer, deleted, elsewhere})
        projections.insert(ReadingOverlays::projectionKey(ReadingOverlays::overlayIdOf(record)), record);
    // Feedback of the same document is no overlay.
    projections.insert(DocumentFeedback::projectionKey(QStringLiteral("9d2f4a51-7c0e-4b8a-a6f1-3e5b2c7d8e90")), specExample());
    // Newest first.
    QCOMPARE(ReadingOverlays::recordsOfDocument(projections, documentId), (QList<QJsonObject>{newer, older}));
    QCOMPARE(ReadingOverlays::recordsOfDocument(projections, documentId.toUpper()).size(), 2);
    QCOMPARE(ReadingOverlays::recordsOfDocument(projections, documentId, true).size(), 3);
    QVERIFY(ReadingOverlays::recordsOfDocument(projections, QString()).isEmpty());
}

void DocumentFeedbackTest::testReadingOverlayMarksAreFoundAsFeedback()
{
    // A mark is found on the PDF on screen as document feedback is (§4): the same PDF draws its
    // regions as they are; another version searches its quote near its pages; no hit is not in
    // this version.
    QHash<int, QList<Glyph>> pages;
    for (int page = 1; page <= 10; ++page) pages.insert(page, line(page, QStringLiteral("page %1 words").arg(page), 0.1, 0.1));
    const auto pageText = [&](const QList<int> &wanted) {
        QList<QList<Glyph>> regions;
        for (const int page : wanted) regions.append(pages.value(page));
        return ReviewCardFeedback::answerText(regions);
    };
    const QJsonObject record = overlayExample();
    const QJsonObject skip = ReadingOverlays::passageOf(ReadingOverlays::markOf(record, QStringLiteral("m1")));
    const QJsonObject read = ReadingOverlays::passageOf(ReadingOverlays::markOf(record, QStringLiteral("m2")));
    const QJsonObject edited = ReadingOverlays::passageOf(ReadingOverlays::markOf(record, QStringLiteral("m3")));
    auto placement = DocumentFeedback::place(skip, pdfSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("exact"));
    QCOMPARE(placement.rects, ReviewCardFeedback::regionsOf(skip));
    const QString otherSha = QString(64, QLatin1Char('d'));
    placement = DocumentFeedback::place(read, otherSha, 10, pageText);
    QCOMPARE(placement.state, QStringLiteral("relocated"));
    QCOMPARE(placement.rects.size(), 1);
    QCOMPARE(placement.rects.first().first, 4);
    QCOMPARE(DocumentFeedback::place(edited, otherSha, 10, pageText).state, QStringLiteral("orphaned"));
    QCOMPARE(DocumentFeedback::place(skip, otherSha, 10, pageText).state, QStringLiteral("orphaned"));
}

QTEST_GUILESS_MAIN(DocumentFeedbackTest)
#include "documentfeedbacktest.moc"
