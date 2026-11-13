#include "../part/sourceannotationindex.h"
#include "../part/thoughtsplatform.h"
#include "../part/sourceowner.h"
#include "../part/projectconfiguration.h"
#include "../part/documentidentity.h"
#include "../part/docscommentsynchronizer.h"
/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>
#include <KZip>
#include <QTimeZone>
#include <QTemporaryDir>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QPdfWriter>
#include <QCryptographicHash>
#include <QPainter>
#include <poppler-qt6.h>
#include "../shell/reviewdocumenttools.h"
#include <QUuid>

#include <algorithm>
#include <iterator>

#include "../shell/reviewstore.h"
#include "../core/htmlattachment.h"
#include "../part/reviewcardfeedback.h"
#include "../part/feedbackversion.h"
#include "../part/documentfeedback.h"
#include "../shell/reviewtagplan.h"
#include "../shell/reviewinterval.h"
#include "../part/desktopsyncstore.h"

using namespace Thoughts;

namespace
{

QJsonObject reviewCard(const QString &cardId,
                       const QString &pdfStem,
                       const QString &pdfSha256,
                       const QString &status = QStringLiteral("approved"),
                       qint64 revision = 1)
{
    return {
        {QStringLiteral("schema_version"), 3},
        {QStringLiteral("card_type"), QStringLiteral("visual_region")},
        {QStringLiteral("card_id"), cardId},
        {QStringLiteral("status"), status},
        {QStringLiteral("revision"), revision},
        {QStringLiteral("primary"), QJsonObject{
             {QStringLiteral("pdf_stem"), pdfStem},
             {QStringLiteral("pdf_sha256"), pdfSha256},
             {QStringLiteral("page"), 1},
             {QStringLiteral("rect"), QJsonArray{0.1, 0.2, 0.8, 0.6}},
             {QStringLiteral("page_visual_sha256"), QString(64, QLatin1Char('b'))},
             {QStringLiteral("question"), QStringLiteral("What is hidden?")},
         }},
    };
}

bool writeObject(const QString &path, const QJsonObject &object)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly)
        && file.write(QJsonDocument(object).toJson(QJsonDocument::Compact)) > 0;
}

QJsonObject readObject(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject();
}

// A built TeX document as Sync installs it: a PDF whose pages show the given lines, its source,
// and a verified source bundle whose reverse-SyncTeX lookup maps each drawn line to its source
// line (0: drawn content that maps to no authored line, such as a generated table).
struct LinkedDocument {
    struct Line { QString text; int sourceLine = 0; double baseline = 0; };
    QString root, pdfPath, bundle, sourceId, sourcePath, pdfSha;
    QByteArray source;
    QVariantMap document;
    bool build(const QString &directory, const QByteArray &sourceText, const QList<QList<Line>> &pages,
               const QList<QPair<int, QRectF>> &drawings = {}, const QByteArray &builtSource = {})
    {
        root = directory;
        pdfPath = QDir(root).filePath(QStringLiteral("current_pdfs/paper.pdf"));
        bundle = QDir(root).filePath(QStringLiteral("bundle"));
        sourceId = QStringLiteral("docs-test/tex/source/main.tex");
        sourcePath = QDir(root).filePath(sourceId);
        source = sourceText;
        if (!QDir().mkpath(QFileInfo(pdfPath).absolutePath())) return false;
        {
            QPdfWriter writer(pdfPath);
            writer.setResolution(72);
            writer.setPageSize(QPageSize(QPageSize::A4));
            writer.setPageMargins(QMarginsF(0, 0, 0, 0));
            QPainter painter(&writer);
            for (int page = 0; page < pages.size(); ++page) {
                if (page) writer.newPage();
                for (const auto &line : pages[page]) painter.drawText(QPointF(30, line.baseline), line.text);
                for (const auto &drawing : drawings)
                    if (drawing.first == page + 1) painter.drawRect(drawing.second);
            }
        }
        const auto put = [](const QString &path, const QByteArray &bytes) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile f(path);
            return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        const auto digest = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
        if (!put(sourcePath, source)) return false;
        const QByteArray built = builtSource.isEmpty() ? source : builtSource;
        const QString sourceSha = digest(built);
        const QString lookupPath = QDir(root).filePath(QStringLiteral("lookup.zip"));
        {
            KZip zip(lookupPath);
            if (!zip.open(QIODevice::WriteOnly)) return false;
            zip.writeFile(QStringLiteral("format.json"), QByteArrayLiteral("{\"schema_version\":1,\"format\":\"thoughts-reverse-synctex\"}"));
            for (int page = 0; page < pages.size(); ++page) {
                QString entries;
                for (const auto &line : pages[page]) {
                    if (line.sourceLine > 0)
                        entries += QStringLiteral("h\t%1\t%2\t0\t%3\t600\t14\t4\n").arg(sourceId).arg(line.sourceLine).arg(line.baseline);
                }
                zip.writeFile(QStringLiteral("pages/%1.tsv").arg(page + 1), entries.toUtf8());
            }
            if (!zip.close()) return false;
        }
        QFile lookup(lookupPath);
        if (!lookup.open(QIODevice::ReadOnly)) return false;
        const auto lookupBytes = lookup.readAll();
        const auto lookupSha = digest(lookupBytes);
        QFile pdf(pdfPath);
        if (!pdf.open(QIODevice::ReadOnly)) return false;
        pdfSha = digest(pdf.readAll());
        if (!put(bundle + QStringLiteral("/objects/") + sourceSha, built) || !put(bundle + QStringLiteral("/objects/") + lookupSha, lookupBytes)) return false;
        const QJsonObject segment {{QStringLiteral("start_page"), 1}, {QStringLiteral("page_count"), int(pages.size())},
            {QStringLiteral("source_available"), true}, {QStringLiteral("lookup_format"), QStringLiteral("thoughts-reverse-synctex-v1")},
            {QStringLiteral("lookup_object"), lookupSha}, {QStringLiteral("lookup_sha256"), lookupSha},
            {QStringLiteral("inputs"), QJsonArray{QJsonObject{{QStringLiteral("synctex_path"), sourceId}, {QStringLiteral("source_file"), sourceId}, {QStringLiteral("source_sha256"), sourceSha}}}}};
        const QJsonObject manifest {{QStringLiteral("schema_version"), 1}, {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
            {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
            {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("pdf_sha256"), pdfSha},
                {QStringLiteral("page_count"), int(pages.size())}, {QStringLiteral("project"), QStringLiteral("docs-test")}, {QStringLiteral("segments"), QJsonArray{segment}}}},
            {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("path"), sourceId}, {QStringLiteral("sha256"), sourceSha}, {QStringLiteral("object"), sourceSha}}}}};
        const auto artifact = QJsonDocument(manifest).toJson();
        const auto artifactSha = digest(artifact);
        if (!put(bundle + QStringLiteral("/objects/") + artifactSha, artifact)) return false;
        if (!writeObject(bundle + QStringLiteral("/documents/paper.pdf.json"), {{QStringLiteral("artifact_sha256"), artifactSha}, {QStringLiteral("schema_version"), 1},
                {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact-pointer")}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                {QStringLiteral("pdf_sha256"), pdfSha}})) return false;
        document = prepareBasicReviewCards(pdfPath);
        document.insert(QStringLiteral("source_bundle_root"), bundle);
        return document.value(QStringLiteral("pdf_sha256")).toString() == pdfSha;
    }
    // A band around the line drawn at @p baseline, normalized to an A4 page.
    static QRectF band(double baseline, double left = 0.0, double right = 1.0)
    {
        return QRectF(QPointF(left, (baseline - 18) / 842.0), QPointF(right, (baseline + 8) / 842.0));
    }
    QByteArray current() const
    {
        QFile f(sourcePath);
        return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
    }
};

}

class ReviewStoreTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void oldWholeDocumentCardsAreStillRead();
    void contextNotesAreSharedNewestFirst();
    void contextNoteSectionFollowsTheOutline();
    void wholeDocumentCardsBecomeDocumentNotes();
    void reviewEventsRecordThePdfVersion();
    void agentVersionFeedbackKeepsOnlyTheVersionOnScreen();
    void cardWritesRecordThePdfVersion();
    void sourceSelectionCreationAndRetries();
    void pdfOnlyCardMovesIntoASourceDocumentWithItsHistory();
    void defaultFolderCardsStayInTheirProject();
    void sourceCommandsAcceptGeneratedCardIds();
    void overlappingMarkFiltersAndNeverIntervals();
    void bulkDeleteValidatesOnceAndRestoresHistory();
    void basicHalfPagesAreCompleteAndRepeatable();
    void basicSourcePlanKeepsCrossPagePassagesAndUsesLatestPdf();
    void basicCardsLimitLargeFigures();
    void projectCardsIgnoreOtherProjectsInvalidRecords();
    void phoneIntervalSuggestionsAndCalendarDates();
    void randomSkipDelayFollowsSharedFormula();
    void oneSuggestionFollowsTheLatestInterval();
    void tagsAreRecordedCountedAndMappedFromOlderAnswers();
    void currentTagsComeFromTheLatestAnswerInEffect();
    void tagMergesFollowAndroidMappingRule();
    void syncedTagMergesApplyWhereTagsAreRead();
    void tagPickerCountsAndOrdersTagsByUsage();
    void tagGroupsRecordsAreValidated();
    void tagGroupsShowMergedNamesAndOther();
    void tagGroupEditsKeepAValidWholeRecord();
    void tagGroupsSyncAsOneQueuedRecord();
    void tagGroupSlidersKeepTheirStepsAndUnknownFields();
    void tagPlanPreviewsAndAppliesAReorganization();
    void tagGroupSectionsSurviveEveryRewrite();
    void tagPlanSetsKeepsAndClearsSections();
    void tagGroupPhasesSurviveEveryRewrite();
    void tagPlanSetsKeepsAndClearsPhases();
    void groupNotesAreRecordedWithTheAnswer();
    void reviewPhasesAreRecordedWithTheAnswer();
    void desktopSkipScopesNotesAndOutbox();
    void unpublishedDocumentCardsAreNotOffered();
    void skipDueOnlyPreservesFutureCards();
    void multiPageAnswerRegions();
    void manualAnswerEditsPreserveSourcesAndHistory();
    void sourceScanReplacesLegacyRangesBeforePdfBuild();
    void severalPassagesAndOlderCardsStayAvailable();
    void schedulesRatings();
    void synchronizedSkipsPreserveSchedulingAndUndo();
    void androidExplicitChoicesRoundTrip();
    void retainedAndCustomEventsRespectRetractions();
    void explicitDatesRejectMalformedEvents();
    void intervalSelectionsChangeScheduleWithoutAddingReviews();
    void deleteIsTerminalAndEventsAreIdempotent();
    void ratingRetractionsRecomputeSchedule();
    void agentRequestCarriesGenerationOverrides();
    void importRespectsRequestedCandidateLimit();
    void importAcceptsVisualRegionsAcrossDocumentPages();
    void importAcceptsLargeAndRejectsDuplicateRegions();
    void importCreatesActiveCardsAndPersistsEdits();
    void manualCardIsApprovedDueAndRejectsDuplicates();
    void importedCardKeepsIdentityAcrossPublication();
    void manualDocumentSelectionInIsolatedCopy();
    void manualLinkingOutcomes_data();
    void manualLinkingOutcomes();
    void pdfOnlyCardReportsItsDocumentSourceState();
    void manualRepairKeepsIdentityAndFollowsSource();
    void manualRepairAcceptsMarkdownMarkers();
    void manualRepairAcceptsNestedLatexEntryPoint();
    void skipPostponesAndKeepsTheCardsTags();
    void failedCardLinksWhenSourceMarkersAppear();
    void localDeletionPreservesTextAndHidesPendingRemoval();
    void archivesLegacyReviewStore();
    void writesLocallyWithoutLegacyPhoneTransport();
    void recoversInterruptedSourceCardWithoutOverwritingLaterEdits();
    void fedoraProjectionIsAuthoritativeAndCanonical();
    void projectionCardMutationUsesServerRevision();
    void browserCardsSupportRevisionSafeEditDeleteAndRestore();
    void learningQueueUsesTwentyMinuteLookahead();
    void queryDatabaseIncludesSharedHistoryAndRetractions();
    void dailyQueueOrderingUsesUtcDateSeed();
    void skippedCardKeepsItsDueDateAndReturnsToItsPlace();
    void dueFromIsWhenIsDueNowStarts();
    void severalAreasOnTwoPagesMarkOneSegmentEach();
    void rectangleAroundFloatMarksTheWholeEnvironment();
    void unmappableAreaIsRefusedAndNothingIsWritten();
    void placingAgainMovesTheCardsMarkers();
    void deletingACardWithSeveralPassagesRemovesAllMarkers();
    void mergingCardsGivesTheKeptCardEveryPassage();
    void mergingCardsFollowsReadingOrderAndTheChosenCard();
    void mergingMarkdownCardsRelabelsTheirMarkers();
    void mergingCardsRefusesWhatItCannotMergeSafely();
    void interruptedMergeIsRecovered();
    void mergeCommandIsOneRequestWithAReceipt();
    void mergeOfARealSourceCopy();
    void cardWithoutLocalSourceMapWaitsForTheServer();
    void htmlCardHasNoPdfGeometryAndWaitsForTheServer();
    void htmlAttachmentCardIsFoundByItsText();
    void areaOfAProposalAcceptedSinceTheBuildIsPlacedByText();
    void wordOnTwoNearbyLinesIsPlacedByTheAreasText();
    void mathAreaIsPlacedByItsProse();
    void figureLabelsInAnAreaAreSkipped();
    void typographicQuotesMatchTheirTexSpelling();
    void retiredTexLinkTargetsAreNotText();
    void repeatedTextIsNotPlaced();
    void shortTextIsNotPlaced();
    void retireFoldMatchesTheConformanceCases();
    void retireAndRestoreKeepTheScheduleAndLeaveTheQueue();
    void retireUndoAndIndex();
};

void ReviewStoreTest::intervalSelectionsChangeScheduleWithoutAddingReviews()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + id,
                            reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')))}};
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime at(QDate(2026, 9, 24), QTime(16, 52), QTimeZone::UTC);
    QString error;
    QVERIFY2(store.recordRating(id, QStringLiteral("custom_interval"), at.addDays(-1), &error,
                                at.addDays(13), nullptr, QStringLiteral("correct")), qPrintable(error));
    auto card = store.cards().first();
    QCOMPARE(card.schedule.reviewCount, 1);
    QCOMPARE(card.currentMark(), QStringLiteral("correct"));

    QVERIFY2(store.setInterval(id, {3, ReviewInterval::Weeks, {}}, false, at, &error), qPrintable(error));
    card = store.cards().first();
    QCOMPARE(card.schedule.reviewCount, 1);
    QCOMPARE(card.schedule.dueAt, at.addDays(21));
    QCOMPARE(card.currentMark(), QStringLiteral("correct"));
    QCOMPARE(card.history.size(), 2);
    QCOMPARE(card.history.last().value(QStringLiteral("interval_action")).toString(), QStringLiteral("set"));
    QCOMPARE(card.history.last().value(QStringLiteral("interval_unit")).toString(), QStringLiteral("weeks"));
    QCOMPARE(card.history.last().value(QStringLiteral("interval_amount")).toDouble(), 3.0);

    QVERIFY2(store.setInterval(id, {}, true, at, &error), qPrintable(error));
    card = store.cards().first();
    QCOMPARE(card.schedule.reviewCount, 1);
    QVERIFY(card.schedule.retained);
    QCOMPARE(card.currentMark(), QStringLiteral("correct"));

    QVERIFY2(store.setInterval(id, {2, ReviewInterval::Days, {}}, false, at, &error), qPrintable(error));
    card = store.cards().first();
    QCOMPARE(card.schedule.reviewCount, 1);
    QVERIFY(!card.schedule.retained);
    QCOMPARE(card.schedule.dueAt, at.addMSecs(2).addDays(2));
    QCOMPARE(card.currentMark(), QStringLiteral("correct"));
    QCOMPARE(card.history.size(), 4);
}

void ReviewStoreTest::projectCardsIgnoreOtherProjectsInvalidRecords()
{
    QTemporaryDir root;
    const QString alpha = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString beta = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString event = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projections;
    projections.insert(QStringLiteral("review_card\ncards\n") + alpha, reviewCard(alpha, QStringLiteral("alpha"), QString(64, QLatin1Char('a'))));
    QJsonObject broken = reviewCard(beta, QStringLiteral("beta"), QString(64, QLatin1Char('b')));
    broken.insert(QStringLiteral("schema_version"), -1);
    projections.insert(QStringLiteral("review_card\ncards\n") + beta, broken);
    projections.insert(QStringLiteral("review_event\nevents\n") + event,
        QJsonObject{{QStringLiteral("event_id"), event}, {QStringLiteral("card_id"), beta}, {QStringLiteral("rating"), QStringLiteral("invalid")}});
    ReviewStore store(root.path(), [&] { return projections; });
    QStringList errors;
    const auto cards = store.cards(&errors, QStringLiteral("alpha"));
    QCOMPARE(cards.size(), 1);
    QCOMPARE(cards.first().id(), alpha);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char(';'))));
}

// Skip's Random, identical on the phone: 2 days + Exponential(ln(20)/12 per day), so 95% within 14 days.
void ReviewStoreTest::randomSkipDelayFollowsSharedFormula()
{
    QCOMPARE(randomSkipDelayDays(0), 2.0);
    QVERIFY(qAbs(randomSkipDelayDays(0.95) - 14.0) < 1e-12);
    QVERIFY(qAbs(randomSkipDelayDays(0.5) - (2.0 + 12.0 * std::log(2.0) / std::log(20.0))) < 1e-12);
    // Never below the 2-day minimum, increasing in u, and invalid input falls back to the minimum.
    double previous = 0;
    for (double u = 0; u < 1; u += 0.01) {
        const double days = randomSkipDelayDays(u);
        QVERIFY(days >= 2.0);
        QVERIFY(days >= previous);
        previous = days;
    }
    QCOMPARE(randomSkipDelayDays(-0.5), 2.0);
    QCOMPARE(randomSkipDelayDays(1.0), 2.0);
    QCOMPARE(randomSkipDelayDays(std::nan("")), 2.0);
    // The dialog's interval is the delay rounded to the nearest whole day, at least 2.
    QCOMPARE(randomSkipDays(0), 2);
    QCOMPARE(randomSkipDays(0.95), 14);
    const auto uFor = [](double days) { return 1.0 - std::exp(-(days - 2.0) * std::log(20.0) / 12.0); };
    QCOMPARE(randomSkipDays(uFor(2.49)), 2);
    QCOMPARE(randomSkipDays(uFor(2.51)), 3);
    QCOMPARE(randomSkipDays(uFor(6.4)), 6);
    QCOMPARE(randomSkipDays(uFor(6.6)), 7);
    QCOMPARE(randomSkipDays(uFor(30.2)), 30);
    QCOMPARE(randomSkipDays(1.0), 2);
    QCOMPARE(randomSkipDays(std::nan("")), 2);
    // The default source draws from [0, 1).
    for (int i = 0; i < 100; ++i) {
        const double u = randomSkipUniformSource()();
        QVERIFY(u >= 0 && u < 1);
    }
}

void ReviewStoreTest::phoneIntervalSuggestionsAndCalendarDates()
{
    const QTimeZone paris("Europe/Paris");
    const QDateTime now(QDate(2026, 3, 28), QTime(12, 0), paris);
    QCOMPARE(ReviewInterval::suggested({}, {}).amount, 2.0);
    QCOMPARE(ReviewInterval::suggested({}, {}).unit, ReviewInterval::Weeks);
    const ReviewInterval interval{2.5, ReviewInterval::Weeks, {}};
    const auto due = interval.dueAt(now, paris);
    QCOMPARE(due.toTimeZone(paris).date(), QDate(2026, 4, 15));
    QCOMPARE(due.toTimeZone(paris).time(), QTime(0, 0));
    const auto next = ReviewInterval::suggested(now, due, paris);
    QCOMPARE(next.amount, 5.0);
    QCOMPARE(next.unit, ReviewInterval::Weeks);
    const ReviewInterval twoDays{2, ReviewInterval::Days, {}};
    QCOMPARE(twoDays.dueAt(now, paris).toTimeZone(paris), QDateTime(QDate(2026, 3, 30), QTime(12, 0), paris));
    QCOMPARE(now.secsTo(twoDays.dueAt(now, paris)), 47 * 3600);
    QCOMPARE(ReviewInterval::suggested(now, twoDays.dueAt(now, paris), paris).amount, 4.0);
    const QDateTime autumn(QDate(2026, 10, 24), QTime(12, 0), paris);
    QCOMPARE(autumn.secsTo(twoDays.dueAt(autumn, paris)), 49 * 3600);
    QCOMPARE(ReviewInterval::suggested(autumn, twoDays.dueAt(autumn, paris), paris).amount, 4.0);
    const ReviewInterval month{1.5, ReviewInterval::Months, {}};
    const QDateTime january(QDate(2026, 1, 31), QTime(10, 0), QTimeZone::UTC);
    QCOMPARE(month.dueAt(january, QTimeZone::UTC), QDateTime(QDate(2026, 3, 15), QTime(22, 0), QTimeZone::UTC));
    const ReviewInterval minutes{10, ReviewInterval::Minutes, {}};
    QCOMPARE(minutes.dueAt(now, paris), now.addSecs(600).toUTC());
    const auto twiceMinutes = ReviewInterval::suggested(now, minutes.dueAt(now, paris), paris);
    QCOMPARE(twiceMinutes.amount, 20.0);
    QCOMPARE(twiceMinutes.unit, ReviewInterval::Minutes);
    // From two days on an interval is whole days, or weeks when it is a whole number of them: 419
    // hours, as a card had after its clocks changed, is 17 days and suggests 5 weeks, not 838 h.
    const QDateTime reviewed(QDate(2026, 10, 7), QTime(16, 52), paris);
    const QDateTime hoursLater = reviewed.addSecs(420 * 3600);
    QCOMPARE(ReviewInterval::between(reviewed, hoursLater, paris).amount, 17.0);
    QCOMPARE(ReviewInterval::between(reviewed, hoursLater, paris).unit, ReviewInterval::Days);
    QCOMPARE(ReviewInterval::suggested(reviewed, hoursLater, paris).amount, 5.0);
    QCOMPARE(ReviewInterval::suggested(reviewed, hoursLater, paris).unit, ReviewInterval::Weeks);
    QCOMPARE(ReviewInterval::readable(3673 * 3600000.0).amount, 153.0);
    QCOMPARE(ReviewInterval::readable(3673 * 3600000.0).unit, ReviewInterval::Days);
    // Below two days it stays exact.
    QCOMPARE(ReviewInterval::readable(30 * 3600000.0).amount, 30.0);
    QCOMPARE(ReviewInterval::readable(30 * 3600000.0).unit, ReviewInterval::Hours);
    const ReviewInterval date{1, ReviewInterval::Days, QDate(2026, 3, 30)};
    QCOMPARE(date.dueAt(now, paris).toTimeZone(paris).time(), QTime(12, 0));
    for (double invalid : {0.0, -1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        const ReviewInterval bad{invalid, ReviewInterval::Days, {}};
        QVERIFY(!bad.dueAt(now, paris).isValid());
    }
}

void ReviewStoreTest::unpublishedDocumentCardsAreNotOffered()
{
    // protocol/project-document-publication.md §4: kept with its schedule, offered for nothing.
    QTemporaryDir root;
    QTemporaryDir sync;
    QVERIFY(root.isValid() && sync.isValid());
    const QString hash(64, QLatin1Char('a'));
    const auto now = QDateTime(QDate(2026, 9, 9), QTime(10, 0), QTimeZone::UTC);
    QHash<QString, QString> cardOfDocument;
    for (const QString &document : {QStringLiteral("shown"), QStringLiteral("hidden")}) {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        cardOfDocument.insert(document, id);
        auto card = reviewCard(id, document, hash);
        auto primary = card.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("document_id"), QUuid::createUuidV5(QUuid(), document).toString(QUuid::WithoutBraces));
        card.insert(QStringLiteral("primary"), primary);
        QVERIFY(writeObject(root.path() + QStringLiteral("/reviews/cards/") + id + QStringLiteral(".json"), card));
    }
    auto configuration = ProjectConfiguration::empty(root.path(), QStringLiteral("reading"), QStringLiteral("Reading"));
    const auto folder = configuration.value(QStringLiteral("default_folder_id")).toString();
    const auto declare = [&](const QString &document, bool included) {
        const QString id = QUuid::createUuidV5(QUuid(), document).toString(QUuid::WithoutBraces);
        return QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), included},
                           {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), QString(document + QStringLiteral(".pdf"))}}}};
    };
    const auto save = [&](bool hiddenPublished) {
        configuration.insert(QStringLiteral("documents"), QJsonArray{declare(QStringLiteral("shown"), true), declare(QStringLiteral("hidden"), hiddenPublished)});
        return ProjectConfiguration::save(ProjectConfiguration::path(root.path(), QStringLiteral("reading")), configuration);
    };
    QVERIFY(save(false).isEmpty());
    ReviewStore store(root.path(), {}, sync.path());
    const auto due = [&] {
        QSet<QString> ids;
        for (const auto &card : store.dueCards(now)) ids.insert(card.id());
        return ids;
    };
    QCOMPARE(due(), QSet<QString>{cardOfDocument.value(QStringLiteral("shown"))});
    QCOMPARE(store.cards().size(), 2);
    // In the browser it is Unpublished: listed with the retired cards, never as unavailable.
    const auto browsed = [&](const QString &document) {
        for (const auto &item : store.browserCards(now))
            if (item.card.id() == cardOfDocument.value(document)) return item;
        return ReviewBrowserCard{};
    };
    QCOMPARE(browsed(QStringLiteral("hidden")).state, ReviewBrowserState::Unpublished);
    QVERIFY(!browsed(QStringLiteral("hidden")).hasIssues);
    QVERIFY(browsed(QStringLiteral("hidden")).matchesFilter(QStringLiteral("retired")));
    QVERIFY(!browsed(QStringLiteral("hidden")).matchesFilter(QStringLiteral("unavailable")));
    QVERIFY(!browsed(QStringLiteral("hidden")).canReview());
    QVERIFY(browsed(QStringLiteral("shown")).state != ReviewBrowserState::Unpublished);
    QVERIFY(!browsed(QStringLiteral("shown")).matchesFilter(QStringLiteral("retired")));
    QVERIFY(save(true).isEmpty());
    QCOMPARE(due(), QSet<QString>(cardOfDocument.cbegin(), cardOfDocument.cend()));
    QVERIFY(browsed(QStringLiteral("hidden")).state != ReviewBrowserState::Unpublished);
}

void ReviewStoreTest::desktopSkipScopesNotesAndOutbox()
{
    QTemporaryDir root;
    QTemporaryDir sync;
    QVERIFY(root.isValid() && sync.isValid());
    const QString hash(64, QLatin1Char('a'));
    const auto now = QDateTime(QDate(2026, 9, 9), QTime(10, 0), QTimeZone::UTC);
    QStringList ids;
    for (const QString &stem : {QStringLiteral("paper"), QStringLiteral("paper"), QStringLiteral("appendix"), QStringLiteral("other")}) {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ids.append(id);
        QVERIFY(writeObject(root.path() + QStringLiteral("/reviews/cards/") + id + QStringLiteral(".json"), reviewCard(id, stem, hash)));
    }
    ReviewStore store(root.path(), {}, sync.path());
    QVERIFY(writeObject(root.path() + QStringLiteral("/current_pdfs/.thoughts-source-index.json"),
                        {{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray{
                            QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("project"), QStringLiteral("A")}},
                            QJsonObject{{QStringLiteral("filename"), QStringLiteral("appendix.pdf")}, {QStringLiteral("project"), QStringLiteral("A")}},
                            QJsonObject{{QStringLiteral("filename"), QStringLiteral("other.pdf")}, {QStringLiteral("project"), QStringLiteral("B")}}}}}));
    QVERIFY(store.recordRating(ids.at(1), QStringLiteral("retained"), now));
    ReviewSkipBatch project;
    QString error;
    QVERIFY2(store.skip(ids.first(), ReviewSkipScope::Project, now.addDays(1), &project, &error, now), qPrintable(error));
    QCOMPARE(QSet<QString>(project.cardIds.begin(), project.cardIds.end()), QSet<QString>({ids.at(0), ids.at(2)}));
    QCOMPARE(store.dueCards(now).size(), 1);
    QCOMPARE(store.dueCards(now).first().id(), ids.at(3));
    ReviewSkipBatch overlapping;
    QVERIFY(store.skip(ids.first(), ReviewSkipScope::Card, now.addDays(2), &overlapping, &error, now));
    QVERIFY(store.undoSkip(project.id, &error, now.addSecs(1)));
    QCOMPARE(store.dueCards(now.addSecs(2)).size(), 2);
    QVERIFY(store.undoSkip(overlapping.id, &error, now.addSecs(3)));
    QCOMPARE(store.dueCards(now.addSecs(4)).size(), 3);
    ReviewSkipBatch document;
    QVERIFY(store.skip(ids.first(), ReviewSkipScope::Document, now.addSecs(600), &document, &error, now));
    QCOMPARE(document.cardIds, QStringList{ids.first()});
    QCOMPARE(store.dueCards(now.addSecs(599)).size(), 2);
    QCOMPARE(store.dueCards(now.addSecs(600)).size(), 3);
    QVERIFY(!store.skip(ids.first(), ReviewSkipScope::Card, now, nullptr, &error, now));
    QFile::remove(root.path() + QStringLiteral("/current_pdfs/.thoughts-source-index.json"));
    QVERIFY(!store.skip(ids.first(), ReviewSkipScope::Project, now.addDays(1), nullptr, &error, now));
    QVERIFY(error.contains(QStringLiteral("Project information")));
    const auto allCards = store.cards();
    const auto card = *std::find_if(allCards.cbegin(), allCards.cend(), [&](const auto &item) { return item.id() == ids.first(); });
    QCOMPARE(card.schedule.reviewCount, 0);
    QVERIFY(store.updateNote(card, QStringLiteral("  Remember this distinction  "), &error));
    QVERIFY(!store.updateNote(card, QStringLiteral("Stale overwrite"), &error));
    const auto updated = store.cards();
    const auto annotated = *std::find_if(updated.cbegin(), updated.cend(), [&](const auto &item) { return item.id() == ids.first(); });
    QCOMPARE(annotated.record.value(QStringLiteral("primary")), card.record.value(QStringLiteral("primary")));
    QCOMPARE(annotated.record.value(QStringLiteral("note")).toString(), QStringLiteral("Remember this distinction"));
    QVERIFY(!store.updateNote(annotated, QString(5001, QLatin1Char('x')), &error));
    QVERIFY(store.updateNote(annotated, QString(), &error));
    const auto clearedCards = store.cards();
    const auto cleared = *std::find_if(clearedCards.cbegin(), clearedCards.cend(), [&](const auto &item) { return item.id() == ids.first(); });
    QCOMPARE(cleared.record.value(QStringLiteral("note")).toString(), QString());
    QCOMPARE(cleared.record.value(QStringLiteral("primary")), card.record.value(QStringLiteral("primary")));
    QCOMPARE(cleared.schedule.reviewCount, card.schedule.reviewCount);
    DesktopSyncOutbox outbox(sync.path() + QStringLiteral("/desktop-sync.sqlite"));
    const auto operations = outbox.operations();
    QVERIFY(!operations.isEmpty());
    QList<QJsonObject> exportedQueue;
    for (const auto &operation : operations) {
        const auto payload = operation.value(QStringLiteral("payload")).toObject();
        if (!payload.contains(QStringLiteral("queue_action"))) continue;
        QCOMPARE(operation.value(QStringLiteral("entity_type")).toString(), QStringLiteral("review_event"));
        QCOMPARE(payload.value(QStringLiteral("schema_version")).toInt(), 3);
        QVERIFY(!payload.contains(QStringLiteral("rating")));
        QVERIFY(!payload.contains(QStringLiteral("scheduled_due_at")));
        exportedQueue.append(payload);
    }
    QCOMPARE(exportedQueue.size(), 7);
    QVERIFY(ReviewStore::foldEvents(exportedQueue).isEmpty());
    // A fresh store reads the same history and expired skips after restart.
    ReviewStore reopened(root.path(), {}, sync.path());
    QCOMPARE(reopened.dueCards(now.addSecs(600)).size(), 3);
    const auto reopenedCards = reopened.cards();
    const auto historyCard = *std::find_if(reopenedCards.cbegin(), reopenedCards.cend(), [&](const auto &item) { return item.id() == ids.first(); });
    QCOMPARE(historyCard.history.size(), 5);
    QCOMPARE(historyCard.schedule.reviewCount, 0);
    QCOMPARE(historyCard.schedule.lastRating, QString());
    QCOMPARE(historyCard.history.last().value(QStringLiteral("queue_action")).toString(), QStringLiteral("undo_skip"));
    int skips = 0;
    for (const auto &event : historyCard.history) {
        if (event.value(QStringLiteral("queue_action")).toString() != QLatin1String("skip")) continue;
        ++skips;
        QVERIFY(QDateTime::fromString(event.value(QStringLiteral("skip_until")).toString(), Qt::ISODate).isValid());
    }
    QCOMPARE(skips, 3);
    const QString exportPath = qEnvironmentVariable("THOUGHTS_REVIEW_PARITY_OPERATIONS");
    if (!exportPath.isEmpty()) {
        QVERIFY(store.recordRating(ids.at(3), QStringLiteral("custom_interval"), now, &error, now.addDays(14)));
        QVERIFY(store.recordRating(ids.at(0), QStringLiteral("ten_minutes"), now, &error));
        QJsonArray exported;
        for (const auto &operation : outbox.operations()) exported.append(operation);
        QVERIFY(writeObject(exportPath, {{QStringLiteral("operations"), exported}}));
    }
}

void ReviewStoreTest::skipDueOnlyPreservesFutureCards()
{
    QTemporaryDir root;
    QTemporaryDir sync;
    QVERIFY(root.isValid() && sync.isValid());
    const QString hash(64, QLatin1Char('a'));
    const auto now = QDateTime(QDate(2026, 9, 9), QTime(10, 0), QTimeZone::UTC);
    QStringList ids;
    for (const QString &stem : {QStringLiteral("paper"), QStringLiteral("paper"), QStringLiteral("appendix"), QStringLiteral("other")}) {
        const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ids.append(id);
        QVERIFY(writeObject(root.path() + QStringLiteral("/reviews/cards/") + id + QStringLiteral(".json"), reviewCard(id, stem, hash)));
    }
    ReviewStore store(root.path(), {}, sync.path());
    QVERIFY(writeObject(root.path() + QStringLiteral("/current_pdfs/.thoughts-source-index.json"),
                        {{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray{
                            QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("project"), QStringLiteral("A")}},
                            QJsonObject{{QStringLiteral("filename"), QStringLiteral("appendix.pdf")}, {QStringLiteral("project"), QStringLiteral("A")}},
                            QJsonObject{{QStringLiteral("filename"), QStringLiteral("other.pdf")}, {QStringLiteral("project"), QStringLiteral("B")}}}}}));
    QString error;
    QVERIFY(store.recordRating(ids.at(1), QStringLiteral("retained"), now));
    // A card due in five minutes is untouched by due-only, but hidden by all-cards.
    const auto later = now;
    QVERIFY(store.recordRating(ids.at(2), QStringLiteral("custom_interval"), later, &error, later.addSecs(300)));
    ReviewSkipBatch dueOnly;
    QVERIFY(store.skip(ids.first(), ReviewSkipScope::Project, later.addSecs(600), &dueOnly, &error, later, true));
    QCOMPARE(dueOnly.cardIds, QStringList{ids.first()});
    const auto dueAtFive = store.dueCards(later.addSecs(300));
    QVERIFY(std::any_of(dueAtFive.cbegin(), dueAtFive.cend(), [&](const auto &card) { return card.id() == ids.at(2); }));
    QVERIFY(store.undoSkip(dueOnly.id, &error, later));
    ReviewSkipBatch all;
    QVERIFY(store.skip(ids.first(), ReviewSkipScope::Project, later.addSecs(600), &all, &error, later, false));
    QVERIFY(all.cardIds.contains(ids.at(2)));
    const auto hiddenAtFive = store.dueCards(later.addSecs(300));
    QVERIFY(std::none_of(hiddenAtFive.cbegin(), hiddenAtFive.cend(), [&](const auto &card) { return card.id() == ids.at(2); }));
    const auto shownAtTen = store.dueCards(later.addSecs(600));
    QVERIFY(std::any_of(shownAtTen.cbegin(), shownAtTen.cend(), [&](const auto &card) { return card.id() == ids.at(2); }));
    QVERIFY(store.undoSkip(all.id, &error, later));
}

void ReviewStoreTest::manualAnswerEditsPreserveSourcesAndHistory()
{
    QTemporaryDir root;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString hash(64, QLatin1Char('a'));
    auto record = reviewCard(id, QStringLiteral("manual"), hash);
    record.insert(QStringLiteral("schema_version"), 4);
    auto primary = record.value(QStringLiteral("primary")).toObject();
    const QJsonObject anchor{{QStringLiteral("version"), 2}, {QStringLiteral("kind"), QStringLiteral("source_markers")}};
    const QJsonArray original{{QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QJsonArray{.1, .2, .5, .4}}}},
                              {QJsonObject{{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QJsonArray{.1, .1, .7, .3}}}}};
    primary.insert(QStringLiteral("regions"), original);
    primary.insert(QStringLiteral("source_anchor"), anchor);
    record.insert(QStringLiteral("primary"), primary);
    const QString path = QDir(root.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(id));
    QVERIFY(writeObject(path, record));
    ReviewStore store(root.path());
    QString error;
    QVERIFY(store.recordRating(id, QStringLiteral("good"), QDateTime::currentDateTimeUtc(), &error));
    const auto before = store.cards().first();
    const QVariantList edited{QVariantMap{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QVariantList{.15, .2, .55, .5}}},
                              QVariantMap{{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QVariantList{.1, .1, .7, .3}}}};
    QVERIFY2(store.updateCardAnswer(before, edited, QStringLiteral("  Revised question?  "), 2, &error), qPrintable(error));
    const auto saved = ReviewStore(root.path()).cards().first();
    QCOMPARE(saved.id(), id);
    QCOMPARE(saved.question(), QStringLiteral("Revised question?"));
    QCOMPARE(saved.regions(), edited);
    QCOMPARE(saved.schedule.reviewCount, before.schedule.reviewCount);
    QCOMPARE(saved.schedule.dueAt, before.schedule.dueAt);
    QCOMPARE(saved.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("regions")).toArray(), original);
    QCOMPARE(saved.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject(), anchor);
    QVERIFY(!store.updateCardAnswer(before, edited, QStringLiteral("Stale overwrite"), 2, &error));
    QVERIFY(!store.updateCardAnswer(saved, {}, saved.question(), 2, &error));
    QVERIFY(!store.updateCardAnswer(saved, edited, QString(), 2, &error));
    QVERIFY(!store.updateCardAnswer(saved, edited, saved.question(), 1, &error));
    QCOMPARE(store.cards().first().record, saved.record);
    auto rebuilt = saved.record;
    auto rebuiltPrimary = rebuilt.value(QStringLiteral("primary")).toObject();
    rebuiltPrimary.insert(QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('c')));
    rebuilt.insert(QStringLiteral("primary"), rebuiltPrimary);
    QVERIFY(writeObject(path, rebuilt));
    QCOMPARE(store.cards().first().regions(), original.toVariantList()); // Never reuse edited coordinates on another PDF revision.
}

void ReviewStoreTest::sourceScanReplacesLegacyRangesBeforePdfBuild()
{
    QTemporaryDir root;
    QTemporaryDir sync;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    const QString path = root.filePath(relative);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    const QByteArray text = QStringLiteral("\\ReviewCardMaskStart{%1}{1}%\nAnswer.\n\\ReviewCardMaskEnd{%1}{1}%\n").arg(id).toUtf8();
    QCOMPARE(source.write(text), text.size());
    source.close();
    auto card = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    auto primary = card.value(QStringLiteral("primary")).toObject();
    QJsonArray oldSegments;
    for (int index = 0; index < 4; ++index) oldSegments.append(QJsonObject{
        {QStringLiteral("segment_id"), QString::number(index)},
        {QStringLiteral("source_file"), relative},
        {QStringLiteral("content_sha256"), QString(64, QLatin1Char('f'))}});
    primary.insert(QStringLiteral("source_anchor"), QJsonObject{{QStringLiteral("version"), 2},
        {QStringLiteral("kind"), QStringLiteral("source_markers")}, {QStringLiteral("segments"), oldSegments}});
    card.insert(QStringLiteral("primary"), primary);
    QVERIFY(writeObject(root.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), card));
    auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(path, {});
    QVERIFY(scanned.value(QStringLiteral("valid")).toBool());
    scanned.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
    const QJsonObject files{{relative, scanned}};
    Thoughts::installSourceLocationIndex(root.path(), {{QStringLiteral("schema_version"), 1},
        {QStringLiteral("files"), files}, {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, files);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + id, card}};
    ReviewStore store(root.path(), [&projection]() { return projection; }, sync.path());
    const auto current = store.cards();
    QCOMPARE(current.size(), 1);
    const auto saved = current.first().record;
    const auto segments = saved.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject()
        .value(QStringLiteral("segments")).toArray();
    QCOMPARE(segments.size(), 1);
    QCOMPARE(segments.first().toObject().value(QStringLiteral("segment_id")).toString(), QStringLiteral("1"));
    QCOMPARE(saved.value(QStringLiteral("primary")).toObject().value(QStringLiteral("pdf_sha256")).toString(), QString(64, QLatin1Char('a')));
    QCOMPARE(saved.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("needs_attention"));
    QCOMPARE(store.cards().first().record.value(QStringLiteral("revision")).toInt(), saved.value(QStringLiteral("revision")).toInt());
    DesktopSyncOutbox outbox(sync.filePath(QStringLiteral("desktop-sync.sqlite")));
    QCOMPARE(outbox.operations().size(), 0);
    QCOMPARE(projection.value(QStringLiteral("review_card\ncards\n") + id).toObject(), card);
}

void ReviewStoreTest::severalPassagesAndOlderCardsStayAvailable()
{
    QTemporaryDir root;
    QTemporaryDir sync;
    const QString several = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString older = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    const QString path = root.filePath(relative);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    // The phone marks separate areas as segments of one card; file order need not follow segment ids.
    const QByteArray text = QStringLiteral(
        "\\ReviewCardMaskStart{%1}{1}%\nSecond passage.\n\\ReviewCardMaskEnd{%1}{1}%\n"
        "Between.\n"
        "\\ReviewCardMaskStart{%1}{0}%\nFirst passage.\n\\ReviewCardMaskEnd{%1}{0}%\n"
        "\\ReviewCardMaskStart{%2}{0}%\nOlder answer.\n\\ReviewCardMaskEnd{%2}{0}%\n").arg(several, older).toUtf8();
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(text), text.size());
    source.close();
    auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(path, {});
    QVERIFY(scanned.value(QStringLiteral("valid")).toBool());
    const auto reviews = scanned.value(QStringLiteral("reviews")).toObject();
    QCOMPARE(reviews.value(several).toArray().size(), 2);
    const auto contentOf = [&reviews](const QString &id, const QString &segment) {
        for (const auto &value : reviews.value(id).toArray())
            if (value.toObject().value(QStringLiteral("segment_id")).toString() == segment)
                return value.toObject().value(QStringLiteral("content_sha256")).toString();
        return QString();
    };
    const auto withAnchor = [&](const QString &id, const QJsonArray &segments) {
        auto card = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
        auto primary = card.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("source_anchor"), QJsonObject{{QStringLiteral("version"), 2}, {QStringLiteral("segments"), segments}});
        card.insert(QStringLiteral("primary"), primary);
        return card;
    };
    const auto segment = [&relative](const QString &content, const QString &id) {
        QJsonObject value{{QStringLiteral("source_file"), relative}, {QStringLiteral("content_sha256"), content}};
        if (!id.isEmpty()) value.insert(QStringLiteral("segment_id"), id);
        return value;
    };
    const auto severalCard = withAnchor(several, {segment(contentOf(several, QStringLiteral("0")), QStringLiteral("0")),
                                                  segment(contentOf(several, QStringLiteral("1")), QStringLiteral("1"))});
    // Some older cards were saved without a segment id.
    const auto olderCard = withAnchor(older, {segment(contentOf(older, QStringLiteral("0")), QString())});
    scanned.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
    const QJsonObject files{{relative, scanned}};
    Thoughts::installSourceLocationIndex(root.path(), {{QStringLiteral("schema_version"), 1},
        {QStringLiteral("files"), files}, {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, files);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + several, severalCard},
                           {QStringLiteral("review_card\ncards\n") + older, olderCard}};
    ReviewStore store(root.path(), [&projection]() { return projection; }, sync.path());
    const auto cards = store.cards();
    QCOMPARE(cards.size(), 2);
    for (const auto &card : cards) {
        QCOMPARE(card.sourceLocation.value(QStringLiteral("state")).toString(), QStringLiteral("linked"));
        QVERIFY2(card.record.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString() != QLatin1String("needs_attention"),
                 qPrintable(card.record.value(QStringLiteral("card_id")).toString()));
        const auto segments = card.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject()
            .value(QStringLiteral("segments")).toArray();
        if (card.record.value(QStringLiteral("card_id")).toString() == several) {
            QCOMPARE(segments.size(), 2);
            QCOMPARE(segments[0].toObject().value(QStringLiteral("segment_id")).toString(), QStringLiteral("0"));
            QCOMPARE(segments[1].toObject().value(QStringLiteral("segment_id")).toString(), QStringLiteral("1"));
        }
    }
}

void ReviewStoreTest::multiPageAnswerRegions()
{
    ReviewCard card{reviewCard(QUuid::createUuid().toString(QUuid::WithoutBraces), QStringLiteral("paper"), QString(64, QLatin1Char('a'))), {}};
    card.record.insert(QStringLiteral("schema_version"), 4);
    QCOMPARE(card.regions().size(), 1);
    QJsonObject primary = card.record.value(QStringLiteral("primary")).toObject();
    const QJsonArray regions{
        QJsonObject{{QStringLiteral("page"), 7}, {QStringLiteral("rect"), QJsonArray{.1, .8, .8, .9}}},
        QJsonObject{{QStringLiteral("page"), 8}, {QStringLiteral("rect"), QJsonArray{.1, .1, .8, .2}}},
    };
    primary.insert(QStringLiteral("regions"), regions);
    card.record.insert(QStringLiteral("primary"), primary);
    QCOMPARE(card.regions().size(), 2);
    QCOMPARE(card.pagesLabel(), QStringLiteral("7, 8"));
    QCOMPARE(card.regions().at(1).toMap().value(QStringLiteral("page")).toInt(), 8);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.path() + QStringLiteral("/reviews/cards/") + card.id() + QStringLiteral(".json");
    QVERIFY(writeObject(path, card.record));
    ReviewStore store(directory.path());
    QCOMPARE(store.cards().size(), 1);
    QVERIFY(QFileInfo::exists(path));
    // One bad continuation invalidates the whole answer; never display a partial mask.
    QJsonArray invalid = regions;
    invalid.append(QJsonObject{{QStringLiteral("page"), 0}, {QStringLiteral("rect"), QJsonArray{.1, .1, .8, .2}}});
    primary.insert(QStringLiteral("regions"), invalid);
    card.record.insert(QStringLiteral("primary"), primary);
    QVERIFY(card.regions().isEmpty());
    primary.insert(QStringLiteral("regions"), QJsonArray{});
    card.record.insert(QStringLiteral("primary"), primary);
    QVERIFY(card.regions().isEmpty());
}

void ReviewStoreTest::synchronizedSkipsPreserveSchedulingAndUndo()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString cardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString otherId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString hash(64, QLatin1Char('a'));
    const QDateTime now(QDate(2026, 9, 8), QTime(10, 0), QTimeZone::UTC);
    const QString batch = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString overlap = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projections;
    projections.insert(QStringLiteral("review_card\ncards\n") + cardId, reviewCard(cardId, QStringLiteral("paper"), hash));
    projections.insert(QStringLiteral("review_card\ncards\n") + otherId, reviewCard(otherId, QStringLiteral("paper"), hash));
    QVERIFY(writeObject(QDir(root.path()).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")),
                        {{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray{
                            QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("pdf_sha256"), hash}}}}}));
    ReviewStore store(root.path(), [&projections] { return projections; });
    const auto addAction = [&](const QString &id, const QString &skipId, const QString &action, const QDateTime &until) {
        const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QJsonObject event{{QStringLiteral("schema_version"), 3}, {QStringLiteral("event_id"), eventId},
                               {QStringLiteral("card_id"), id}, {QStringLiteral("skip_id"), skipId},
                               {QStringLiteral("queue_action"), action}, {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)},
                               {QStringLiteral("skip_until"), until.toString(Qt::ISODate)}};
        projections.insert(QStringLiteral("review_event\nevents\n") + eventId, event);
        return event;
    };
    QCOMPARE(store.dueCards(now).size(), 2);
    const QJsonObject skip = addAction(cardId, batch, QStringLiteral("skip"), now.addDays(1));
    QStringList errors;
    QVERIFY(ReviewStore::foldEvents({skip}, &errors).isEmpty());
    QVERIFY(errors.isEmpty());
    QCOMPARE(store.dueCards(now).size(), 1);
    QCOMPARE(store.dueCards(now).first().id(), otherId);
    const auto findCard = [&] {
        const auto rows = store.browserCards(now);
        return *std::find_if(rows.cbegin(), rows.cend(), [&](const ReviewBrowserCard &row) { return row.card.id() == cardId; });
    };
    QCOMPARE(findCard().state, ReviewBrowserState::Skipped);
    QVERIFY(!findCard().card.schedule.reviewed);
    QCOMPARE(findCard().card.schedule.reviewCount, 0);
    QCOMPARE(store.dueCards(now.addDays(1).addMSecs(-1)).size(), 1);
    QCOMPARE(store.dueCards(now.addDays(1)).size(), 2);
    addAction(cardId, overlap, QStringLiteral("skip"), now.addDays(2));
    addAction(cardId, overlap, QStringLiteral("undo_skip"), {});
    QCOMPARE(findCard().card.skippedUntil, now.addDays(1));
    addAction(otherId, batch, QStringLiteral("undo_skip"), {});
    QCOMPARE(store.dueCards(now).size(), 1);
    addAction(cardId, batch, QStringLiteral("undo_skip"), {});
    QCOMPARE(store.dueCards(now).size(), 2);
    QCOMPARE(findCard().state, ReviewBrowserState::New);
    // A stale skip delivered after its undo cannot hide the card again.
    addAction(cardId, batch, QStringLiteral("skip"), now.addDays(1));
    QCOMPARE(store.dueCards(now).size(), 2);
    const QDateTime later = now.addDays(14);
    const QString eventId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    projections.insert(QStringLiteral("review_event\nevents\n") + eventId,
                       QJsonObject{{QStringLiteral("event_id"), eventId}, {QStringLiteral("card_id"), cardId},
                                   {QStringLiteral("rating"), QStringLiteral("custom_interval")},
                                   {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)},
                                   {QStringLiteral("scheduled_due_at"), later.toString(Qt::ISODate)}});
    addAction(cardId, QUuid::createUuid().toString(QUuid::WithoutBraces), QStringLiteral("skip"), now.addDays(1));
    QCOMPARE(findCard().card.schedule.dueAt, later);
    QCOMPARE(findCard().card.schedule.reviewCount, 1);
    QCOMPARE(store.dueCards(now.addDays(1)).size(), 1);
    QCOMPARE(store.dueCards(later).size(), 2);
}

void ReviewStoreTest::schedulesRatings()
{
    const QDateTime now(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC);
    const ReviewSchedule again = ReviewStore::nextSchedule({}, QStringLiteral("again"), now, false);
    QCOMPARE(again.dueAt, now.addSecs(60));
    QCOMPARE(again.learningStep, 0);
    QVERIFY(!again.relearning);

    const ReviewSchedule hard = ReviewStore::nextSchedule({}, QStringLiteral("hard"), now, false);
    QCOMPARE(hard.dueAt, now.addSecs(6 * 60));
    QCOMPARE(hard.learningStep, 0);

    const ReviewSchedule good = ReviewStore::nextSchedule({}, QStringLiteral("good"), now, false);
    QCOMPARE(good.dueAt, now.addSecs(10 * 60));
    QCOMPARE(good.learningStep, 1);
    QCOMPARE(ReviewStore::nextSchedule(good, QStringLiteral("hard"), now.addSecs(10 * 60)).dueAt,
             now.addSecs(20 * 60));
    const ReviewSchedule graduated = ReviewStore::nextSchedule(good, QStringLiteral("good"), now.addSecs(10 * 60));
    QCOMPARE(graduated.intervalDays, 3);
    QCOMPARE(graduated.dueAt, now.addSecs(10 * 60).addDays(3));

    QCOMPARE(ReviewStore::nextSchedule({}, QStringLiteral("easy"), now, false).intervalDays, 7);

    ReviewSchedule establishedReview;
    establishedReview.intervalDays = 20;
    const ReviewSchedule relearning = ReviewStore::nextSchedule(establishedReview, QStringLiteral("again"), now);
    QCOMPARE(relearning.dueAt, now.addSecs(10 * 60));
    QVERIFY(relearning.relearning);
    QCOMPARE(relearning.previousIntervalDays, 20);
    QCOMPARE(ReviewStore::nextSchedule(relearning, QStringLiteral("hard"), now).dueAt, now.addSecs(15 * 60));
    QCOMPARE(ReviewStore::nextSchedule(relearning, QStringLiteral("good"), now).intervalDays, 1);

    ReviewSchedule old;
    old.intervalDays = 200;
    QCOMPARE(ReviewStore::nextSchedule(old, QStringLiteral("easy"), now).intervalDays, 365);

    QCOMPARE(ReviewStore::nextSchedule({}, QStringLiteral("again"), now, false, 0).dueAt, now.addSecs(10 * 60));
    QCOMPARE(ReviewStore::nextSchedule({}, QStringLiteral("hard"), now, false, 0).intervalDays, 1);
    QCOMPARE(ReviewStore::nextSchedule({}, QStringLiteral("good"), now, false, 0).intervalDays, 3);
    QCOMPARE(ReviewStore::nextSchedule({}, QStringLiteral("easy"), now, false, 0).intervalDays, 7);

    const ReviewSchedule legacyNewLearning = ReviewStore::nextSchedule({}, QStringLiteral("again"), now, false, 0);
    QCOMPARE(ReviewStore::nextSchedule(legacyNewLearning, QStringLiteral("good"), now).learningStep, 1);
    const ReviewSchedule legacyReviewLapse = ReviewStore::nextSchedule(establishedReview, QStringLiteral("again"), now, true, 0);
    QVERIFY(legacyReviewLapse.relearning);
    QCOMPARE(ReviewStore::nextSchedule(legacyReviewLapse, QStringLiteral("good"), now).intervalDays, 1);
}


void ReviewStoreTest::androidExplicitChoicesRoundTrip()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-07T08:11:26.082Z"), Qt::ISODateWithMs);
    const QStringList ratings = {QStringLiteral("ten_minutes"), QStringLiteral("next_day"), QStringLiteral("next_week"),
                                 QStringLiteral("custom_interval"), QStringLiteral("custom_interval"), QStringLiteral("retained")};
    const QList<QDateTime> dates = {now.addSecs(600), now.addDays(1), now.addDays(7), now.addSecs(1800), now.addYears(2), now};
    for (int i = 0; i < ratings.size(); ++i) {
        const QString cardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                            reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
        QString error;
        // A previous long interval must not multiply an explicit delay.
        QVERIFY2(store.recordRating(cardId, QStringLiteral("easy"), now.addDays(-1), &error), qPrintable(error));
        QVERIFY2(store.recordRating(cardId, ratings.at(i), now, &error, dates.at(i)), qPrintable(error));
        const auto cards = store.browserCards(now);
        const auto it = std::find_if(cards.cbegin(), cards.cend(), [&](const ReviewBrowserCard &card) { return card.card.id() == cardId; });
        QVERIFY(it != cards.cend());
        QCOMPARE(it->card.schedule.dueAt, dates.at(i));
        QCOMPARE(it->card.schedule.reviewCount, 2);
        QCOMPARE(it->card.schedule.lastRating, ratings.at(i));
        const bool retained = ratings.at(i) == QLatin1String("retained");
        QCOMPARE(it->card.schedule.retained, retained);
        QCOMPARE(it->card.schedule.fixedInterval, !retained);
        QCOMPARE(it->state, retained ? ReviewBrowserState::Retained : ReviewBrowserState::Future);
        const auto includes = [&](const QDateTime &at) {
            const auto due = store.dueCards(at);
            return std::any_of(due.cbegin(), due.cend(), [&](const ReviewCard &card) { return card.id() == cardId; });
        };
        QVERIFY(!includes(dates.at(i).addMSecs(-1)));
        QCOMPARE(includes(dates.at(i)), !retained);
        if (retained) {
            QVERIFY(!includes(now.addYears(10)));
        }
    }
    // Ten-minute and short custom choices must not use the learning lookahead.
    QVERIFY(store.dueCards(now).isEmpty());
    QCOMPARE(store.activeCards().size(), 5);
    QCOMPARE(store.dueCards(now.addSecs(600)).size(), 1);
}

void ReviewStoreTest::oneSuggestionFollowsTheLatestInterval()
{
    const auto now = QDateTime::fromString(QStringLiteral("2026-09-07T08:00:00Z"), Qt::ISODate);
    QJsonObject correct{{QStringLiteral("event_id"), QStringLiteral("correct")}, {QStringLiteral("card_id"), QStringLiteral("card")},
                        {QStringLiteral("rating"), QStringLiteral("custom_interval")}, {QStringLiteral("review_action"), QStringLiteral("correct")},
                        {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)},
                        {QStringLiteral("scheduled_due_at"), now.addSecs(17 * 86400 + 43200).toString(Qt::ISODate)}};
    QJsonObject error = correct;
    const auto failedAt = now.addDays(20);
    error.insert(QStringLiteral("event_id"), QStringLiteral("error"));
    error.insert(QStringLiteral("review_action"), QStringLiteral("error"));
    error.insert(QStringLiteral("reviewed_at"), failedAt.toString(Qt::ISODate));
    error.insert(QStringLiteral("scheduled_due_at"), failedAt.addSecs(1200).toString(Qt::ISODate));
    auto schedule = ReviewStore::foldEvents({correct, error, error}).value(QStringLiteral("card"));
    // Tags are labels: a wrong answer's twenty minutes is doubled like any other interval.
    auto suggestion = ReviewInterval::suggested(schedule.suggestionReviewedAt, schedule.suggestionDueAt);
    QCOMPARE(suggestion.amount, 40.0);
    QCOMPARE(suggestion.unit, ReviewInterval::Minutes);
    QCOMPARE(schedule.reviewCount, 2);
    QJsonObject undo{{QStringLiteral("event_id"), QStringLiteral("undo")}, {QStringLiteral("card_id"), QStringLiteral("card")},
                     {QStringLiteral("retracted_event_id"), QStringLiteral("error")},
                     {QStringLiteral("reviewed_at"), failedAt.addSecs(1).toString(Qt::ISODate)}};
    schedule = ReviewStore::foldEvents({correct, error, undo}).value(QStringLiteral("card"));
    suggestion = ReviewInterval::suggested(schedule.suggestionReviewedAt, schedule.suggestionDueAt);
    QCOMPARE(suggestion.amount, 5.0);
    QCOMPARE(suggestion.unit, ReviewInterval::Weeks);
    error.insert(QStringLiteral("rating"), QStringLiteral("retained"));
    error.remove(QStringLiteral("scheduled_due_at"));
    schedule = ReviewStore::foldEvents({correct, error}).value(QStringLiteral("card"));
    QVERIFY(schedule.retained);
    QCOMPARE(ReviewInterval::suggested(schedule.suggestionReviewedAt, schedule.suggestionDueAt).amount, 5.0);
}

void ReviewStoreTest::tagsAreRecordedCountedAndMappedFromOlderAnswers()
{
    // Skip postpones without answering, so it is never offered as a tag.
    QCOMPARE(ReviewTags::byPopularity({}), (QStringList{QStringLiteral("correct"), QStringLiteral("wrong")}));
    QCOMPARE(ReviewTags::byPopularity({{QStringLiteral("hard"), 5}, {QStringLiteral("skip"), 2}}, {QStringLiteral("later")}),
             (QStringList{QStringLiteral("hard"), QStringLiteral("correct"), QStringLiteral("wrong"), QStringLiteral("later")}));
    // A built-in merged into another tag is no longer offered: its answers count under the new name.
    QCOMPARE(ReviewTags::byPopularity({{QStringLiteral("Recalled"), 3}}, {}, {{QStringLiteral("correct"), QStringLiteral("Recalled")}}),
             (QStringList{QStringLiteral("Recalled"), QStringLiteral("wrong")}));
    QCOMPARE(ReviewTags::normalize(QStringLiteral(" WRONG ")), QStringLiteral("wrong"));
    QCOMPARE(ReviewTags::normalize(QStringLiteral("good"), {{QStringLiteral("correct"), QStringLiteral("Good")}}), QStringLiteral("correct"));
    QCOMPARE(ReviewTags::normalize(QStringLiteral(" two \t words ")), QStringLiteral("two words"));
    QVERIFY(ReviewTags::normalize(QStringLiteral("   ")).isEmpty());
    // Long tags are stored whole and only shortened on buttons, matching Android.
    const QString longTag = QStringLiteral("never submitted and a lot of suffering reading this");
    QCOMPARE(ReviewTags::normalize(longTag), longTag);
    QCOMPARE(ReviewTags::shortLabel(QStringLiteral("missed the key step")), QStringLiteral("missed the key step"));
    QCOMPARE(ReviewTags::shortLabel(longTag), QStringLiteral("never submitted and…"));
    QCOMPARE(ReviewTags::shortLabel(QStringLiteral("Supercalifragilisticexpialidocious")), QStringLiteral("Supercalifragilisticex…"));
    QCOMPARE(ReviewTags::legacyReviewAction({QStringLiteral("skip"), QStringLiteral("wrong")}), QStringLiteral("wrong"));
    QVERIFY(ReviewTags::legacyReviewAction({QStringLiteral("hard")}).isEmpty());

    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + first, reviewCard(first, QStringLiteral("paper"), QString(64, QLatin1Char('a')))},
                           {QStringLiteral("review_card\ncards\n") + second, reviewCard(second, QStringLiteral("paper"), QString(64, QLatin1Char('a')))}};
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime at(QDate(2026, 9, 29), QTime(20, 0), QTimeZone::UTC);
    QString error;
    QString tagged;
    QVERIFY2(store.recordRating(first, QStringLiteral("custom_interval"), at, &error, at.addSecs(600), &tagged, {},
                                {QStringLiteral(" Skip "), QStringLiteral("confused  notation"), QStringLiteral("skip")}), qPrintable(error));
    QVERIFY2(store.recordRating(second, QStringLiteral("custom_interval"), at.addMSecs(500), &error, at.addSecs(600), nullptr, {},
                                {QStringLiteral("confused notation")}), qPrintable(error));
    QVERIFY2(store.recordRating(second, QStringLiteral("custom_interval"), at.addSecs(1), &error, at.addSecs(900), nullptr,
                                QStringLiteral("error")), qPrintable(error));
    QJsonObject taggedEvent;
    for (const auto &card : store.cards()) {
        for (const auto &event : card.history)
            if (event.value(QStringLiteral("event_id")).toString() == tagged) taggedEvent = event;
    }
    // Skip is never recorded as an answer's tag; Skip postpones instead.
    QCOMPARE(taggedEvent.value(QStringLiteral("review_tags")).toArray(),
             QJsonArray::fromStringList({QStringLiteral("confused notation")}));
    QVERIFY(taggedEvent.value(QStringLiteral("review_action")).toString().isEmpty());
    // Cards per current tag: the second card's later "error" answer moved it to wrong.
    const QHash<QString, int> expected{{QStringLiteral("confused notation"), 1}, {QStringLiteral("wrong"), 1}};
    QCOMPARE(store.reviewTagUsage(), expected);
    for (const auto &card : store.cards()) {
        if (card.id() == first) QCOMPARE(card.currentTags(), (QStringList{QStringLiteral("confused notation")}));
        else QCOMPARE(card.currentTags(), QStringList{QStringLiteral("wrong")});
    }
    QVERIFY2(store.retractRating(first, tagged, at.addSecs(2), &error), qPrintable(error));
    // The first card's only answer is undone: it is new again and counts for nothing. The
    // second card's earlier answer still lists its tag, with no card.
    const QHash<QString, int> afterUndo{{QStringLiteral("confused notation"), 0}, {QStringLiteral("wrong"), 1}};
    QCOMPARE(store.reviewTagUsage(), afterUndo);
}

void ReviewStoreTest::currentTagsComeFromTheLatestAnswerInEffect()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString legacy = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection;
    for (const QString &id : {first, second, legacy})
        projection.insert(QStringLiteral("review_card\ncards\n") + id, reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a'))));
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime at(QDate(2026, 10, 12), QTime(9, 0), QTimeZone::UTC);
    QString error;
    const auto cardOf = [&store](const QString &id) {
        for (const auto &card : store.cards())
            if (card.id() == id) return card;
        return ReviewCard{};
    };
    // Never answered: no tags, and new in the browser.
    QVERIFY(cardOf(first).currentTags().isEmpty());
    QVERIFY(!cardOf(first).wasAnswered());
    QVERIFY(ReviewBrowserCard{cardOf(first)}.matchesFilter(QStringLiteral("new")));

    QString alpha, betaGamma, untagged;
    QVERIFY2(store.recordRating(first, QStringLiteral("custom_interval"), at, &error, at.addSecs(60), &alpha, {}, {QStringLiteral("alpha")}), qPrintable(error));
    QCOMPARE(cardOf(first).currentTags(), QStringList{QStringLiteral("alpha")});
    QVERIFY2(store.recordRating(first, QStringLiteral("custom_interval"), at.addSecs(10), &error, at.addSecs(70), &betaGamma, {},
                                {QStringLiteral("beta"), QStringLiteral("gamma")}), qPrintable(error));
    QCOMPARE(cardOf(first).currentTags(), QStringList({QStringLiteral("beta"), QStringLiteral("gamma")}));
    // A skip is not an answer.
    ReviewSkipBatch batch;
    QVERIFY2(store.skip(first, ReviewSkipScope::Card, at.addSecs(600), &batch, &error, at.addSecs(20)), qPrintable(error));
    QCOMPARE(cardOf(first).currentTags(), QStringList({QStringLiteral("beta"), QStringLiteral("gamma")}));
    // An answer with no tag records an empty list and is the last answer: the card is answered,
    // with no tags (shown as "No tag"), not back to the tags of an earlier answer.
    QVERIFY2(store.recordRating(first, QStringLiteral("custom_interval"), at.addSecs(30), &error, at.addSecs(90), &untagged, {}, {}), qPrintable(error));
    QJsonObject untaggedEvent;
    for (const auto &event : cardOf(first).history)
        if (event.value(QStringLiteral("event_id")).toString() == untagged) untaggedEvent = event;
    QCOMPARE(untaggedEvent.value(QStringLiteral("review_tags")).toArray(), QJsonArray{});
    QVERIFY(untaggedEvent.value(QStringLiteral("review_tags")).isArray());
    QVERIFY(!untaggedEvent.contains(QStringLiteral("review_action")));
    QVERIFY(cardOf(first).tagsOfEvent(untaggedEvent).isEmpty());
    QVERIFY(cardOf(first).currentTags().isEmpty());
    QVERIFY(cardOf(first).wasAnswered());
    QVERIFY(cardOf(first).currentMark().isEmpty());
    ReviewBrowserCard untaggedItem{cardOf(first)};
    QVERIFY(untaggedItem.matchesFilter(QStringLiteral("seen")));
    QVERIFY(!untaggedItem.matchesFilter(QStringLiteral("new")));
    QVERIFY(!untaggedItem.matchesFilter(QStringLiteral("tag:beta")));
    // Tag counts are cards per current tag: the untagged answer counts toward none, and the
    // cards never answered count for nothing. Earlier answers' tags are listed with 0.
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("alpha"), 0}, {QStringLiteral("beta"), 0}, {QStringLiteral("gamma"), 0}}));
    // Undoing the last answer brings back the one before; undoing every answer, none.
    QVERIFY2(store.retractRating(first, untagged, at.addSecs(40), &error), qPrintable(error));
    QCOMPARE(cardOf(first).currentTags(), QStringList({QStringLiteral("beta"), QStringLiteral("gamma")}));
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("alpha"), 0}, {QStringLiteral("beta"), 1}, {QStringLiteral("gamma"), 1}}));
    QVERIFY2(store.retractRating(first, betaGamma, at.addSecs(41), &error), qPrintable(error));
    QCOMPARE(cardOf(first).currentTags(), QStringList{QStringLiteral("alpha")});
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("alpha"), 1}}));
    QVERIFY2(store.retractRating(first, alpha, at.addSecs(42), &error), qPrintable(error));
    QVERIFY(cardOf(first).currentTags().isEmpty());
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{}));
    QVERIFY(!cardOf(first).wasAnswered());
    // An interval change is not an answer.
    QVERIFY2(store.setInterval(first, {3, ReviewInterval::Weeks, {}}, false, at.addSecs(43), &error), qPrintable(error));
    QVERIFY(!cardOf(first).wasAnswered());

    // One answer given to two cards (a document answer) is the last answer of each.
    for (const QString &id : {first, second})
        QVERIFY2(store.recordRating(id, QStringLiteral("custom_interval"), at.addSecs(50), &error, at.addSecs(3600), nullptr, {},
                                    {QStringLiteral("tough"), QStringLiteral("delta")}), qPrintable(error));
    QCOMPARE(cardOf(first).currentTags(), QStringList({QStringLiteral("tough"), QStringLiteral("delta")}));
    QCOMPARE(cardOf(second).currentTags(), QStringList({QStringLiteral("tough"), QStringLiteral("delta")}));
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("tough"), 2}, {QStringLiteral("delta"), 2}}));
    // Merged tags read under their new names, and a card counts a merged name once.
    QVERIFY2(store.recordTagMerge({QStringLiteral("tough"), QStringLiteral("delta")}, QStringLiteral("Hard"), &error), qPrintable(error));
    QCOMPARE(cardOf(second).currentTags(), QStringList{QStringLiteral("Hard")});
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("Hard"), 2}}));

    // Older answers keep the tags derived from their rating and action; a skip they recorded as an
    // answer (action or tag "skip") is still not an answer.
    QVERIFY2(store.recordRating(legacy, QStringLiteral("good"), at, &error), qPrintable(error));
    QCOMPARE(cardOf(legacy).currentTags(), QStringList{QStringLiteral("correct")});
    QVERIFY2(store.recordRating(legacy, QStringLiteral("custom_interval"), at.addSecs(10), &error, at.addSecs(70), nullptr, QStringLiteral("skip")), qPrintable(error));
    QCOMPARE(cardOf(legacy).currentTags(), QStringList{QStringLiteral("correct")});
    QVERIFY2(store.recordRating(legacy, QStringLiteral("custom_interval"), at.addSecs(20), &error, at.addSecs(80), nullptr, QStringLiteral("error")), qPrintable(error));
    QCOMPARE(cardOf(legacy).currentTags(), QStringList{QStringLiteral("wrong")});
}

namespace
{
QJsonObject tagMerge(const QString &id, const QString &createdAt, const QString &to, const QStringList &from)
{
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("merge_id"), id}, {QStringLiteral("from"), QJsonArray::fromStringList(from)},
            {QStringLiteral("to"), to}, {QStringLiteral("created_at"), createdAt}, {QStringLiteral("origin_id"), QStringLiteral("phone")}};
}
}

void ReviewStoreTest::tagMergesFollowAndroidMappingRule()
{
    using Mapping = QHash<QString, QString>;
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QVERIFY(ReviewTags::mergeMapping({}).isEmpty());

    // A chain: A -> B, then B -> C makes A read C. Order comes from created_at, not list order.
    const Mapping chain = ReviewTags::mergeMapping({tagMerge(s("2"), s("2026-10-05T11:00:00Z"), s("C"), {s("B")}),
                                                     tagMerge(s("1"), s("2026-10-05T10:00:00Z"), s("B"), {s("A")})});
    QCOMPARE(chain, (Mapping{{s("A"), s("C")}, {s("B"), s("C")}}));
    QCOMPARE(ReviewTags::resolve(s("A"), chain), s("C"));
    QCOMPARE(ReviewTags::resolve(s("unmerged"), chain), s("unmerged"));

    // Android's own case: "Good" already reads as "known", so merging "known" into it changes nothing and cannot loop.
    const Mapping android = ReviewTags::mergeMapping({tagMerge(s("1"), s("2026-10-05T10:00:00Z"), s("good"), {s("Good"), s("correct")}),
                                                       tagMerge(s("2"), s("2026-10-05T11:00:00Z"), s("known"), {s("good")}),
                                                       tagMerge(s("3"), s("2026-10-05T12:00:00Z"), s("Good"), {s("known")})});
    QCOMPARE(android, (Mapping{{s("Good"), s("known")}, {s("correct"), s("known")}, {s("good"), s("known")}}));
    for (const QString &value : android) QVERIFY(!android.contains(value));

    // A re-merge into the same name is a no-op; so is a tag merged into itself.
    const QJsonObject first = tagMerge(s("1"), s("2026-10-05T10:00:00Z"), s("good"), {s("Good"), s("good")});
    const Mapping once = ReviewTags::mergeMapping({first});
    QCOMPARE(once, (Mapping{{s("Good"), s("good")}}));
    QCOMPARE(ReviewTags::mergeMapping({first, tagMerge(s("2"), s("2026-10-05T11:00:00Z"), s("good"), {s("Good")})}), once);

    // Equal creation times are ordered by merge_id.
    QCOMPARE(ReviewTags::mergeMapping({tagMerge(s("b"), s("2026-10-05T10:00:00Z"), s("y"), {s("x")}),
                                       tagMerge(s("a"), s("2026-10-05T10:00:00Z"), s("x"), {s("w")})}),
             (Mapping{{s("w"), s("y")}, {s("x"), s("y")}}));

    // Malformed merges are ignored; names are trimmed.
    QCOMPARE(ReviewTags::mergeMapping({tagMerge(QString(), s("2026-10-05T10:00:00Z"), s("x"), {s("a")}),
                                       tagMerge(s("1"), s("2026-10-05T10:00:00Z"), s(" "), {s("b")}),
                                       tagMerge(s("2"), s("2026-10-05T10:00:00Z"), s("x"), {s(" ")}),
                                       tagMerge(s("3"), s("2026-10-05T10:00:00Z"), s(" z "), {s(" c ")})}),
             (Mapping{{s("c"), s("z")}}));

    // Mapped tags are de-duplicated, keeping the first occurrence.
    QCOMPARE(ReviewTags::applyMerges({s("Good"), s("skip"), s("correct"), s("known")}, android), (QStringList{s("known"), s("skip")}));
    QCOMPARE(ReviewTags::applyMerges({s("skip"), s("A"), s("B")}, chain), (QStringList{s("skip"), s("C")}));
}

namespace
{
QJsonObject tagGroupsRecord(const QJsonArray &groups, const QString &updatedAt = QStringLiteral("2026-10-08T21:00:00.000Z"))
{
    return {{QStringLiteral("layout_id"), QStringLiteral("groups")}, {QStringLiteral("schema_version"), 1}, {QStringLiteral("groups"), groups},
            {QStringLiteral("updated_at"), updatedAt}, {QStringLiteral("origin_id"), QStringLiteral("phone")}};
}

QJsonObject tagGroup(const QString &id, const QString &name, const QStringList &tags)
{
    return {{QStringLiteral("group_id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}};
}

QStringList groupNames(const QList<ReviewTags::TagGroup> &groups)
{
    QStringList names;
    for (const auto &group : groups) names.append(group.name);
    return names;
}
}

void ReviewStoreTest::tagGroupsRecordsAreValidated()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QList<ReviewTags::TagGroup> groups;
    QString error;
    // The spec's example record.
    QVERIFY2(ReviewTags::parseGroups(tagGroupsRecord({tagGroup(s("u"), s(" Understanding "), {s("correct"), s("wrong"), s("Slowly understanding it")}),
                                                      tagGroup(s("i"), s("Interest"), {s("interesting"), s("cool")})}), &groups, &error), qPrintable(error));
    QCOMPARE(groups.size(), 2);
    QCOMPARE(groups.at(0), (ReviewTags::TagGroup{s("u"), s("Understanding"), {s("correct"), s("wrong"), s("Slowly understanding it")}}));
    QCOMPARE(groups.at(1).tags, (QStringList{s("interesting"), s("cool")}));
    // No groups is a valid layout: everything is in Other.
    QVERIFY(ReviewTags::parseGroups(tagGroupsRecord({}), &groups));
    QVERIFY(groups.isEmpty());
    // A tag in two groups keeps its first occurrence; a repeat inside one group collapses.
    QVERIFY(ReviewTags::parseGroups(tagGroupsRecord({tagGroup(s("a"), s("A"), {s("cool"), s("cool")}), tagGroup(s("b"), s("B"), {s("cool"), s("fun")})}), &groups));
    QCOMPARE(groups.at(0).tags, QStringList{s("cool")});
    QCOMPARE(groups.at(1).tags, QStringList{s("fun")});

    auto wrongLayout = tagGroupsRecord({});
    wrongLayout.insert(QStringLiteral("layout_id"), QStringLiteral("layout"));
    auto wrongVersion = tagGroupsRecord({});
    wrongVersion.insert(QStringLiteral("schema_version"), 2);
    QJsonArray tooMany, manyTags;
    for (int index = 0; index < 51; ++index) tooMany.append(tagGroup(QString::number(index), QString::number(index), {}));
    for (int index = 0; index < 10; ++index) {
        QStringList tags;
        for (int tag = 0; tag < 101; ++tag) tags.append(QStringLiteral("%1-%2").arg(index).arg(tag));
        manyTags.append(tagGroup(QString::number(index), QString::number(index), tags));
    }
    const QList<QJsonObject> invalid {
        wrongLayout, wrongVersion,
        {{QStringLiteral("layout_id"), QStringLiteral("groups")}, {QStringLiteral("schema_version"), 1}},
        tagGroupsRecord({tagGroup(s("o"), s("Other"), {})}),
        tagGroupsRecord({tagGroup(s("o"), s("  oTHer "), {})}),
        tagGroupsRecord({tagGroup(s("a"), s("Interest"), {}), tagGroup(s("b"), s("interest "), {})}),
        tagGroupsRecord({tagGroup(s("same"), s("A"), {}), tagGroup(s("same"), s("B"), {})}),
        tagGroupsRecord({tagGroup(s("a"), s("   "), {})}),
        tagGroupsRecord({tagGroup(s("a"), QString(81, QLatin1Char('x')), {})}),
        tagGroupsRecord({tagGroup(QString(65, QLatin1Char('g')), s("A"), {})}),
        tagGroupsRecord({tagGroup(QString(), s("A"), {})}),
        tagGroupsRecord({tagGroup(s("a"), s("A"), {QString()})}),
        tagGroupsRecord({tagGroup(s("a"), s("A"), {QString(201, QLatin1Char('t'))})}),
        tagGroupsRecord(tooMany),
        tagGroupsRecord(manyTags),
        tagGroupsRecord({QJsonObject{{QStringLiteral("group_id"), s("a")}, {QStringLiteral("name"), s("A")}, {QStringLiteral("tags"), QJsonArray{1}}}}),
    };
    for (const QJsonObject &record : invalid) {
        groups = {{s("kept"), s("Kept"), {}}};
        error.clear();
        QVERIFY2(!ReviewTags::parseGroups(record, &groups, &error), QJsonDocument(record).toJson().constData());
        QVERIFY(!error.isEmpty());
        QCOMPARE(groups.size(), 1);
    }
    // Lengths count characters as the server does: 80 emoji make a valid name.
    QString emoji;
    for (int index = 0; index < 80; ++index) emoji += QString::fromUcs4(U"\U0001F600", 1);
    QVERIFY(ReviewTags::parseGroups(tagGroupsRecord({tagGroup(s("a"), emoji, {})}), &groups));
    QVERIFY(!ReviewTags::parseGroups(tagGroupsRecord({tagGroup(s("a"), emoji + QStringLiteral("x"), {})}), &groups));
}

void ReviewStoreTest::tagGroupsShowMergedNamesAndOther()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    const QHash<QString, QString> merges = ReviewTags::mergeMapping({tagMerge(s("1"), s("2026-10-05T10:00:00Z"), s("good"), {s("Good"), s("fine")})});
    const QList<ReviewTags::TagGroup> stored {
        {s("u"), s("Understanding"), {s("Good"), s(" Correct "), s("skip"), s("wrong")}},
        {s("q"), s("Quality"), {s("good"), s("fine"), s("cool")}},
    };
    const auto shown = ReviewTags::effectiveGroups(stored, merges);
    // "Good" now reads "good", which Quality lists by its final name; built-in names become IDs; Skip is never a tag.
    QCOMPARE(shown.at(0).tags, (QStringList{s("correct"), s("wrong")}));
    QCOMPARE(shown.at(1).tags, (QStringList{s("good"), s("cool")}));
    // Without a group listing the final name, it shows where its first old name is, once.
    const auto onlyOld = ReviewTags::effectiveGroups({{s("a"), s("A"), {s("Good")}}, {s("b"), s("B"), {s("fine")}}}, merges);
    QCOMPARE(onlyOld.at(0).tags, QStringList{s("good")});
    QVERIFY(onlyOld.at(1).tags.isEmpty());

    // Sections: groups in record order, their tags in the plain list's order, then Other last.
    const QStringList known = ReviewTags::byPopularity({{s("cool"), 5}, {s("good"), 3}, {s("later"), 1}}, {s("unused")});
    const auto sections = ReviewTags::sections(shown, known);
    QCOMPARE(groupNames(sections), (QStringList{s("Understanding"), s("Quality"), s("Other")}));
    QCOMPARE(sections.at(1).tags, (QStringList{s("cool"), s("good")}));
    QCOMPARE(sections.constLast().id, QString());
    QCOMPARE(sections.constLast().tags, (QStringList{s("later"), s("unused")}));
    QVERIFY(!sections.constLast().tags.contains(s("skip")));
    // Other is hidden when every tag has a group; with no groups everything is in Other.
    QCOMPARE(groupNames(ReviewTags::sections(shown, {s("correct"), s("good"), s("skip")})), (QStringList{s("Understanding"), s("Quality")}));
    const auto ungrouped = ReviewTags::sections({}, known);
    QCOMPARE(ungrouped.size(), 1);
    QCOMPARE(ungrouped.constFirst().tags, known);
    QCOMPARE(ReviewTags::groupOf(shown, s("cool")), s("q"));
    QVERIFY(ReviewTags::groupOf(shown, s("later")).isEmpty());
}

void ReviewStoreTest::tagGroupEditsKeepAValidWholeRecord()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QList<ReviewTags::TagGroup> groups;
    QString error, understanding, interest;
    QVERIFY2(ReviewTags::addGroup(groups, s(" Understanding "), &error, &understanding), qPrintable(error));
    QVERIFY(ReviewTags::addGroup(groups, s("Interest"), &error, &interest));
    QCOMPARE(groupNames(groups), (QStringList{s("Understanding"), s("Interest")}));
    QVERIFY(!QUuid(understanding).isNull());
    // "Other" is the implicit group, names are unique case-insensitively; a refused edit changes nothing.
    const auto before = groups;
    for (const QString &name : {s("other"), s(" OTHER"), s("interest"), s(" "), QString(81, QLatin1Char('n'))}) {
        error.clear();
        QVERIFY(!ReviewTags::addGroup(groups, name, &error));
        QVERIFY(!error.isEmpty());
    }
    QVERIFY(!ReviewTags::renameGroup(groups, interest, s("Other"), &error));
    QVERIFY(!ReviewTags::renameGroup(groups, interest, s("understanding"), &error));
    QVERIFY(!ReviewTags::renameGroup(groups, s("missing"), s("Fine"), &error));
    QCOMPARE(groups, before);
    QVERIFY(ReviewTags::renameGroup(groups, interest, s("Interesting"), &error));
    QVERIFY(ReviewTags::renameGroup(groups, interest, s("interesting"), &error)); // Its own name in another case.
    QCOMPARE(groups.at(1).name, s("interesting"));

    // Moving a tag takes it out of every other group; Other is no group at all.
    QVERIFY(ReviewTags::moveTag(groups, s("cool"), understanding, &error));
    QVERIFY(ReviewTags::moveTag(groups, s("cool"), interest, &error));
    QVERIFY(groups.at(0).tags.isEmpty());
    QCOMPARE(groups.at(1).tags, QStringList{s("cool")});
    QVERIFY(ReviewTags::moveTag(groups, s("cool"), QString(), &error));
    QVERIFY(groups.at(1).tags.isEmpty());
    QVERIFY(!ReviewTags::moveTag(groups, s("skip"), interest, &error));
    QVERIFY(!ReviewTags::moveTag(groups, QString(201, QLatin1Char('t')), interest, &error));
    QVERIFY(!ReviewTags::moveTag(groups, s("cool"), s("missing"), &error));

    QVERIFY(ReviewTags::moveTag(groups, s("cool"), interest, &error));
    QVERIFY(!ReviewTags::moveGroup(groups, understanding, -1));
    QVERIFY(ReviewTags::moveGroup(groups, understanding, 1));
    QCOMPARE(groupNames(groups), (QStringList{s("interesting"), s("Understanding")}));
    QVERIFY(!ReviewTags::moveGroup(groups, understanding, 1));
    // Deleting a group sends its tags to Other: they leave the record.
    QVERIFY(ReviewTags::deleteGroup(groups, interest));
    QCOMPARE(groupNames(groups), QStringList{s("Understanding")});
    QVERIFY(ReviewTags::groupOf(groups, s("cool")).isEmpty());
    QVERIFY(!ReviewTags::deleteGroup(groups, interest));

    // Every edit yields a record that parses back to the same groups.
    QVERIFY(ReviewTags::moveTag(groups, s("cool"), understanding, &error));
    const QJsonObject record = ReviewTags::groupsRecord(groups, s("2026-10-08T21:00:00.000Z"), s("desktop"));
    QList<ReviewTags::TagGroup> parsed;
    QVERIFY2(ReviewTags::parseGroups(record, &parsed, &error), qPrintable(error));
    QCOMPARE(parsed, groups);
    const QStringList fields = record.keys();
    QCOMPARE(QSet<QString>(fields.cbegin(), fields.cend()),
             (QSet<QString>{s("layout_id"), s("schema_version"), s("groups"), s("updated_at"), s("origin_id")}));
    const QStringList groupFields = record.value(s("groups")).toArray().first().toObject().keys();
    QCOMPARE(QSet<QString>(groupFields.cbegin(), groupFields.cend()), (QSet<QString>{s("group_id"), s("name"), s("tags")}));
    QList<ReviewTags::TagGroup> full;
    for (int index = 0; index < 50; ++index) QVERIFY(ReviewTags::addGroup(full, QString::number(index), &error));
    QVERIFY(!ReviewTags::addGroup(full, s("one more"), &error));
}

void ReviewStoreTest::groupNotesAreRecordedWithTheAnswer()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + first, reviewCard(first, QStringLiteral("paper"), QString(64, QLatin1Char('a')))},
                           {QStringLiteral("review_card\ncards\n") + second, reviewCard(second, QStringLiteral("paper"), QString(64, QLatin1Char('a')))}};
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime at(QDate(2026, 10, 10), QTime(20, 0), QTimeZone::UTC);
    QString error;

    // Notes are trimmed and cut to 500 characters (code points); empty ones are left out.
    const QString emoji = QString::fromUtf8("\xF0\x9F\x99\x82");
    QString longNote;
    for (int index = 0; index < 501; ++index) longNote += emoji;
    QCOMPARE(ReviewTags::cleanGroupNote(longNote).size(), 1000);
    QCOMPARE(ReviewTags::cutGroupNote(QString(600, QLatin1Char('x'))).size(), 500);
    const QHash<QString, QString> notes{{QStringLiteral("u"), QStringLiteral("  Mostly there\nproof still fuzzy  ")},
                                        {ReviewTags::otherGroupNoteKey(), longNote},
                                        {ReviewTags::answerNoteKey(), QStringLiteral(" Check the proof again ")},
                                        {QStringLiteral("i"), QStringLiteral(" \n ")}};
    QString noted;
    QVERIFY2(store.recordRating(first, QStringLiteral("custom_interval"), at, &error, at.addDays(14), &noted, {},
                                {QStringLiteral("correct")}, notes), qPrintable(error));
    // Without notes (or with empty ones only) the field is omitted.
    QString plain;
    QVERIFY2(store.recordRating(second, QStringLiteral("custom_interval"), at, &error, at.addDays(14), &plain, {},
                                {QStringLiteral("correct")}, {{QStringLiteral("u"), QStringLiteral("   ")}}), qPrintable(error));

    // The queued operation carries the notes as uploaded: group_id -> note, "other" for Other.
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    QJsonObject uploaded, uploadedPlain;
    for (const auto &operation : outbox.operations()) {
        const auto payload = operation.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("event_id")).toString() == noted) uploaded = payload;
        if (payload.value(QStringLiteral("event_id")).toString() == plain) uploadedPlain = payload;
    }
    QCOMPARE(uploaded.value(QStringLiteral("review_group_notes")).toObject(),
             (QJsonObject{{QStringLiteral("u"), QStringLiteral("Mostly there\nproof still fuzzy")},
                          {QStringLiteral("other"), ReviewTags::cleanGroupNote(longNote)},
                          {QStringLiteral("note"), QStringLiteral("Check the proof again")}}));
    QCOMPARE(uploaded.value(QStringLiteral("review_tags")).toArray(), QJsonArray{QStringLiteral("correct")});
    QVERIFY(!uploadedPlain.isEmpty());
    QVERIFY(!uploadedPlain.contains(QStringLiteral("review_group_notes")));

    // Round trip: the event read back, as from the server's projection, gives the same notes.
    projection.insert(QStringLiteral("review_event\nevents\n") + noted, uploaded);
    QHash<QString, QString> readBack;
    for (const auto &card : store.cards()) {
        for (const auto &event : card.history)
            if (event.value(QStringLiteral("event_id")).toString() == noted) readBack = ReviewTags::groupNotesOfEvent(event);
    }
    QCOMPARE(readBack, ReviewTags::cleanGroupNotes(notes));
    QCOMPARE(readBack.size(), 3);

    // Shown as "<Group>: <note>" in group order, then Other, then groups that no longer exist.
    const QList<ReviewTags::TagGroup> groups{{QStringLiteral("i"), QStringLiteral("Interest"), {}}, {QStringLiteral("u"), QStringLiteral("Understanding"), {}}};
    QCOMPARE(ReviewTags::groupNoteLines({{QStringLiteral("gone"), QStringLiteral("Old")}, {QStringLiteral("other"), QStringLiteral("Ask")},
                                         {QStringLiteral("u"), QStringLiteral("Mostly")}, {QStringLiteral("i"), QStringLiteral("Fun")}}, groups),
             QStringList({QStringLiteral("Interest: Fun"), QStringLiteral("Understanding: Mostly"), QStringLiteral("Other: Ask"), QStringLiteral("gone: Old")}));
    // The answer's own note ("note") comes last as "Note: …", after notes of unknown groups, and
    // a group with that ID never takes it.
    const QList<ReviewTags::TagGroup> clashing{{QStringLiteral("note"), QStringLiteral("Clash"), {}}, {QStringLiteral("u"), QStringLiteral("Understanding"), {}}};
    QCOMPARE(ReviewTags::groupNoteLines({{QStringLiteral("note"), QStringLiteral("Redo")}, {QStringLiteral("zz-gone"), QStringLiteral("Old")},
                                         {QStringLiteral("other"), QStringLiteral("Ask")}, {QStringLiteral("u"), QStringLiteral("Mostly")}}, clashing),
             QStringList({QStringLiteral("Understanding: Mostly"), QStringLiteral("Other: Ask"), QStringLiteral("zz-gone: Old"), QStringLiteral("Note: Redo")}));
    QCOMPARE(ReviewTags::groupNoteLines({{QStringLiteral("note"), QStringLiteral("Redo")}}, groups, QStringLiteral("Andere"), QStringLiteral("Notiz")),
             QStringList{QStringLiteral("Notiz: Redo")});
    QVERIFY(ReviewTags::groupNoteLines({}, groups).isEmpty());
    QVERIFY(ReviewTags::groupNotesOfEvent(uploadedPlain).isEmpty());
}

// review-phases spec, section 3: an answer given in two steps records review_phases and the
// before note under "before_note"; readers of older events see nothing new.
void ReviewStoreTest::reviewPhasesAreRecordedWithTheAnswer()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString card = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + card, reviewCard(card, QStringLiteral("paper"), QString(64, QLatin1Char('a')))}};
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime shown(QDate(2026, 11, 3), QTime(12, 0, 5, 120), QTimeZone::UTC);
    const QJsonObject prediction{{QStringLiteral("tag"), s("Partly recalled")}, {QStringLiteral("group_id"), s("r")}, {QStringLiteral("reason"), s("Missed twice.")},
                                 {QStringLiteral("model"), s("m")}, {QStringLiteral("prediction_id"), s("p-1")},
                                 {QStringLiteral("requested_at"), s("2026-11-03T12:00:00.000Z")}, {QStringLiteral("made_at"), s("2026-11-03T12:00:03.000Z")},
                                 {QStringLiteral("arrived_before_answer"), true}};
    const QJsonObject phases = ReviewPhases::toField({s("Meh"), s("Partly recalled")}, {s("Recalled"), s("I get it")}, shown, prediction);
    QCOMPARE(phases, (QJsonObject{{QStringLiteral("schema_version"), 1},
                                  {QStringLiteral("before"), QJsonObject{{QStringLiteral("tags"), QJsonArray{s("Meh"), s("Partly recalled")}},
                                                                         {QStringLiteral("answer_shown_at"), s("2026-11-03T12:00:05.120Z")}}},
                                  {QStringLiteral("after"), QJsonObject{{QStringLiteral("tags"), QJsonArray{s("Recalled"), s("I get it")}}}},
                                  {QStringLiteral("agent_prediction"), prediction}}));
    // Never shown, and no prediction: both left out.
    const QJsonObject unshown = ReviewPhases::toField({}, {s("Recalled")}, {});
    QVERIFY(!unshown.value(QStringLiteral("before")).toObject().contains(QStringLiteral("answer_shown_at")));
    QVERIFY(!unshown.contains(QStringLiteral("agent_prediction")));

    QString error, eventId, deleted;
    const QDateTime at = shown.addSecs(20);
    QVERIFY2(store.recordRating(card, QStringLiteral("custom_interval"), at, &error, at.addDays(4), &eventId, {}, {s("Meh"), s("Recalled"), s("I get it")},
                                {{ReviewTags::beforeNoteKey(), s("  not sure  ")}, {ReviewTags::answerNoteKey(), s("ok")}}, {}, nullptr, phases),
             qPrintable(error));
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    QJsonObject uploaded;
    for (const auto &operation : outbox.operations()) {
        const auto payload = operation.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("event_id")).toString() == eventId) uploaded = payload;
    }
    QCOMPARE(uploaded.value(QStringLiteral("review_phases")).toObject(), phases);
    QCOMPARE(uploaded.value(QStringLiteral("review_tags")).toArray(), (QJsonArray{s("Meh"), s("Recalled"), s("I get it")}));
    QCOMPARE(uploaded.value(QStringLiteral("review_group_notes")).toObject(),
             (QJsonObject{{QStringLiteral("before_note"), s("not sure")}, {QStringLiteral("note"), s("ok")}}));
    const auto recorded = ReviewPhases::ofEvent(uploaded);
    QVERIFY(recorded.present);
    QCOMPARE(recorded.beforeTags, (QStringList{s("Meh"), s("Partly recalled")}));
    QCOMPARE(recorded.afterTags, (QStringList{s("Recalled"), s("I get it")}));
    QCOMPARE(recorded.answerShownAt, shown);
    QCOMPARE(recorded.agentPrediction.value(QStringLiteral("tag")).toString(), s("Partly recalled"));
    // An older event has none.
    QVERIFY(!ReviewPhases::ofEvent(QJsonObject{{QStringLiteral("review_tags"), QJsonArray{s("Recalled")}}}).present);
    // A deletion records no phases.
    QVERIFY2(store.recordRating(card, QStringLiteral("delete"), at.addSecs(60), &error, {}, &deleted, {}, {}, {}, {}, nullptr, phases), qPrintable(error));
    for (const auto &operation : outbox.operations()) {
        const auto payload = operation.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("event_id")).toString() == deleted) QVERIFY(!payload.contains(QStringLiteral("review_phases")));
    }
    // The before note is listed after the groups' notes, before the answer's own note; a group
    // with its key never takes it.
    const QList<ReviewTags::TagGroup> groups{{s("before_note"), s("Clash"), {}}, {s("u"), s("Understanding"), {}}};
    QCOMPARE(ReviewTags::groupNoteLines({{s("note"), s("Redo")}, {s("before_note"), s("Unsure")}, {s("u"), s("Mostly")}}, groups),
             (QStringList{s("Understanding: Mostly"), s("Before note: Unsure"), s("Note: Redo")}));
}

void ReviewStoreTest::tagGroupsSyncAsOneQueuedRecord()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject projection;
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    QStringList errors;
    // No record yet: no groups, so every tag shows under Other.
    QVERIFY(store.tagGroups(&errors).isEmpty());
    QVERIFY(errors.isEmpty());

    // The phone's layout arrives through Sync; names read through the merges.
    const QString key = QStringLiteral("review_tag_groups\nreview-tags\ngroups");
    projection.insert(QStringLiteral("review_tag_merge\nreview-tags\nm"), tagMerge(s("m"), s("2026-10-05T10:00:00Z"), s("good"), {s("Good")}));
    projection.insert(key, tagGroupsRecord({tagGroup(s("u"), s("Understanding"), {s("Good"), s("wrong")})}, s("2026-10-08T21:00:00.000Z")));
    auto groups = store.tagGroups(&errors);
    QCOMPARE(groupNames(groups), QStringList{s("Understanding")});
    QCOMPARE(groups.constFirst().tags, (QStringList{s("good"), s("wrong")}));

    // An edit here writes the whole record, queued for the server, and is shown at once.
    QString error;
    QVERIFY(ReviewTags::addGroup(groups, s("Interest"), &error));
    QVERIFY(ReviewTags::moveTag(groups, s("cool"), groups.constLast().id, &error));
    QVERIFY2(store.saveTagGroups(groups, &error), qPrintable(error));
    QCOMPARE(store.tagGroups(), groups);
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    const auto queued = [&outbox] {
        QList<QJsonObject> result;
        for (const auto &operation : outbox.operations())
            if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_tag_groups")) result.append(operation);
        return result;
    };
    QCOMPARE(queued().size(), 1);
    const QJsonObject operation = queued().constFirst();
    QCOMPARE(operation.value(QStringLiteral("collection")).toString(), s("review-tags"));
    QCOMPARE(operation.value(QStringLiteral("entity_id")).toString(), s("groups"));
    const QJsonObject payload = operation.value(QStringLiteral("payload")).toObject();
    QList<ReviewTags::TagGroup> uploaded;
    QVERIFY2(ReviewTags::parseGroups(payload, &uploaded, &error), qPrintable(error));
    QCOMPARE(uploaded, groups);
    QCOMPARE(payload.value(QStringLiteral("groups")).toArray().first().toObject().value(QStringLiteral("tags")).toArray(),
             (QJsonArray{s("good"), s("wrong")}));
    QVERIFY(!payload.value(QStringLiteral("origin_id")).toString().isEmpty());
    const QDateTime updatedAt = QDateTime::fromString(payload.value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs);
    QVERIFY(updatedAt.isValid());
    QVERIFY(payload.value(QStringLiteral("updated_at")).toString().endsWith(QLatin1Char('Z')));
    const QStringList fields = payload.keys();
    QCOMPARE(QSet<QString>(fields.cbegin(), fields.cend()),
             (QSet<QString>{s("layout_id"), s("schema_version"), s("groups"), s("updated_at"), s("origin_id")}));
    // Invalid layouts are refused before anything is written.
    QVERIFY(!store.saveTagGroups({{s("x"), s("Other"), {}}}, &error));
    QCOMPARE(queued().size(), 1);

    // Last write wins: a newer layout from the phone replaces this one...
    projection.insert(key, tagGroupsRecord({tagGroup(s("w"), s("Work progress"), {s("done")})}, updatedAt.addSecs(60).toString(Qt::ISODateWithMs)));
    QCOMPARE(groupNames(store.tagGroups()), QStringList{s("Work progress")});
    // ...and an edit made after it still wins here, even when the phone's clock ran ahead.
    groups = store.tagGroups();
    QVERIFY(ReviewTags::renameGroup(groups, s("w"), s("Progress"), &error));
    QVERIFY2(store.saveTagGroups(groups, &error), qPrintable(error));
    QCOMPARE(groupNames(store.tagGroups()), QStringList{s("Progress")});
    QCOMPARE(queued().size(), 2);
    QVERIFY(QDateTime::fromString(queued().constLast().value(QStringLiteral("payload")).toObject().value(QStringLiteral("updated_at")).toString(), Qt::ISODateWithMs)
            > updatedAt.addSecs(60));

    // An invalid synced layout only loses the grouping.
    outbox.acknowledge({queued().at(0).value(QStringLiteral("operation_id")).toString(), queued().at(1).value(QStringLiteral("operation_id")).toString()});
    projection.insert(key, tagGroupsRecord({tagGroup(s("o"), s("Other"), {})}, updatedAt.addSecs(120).toString(Qt::ISODateWithMs)));
    errors.clear();
    QVERIFY(store.tagGroups(&errors).isEmpty());
    QCOMPARE(errors.size(), 1);
    // Tag merges are unaffected by the groups sharing their collection.
    errors.clear();
    QCOMPARE(store.tagMerges(&errors).value(s("Good")), s("good"));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));

    // A store without Sync keeps the layout in its own folder, apart from the merges.
    QTemporaryDir plainRoot;
    ReviewStore plain(plainRoot.path());
    QVERIFY(ReviewTags::addGroup(groups, s("Local"), &error));
    QVERIFY2(plain.saveTagGroups(groups, &error), qPrintable(error));
    QCOMPARE(plain.tagGroups(), groups);
    QVERIFY2(plain.recordTagMerge({s("a")}, s("b"), &error), qPrintable(error));
    errors.clear();
    QCOMPARE(plain.tagMerges(&errors).value(s("a")), s("b"));
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    errors.clear();
    QCOMPARE(plain.tagGroups(&errors), groups);
    QVERIFY(errors.isEmpty());
}

void ReviewStoreTest::tagGroupSlidersKeepTheirStepsAndUnknownFields()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject projection;
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    // A record written by a newer client: a slider group, a field of a group and a field of the
    // record this client does not know. A step merged into another tag reads under its new name.
    QJsonObject understanding = tagGroup(s("u"), s("Understanding"), {s("Don't get it"), s("kinda"), s("I get it"), s("Asked AI")});
    understanding.insert(QStringLiteral("scale"), QJsonArray{s("Don't get it"), s("kinda"), s("I get it")});
    understanding.insert(QStringLiteral("color"), s("#336699"));
    QJsonObject record = tagGroupsRecord({understanding, tagGroup(s("s"), s("Source"), {s("Me"), s("AI")})});
    record.insert(QStringLiteral("layout_hint"), QJsonObject{{QStringLiteral("compact"), true}});
    projection.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"), record);
    projection.insert(QStringLiteral("review_tag_merge\nreview-tags\nm"), tagMerge(s("m"), s("2026-10-05T10:00:00Z"), s("Partly"), {s("kinda")}));
    QStringList errors;
    auto groups = store.tagGroups(&errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(groups.size(), 2);
    QCOMPARE(groups.at(0).tags, (QStringList{s("Don't get it"), s("Partly"), s("I get it"), s("Asked AI")}));
    QCOMPARE(groups.at(0).scale, (QStringList{s("Don't get it"), s("Partly"), s("I get it")}));
    QCOMPARE(groups.at(0).extra.value(QStringLiteral("color")).toString(), s("#336699"));
    QVERIFY(groups.at(1).scale.isEmpty());

    // As on the phone: a step missing from the group's tags is added to them; one already in an
    // earlier group's tags is no step of this one.
    QJsonObject first = tagGroup(s("a"), s("A"), {s("x")});
    QJsonObject second = tagGroup(s("b"), s("B"), {s("y")});
    second.insert(QStringLiteral("scale"), QJsonArray{s("x"), s("y"), s("z")});
    QList<ReviewTags::TagGroup> phone;
    QString parseError;
    QVERIFY2(ReviewTags::parseGroups(tagGroupsRecord({first, second}), &phone, &parseError), qPrintable(parseError));
    QCOMPARE(phone.at(1).tags, (QStringList{s("y"), s("z")}));
    QCOMPARE(phone.at(1).scale, (QStringList{s("y"), s("z")}));

    // Steps are tags of their group, each once, at least two; a step moved out stops being one.
    QString error;
    auto edited = groups;
    QVERIFY(!ReviewTags::setScale(edited, s("s"), {s("Me"), s("Nope")}, &error));
    QVERIFY(!ReviewTags::setScale(edited, s("s"), {s("Me")}, &error));
    QVERIFY2(ReviewTags::setScale(edited, s("s"), {s("AI"), s("Me")}, &error), qPrintable(error));
    QCOMPARE(edited.at(1).scale, (QStringList{s("AI"), s("Me")}));
    QVERIFY(ReviewTags::groupsError({{s("x"), s("X"), {s("a")}, {s("a"), s("b")}}}).contains(QStringLiteral("not one of its tags")));
    QVERIFY(ReviewTags::moveTag(edited, s("I get it"), s("s"), &error));
    QCOMPARE(edited.at(0).scale, (QStringList{s("Don't get it"), s("Partly")}));
    QVERIFY(ReviewTags::moveTag(edited, s("Partly"), QString(), &error));
    QCOMPARE(edited.at(0).scale, QStringList{s("Don't get it")});
    QVERIFY(ReviewTags::setScale(edited, s("s"), {}, &error));
    QVERIFY(edited.at(1).scale.isEmpty());

    // Saving keeps what this client does not know, on the record and on each group, and
    // writes "scale" only for sliders.
    QVERIFY(ReviewTags::renameGroup(groups, s("s"), s("Who wrote it"), &error));
    QVERIFY2(store.saveTagGroups(groups, &error), qPrintable(error));
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    QJsonObject payload;
    for (const auto &operation : outbox.operations())
        if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_tag_groups")) payload = operation.value(QStringLiteral("payload")).toObject();
    QCOMPARE(payload.value(QStringLiteral("layout_hint")).toObject(), (QJsonObject{{QStringLiteral("compact"), true}}));
    const QJsonArray saved = payload.value(QStringLiteral("groups")).toArray();
    QCOMPARE(saved.at(0).toObject().value(QStringLiteral("color")).toString(), s("#336699"));
    QCOMPARE(saved.at(0).toObject().value(QStringLiteral("scale")).toArray(), (QJsonArray{s("Don't get it"), s("Partly"), s("I get it")}));
    QVERIFY(!saved.at(1).toObject().contains(QStringLiteral("scale")));
    QCOMPARE(saved.at(1).toObject().value(QStringLiteral("name")).toString(), s("Who wrote it"));
    QVERIFY(ReviewTags::recordHasScale(payload));
    QVERIFY(!ReviewTags::recordHasScale(tagGroupsRecord({tagGroup(s("s"), s("Source"), {s("Me")})})));
    QCOMPARE(store.tagGroups().at(0).scale, (QStringList{s("Don't get it"), s("Partly"), s("I get it")}));

    // A malformed scale only loses the slider; the next save leaves it out (the server would refuse it).
    QJsonObject odd = tagGroup(s("o"), s("Odd"), {s("a"), s("b")});
    odd.insert(QStringLiteral("scale"), s("a,b"));
    QList<ReviewTags::TagGroup> parsed;
    QVERIFY2(ReviewTags::parseGroups(tagGroupsRecord({odd}), &parsed, &error), qPrintable(error));
    QVERIFY(parsed.at(0).scale.isEmpty());
    QVERIFY(!ReviewTags::groupsRecord(parsed, s("2026-10-15T10:00:00.000Z"), s("mac")).value(QStringLiteral("groups")).toArray().at(0).toObject().contains(QStringLiteral("scale")));
}

void ReviewStoreTest::tagPlanPreviewsAndAppliesAReorganization()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QStringList ids;
    QJsonObject projection;
    for (int index = 0; index < 3; ++index) {
        ids.append(QUuid::createUuid().toString(QUuid::WithoutBraces));
        projection.insert(QStringLiteral("review_card\ncards\n") + ids.last(), reviewCard(ids.last(), s("paper"), QString(64, QLatin1Char('a'))));
    }
    projection.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"),
                      tagGroupsRecord({tagGroup(s("u"), s("Understanding"), {s("correct"), s("wrong"), s("I kind of get it"), s("I get it")}),
                                       tagGroup(s("e"), s("Effort / status"), {s("Read it fast"), s("Spent time on it")}),
                                       tagGroup(s("x"), s("Old"), {s("leftover")})}));
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime at(QDate(2026, 10, 12), QTime(9, 0), QTimeZone::UTC);
    QString error;
    const auto answer = [&](int card, int minute, const QStringList &tags) {
        return store.recordRating(ids.at(card), QStringLiteral("custom_interval"), at.addSecs(60 * minute), &error, at.addDays(3), nullptr, {}, tags);
    };
    QVERIFY2(answer(0, 0, {s("wrong")}), qPrintable(error));
    QVERIFY2(answer(0, 1, {s("correct"), s("I kind of get it"), s("Read it fast")}), qPrintable(error));
    QVERIFY2(answer(1, 2, {s("correct"), s("Spent time on it"), s("stray")}), qPrintable(error));

    const QJsonObject document{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("groups"), QJsonArray{
            QJsonObject{{QStringLiteral("name"), s("Understanding")}, {QStringLiteral("scale"), QJsonArray{
                QJsonObject{{QStringLiteral("tag"), s("Partly")}, {QStringLiteral("from"), QJsonArray{s("I kind of get it")}}}, s("I get it")}}},
            QJsonObject{{QStringLiteral("name"), s("Recall")}, {QStringLiteral("scale"), QJsonArray{
                QJsonObject{{QStringLiteral("tag"), s("Forgot")}, {QStringLiteral("from"), QJsonArray{s("Wrong")}}},
                QJsonObject{{QStringLiteral("tag"), s("Recalled")}, {QStringLiteral("from"), QJsonArray{s("correct"), s("Corect")}}}}}},
            QJsonObject{{QStringLiteral("name"), s("Study")}, {QStringLiteral("previous_names"), QJsonArray{s("Effort / status")}}, {QStringLiteral("tags"), QJsonArray{
                QJsonObject{{QStringLiteral("tag"), s("Skimmed")}, {QStringLiteral("from"), QJsonArray{s("Read it fast")}}},
                QJsonObject{{QStringLiteral("tag"), s("Studied")}, {QStringLiteral("from"), QJsonArray{s("Spent time on it")}}}}}}}},
        {QStringLiteral("knowledge_levels"), QJsonObject{{s("Forgot"), s("unknown")}, {s("Recalled"), s("solid")}}}};
    ReviewTagPlan::Plan plan;
    QVERIFY2(ReviewTagPlan::parse(document, &plan, &error), qPrintable(error));
    QCOMPARE(plan.merges.size(), 5);
    QCOMPARE(plan.merges.at(1).from, QStringList{s("wrong")}); // "Wrong" is the built-in.
    QCOMPARE(plan.groups.at(1).scale, (QStringList{s("Forgot"), s("Recalled")}));
    QCOMPARE(plan.levels.size(), 2);

    // Invalid plans say why.
    const auto refused = [&](QJsonObject changed, const QString &expected) {
        ReviewTagPlan::Plan ignored;
        QString message;
        if (ReviewTagPlan::parse(changed, &ignored, &message)) return QStringLiteral("accepted");
        return message.contains(expected) ? QString() : message;
    };
    QJsonObject twice = document;
    twice.insert(QStringLiteral("groups"), QJsonArray{QJsonObject{{QStringLiteral("name"), s("A")}, {QStringLiteral("tags"), QJsonArray{s("x"), s("x")}}}});
    QCOMPARE(refused(twice, s("listed twice")), QString());
    QJsonObject kept = document;
    kept.insert(QStringLiteral("groups"), QJsonArray{QJsonObject{{QStringLiteral("name"), s("A")}, {QStringLiteral("tags"), QJsonArray{
        s("x"), QJsonObject{{QStringLiteral("tag"), s("y")}, {QStringLiteral("from"), QJsonArray{s("x")}}}}}}});
    QCOMPARE(refused(kept, s("both a tag")), QString());
    QJsonObject typo = document;
    typo.insert(QStringLiteral("grups"), QJsonArray{});
    QCOMPARE(refused(typo, s("unknown field")), QString());
    QJsonObject oneStep = document;
    oneStep.insert(QStringLiteral("groups"), QJsonArray{QJsonObject{{QStringLiteral("name"), s("A")}, {QStringLiteral("scale"), QJsonArray{s("x")}}}});
    QCOMPARE(refused(oneStep, s("at least two steps")), QString());

    // The preview: counts per merge, the resulting groups, removed groups, what stays uncovered.
    QList<ReviewTagPlan::Merge> merges;
    QList<ReviewTags::TagGroup> layout;
    const QJsonObject report = ReviewTagPlan::evaluate(plan, store.cards(), store.tagMerges(), store.tagGroups(), &merges, &layout);
    QVERIFY2(report.value(QStringLiteral("accepted")).toBool(), QJsonDocument(report).toJson().constData());
    QCOMPARE(report.value(QStringLiteral("cards")).toInt(), 3);
    QCOMPARE(report.value(QStringLiteral("answers")).toInt(), 3);
    QCOMPARE(merges.size(), 5);
    const auto mergeReport = [&report](const QString &to) {
        for (const auto &value : report.value(QStringLiteral("merges")).toArray())
            if (value.toObject().value(QStringLiteral("to")).toString() == to) return value.toObject();
        return QJsonObject();
    };
    const QJsonArray recalled = mergeReport(s("Recalled")).value(QStringLiteral("from")).toArray();
    QCOMPARE(recalled.at(0).toObject().value(QStringLiteral("cards")).toInt(), 2);
    QCOMPARE(recalled.at(0).toObject().value(QStringLiteral("answers")).toInt(), 2);
    QCOMPARE(recalled.at(1).toObject().value(QStringLiteral("state")).toString(), s("not_found"));
    QCOMPARE(mergeReport(s("Forgot")).value(QStringLiteral("from")).toArray().at(0).toObject().value(QStringLiteral("cards")).toInt(), 0);
    QCOMPARE(mergeReport(s("Forgot")).value(QStringLiteral("from")).toArray().at(0).toObject().value(QStringLiteral("answers")).toInt(), 1);
    QCOMPARE(report.value(QStringLiteral("problems")).toArray().size(), 1);
    QVERIFY(report.value(QStringLiteral("problems")).toArray().at(0).toString().contains(s("Corect")));
    const QJsonArray resulting = report.value(QStringLiteral("groups")).toArray();
    QCOMPARE(resulting.size(), 3);
    QCOMPARE(resulting.at(0).toObject().value(QStringLiteral("group_id")).toString(), s("u"));
    QCOMPARE(resulting.at(1).toObject().value(QStringLiteral("state")).toString(), s("new"));
    QCOMPARE(resulting.at(2).toObject().value(QStringLiteral("group_id")).toString(), s("e"));
    QCOMPARE(resulting.at(2).toObject().value(QStringLiteral("previous_name")).toString(), s("Effort / status"));
    QCOMPARE(resulting.at(1).toObject().value(QStringLiteral("scale")).toArray().at(1).toObject().value(QStringLiteral("cards")).toInt(), 2);
    QCOMPARE(report.value(QStringLiteral("removed_groups")).toArray().at(0).toObject().value(QStringLiteral("name")).toString(), s("Old"));
    QStringList uncovered;
    for (const auto &value : report.value(QStringLiteral("not_covered")).toArray()) uncovered.append(value.toObject().value(QStringLiteral("tag")).toString());
    QCOMPARE(uncovered, (QStringList{s("stray"), s("leftover")}));
    QVERIFY(ReviewTagPlan::formatReport(report).contains(QStringLiteral("NOT FOUND")));
    // A preview writes nothing.
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    const auto queued = [&outbox](const QString &type) {
        QList<QJsonObject> result;
        for (const auto &operation : outbox.operations())
            if (operation.value(QStringLiteral("entity_type")).toString() == type) result.append(operation.value(QStringLiteral("payload")).toObject());
        return result;
    };
    QVERIFY(queued(s("review_tag_merge")).isEmpty());

    // Applying records one merge per target, queued as Compact tags queues them, and the
    // layout with its sliders as one groups record.
    const QJsonObject applied = ReviewTagPlan::apply(plan, store);
    QVERIFY2(applied.value(QStringLiteral("accepted")).toBool(), QJsonDocument(applied).toJson().constData());
    QCOMPARE(applied.value(QStringLiteral("applied")).toObject().value(QStringLiteral("merges_recorded")).toInt(), 5);
    QVERIFY(applied.value(QStringLiteral("applied")).toObject().value(QStringLiteral("groups_saved")).toBool());
    QCOMPARE(queued(s("review_tag_merge")).size(), 5);
    QCOMPARE(queued(s("review_tag_groups")).size(), 1);
    const auto groups = store.tagGroups();
    QCOMPARE(groupNames(groups), (QStringList{s("Understanding"), s("Recall"), s("Study")}));
    QCOMPARE(groups.at(0).scale, (QStringList{s("Partly"), s("I get it")}));
    QCOMPARE(groups.at(1).scale, (QStringList{s("Forgot"), s("Recalled")}));
    QCOMPARE(groups.at(2).id, s("e"));
    QVERIFY(groups.at(2).scale.isEmpty());
    QCOMPARE(groups.at(2).tags, (QStringList{s("Skimmed"), s("Studied")}));
    for (const auto &card : store.cards()) {
        if (card.id() == ids.at(0)) QCOMPARE(card.currentTags(), (QStringList{s("Recalled"), s("Partly"), s("Skimmed")}));
        if (card.id() == ids.at(1)) QCOMPARE(card.currentTags(), (QStringList{s("Recalled"), s("Studied"), s("stray")}));
    }
    // Old answers keep reading right: the legacy "wrong" answer is a Forgot now.
    QCOMPARE(store.reviewTagUsage().value(s("Recalled")), 2);
    QCOMPARE(ReviewTags::byPopularity(store.reviewTagUsage(), {}, store.tagMerges()).filter(s("correct")).size(), 0);

    // Applying again writes nothing.
    const QJsonObject again = ReviewTagPlan::apply(plan, store);
    QVERIFY(again.value(QStringLiteral("accepted")).toBool());
    QCOMPARE(again.value(QStringLiteral("applied")).toObject().value(QStringLiteral("merges_recorded")).toInt(), 0);
    QVERIFY(!again.value(QStringLiteral("applied")).toObject().value(QStringLiteral("groups_saved")).toBool());
    QCOMPARE(queued(s("review_tag_merge")).size(), 5);
    QCOMPARE(queued(s("review_tag_groups")).size(), 1);
    QVERIFY(again.value(QStringLiteral("message")).toString().contains(QStringLiteral("already applied")));
}

void ReviewStoreTest::tagGroupSectionsSurviveEveryRewrite()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject projection;
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    // Recall in no section, then Work left (trimmed when read) over two sliders, a group whose
    // section the server would refuse (too long), one with a malformed section, then Work done.
    QJsonObject recall = tagGroup(s("r"), s("Recall"), {s("Forgot"), s("Recalled")});
    recall.insert(QStringLiteral("scale"), QJsonArray{s("Forgot"), s("Recalled")});
    QJsonObject structure = tagGroup(s("st"), s("Structure"), {s("Missing steps"), s("Clear"), s("Messy")});
    structure.insert(QStringLiteral("scale"), QJsonArray{s("Missing steps"), s("Clear")});
    structure.insert(QStringLiteral("section"), s(" Work left "));
    structure.insert(QStringLiteral("color"), s("#336699"));
    QJsonObject style = tagGroup(s("sy"), s("Style"), {s("kinda clean"), s("Polished")});
    style.insert(QStringLiteral("section"), s("Work left"));
    QJsonObject longer = tagGroup(s("l"), s("Long"), {});
    longer.insert(QStringLiteral("section"), QString(61, QLatin1Char('x')));
    QJsonObject odd = tagGroup(s("o"), s("Odd"), {});
    odd.insert(QStringLiteral("section"), QJsonArray{s("Work left")});
    QJsonObject done = tagGroup(s("sd"), s("Structure done"), {s("Reordered")});
    done.insert(QStringLiteral("section"), s("Work done"));
    projection.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"), tagGroupsRecord({recall, structure, style, longer, odd, done}));
    // Compact tags merged a tag of Style: the group keeps its section.
    projection.insert(QStringLiteral("review_tag_merge\nreview-tags\nm"), tagMerge(s("m"), s("2026-10-05T10:00:00Z"), s("Clean"), {s("kinda clean")}));
    QStringList errors;
    auto groups = store.tagGroups(&errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(groups.size(), 6);
    QStringList sections;
    for (const auto &group : groups) sections.append(group.section);
    QCOMPARE(sections, (QStringList{QString(), s("Work left"), s("Work left"), QString(), QString(), s("Work done")}));
    QCOMPARE(groups.at(2).tags, (QStringList{s("Clean"), s("Polished")}));
    QCOMPARE(groups.at(1).extra.value(QStringLiteral("color")).toString(), s("#336699"));
    QVERIFY(!groups.at(1).extra.contains(QStringLiteral("section")));
    QCOMPARE(ReviewTags::sectionNames(groups), (QStringList{s("Work left"), s("Work done")}));
    // The picker's sections() keep it too, Other has none.
    const auto shown = ReviewTags::sections(groups, {s("Clean"), s("stray")});
    QCOMPARE(shown.at(2).section, s("Work left"));
    QVERIFY(shown.constLast().id.isEmpty() && shown.constLast().section.isEmpty());

    // Every edit keeps the sections of the groups it touches and of the others.
    QString error;
    auto edited = groups;
    QVERIFY2(ReviewTags::moveTag(edited, s("Messy"), s("sy"), &error), qPrintable(error));
    QVERIFY2(ReviewTags::moveTag(edited, s("Reordered"), QString(), &error), qPrintable(error));
    QVERIFY2(ReviewTags::setScale(edited, s("sy"), {s("Clean"), s("Polished")}, &error), qPrintable(error));
    QVERIFY2(ReviewTags::renameGroup(edited, s("st"), s("Structure left"), &error), qPrintable(error));
    QVERIFY(ReviewTags::moveGroup(edited, s("r"), 1));
    QVERIFY(ReviewTags::moveGroup(edited, s("r"), -1));
    QVERIFY(ReviewTags::deleteGroup(edited, s("o")));
    QVERIFY2(ReviewTags::addGroup(edited, s("Mood"), &error), qPrintable(error));
    QCOMPARE(edited.at(1).section, s("Work left"));
    QCOMPARE(edited.at(2).section, s("Work left"));
    QCOMPARE(edited.at(4).section, s("Work done"));
    QVERIFY(edited.constLast().section.isEmpty()); // A new group is in no section.

    // Section… sets (trimmed) and clears; names are checked as the server checks them.
    QVERIFY2(ReviewTags::setSection(edited, s("l"), s("  Work done "), &error), qPrintable(error));
    QCOMPARE(edited.at(3).section, s("Work done"));
    QVERIFY(!ReviewTags::setSection(edited, s("l"), QString(61, QLatin1Char('y')), &error));
    QVERIFY(error.contains(QStringLiteral("at most 60")));
    QVERIFY(ReviewTags::setSection(edited, s("l"), QString(60, QLatin1Char('y')), &error));
    QVERIFY(!ReviewTags::setSection(edited, s("gone"), s("Work left"), &error));
    QVERIFY(ReviewTags::groupsError({{s("x"), s("X"), {}, {}, s("   ")}}).contains(QStringLiteral("section")));
    QVERIFY(ReviewTags::setSection(edited, s("l"), QString(), &error));
    QVERIFY(edited.at(3).section.isEmpty());

    // Saving writes "section" only for groups in one, next to what this client does not know;
    // a cleared section is gone from the record although the stored group had one.
    QVERIFY2(ReviewTags::setSection(edited, s("sd"), QString(), &error), qPrintable(error));
    QVERIFY2(store.saveTagGroups(edited, &error), qPrintable(error));
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    QJsonObject payload;
    for (const auto &operation : outbox.operations())
        if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_tag_groups")) payload = operation.value(QStringLiteral("payload")).toObject();
    const QJsonArray saved = payload.value(QStringLiteral("groups")).toArray();
    QCOMPARE(saved.size(), 6);
    QCOMPARE(saved.at(1).toObject().value(QStringLiteral("section")).toString(), s("Work left"));
    QCOMPARE(saved.at(1).toObject().value(QStringLiteral("color")).toString(), s("#336699"));
    QCOMPARE(saved.at(2).toObject().value(QStringLiteral("section")).toString(), s("Work left"));
    QVERIFY(!saved.at(0).toObject().contains(QStringLiteral("section")));
    QVERIFY(!saved.at(3).toObject().contains(QStringLiteral("section")));
    QVERIFY(!saved.at(4).toObject().contains(QStringLiteral("section")));
    QVERIFY(ReviewTags::recordHasSection(payload));
    QVERIFY(!ReviewTags::recordHasSection(tagGroupsRecord({recall})));
    QCOMPARE(ReviewTags::sectionCapability(), s("review_tag_sections_v1"));
    // Read back, the layout is the one saved.
    QList<ReviewTags::TagGroup> parsed;
    QVERIFY2(ReviewTags::parseGroups(payload, &parsed, &error), qPrintable(error));
    QCOMPARE(parsed, edited);
}

void ReviewStoreTest::tagPlanSetsKeepsAndClearsSections()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject projection;
    QJsonObject structure = tagGroup(s("st"), s("Structure"), {s("Missing steps"), s("Clear")});
    structure.insert(QStringLiteral("scale"), QJsonArray{s("Missing steps"), s("Clear")});
    QJsonObject style = tagGroup(s("sy"), s("Style"), {s("Clean")});
    style.insert(QStringLiteral("section"), s("Old"));
    QJsonObject source = tagGroup(s("so"), s("Source"), {s("Me")});
    source.insert(QStringLiteral("section"), s("Origin"));
    projection.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"), tagGroupsRecord({structure, style, source}));
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    // Structure gains a section, Style moves to it, Source keeps its own (no "section"), a new
    // group gets one; then a plan with "section": null and "" takes them out again.
    const auto planOf = [&](const QJsonValue &styleSection, const QJsonValue &newSection) {
        QJsonObject styleGroup{{QStringLiteral("group_id"), s("sy")}, {QStringLiteral("name"), s("Style")}, {QStringLiteral("tags"), QJsonArray{s("Clean")}}};
        styleGroup.insert(QStringLiteral("section"), styleSection);
        QJsonObject newGroup{{QStringLiteral("name"), s("Content")}, {QStringLiteral("tags"), QJsonArray{s("Thin")}}};
        if (!newSection.isUndefined()) newGroup.insert(QStringLiteral("section"), newSection);
        return QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("groups"), QJsonArray{
            QJsonObject{{QStringLiteral("group_id"), s("st")}, {QStringLiteral("name"), s("Structure")}, {QStringLiteral("section"), s(" Work left ")},
                        {QStringLiteral("scale"), QJsonArray{s("Missing steps"), s("Clear")}}},
            styleGroup, newGroup,
            QJsonObject{{QStringLiteral("group_id"), s("so")}, {QStringLiteral("name"), s("Source")}, {QStringLiteral("tags"), QJsonArray{s("Me")}}}}}};
    };
    ReviewTagPlan::Plan plan;
    QString error;
    QVERIFY2(ReviewTagPlan::parse(planOf(s("Work left"), s("Work left")), &plan, &error), qPrintable(error));
    QVERIFY(plan.groups.at(0).section == std::optional<QString>(s("Work left")));
    QVERIFY(!plan.groups.at(3).section.has_value());
    const QJsonObject report = ReviewTagPlan::evaluate(plan, store.cards(), store.tagMerges(), store.tagGroups());
    QVERIFY2(report.value(QStringLiteral("accepted")).toBool(), QJsonDocument(report).toJson().constData());
    const QJsonArray resulting = report.value(QStringLiteral("groups")).toArray();
    QCOMPARE(resulting.at(0).toObject().value(QStringLiteral("section")).toString(), s("Work left"));
    QCOMPARE(resulting.at(0).toObject().value(QStringLiteral("previous_section")).toString(), QString());
    QCOMPARE(resulting.at(1).toObject().value(QStringLiteral("previous_section")).toString(), s("Old"));
    QCOMPARE(resulting.at(3).toObject().value(QStringLiteral("section")).toString(), s("Origin"));
    QVERIFY(!resulting.at(3).toObject().contains(QStringLiteral("previous_section")));
    const QString text = ReviewTagPlan::formatReport(report);
    QVERIFY2(text.contains(QStringLiteral("[section \"Work left\"]")) && text.contains(QStringLiteral("section was \"Old\"")), qPrintable(text));
    QCOMPARE(text.count(QStringLiteral("[section \"Work left\"]")), 1);
    QVERIFY(text.contains(QStringLiteral("review_tag_sections_v1")));

    const QJsonObject applied = ReviewTagPlan::apply(plan, store);
    QVERIFY2(applied.value(QStringLiteral("applied")).toObject().value(QStringLiteral("groups_saved")).toBool(), QJsonDocument(applied).toJson().constData());
    QStringList sections;
    for (const auto &group : store.tagGroups()) sections.append(group.section);
    QCOMPARE(sections, (QStringList{s("Work left"), s("Work left"), s("Work left"), s("Origin")}));
    QCOMPARE(store.tagGroups().at(0).scale, (QStringList{s("Missing steps"), s("Clear")}));
    // Applying again writes nothing; null and "" clear.
    QVERIFY(!ReviewTagPlan::apply(plan, store).value(QStringLiteral("applied")).toObject().value(QStringLiteral("groups_saved")).toBool());
    QVERIFY2(ReviewTagPlan::parse(planOf(QJsonValue(QJsonValue::Null), s("")), &plan, &error), qPrintable(error));
    QVERIFY(ReviewTagPlan::apply(plan, store).value(QStringLiteral("applied")).toObject().value(QStringLiteral("groups_saved")).toBool());
    sections.clear();
    for (const auto &group : store.tagGroups()) sections.append(group.section);
    QCOMPARE(sections, (QStringList{s("Work left"), QString(), QString(), s("Origin")}));

    // Invalid sections say why.
    QVERIFY(!ReviewTagPlan::parse(planOf(3, s("x")), &plan, &error));
    QVERIFY(error.contains(QStringLiteral("must be a string")));
    QVERIFY(!ReviewTagPlan::parse(planOf(QString(61, QLatin1Char('x')), s("x")), &plan, &error));
    QVERIFY(error.contains(QStringLiteral("at most 60")));
}

// review-phases spec: a group's "phase" says when it is asked; values this client does not know
// stay, unchanged, in the group's extra fields.
void ReviewStoreTest::tagGroupPhasesSurviveEveryRewrite()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject projection;
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    QJsonObject feeling = tagGroup(s("f"), s("Feeling"), {s("Meh"), s("Good")});
    feeling.insert(QStringLiteral("scale"), QJsonArray{s("Meh"), s("Good")});
    feeling.insert(QStringLiteral("phase"), ReviewTags::beforePhase());
    QJsonObject recall = tagGroup(s("r"), s("Recall"), {s("Forgot"), s("Partly recalled"), s("Recalled")});
    recall.insert(QStringLiteral("scale"), QJsonArray{s("Forgot"), s("Partly recalled"), s("Recalled")});
    recall.insert(QStringLiteral("phase"), s("before_and_after"));
    QJsonObject understanding = tagGroup(s("u"), s("Understanding"), {s("Lost"), s("I get it")});
    understanding.insert(QStringLiteral("scale"), QJsonArray{s("Lost"), s("I get it")});
    // A newer client's value and a malformed one: kept as they are, the groups asked after.
    QJsonObject later = tagGroup(s("l"), s("Later"), {s("Soon")});
    later.insert(QStringLiteral("phase"), s("during"));
    QJsonObject odd = tagGroup(s("o"), s("Odd"), {});
    odd.insert(QStringLiteral("phase"), 3);
    projection.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"), tagGroupsRecord({feeling, recall, understanding, later, odd}));
    QStringList errors;
    const auto groups = store.tagGroups(&errors);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(groups.size(), 5);
    QStringList phases;
    for (const auto &group : groups) phases.append(group.phase);
    QCOMPARE(phases, (QStringList{s("before"), s("before_and_after"), QString(), QString(), QString()}));
    QVERIFY(!groups.at(0).extra.contains(QStringLiteral("phase")));
    QCOMPARE(groups.at(3).extra.value(QStringLiteral("phase")).toString(), s("during"));
    QCOMPARE(groups.at(4).extra.value(QStringLiteral("phase")).toInt(), 3);
    QVERIFY(ReviewTags::askedBefore(groups.at(0)) && !ReviewTags::askedAfter(groups.at(0)));
    QVERIFY(ReviewTags::askedBefore(groups.at(1)) && ReviewTags::askedAfter(groups.at(1)));
    QVERIFY(!ReviewTags::askedBefore(groups.at(3)) && ReviewTags::askedAfter(groups.at(3)));
    // The recall group: the first one asked before and after with a scale.
    QVERIFY(ReviewTags::recallGroup(groups));
    QCOMPARE(ReviewTags::recallGroup(groups)->id, s("r"));
    QVERIFY(!ReviewTags::recallGroup({groups.at(0), groups.at(2)}));
    auto noScale = groups.at(1);
    noScale.scale.clear();
    QVERIFY(!ReviewTags::recallGroup({noScale}));
    // The picker's sections keep the phase too.
    QCOMPARE(ReviewTags::sections(groups, {}).at(1).phase, s("before_and_after"));

    // Every edit keeps the phases; Ask sets one, and replaces a newer client's value.
    QString error;
    auto edited = groups;
    QVERIFY2(ReviewTags::moveTag(edited, s("Soon"), s("u"), &error), qPrintable(error));
    QVERIFY2(ReviewTags::renameGroup(edited, s("f"), s("Mood"), &error), qPrintable(error));
    QVERIFY(ReviewTags::moveGroup(edited, s("r"), 1));
    QVERIFY(ReviewTags::moveGroup(edited, s("r"), -1));
    QVERIFY2(ReviewTags::setSection(edited, s("f"), s("Before"), &error), qPrintable(error));
    QCOMPARE(edited.at(0).phase, s("before"));
    QCOMPARE(edited.at(1).phase, s("before_and_after"));
    QVERIFY2(ReviewTags::setPhase(edited, s("u"), ReviewTags::beforePhase(), &error), qPrintable(error));
    QCOMPARE(edited.at(2).phase, s("before"));
    QVERIFY2(ReviewTags::setPhase(edited, s("u"), QString(), &error), qPrintable(error));
    QVERIFY(edited.at(2).phase.isEmpty());
    QVERIFY(!ReviewTags::setPhase(edited, s("gone"), ReviewTags::beforePhase(), &error));
    QVERIFY(!ReviewTags::setPhase(edited, s("u"), s("during"), &error));
    QVERIFY(edited.at(2).phase.isEmpty());
    QVERIFY(!ReviewTags::groupsError({{s("x"), s("X"), {}, {}, {}, {}, s("sometimes")}}).isEmpty());

    // Saving writes "phase" for the groups that have one, keeps the newer client's value, and
    // needs the server's review_tag_phases_v1.
    QVERIFY2(store.saveTagGroups(edited, &error), qPrintable(error));
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    QJsonObject payload;
    for (const auto &operation : outbox.operations())
        if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_tag_groups")) payload = operation.value(QStringLiteral("payload")).toObject();
    const QJsonArray saved = payload.value(QStringLiteral("groups")).toArray();
    QCOMPARE(saved.size(), 5);
    QCOMPARE(saved.at(0).toObject().value(QStringLiteral("phase")).toString(), s("before"));
    QCOMPARE(saved.at(1).toObject().value(QStringLiteral("phase")).toString(), s("before_and_after"));
    QVERIFY(!saved.at(2).toObject().contains(QStringLiteral("phase")));
    QCOMPARE(saved.at(3).toObject().value(QStringLiteral("phase")).toString(), s("during"));
    QCOMPARE(saved.at(4).toObject().value(QStringLiteral("phase")).toInt(), 3);
    QVERIFY(ReviewTags::recordHasPhase(payload));
    QVERIFY(!ReviewTags::recordHasPhase(tagGroupsRecord({understanding})));
    QCOMPARE(ReviewTags::phaseCapability(), s("review_tag_phases_v1"));
    // Read back, the layout is the one saved.
    QList<ReviewTags::TagGroup> parsed;
    QVERIFY2(ReviewTags::parseGroups(payload, &parsed, &error), qPrintable(error));
    QCOMPARE(parsed, edited);
    // Ask on a newer client's value replaces it.
    QVERIFY2(ReviewTags::setPhase(edited, s("l"), ReviewTags::beforeAndAfterPhase(), &error), qPrintable(error));
    QVERIFY(!edited.at(3).extra.contains(QStringLiteral("phase")));
    QCOMPARE(ReviewTags::groupsRecord(edited, s("2026-11-03T12:00:00.000Z"), s("desktop")).value(QStringLiteral("groups")).toArray().at(3).toObject()
                 .value(QStringLiteral("phase")).toString(), s("before_and_after"));
}

void ReviewStoreTest::tagPlanSetsKeepsAndClearsPhases()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject projection;
    QJsonObject feeling = tagGroup(s("f"), s("Feeling"), {s("Meh"), s("Good")});
    feeling.insert(QStringLiteral("scale"), QJsonArray{s("Meh"), s("Good")});
    feeling.insert(QStringLiteral("phase"), s("before"));
    QJsonObject recall = tagGroup(s("r"), s("Recall"), {s("Forgot"), s("Recalled")});
    recall.insert(QStringLiteral("scale"), QJsonArray{s("Forgot"), s("Recalled")});
    projection.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"), tagGroupsRecord({feeling, recall}));
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    // Feeling keeps its phase (no "phase"), Recall gets one; then "after" and null clear them.
    const auto planOf = [&](const QJsonValue &feelingPhase, const QJsonValue &recallPhase) {
        QJsonObject feelingGroup{{QStringLiteral("group_id"), s("f")}, {QStringLiteral("name"), s("Feeling")}, {QStringLiteral("scale"), QJsonArray{s("Meh"), s("Good")}}};
        if (!feelingPhase.isUndefined()) feelingGroup.insert(QStringLiteral("phase"), feelingPhase);
        return QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("groups"), QJsonArray{
            feelingGroup,
            QJsonObject{{QStringLiteral("group_id"), s("r")}, {QStringLiteral("name"), s("Recall")}, {QStringLiteral("phase"), recallPhase},
                        {QStringLiteral("scale"), QJsonArray{s("Forgot"), s("Recalled")}}}}}};
    };
    ReviewTagPlan::Plan plan;
    QString error;
    QVERIFY2(ReviewTagPlan::parse(planOf(QJsonValue(QJsonValue::Undefined), s("before_and_after")), &plan, &error), qPrintable(error));
    QVERIFY(!plan.groups.at(0).phase.has_value());
    QVERIFY(plan.groups.at(1).phase == std::optional<QString>(s("before_and_after")));
    const QJsonObject report = ReviewTagPlan::evaluate(plan, store.cards(), store.tagMerges(), store.tagGroups());
    QVERIFY2(report.value(QStringLiteral("accepted")).toBool(), QJsonDocument(report).toJson().constData());
    const QJsonArray resulting = report.value(QStringLiteral("groups")).toArray();
    QCOMPARE(resulting.at(0).toObject().value(QStringLiteral("phase")).toString(), s("before"));
    QVERIFY(!resulting.at(0).toObject().contains(QStringLiteral("previous_phase")));
    QCOMPARE(resulting.at(1).toObject().value(QStringLiteral("phase")).toString(), s("before_and_after"));
    QCOMPARE(resulting.at(1).toObject().value(QStringLiteral("previous_phase")).toString(), QString());
    const QString text = ReviewTagPlan::formatReport(report);
    QVERIFY2(text.contains(QStringLiteral("[asked before]")) && text.contains(QStringLiteral("[asked before and after]"))
                 && text.contains(QStringLiteral("was asked after")) && text.contains(QStringLiteral("review_tag_phases_v1")), qPrintable(text));
    QVERIFY(ReviewTagPlan::apply(plan, store).value(QStringLiteral("applied")).toObject().value(QStringLiteral("groups_saved")).toBool());
    QCOMPARE(store.tagGroups().at(0).phase, s("before"));
    QCOMPARE(store.tagGroups().at(1).phase, s("before_and_after"));
    // "after" and null: asked after the answer only.
    QVERIFY2(ReviewTagPlan::parse(planOf(s("after"), QJsonValue(QJsonValue::Null)), &plan, &error), qPrintable(error));
    QVERIFY(ReviewTagPlan::apply(plan, store).value(QStringLiteral("applied")).toObject().value(QStringLiteral("groups_saved")).toBool());
    QVERIFY(store.tagGroups().at(0).phase.isEmpty());
    QVERIFY(store.tagGroups().at(1).phase.isEmpty());
    // Other values say why.
    QVERIFY(!ReviewTagPlan::parse(planOf(s("during"), QJsonValue(QJsonValue::Null)), &plan, &error));
    QVERIFY2(error.contains(QStringLiteral("\"phase\" must be")), qPrintable(error));
    QVERIFY(!ReviewTagPlan::parse(planOf(1, QJsonValue(QJsonValue::Null)), &plan, &error));
}

void ReviewStoreTest::syncedTagMergesApplyWhereTagsAreRead()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + first, reviewCard(first, QStringLiteral("paper"), QString(64, QLatin1Char('a')))},
                           {QStringLiteral("review_card\ncards\n") + second, reviewCard(second, QStringLiteral("paper"), QString(64, QLatin1Char('a')))}};
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime at(QDate(2026, 10, 5), QTime(9, 0), QTimeZone::UTC);
    QString error;
    // An answer from before tags existed reads as "correct"; another was tagged "Good".
    QVERIFY2(store.recordRating(first, QStringLiteral("good"), at, &error), qPrintable(error));
    QVERIFY2(store.recordRating(second, QStringLiteral("custom_interval"), at, &error, at.addSecs(600), nullptr, {},
                                {QStringLiteral("Good"), QStringLiteral("hard")}), qPrintable(error));
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("correct"), 1}, {QStringLiteral("Good"), 1}, {QStringLiteral("hard"), 1}}));

    // The phone's merge arrives through Sync as a review_tag_merge projection.
    const QString mergeId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    projection.insert(QStringLiteral("review_tag_merge\nreview-tags\n") + mergeId,
                      tagMerge(mergeId, QStringLiteral("2026-10-05T10:00:00Z"), QStringLiteral("good"), {QStringLiteral("Good"), QStringLiteral("correct")}));
    QCOMPARE(store.tagMerges(), (QHash<QString, QString>{{QStringLiteral("Good"), QStringLiteral("good")}, {QStringLiteral("correct"), QStringLiteral("good")}}));
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("good"), 2}, {QStringLiteral("hard"), 1}}));
    const auto browser = store.browserCards(at.addSecs(60));
    QCOMPARE(browser.size(), 2);
    for (const auto &item : browser) {
        QCOMPARE(item.card.currentTags().constFirst(), QStringLiteral("good"));
        QVERIFY(item.matchesFilter(QStringLiteral("tag:good")));
        // An old name finds the cards now shown under the merged one.
        QVERIFY(item.matchesFilter(QStringLiteral("tag:correct")));
        // Recorded answers are not rewritten.
        QVERIFY(!item.card.history.isEmpty());
        QVERIFY(!item.card.history.constLast().value(QStringLiteral("review_tags")).toArray().contains(QStringLiteral("good")));
    }
    // A malformed merge is ignored without making the cards unavailable.
    projection.insert(QStringLiteral("review_tag_merge\nreview-tags\nbroken"), QJsonObject{{QStringLiteral("merge_id"), QStringLiteral("broken")}});
    QStringList errors;
    QCOMPARE(store.cards(&errors).size(), 2);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QLatin1Char('\n'))));
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("good"), 2}, {QStringLiteral("hard"), 1}}));

    // Compact tags on this computer: the merge applies at once and is queued for Fedora.
    QVERIFY(!store.recordTagMerge({QStringLiteral("good")}, QStringLiteral("good"), &error));
    error.clear();
    QVERIFY2(store.recordTagMerge({QStringLiteral("hard"), QStringLiteral(" hard ")}, QStringLiteral("Tough"), &error), qPrintable(error));
    QCOMPARE(store.tagMerges().value(QStringLiteral("hard")), QStringLiteral("Tough"));
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("good"), 2}, {QStringLiteral("Tough"), 1}}));
    // A later merge of a merge target carries its sources along, as on the phone.
    QVERIFY2(store.recordTagMerge({QStringLiteral("Tough")}, QStringLiteral("good"), &error), qPrintable(error));
    QCOMPARE(store.tagMerges().value(QStringLiteral("hard")), QStringLiteral("good"));
    // A card counts a tag once, even when two of its current tags now share a name.
    QCOMPARE(store.reviewTagUsage(), (QHash<QString, int>{{QStringLiteral("good"), 2}}));
    DesktopSyncOutbox outbox(QDir(root.path()).filePath(QStringLiteral("sync/desktop-sync.sqlite")));
    QList<QJsonObject> queued;
    for (const auto &operation : outbox.operations())
        if (operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_tag_merge")) queued.append(operation);
    QCOMPARE(queued.size(), 2);
    const QJsonObject payload = queued.constFirst().value(QStringLiteral("payload")).toObject();
    QCOMPARE(queued.constFirst().value(QStringLiteral("collection")).toString(), QStringLiteral("review-tags"));
    QCOMPARE(queued.constFirst().value(QStringLiteral("entity_id")).toString(), payload.value(QStringLiteral("merge_id")).toString());
    QCOMPARE(payload.value(QStringLiteral("schema_version")).toInt(), 1);
    QCOMPARE(payload.value(QStringLiteral("from")).toArray(), QJsonArray{QStringLiteral("hard")});
    QCOMPARE(payload.value(QStringLiteral("to")).toString(), QStringLiteral("Tough"));
    QVERIFY(!payload.value(QStringLiteral("origin_id")).toString().isEmpty());
    QVERIFY(QDateTime::fromString(payload.value(QStringLiteral("created_at")).toString(), Qt::ISODateWithMs).isValid());
    const QStringList fields = payload.keys();
    QCOMPARE(QSet<QString>(fields.cbegin(), fields.cend()),
             (QSet<QString>{QStringLiteral("schema_version"), QStringLiteral("merge_id"), QStringLiteral("from"), QStringLiteral("to"),
                            QStringLiteral("created_at"), QStringLiteral("origin_id")}));
}

void ReviewStoreTest::tagPickerCountsAndOrdersTagsByUsage()
{
    const auto s = [](const char *text) { return QString::fromUtf8(text); };
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QStringList ids;
    QJsonObject projection;
    for (int index = 0; index < 6; ++index) {
        ids << QUuid::createUuid().toString(QUuid::WithoutBraces);
        projection.insert(s("review_card\ncards\n") + ids.constLast(), reviewCard(ids.constLast(), s("paper"), QString(64, QLatin1Char('a'))));
    }
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(s("sync")));
    const QDateTime at(QDate(2026, 10, 10), QTime(9, 0), QTimeZone::UTC);
    QString error;
    const auto answer = [&](const QString &card, int minute, const QStringList &tags, QString *eventId = nullptr) {
        return store.recordRating(card, s("custom_interval"), at.addSecs(60 * minute), &error, at.addDays(30), eventId, {}, tags);
    };
    // Usage is the number of cards whose current tags (their latest answer in effect) include a
    // tag, as on the phone: a card with several tags counts for each; earlier answers do not count.
    QVERIFY2(answer(ids[0], 0, {s("correct"), s("cool"), s("zed"), s("old")}), qPrintable(error));
    // Two of its tags become one name through a merge: the card counts the name once.
    QVERIFY2(answer(ids[0], 1, {s("correct"), s("hard"), s("tough"), s("zed")}), qPrintable(error));
    // An undone answer counts for nothing: the card is back to its answer before.
    QString undone;
    QVERIFY2(answer(ids[1], 0, {s("wrong")}), qPrintable(error));
    QVERIFY2(answer(ids[1], 1, {s("hard"), s("Apple")}, &undone), qPrintable(error));
    QVERIFY2(store.retractRating(ids[1], undone, at.addSecs(90), &error), qPrintable(error));
    // A deleted card counts for nothing, whatever its last answer was; its tag is still listed.
    QVERIFY2(answer(ids[2], 0, {s("basil")}), qPrintable(error));
    QVERIFY2(store.deleteCard(ids[2], at.addSecs(600), &error), qPrintable(error));
    // One answer given to several cards at once (a document or project answer) counts each card.
    QVERIFY2(answer(ids[3], 5, {s("cool")}), qPrintable(error));
    QVERIFY2(answer(ids[4], 5, {s("cool")}), qPrintable(error));
    QVERIFY2(answer(ids[5], 5, {s("cool"), s("zed")}), qPrintable(error));
    // A skip keeps the card's tags.
    ReviewSkipBatch batch;
    QVERIFY2(store.skip(ids[5], ReviewSkipScope::Card, at.addDays(40), &batch, &error, at.addSecs(60 * 6)), qPrintable(error));
    QVERIFY2(store.recordTagMerge({s("tough")}, s("hard"), &error), qPrintable(error));
    const QHash<QString, int> usage = store.reviewTagUsage();
    // "old" and "basil" are listed with no card: only earlier answers or a deleted card have them.
    QCOMPARE(usage, (QHash<QString, int>{{s("correct"), 1}, {s("zed"), 2}, {s("cool"), 3}, {s("hard"), 1}, {s("wrong"), 1},
                                         {s("old"), 0}, {s("basil"), 0}}));
    QCOMPARE(usage.value(s("never used")), 0);

    // Most-used first; ties by the label shown, ignoring case; never used last, by label too.
    const auto label = [&s](const QString &tag) { return tag == QLatin1String("correct") ? s("Correct") : tag == QLatin1String("wrong") ? s("Wrong") : tag; };
    QCOMPARE(ReviewTags::byUsage({s("wrong"), s("Beta"), s("hard"), s("alpha"), s("zed"), s("cool"), s("correct")}, usage, label),
             (QStringList{s("cool"), s("zed"), s("correct"), s("hard"), s("wrong"), s("alpha"), s("Beta")}));
    // Ties go by the label, not the stored name.
    const auto renamed = [&s](const QString &tag) { return tag == QLatin1String("wrong") ? s("a wrong answer") : tag; };
    QCOMPARE(ReviewTags::byUsage({s("hard"), s("wrong"), s("zed")}, usage, renamed), (QStringList{s("zed"), s("wrong"), s("hard")}));

    // Chips: "label (count)", "(0)" included; a long label is cut, never its count.
    QCOMPARE(ReviewTags::withCount(s("Correct"), 42), s("Correct (42)"));
    QCOMPARE(ReviewTags::chipText(s("Correct"), 42), s("Correct (42)"));
    QCOMPARE(ReviewTags::chipText(s("cool"), 0), s("cool (0)"));
    const QString longTag = s("never submitted and a lot of suffering reading this");
    QCOMPARE(ReviewTags::chipText(longTag, 1234), s("never submitted and… (1234)"));
    QCOMPARE(ReviewTags::withCount(longTag, 1234), longTag + s(" (1234)"));
    QCOMPARE(ReviewTags::chipText(s("Supercalifragilisticexpialidocious"), 7), s("Supercalifragilisticex… (7)"));
}

void ReviewStoreTest::overlappingMarkFiltersAndNeverIntervals()
{
    const auto now = QDateTime::fromString(QStringLiteral("2026-09-22T10:00:00Z"), Qt::ISODate);
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    ReviewBrowserCard item;
    item.card.record = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    QVERIFY(item.matchesFilter(QStringLiteral("new")));
    QVERIFY(!item.matchesFilter(QStringLiteral("seen")));
    QJsonObject wrong{{QStringLiteral("event_id"), QStringLiteral("wrong")}, {QStringLiteral("card_id"), id},
        {QStringLiteral("rating"), QStringLiteral("retained")}, {QStringLiteral("review_action"), QStringLiteral("wrong")},
        {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)}};
    item.card.history = {wrong};
    item.card.schedule = ReviewStore::foldEvents(item.card.history).value(id);
    item.hasIssues = true;
    for (const auto &filter : {"seen", "never", "tag:wrong", "issues", "all"}) QVERIFY(item.matchesFilter(QString::fromLatin1(filter)));
    QVERIFY(item.matchesFilter(QStringLiteral("unavailable")));
    QVERIFY(!item.matchesFilter(QStringLiteral("tag:correct")));
    item.state = ReviewBrowserState::Retained;
    QVERIFY(!item.canReview());
    item.state = ReviewBrowserState::NeedsAttention;
    QVERIFY(!item.canReview());
    item.hasIssues = false;
    item.state = ReviewBrowserState::Retained;
    QVERIFY(item.canReview());
    QJsonObject skip{{QStringLiteral("event_id"), QStringLiteral("skip")}, {QStringLiteral("card_id"), id},
        {QStringLiteral("rating"), QStringLiteral("custom_interval")}, {QStringLiteral("review_action"), QStringLiteral("skip")},
        {QStringLiteral("reviewed_at"), now.addSecs(1).toString(Qt::ISODate)},
        {QStringLiteral("scheduled_due_at"), now.addDays(1).toString(Qt::ISODate)}};
    item.card.history << skip;
    item.card.schedule = ReviewStore::foldEvents(item.card.history).value(id);
    QVERIFY(!item.matchesFilter(QStringLiteral("never")));
    // An older skip recorded as an answer postpones the card but keeps its tag.
    QVERIFY(!item.matchesFilter(QStringLiteral("tag:skip")));
    QVERIFY(item.matchesFilter(QStringLiteral("tag:wrong")));
    QCOMPARE(item.card.schedule.dueAt, now.addDays(1));
    QJsonObject undo{{QStringLiteral("event_id"), QStringLiteral("undo")}, {QStringLiteral("card_id"), id},
        {QStringLiteral("retracted_event_id"), QStringLiteral("skip")}, {QStringLiteral("reviewed_at"), now.addSecs(2).toString(Qt::ISODate)}};
    item.card.history << undo;
    item.card.schedule = ReviewStore::foldEvents(item.card.history).value(id);
    QVERIFY(item.matchesFilter(QStringLiteral("never")));
    QVERIFY(item.matchesFilter(QStringLiteral("tag:wrong")));
    // A postponement alone is no answer: the card is still new and has no tag.
    item.card.history = {QJsonObject{{QStringLiteral("card_id"), id}, {QStringLiteral("event_id"), QStringLiteral("legacy")},
        {QStringLiteral("queue_action"), QStringLiteral("skip")}, {QStringLiteral("skip_id"), QStringLiteral("batch")},
        {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)}, {QStringLiteral("skip_until"), now.addSecs(600).toString(Qt::ISODate)}}};
    QVERIFY(item.matchesFilter(QStringLiteral("new")));
    QVERIFY(!item.matchesFilter(QStringLiteral("tag:skip")));
    item.card.history << QJsonObject{{QStringLiteral("card_id"), id}, {QStringLiteral("event_id"), QStringLiteral("undo-queue")},
        {QStringLiteral("queue_action"), QStringLiteral("undo_skip")}, {QStringLiteral("skip_id"), QStringLiteral("batch")}};
    QVERIFY(item.matchesFilter(QStringLiteral("new")));
}

void ReviewStoreTest::retainedAndCustomEventsRespectRetractions()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-07T08:00:00Z"), Qt::ISODate);
    const auto event = [&](const QString &id, const QString &rating, int seconds) {
        return QJsonObject{{QStringLiteral("event_id"), id}, {QStringLiteral("card_id"), QStringLiteral("card")},
                           {QStringLiteral("rating"), rating}, {QStringLiteral("scheduler_version"), 2},
                           {QStringLiteral("reviewed_at"), now.addSecs(seconds).toString(Qt::ISODate)}};
    };
    QJsonObject custom = event(QStringLiteral("custom"), QStringLiteral("custom_interval"), 0);
    custom.insert(QStringLiteral("scheduled_due_at"), now.addMonths(5).toString(Qt::ISODate));
    QList<QJsonObject> events{custom, event(QStringLiteral("retained"), QStringLiteral("retained"), 1),
                             event(QStringLiteral("ignored"), QStringLiteral("next_week"), 2)};
    auto schedule = ReviewStore::foldEvents(events).value(QStringLiteral("card"));
    QVERIFY(!schedule.retained);
    QCOMPARE(schedule.reviewCount, 3);
    QCOMPARE(schedule.lastRating, QStringLiteral("next_week"));
    events.append(event(QStringLiteral("delete"), QStringLiteral("delete"), 3));
    QVERIFY(ReviewStore::foldEvents(events).value(QStringLiteral("card")).deleted);
    QJsonObject undoDelete = event(QStringLiteral("undo-delete"), QString(), 4);
    undoDelete.insert(QStringLiteral("retracted_event_id"), QStringLiteral("delete"));
    events.append(undoDelete);
    QVERIFY(ReviewStore::foldEvents(events).value(QStringLiteral("card")).deleted);
    auto inferredDelete = event(QStringLiteral("inferred-delete"), QStringLiteral("delete"), 5);
    inferredDelete.insert(QStringLiteral("origin_id"), QStringLiteral("thoughts-source-review"));
    inferredDelete.insert(QStringLiteral("reason"), QStringLiteral("source_markers_removed"));
    QVERIFY(!ReviewStore::foldEvents({custom, inferredDelete}).value(QStringLiteral("card")).deleted);
    auto inferredFileDelete = event(QStringLiteral("inferred-file-delete"), QStringLiteral("delete"), 5);
    inferredFileDelete.insert(QStringLiteral("origin_id"), QStringLiteral("thoughts-document-deletion"));
    inferredFileDelete.insert(QStringLiteral("reason"), QStringLiteral("document_deleted"));
    inferredFileDelete.insert(QStringLiteral("document_deletion"), QJsonObject{
        {QStringLiteral("source_files"), QJsonArray{QStringLiteral("docs-paper/tex/source/main.tex")}},
        {QStringLiteral("filenames"), QJsonArray{}},
        {QStringLiteral("project_document_removed"), false}});
    QVERIFY(!ReviewStore::foldEvents({custom, inferredFileDelete}).value(QStringLiteral("card")).deleted);
    events.removeLast();
    events.removeLast(); // Test ordinary rating undo independently of permanent deletion.
    // Retraction of a new Android rating must recompute the earlier schedule.
    QJsonObject undoRetained = event(QStringLiteral("undo-retained"), QString(), 5);
    undoRetained.insert(QStringLiteral("retracted_event_id"), QStringLiteral("retained"));
    events.append(undoRetained);
    schedule = ReviewStore::foldEvents(events).value(QStringLiteral("card"));
    QVERIFY(!schedule.retained);
    QCOMPARE(schedule.dueAt, now.addSecs(2).addDays(7));
    QJsonObject undoWeek = event(QStringLiteral("undo-week"), QString(), 6);
    undoWeek.insert(QStringLiteral("retracted_event_id"), QStringLiteral("ignored"));
    events.append(undoWeek);
    events.append(custom); // Duplicate synced event is applied only once.
    schedule = ReviewStore::foldEvents(events).value(QStringLiteral("card"));
    QCOMPARE(schedule.dueAt, now.addMonths(5));
    QCOMPARE(schedule.reviewCount, 1);
    QJsonObject undoCustom = event(QStringLiteral("undo-custom"), QString(), 7);
    undoCustom.insert(QStringLiteral("retracted_event_id"), QStringLiteral("custom"));
    events.append(undoCustom);
    QVERIFY(ReviewStore::foldEvents(events).isEmpty());
}

void ReviewStoreTest::explicitDatesRejectMalformedEvents()
{
    const QDateTime now = QDateTime::fromString(QStringLiteral("2026-09-07T08:00:00Z"), Qt::ISODate);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QString cardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    for (const QDateTime &due : {QDateTime(), now, now.addSecs(-1)}) {
        QString error;
        QVERIFY(!store.recordRating(cardId, QStringLiteral("custom_interval"), now, &error, due));
        QVERIFY(!error.isEmpty());
        const QJsonObject event{{QStringLiteral("event_id"), QStringLiteral("invalid")},
                                {QStringLiteral("card_id"), cardId},
                                {QStringLiteral("rating"), QStringLiteral("custom_interval")},
                                {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)},
                                {QStringLiteral("scheduled_due_at"), due.toString(Qt::ISODate)}};
        QStringList errors;
        QVERIFY(ReviewStore::foldEvents({event}, &errors).isEmpty());
        QCOMPARE(errors.size(), 1);
    }
    QVERIFY(!QFileInfo::exists(QDir(temporary.path()).filePath(QStringLiteral("reviews/events"))));
}

void ReviewStoreTest::deleteIsTerminalAndEventsAreIdempotent()
{
    QList<QJsonObject> events;
    events.append({
        {QStringLiteral("event_id"), QStringLiteral("one")},
        {QStringLiteral("card_id"), QStringLiteral("card")},
        {QStringLiteral("rating"), QStringLiteral("good")},
        {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-05T12:00:00Z")},
    });
    events.append({
        {QStringLiteral("event_id"), QStringLiteral("two")},
        {QStringLiteral("card_id"), QStringLiteral("card")},
        {QStringLiteral("rating"), QStringLiteral("delete")},
        {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-06T12:00:00Z")},
    });
    events.prepend(events.constFirst());
    events.append({
        {QStringLiteral("event_id"), QStringLiteral("three")},
        {QStringLiteral("card_id"), QStringLiteral("card")},
        {QStringLiteral("rating"), QStringLiteral("easy")},
        {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-07T12:00:00Z")},
    });

    const QHash<QString, ReviewSchedule> schedules = ReviewStore::foldEvents(events);

    QVERIFY(schedules.value(QStringLiteral("card")).deleted);
    QCOMPARE(schedules.value(QStringLiteral("card")).dueAt, QDateTime(QDate(2026, 8, 6), QTime(12, 0), QTimeZone::UTC));
}

void ReviewStoreTest::ratingRetractionsRecomputeSchedule()
{
    const QList<QJsonObject> events = {
        {
            {QStringLiteral("event_id"), QStringLiteral("first")},
            {QStringLiteral("card_id"), QStringLiteral("card")},
            {QStringLiteral("rating"), QStringLiteral("hard")},
            {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-04T12:00:00Z")},
        },
        {
            {QStringLiteral("event_id"), QStringLiteral("deleted")},
            {QStringLiteral("card_id"), QStringLiteral("card")},
            {QStringLiteral("rating"), QStringLiteral("delete")},
            {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-05T12:00:00Z")},
        },
        {
            {QStringLiteral("event_id"), QStringLiteral("retraction")},
            {QStringLiteral("card_id"), QStringLiteral("card")},
            {QStringLiteral("retracted_event_id"), QStringLiteral("deleted")},
            {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-05T12:00:01Z")},
        },
        {
            {QStringLiteral("event_id"), QStringLiteral("wrong-card-retraction")},
            {QStringLiteral("card_id"), QStringLiteral("other-card")},
            {QStringLiteral("retracted_event_id"), QStringLiteral("first")},
            {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-05T12:00:02Z")},
        },
    };

    const QHash<QString, ReviewSchedule> schedules = ReviewStore::foldEvents(events);

    QVERIFY(schedules.value(QStringLiteral("card")).deleted);
    auto reversed = events;
    std::reverse(reversed.begin(), reversed.end());
    QVERIFY(ReviewStore::foldEvents(reversed).value(QStringLiteral("card")).deleted);
}

void ReviewStoreTest::agentRequestCarriesGenerationOverrides()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QVariantMap document = {{QStringLiteral("pdf_path"), QStringLiteral("/tmp/paper.pdf")},
                                  {QStringLiteral("pdf_stem"), QStringLiteral("paper")},
                                  {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                                  {QStringLiteral("page_count"), 12}};

    const QJsonObject defaultRequest = store.agentRequest(document);
    QVERIFY(!defaultRequest.contains(QStringLiteral("agent_model")));
    QVERIFY(!defaultRequest.contains(QStringLiteral("agent_reasoning_effort")));
    QCOMPARE(defaultRequest.value(QStringLiteral("max_candidates")).toInt(), 6);
    QCOMPARE(defaultRequest.value(QStringLiteral("schema_version")).toInt(), 3);
    QVERIFY(defaultRequest.contains(QStringLiteral("avoid_regions")));
    QVERIFY(!defaultRequest.contains(QStringLiteral("allowed_source_files")));
    const QJsonObject requestDocument = defaultRequest.value(QStringLiteral("document")).toObject();
    QCOMPARE(requestDocument.value(QStringLiteral("pdf_path")).toString(), QStringLiteral("/tmp/paper.pdf"));
    QCOMPARE(requestDocument.value(QStringLiteral("page_count")).toInt(), 12);
    QVERIFY(!defaultRequest.contains(QStringLiteral("selection")));

    const QJsonObject configuredRequest = store.agentRequest(document,
                                                              QStringLiteral(" gpt-5.6-sol "),
                                                              QStringLiteral(" xhigh "),
                                                              27,
                                                              QStringLiteral(" Generate exactly the requested card count when suitable. "));
    QCOMPARE(configuredRequest.value(QStringLiteral("agent_model")).toString(), QStringLiteral("gpt-5.6-sol"));
    QCOMPARE(configuredRequest.value(QStringLiteral("agent_reasoning_effort")).toString(), QStringLiteral("xhigh"));
    QCOMPARE(configuredRequest.value(QStringLiteral("max_candidates")).toInt(), 27);
    QCOMPARE(configuredRequest.value(QStringLiteral("agent_instruction")).toString(), QStringLiteral("Generate exactly the requested card count when suitable."));
}

void ReviewStoreTest::importRespectsRequestedCandidateLimit()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QJsonObject payload{{QStringLiteral("schema_version"), 3}, {QStringLiteral("candidates"), QJsonArray{QJsonObject{}, QJsonObject{}}}};
    const QVariantMap document = {{QStringLiteral("pdf_stem"), QStringLiteral("paper")},
                                  {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                                  {QStringLiteral("page_count"), 2}};
    const ReviewImportResult imported = store.importAgentOutput(QJsonDocument(payload).toJson(QJsonDocument::Compact), document, 1);
    QVERIFY(imported.cards.isEmpty());
    QCOMPARE(imported.errors, QStringList{QStringLiteral("Agent output contains more than the requested 1 candidates.")});
}

void ReviewStoreTest::importAcceptsVisualRegionsAcrossDocumentPages()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QString pdfSha256(64, QLatin1Char('b'));
    const QVariantMap document = {{QStringLiteral("pdf_stem"), QStringLiteral("paper")},
                                  {QStringLiteral("pdf_sha256"), pdfSha256},
                                  {QStringLiteral("page_count"), 2}};
    const auto candidate = [](int page, QChar fingerprintCharacter) {
        return QJsonObject{{QStringLiteral("page"), page},
                           {QStringLiteral("card_id"), QStringLiteral("10000000-0000-4000-8000-%1").arg(page, 12, 10, QLatin1Char('0'))},
                           {QStringLiteral("note"), QStringLiteral("Possible duplicate of an earlier question.")},
                           {QStringLiteral("duplicate_check"), QJsonObject{{QStringLiteral("duplicate_of"), QStringLiteral("existing-card")}}},
                           {QStringLiteral("rect"), QJsonArray{0.1, 0.2, 0.8, 0.6}},
                           {QStringLiteral("question"), QStringLiteral("What does this result demonstrate?")},
                           {QStringLiteral("page_visual_sha256"), QString(64, fingerprintCharacter)}};
    };
    const QJsonObject acceptedPayload{{QStringLiteral("schema_version"), 3},
                                      {QStringLiteral("candidates"), QJsonArray{candidate(1, QLatin1Char('c')), candidate(2, QLatin1Char('d'))}}};
    const ReviewImportResult accepted = store.importAgentOutput(QJsonDocument(acceptedPayload).toJson(QJsonDocument::Compact), document, 2);
    QCOMPARE(accepted.cards.size(), 2);
    QVERIFY(accepted.cards.constFirst().isApproved());
    QCOMPARE(store.dueCards().size(), 2);
    QCOMPARE(accepted.cards.constFirst().id(), QStringLiteral("10000000-0000-4000-8000-000000000001"));
    QCOMPARE(accepted.cards.constFirst().record.value(QStringLiteral("note")).toString(), QStringLiteral("Possible duplicate of an earlier question."));
    QVERIFY(accepted.cards.constFirst().record.contains(QStringLiteral("duplicate_check")));
    QVERIFY(store.importAgentOutput(QJsonDocument(acceptedPayload).toJson(), document, 2).cards.isEmpty());
    QCOMPARE(accepted.cards.constFirst().rect(), QVariantList({0.1, 0.2, 0.8, 0.6}));
    QCOMPARE(accepted.cards.constFirst().question(), QStringLiteral("What does this result demonstrate?"));
    QVERIFY(!accepted.cards.constFirst().record.value(QStringLiteral("primary")).toObject().contains(QStringLiteral("source_path")));

    const QJsonObject invalidPayload{{QStringLiteral("schema_version"), 3},
                                     {QStringLiteral("candidates"), QJsonArray{candidate(3, QLatin1Char('e'))}}};
    const ReviewImportResult invalid = store.importAgentOutput(QJsonDocument(invalidPayload).toJson(QJsonDocument::Compact), document, 1);
    QVERIFY(invalid.cards.isEmpty());
    QCOMPARE(invalid.errors, QStringList{QStringLiteral("Rejected an invalid visual region on PDF page 3.")});
}

void ReviewStoreTest::importAcceptsLargeAndRejectsDuplicateRegions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QVariantMap document = {{QStringLiteral("pdf_stem"), QStringLiteral("paper")},
                                  {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                                  {QStringLiteral("page_count"), 2}};
    const QString visualSha(64, QLatin1Char('b'));
    const auto payload = [&visualSha](const QJsonArray &rect) {
        return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 3},
                                         {QStringLiteral("candidates"), QJsonArray{QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), rect}, {QStringLiteral("question"), QStringLiteral("What is the reported outcome?")}, {QStringLiteral("page_visual_sha256"), visualSha}}}}})
            .toJson(QJsonDocument::Compact);
    };
    const QJsonArray largeRect{0.0, 0.0, 1.0, 0.6};
    QCOMPARE(store.importAgentOutput(payload(largeRect), document, 1).cards.size(), 1);
    QVERIFY(store.importAgentOutput(payload(largeRect), document, 1).cards.isEmpty());
    QVERIFY(store.importAgentOutput(payload(QJsonArray{0.0, 0.0, 1.0, 1.1}), document, 1).cards.isEmpty());
}

void ReviewStoreTest::importCreatesActiveCardsAndPersistsEdits()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QVariantMap document = {{QStringLiteral("pdf_stem"), QStringLiteral("paper")},
                                  {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                                  {QStringLiteral("page_count"), 1}};
    const QString visualSha(64, QLatin1Char('b'));
    const auto payload = [&visualSha](const QJsonValue &question) {
        return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 3},
                                         {QStringLiteral("candidates"),
                                          QJsonArray{QJsonObject{{QStringLiteral("page"), 1},
                                                                 {QStringLiteral("rect"), QJsonArray{0.1, 0.1, 0.7, 0.5}},
                                                                 {QStringLiteral("question"), question},
                                                                 {QStringLiteral("page_visual_sha256"), visualSha}}}}})
            .toJson(QJsonDocument::Compact);
    };

    QVERIFY(store.importAgentOutput(payload(QJsonValue()), document, 1).cards.isEmpty());
    QVERIFY(store.importAgentOutput(payload(QStringLiteral("   ")), document, 1).cards.isEmpty());
    QVERIFY(store.importAgentOutput(payload(QString(501, QLatin1Char('x'))), document, 1).cards.isEmpty());

    const ReviewImportResult imported = store.importAgentOutput(payload(QStringLiteral("  What is the original result?  ")), document, 1);
    QCOMPARE(imported.cards.size(), 1);
    QCOMPARE(imported.cards.constFirst().question(), QStringLiteral("What is the original result?"));
    QString error;
    QVERIFY(!store.updateCardContent(imported.cards.constFirst(), imported.cards.constFirst().rect(), QStringLiteral("   "), &error));
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY2(store.updateCardContent(imported.cards.constFirst(), imported.cards.constFirst().rect(), QStringLiteral("  What is the edited result?  "), &error), qPrintable(error));
    const QList<ReviewCard> cards = store.cards();
    QCOMPARE(cards.size(), 1);
    QVERIFY(cards.constFirst().isApproved());
    QCOMPARE(cards.constFirst().question(), QStringLiteral("What is the edited result?"));

    // After Edit Areas… overrides the areas, rect() reports the new first area; saving
    // only a new question with that value must succeed and leave the areas alone.
    const QVariantList areas{QVariantMap{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QVariantList{0.1, 0.2, 0.8, 0.6}}}};
    QVERIFY2(store.updateCardAnswer(cards.constFirst(), areas, cards.constFirst().question(), 1, &error), qPrintable(error));
    const ReviewCard overridden = store.cards().constFirst();
    QCOMPARE(overridden.rect(), (QVariantList{0.1, 0.2, 0.8, 0.6}));
    const QJsonObject rectBefore = overridden.record.value(QStringLiteral("primary")).toObject();
    QVERIFY2(store.updateCardContent(overridden, overridden.rect(), QStringLiteral("What is the result now?"), &error), qPrintable(error));
    const ReviewCard renamed = store.cards().constFirst();
    QCOMPARE(renamed.question(), QStringLiteral("What is the result now?"));
    QCOMPARE(renamed.regions(), overridden.regions());
    QCOMPARE(renamed.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("rect")),
             rectBefore.value(QStringLiteral("rect")));
}

void ReviewStoreTest::importedCardKeepsIdentityAcrossPublication()
{
    QTemporaryDir dir;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral(".thoughts/project-imports")));
    const auto imported = dir.filePath(QStringLiteral(".thoughts/project-imports/paper.pdf"));
    { QPdfWriter writer(imported); QPainter painter(&writer);
      painter.drawText(QPointF(80, 100), QStringLiteral("An imported answer.")); }
    auto document = prepareBasicReviewCards(imported);
    const auto sha = document.value(QStringLiteral("pdf_sha256")).toString();
    QString error;
    const auto documentId = DocumentIdentityStore(dir.path()).registerImport(QStringLiteral("paper.pdf"), sha, imported, &error);
    QVERIFY2(DocumentIdentityStore::validId(documentId), qPrintable(error));
    document.insert(QStringLiteral("document_id"), documentId);
    ReviewCard created;
    ReviewStore store(dir.path());
    QVERIFY2(store.createManualCard(document, 1, {0.1, 0.1, 0.8, 0.3}, QStringLiteral("What is the answer?"),
                                   QString(64, QLatin1Char('b')), &created, &error), qPrintable(error));
    QCOMPARE(created.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("document_id")).toString(), documentId);
    QCOMPARE(reviewCardPdfPath(dir.path(), created.record), imported);
    QVERIFY(store.recordRating(created.id(), QStringLiteral("good"), QDateTime::currentDateTimeUtc(), &error));
    const auto before = ReviewStore(dir.path()).cards().first().record;
    QVERIFY(QDir(dir.path()).mkpath(QStringLiteral("current_pdfs")));
    const auto published = dir.filePath(QStringLiteral("current_pdfs/paper.pdf"));
    QVERIFY(QFile::copy(imported, published));
    QVERIFY(writeObject(dir.filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")),
                        {{QStringLiteral("pdfs"), QJsonArray{QJsonObject{
                            {QStringLiteral("document_id"), documentId}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                            {QStringLiteral("pdf_sha256"), sha}, {QStringLiteral("kind"), QStringLiteral("unlinked")}}}}}));
    const auto after = ReviewStore(dir.path()).cards().first().record;
    QCOMPARE(after, before);
    QCOMPARE(reviewCardPdfPath(dir.path(), after), published);
    QCOMPARE(DocumentIdentityStore(dir.path()).forFile(imported).value(QStringLiteral("document_id")).toString(), documentId);
}

namespace
{
// A whole-document card as clients made them before context notes: every page hidden, no markers.
QJsonObject wholeDocumentCard(const QString &cardId, const QString &documentId, const QString &pdfStem, const QString &question, int pageCount = 3)
{
    const QJsonArray full{0.0, 0.0, 1.0, 1.0};
    QJsonArray regions;
    for (int page = 1; page <= pageCount; ++page) regions.append(QJsonObject{{QStringLiteral("page"), page}, {QStringLiteral("rect"), full}});
    return {{QStringLiteral("schema_version"), 4}, {QStringLiteral("card_type"), QStringLiteral("visual_region")}, {QStringLiteral("card_id"), cardId},
            {QStringLiteral("status"), QStringLiteral("approved")}, {QStringLiteral("revision"), 1},
            {QStringLiteral("created_at"), QStringLiteral("2026-10-05T10:00:00Z")},
            {QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("whole_document")}}},
            {QStringLiteral("primary"), QJsonObject{{QStringLiteral("document_id"), documentId}, {QStringLiteral("pdf_stem"), pdfStem},
                                                    {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("page"), 1},
                                                    {QStringLiteral("rect"), full}, {QStringLiteral("regions"), regions},
                                                    {QStringLiteral("scope"), QStringLiteral("document")}, {QStringLiteral("question"), question},
                                                    {QStringLiteral("page_visual_sha256"), QString(64, QLatin1Char('b'))}}}};
}

// reviewCard() of document @p documentId, answered on @p page from @p top down.
QJsonObject documentCard(const QString &cardId, const QString &documentId, const QString &pdfStem, int page, double top)
{
    QJsonObject card = reviewCard(cardId, pdfStem, QString(64, QLatin1Char('a')));
    QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    if (!documentId.isEmpty()) primary.insert(QStringLiteral("document_id"), documentId);
    primary.insert(QStringLiteral("page"), page);
    primary.insert(QStringLiteral("rect"), QJsonArray{0.1, top, 0.9, std::min(1.0, top + 0.2)});
    card.insert(QStringLiteral("primary"), primary);
    return card;
}

QJsonObject queuedEvent(const QString &syncRoot, const QString &eventId)
{
    DesktopSyncOutbox outbox(QDir(syncRoot).filePath(QStringLiteral("desktop-sync.sqlite")));
    for (const auto &operation : outbox.operations()) {
        const auto payload = operation.value(QStringLiteral("payload")).toObject();
        if (payload.value(QStringLiteral("event_id")).toString() == eventId) return payload;
    }
    return {};
}
}

void ReviewStoreTest::oldWholeDocumentCardsAreStillRead()
{
    // New ones can no longer be made; those made before still read and review until converted.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(id)),
                        wholeDocumentCard(id, QStringLiteral("doc-1"), QStringLiteral("paper"), QStringLiteral("household rules"))));
    ReviewStore store(temporary.path());
    const auto cards = store.cards();
    QCOMPARE(cards.size(), 1);
    QVERIFY(cards.first().isWholeDocument());
    QCOMPARE(cards.first().linkingState(), QStringLiteral("whole_document"));
    QCOMPARE(cards.first().pagesLabel(), QStringLiteral("1, 2, 3"));
    QCOMPARE(store.dueCards().size(), 1);
}

void ReviewStoreTest::contextNotesAreSharedNewestFirst()
{
    using namespace ReviewContextNotes;
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString syncRoot = QDir(root.path()).filePath(QStringLiteral("sync"));
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString other = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stemOnly = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // Two cards of one document (its PDF renamed in between), one of another document on the
    // same page number, and one without a document_id.
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + first, documentCard(first, QStringLiteral("doc-1"), QStringLiteral("paper"), 3, 0.5)},
                           {QStringLiteral("review_card\ncards\n") + second, documentCard(second, QStringLiteral("doc-1"), QStringLiteral("paper-v2"), 3, 0.1)},
                           {QStringLiteral("review_card\ncards\n") + other, documentCard(other, QStringLiteral("doc-2"), QStringLiteral("notes"), 3, 0.5)},
                           {QStringLiteral("review_card\ncards\n") + stemOnly, documentCard(stemOnly, QString(), QStringLiteral("loose"), 1, 0.5)}};
    ReviewStore store(root.path(), [&projection] { return projection; }, syncRoot);
    const auto cardOf = [&store](const QString &id) {
        for (const auto &card : store.cards())
            if (card.id() == id) return card;
        return ReviewCard{};
    };
    QCOMPARE(documentKey(cardOf(stemOnly).record), QStringLiteral("loose"));
    QCOMPARE(documentKey(cardOf(first).record), QStringLiteral("doc-1"));
    const QDateTime at(QDate(2026, 10, 21), QTime(10, 0), QTimeZone::UTC);
    QString error;
    // Every answer of a card syncs as the server projects it.
    const auto answer = [&](const QString &card, const QDateTime &when, const QHash<QString, Note> &notes) {
        QString eventId;
        if (!store.recordRating(card, QStringLiteral("custom_interval"), when, &error, when.addDays(7), &eventId, {}, {}, {}, notes)) return QString();
        projection.insert(QStringLiteral("review_event\nevents\n") + eventId, queuedEvent(syncRoot, eventId));
        return eventId;
    };

    // Without an outline there is no section; the page and the document can be noted.
    Draft draft = store.contextDraft(cardOf(first), {}, QStringLiteral("A paper"));
    QCOMPARE(draft.entries.size(), 3);
    QCOMPARE(draft.entries.at(0).scope, sectionScope());
    QVERIFY(!draft.entries.at(0).available);
    QCOMPARE(draft.selected, 1);
    QCOMPARE(draft.entries.at(1).note, (Note{QStringLiteral("3"), QStringLiteral("Page 3"), QString()}));
    QCOMPARE(draft.entries.at(2).note, (Note{QString(), QStringLiteral("A paper"), QString()}));
    // Nothing typed: the answer carries no field.
    QVERIFY(draft.changes().isEmpty());
    const QString plain = answer(first, at, draft.changes());
    QVERIFY2(!plain.isEmpty(), qPrintable(error));
    QVERIFY(!queuedEvent(syncRoot, plain).contains(field()));

    // Only the scopes changed are recorded, cleaned like the answer's Note.
    draft.entries[1].text = QStringLiteral("  Figure 2 is wrong \n");
    draft.entries[2].text = QStringLiteral("Read again before the exam");
    draft.entries[0].text = QStringLiteral("not available, never recorded");
    QCOMPARE(draft.changes().size(), 2);
    const QString noted = answer(first, at.addSecs(60), draft.changes());
    QVERIFY2(!noted.isEmpty(), qPrintable(error));
    QCOMPARE(queuedEvent(syncRoot, noted).value(field()).toObject(),
             (QJsonObject{{QStringLiteral("page"), QJsonObject{{QStringLiteral("key"), QStringLiteral("3")}, {QStringLiteral("label"), QStringLiteral("Page 3")},
                                                               {QStringLiteral("text"), QStringLiteral("Figure 2 is wrong")}}},
                          {QStringLiteral("document"), QJsonObject{{QStringLiteral("key"), QString()}, {QStringLiteral("label"), QStringLiteral("A paper")},
                                                                   {QStringLiteral("text"), QStringLiteral("Read again before the exam")}}}}));

    // Every card of the document shows them; another document, even on page 3, does not.
    Draft secondDraft = store.contextDraft(cardOf(second), {}, QStringLiteral("A paper"));
    QCOMPARE(secondDraft.entries.at(1).text, QStringLiteral("Figure 2 is wrong"));
    QCOMPARE(secondDraft.entries.at(2).text, QStringLiteral("Read again before the exam"));
    QVERIFY(secondDraft.changes().isEmpty());
    const Draft otherDraft = store.contextDraft(cardOf(other), {}, QStringLiteral("Notes"));
    QVERIFY(otherDraft.entries.at(1).text.isEmpty() && otherDraft.entries.at(2).text.isEmpty());

    // The newest note wins, whichever card it was written on.
    secondDraft.entries[1].text = QStringLiteral("Fixed in v2");
    const QString newer = answer(second, at.addSecs(120), secondDraft.changes());
    QVERIFY2(!newer.isEmpty(), qPrintable(error));
    QCOMPARE(store.contextNotes(QStringLiteral("doc-1")),
             (QHash<QString, QString>{{noteId(pageScope(), QStringLiteral("3")), QStringLiteral("Fixed in v2")},
                                      {noteId(documentScope(), QString()), QStringLiteral("Read again before the exam")}}));

    // An empty text clears a note.
    Draft clearing = store.contextDraft(cardOf(first), {}, QStringLiteral("A paper"));
    clearing.entries[2].text = QStringLiteral("   ");
    QCOMPARE(clearing.changes().value(documentScope()).text, QString());
    const QString cleared = answer(first, at.addSecs(180), clearing.changes());
    QVERIFY2(!cleared.isEmpty(), qPrintable(error));
    QCOMPARE(queuedEvent(syncRoot, cleared).value(field()).toObject().value(documentScope()).toObject().value(QStringLiteral("text")).toString(), QString());
    QVERIFY(!store.contextNotes(QStringLiteral("doc-1")).contains(noteId(documentScope(), QString())));

    // Undoing an answer undoes its notes: the page note is the earlier one again.
    QVERIFY2(store.retractRating(second, newer, at.addSecs(240), &error), qPrintable(error));
    QCOMPARE(store.contextNotes(QStringLiteral("doc-1")).value(noteId(pageScope(), QStringLiteral("3"))), QStringLiteral("Figure 2 is wrong"));

    // The rule itself: reviewed_at, then event_id; deleted events and other keys apart.
    const auto event = [](const QString &id, const QString &card, const QString &when, const QJsonObject &notes) {
        return QJsonObject{{QStringLiteral("event_id"), id}, {QStringLiteral("card_id"), card}, {QStringLiteral("reviewed_at"), when}, {field(), notes}};
    };
    const auto pageNoteOf = [](const QString &key, const QString &text) {
        return QJsonObject{{pageScope(), QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("label"), QString(QStringLiteral("Page ") + key)}, {QStringLiteral("text"), text}}}};
    };
    const QList<QJsonObject> records{documentCard(QStringLiteral("a"), QStringLiteral("d"), QStringLiteral("s"), 1, 0.1),
                                     documentCard(QStringLiteral("b"), QStringLiteral("d"), QStringLiteral("s"), 2, 0.1)};
    QList<QJsonObject> events{event(QStringLiteral("e1"), QStringLiteral("a"), QStringLiteral("2026-10-21T10:00:00Z"), pageNoteOf(QStringLiteral("1"), QStringLiteral("one"))),
                              event(QStringLiteral("e2"), QStringLiteral("b"), QStringLiteral("2026-10-21T10:00:00Z"), pageNoteOf(QStringLiteral("1"), QStringLiteral("two"))),
                              event(QStringLiteral("e0"), QStringLiteral("b"), QStringLiteral("2026-10-21T11:00:00Z"), pageNoteOf(QStringLiteral("2"), QStringLiteral("page two"))),
                              event(QStringLiteral("e9"), QStringLiteral("x"), QStringLiteral("2026-10-21T12:00:00Z"), pageNoteOf(QStringLiteral("1"), QStringLiteral("other document")))};
    QCOMPARE(current(records, events, QStringLiteral("d")),
             (QHash<QString, QString>{{noteId(pageScope(), QStringLiteral("1")), QStringLiteral("two")}, {noteId(pageScope(), QStringLiteral("2")), QStringLiteral("page two")}}));
    QJsonObject deleted = event(QStringLiteral("e3"), QStringLiteral("a"), QStringLiteral("2026-10-21T13:00:00Z"), pageNoteOf(QStringLiteral("1"), QStringLiteral("deleted")));
    deleted.insert(QStringLiteral("deleted"), true);
    events.append(deleted);
    QCOMPARE(current(records, events, QStringLiteral("d")).value(noteId(pageScope(), QStringLiteral("1"))), QStringLiteral("two"));
    // Malformed entries are skipped; a scope missing means unchanged.
    QVERIFY(ofEvent(QJsonObject{{field(), QJsonObject{{pageScope(), QJsonObject{{QStringLiteral("key"), 3}, {QStringLiteral("text"), QStringLiteral("x")}}}}}}).isEmpty());
    QVERIFY(toField({}).isEmpty());
}

void ReviewStoreTest::contextNoteSectionFollowsTheOutline()
{
    using namespace ReviewContextNotes;
    const QVariantList outlineData{
        QVariantMap{{QStringLiteral("title"), QStringLiteral("1 Intro")}, {QStringLiteral("page"), 1}, {QStringLiteral("top"), 0.1}},
        QVariantMap{{QStringLiteral("title"), QStringLiteral("2 Method")}, {QStringLiteral("page"), 2}, {QStringLiteral("top"), 0.1},
                    {QStringLiteral("children"), QVariantList{
                         QVariantMap{{QStringLiteral("title"), QStringLiteral("2.1 Data")}, {QStringLiteral("page"), 2}, {QStringLiteral("top"), 0.5}},
                         QVariantMap{{QStringLiteral("title"), QStringLiteral("2.2 Loss")}, {QStringLiteral("page"), 3}, {QStringLiteral("top"), 0.2},
                                     {QStringLiteral("children"), QVariantList{QVariantMap{{QStringLiteral("title"), QStringLiteral("  2.2.1\n  Detail ")},
                                                                                           {QStringLiteral("page"), 3}, {QStringLiteral("top"), 0.6}}}}}}}},
        // A part heading without a destination starts with its first chapter.
        QVariantMap{{QStringLiteral("title"), QStringLiteral("Part II")}, {QStringLiteral("page"), 0},
                    {QStringLiteral("children"), QVariantList{QVariantMap{{QStringLiteral("title"), QStringLiteral("3 Results")}, {QStringLiteral("page"), 5}, {QStringLiteral("top"), 0.1}}}}},
        QVariantMap{{QStringLiteral("title"), QStringLiteral("Links elsewhere")}, {QStringLiteral("page"), 0}}};
    const QList<OutlineItem> outline = outlineFromVariant(outlineData);
    QCOMPARE(outline.size(), 4);
    QCOMPARE(outline.at(1).children.at(1).children.first().page, 3);
    const auto sectionOf = [&outline](int page, double top) {
        Note section;
        return sectionAt(outline, page, top, &section) ? section : Note{QStringLiteral("none"), {}, {}};
    };
    const QString separator = sectionSeparator();
    QCOMPARE(separator, QStringLiteral(" › "));
    // Before the first heading there is no section.
    QCOMPARE(sectionOf(1, 0.05).key, QStringLiteral("none"));
    QCOMPARE(sectionOf(1, 0.1), (Note{QStringLiteral("1 Intro"), QStringLiteral("1 Intro"), {}}));
    QCOMPARE(sectionOf(2, 0.3), (Note{QStringLiteral("2 Method"), QStringLiteral("2 Method"), {}}));
    QCOMPARE(sectionOf(2, 0.6), (Note{QStringLiteral("2 Method") + separator + QStringLiteral("2.1 Data"), QStringLiteral("2.1 Data"), {}}));
    const Note detail{QStringLiteral("2 Method") + separator + QStringLiteral("2.2 Loss") + separator + QStringLiteral("2.2.1 Detail"), QStringLiteral("2.2.1 Detail"), {}};
    QCOMPARE(sectionOf(3, 0.7), detail);
    QCOMPARE(sectionOf(4, 0.0), detail);
    QCOMPARE(sectionOf(5, 0.05), detail);
    QCOMPARE(sectionOf(5, 0.5), (Note{QStringLiteral("Part II") + separator + QStringLiteral("3 Results"), QStringLiteral("3 Results"), {}}));
    QVERIFY(!sectionAt({}, 3, 0.5, nullptr));

    // A card's section is that of the top of its first area; before the first heading the
    // Section choice is unavailable and the page is chosen.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString inLoss = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString beforeAll = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject multiArea = documentCard(inLoss, QStringLiteral("doc-1"), QStringLiteral("paper"), 3, 0.7);
    multiArea.insert(QStringLiteral("schema_version"), 4);
    QJsonObject primary = multiArea.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("page"), 4}, {QStringLiteral("rect"), QJsonArray{0.1, 0.0, 0.9, 0.3}}},
                                                         QJsonObject{{QStringLiteral("page"), 3}, {QStringLiteral("rect"), QJsonArray{0.1, 0.7, 0.9, 0.9}}},
                                                         QJsonObject{{QStringLiteral("page"), 3}, {QStringLiteral("rect"), QJsonArray{0.1, 0.3, 0.9, 0.4}}}});
    multiArea.insert(QStringLiteral("primary"), primary);
    const QJsonObject projection{{QStringLiteral("review_card\ncards\n") + inLoss, multiArea},
                                 {QStringLiteral("review_card\ncards\n") + beforeAll, documentCard(beforeAll, QStringLiteral("doc-1"), QStringLiteral("paper"), 1, 0.02)}};
    ReviewStore store(root.path(), [&projection] { return projection; });
    for (const auto &card : store.cards()) {
        const Draft draft = store.contextDraft(card, outline, QStringLiteral("Paper"));
        if (card.id() == inLoss) {
            int page = 0;
            double top = 0;
            QVERIFY(primaryPosition(card, &page, &top));
            QCOMPARE(page, 3);
            QCOMPARE(top, 0.3);
            QVERIFY(draft.entries.at(0).available);
            QCOMPARE(draft.entries.at(0).note.key, QStringLiteral("2 Method") + separator + QStringLiteral("2.2 Loss"));
            QCOMPARE(draft.entries.at(0).note.label, QStringLiteral("2.2 Loss"));
            QCOMPARE(draft.selected, 0);
        } else {
            QVERIFY(!draft.entries.at(0).available);
            QCOMPARE(draft.selected, 1);
            QCOMPARE(draft.entries.at(1).note.key, QStringLiteral("1"));
        }
    }
}

void ReviewStoreTest::wholeDocumentCardsBecomeDocumentNotes()
{
    using namespace ReviewContextNotes;
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString syncRoot = QDir(root.path()).filePath(QStringLiteral("sync"));
    const QString rules = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString single = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString gone = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString ordinary = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + rules, wholeDocumentCard(rules, QStringLiteral("doc-1"), QStringLiteral("rules"), QStringLiteral("household rules"))},
                           {QStringLiteral("review_card\ncards\n") + single, wholeDocumentCard(single, QStringLiteral("doc-2"), QStringLiteral("other"), QStringLiteral("What is it about?"))},
                           {QStringLiteral("review_card\ncards\n") + gone, wholeDocumentCard(gone, QStringLiteral("doc-3"), QStringLiteral("gone"), QStringLiteral("deleted before"))},
                           {QStringLiteral("review_card\ncards\n") + ordinary, documentCard(ordinary, QStringLiteral("doc-1"), QStringLiteral("rules"), 2, 0.4)}};
    ReviewStore store(root.path(), [&projection] { return projection; }, syncRoot);
    const QDateTime at = QDateTime::currentDateTimeUtc().addSecs(-3600);
    QString error;
    // The rules card was answered twice, the earlier answer with a Note; doc-1 already has a
    // document note, written from the ordinary card; the third card was deleted.
    QVERIFY2(store.recordRating(rules, QStringLiteral("custom_interval"), at, &error, at.addDays(7), nullptr, {}, {},
                                {{ReviewTags::answerNoteKey(), QStringLiteral("Check the fridge list")}}), qPrintable(error));
    QVERIFY2(store.recordRating(rules, QStringLiteral("custom_interval"), at.addSecs(60), &error, at.addDays(7), nullptr, {}, {}, {}), qPrintable(error));
    QVERIFY2(store.recordRating(ordinary, QStringLiteral("custom_interval"), at.addSecs(120), &error, at.addDays(7), nullptr, {}, {}, {},
                                {{documentScope(), Note{QString(), QStringLiteral("Rules"), QStringLiteral("Existing note")}}}), qPrintable(error));
    QVERIFY2(store.deleteCard(gone, at.addSecs(180), &error), qPrintable(error));

    const auto converted = store.convertWholeDocumentCards([](const ReviewCard &card) { return card.pdfStem() + QStringLiteral(" (shown)"); });
    QCOMPARE(converted.size(), 2);
    const QUuid urlNamespace(QStringLiteral("6ba7b811-9dad-11d1-80b4-00c04fd430c8"));
    const auto noteEventOf = [&urlNamespace](const QString &card) {
        return QUuid::createUuidV5(urlNamespace, QString(QStringLiteral("context-note-conversion:") + card)).toString(QUuid::WithoutBraces);
    };
    for (const auto &entry : converted) {
        QVERIFY(entry.cardId == rules || entry.cardId == single);
        QCOMPARE(entry.noteEventId, noteEventOf(entry.cardId));
    }
    // The note is appended to the one in effect: the question, then the latest answer Note.
    QCOMPARE(store.contextNotes(QStringLiteral("doc-1")).value(noteId(documentScope(), QString())),
             QStringLiteral("Existing note\nhousehold rules\nCheck the fridge list"));
    QCOMPARE(store.contextNotes(QStringLiteral("doc-2")).value(noteId(documentScope(), QString())), QStringLiteral("What is it about?"));
    QVERIFY(store.contextNotes(QStringLiteral("doc-3")).isEmpty());
    const QJsonObject noteEvent = queuedEvent(syncRoot, noteEventOf(single));
    QCOMPARE(noteEvent.value(field()).toObject().value(documentScope()).toObject().value(QStringLiteral("label")).toString(), QStringLiteral("other (shown)"));
    QCOMPARE(noteEvent.value(QStringLiteral("queue_action")).toString(), QStringLiteral("skip"));
    // Then the card is deleted as thoughtsctl delete-review-card deletes it.
    const QString deleteId = QUuid::createUuidV5(urlNamespace, QString(QStringLiteral("context-note-conversion-delete:") + single)).toString(QUuid::WithoutBraces);
    const QJsonObject deletion = queuedEvent(syncRoot, deleteId);
    QCOMPARE(deletion.value(QStringLiteral("rating")).toString(), QStringLiteral("delete"));
    QCOMPARE(deletion.value(QStringLiteral("marker_removal")).toString(), QStringLiteral("not_required"));
    QCOMPARE(deletion.value(QStringLiteral("source_authority")).toString(), QStringLiteral("client-v1"));
    QVERIFY(QDateTime::fromString(deletion.value(QStringLiteral("reviewed_at")).toString(), Qt::ISODateWithMs)
            > QDateTime::fromString(noteEvent.value(QStringLiteral("reviewed_at")).toString(), Qt::ISODateWithMs));
    for (const auto &card : store.cards()) QCOMPARE(card.schedule.deleted, card.id() != ordinary);

    // A second run changes nothing.
    DesktopSyncOutbox outbox(QDir(syncRoot).filePath(QStringLiteral("desktop-sync.sqlite")));
    const qsizetype queued = outbox.operations().size();
    QVERIFY(store.convertWholeDocumentCards().isEmpty());
    QCOMPARE(outbox.operations().size(), queued);

    // Another device converted the card, but only its note has arrived here: the card is
    // deleted without a second note.
    QTemporaryDir otherRoot;
    QVERIFY(otherRoot.isValid());
    QJsonObject otherProjection{{QStringLiteral("review_card\ncards\n") + rules, projection.value(QStringLiteral("review_card\ncards\n") + rules)},
                                {QStringLiteral("review_event\nevents\n") + noteEventOf(rules), queuedEvent(syncRoot, noteEventOf(rules))}};
    ReviewStore otherStore(otherRoot.path(), [&otherProjection] { return otherProjection; }, QDir(otherRoot.path()).filePath(QStringLiteral("sync")));
    const auto again = otherStore.convertWholeDocumentCards();
    QCOMPARE(again.size(), 1);
    QVERIFY(again.first().noteEventId.isEmpty());
    QCOMPARE(otherStore.contextNotes(QStringLiteral("doc-1")).value(noteId(documentScope(), QString())),
             QStringLiteral("Existing note\nhousehold rules\nCheck the fridge list"));
    QVERIFY(otherStore.cards().first().schedule.deleted);
}

// A reader tab's Copy for agent: only the cards answered, and the feedback given, on the PDF version
// on screen; answers, answer feedback and text feedback of other versions are left out.
void ReviewStoreTest::agentVersionFeedbackKeepsOnlyTheVersionOnScreen()
{
    const QString current(64, QLatin1Char('a'));
    const QString older(64, QLatin1Char('b'));
    const auto version = [](const QString &sha) { return QJsonObject{{QStringLiteral("pdf_sha256"), sha}, {QStringLiteral("source_built"), true}}; };
    const auto card = [](const QString &id, const QString &question, int page) {
        Thoughts::ReviewCard result;
        result.record = {{QStringLiteral("card_id"), id},
                         {QStringLiteral("primary"), QJsonObject{{QStringLiteral("question"), question}, {QStringLiteral("pdf_stem"), QStringLiteral("grpo")},
                                                                 {QStringLiteral("page"), page},
                                                                 {QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("page"), page},
                                                                     {QStringLiteral("x"), 0.1}, {QStringLiteral("y"), 0.1}, {QStringLiteral("width"), 0.5}, {QStringLiteral("height"), 0.2}}}}}}};
        return result;
    };
    const auto answer = [&](const QString &cardId, const QString &eventId, const QString &at, const QStringList &tags, const QString &sha, const QString &note = {}) {
        QJsonObject event{{QStringLiteral("card_id"), cardId}, {QStringLiteral("event_id"), eventId}, {QStringLiteral("reviewed_at"), at},
                          {QStringLiteral("rating"), QStringLiteral("good")}, {QStringLiteral("review_tags"), QJsonArray::fromStringList(tags)},
                          {Thoughts::FeedbackVersion::field(), version(sha)}};
        if (!note.isEmpty()) event.insert(Thoughts::ReviewTags::groupNotesField(), QJsonObject{{Thoughts::ReviewTags::answerNoteKey(), note}});
        return event;
    };
    const auto feedback = [&](const QString &id, const QString &cardId, const QString &polarity, const QString &quote, const QString &comment,
                              const QString &markedOn, int page, bool deleted = false) {
        QJsonObject record{{QStringLiteral("feedback_id"), id}, {QStringLiteral("polarity"), polarity}, {QStringLiteral("comment"), comment},
                           {QStringLiteral("created_at"), QStringLiteral("2026-10-29T10:00:0%1Z").arg(id.right(1))},
                           {QStringLiteral("deleted"), deleted},
                           {QStringLiteral("anchor"), QJsonObject{{QStringLiteral("quote"), QJsonObject{{QStringLiteral("exact"), quote}}},
                                                                 {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("pdf_sha256"), markedOn},
                                                                     {QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("page"), page}}}}}}}}};
        if (!cardId.isEmpty()) record.insert(QStringLiteral("card_id"), cardId);
        return record;
    };

    auto first = card(QStringLiteral("card-2"), QStringLiteral("Why the KL term?"), 4);
    first.history = {answer(QStringLiteral("card-2"), QStringLiteral("e1"), QStringLiteral("2026-10-20T09:00:00Z"), {QStringLiteral("Wrong")}, older),
                     answer(QStringLiteral("card-2"), QStringLiteral("e2"), QStringLiteral("2026-10-29T09:00:00Z"), {QStringLiteral("partly")}, current,
                            QStringLiteral("Mixed up the\nsign"))};
    auto second = card(QStringLiteral("card-1"), QStringLiteral("Explain  GRPO"), 2);
    second.history = {answer(QStringLiteral("card-1"), QStringLiteral("e3"), QStringLiteral("2026-10-29T08:00:00Z"), {QStringLiteral("Correct")}, current)};
    // Answered only on an older version: left out.
    auto stale = card(QStringLiteral("card-3"), QStringLiteral("Old question"), 1);
    stale.history = {answer(QStringLiteral("card-3"), QStringLiteral("e4"), QStringLiteral("2026-10-20T08:00:00Z"), {QStringLiteral("partly")}, older)};
    // Deleted: left out.
    auto deleted = card(QStringLiteral("card-4"), QStringLiteral("Deleted"), 1);
    deleted.history = {answer(QStringLiteral("card-4"), QStringLiteral("e5"), QStringLiteral("2026-10-29T08:00:00Z"), {QStringLiteral("partly")}, current)};
    deleted.schedule.deleted = true;

    const QList<QJsonObject> answerFeedback{
        feedback(QStringLiteral("f1"), QStringLiteral("card-2"), Thoughts::ReviewCardFeedback::positive(), QStringLiteral("keeps the policy close"),
                 QStringLiteral("Clear"), current, 4),
        feedback(QStringLiteral("f2"), QStringLiteral("card-2"), Thoughts::ReviewCardFeedback::negative(), QStringLiteral("old wording"), {}, older, 4),
        feedback(QStringLiteral("f3"), QStringLiteral("card-2"), Thoughts::ReviewCardFeedback::negative(), QStringLiteral("deleted one"), {}, current, 4, true),
    };
    QList<QJsonObject> textFeedback{
        feedback(QStringLiteral("t2"), {}, Thoughts::ReviewCardFeedback::negative(), QStringLiteral("beta is set\n to 0.1"), QStringLiteral("Should be 0.05"), current, 5),
        feedback(QStringLiteral("t1"), {}, Thoughts::ReviewCardFeedback::positive(), QStringLiteral("nice derivation"), {}, current, 3),
        feedback(QStringLiteral("t3"), {}, Thoughts::ReviewCardFeedback::positive(), QStringLiteral("older version"), {}, older, 3),
    };
    // A question and a text box are named as the phone names them; a resolved question says so.
    // A comment never goes to agents.
    auto question = feedback(QStringLiteral("t4"), {}, Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("the learning rate"), QStringLiteral("Why 0.1?"), current, 6);
    question.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::question());
    question.insert(QStringLiteral("resolved_at"), QStringLiteral("2026-10-29T11:00:00.000Z"));
    auto other = feedback(QStringLiteral("t5"), {}, Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("this proof"), QStringLiteral("See Lemma 2"), current, 7);
    other.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::other());
    // A drawing or a text box has no quote of its own (its anchor's is a placeholder).
    auto textBox = feedback(QStringLiteral("t6"), {}, Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("Text"), QStringLiteral("check this"), current, 8);
    textBox.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::textBox());
    auto drawing = feedback(QStringLiteral("t7"), {}, Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("Drawing"), {}, current, 3);
    drawing.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::drawing());
    textFeedback.append({question, other, textBox, drawing});
    const QList<Thoughts::ReviewTags::TagGroup> groups{
        {QStringLiteral("recall"), QStringLiteral("Recall"), {Thoughts::ReviewTags::correct(), QStringLiteral("partly"), Thoughts::ReviewTags::wrong()}},
    };

    const QString text = Thoughts::agentVersionFeedback({first, second, stale, deleted}, answerFeedback, textFeedback, groups, current);
    const QString at1 = QDateTime::fromString(QStringLiteral("2026-10-29T08:00:00Z"), Qt::ISODate).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    const QString at2 = QDateTime::fromString(QStringLiteral("2026-10-29T09:00:00Z"), Qt::ISODate).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    QCOMPARE(text, QStringLiteral("Cards reviewed on this PDF version:\n"
                                  "- Card card-1 (page 2): \"Explain GRPO\"\n"
                                  "  - Reviewed %1: Recall: Correct\n"
                                  "- Card card-2 (page 4): \"Why the KL term?\"\n"
                                  "  - Reviewed %2: Recall: partly\n"
                                  "    - Note: Mixed up the sign\n"
                                  "  Answer feedback:\n"
                                  "  - Liked: \"keeps the policy close\" — Clear\n"
                                  "Text feedback on this PDF version:\n"
                                  "- Liked (page 3): \"nice derivation\"\n"
                                  "- Disliked (page 5): \"beta is set to 0.1\" — Should be 0.05\n"
                                  "- Question (page 6): \"the learning rate\" — Why 0.1? (resolved)\n"
                                  "- Text box (page 8) — check this\n"
                                  "- Drawing (page 3)\n")
                       .arg(at1, at2));
    // The user's rule (10 November): likes, dislikes and questions belong to the version they were
    // given on, as do drawings, text boxes and a card's answer feedback, so what was given on
    // another build stays with it, even where the reader would find its passage again.
    const QJsonObject staleFeedback = feedback(QStringLiteral("f4"), QStringLiteral("card-3"), Thoughts::ReviewCardFeedback::negative(), QStringLiteral("old answer"),
                                               QStringLiteral("Unclear"), older, 1);
    const QJsonObject rebuiltText = feedback(QStringLiteral("t8"), {}, Thoughts::ReviewCardFeedback::positive(), QStringLiteral("found again"), {}, older, 2);
    auto oldDrawing = feedback(QStringLiteral("t9"), {}, Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("Drawing"), {}, older, 2);
    oldDrawing.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::drawing());
    auto openQuestion = feedback(QStringLiteral("t10"), {}, Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("still open"), QStringLiteral("Why?"), older, 2);
    openQuestion.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::question());
    auto answeredQuestion = feedback(QStringLiteral("t11"), {}, Thoughts::ReviewCardFeedback::neutral(), QStringLiteral("answered"), QStringLiteral("How?"), older, 2);
    answeredQuestion.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::question());
    answeredQuestion.insert(QStringLiteral("resolved_at"), QStringLiteral("2026-11-09T10:00:00Z"));
    QCOMPARE(Thoughts::agentVersionFeedback({stale}, {staleFeedback}, {rebuiltText, oldDrawing, openQuestion, answeredQuestion}, groups, current), QString());
    QCOMPARE(Thoughts::agentVersionFeedback({stale}, {staleFeedback}, {rebuiltText, oldDrawing, openQuestion}, groups, current), QString());
    // Nothing given on the version on screen, or no version known: nothing is added.
    QCOMPARE(Thoughts::agentVersionFeedback({stale}, answerFeedback, {}, groups, current), QString());
    QCOMPARE(Thoughts::agentVersionFeedback({first}, answerFeedback, textFeedback, groups, QString()), QString());
}

void ReviewStoreTest::reviewEventsRecordThePdfVersion()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString syncRoot = QDir(root.path()).filePath(QStringLiteral("sync"));
    const QDir current(QDir(root.path()).filePath(QStringLiteral("current_pdfs")));
    QVERIFY(current.mkpath(QStringLiteral(".")));
    const auto writeFile = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    const auto sha = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QByteArray built = "%PDF-1.4\n% built from notes.tex\n";
    const QByteArray imported = "%PDF-1.4\n% an imported paper\n";
    QVERIFY(writeFile(current.filePath(QStringLiteral("notes.pdf")), built));
    QVERIFY(writeFile(current.filePath(QStringLiteral("paper.pdf")), imported));
    const QString notesId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString paperId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(writeObject(current.filePath(QStringLiteral(".thoughts-library-index.json")), QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray{
        QJsonObject{{QStringLiteral("document_id"), notesId}, {QStringLiteral("filename"), QStringLiteral("notes.pdf")}, {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), sha(built)}},
        QJsonObject{{QStringLiteral("document_id"), paperId}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("kind"), QStringLiteral("unlinked")}, {QStringLiteral("pdf_sha256"), sha(imported)}}}}}));
    const QString onNotes = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString onPaper = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString elsewhere = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString deleted = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString whole = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + onNotes, documentCard(onNotes, notesId, QStringLiteral("notes"), 1, 0.2)},
                           {QStringLiteral("review_card\ncards\n") + onPaper, documentCard(onPaper, paperId, QStringLiteral("paper"), 1, 0.2)},
                           {QStringLiteral("review_card\ncards\n") + elsewhere, documentCard(elsewhere, QString(), QStringLiteral("not-here"), 1, 0.2)},
                           {QStringLiteral("review_card\ncards\n") + deleted, documentCard(deleted, notesId, QStringLiteral("notes"), 1, 0.5)},
                           {QStringLiteral("review_card\ncards\n") + whole, wholeDocumentCard(whole, notesId, QStringLiteral("notes"), QStringLiteral("all of it"))}};
    ReviewStore store(root.path(), [&projection] { return projection; }, syncRoot);
    const QDateTime at = QDateTime::currentDateTimeUtc().addSecs(-600);
    QString error;
    const auto answer = [&](const QString &card, const QDateTime &when) {
        QString eventId;
        if (!store.recordRating(card, QStringLiteral("custom_interval"), when, &error, when.addDays(3), &eventId, {}, {QStringLiteral("correct")})) return QJsonObject();
        return queuedEvent(syncRoot, eventId);
    };
    const QString field = ReviewStore::reviewVersionField();
    QCOMPARE(field, QStringLiteral("review_version"));

    // An answer records the PDF on screen: the bytes' SHA-256, and whether a source builds it.
    const QJsonObject notesAnswer = answer(onNotes, at);
    QVERIFY2(!notesAnswer.isEmpty(), qPrintable(error));
    QCOMPARE(notesAnswer.value(field).toObject(), (QJsonObject{{QStringLiteral("pdf_sha256"), sha(built)}, {QStringLiteral("source_built"), true}}));
    QCOMPARE(answer(onPaper, at).value(field).toObject(), (QJsonObject{{QStringLiteral("pdf_sha256"), sha(imported)}, {QStringLiteral("source_built"), false}}));
    // No PDF here: no stamp, and the answer is still recorded.
    const QJsonObject unstamped = answer(elsewhere, at);
    QVERIFY2(!unstamped.isEmpty(), qPrintable(error));
    QVERIFY(!unstamped.contains(field));

    // A rebuilt PDF is a new version.
    const QByteArray rebuilt = built + "% edited\n";
    QVERIFY(writeFile(current.filePath(QStringLiteral("notes.pdf")), rebuilt));
    QCOMPARE(answer(onNotes, at.addSecs(60)).value(field).toObject().value(QStringLiteral("pdf_sha256")).toString(), sha(rebuilt));

    // Skips and interval changes are stamped; deletions are not.
    ReviewSkipBatch batch;
    QVERIFY2(store.skip(onPaper, ReviewSkipScope::Card, at.addSecs(3600 * 24), &batch, &error, at.addSecs(120)), qPrintable(error));
    DesktopSyncOutbox outbox(QDir(syncRoot).filePath(QStringLiteral("desktop-sync.sqlite")));
    const auto queuedWhere = [&outbox](const std::function<bool(const QJsonObject &)> &match) {
        for (const auto &operation : outbox.operations()) {
            const auto payload = operation.value(QStringLiteral("payload")).toObject();
            if (match(payload)) return payload;
        }
        return QJsonObject();
    };
    const QJsonObject skipEvent = queuedWhere([&](const QJsonObject &event) { return event.value(QStringLiteral("skip_id")).toString() == batch.id; });
    QCOMPARE(skipEvent.value(field).toObject().value(QStringLiteral("pdf_sha256")).toString(), sha(imported));
    ReviewInterval interval;
    interval.amount = 2;
    interval.unit = ReviewInterval::Days;
    QVERIFY2(store.setInterval(onNotes, interval, false, at.addSecs(180), &error), qPrintable(error));
    const QJsonObject intervalEvent = queuedWhere([&](const QJsonObject &event) {
        return event.value(QStringLiteral("card_id")).toString() == onNotes && event.value(QStringLiteral("interval_action")).toString() == QLatin1String("set");
    });
    QCOMPARE(intervalEvent.value(field).toObject().value(QStringLiteral("pdf_sha256")).toString(), sha(rebuilt));
    QVERIFY2(store.deleteCard(deleted, at.addSecs(240), &error), qPrintable(error));
    const QJsonObject deletion = queuedWhere([&](const QJsonObject &event) {
        return event.value(QStringLiteral("card_id")).toString() == deleted && event.value(QStringLiteral("rating")).toString() == QLatin1String("delete");
    });
    QVERIFY(!deletion.isEmpty());
    QVERIFY(!deletion.contains(field));

    // The conversion's note event is stamped too; its deletion is not.
    const auto converted = store.convertWholeDocumentCards();
    QCOMPARE(converted.size(), 1);
    QCOMPARE(queuedEvent(syncRoot, converted.first().noteEventId).value(field).toObject(),
             (QJsonObject{{QStringLiteral("pdf_sha256"), sha(rebuilt)}, {QStringLiteral("source_built"), true}}));
    QVERIFY(!queuedEvent(syncRoot, converted.first().deleteEventId).contains(field));

    // Round trip: the stamp comes back unchanged from the server's projection.
    const QString eventId = notesAnswer.value(QStringLiteral("event_id")).toString();
    projection.insert(QStringLiteral("review_event\nevents\n") + eventId, notesAnswer);
    QJsonObject readBack;
    for (const auto &card : store.cards())
        for (const auto &event : card.history)
            if (event.value(QStringLiteral("event_id")).toString() == eventId) readBack = event;
    QCOMPARE(readBack.value(field).toObject(), notesAnswer.value(field).toObject());
}

void ReviewStoreTest::cardWritesRecordThePdfVersion()
{
    // thoughts-feedback-version-stamps-SPEC.md: a card the user makes or edits records the PDF on
    // screen, like an answer; other rewrites keep the stamp and every unknown field.
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QDir current(QDir(root.path()).filePath(QStringLiteral("current_pdfs")));
    QVERIFY(current.mkpath(QStringLiteral(".")));
    const auto writeFile = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    const auto sha = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const QString pdf = current.filePath(QStringLiteral("paper.pdf"));
    const QByteArray first = "%PDF-1.4\n% the paper\n";
    QVERIFY(writeFile(pdf, first));
    QVERIFY(writeObject(current.filePath(QStringLiteral(".thoughts-library-index.json")), QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray{
        QJsonObject{{QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                    {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), sha(first)}}}}}));
    ReviewStore store(root.path());
    const QString field = ReviewStore::reviewVersionField();
    const auto version = [](const QString &digest) { return QJsonObject{{QStringLiteral("pdf_sha256"), digest}, {QStringLiteral("source_built"), true}}; };
    const QVariantMap document = {{QStringLiteral("pdf_stem"), QStringLiteral("paper")}, {QStringLiteral("pdf_sha256"), sha(first)}, {QStringLiteral("page_count"), 3}};
    ReviewCard created;
    QString error;
    QVERIFY2(store.createManualCard(document, 2, {0.1, 0.2, 0.7, 0.6}, QStringLiteral("What is shown?"), QString(64, QLatin1Char('b')), &created, &error), qPrintable(error));
    QCOMPARE(created.record.value(field).toObject(), version(sha(first)));
    const auto stored = [&store, &created] {
        for (const auto &card : store.cards())
            if (card.id() == created.id()) return card;
        return ReviewCard();
    };
    QCOMPARE(stored().record.value(field).toObject(), version(sha(first)));

    // A field this version does not know survives every rewrite.
    const QString cardPath = QDir(store.rootPath()).filePath(QStringLiteral("cards/%1.json").arg(created.id()));
    QJsonObject withUnknown = readObject(cardPath);
    QVERIFY(!withUnknown.isEmpty());
    withUnknown.insert(QStringLiteral("future_field"), QJsonObject{{QStringLiteral("kept"), true}});
    QVERIFY(writeObject(cardPath, withUnknown));

    // Each edit records the PDF it was made on, replacing the earlier stamp.
    const QByteArray second = first + "% rebuilt\n";
    QVERIFY(writeFile(pdf, second));
    QVERIFY2(store.updateQuestion(created.id(), QStringLiteral("What is shown now?"), &error), qPrintable(error));
    QCOMPARE(stored().record.value(field).toObject(), version(sha(second)));
    const QByteArray third = second + "% rebuilt again\n";
    QVERIFY(writeFile(pdf, third));
    QVERIFY2(store.updateNote(stored(), QStringLiteral("Read section 2 first."), &error), qPrintable(error));
    QCOMPARE(stored().record.value(field).toObject(), version(sha(third)));
    const QByteArray fourth = third + "% and again\n";
    QVERIFY(writeFile(pdf, fourth));
    QVERIFY2(store.updateCardContent(stored(), {0.1, 0.25, 0.7, 0.6}, QStringLiteral("What is shown here?"), &error), qPrintable(error));
    QCOMPARE(stored().record.value(field).toObject(), version(sha(fourth)));
    QCOMPARE(stored().record.value(QStringLiteral("future_field")).toObject().value(QStringLiteral("kept")).toBool(), true);

    // A rewrite that is not the user's (a reanchor) keeps the stamp.
    QVERIFY(writeFile(pdf, fourth + "% not looked at\n"));
    QVERIFY2(store.markNeedsReanchor(created.id(), &error), qPrintable(error));
    QCOMPARE(stored().record.value(field).toObject(), version(sha(fourth)));
    QCOMPARE(stored().record.value(QStringLiteral("future_field")).toObject().value(QStringLiteral("kept")).toBool(), true);

    // Without the PDF an edit leaves the field out rather than keep a version it was not made on.
    QVERIFY(QFile::remove(pdf));
    QVERIFY2(store.updateQuestion(created.id(), QStringLiteral("What was shown?"), &error), qPrintable(error));
    QVERIFY(!stored().record.contains(field));
    QCOMPARE(stored().record.value(QStringLiteral("future_field")).toObject().value(QStringLiteral("kept")).toBool(), true);
}

void ReviewStoreTest::manualCardIsApprovedDueAndRejectsDuplicates()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    ReviewStore store(temporary.path());
    const QVariantMap document = {{QStringLiteral("pdf_stem"), QStringLiteral("paper")}, {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("page_count"), 3}};
    const QVariantList rect = {0.1, 0.2, 0.7, 0.6};
    const QString visualSha(64, QLatin1Char('b'));
    ReviewCard created;
    QString error;
    QVERIFY2(store.createManualCard(document, 2, rect, QStringLiteral("  What result is shown?  "), visualSha, &created, &error), qPrintable(error));
    QVERIFY(created.isApproved());
    QCOMPARE(created.question(), QStringLiteral("What result is shown?"));
    QCOMPARE(created.page(), 2);
    QCOMPARE(created.rect(), rect);
    QCOMPARE(store.candidateCards().size(), 0);
    QCOMPARE(store.dueCards().size(), 1);

    ReviewCard later;
    QVERIFY2(store.createManualCard(document, 1, rect, QStringLiteral("What is shown later?"),
                                   QString(64, QLatin1Char('c')), &later, &error, 2), qPrintable(error));
    QCOMPARE(store.dueCards().size(), 1);
    const auto firstDue = later.record.value(QStringLiteral("initial_due_at")).toString();
    QVERIFY(!firstDue.isEmpty());
    const auto dueAt = QDateTime::fromString(firstDue, Qt::ISODate).toUTC();
    QVERIFY(dueAt > QDateTime::currentDateTimeUtc().addSecs(47 * 3600));
    QCOMPARE(store.dueCards(dueAt.addSecs(-60)).size(), 1);
    QCOMPARE(store.dueCards(dueAt.addSecs(1)).size(), 2);

    const auto customDueAt = QDateTime::fromSecsSinceEpoch(QDateTime::currentSecsSinceEpoch() + 5 * 3600 + 23 * 60, QTimeZone::UTC);
    ReviewCard custom;
    QVERIFY2(store.createManualCard(document, 3, rect, QStringLiteral("What is shown at the custom time?"),
                                   QString(64, QLatin1Char('d')), &custom, &error, -1, customDueAt), qPrintable(error));
    QCOMPARE(QDateTime::fromString(custom.record.value(QStringLiteral("initial_due_at")).toString(), Qt::ISODate).toUTC(), customDueAt);
    QCOMPARE(store.dueCards(customDueAt.addSecs(-60)).size(), 1);
    QCOMPARE(store.dueCards(customDueAt.addSecs(1)).size(), 2);
    QVERIFY(!store.createManualCard(document, 3, rect, QStringLiteral("Past custom time?"),
                                    QString(64, QLatin1Char('e')), nullptr, &error, -1, customDueAt.addDays(-1)));

    error.clear();
    QVERIFY(!store.createManualCard(document, 2, {0.12, 0.22, 0.68, 0.58}, QStringLiteral("Duplicate?"), visualSha, nullptr, &error));
    QCOMPARE(error, QStringLiteral("A review card already covers this visual region."));
    error.clear();
    ReviewCard large;
    QVERIFY2(store.createManualCard(document, 2, {0.0, 0.0, 1.0, 0.6}, QStringLiteral("What does the page show?"), visualSha, &large, &error), qPrintable(error));
    QCOMPARE(large.rect(), QVariantList({0.0, 0.0, 1.0, 0.6}));
    error.clear();
    QVERIFY(!store.createManualCard(document, 2, {0.0, 0.0, 1.0, 1.1}, QStringLiteral("Outside the page?"), visualSha, nullptr, &error));
    QVERIFY(!error.isEmpty());
    error.clear();
    QVERIFY(!store.createManualCard(document, 2, rect, QStringLiteral("   "), QString(64, QLatin1Char('c')), nullptr, &error));
    QVERIFY(!error.isEmpty());
}

void ReviewStoreTest::archivesLegacyReviewStore()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cards = QDir(temporary.path()).filePath(QStringLiteral("reviews/cards"));
    QVERIFY(QDir().mkpath(cards));
    QFile legacy(QDir(cards).filePath(QStringLiteral("legacy.json")));
    QVERIFY(legacy.open(QIODevice::WriteOnly));
    legacy.write("{\"schema_version\":1,\"card_id\":\"legacy\"}\n");
    legacy.close();
    ReviewStore store(temporary.path());
    QVERIFY(QDir(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards"))).entryList(QDir::Files).isEmpty());
    const QDir backups(QDir(temporary.path()).filePath(QStringLiteral(".runtime/review-backups")));
    QCOMPARE(backups.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size(), 1);
}

void ReviewStoreTest::writesLocallyWithoutLegacyPhoneTransport()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString documents = QDir(temporary.path()).filePath(QStringLiteral("Documents"));
    const QString docs = QDir(documents).filePath(QStringLiteral("docs"));
    const QString syncState = QDir(temporary.path()).filePath(QStringLiteral("sync-state"));
    QVERIFY(QDir().mkpath(docs));
    ReviewStore store(docs, {}, syncState);
    QString error;
    QVERIFY2(store.excludePage(QString(64, QLatin1Char('a')), 4, QStringLiteral("paper"), &error), qPrintable(error));
    const QVariantMap document = {{QStringLiteral("pdf_stem"), QStringLiteral("paper")}, {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('b'))}, {QStringLiteral("page_count"), 4}};
    QVERIFY2(store.createManualCard(document, 2, {0.1, 0.1, 0.4, 0.4}, QStringLiteral("What is shown?"), QString(64, QLatin1Char('c')), nullptr, &error), qPrintable(error));
    const QString cardId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY2(store.recordRating(cardId, QStringLiteral("good"), QDateTime(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC), &error), qPrintable(error));

    const QDir local(QDir(docs).filePath(QStringLiteral("reviews")));
    QCOMPARE(QDir(local.filePath(QStringLiteral("cards"))).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
    QCOMPARE(QDir(local.filePath(QStringLiteral("exclusions"))).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
    QCOMPARE(QDir(local.filePath(QStringLiteral("events/mac-okular"))).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
    const QString eventPath = QDir(local.filePath(QStringLiteral("events/mac-okular")))
                                  .filePath(QDir(local.filePath(QStringLiteral("events/mac-okular")))
                                                .entryList({QStringLiteral("*.json")}, QDir::Files)
                                                .constFirst());
    QFile eventFile(eventPath);
    QVERIFY(eventFile.open(QIODevice::ReadOnly));
    QCOMPARE(QJsonDocument::fromJson(eventFile.readAll()).object().value(QStringLiteral("scheduler_version")).toInt(), 2);
    QVERIFY(!QFileInfo::exists(QDir(documents).filePath(QStringLiteral("mac-to-phone"))));
    QVERIFY(!QFileInfo::exists(QDir(syncState).filePath(QStringLiteral("outbox.json"))));
    DesktopSyncOutbox outbox(QDir(syncState).filePath(QStringLiteral("desktop-sync.sqlite")));
    QVERIFY2(outbox.ready(&error), qPrintable(error));
    const QList<QJsonObject> queued = outbox.operations(&error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(queued.size(), 3);
    QSet<QString> entityTypes;
    for (const QJsonObject &operation : queued) {
        entityTypes.insert(operation.value(QStringLiteral("entity_type")).toString());
    }
    QCOMPARE(entityTypes, QSet<QString>({QStringLiteral("review_card"), QStringLiteral("review_event"), QStringLiteral("review_exclusion")}));
}

void ReviewStoreTest::fedoraProjectionIsAuthoritativeAndCanonical()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString currentHash(64, QLatin1Char('a'));
    const QJsonObject index{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("pdfs"), QJsonArray{QJsonObject{
             {QStringLiteral("filename"), QStringLiteral("course-notes.pdf")},
             {QStringLiteral("pdf_sha256"), currentHash},
         }}},
    };
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")), index));

    const QString staleLocalId = QStringLiteral("00000000-0000-4000-8000-000000000010");
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(staleLocalId)),
                        reviewCard(staleLocalId, QStringLiteral("data-preprocessing"), QString(64, QLatin1Char('c')))));

    const QString activeId = QStringLiteral("00000000-0000-4000-8000-000000000001");
    const QString staleHashId = QStringLiteral("00000000-0000-4000-8000-000000000002");
    const QString needsReanchorId = QStringLiteral("00000000-0000-4000-8000-000000000003");
    QJsonObject projections{
        {QStringLiteral("review_card\ncards\n") + activeId,
         reviewCard(activeId, QStringLiteral("course-notes"), currentHash, QStringLiteral("approved"), 12)},
        {QStringLiteral("review_card\ncards\n") + staleHashId,
         reviewCard(staleHashId, QStringLiteral("course-notes"), QString(64, QLatin1Char('d')), QStringLiteral("approved"), 13)},
        {QStringLiteral("review_card\ncards\n") + needsReanchorId,
         reviewCard(needsReanchorId, QStringLiteral("course-notes"), currentHash, QStringLiteral("needs_reanchor"), 14)},
    };
    ReviewStore store(temporary.path(), [&projections]() { return projections; });

    QCOMPARE(store.cards().size(), 3);
    QCOMPARE(store.activeCards().size(), 1);
    QCOMPARE(store.activeCards().constFirst().id(), activeId);
    QCOMPARE(store.dueCards(QDateTime(QDate(2026, 8, 20), QTime(12, 0), QTimeZone::UTC)).constFirst().id(), activeId);
    // The same rule on cards already read; without the Library comparison the card made on another
    // PDF stays (a reader tab compares it with the PDF it shows).
    const auto ids = [](const QList<ReviewCard> &cards) {
        QSet<QString> result;
        for (const ReviewCard &card : cards) result.insert(card.id());
        return result;
    };
    const QList<ReviewCard> read = store.cards();
    QCOMPARE(ids(store.activeOf(read)), QSet<QString>{activeId});
    QCOMPARE(ids(store.activeOf(read, nullptr, false)), (QSet<QString>{activeId, staleHashId}));
}

void ReviewStoreTest::projectionCardMutationUsesServerRevision()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cardId = QStringLiteral("00000000-0000-4000-8000-000000000020");
    QJsonObject projections{
        {QStringLiteral("review_card\ncards\n") + cardId,
         reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')), QStringLiteral("approved"), 7)},
    };
    ReviewStore store(temporary.path(), [&projections]() { return projections; });
    QString error;
    QVERIFY2(store.updateQuestion(cardId, QStringLiteral("What changed?"), &error), qPrintable(error));

    QFile file(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonObject written = QJsonDocument::fromJson(file.readAll()).object();
    QCOMPARE(written.value(QStringLiteral("revision")).toInteger(), 8);
    QCOMPARE(written.value(QStringLiteral("status")).toString(), QStringLiteral("approved"));
    QCOMPARE(written.value(QStringLiteral("origin_id")).toString(), Thoughts::Platform::reviewOriginId());
    QVERIFY(!written.value(QStringLiteral("updated_at")).toString().isEmpty());
}

void ReviewStoreTest::browserCardsSupportRevisionSafeEditDeleteAndRestore()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cardId = QStringLiteral("00000000-0000-4000-8000-000000000030");
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                        reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')), QStringLiteral("approved"), 4)));
    const QString firstEventId = QStringLiteral("00000000-0000-4000-8000-000000000031");
    const QString lapseEventId = QStringLiteral("00000000-0000-4000-8000-000000000032");
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/%1.json").arg(firstEventId)),
                        QJsonObject{{QStringLiteral("event_id"), firstEventId},
                                    {QStringLiteral("card_id"), cardId},
                                    {QStringLiteral("rating"), QStringLiteral("good")},
                                    {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-05T12:00:00Z")}}));
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/%1.json").arg(lapseEventId)),
                        QJsonObject{{QStringLiteral("event_id"), lapseEventId},
                                    {QStringLiteral("card_id"), cardId},
                                    {QStringLiteral("rating"), QStringLiteral("again")},
                                    {QStringLiteral("reviewed_at"), QStringLiteral("2026-08-06T12:00:00Z")}}));

    ReviewStore store(temporary.path());
    const QDateTime now(QDate(2026, 8, 7), QTime(12, 0), QTimeZone::UTC);
    QList<ReviewBrowserCard> cards = store.browserCards(now);
    QCOMPARE(cards.size(), 1);
    QCOMPARE(cards.constFirst().state, ReviewBrowserState::Learning);
    QCOMPARE(cards.constFirst().card.schedule.reviewCount, 2);
    QCOMPARE(cards.constFirst().card.schedule.lapseCount, 1);
    QCOMPARE(cards.constFirst().card.schedule.lastRating, QStringLiteral("again"));

    QString error;
    QVERIFY2(store.updateQuestion(cardId, QStringLiteral("  What changed in the result?  "), &error), qPrintable(error));
    QFile updatedFile(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)));
    QVERIFY(updatedFile.open(QIODevice::ReadOnly));
    const QJsonObject updated = QJsonDocument::fromJson(updatedFile.readAll()).object();
    QCOMPARE(updated.value(QStringLiteral("revision")).toInteger(), 5);
    QCOMPARE(updated.value(QStringLiteral("primary")).toObject().value(QStringLiteral("question")).toString(), QStringLiteral("What changed in the result?"));

    QVERIFY2(store.deleteCard(cardId, now, &error), qPrintable(error));
    cards = store.browserCards(now.addSecs(1));
    QVERIFY(cards.isEmpty());
    const auto deleted = store.cards().constFirst();
    QVERIFY(deleted.schedule.deleted);
    QVERIFY(!store.restoreCard(cardId, now.addSecs(2), &error));
    QVERIFY(!store.retractRating(cardId, deleted.schedule.deleteEventId, now.addSecs(2), &error));
    ReviewStore reopened(temporary.path());
    QVERIFY(reopened.browserCards(now.addSecs(3)).isEmpty());
    QVERIFY(reopened.dueCards(now.addDays(10)).isEmpty());
    QCOMPARE(QDir(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular"))).entryList({QStringLiteral("*.json")}, QDir::Files).size(), 1);
}

void ReviewStoreTest::bulkDeleteValidatesOnceAndRestoresHistory()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    QStringList ids;
    QJsonObject projections;
    for (int i = 0; i < 87; ++i) {
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        ids.append(id);
        const auto card = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
        projections.insert(QStringLiteral("review_card\ncards\n") + id, card);
        QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), card));
    }
    int reads = 0;
    ReviewStore store(temporary.path(), [&] { ++reads; return projections; });
    QString error;
    const auto now = QDateTime::currentDateTimeUtc();
    auto invalidIds = ids;
    invalidIds.append(QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(!store.setCardsDeleted(invalidIds, true, now, &error));
    QVERIFY(!QDir(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular"))).exists());
    reads = 0;
    auto duplicateIds = ids;
    duplicateIds.append(ids.first());
    QVERIFY2(store.setCardsDeleted(duplicateIds, true, now, &error), qPrintable(error));
    QCOMPARE(reads, 2); // One card snapshot and one event snapshot, independent of selection size.
    ReviewStore local(temporary.path());
    const auto deleted = local.cards();
    QCOMPARE(deleted.size(), 87);
    for (const auto &card : deleted) QVERIFY(card.schedule.deleted);
    const auto eventDirectory = QDir(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular")));
    QCOMPARE(eventDirectory.entryList({QStringLiteral("*.json")}, QDir::Files).size(), 87);
    QVERIFY(!local.setCardsDeleted(ids, false, now.addSecs(1), &error));
    QVERIFY(local.browserCards(now.addSecs(2)).isEmpty());
    for (const auto &card : local.cards()) {
        QVERIFY(card.schedule.deleted);
        QCOMPARE(card.schedule.reviewCount, 0);
    }
    QCOMPARE(eventDirectory.entryList({QStringLiteral("*.json")}, QDir::Files).size(), 87);
}

void ReviewStoreTest::learningQueueUsesTwentyMinuteLookahead()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime now(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC);
    const QString newCardId = QStringLiteral("00000000-0000-4000-8000-000000000040");
    const QString nearLearningCardId = QStringLiteral("00000000-0000-4000-8000-000000000041");
    const QString laterLearningCardId = QStringLiteral("00000000-0000-4000-8000-000000000042");
    const QString dueLearningCardId = QStringLiteral("00000000-0000-4000-8000-000000000043");
    const QString overdueReviewCardId = QStringLiteral("00000000-0000-4000-8000-000000000044");
    for (const QString &cardId : {newCardId, nearLearningCardId, laterLearningCardId, dueLearningCardId, overdueReviewCardId}) {
        QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                            reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
    }
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/near.json")),
                        QJsonObject{{QStringLiteral("event_id"), QStringLiteral("near")},
                                    {QStringLiteral("card_id"), nearLearningCardId},
                                    {QStringLiteral("rating"), QStringLiteral("good")},
                                    {QStringLiteral("scheduler_version"), 2},
                                    {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)}}));
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/later.json")),
                        QJsonObject{{QStringLiteral("event_id"), QStringLiteral("later")},
                                    {QStringLiteral("card_id"), laterLearningCardId},
                                    {QStringLiteral("rating"), QStringLiteral("good")},
                                    {QStringLiteral("scheduler_version"), 2},
                                    {QStringLiteral("reviewed_at"), now.addSecs(11 * 60).toString(Qt::ISODate)}}));
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/due-learning.json")),
                        QJsonObject{{QStringLiteral("event_id"), QStringLiteral("due-learning")},
                                    {QStringLiteral("card_id"), dueLearningCardId},
                                    {QStringLiteral("rating"), QStringLiteral("again")},
                                    {QStringLiteral("scheduler_version"), 2},
                                    {QStringLiteral("reviewed_at"), now.addSecs(-60).toString(Qt::ISODate)}}));
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/overdue-review.json")),
                        QJsonObject{{QStringLiteral("event_id"), QStringLiteral("overdue-review")},
                                    {QStringLiteral("card_id"), overdueReviewCardId},
                                    {QStringLiteral("rating"), QStringLiteral("good")},
                                    {QStringLiteral("reviewed_at"), now.addDays(-5).toString(Qt::ISODate)}}));

    ReviewStore store(temporary.path());
    const QList<ReviewCard> due = store.dueCards(now);
    QCOMPARE(due.size(), 4);
    // A learning step that just came due waits behind the older overdue review.
    QCOMPARE(due.at(0).id(), overdueReviewCardId);
    QCOMPARE(due.at(1).id(), dueLearningCardId);
    QCOMPARE(due.at(2).id(), newCardId);
    QCOMPARE(due.at(3).id(), nearLearningCardId);
    const QList<ReviewBrowserCard> browserCards = store.browserCards(now);
    const auto stateFor = [&browserCards](const QString &cardId) {
        return std::find_if(browserCards.cbegin(), browserCards.cend(), [&cardId](const ReviewBrowserCard &card) {
            return card.card.id() == cardId;
        })->state;
    };
    QCOMPARE(stateFor(nearLearningCardId), ReviewBrowserState::Learning);
    QCOMPARE(stateFor(laterLearningCardId), ReviewBrowserState::Learning);
}

void ReviewStoreTest::skippedCardKeepsItsDueDateAndReturnsToItsPlace()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime now(QDate(2026, 8, 5), QTime(12, 0), QTimeZone::UTC);
    const QString olderId = QStringLiteral("00000000-0000-4000-8000-000000000050");
    const QString newerId = QStringLiteral("00000000-0000-4000-8000-000000000051");
    const QString freshId = QStringLiteral("00000000-0000-4000-8000-000000000052");
    for (const QString &cardId : {olderId, newerId, freshId}) {
        QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                            reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
    }
    const auto review = [&](const QString &name, const QString &cardId, const QDateTime &at) {
        return writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/%1.json").arg(name)),
                           QJsonObject{{QStringLiteral("event_id"), name}, {QStringLiteral("card_id"), cardId},
                                       {QStringLiteral("rating"), QStringLiteral("good")},
                                       {QStringLiteral("reviewed_at"), at.toString(Qt::ISODate)}});
    };
    QVERIFY(review(QStringLiteral("older"), olderId, now.addDays(-10)));
    QVERIFY(review(QStringLiteral("newer"), newerId, now.addDays(-5)));
    const QDateTime olderDue = ReviewStore(temporary.path()).dueCards(now).first().schedule.dueAt;
    QVERIFY(olderDue.isValid() && olderDue <= now);

    const auto order = [&](const QDateTime &at) {
        QStringList ids;
        for (const ReviewCard &card : ReviewStore(temporary.path()).dueCards(at)) ids.append(card.id());
        return ids;
    };
    // Oldest due first; never-answered cards follow.
    QCOMPARE(order(now), QStringList({olderId, newerId, freshId}));

    const QDateTime until = now.addSecs(600);
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/skip.json")),
                        QJsonObject{{QStringLiteral("schema_version"), 3},
                                    {QStringLiteral("event_id"), QStringLiteral("00000000-0000-4000-8000-0000000000aa")},
                                    {QStringLiteral("card_id"), olderId},
                                    {QStringLiteral("skip_id"), QStringLiteral("00000000-0000-4000-8000-0000000000ab")},
                                    {QStringLiteral("queue_action"), QStringLiteral("skip")},
                                    {QStringLiteral("reviewed_at"), now.toString(Qt::ISODate)},
                                    {QStringLiteral("skip_until"), until.toString(Qt::ISODate)}}));
    QCOMPARE(order(now), QStringList({newerId, freshId}));
    const QList<ReviewBrowserCard> rows = ReviewStore(temporary.path()).browserCards(now);
    const auto skipped = std::find_if(rows.cbegin(), rows.cend(), [&](const ReviewBrowserCard &row) { return row.card.id() == olderId; });
    QVERIFY(skipped != rows.cend());
    QCOMPARE(skipped->state, ReviewBrowserState::Skipped);
    QCOMPARE(skipped->card.schedule.dueAt, olderDue);
    QCOMPARE(order(until), QStringList({olderId, newerId, freshId}));
}

void ReviewStoreTest::dueFromIsWhenIsDueNowStarts()
{
    // The reader's Cards list is told when a card becomes due and compares that with its clock;
    // it must agree with the queue's own rule at every time, also just before and at that instant.
    const QDateTime base(QDate(2026, 11, 12), QTime(9, 0), QTimeZone::UTC);
    const QList<QDateTime> dues{QDateTime(), base, base.addSecs(-3600), base.addSecs(15 * 60), base.addDays(2)};
    const QList<QDateTime> skips{QDateTime(), base.addSecs(600), base.addDays(5), base.addDays(-1)};
    const QList<qint64> offsets{-3 * 86400, -3600, -21 * 60, -20 * 60, -19 * 60, -1, 0, 1, 19 * 60, 20 * 60, 3600, 86400, 3 * 86400, 6 * 86400};
    int checked = 0;
    for (const bool reviewed : {false, true}) {
        for (const int intervalDays : {0, 3}) {
            for (const bool fixedInterval : {false, true}) {
                for (const bool retired : {false, true}) {
                    for (const QDateTime &due : dues) {
                        for (const QDateTime &skip : skips) {
                            ReviewCard card;
                            card.schedule.reviewed = reviewed;
                            card.schedule.intervalDays = intervalDays;
                            card.schedule.fixedInterval = fixedInterval;
                            card.schedule.retired = retired;
                            card.schedule.dueAt = due;
                            card.skippedUntil = skip;
                            const QDateTime from = ReviewStore::dueFrom(card);
                            QCOMPARE(from.isValid(), !retired);
                            QList<QDateTime> times;
                            for (const qint64 offset : offsets) times.append(base.addSecs(offset));
                            if (from.isValid() && from.toSecsSinceEpoch() > 0) times << from << from.addSecs(-1);
                            for (const QDateTime &at : std::as_const(times)) {
                                QVERIFY2(ReviewStore::isDueNow(card, at) == (from.isValid() && at >= from),
                                         qPrintable(QStringLiteral("reviewed %1 interval %2 fixed %3 due %4 skip %5 at %6 from %7")
                                                        .arg(reviewed).arg(intervalDays).arg(fixedInterval)
                                                        .arg(due.toString(Qt::ISODate), skip.toString(Qt::ISODate), at.toString(Qt::ISODate), from.toString(Qt::ISODate))));
                                ++checked;
                            }
                        }
                    }
                }
            }
        }
    }
    QVERIFY(checked > 2000);
}

void ReviewStoreTest::dailyQueueOrderingUsesUtcDateSeed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QStringList cardIds{
        QStringLiteral("00000000-0000-4000-8000-000000000001"),
        QStringLiteral("00000000-0000-4000-8000-000000000005"),
        QStringLiteral("00000000-0000-4000-8000-000000000008"),
    };
    for (const QString &cardId : cardIds) {
        QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                            reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
    }
    const QList<ReviewCard> ordered = ReviewStore(temporary.path()).dueCards(
        QDateTime::fromString(QStringLiteral("2026-08-20T23:30:00Z"), Qt::ISODate));
    QStringList orderedIds;
    std::transform(ordered.cbegin(), ordered.cend(), std::back_inserter(orderedIds), [](const ReviewCard &card) { return card.id(); });
    QCOMPARE(orderedIds,
             QStringList({QStringLiteral("00000000-0000-4000-8000-000000000005"),
                          QStringLiteral("00000000-0000-4000-8000-000000000008"),
                          QStringLiteral("00000000-0000-4000-8000-000000000001")}));
}

void ReviewStoreTest::recoversInterruptedSourceCardWithoutOverwritingLaterEdits()
{
    QTemporaryDir temporary;
    const QString source = QDir(temporary.path()).filePath(QStringLiteral("docs-paper/tex/source/main.tex"));
    QVERIFY(QDir().mkpath(QFileInfo(source).absolutePath()));
    const QByteArray before("original source\n"), after("source with markers\n");
    const auto put = [&source](const QByteArray &raw) { QFile f(source); return f.open(QIODevice::WriteOnly) && f.write(raw) == raw.size(); };
    QVERIFY(put(after));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto card = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    const QString journal = QDir(temporary.path()).filePath(QStringLiteral("reviews/source-transactions/%1.json").arg(id));
    const auto pending = QJsonObject {{QStringLiteral("path"), source}, {QStringLiteral("before"), QString::fromLatin1(before.toBase64())}, {QStringLiteral("after"), QString::fromLatin1(after.toBase64())}, {QStringLiteral("card"), card}};
    QVERIFY(writeObject(journal, pending));
    { ReviewStore recovered(temporary.path()); QVERIFY(!QFileInfo::exists(journal)); }
    QVERIFY(QFileInfo::exists(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(id))));
    QVERIFY(put(QByteArrayLiteral("later author edit\n")));
    QVERIFY(writeObject(journal, pending));
    { ReviewStore recovered(temporary.path()); QVERIFY(QFileInfo::exists(journal)); }
    QFile file(source); QVERIFY(file.open(QIODevice::ReadOnly)); QCOMPARE(file.readAll(), QByteArrayLiteral("later author edit\n"));
}

void ReviewStoreTest::basicHalfPagesAreCompleteAndRepeatable()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("paper.pdf"));
    {
        QPdfWriter writer(path);
        QPainter painter(&writer);
        painter.drawText(QPointF(100, 100), QStringLiteral("First page."));
        writer.newPage();
        painter.drawText(QPointF(100, 100), QStringLiteral("Second page."));
    }
    ReviewStore store(dir.path());
    const auto prepared = prepareBasicReviewCards(path);
    QVERIFY(!prepared.contains(QStringLiteral("error")));
    int skipped = -1;
    const auto created = store.createBasicCards(prepared, &skipped);
    QVERIFY2(created.errors.isEmpty(), qPrintable(created.errors.join(QLatin1Char('\n'))));
    QCOMPARE(created.cards.size(), 4);
    QCOMPARE(skipped, 0);
    for (int i = 0; i < 4; ++i) {
        const auto card = created.cards[i];
        QCOMPARE(card.page(), i / 2 + 1);
        QCOMPARE(card.record.value(QStringLiteral("generation_method")).toString(), QStringLiteral("basic_passages_v1"));
        QCOMPARE(card.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("rect")).toArray(),
                 (QJsonArray {0.0, (i % 2) * .5, 1.0, (i % 2 + 1) * .5}));
    }
    QCOMPARE(store.dueCards().size(), 4);
    QVERIFY(!QFileInfo::exists(basicReviewSnapshotPath(dir.path(), created.cards.first().record)));
    const auto repeated = store.createBasicCards(prepared, &skipped);
    QVERIFY(repeated.errors.isEmpty());
    QVERIFY(repeated.cards.isEmpty());
    QCOMPARE(skipped, 4);
    auto invalid = prepared;
    invalid.insert(QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a')));
    QVERIFY(!store.createBasicCards(invalid).errors.isEmpty());
    QCOMPARE(store.cards().size(), 4);
}

void ReviewStoreTest::basicCardsLimitLargeFigures()
{
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("figure.pdf"));
    { QPdfWriter writer(path); writer.setResolution(72); QPainter painter(&writer);
      painter.drawText(QPointF(20, 100), QStringLiteral("Figure starts here."));
      painter.drawText(QPointF(20, 650), QStringLiteral("Figure ends here."));
      painter.fillRect(QRectF(50, 200, 350, 350), Qt::black); }
    const auto prepared = prepareBasicReviewCards(path);
    QVERIFY(!prepared.contains(QStringLiteral("error")));
    QCOMPARE(prepared.value(QStringLiteral("basic_candidates")).toList().size(), 2);
}

void ReviewStoreTest::basicSourcePlanKeepsCrossPagePassagesAndUsesLatestPdf()
{
    QTemporaryDir dir;
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("current_pdfs"))));
    const auto path = dir.filePath(QStringLiteral("current_pdfs/paper.pdf"));
    { QPdfWriter writer(path); QPainter painter(&writer);
      painter.drawText(QPointF(100, 100), QStringLiteral("Start of the answer."));
      writer.newPage(); painter.drawText(QPointF(100, 100), QStringLiteral("End of the answer.")); }
    auto prepared = prepareBasicReviewCards(path);
    const auto sha = prepared.value(QStringLiteral("pdf_sha256")).toString();
    QString registrationError;
    const auto documentId = DocumentIdentityStore(dir.path()).registerImport(QStringLiteral("paper.pdf"), sha, path, &registrationError);
    QVERIFY2(DocumentIdentityStore::validId(documentId), qPrintable(registrationError));

    const QJsonArray regions {QJsonObject{{QStringLiteral("page"),1},{QStringLiteral("rect"),QJsonArray{.1,.7,.9,.9}}},
                              QJsonObject{{QStringLiteral("page"),2},{QStringLiteral("rect"),QJsonArray{.1,.1,.9,.3}}}};
    const QJsonObject anchor {{QStringLiteral("version"),3},{QStringLiteral("kind"),QStringLiteral("basic_source_passages")},
        {QStringLiteral("segments"), QJsonArray{QJsonObject{{QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
            {QStringLiteral("content_sha256"), QString(64, QLatin1Char('f'))}}}}};
    const QJsonObject candidate {{QStringLiteral("key"),QString(64,QLatin1Char('e'))}, {QStringLiteral("regions"),regions},{QStringLiteral("source_anchor"),anchor}};
    const QJsonObject plan {{QStringLiteral("schema_version"),1},{QStringLiteral("pdf_sha256"),sha},{QStringLiteral("cards"),QJsonArray{candidate}}};
    const QJsonObject manifest {{QStringLiteral("pdf"),QJsonObject{{QStringLiteral("pdf_sha256"),sha},{QStringLiteral("basic_review"),plan}}}};
    const auto bytes = QJsonDocument(manifest).toJson();
    const auto digest = QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
    const auto bundle = dir.filePath(QStringLiteral("bundle"));
    QVERIFY(QDir().mkpath(bundle + QStringLiteral("/objects")));
    QFile object(bundle + QStringLiteral("/objects/") + digest); QVERIFY(object.open(QIODevice::WriteOnly)); object.write(bytes); object.close();
    QVERIFY(writeObject(bundle + QStringLiteral("/documents/paper.pdf.json"), QJsonObject{{QStringLiteral("artifact_sha256"),digest}}));
    prepared = prepareBasicReviewCards(path, bundle);
    ReviewStore store(dir.path());
    const auto generated = store.createBasicCards(prepared);
    QVERIFY2(generated.errors.isEmpty(),qPrintable(generated.errors.join(QLatin1Char('\n'))));
    QCOMPARE(generated.cards.size(),1);
    QCOMPARE(generated.cards.first().regions(),regions.toVariantList());
    QCOMPARE(generated.cards.first().record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_selection")).toObject(),anchor);
    QCOMPARE(generated.cards.first().linkingState(), QStringLiteral("pending"));
    QJsonArray largeRegions;
    for (int page = 1; page <= 2; ++page)
        for (double top : {0.0, 0.5})
            largeRegions.append(QJsonObject{{QStringLiteral("page"),page},{QStringLiteral("rect"),QJsonArray{0.0,top,1.0,top+.5}}});
    auto large = candidate;
    large.insert(QStringLiteral("regions"), largeRegions);
    QCOMPARE(limitBasicCardExtent(QJsonArray{large}, sha).size(), 1);
    auto disjoint = large;
    auto invalidAnchor = anchor;
    invalidAnchor.insert(QStringLiteral("segments"), QJsonArray{anchor.value(QStringLiteral("segments")).toArray().first(), anchor.value(QStringLiteral("segments")).toArray().first()});
    disjoint.insert(QStringLiteral("source_anchor"), invalidAnchor);
    QCOMPARE(limitBasicCardExtent(QJsonArray{disjoint}, sha).size(), 1);
    auto invalidDocument = prepared;
    invalidDocument.insert(QStringLiteral("basic_candidates"), QJsonArray{disjoint}.toVariantList());
    QVERIFY(!store.createBasicCards(invalidDocument).errors.isEmpty());
    QVERIFY(QFile::remove(bundle + QStringLiteral("/objects/") + digest));
    QVERIFY(prepareBasicReviewCards(path, bundle).contains(QStringLiteral("error")));
    large.remove(QStringLiteral("source_anchor"));
    const auto parts = limitBasicCardExtent(QJsonArray{large}, sha);
    QCOMPARE(parts.size(), 4);
    QSet<QString> keys;
    for (const auto &part : parts) {
        const auto c = part.toObject();
        QVERIFY(!c.contains(QStringLiteral("source_anchor")));
        QCOMPARE(c.value(QStringLiteral("regions")).toArray().size(), 1);
        const auto r = c.value(QStringLiteral("regions")).toArray().first().toObject().value(QStringLiteral("rect")).toArray();
        QVERIFY(r[3].toDouble() - r[1].toDouble() <= .5);
        keys.insert(c.value(QStringLiteral("key")).toString());
    }
    QCOMPARE(keys.size(), 4);
    QCOMPARE(limitBasicCardExtent(parts, sha), parts);
    QCOMPARE(limitBasicCardExtent(QJsonArray{large}, sha), parts);
    const auto savedPath = basicReviewSnapshotPath(dir.path(), store.cards().first().record);
    QVERIFY(QDir().mkpath(QFileInfo(savedPath).absolutePath()));
    QVERIFY(QFile::copy(path, savedPath));
    QFile original(path); QVERIFY(original.open(QIODevice::WriteOnly)); original.write("A replaced PDF"); original.close();
    const auto card = store.cards().first();
    QCOMPARE(reviewCardPdfPath(dir.path(), card.record), path);
    auto legacyCard = card.record;
    legacyCard.insert(QStringLiteral("generation_method"), QStringLiteral("basic_half_page"));
    QCOMPARE(reviewCardPdfPath(dir.path(), legacyCard), path);
    QVERIFY(QFile::remove(path));
    QCOMPARE(reviewCardPdfPath(dir.path(), card.record), path);
    QVERIFY(store.browserCards().first().hasIssues);
    QCOMPARE(store.dueCards().size(),0);
}

void ReviewStoreTest::defaultFolderCardsStayInTheirProject()
{
    // Two Projects' default folders each hold notes.md: a card made in one
    // uses that Project's own build and never touches the other.
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const auto digest = [](const QByteArray &value) { return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()); };
    const QByteArray original("# Topic\n\nAn exact answer.\n\nNext paragraph.\n");
    const QString bundle = QDir(root).filePath(QStringLiteral("bundle"));
    QJsonArray pdfs, library;
    QStringList relatives;
    for (const auto &id : {QStringLiteral("beta"), QStringLiteral("alpha")}) {
        const QString owner = QStringLiteral(".thoughts/projects/") + digest(id.toUtf8()) + QStringLiteral("/documents");
        const QString relative = owner + QStringLiteral("/notes.md");
        relatives.append(relative);
        const QString path = QDir(root).filePath(relative);
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); source.write(original); source.close();
        const QString filename = SourceOwner::defaultFolderOutput(relative);
        const QString pdfHash = digest(filename.toUtf8());
        const QJsonObject artifact{{QStringLiteral("pdf"), QJsonObject{{QStringLiteral("pdf_sha256"), pdfHash}}},
            {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("path"), relative}, {QStringLiteral("sha256"), digest(original)}}}}};
        const auto bytes = QJsonDocument(artifact).toJson(QJsonDocument::Compact);
        QVERIFY(writeObject(QDir(bundle).filePath(QStringLiteral("objects/") + digest(bytes)), artifact));
        pdfs.append(QJsonObject{{QStringLiteral("filename"), filename}, {QStringLiteral("project"), owner},
            {QStringLiteral("pdf_sha256"), pdfHash}, {QStringLiteral("source_artifact_sha256"), digest(bytes)}});
        library.append(QJsonObject{{QStringLiteral("filename"), filename}, {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
    }
    QVERIFY(writeObject(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")), {{QStringLiteral("pdfs"), library}}));
    QVERIFY(writeObject(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-source-index.json")), {{QStringLiteral("pdfs"), pdfs}}));
    ReviewStore store(root);
    const QString alpha = QDir(root).filePath(relatives.last());
    const QJsonObject request{{QStringLiteral("command"), QStringLiteral("create-review-card")}, {QStringLiteral("source"), alpha},
        {QStringLiteral("expected_source_sha256"), digest(original)}, {QStringLiteral("start_line"), 3}, {QStringLiteral("end_line"), 4},
        {QStringLiteral("question"), QStringLiteral("What is the answer?")}, {QStringLiteral("due_at"), QDateTime::currentDateTimeUtc().addDays(2).toString(Qt::ISODate)},
        {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    const auto result = store.sourceCardCommand(request, bundle);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QFile marked(alpha); QVERIFY(marked.open(QIODevice::ReadOnly));
    QVERIFY(marked.readAll().contains("<!-- thoughts:v1:start review:"));
    QFile other(QDir(root).filePath(relatives.first())); QVERIFY(other.open(QIODevice::ReadOnly));
    QCOMPARE(other.readAll(), original);
    const auto cards = store.cards();
    QCOMPARE(cards.size(), 1);
    QCOMPARE(cards.first().linkingSource(), relatives.last());
}

void ReviewStoreTest::sourceSelectionCreationAndRetries()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QString relative = QStringLiteral("docs-test/markdown/source/answer.md");
    const QString path = QDir(root).filePath(relative);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    const QByteArray original("# Topic\n\nAn exact answer with caf\xc3\xa9.\n\nNext paragraph.\n");
    QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); source.write(original); source.close();
    const auto digest = [](const QByteArray &value) { return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()); };
    const auto documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(writeObject(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")),
        {{QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("test.pdf")}, {QStringLiteral("document_id"), documentId}}}}}));
    const QString pdfHash(64, QLatin1Char('a'));
    const QJsonObject artifact{{QStringLiteral("pdf"), QJsonObject{{QStringLiteral("pdf_sha256"), pdfHash}}},
        {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("path"), relative}, {QStringLiteral("sha256"), digest(original)}}}}};
    const auto bytes = QJsonDocument(artifact).toJson(QJsonDocument::Compact);
    const QString bundle = QDir(root).filePath(QStringLiteral("bundle"));
    QVERIFY(writeObject(QDir(bundle).filePath(QStringLiteral("objects/") + digest(bytes)), artifact));
    QVERIFY(writeObject(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-source-index.json")),
        {{QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("test.pdf")},
            {QStringLiteral("project"), QStringLiteral("docs-test")}, {QStringLiteral("pdf_sha256"), pdfHash},
            {QStringLiteral("source_artifact_sha256"), digest(bytes)}}}}}));
    ReviewStore store(root);
    const QString due = QDateTime::currentDateTimeUtc().addDays(2).toString(Qt::ISODate);
    QJsonObject request{{QStringLiteral("command"), QStringLiteral("create-review-card")}, {QStringLiteral("source"), path},
        {QStringLiteral("expected_source_sha256"), digest(original)}, {QStringLiteral("start_line"), 3}, {QStringLiteral("end_line"), 4},
        {QStringLiteral("question"), QStringLiteral("What is the answer?")}, {QStringLiteral("due_at"), due},
        {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    auto invalid = request; invalid.insert(QStringLiteral("expected_source_sha256"), QString(64, QLatin1Char('b')));
    QCOMPARE(store.sourceCardCommand(invalid, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("source_conflict"));
    const auto result = store.sourceCardCommand(request, bundle);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("pdf_placement_state")).toString(), QStringLiteral("pending"));
    QVERIFY(source.open(QIODevice::ReadOnly)); const auto marked = source.readAll(); source.close();
    QVERIFY(marked.contains(original.split('\n')[2]));
    QVERIFY(marked.contains("<!-- thoughts:v1:start review:"));
    auto restored = marked;
    const auto id = result.value(QStringLiteral("card_id")).toString();
    restored.replace(QByteArray(QByteArray("<!-- thoughts:v1:start review:") + id.toUtf8() + ":answer:0 -->\n"), "");
    restored.replace(QByteArray(QByteArray("<!-- thoughts:v1:end review:") + id.toUtf8() + ":answer:0 -->\n"), "");
    QCOMPARE(restored, original);
    const auto cards = store.cards(); QCOMPARE(cards.size(), 1);
    QCOMPARE(cards.first().linkingState(), QStringLiteral("linked"));
    QCOMPARE(cards.first().schedule.dueAt, QDateTime::fromString(due, Qt::ISODateWithMs));
    QVERIFY(cards.first().regions().isEmpty());
    QVERIFY(store.dueCards(QDateTime::currentDateTimeUtc().addDays(3)).isEmpty());
    QCOMPARE(store.browserCards().first().state, ReviewBrowserState::Repairing);
    QVERIFY(store.sourceCardCommand(request, bundle).value(QStringLiteral("replayed")).toBool());
    auto changed = request; changed.insert(QStringLiteral("question"), QStringLiteral("Changed?"));
    QCOMPARE(store.sourceCardCommand(changed, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("request_conflict"));
    changed = request; changed.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    changed.insert(QStringLiteral("expected_source_sha256"), digest(marked)); changed.insert(QStringLiteral("start_line"), 4); changed.insert(QStringLiteral("end_line"), 5);
    QCOMPARE(store.sourceCardCommand(changed, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_selection"));
    auto second = request;
    second.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    second.insert(QStringLiteral("question"), QStringLiteral("What follows?"));
    second.insert(QStringLiteral("expected_source_sha256"), digest(marked));
    second.insert(QStringLiteral("start_line"), 7);
    second.insert(QStringLiteral("end_line"), 8);
    const auto secondResult = store.sourceCardCommand(second, bundle);
    QVERIFY2(secondResult.value(QStringLiteral("accepted")).toBool(), QJsonDocument(secondResult).toJson().constData());
    const auto afterSecond = store.cards();
    QCOMPARE(afterSecond.size(), 2);
    for (const auto &item : afterSecond) QCOMPARE(item.linkingState(), QStringLiteral("linked"));
    QVERIFY(source.open(QIODevice::ReadOnly)); const auto twiceMarked = source.readAll(); source.close();
    const auto listed = store.sourceCardCommand({{QStringLiteral("command"), QStringLiteral("list-review-cards")}, {QStringLiteral("source"), path}}, bundle);
    QCOMPARE(listed.value(QStringLiteral("cards")).toArray().size(), 2);
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), twiceMarked); source.close();
    QVERIFY(QFile::rename(path, path + QStringLiteral(".moved")));
    for (const auto &item : store.cards()) QVERIFY(!item.schedule.deleted);
    QVERIFY(store.sourceCardCommand(request, bundle).value(QStringLiteral("replayed")).toBool());
    QVERIFY(QFile::rename(path + QStringLiteral(".moved"), path));
    QJsonObject action{{QStringLiteral("command"), QStringLiteral("set-review-card-interval")},
        {QStringLiteral("card_id"), id}, {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("source"), path}, {QStringLiteral("expected_source_sha256"), digest(twiceMarked)},
        {QStringLiteral("interval_unit"), QStringLiteral("hours")}, {QStringLiteral("interval_amount"), 3}};
    const auto intervalResult = store.sourceCardCommand(action, bundle);
    QVERIFY2(intervalResult.value(QStringLiteral("accepted")).toBool(), QJsonDocument(intervalResult).toJson().constData());
    const auto cardHistory = [&] { for (const auto &card : store.cards()) if (card.id() == id) return card.history; return QList<QJsonObject>{}; };
    const auto eventCount = cardHistory().size();
    QVERIFY(store.sourceCardCommand(action, bundle).value(QStringLiteral("replayed")).toBool());
    QCOMPARE(cardHistory().size(), eventCount);
    auto conflict = action; conflict.insert(QStringLiteral("interval_amount"), 4);
    QCOMPARE(store.sourceCardCommand(conflict, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("request_conflict"));
    action.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    action.insert(QStringLiteral("interval_unit"), QStringLiteral("never")); action.remove(QStringLiteral("interval_amount"));
    QVERIFY(store.sourceCardCommand(action, bundle).value(QStringLiteral("accepted")).toBool());
    for (const auto &card : store.cards()) if (card.id() == id) QVERIFY(card.schedule.retained);
    action.insert(QStringLiteral("command"), QStringLiteral("delete-review-card")); action.remove(QStringLiteral("interval_unit"));
    action.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    action.insert(QStringLiteral("expected_source_sha256"), digest(original));
    QCOMPARE(store.sourceCardCommand(action, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("source_conflict"));
    action.insert(QStringLiteral("expected_source_sha256"), digest(twiceMarked));
    auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(path, {});
    scanned.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
    const QJsonObject files{{relative, scanned}};
    Thoughts::installSourceLocationIndex(root, {{QStringLiteral("schema_version"), 1},
        {QStringLiteral("files"), files}, {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, files);
    const auto deleted = store.sourceCardCommand(action, bundle);
    QVERIFY2(deleted.value(QStringLiteral("accepted")).toBool(), QJsonDocument(deleted).toJson().constData());
    QCOMPARE(deleted.value(QStringLiteral("marker_removal")).toString(), QStringLiteral("complete"));
    QVERIFY(source.open(QIODevice::ReadOnly)); const auto afterDelete = source.readAll(); source.close();
    QVERIFY(!afterDelete.contains(id.toUtf8())); QVERIFY(afterDelete.contains(original.split('\n')[2]));
    QVERIFY(afterDelete.contains(secondResult.value(QStringLiteral("card_id")).toString().toUtf8()));
    const auto afterDeleteEvents = cardHistory();
    QVERIFY(store.sourceCardCommand(action, bundle).value(QStringLiteral("replayed")).toBool());
    QCOMPARE(cardHistory(), afterDeleteEvents);
    for (const auto &card : store.cards()) QCOMPARE(card.schedule.deleted, card.id() == id);
    // Simulate an interrupted acknowledgement after the durable event committed.
    const auto journalPath = QDir(root).filePath(QStringLiteral("reviews/card-commands/%1.json").arg(action.value(QStringLiteral("request_id")).toString()));
    QFile journalFile(journalPath); QVERIFY(journalFile.open(QIODevice::ReadOnly));
    auto journal = QJsonDocument::fromJson(journalFile.readAll()).object(); journalFile.close();
    journal.remove(QStringLiteral("complete")); QVERIFY(writeObject(journalPath, journal));
    QVERIFY(store.sourceCardCommand(action, bundle).value(QStringLiteral("replayed")).toBool());
    QCOMPARE(cardHistory(), afterDeleteEvents);
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), afterDelete); source.close();
    // An explicitly deleted missing-marker card remains terminal without editing text.
    QVERIFY(source.open(QIODevice::WriteOnly | QIODevice::Truncate)); source.write(original); source.close();
    action.remove(QStringLiteral("source")); action.remove(QStringLiteral("expected_source_sha256"));
    action.insert(QStringLiteral("card_id"), secondResult.value(QStringLiteral("card_id")));
    action.insert(QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(store.sourceCardCommand(action, bundle).value(QStringLiteral("accepted")).toBool());
    for (const auto &card : store.cards()) QVERIFY(card.schedule.deleted);
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), original); source.close();
    const QByteArray fenced("```\ncode line\n```\n");
    QVERIFY(!markReviewSourceSelection(relative, fenced, 2, 3, QUuid::createUuid().toString(QUuid::WithoutBraces)).error.isEmpty());
    const QByteArray separatedBlocks("### Topic\n\nFirst answer.\n\n### Details\n\nSecond answer.\n\nNext paragraph.\n");
    const auto spaced = markReviewSourceSelection(relative, separatedBlocks, 2, 9,
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY2(spaced.error.isEmpty(), qPrintable(spaced.error));
    QVERIFY(spaced.replacement.contains("First answer."));
    QVERIFY(spaced.replacement.contains("Second answer."));
    const QByteArray paragraph("First line\nSecond line\n");
    QVERIFY(!markReviewSourceSelection(relative, paragraph, 2, 3, QUuid::createUuid().toString(QUuid::WithoutBraces)).error.isEmpty());
    const QByteArray tex("Before.\n\\begin{equation}\nx=y\n\\end{equation}\nAfter.\n");
    QVERIFY(markReviewSourceSelection(QStringLiteral("docs-test/tex/source/a.tex"), tex, 2, 5, QUuid::createUuid().toString(QUuid::WithoutBraces)).error.isEmpty());
    QVERIFY(!markReviewSourceSelection(QStringLiteral("docs-test/tex/source/a.tex"), tex, 3, 4, QUuid::createUuid().toString(QUuid::WithoutBraces)).error.isEmpty());
}

void ReviewStoreTest::pdfOnlyCardMovesIntoASourceDocumentWithItsHistory()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QString relative = QStringLiteral("docs-notes/markdown/source/notes.md");
    const QString path = QDir(root).filePath(relative);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    const QByteArray original("# Notes\n\nHow the paper builds its model.\n\nMore notes.\n");
    QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly)); source.write(original); source.close();
    const auto digest = [](const QByteArray &value) { return QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()); };
    const auto newId = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
    const QString paperId = newId(), notesId = newId(), otherId = newId();
    QVERIFY(writeObject(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")), {{QStringLiteral("pdfs"), QJsonArray{
        QJsonObject{{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("document_id"), paperId}},
        QJsonObject{{QStringLiteral("filename"), QStringLiteral("other.pdf")}, {QStringLiteral("document_id"), otherId}},
        QJsonObject{{QStringLiteral("filename"), QStringLiteral("notes.pdf")}, {QStringLiteral("document_id"), notesId}}}}}));
    const QString pdfHash(64, QLatin1Char('a'));
    const QJsonObject artifact{{QStringLiteral("pdf"), QJsonObject{{QStringLiteral("pdf_sha256"), pdfHash}}},
        {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("path"), relative}, {QStringLiteral("sha256"), digest(original)}}}}};
    const auto bytes = QJsonDocument(artifact).toJson(QJsonDocument::Compact);
    const QString bundle = QDir(root).filePath(QStringLiteral("bundle"));
    QVERIFY(writeObject(QDir(bundle).filePath(QStringLiteral("objects/") + digest(bytes)), artifact));
    QVERIFY(writeObject(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-source-index.json")),
        {{QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("notes.pdf")},
            {QStringLiteral("project"), QStringLiteral("docs-notes")}, {QStringLiteral("pdf_sha256"), pdfHash},
            {QStringLiteral("source_artifact_sha256"), digest(bytes)}}}}}));
    // The Project spans its default folder (the imported paper) and docs-notes; other.pdf is in no Project.
    const QString manifestPath = ProjectConfiguration::path(root, QStringLiteral("models"));
    const QString defaultFolder = QDir(root).relativeFilePath(QFileInfo(manifestPath).absolutePath());
    const QString defaultId = newId(), notesFolder = newId();
    QVERIFY(writeObject(manifestPath, {{QStringLiteral("schema_version"), 5}, {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
        {QStringLiteral("id"), QStringLiteral("models")}, {QStringLiteral("name"), QStringLiteral("Models")}, {QStringLiteral("default_folder_id"), defaultId},
        {QStringLiteral("source_folders"), QJsonArray{
            QJsonObject{{QStringLiteral("folder_id"), defaultId}, {QStringLiteral("path"), defaultFolder}, {QStringLiteral("document_id"), newId()}},
            QJsonObject{{QStringLiteral("folder_id"), notesFolder}, {QStringLiteral("path"), QStringLiteral("docs-notes")}, {QStringLiteral("document_id"), newId()}}}},
        {QStringLiteral("documents"), QJsonArray{
            QJsonObject{{QStringLiteral("document_id"), paperId}, {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), true},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), defaultId}, {QStringLiteral("path"), QStringLiteral("paper.pdf")}}}},
            QJsonObject{{QStringLiteral("document_id"), notesId}, {QStringLiteral("format"), QStringLiteral("markdown")}, {QStringLiteral("included"), true},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), notesFolder}, {QStringLiteral("path"), QStringLiteral("markdown/source/notes.md")}}}}}}}));
    // Distinct pages and fingerprints: equal visual answers would be treated as one card.
    const auto pdfOnlyCard = [&](const QString &id, const QString &stem, const QString &document, QChar fingerprint) {
        auto card = reviewCard(id, stem, QString(64, QLatin1Char('b')), QStringLiteral("approved"), 3);
        auto primary = card.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("document_id"), document);
        primary.insert(QStringLiteral("pdf_document_id"), document);
        primary.insert(QStringLiteral("page"), 9);
        primary.insert(QStringLiteral("question"), QStringLiteral("How is the model built?"));
        primary.insert(QStringLiteral("page_visual_sha256"), QString(64, fingerprint));
        primary.insert(QStringLiteral("source_kind"), QStringLiteral("pdf"));
        card.insert(QStringLiteral("primary"), primary);
        card.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pdf_without_source")}});
        return writeObject(QDir(root).filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), card);
    };
    const QString id = newId(), stranger = newId();
    QVERIFY(pdfOnlyCard(id, QStringLiteral("paper"), paperId, QLatin1Char('c')));
    QVERIFY(pdfOnlyCard(stranger, QStringLiteral("other"), otherId, QLatin1Char('e')));
    ReviewStore store(root);
    QString error;
    const auto reviewed = QDateTime::currentDateTimeUtc().addDays(-1);
    QVERIFY2(store.recordRating(id, QStringLiteral("custom_interval"), reviewed, &error, reviewed.addDays(14)), qPrintable(error));
    // Review events are keyed by card ID and carry no document: the move leaves them untouched.
    const auto events = [&] {
        QMap<QString, QByteArray> found;
        QDirIterator it(QDir(root).filePath(QStringLiteral("reviews/events")), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { QFile f(it.next()); if (f.open(QIODevice::ReadOnly)) found.insert(it.filePath(), f.readAll()); }
        return found;
    };
    const auto history = events();
    QCOMPARE(history.size(), 1);
    QVERIFY(history.first().contains(id.toUtf8()));
    QJsonObject request{{QStringLiteral("command"), QStringLiteral("move-review-card")}, {QStringLiteral("card_id"), id},
        {QStringLiteral("request_id"), newId()}, {QStringLiteral("source"), path}, {QStringLiteral("expected_source_sha256"), digest(original)},
        {QStringLiteral("start_line"), 3}, {QStringLiteral("end_line"), 4}};
    auto foreign = request; foreign.insert(QStringLiteral("card_id"), stranger); foreign.insert(QStringLiteral("request_id"), newId());
    QSettings().remove(QStringLiteral("thoughts-server/capabilities"));
    QCOMPARE(store.sourceCardCommand(foreign, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("other_project"));
    auto stale = request; stale.insert(QStringLiteral("expected_source_sha256"), QString(64, QLatin1Char('d')));
    QCOMPARE(store.sourceCardCommand(stale, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("source_conflict"));
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), original); source.close();

    const auto result = store.sourceCardCommand(request, bundle);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("card_moved"));
    QCOMPARE(result.value(QStringLiteral("from_document")).toString(), QStringLiteral("paper.pdf"));
    QCOMPARE(result.value(QStringLiteral("document")).toString(), QStringLiteral("notes.pdf"));
    QVERIFY(source.open(QIODevice::ReadOnly)); const auto marked = source.readAll(); source.close();
    QVERIFY(marked.contains(QByteArray(QByteArray("<!-- thoughts:v1:start review:") + id.toUtf8() + ":answer:0 -->\nHow the paper builds its model.")));
    QCOMPARE(stripReviewCardMarkers(relative, marked, id).text, original);
    const auto cards = store.cards();
    const auto card = *std::find_if(cards.cbegin(), cards.cend(), [&](const ReviewCard &c) { return c.id() == id; });
    QCOMPARE(card.pdfStem(), QStringLiteral("notes"));
    QCOMPARE(card.linkingState(), QStringLiteral("linked"));
    QCOMPARE(card.question(), QStringLiteral("How is the model built?"));
    QCOMPARE(card.record.value(QStringLiteral("revision")).toInt(), 4);
    const auto primary = card.record.value(QStringLiteral("primary")).toObject();
    QCOMPARE(primary.value(QStringLiteral("document_id")).toString(), notesId);
    QCOMPARE(primary.value(QStringLiteral("pdf_document_id")).toString(), notesId);
    for (const auto &gone : {QStringLiteral("source_kind"), QStringLiteral("pdf_sha256"), QStringLiteral("page"), QStringLiteral("rect")})
        QVERIFY2(!primary.contains(gone), qPrintable(gone));
    QCOMPARE(primary.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().size(), 1);
    QCOMPARE(card.record.value(QStringLiteral("move")).toObject(),
        (QJsonObject{{QStringLiteral("from_pdf_stem"), QStringLiteral("paper")}, {QStringLiteral("to_pdf_stem"), QStringLiteral("notes")}}));
    const auto change = card.record.value(QStringLiteral("source_change")).toObject();
    QCOMPARE(change.value(QStringLiteral("transaction_id")).toString(), request.value(QStringLiteral("request_id")).toString());
    QCOMPARE(change.value(QStringLiteral("base_sha256")).toString(), digest(original));
    QCOMPARE(events(), history);
    // An uncertain retry replays; the same request ID with other arguments is refused.
    const auto replay = store.sourceCardCommand(request, bundle);
    QVERIFY(replay.value(QStringLiteral("replayed")).toBool());
    QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), marked); source.close();
    auto changed = request; changed.insert(QStringLiteral("end_line"), 5);
    QCOMPARE(store.sourceCardCommand(changed, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("request_conflict"));
    // Now linked: a further move goes through its markers.
    changed = request; changed.insert(QStringLiteral("request_id"), newId()); changed.insert(QStringLiteral("expected_source_sha256"), digest(marked));
    QCOMPARE(store.sourceCardCommand(changed, bundle).value(QStringLiteral("error_code")).toString(), QStringLiteral("linked_card"));
    // Once the server accepts moves across Projects, the card outside this Project may move in too.
    QSettings().setValue(QStringLiteral("thoughts-server/capabilities"), QStringList{QStringLiteral("client_review_card_move_any_project_v1")});
    const int more = static_cast<int>(marked.left(marked.indexOf("More notes.")).count('\n')) + 1;
    foreign.insert(QStringLiteral("request_id"), newId()); foreign.insert(QStringLiteral("expected_source_sha256"), digest(marked));
    foreign.insert(QStringLiteral("start_line"), more); foreign.insert(QStringLiteral("end_line"), more + 1);
    const auto across = store.sourceCardCommand(foreign, bundle);
    QSettings().remove(QStringLiteral("thoughts-server/capabilities"));
    QVERIFY2(across.value(QStringLiteral("accepted")).toBool(), QJsonDocument(across).toJson().constData());
    QCOMPARE(across.value(QStringLiteral("from_document")).toString(), QStringLiteral("other.pdf"));
    QCOMPARE(across.value(QStringLiteral("document")).toString(), QStringLiteral("notes.pdf"));
}

void ReviewStoreTest::sourceCommandsAcceptGeneratedCardIds()
{
    QTemporaryDir dir;
    const QString id = QStringLiteral("0729a59c-a185-5924-a781-f9f1f7b3033e");
    QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)),
        reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
    ReviewStore store(dir.path());
    const QJsonObject request{{QStringLiteral("command"), QStringLiteral("delete-review-card")},
        {QStringLiteral("card_id"), id}, {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    const auto result = store.sourceCardCommand(request, {});
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QVERIFY(store.cards().first().schedule.deleted);
    QVERIFY(store.sourceCardCommand(request, {}).value(QStringLiteral("replayed")).toBool());
}

QTEST_MAIN(ReviewStoreTest)
#include "reviewstoretest.moc"


void ReviewStoreTest::manualLinkingOutcomes_data()
{
    QTest::addColumn<QString>("mode");
    QTest::newRow("linked") << QStringLiteral("linked");
    QTest::newRow("pdf-points") << QStringLiteral("points");
    QTest::newRow("context-recovers-structural-line") << QStringLiteral("context");
    QTest::newRow("typeset-citation-inside-passage") << QStringLiteral("citation");
    QTest::newRow("only-typeset-citation-unmappable") << QStringLiteral("citation_only");
    QTest::newRow("historical-line-unmappable") << QStringLiteral("ambiguous");
    QTest::newRow("missing-source-bundle") << QStringLiteral("missing");
    QTest::newRow("pdf-without-source") << QStringLiteral("pdf_only");
    QTest::newRow("wrong-pdf-revision") << QStringLiteral("wrong_pdf");
    // An edit since the build: the answer's text, found once in the current source, still links it.
    QTest::newRow("working-source-changed") << QStringLiteral("stale_source");
    QTest::newRow("working-source-answer-rewritten") << QStringLiteral("stale_answer");
    QTest::newRow("source-edited-during-background-preparation") << QStringLiteral("changed_after_preparation");
}

void ReviewStoreTest::pdfOnlyCardReportsItsDocumentSourceState()
{
    auto record = reviewCard(QUuid::createUuid().toString(QUuid::WithoutBraces),
                             QStringLiteral("combined"), QString(64, QLatin1Char('a')));
    auto primary = record.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("source_kind"), QStringLiteral("pdf"));
    record.insert(QStringLiteral("primary"), primary);
    ReviewCard card {record, {}};
    QCOMPARE(card.linkingState(), QStringLiteral("pdf_without_source"));
    card.sourceLocation = {{QStringLiteral("state"), QStringLiteral("unavailable")}};
    QCOMPARE(card.linkingState(), QStringLiteral("pdf_without_source"));
}

void ReviewStoreTest::manualLinkingOutcomes()
{
    QFETCH(QString, mode);
    QTemporaryDir dir;
    const auto pdfPath = dir.filePath(QStringLiteral("current_pdfs/paper.pdf"));
    QVERIFY(QDir().mkpath(QFileInfo(pdfPath).absolutePath()));
    const QString answer = mode == QLatin1String("citation") ? QStringLiteral("The complete answer cites prior work [3], with enough context.")
        : mode == QLatin1String("citation_only") ? QStringLiteral("[3],")
        : QStringLiteral("The complete answer has enough context to locate its source.");
    { QPdfWriter writer(pdfPath); writer.setResolution(72); QPainter painter(&writer);
      painter.drawText(QPointF(30, 100), answer);
      if (mode == QLatin1String("points")) painter.drawText(QPointF(30, 30), QStringLiteral("Title")); }
    auto document = prepareBasicReviewCards(pdfPath);
    const auto sha = document.value(QStringLiteral("pdf_sha256")).toString();
    const auto visual = fingerprintReviewPage(pdfPath, 1).value(QStringLiteral("page_visual_sha256")).toString();
    const auto bundle = dir.filePath(QStringLiteral("bundle"));
    document.insert(QStringLiteral("source_bundle_root"), bundle);
    const QString sourceId = QStringLiteral("docs-test/tex/source/main.tex");
    const auto sourcePath = dir.filePath(sourceId);
    const QByteArray source = mode == QLatin1String("points") ? QByteArray("Title\n") + QByteArray(18, '\n') + answer.toUtf8() + "\n" : mode == QLatin1String("ambiguous") ? QByteArray("An entirely different source sentence.\n\\par\n")
        : mode == QLatin1String("citation") ? QByteArray("The complete answer cites prior work \\cite{key}, with enough context.\n\\par\nTitle\n")
        : mode == QLatin1String("citation_only") ? QByteArray("\\cite{key},\n\\par\n") : answer.toUtf8() + "\n\\par\nTitle\n";
    auto put = [](const QString &path, const QByteArray &bytes) { QDir().mkpath(QFileInfo(path).absolutePath()); QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(); };
    QVERIFY(put(sourcePath, source));
    const auto digest = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
    const auto sourceSha = digest(source);
    const auto lookupPath = dir.filePath(QStringLiteral("lookup.zip"));
    { KZip zip(lookupPath); QVERIFY(zip.open(QIODevice::WriteOnly));
      QVERIFY(zip.writeFile(QStringLiteral("format.json"), QByteArrayLiteral("{\"schema_version\":1,\"format\":\"thoughts-reverse-synctex\"}")));
      const int line = mode == QLatin1String("linked") || mode.startsWith(QLatin1String("stale_")) || mode.startsWith(QLatin1String("citation")) ? 1 : 2;
      const auto entries = mode == QLatin1String("points")
          ? QStringLiteral("h\t%1\t1\t0\t45\t600\t20\t2\nh\t%1\t20\t0\t115\t600\t20\t2\n").arg(sourceId)
          : QStringLiteral("h\t%1\t%2\t0\t0\t600\t400\t2\n").arg(sourceId).arg(line);
      QVERIFY(zip.writeFile(QStringLiteral("pages/1.tsv"), entries.toUtf8()));
      QVERIFY(zip.close()); }
    QFile lookup(lookupPath); QVERIFY(lookup.open(QIODevice::ReadOnly)); const auto lookupBytes = lookup.readAll();
    const auto lookupSha = digest(lookupBytes);
    QVERIFY(put(bundle + QStringLiteral("/objects/") + sourceSha, source));
    QVERIFY(put(bundle + QStringLiteral("/objects/") + lookupSha, lookupBytes));
    const bool pdfOnly = mode == QLatin1String("pdf_only");
    const QJsonObject segment {{QStringLiteral("start_page"),1},{QStringLiteral("page_count"),1},
        {QStringLiteral("source_available"),!pdfOnly}, {QStringLiteral("lookup_format"),QStringLiteral("thoughts-reverse-synctex-v1")},
        {QStringLiteral("lookup_object"),lookupSha},{QStringLiteral("lookup_sha256"),lookupSha},
        {QStringLiteral("inputs"),QJsonArray{QJsonObject{{QStringLiteral("synctex_path"),sourceId},{QStringLiteral("source_file"),sourceId},{QStringLiteral("source_sha256"),sourceSha}}}}};
    const QJsonObject manifest {{QStringLiteral("schema_version"),1},{QStringLiteral("kind"),QStringLiteral("thoughts-document-source-artifact")},
        {QStringLiteral("filename"),QStringLiteral("paper.pdf")},
        {QStringLiteral("pdf"),QJsonObject{{QStringLiteral("filename"),QStringLiteral("paper.pdf")},{QStringLiteral("pdf_sha256"),sha},
            {QStringLiteral("page_count"),1},{QStringLiteral("project"),QStringLiteral("docs-test")},{QStringLiteral("segments"),QJsonArray{segment}}}},
        {QStringLiteral("sources"),QJsonArray{QJsonObject{{QStringLiteral("path"),sourceId},{QStringLiteral("sha256"),sourceSha},{QStringLiteral("object"),sourceSha}}}}};
    const auto artifact = QJsonDocument(manifest).toJson(); const auto artifactSha = digest(artifact);
    QVERIFY(put(bundle + QStringLiteral("/objects/") + artifactSha, artifact));
    if (mode != QLatin1String("missing")) QVERIFY(writeObject(bundle + QStringLiteral("/documents/paper.pdf.json"), {{QStringLiteral("artifact_sha256"),artifactSha}, {QStringLiteral("schema_version"),1}, {QStringLiteral("kind"),QStringLiteral("thoughts-document-source-artifact-pointer")}, {QStringLiteral("filename"),QStringLiteral("paper.pdf")}, {QStringLiteral("pdf_sha256"),sha}}));
    if (mode == QLatin1String("wrong_pdf")) document.insert(QStringLiteral("pdf_sha256"),QString(64,QLatin1Char('a')));
    if (mode == QLatin1String("stale_source")) QVERIFY(put(sourcePath, source + "Another author edit.\n"));
    const QByteArray rewritten("The complete answer was rewritten by its author since.\n\\par\nTitle\n");
    if (mode == QLatin1String("stale_answer")) QVERIFY(put(sourcePath, rewritten));
    ReviewCard card;
    QString error;
    ReviewStore store(dir.path());
    const QVariantList region = mode == QLatin1String("points") ? QVariantList{0.0,0.09,1.0,0.16} : QVariantList{0.0,0.0,1.0,0.4};
    const QString preparedId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto prepared = planReviewSourceAnchor(dir.path(), bundle, pdfPath, document.value(QStringLiteral("pdf_sha256")).toString(), 1,
        QRectF(region[0].toDouble(), region[1].toDouble(), region[2].toDouble()-region[0].toDouble(), region[3].toDouble()-region[1].toDouble()), preparedId);
    if (mode == QLatin1String("changed_after_preparation")) QVERIFY(put(sourcePath, source + "Another author edit.\n"));
    const bool created = store.createManualCard(document,1,region,QStringLiteral("What is the answer?"),visual,&card,&error,0,{},&prepared,preparedId);
    if (mode == QLatin1String("wrong_pdf")) { QVERIFY(!created); QCOMPARE(store.cards().size(),0); return; }
    QVERIFY2(created,qPrintable(error));
    const bool linked = mode == QLatin1String("linked") || mode == QLatin1String("context") || mode == QLatin1String("points") || mode == QLatin1String("citation")
        || mode == QLatin1String("stale_source");
    QCOMPARE(card.linkingState(), linked ? QStringLiteral("linked") : pdfOnly ? QStringLiteral("pdf_without_source") : QStringLiteral("linking_failed"));
    QCOMPARE(card.rect(),region);
    QCOMPARE(card.pdfSha256(),sha);
    QVERIFY(!card.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("answer_anchor")).toObject().isEmpty());
    QFile current(sourcePath); QVERIFY(current.open(QIODevice::ReadOnly)); const auto currentBytes = current.readAll();
    if (linked) QVERIFY(currentBytes.contains(card.id().toUtf8()));
    else QCOMPARE(currentBytes, mode == QLatin1String("stale_answer") ? rewritten : mode == QLatin1String("changed_after_preparation") ? source + "Another author edit.\n" : source);
    if (mode == QLatin1String("stale_source")) QVERIFY(currentBytes.startsWith(QByteArray("\\ReviewCardMaskStart{" + card.id().toUtf8() + "}{0}%\n" + answer.toUtf8() + "\n\\ReviewCardMaskEnd{")));
    if (mode == QLatin1String("stale_answer")) QVERIFY2(card.linkingReason().contains(QStringLiteral("could not be mapped safely")), qPrintable(card.linkingReason()));
    if (mode == QLatin1String("ambiguous")) QVERIFY(card.linkingReason().contains(QStringLiteral("historical source line")));
    if (mode == QLatin1String("citation_only")) QVERIFY2(card.linkingReason().contains(QStringLiteral("could not be mapped safely (selected text: [3],)")), qPrintable(card.linkingReason()));
    ReviewStore reopened(dir.path());
    QCOMPARE(reopened.cards().size(),1);
    QCOMPARE(reopened.cards().first().linkingState(),card.linkingState());
    QCOMPARE(reopened.dueCards().size(), linked || pdfOnly ? 1 : 0);
}

void ReviewStoreTest::localDeletionPreservesTextAndHidesPendingRemoval()
{
    // Pending removals finish later: by the retry once indexed, or by hand.
    for (const QString mode : {QStringLiteral("indexed"), QStringLiteral("retry"), QStringLiteral("by_hand")}) {
        const bool indexed = mode == QLatin1String("indexed");
        QTemporaryDir dir;
        const auto readFile = [](const QString &file) { QFile f(file); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        const auto writeFile = [](const QString &file, const QByteArray &bytes) { QFile f(file); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(); };
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
        const QString path = dir.filePath(relative);
        const QByteArray body("Before.\nAnswer with café.\nAfter.\n");
        const auto marked = markReviewSourceLines(relative, body, {2}, id);
        QVERIFY2(marked.error.isEmpty(), qPrintable(marked.error));
        QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile source(path); QVERIFY(source.open(QIODevice::WriteOnly));
        QCOMPARE(source.write(marked.replacement), marked.replacement.size()); source.close();
        auto record = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
        auto primary = record.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("source_anchor"), marked.anchor); record.insert(QStringLiteral("primary"), primary);
        QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), record));
        const auto indexSource = [&] {
            auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(path, {});
            scanned.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
            const QJsonObject files{{relative, scanned}};
            Thoughts::installSourceLocationIndex(dir.path(), {{QStringLiteral("schema_version"), 1},
                {QStringLiteral("files"), files}, {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, files);
        };
        if (indexed) indexSource();
        else Thoughts::invalidateSourceLocationIndex(dir.path());
        ReviewStore store(dir.path()); QString error, eventId;
        QVERIFY2(store.recordRating(id, QStringLiteral("delete"), QDateTime::currentDateTimeUtc(), &error, {}, &eventId), qPrintable(error));
        QVERIFY(source.open(QIODevice::ReadOnly));
        QCOMPARE(source.readAll(), indexed ? body : marked.replacement); source.close();
        const auto eventPath = dir.filePath(QStringLiteral("reviews/events/mac-okular/%1.json").arg(eventId));
        QFile eventFile(eventPath); QVERIFY(eventFile.open(QIODevice::ReadOnly));
        const auto event = QJsonDocument::fromJson(eventFile.readAll()).object();
        QCOMPARE(event.value(QStringLiteral("source_authority")).toString(), QStringLiteral("client-v1"));
        QCOMPARE(event.value(QStringLiteral("marker_removal")).toString(), indexed ? QStringLiteral("complete") : QStringLiteral("pending"));
        if (indexed) {
            const auto change = event.value(QStringLiteral("source_change")).toObject();
            QCOMPARE(change.value(QStringLiteral("after_sha256")).toString(), QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex()));
            QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("reviews/source-objects/") + change.value(QStringLiteral("before_sha256")).toString())));
        } else {
            QVERIFY(store.browserCards().isEmpty());
            QCOMPARE(store.cards().first().linkingState(), QStringLiteral("marker_removal_pending"));
            QVERIFY(!event.value(QStringLiteral("marker_removal_reason")).toString().isEmpty());
            // Not safe while the source location is unverified.
            QCOMPARE(store.retryPendingMarkerRemovals(), 0);
            QCOMPARE(readFile(path), marked.replacement);
            if (mode == QLatin1String("retry")) {
                indexSource();
                QCOMPARE(store.retryPendingMarkerRemovals(), 1);
                QCOMPARE(readFile(path), body);
                QCOMPARE(store.retryPendingMarkerRemovals(), 0);
            } else {
                QVERIFY(writeFile(path, body));
            }
            QVERIFY(store.cards().first().linkingState() != QLatin1String("marker_removal_pending"));
        }
        QVERIFY(QDir(dir.filePath(QStringLiteral("reviews/source-transactions"))).entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty());
    }
}

void ReviewStoreTest::manualRepairKeepsIdentityAndFollowsSource()
{
    QTemporaryDir dir;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto record = reviewCard(id,QStringLiteral("paper"),QString(64,QLatin1Char('a')));
    record.insert(QStringLiteral("source_link"),QJsonObject{{QStringLiteral("state"),QStringLiteral("linking_failed")}});
    QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)),record));
    const QString sourceId = QStringLiteral("docs-test/tex/source/main.tex"), sourcePath = dir.filePath(sourceId);
    const QByteArray source("A complete answer sentence.\n");
    const auto marked = markReviewSourceLines(sourceId,source,{1},id);
    QVERIFY2(marked.error.isEmpty(),qPrintable(marked.error));
    auto put = [&](const QByteArray &bytes) {
        QDir().mkpath(QFileInfo(sourcePath).absolutePath()); QFile f(sourcePath);
        if (!f.open(QIODevice::WriteOnly) || f.write(bytes) != bytes.size()) return false;
        f.close();
        const auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(sourcePath, {});
        auto indexed = scanned; indexed.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
        Thoughts::installSourceLocationIndex(dir.path(), {{QStringLiteral("schema_version"), 1},
            {QStringLiteral("files"), QJsonObject{{sourceId, indexed}}},
            {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, QJsonObject{{sourceId, scanned}});
        return true;
    };
    ReviewStore store(dir.path());
    const auto now = QDateTime::currentDateTimeUtc();
    QVERIFY(store.recordRating(id,QStringLiteral("good"),now));
    auto expected = store.cards().first();
    QString error;
    QVERIFY(put(source));
    QVERIFY(!store.checkManualSourceLink(expected,sourcePath,&error));
    QVERIFY(put(marked.replacement + marked.replacement));
    QVERIFY(!store.checkManualSourceLink(expected,sourcePath,&error));
    QVERIFY(put(marked.replacement));
    QVERIFY2(store.checkManualSourceLink(expected,sourcePath,&error),qPrintable(error));
    QVERIFY(!store.checkManualSourceLink(expected,sourcePath,&error));
    ReviewStore reopened(dir.path());
    QCOMPARE(reopened.cards().size(),1);
    const auto linked = reopened.cards().first();
    QCOMPARE(linked.id(),id);
    QCOMPARE(linked.linkingState(),QStringLiteral("linked"));
    QCOMPARE(linked.schedule.reviewCount,expected.schedule.reviewCount);
    QCOMPARE(linked.question(),expected.question());
    QCOMPARE(linked.rect(),expected.rect());
    QCOMPARE(linked.pdfSha256(),expected.pdfSha256());
    QVERIFY(put(QByteArray("An unrelated added sentence.\n") + marked.replacement));
    QCOMPARE(reopened.activeCards().size(),1);
    QByteArray changed = marked.replacement; changed.replace("complete","changed");
    QVERIFY(put(changed));
    QCOMPARE(reopened.activeCards().size(),0);
    QVERIFY(put(source));
    QVERIFY(!reopened.cards().first().schedule.deleted);
    QCOMPARE(reopened.cards().first().linkingState(), QStringLiteral("unavailable"));
    QVERIFY(put(marked.replacement));
    QVERIFY(!reopened.cards().first().schedule.deleted);
    QCOMPARE(reopened.cards().first().linkingState(), QStringLiteral("linked"));
    const QString movedId = QStringLiteral("docs-test/tex/source/moved.tex");
    const QString movedPath = dir.filePath(movedId);
    QVERIFY(QFile::rename(sourcePath, movedPath));
    auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(movedPath, {});
    scanned.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
    const QJsonObject files{{movedId, scanned}};
    Thoughts::installSourceLocationIndex(dir.path(), {{QStringLiteral("schema_version"), 1}, {QStringLiteral("files"), files},
        {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, files);
    QCOMPARE(reopened.cards().first().linkingSource(), movedId);
    QCOMPARE(reopened.cards().first().id(), id);
    QCOMPARE(reopened.cards().first().schedule.reviewCount, expected.schedule.reviewCount);
    QCOMPARE(reopened.activeCards().size(), 1);
    Thoughts::invalidateSourceLocationIndex(dir.path());
    QVERIFY(!reopened.cards().first().schedule.deleted);
    QCOMPARE(reopened.cards().first().linkingState(), QStringLiteral("unavailable"));
}

void ReviewStoreTest::manualRepairAcceptsMarkdownMarkers()
{
    QTemporaryDir dir;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString relative = QStringLiteral("docs-test/markdown/source/main.md");
    const QString sourcePath = dir.filePath(relative);
    auto record = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    record.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linking_failed")},
        {QStringLiteral("source_file"), relative}});
    QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), record));
    QVERIFY(QDir().mkpath(QFileInfo(sourcePath).absolutePath()));
    const auto put = [&](const QByteArray &bytes) {
        QFile file(sourcePath);
        return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(bytes) == bytes.size();
    };
    ReviewStore store(dir.path());
    const ReviewCard expected = store.cards().first();
    QString error;
    QVERIFY(put("# Example\n\nAn answer.\n"));
    QVERIFY(!store.checkManualSourceLink(expected, sourcePath, &error));
    const QByteArray start = QStringLiteral("<!-- thoughts:v1:start review:%1:answer:0 -->\n").arg(id).toUtf8();
    const QByteArray end = QStringLiteral("<!-- thoughts:v1:end review:%1:answer:0 -->\n").arg(id).toUtf8();
    const QByteArray marked = "# Example\n\n" + start + "An answer.\n" + end;
    QVERIFY(put(marked));
    QVERIFY2(store.checkManualSourceLink(expected, sourcePath, &error), qPrintable(error));
    const auto linked = ReviewStore(dir.path()).cards().first();
    QCOMPARE(linked.id(), id);
    QCOMPARE(linked.linkingState(), QStringLiteral("linked"));
    const auto segment = linked.record.value(QStringLiteral("primary")).toObject()
        .value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().first().toObject();
    QCOMPARE(segment.value(QStringLiteral("source_file")).toString(), relative);
    QCOMPARE(segment.value(QStringLiteral("content_sha256")).toString(),
        QString::fromLatin1(QCryptographicHash::hash("An answer.\n", QCryptographicHash::Sha256).toHex()));
}

void ReviewStoreTest::manualRepairAcceptsNestedLatexEntryPoint()
{
    // A LaTeX entry point may live anywhere in its folder (paper/main.tex);
    // generated folders such as build/ are still not editable sources.
    QTemporaryDir dir;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString relative = QStringLiteral("docs-test/paper/main.tex");
    auto record = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    record.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linking_failed")},
        {QStringLiteral("source_file"), relative}});
    QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), record));
    const QByteArray marked = QStringLiteral("\\ReviewCardMaskStart{%1}{0}%\nAn answer.\n\\ReviewCardMaskEnd{%1}{0}%\n").arg(id).toUtf8();
    for (const QString &path : {relative, QStringLiteral("docs-test/build/main.tex")}) {
        QVERIFY(QDir().mkpath(QFileInfo(dir.filePath(path)).absolutePath()));
        QFile file(dir.filePath(path));
        QVERIFY(file.open(QIODevice::WriteOnly) && file.write(marked) == marked.size());
    }
    ReviewStore store(dir.path());
    const ReviewCard expected = store.cards().first();
    QString error;
    QVERIFY(!store.checkManualSourceLink(expected, dir.filePath(QStringLiteral("docs-test/build/main.tex")), &error));
    QVERIFY2(error.contains(QStringLiteral("editable")), qPrintable(error));
    QVERIFY2(store.checkManualSourceLink(expected, dir.filePath(relative), &error), qPrintable(error));
    const auto linked = ReviewStore(dir.path()).cards().first();
    QCOMPARE(linked.linkingState(), QStringLiteral("linked"));
    const auto segment = linked.record.value(QStringLiteral("primary")).toObject()
        .value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().first().toObject();
    QCOMPARE(segment.value(QStringLiteral("source_file")).toString(), relative);
}

void ReviewStoreTest::skipPostponesAndKeepsTheCardsTags()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject projection{{QStringLiteral("review_card\ncards\n") + id, reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')))}};
    ReviewStore store(root.path(), [&projection] { return projection; }, QDir(root.path()).filePath(QStringLiteral("sync")));
    const QDateTime at(QDate(2026, 10, 5), QTime(18, 0), QTimeZone::UTC);
    QString error;
    QVERIFY2(store.recordRating(id, QStringLiteral("custom_interval"), at, &error, at.addSecs(60), nullptr, {},
                                {QStringLiteral("correct")}), qPrintable(error));
    ReviewSkipBatch batch;
    QVERIFY2(store.skip(id, ReviewSkipScope::Card, at.addDays(4), &batch, &error, at.addSecs(120)), qPrintable(error));
    auto card = store.cards().first();
    QCOMPARE(card.currentTags(), QStringList{QStringLiteral("correct")});
    QCOMPARE(card.schedule.reviewCount, 1);
    QCOMPARE(card.skippedUntil, at.addDays(4));
    QVERIFY(store.dueCards(at.addDays(1)).isEmpty());
    // An older skip recorded as an answer tagged "skip" postpones but keeps the tag too.
    QVERIFY2(store.recordRating(id, QStringLiteral("custom_interval"), at.addSecs(180), &error, at.addDays(5), nullptr,
                                QStringLiteral("skip")), qPrintable(error));
    card = store.cards().first();
    QCOMPARE(card.currentTags(), QStringList{QStringLiteral("correct")});
    QCOMPARE(card.currentMark(), QStringLiteral("correct"));
    QVERIFY(!store.reviewTagUsage().contains(QStringLiteral("skip")));
}

void ReviewStoreTest::failedCardLinksWhenSourceMarkersAppear()
{
    QTemporaryDir dir;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString sourceId = QStringLiteral("docs-test/tex/source/main.tex");
    const QString sourcePath = dir.filePath(sourceId);
    auto record = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    record.insert(QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("linking_failed")},
        {QStringLiteral("source_file"), sourceId}});
    QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), record));
    ReviewStore store(dir.path());
    QVERIFY(store.recordRating(id, QStringLiteral("good"), QDateTime::currentDateTimeUtc()));
    QCOMPARE(store.cards().first().linkingState(), QStringLiteral("linking_failed"));

    const QByteArray answer("A complete answer sentence.\n");
    const auto marked = markReviewSourceLines(sourceId, answer, {1}, id);
    QVERIFY2(marked.error.isEmpty(), qPrintable(marked.error));
    QVERIFY(QDir().mkpath(QFileInfo(sourcePath).absolutePath()));
    QFile source(sourcePath);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(marked.replacement), marked.replacement.size());
    source.close();
    const auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(sourcePath, {});
    auto indexed = scanned;
    indexed.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
    Thoughts::installSourceLocationIndex(dir.path(), {{QStringLiteral("schema_version"), 1},
        {QStringLiteral("files"), QJsonObject{{sourceId, indexed}}},
        {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, QJsonObject{{sourceId, scanned}});

    const auto linked = store.cards().first();
    QCOMPARE(linked.linkingState(), QStringLiteral("linked"));
    QCOMPARE(linked.schedule.reviewCount, 1);
    QCOMPARE(linked.id(), id);
    QCOMPARE(linked.linkingSource(), sourceId);
    QVERIFY(!linked.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject().isEmpty());
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), marked.replacement);
    ReviewStore reopened(dir.path());
    QCOMPARE(reopened.cards().first().linkingState(), QStringLiteral("linked"));
}


// Optional reproduction against an installed PDF/source bundle. All writes go
// to a temporary library; the original PDF, notes and source bundle are read-only.
void ReviewStoreTest::manualDocumentSelectionInIsolatedCopy()
{
    const auto configPath = qEnvironmentVariable("THOUGHTS_REVIEW_REPRO_CONFIG");
    if (configPath.isEmpty()) QSKIP("Set THOUGHTS_REVIEW_REPRO_CONFIG to reproduce an installed document selection.");
    auto read = [](const QString &path) { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
    const auto config = QJsonDocument::fromJson(read(configPath)).object();
    const QString originalPdf = config.value(QStringLiteral("pdf_path")).toString();
    const QString bundle = config.value(QStringLiteral("bundle_root")).toString();
    const QString filename = QFileInfo(originalPdf).fileName();
    const auto pointer = QJsonDocument::fromJson(read(QDir(bundle).filePath(QStringLiteral("documents/%1.json").arg(filename)))).object();
    const auto manifest = QJsonDocument::fromJson(read(QDir(bundle).filePath(QStringLiteral("objects/%1").arg(pointer.value(QStringLiteral("artifact_sha256")).toString())))).object();
    QVERIFY(!manifest.isEmpty());
    QTemporaryDir isolated;
    for (const auto &value : manifest.value(QStringLiteral("sources")).toArray()) {
        const auto relative = value.toObject().value(QStringLiteral("path")).toString();
        QVERIFY(!QDir::isAbsolutePath(relative) && !relative.contains(QStringLiteral("..")));
        const auto target = isolated.filePath(relative);
        QVERIFY(QDir().mkpath(QFileInfo(target).absolutePath()));
        const auto entry = value.toObject();
        const auto published = read(QDir(bundle).filePath(QStringLiteral("objects/%1").arg(entry.value(QStringLiteral("object")).toString())));
        QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(published, QCryptographicHash::Sha256).toHex()), entry.value(QStringLiteral("sha256")).toString());
        QFile source(target); QVERIFY(source.open(QIODevice::WriteOnly)); QCOMPARE(source.write(published), published.size());
    }
    QVERIFY(QDir().mkpath(isolated.filePath(QStringLiteral("current_pdfs"))));
    const auto pdf = isolated.filePath(QStringLiteral("current_pdfs/%1").arg(filename));
    QVERIFY(QFile::copy(originalPdf,pdf));
    const auto sha = pointer.value(QStringLiteral("pdf_sha256")).toString();
    const int page = config.value(QStringLiteral("page")).toInt();
    const QVariantMap document {{QStringLiteral("pdf_path"),pdf},{QStringLiteral("pdf_stem"),QFileInfo(pdf).completeBaseName()},
        {QStringLiteral("pdf_sha256"),sha},{QStringLiteral("source_bundle_root"),bundle},
        {QStringLiteral("page_count"),manifest.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("page_count")).toInt()}};
    const auto visual = fingerprintReviewPage(pdf,page,sha);
    QVERIFY(!visual.contains(QStringLiteral("error")));
    const auto rect = config.value(QStringLiteral("rect")).toArray().toVariantList();
    ReviewStore store(isolated.path()); ReviewCard card; QString error;
    QVERIFY2(store.createManualCard(document,page,rect,QStringLiteral("Reproduction of selected passage"),visual.value(QStringLiteral("page_visual_sha256")).toString(),&card,&error),qPrintable(error));
    QCOMPARE(card.rect(),rect);
    QVERIFY2(card.linkingState() == QStringLiteral("linked"), qPrintable(card.linkingReason()));
    QCOMPARE(store.cards().size(),1);
    const auto segments = card.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray();
    QVERIFY(!segments.isEmpty());
    const QString markedSource = segments.first().toObject().value(QStringLiteral("source_file")).toString();
    if (markedSource.endsWith(QLatin1String(".md")) || markedSource.endsWith(QLatin1String(".markdown"))) {
        const auto marked = read(isolated.filePath(markedSource));
        QVERIFY(marked.contains(QByteArray("<!-- thoughts:v1:start review:" + card.id().toUtf8() + ":answer:0 -->")));
        QVERIFY(!marked.contains(QByteArray("\\ReviewCardMaskStart{" + card.id().toUtf8())));
        QVERIFY(readReviewSourceAnchor(markedSource, marked, card.id()).error.isEmpty());
    }
    qInfo() << "Saved card:" << card.linkingState() << card.linkingReason() << "PDF:" << card.pdfSha256() << "page:" << page;
}

void ReviewStoreTest::queryDatabaseIncludesSharedHistoryAndRetractions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString cardId = QStringLiteral("00000000-0000-4000-8000-000000000091");
    const QString otherId = QStringLiteral("00000000-0000-4000-8000-000000000092");
    const QString first = QStringLiteral("00000000-0000-4000-8000-000000000093");
    const QString second = QStringLiteral("00000000-0000-4000-8000-000000000094");
    const QString retraction = QStringLiteral("00000000-0000-4000-8000-000000000095");
    const QString unrelatedRetraction = QStringLiteral("00000000-0000-4000-8000-000000000096");
    const QDateTime reviewedAt(QDate(2026, 8, 11), QTime(12, 0), QTimeZone::UTC);
    const auto event = [&](const QString &id, const QString &origin, int day) {
        return QJsonObject{{QStringLiteral("event_id"), id}, {QStringLiteral("card_id"), cardId},
            {QStringLiteral("rating"), QStringLiteral("good")}, {QStringLiteral("origin_id"), origin},
            {QStringLiteral("scheduler_version"), 2},
            {QStringLiteral("reviewed_at"), reviewedAt.addDays(day).toString(Qt::ISODateWithMs)}};
    };
    QJsonObject projections;
    projections.insert(QStringLiteral("review_card\ncards\n") + cardId, reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a'))));
    projections.insert(QStringLiteral("review_card\ncards\n") + otherId, reviewCard(otherId, QStringLiteral("other"), QString(64, QLatin1Char('b'))));
    const QString invalidId = QStringLiteral("00000000-0000-4000-8000-000000000097");
    auto invalidCard = reviewCard(invalidId, QStringLiteral("invalid"), QString(64, QLatin1Char('c')));
    auto invalidPrimary = invalidCard.value(QStringLiteral("primary")).toObject();
    invalidPrimary.insert(QStringLiteral("question"), QString());
    invalidCard.insert(QStringLiteral("primary"), invalidPrimary);
    projections.insert(QStringLiteral("review_card\ncards\n") + invalidId, invalidCard);
    projections.insert(QStringLiteral("review_event\nevents\n") + first, event(first, QStringLiteral("mac-okular"), 0));
    projections.insert(QStringLiteral("review_event\nevents\n") + second, event(second, QStringLiteral("android"), 7));
    const QJsonObject groupsRecord{{QStringLiteral("layout_id"), QStringLiteral("groups")}, {QStringLiteral("schema_version"), 1},
        {QStringLiteral("updated_at"), QStringLiteral("2026-10-01T00:00:00.000Z")}, {QStringLiteral("origin_id"), QStringLiteral("android")},
        {QStringLiteral("groups"), QJsonArray{QJsonObject{{QStringLiteral("group_id"), QStringLiteral("g1")}, {QStringLiteral("name"), QStringLiteral("Understanding")},
            {QStringLiteral("tags"), QJsonArray{QStringLiteral("correct")}}}}}};
    projections.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"), groupsRecord);
    projections.insert(QStringLiteral("review_tag_merge\nreview-tags\nmerge-1"), QJsonObject{{QStringLiteral("merge_id"), QStringLiteral("merge-1")},
        {QStringLiteral("from"), QJsonArray{QStringLiteral("Got it")}}, {QStringLiteral("to"), QStringLiteral("correct")},
        {QStringLiteral("created_at"), QStringLiteral("2026-10-01T00:00:00Z")}, {QStringLiteral("origin_id"), QStringLiteral("android")}});
    // Answer feedback, a deleted record included (review-card-feedback.md).
    const QJsonObject feedback = Thoughts::ReviewCardFeedback::create(cardId, QStringLiteral("negative"), QStringLiteral("Wrong sign."),
        Thoughts::ReviewCardFeedback::anchor(QStringLiteral("the KL term"), QString(), QString(), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), QString(64, QLatin1Char('a')),
                                             Thoughts::ReviewCardFeedback::regionsJson({{1, QRectF(0.1, 0.2, 0.3, 0.02)}})),
        QStringLiteral("android"), QDateTime::fromString(QStringLiteral("2026-10-20T10:00:00.000Z"), Qt::ISODateWithMs));
    const QJsonObject deletedFeedback = Thoughts::ReviewCardFeedback::remove(
        Thoughts::ReviewCardFeedback::create(cardId, QStringLiteral("positive"), QString(), feedback.value(QStringLiteral("anchor")).toObject(), QStringLiteral("android")),
        QStringLiteral("android"));
    for (const QJsonObject &record : {feedback, deletedFeedback})
        projections.insert(Thoughts::ReviewCardFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
    // Document feedback (document-feedback.md): no card, the document and PDF version it was marked on.
    const QJsonObject documentFeedback = Thoughts::DocumentFeedback::create(QStringLiteral("positive"), QStringLiteral("Nice picture."),
        Thoughts::ReviewCardFeedback::anchor(QStringLiteral("each route"), QString(), QString(), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), QString(64, QLatin1Char('a')),
                                             Thoughts::ReviewCardFeedback::regionsJson({{3, QRectF(0.1, 0.2, 0.3, 0.02)}})),
        QStringLiteral("android"), QDateTime::fromString(QStringLiteral("2026-10-28T08:52:03.512Z"), Qt::ISODateWithMs));
    const QJsonObject deletedDocumentFeedback = Thoughts::DocumentFeedback::remove(
        Thoughts::DocumentFeedback::create(QStringLiteral("negative"), QString(), documentFeedback.value(QStringLiteral("anchor")).toObject(), QStringLiteral("android")),
        QStringLiteral("android"));
    for (const QJsonObject &record : {documentFeedback, deletedDocumentFeedback})
        projections.insert(Thoughts::DocumentFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
    ReviewStore store(temporary.path(), [&] { return projections; }, {}, true);
    QCOMPARE(store.cardFeedback(cardId), QList<QJsonObject>{feedback});
    QCOMPARE(store.cardFeedback(cardId, true).size(), 2);
    QVERIFY(store.cardFeedback(otherId).isEmpty());
    QCOMPARE(store.documentFeedback(QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848")), QList<QJsonObject>{documentFeedback});
    QCOMPARE(store.documentFeedback(QString(), true).size(), 2);
    QVERIFY(store.documentFeedback(QStringLiteral("0b9c8d7e-6f5a-4b3c-9d2e-1f0a9b8c7d6e")).isEmpty());
    const QString path = temporary.filePath(QStringLiteral("review-query.sqlite"));
    QString error;
    QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("review-query-test"));
    database.setDatabaseName(path);
    QVERIFY(database.open());
    {
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("SELECT c.card_id, c.review_count, count(*) FROM review_cards c JOIN review_events e USING(card_id) WHERE e.rating='good' AND e.is_retracted=0 GROUP BY c.card_id HAVING count(*) >= 2")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), cardId);
        QCOMPARE(query.value(1).toInt(), 2);
        QCOMPARE(query.value(2).toInt(), 2);
        QVERIFY(!query.next());
        QVERIFY(query.exec(QStringLiteral("SELECT origin_id FROM review_events WHERE card_id='00000000-0000-4000-8000-000000000091' ORDER BY reviewed_at DESC LIMIT 1")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("android"));
        QVERIFY(query.exec(QStringLiteral("UPDATE review_cards SET state='deleted', is_deleted=1 WHERE card_id='00000000-0000-4000-8000-000000000091'")));
        QVERIFY(query.exec(QStringLiteral("UPDATE review_meta SET value='1:old-schedule-rule' WHERE key='source_hash'")));
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
        QVERIFY(query.exec(QStringLiteral("SELECT state, is_deleted FROM review_cards WHERE card_id='00000000-0000-4000-8000-000000000091'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("correct"));
        QCOMPARE(query.value(1).toInt(), 0);
        QVERIFY(query.exec(QStringLiteral("SELECT value FROM review_meta WHERE key='source_hash'")));
        QVERIFY(query.next());
        QVERIFY(query.value(0).toString().startsWith(QLatin1String("6:")));
        // Answer feedback, deleted records flagged, the whole record kept.
        QVERIFY(query.exec(QStringLiteral("SELECT feedback_id, card_id, polarity, quote, comment, created_at, deleted, record_json FROM review_feedback ORDER BY deleted")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), feedback.value(QStringLiteral("feedback_id")).toString());
        QCOMPARE(query.value(1).toString(), cardId);
        QCOMPARE(query.value(2).toString(), QStringLiteral("negative"));
        QCOMPARE(query.value(3).toString(), QStringLiteral("the KL term"));
        QCOMPARE(query.value(4).toString(), QStringLiteral("Wrong sign."));
        QCOMPARE(query.value(5).toString(), QStringLiteral("2026-10-20T10:00:00.000Z"));
        QCOMPARE(query.value(6).toInt(), 0);
        QCOMPARE(QJsonDocument::fromJson(query.value(7).toString().toUtf8()).object(), feedback);
        QVERIFY(query.next());
        QCOMPARE(query.value(6).toInt(), 1);
        QVERIFY(!query.next());
        // Document feedback, deleted records flagged, the whole record kept.
        QVERIFY(query.exec(QStringLiteral("SELECT feedback_id, document_id, pdf_sha256, page, polarity, quote, comment, created_at, deleted, record_json FROM document_feedback ORDER BY deleted")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), documentFeedback.value(QStringLiteral("feedback_id")).toString());
        QCOMPARE(query.value(1).toString(), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"));
        QCOMPARE(query.value(2).toString(), QString(64, QLatin1Char('a')));
        QCOMPARE(query.value(3).toInt(), 3);
        QCOMPARE(query.value(4).toString(), QStringLiteral("positive"));
        QCOMPARE(query.value(5).toString(), QStringLiteral("each route"));
        QCOMPARE(query.value(6).toString(), QStringLiteral("Nice picture."));
        QCOMPARE(query.value(7).toString(), QStringLiteral("2026-10-28T08:52:03.512Z"));
        QCOMPARE(query.value(8).toInt(), 0);
        QCOMPARE(QJsonDocument::fromJson(query.value(9).toString().toUtf8()).object(), documentFeedback);
        QVERIFY(query.next());
        QCOMPARE(query.value(8).toInt(), 1);
        QVERIFY(!query.next());
        // Tag merges and the groups record are kept as stored, for thoughtsctl knowledge-profile.
        QVERIFY(query.exec(QStringLiteral("SELECT entity_type, entity_id, record_json FROM review_tag_records ORDER BY entity_type")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("review_tag_groups"));
        QCOMPARE(query.value(1).toString(), QStringLiteral("groups"));
        QCOMPARE(QJsonDocument::fromJson(query.value(2).toString().toUtf8()).object(), groupsRecord);
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), QStringLiteral("review_tag_merge"));
        QCOMPARE(query.value(1).toString(), QStringLiteral("merge-1"));
        QVERIFY(!query.next());
    }
    projections.insert(QStringLiteral("review_event\nevents\n") + retraction,
        QJsonObject{{QStringLiteral("event_id"), retraction}, {QStringLiteral("card_id"), cardId},
            {QStringLiteral("retracted_event_id"), second},
            {QStringLiteral("reviewed_at"), reviewedAt.addDays(8).toString(Qt::ISODateWithMs)}});
    projections.insert(QStringLiteral("review_event\nevents\n") + unrelatedRetraction,
        QJsonObject{{QStringLiteral("event_id"), unrelatedRetraction}, {QStringLiteral("card_id"), otherId},
            {QStringLiteral("retracted_event_id"), first},
            {QStringLiteral("reviewed_at"), reviewedAt.addDays(8).toString(Qt::ISODateWithMs)}});
    QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    {
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("SELECT review_count FROM review_cards WHERE card_id='00000000-0000-4000-8000-000000000091'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        QVERIFY(query.exec(QStringLiteral("SELECT is_retracted FROM review_events WHERE event_id='00000000-0000-4000-8000-000000000094'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 1);
        QVERIFY(query.exec(QStringLiteral("SELECT is_retracted FROM review_events WHERE event_id='00000000-0000-4000-8000-000000000093'")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 0);
        QVERIFY(query.exec(QStringLiteral("SELECT count(*) FROM review_cards")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toInt(), 2);
    }
    database.close();
    database = {};
    QSqlDatabase::removeDatabase(QStringLiteral("review-query-test"));
}

namespace
{
QVariantList rectList(const QRectF &rect)
{
    return {rect.left(), rect.top(), rect.right(), rect.bottom()};
}
QString sha256Hex(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
const QByteArray fourParagraphs("\\documentclass{article}\n\\begin{document}\nAlpha bravo charlie.\n\nDelta echo foxtrot.\n\nGolf hotel india.\n\nJuliet kilo lima.\n\\end{document}\n");
const QList<QList<LinkedDocument::Line>> fourParagraphPages {
    {{QStringLiteral("Alpha bravo charlie."), 3, 100}, {QStringLiteral("Delta echo foxtrot."), 5, 200}},
    {{QStringLiteral("Golf hotel india."), 7, 100}, {QStringLiteral("Juliet kilo lima."), 9, 200}}};
}

void ReviewStoreTest::severalAreasOnTwoPagesMarkOneSegmentEach()
{
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), fourParagraphs, fourParagraphPages));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // The page-2 area comes first: segments and regions still follow the source.
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha,
        {{2, LinkedDocument::band(200)}, {1, LinkedDocument::band(100)}}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.failedArea, -1);
    const auto segments = plan.anchor.value(QStringLiteral("segments")).toArray();
    QCOMPARE(segments.size(), 2);
    QCOMPARE(segments[0].toObject().value(QStringLiteral("segment_id")).toString(), QStringLiteral("0"));
    QCOMPARE(segments[1].toObject().value(QStringLiteral("segment_id")).toString(), QStringLiteral("1"));
    QCOMPARE(plan.regions.size(), 2);
    QCOMPARE(plan.regions[0].toObject().value(QStringLiteral("page")).toInt(), 1);
    QCOMPARE(plan.regions[1].toObject().value(QStringLiteral("page")).toInt(), 2);
    QCOMPARE(plan.pageVisualSha256, fingerprintReviewPage(doc.pdfPath, 1).value(QStringLiteral("page_visual_sha256")).toString());
    const QByteArray start0 = "\\ReviewCardMaskStart{" + id.toUtf8() + "}{0}%\n", end0 = "\\ReviewCardMaskEnd{" + id.toUtf8() + "}{0}%\n";
    const QByteArray start1 = "\\ReviewCardMaskStart{" + id.toUtf8() + "}{1}%\n", end1 = "\\ReviewCardMaskEnd{" + id.toUtf8() + "}{1}%\n";
    QCOMPARE(plan.replacement, QByteArray("\\documentclass{article}\n\\begin{document}\n" + start0 + "Alpha bravo charlie.\n" + end0
        + "\nDelta echo foxtrot.\n\nGolf hotel india.\n\n" + start1 + "Juliet kilo lima.\n" + end1 + "\\end{document}\n"));
    QCOMPARE(stripReviewCardMarkers(doc.sourceId, plan.replacement, id).text, fourParagraphs);

    ReviewStore store(dir.path());
    ReviewCard card;
    QString error;
    const auto visual = fingerprintReviewPage(doc.pdfPath, 2).value(QStringLiteral("page_visual_sha256")).toString();
    QVERIFY2(store.createManualCard(doc.document, 2, rectList(LinkedDocument::band(200)), QStringLiteral("Which words open and close it?"),
        visual, &card, &error, 0, {}, &plan, id), qPrintable(error));
    QCOMPARE(card.linkingState(), QStringLiteral("linked"));
    QCOMPARE(card.record.value(QStringLiteral("schema_version")).toInt(), 4);
    const auto primary = card.record.value(QStringLiteral("primary")).toObject();
    QCOMPARE(primary.value(QStringLiteral("page")).toInt(), 1);
    QCOMPARE(primary.value(QStringLiteral("regions")).toArray(), plan.regions);
    QCOMPARE(primary.value(QStringLiteral("page_visual_sha256")).toString(), plan.pageVisualSha256);
    QCOMPARE(primary.value(QStringLiteral("source_anchor")).toObject(), plan.anchor);
    QCOMPARE(card.pagesLabel(), QStringLiteral("1, 2"));
    QCOMPARE(doc.current(), plan.replacement);
    const auto change = card.record.value(QStringLiteral("source_change")).toObject();
    QCOMPARE(change.value(QStringLiteral("transaction_id")).toString(), id);
    QCOMPARE(change.value(QStringLiteral("before_sha256")).toString(), sha256Hex(fourParagraphs));
    // The two passages read back from the source as the card's anchor.
    QCOMPARE(readReviewSourceAnchor(doc.sourceId, doc.current(), id).anchor.value(QStringLiteral("segments")).toArray(), segments);
    ReviewStore reopened(dir.path());
    QCOMPARE(reopened.cards().size(), 1);
    QCOMPARE(reopened.cards().first().regions().size(), 2);
}

void ReviewStoreTest::rectangleAroundFloatMarksTheWholeEnvironment()
{
    QTemporaryDir dir;
    const QByteArray source("\\documentclass{article}\n\\begin{document}\nAlpha bravo charlie.\n\n\\begin{table}\n\\centering\n"
                            "\\input{generated-table}\n\\caption{Delta echo foxtrot.}\n\\end{table}\n\nGolf hotel india.\n\\end{document}\n");
    LinkedDocument doc;
    // The table body comes from a generated file and maps to no authored line.
    QVERIFY(doc.build(dir.path(), source, {{{QStringLiteral("Alpha bravo charlie."), 3, 100}, {QStringLiteral("Kilo lima mike"), 0, 200},
        {QStringLiteral("Delta echo foxtrot."), 8, 230}, {QStringLiteral("Golf hotel india."), 11, 320}}}));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QRectF table(QPointF(0.0, 180 / 842.0), QPointF(1.0, 240 / 842.0));
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, table}}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    const auto segments = plan.anchor.value(QStringLiteral("segments")).toArray();
    QCOMPARE(segments.size(), 1);
    const QByteArray start = "\\ReviewCardMaskStart{" + id.toUtf8() + "}{0}%\n", end = "\\ReviewCardMaskEnd{" + id.toUtf8() + "}{0}%\n";
    QCOMPARE(plan.replacement, QByteArray("\\documentclass{article}\n\\begin{document}\nAlpha bravo charlie.\n\n" + start
        + "\\begin{table}\n\\centering\n\\input{generated-table}\n\\caption{Delta echo foxtrot.}\n\\end{table}\n" + end
        + "\nGolf hotel india.\n\\end{document}\n"));
    // A rectangle around the caption alone also stands for the whole table.
    const auto caption = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, LinkedDocument::band(230)}}, id);
    QVERIFY2(caption.error.isEmpty(), qPrintable(caption.error));
    QCOMPARE(caption.replacement, plan.replacement);
    // A text selection is the text itself: the generated words cannot be linked.
    const auto selected = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, table, true}}, id);
    QVERIFY(!selected.error.isEmpty());
    QVERIFY2(selected.error.contains(QStringLiteral("Kilo")), qPrintable(selected.error));
    QCOMPARE(selected.failedArea, 0);
    // Outside a float the unmapped words are an error for a rectangle too.
    const QByteArray plain("\\documentclass{article}\n\\begin{document}\nAlpha bravo charlie.\n\n\\input{generated-table}\n\nDelta echo foxtrot.\n\\end{document}\n");
    QTemporaryDir other;
    LinkedDocument plainDoc;
    QVERIFY(plainDoc.build(other.path(), plain, {{{QStringLiteral("Alpha bravo charlie."), 3, 100}, {QStringLiteral("Kilo lima mike"), 0, 200},
        {QStringLiteral("Delta echo foxtrot."), 7, 230}}}));
    const auto noFloat = planReviewSourceAnchors(other.path(), plainDoc.bundle, plainDoc.pdfPath, plainDoc.pdfSha, {{1, table}}, id);
    QVERIFY2(noFloat.error.contains(QStringLiteral("Kilo")), qPrintable(noFloat.error));
}

void ReviewStoreTest::unmappableAreaIsRefusedAndNothingIsWritten()
{
    QTemporaryDir dir;
    LinkedDocument doc;
    // Page 2 also has a drawing without text, like a TikZ picture.
    const QRectF drawing(100, 400, 300, 120);
    QVERIFY(doc.build(dir.path(), fourParagraphs, fourParagraphPages, {{2, drawing}}));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QRectF drawingArea(QPointF(90 / 595.0, 390 / 842.0), QPointF(410 / 595.0, 530 / 842.0));
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha,
        {{1, LinkedDocument::band(100)}, {2, drawingArea}, {2, LinkedDocument::band(200)}}, id);
    QCOMPARE(plan.error, QStringLiteral("The selected region has no verifiable source-linked text."));
    QCOMPARE(plan.failedArea, 1);
    QVERIFY(!plan.invalidPdf);
    QVERIFY(!plan.withoutSource);
    QVERIFY(plan.replacement.isEmpty());
    QCOMPARE(doc.current(), fourParagraphs);
    // An area cutting through a line of the passage is refused for that area.
    const auto partial = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha,
        {{1, LinkedDocument::band(100)}, {2, LinkedDocument::band(200, 0.0, 0.08)}}, id);
    QVERIFY2(partial.failedArea == 1, qPrintable(partial.error));
    QVERIFY(!partial.error.isEmpty());
    QCOMPARE(ReviewStore(dir.path()).cards().size(), 0);
}

void ReviewStoreTest::placingAgainMovesTheCardsMarkers()
{
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), fourParagraphs, fourParagraphPages));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto first = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, LinkedDocument::band(100)}}, id);
    QVERIFY2(first.error.isEmpty(), qPrintable(first.error));
    ReviewStore store(dir.path());
    ReviewCard card;
    QString error;
    QVERIFY2(store.createManualCard(doc.document, 1, rectList(LinkedDocument::band(100)), QStringLiteral("Which words?"),
        first.pageVisualSha256, &card, &error, 0, {}, &first, id), qPrintable(error));
    const QByteArray before = doc.current();
    QCOMPARE(before, first.replacement);
    card = store.cards().first();

    // The PDF was built before the markers existed; the source without them still matches it.
    const auto moved = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha,
        {{1, LinkedDocument::band(200)}, {2, LinkedDocument::band(200)}}, id, true);
    QVERIFY2(moved.error.isEmpty(), qPrintable(moved.error));
    QCOMPARE(moved.original, before);
    QCOMPARE(moved.anchor.value(QStringLiteral("segments")).toArray().size(), 2);
    // Without replacing, the card's own markers are in the way.
    QVERIFY(!planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, LinkedDocument::band(200)}}, id).error.isEmpty());

    QVERIFY2(store.replaceCardAreas(card, doc.pdfSha, QStringLiteral("Which words now?"), moved, &error), qPrintable(error));
    const QByteArray after = doc.current();
    QCOMPARE(after, moved.replacement);
    QVERIFY(!after.contains(QByteArray("Start{" + id.toUtf8() + "}{0}%\nAlpha")));
    // The server's rule: the source without the card's old markers equals the source without its new ones.
    QCOMPARE(stripReviewCardMarkers(doc.sourceId, before, id).text, fourParagraphs);
    QCOMPARE(stripReviewCardMarkers(doc.sourceId, after, id).text, fourParagraphs);
    const auto updated = store.cards().first();
    QCOMPARE(updated.question(), QStringLiteral("Which words now?"));
    QCOMPARE(updated.record.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(updated.record.value(QStringLiteral("schema_version")).toInt(), 4);
    QCOMPARE(updated.linkingState(), QStringLiteral("linked"));
    const auto primary = updated.record.value(QStringLiteral("primary")).toObject();
    QCOMPARE(primary.value(QStringLiteral("regions")).toArray(), moved.regions);
    QCOMPARE(primary.value(QStringLiteral("page")).toInt(), 1);
    QCOMPARE(primary.value(QStringLiteral("source_anchor")).toObject(), moved.anchor);
    QVERIFY(!primary.contains(QStringLiteral("answer_region_override")));
    const auto change = updated.record.value(QStringLiteral("source_change")).toObject();
    const QString transaction = change.value(QStringLiteral("transaction_id")).toString();
    QVERIFY(!QUuid(transaction).isNull());
    QVERIFY(transaction != id);
    QCOMPARE(change.value(QStringLiteral("before_sha256")).toString(), sha256Hex(before));
    QCOMPARE(change.value(QStringLiteral("after_sha256")).toString(), sha256Hex(after));
    QCOMPARE(change.value(QStringLiteral("path")).toString(), doc.sourceId);
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("reviews/source-objects/") + sha256Hex(before))));
    QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("reviews/source-objects/") + sha256Hex(after))));
    // An outdated copy of the card is refused.
    QVERIFY(!store.replaceCardAreas(card, doc.pdfSha, QStringLiteral("Again?"), moved, &error));
    QCOMPARE(doc.current(), after);

    // A PDF built after the markers were placed: the lookup sees the marked source.
    QTemporaryDir rebuilt;
    LinkedDocument marked;
    const QList<QList<LinkedDocument::Line>> shifted {
        {{QStringLiteral("Alpha bravo charlie."), 4, 100}, {QStringLiteral("Delta echo foxtrot."), 7, 200}},
        {{QStringLiteral("Golf hotel india."), 9, 100}, {QStringLiteral("Juliet kilo lima."), 11, 200}}};
    QVERIFY(marked.build(rebuilt.path(), first.replacement, shifted));
    auto record = card.record;
    record.insert(QStringLiteral("revision"), 1);
    auto markedPrimary = record.value(QStringLiteral("primary")).toObject();
    markedPrimary.insert(QStringLiteral("pdf_sha256"), marked.pdfSha);
    record.insert(QStringLiteral("primary"), markedPrimary);
    QVERIFY(writeObject(rebuilt.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), record));
    const auto again = planReviewSourceAnchors(rebuilt.path(), marked.bundle, marked.pdfPath, marked.pdfSha, {{2, LinkedDocument::band(200)}}, id, true);
    QVERIFY2(again.error.isEmpty(), qPrintable(again.error));
    const QByteArray start = "\\ReviewCardMaskStart{" + id.toUtf8() + "}{0}%\n", end = "\\ReviewCardMaskEnd{" + id.toUtf8() + "}{0}%\n";
    QCOMPARE(again.replacement, QByteArray("\\documentclass{article}\n\\begin{document}\nAlpha bravo charlie.\n\nDelta echo foxtrot.\n\nGolf hotel india.\n\n"
        + start + "Juliet kilo lima.\n" + end + "\\end{document}\n"));
    ReviewStore rebuiltStore(rebuilt.path());
    QVERIFY2(rebuiltStore.replaceCardAreas(rebuiltStore.cards().first(), marked.pdfSha, QStringLiteral("Which words?"), again, &error), qPrintable(error));
    QCOMPARE(marked.current(), again.replacement);
}

void ReviewStoreTest::deletingACardWithSeveralPassagesRemovesAllMarkers()
{
    QTemporaryDir dir;
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    const QString path = dir.filePath(relative);
    const QByteArray body("First.\n\nAnswer one.\n\nMiddle.\n\nAnswer two.\n\nLast.\n");
    const auto marked = markReviewSourceRanges(relative, body, {{3, 3}, {7, 7}}, id);
    QVERIFY2(marked.error.isEmpty(), qPrintable(marked.error));
    QCOMPARE(marked.anchor.value(QStringLiteral("segments")).toArray().size(), 2);
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile source(path);
    QVERIFY(source.open(QIODevice::WriteOnly));
    QCOMPARE(source.write(marked.replacement), marked.replacement.size());
    source.close();
    auto record = reviewCard(id, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    auto primary = record.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("source_anchor"), marked.anchor);
    record.insert(QStringLiteral("primary"), primary);
    QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/cards/%1.json").arg(id)), record));
    auto scanned = Okular::DocsCommentSynchronizer::sourceCommentFileIndex(path, {});
    scanned.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
    const QJsonObject files{{relative, scanned}};
    Thoughts::installSourceLocationIndex(dir.path(), {{QStringLiteral("schema_version"), 1},
        {QStringLiteral("files"), files}, {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}}, files);
    ReviewStore store(dir.path());
    QString error, eventId;
    QVERIFY2(store.recordRating(id, QStringLiteral("delete"), QDateTime::currentDateTimeUtc(), &error, {}, &eventId), qPrintable(error));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QCOMPARE(source.readAll(), body);
    QFile eventFile(dir.filePath(QStringLiteral("reviews/events/mac-okular/%1.json").arg(eventId)));
    QVERIFY(eventFile.open(QIODevice::ReadOnly));
    const auto event = QJsonDocument::fromJson(eventFile.readAll()).object();
    QCOMPARE(event.value(QStringLiteral("marker_removal")).toString(), QStringLiteral("complete"));
    QCOMPARE(event.value(QStringLiteral("source_change")).toObject().value(QStringLiteral("after_sha256")).toString(), sha256Hex(body));
}

namespace {
// Cards whose passages are marked in one source, as Sync leaves them: server records of one PDF,
// read through a projection, and a local queue for what this device writes.
struct MergeFixture {
    QString root, relative, path;
    QByteArray source;
    QJsonObject projection;
    bool start(const QString &directory, const QString &file, const QByteArray &text)
    {
        root = directory;
        relative = file;
        path = QDir(root).filePath(relative);
        source = text;
        return QDir().mkpath(QFileInfo(path).absolutePath()) && save();
    }
    bool save() const
    {
        QFile out(path);
        return out.open(QIODevice::WriteOnly) && out.write(source) == source.size();
    }
    QByteArray current() const
    {
        QFile in(path);
        return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
    }
    // Marks @p ranges (lines of the source as it is now) as the passages of @p id.
    bool mark(const QString &id, const QList<QPair<int, int>> &ranges)
    {
        const auto plan = markReviewSourceRanges(relative, source, ranges, id);
        if (!plan.error.isEmpty()) qWarning() << plan.error;
        if (!plan.error.isEmpty()) return false;
        source = plan.replacement;
        return save();
    }
    // The card's record, its anchor read from its markers in the source as it is now.
    void card(const QString &id, const QString &question, const QJsonArray &regions, const QString &pageVisual, const QString &answer,
              const QString &stem = QStringLiteral("paper"), const QString &pdfSha = QString(64, QLatin1Char('a')))
    {
        auto record = reviewCard(id, stem, pdfSha);
        record.insert(QStringLiteral("schema_version"), 4);
        auto primary = record.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("question"), question);
        primary.insert(QStringLiteral("regions"), regions);
        primary.insert(QStringLiteral("page"), regions.first().toObject().value(QStringLiteral("page")));
        primary.insert(QStringLiteral("rect"), regions.first().toObject().value(QStringLiteral("rect")));
        primary.insert(QStringLiteral("page_visual_sha256"), pageVisual);
        primary.insert(QStringLiteral("answer_anchor"), QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("normalized_text"), answer},
            {QStringLiteral("text_sha256"), QString::fromLatin1(QCryptographicHash::hash(answer.toUtf8(), QCryptographicHash::Sha256).toHex())}});
        primary.insert(QStringLiteral("source_anchor"), readReviewSourceAnchor(relative, source, id).anchor);
        record.insert(QStringLiteral("primary"), primary);
        projection.insert(QStringLiteral("review_card\ncards\n") + id, record);
    }
    std::unique_ptr<ReviewStore> store()
    {
        return std::make_unique<ReviewStore>(root, [this] { return projection; }, QDir(root).filePath(QStringLiteral("sync")));
    }
    QList<QJsonObject> queued() const
    {
        return DesktopSyncOutbox(QDir(root).filePath(QStringLiteral("sync/desktop-sync.sqlite"))).operations();
    }
};

QJsonArray mergeRegions(std::initializer_list<double> tops)
{
    QJsonArray regions;
    for (double top : tops) regions.append(QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QJsonArray{0.1, top, 0.9, top + 0.04}}});
    return regions;
}

ReviewCard cardWithId(const QList<ReviewCard> &cards, const QString &id)
{
    for (const auto &card : cards) if (card.id() == id) return card;
    return {};
}

// A note shaped like a real one: the first passage ends inside a list, a figure and a paragraph
// lie between the passages, and the second holds display math.
const QByteArray mergeNote(
    "\\documentclass{article}\n"   // 1
    "\\begin{document}\n"          // 2
    "\\section{Method}\n"          // 3
    "Intro sentence.\n"            // 4
    "First passage opens here.\n"  // 5
    "\n"                           // 6
    "A list follows:\n"            // 7
    "\\begin{itemize}\n"           // 8
    "    \\item One step.\n"       // 9
    "    \\item Another step.\n"   // 10
    "\\end{itemize}\n"             // 11
    "\n"                           // 12
    "\\begin{figure}[htbp]\n"      // 13
    "\\centering\n"                // 14
    "\\caption{A figure between the passages.}\n" // 15
    "\\end{figure}\n"              // 16
    "\n"                           // 17
    "A paragraph between the passages.\n" // 18
    "\n"                           // 19
    "The signal is\n"              // 20
    "\\[\n"                        // 21
    "x = y\n"                      // 22
    "\\]\n"                        // 23
    "and the rest of the second passage.\n" // 24
    "\n"                           // 25
    "Closing paragraph.\n"         // 26
    "\\end{document}\n");          // 27
}

void ReviewStoreTest::mergingCardsGivesTheKeptCardEveryPassage()
{
    QTemporaryDir dir;
    MergeFixture doc;
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    QVERIFY(doc.start(dir.path(), relative, mergeNote));
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // The later passage first, so the earlier lines keep their numbers.
    QVERIFY(doc.mark(second, {{20, 24}}));
    const QByteArray secondOnly = doc.source;
    QVERIFY(doc.mark(first, {{5, 10}}));
    const QByteArray original = doc.source;
    doc.card(first, QStringLiteral("What opens the method?"), mergeRegions({0.10}), QString(64, QLatin1Char('c')), QStringLiteral("First passage"));
    doc.card(second, QStringLiteral("What is the signal?"), mergeRegions({0.50, 0.55}), QString(64, QLatin1Char('d')), QStringLiteral("The signal"));
    auto store = doc.store();
    QString error;
    // The second card was reviewed twice, the first never: the second keeps its schedule by default.
    const QDateTime at = QDateTime::currentDateTimeUtc().addSecs(-3600);
    QVERIFY2(store->recordRating(second, QStringLiteral("good"), at, &error), qPrintable(error));
    QVERIFY2(store->recordRating(second, QStringLiteral("good"), at.addSecs(600), &error), qPrintable(error));
    const auto before = store->cards();
    const ReviewCard firstCard = cardWithId(before, first), secondCard = cardWithId(before, second);
    QCOMPARE(secondCard.schedule.reviewCount, 2);
    QCOMPARE(ReviewStore::defaultMergeKeptCard({firstCard, secondCard}), second);
    QCOMPARE(ReviewStore::joinedMergeQuestion({firstCard, secondCard}), QStringLiteral("What opens the method? What is the signal?"));
    ReviewStore::CardMerge preview;
    QVERIFY2(store->cardMergeProblem({secondCard, firstCard}, {}, &preview).isEmpty(), qPrintable(store->cardMergeProblem({secondCard, firstCard}, {})));
    QCOMPARE(preview.order, (QStringList{first, second}));
    QCOMPARE(preview.keptId, second);
    QCOMPARE(preview.passages, 2);
    QCOMPARE(preview.sourceFile, relative);
    // Checking writes nothing.
    QCOMPARE(doc.current(), original);
    const int queuedBefore = doc.queued().size();

    ReviewStore::CardMerge merged;
    QVERIFY2(store->mergeCards({secondCard, firstCard}, second, QStringLiteral("What opens the method, and what is the signal?"), {}, &error, &merged),
             qPrintable(error));
    QCOMPARE(merged.keptId, second);
    QCOMPARE(merged.deletedIds, QStringList{first});
    QCOMPARE(merged.passages, 2);

    // The kept card marks both passages; the figure and the paragraph between them stay out.
    const QByteArray after = doc.current();
    QVERIFY(!after.contains(first.toUtf8()));
    QCOMPARE(after, markReviewSourceRanges(relative, mergeNote, {{5, 10}, {20, 24}}, second).replacement);
    const QByteArray start0 = "\\ReviewCardMaskStart{" + second.toUtf8() + "}{0}%\n";
    const QByteArray end0 = "\\ReviewCardMaskEnd{" + second.toUtf8() + "}{0}%\n";
    const QByteArray start1 = "\\ReviewCardMaskStart{" + second.toUtf8() + "}{1}%\n";
    QVERIFY(after.contains(QByteArray(start0 + "First passage opens here.\n")));
    QVERIFY(after.contains(QByteArray("    \\item Another step.\n" + end0 + "\\end{itemize}\n")));
    const QByteArray between = after.mid(after.indexOf(end0), after.indexOf(start1) - after.indexOf(end0));
    QVERIFY(between.contains("\\begin{figure}"));
    QVERIFY(between.contains("A paragraph between the passages."));
    QVERIFY(after.contains(QByteArray(start1 + "The signal is\n")));
    QCOMPARE(sha256Hex(after), merged.sourceSha256);

    // One batch: the other card's deletion, then the kept card's new areas.
    const auto queued = doc.queued();
    QCOMPARE(queued.size(), queuedBefore + 2);
    const auto deletion = queued.at(queuedBefore), update = queued.at(queuedBefore + 1);
    QCOMPARE(deletion.value(QStringLiteral("entity_type")).toString(), QStringLiteral("review_event"));
    QCOMPARE(update.value(QStringLiteral("entity_type")).toString(), QStringLiteral("review_card"));
    QCOMPARE(update.value(QStringLiteral("entity_id")).toString(), second);
    QCOMPARE(update.value(QStringLiteral("device_sequence")).toInteger(), deletion.value(QStringLiteral("device_sequence")).toInteger() + 1);
    const auto event = deletion.value(QStringLiteral("payload")).toObject();
    QCOMPARE(event.value(QStringLiteral("card_id")).toString(), first);
    QCOMPARE(event.value(QStringLiteral("rating")).toString(), QStringLiteral("delete"));
    QCOMPARE(event.value(QStringLiteral("merged_into")).toString(), second);
    QCOMPARE(event.value(QStringLiteral("marker_removal")).toString(), QStringLiteral("complete"));
    QCOMPARE(event.value(QStringLiteral("source_authority")).toString(), QStringLiteral("client-v1"));
    QCOMPARE(event.value(QStringLiteral("source_anchor")).toObject(), firstCard.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject());
    QCOMPARE(merged.deleteEventIds, QStringList{event.value(QStringLiteral("event_id")).toString()});
    // The chain the server applies: the other card's markers go before the kept card's new ones
    // appear, so no version marks a passage twice.
    const auto removal = event.value(QStringLiteral("source_change")).toObject();
    const auto card = update.value(QStringLiteral("payload")).toObject();
    const auto placement = card.value(QStringLiteral("source_change")).toObject();
    QCOMPARE(removal.value(QStringLiteral("transaction_id")).toString(), event.value(QStringLiteral("event_id")).toString());
    QCOMPARE(removal.value(QStringLiteral("path")).toString(), relative);
    QCOMPARE(removal.value(QStringLiteral("before_sha256")).toString(), sha256Hex(original));
    QCOMPARE(removal.value(QStringLiteral("after_sha256")).toString(), sha256Hex(secondOnly));
    QCOMPARE(placement.value(QStringLiteral("before_sha256")).toString(), sha256Hex(secondOnly));
    QCOMPARE(placement.value(QStringLiteral("after_sha256")).toString(), sha256Hex(after));
    QCOMPARE(removal.value(QStringLiteral("base_sha256")).toString(), sha256Hex(original));
    QCOMPARE(placement.value(QStringLiteral("base_sha256")).toString(), sha256Hex(original));
    QVERIFY(placement.value(QStringLiteral("transaction_id")).toString() != second);
    QVERIFY(!QUuid(placement.value(QStringLiteral("transaction_id")).toString()).isNull());
    for (const QByteArray &version : {original, secondOnly, after})
        QVERIFY(QFileInfo::exists(dir.filePath(QStringLiteral("reviews/source-objects/") + sha256Hex(version))));
    // The server's rules: a deletion removes only its card's markers; the kept card's change only moves its own.
    QCOMPARE(removeReviewCardMarkers(relative, original, first, {QStringLiteral("0")}, &error), secondOnly);
    QCOMPARE(stripReviewCardMarkers(relative, secondOnly, second).text, stripReviewCardMarkers(relative, after, second).text);
    QVERIFY(QDir(dir.filePath(QStringLiteral("reviews/source-transactions"))).entryList({QStringLiteral("*.json")}, QDir::Files).isEmpty());

    // The kept card keeps its ID, schedule and history and takes both cards' areas, in reading order.
    const auto cards = store->cards();
    QVERIFY(cardWithId(cards, first).schedule.deleted);
    const ReviewCard kept = cardWithId(cards, second);
    QVERIFY(!kept.schedule.deleted);
    QCOMPARE(kept.schedule.reviewCount, 2);
    QCOMPARE(kept.schedule.dueAt, secondCard.schedule.dueAt);
    QCOMPARE(kept.question(), QStringLiteral("What opens the method, and what is the signal?"));
    QCOMPARE(kept.record.value(QStringLiteral("revision")).toInt(), 2);
    const auto primary = kept.record.value(QStringLiteral("primary")).toObject();
    QJsonArray expectedRegions = mergeRegions({0.10});
    for (const auto &region : mergeRegions({0.50, 0.55})) expectedRegions.append(region);
    QCOMPARE(primary.value(QStringLiteral("regions")).toArray(), expectedRegions);
    QCOMPARE(primary.value(QStringLiteral("page_visual_sha256")).toString(), QString(64, QLatin1Char('c')));
    QCOMPARE(primary.value(QStringLiteral("rect")).toArray(), expectedRegions.first().toObject().value(QStringLiteral("rect")).toArray());
    QCOMPARE(primary.value(QStringLiteral("pdf_sha256")).toString(), QString(64, QLatin1Char('a')));
    QCOMPARE(primary.value(QStringLiteral("answer_anchor")).toObject().value(QStringLiteral("normalized_text")).toString(), QStringLiteral("First passage The signal"));
    QCOMPARE(primary.value(QStringLiteral("source_anchor")).toObject(), readReviewSourceAnchor(relative, after, second).anchor);
    QCOMPARE(primary.value(QStringLiteral("source_anchor")).toObject().value(QStringLiteral("segments")).toArray().size(), 2);
    QCOMPARE(kept.linkingState(), QStringLiteral("linked"));

    // Merging the old copies again is refused and changes nothing.
    QVERIFY(!store->mergeCards({secondCard, firstCard}, second, QStringLiteral("Again?"), {}, &error));
    QCOMPARE(doc.current(), after);
    QCOMPARE(doc.queued().size(), queuedBefore + 2);
}

void ReviewStoreTest::mergingCardsFollowsReadingOrderAndTheChosenCard()
{
    QTemporaryDir dir;
    MergeFixture doc;
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    QVERIFY(doc.start(dir.path(), relative, mergeNote));
    // The first card has two passages around the second card's one.
    const QString outer = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString inner = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString last = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(doc.mark(last, {{26, 26}}));
    QVERIFY(doc.mark(inner, {{20, 24}}));
    QVERIFY(doc.mark(outer, {{5, 5}, {7, 10}}));
    doc.card(outer, QStringLiteral("Q outer?"), mergeRegions({0.10, 0.20}), QString(64, QLatin1Char('c')), QStringLiteral("outer"));
    doc.card(inner, QStringLiteral("Q inner?"), mergeRegions({0.50}), QString(64, QLatin1Char('d')), QStringLiteral("inner"));
    doc.card(last, QStringLiteral("Q last?"), mergeRegions({0.80}), QString(64, QLatin1Char('e')), QStringLiteral("last"));
    auto store = doc.store();
    QString error;
    QVERIFY2(store->recordRating(inner, QStringLiteral("good"), QDateTime::currentDateTimeUtc().addSecs(-60), &error), qPrintable(error));
    const auto cards = store->cards();
    const QList<ReviewCard> chosen{cardWithId(cards, last), cardWithId(cards, inner), cardWithId(cards, outer)};
    ReviewStore::CardMerge preview;
    QVERIFY2(store->cardMergeProblem(chosen, {}, &preview).isEmpty(), qPrintable(store->cardMergeProblem(chosen, {})));
    QCOMPARE(preview.order, (QStringList{outer, inner, last}));
    QCOMPARE(preview.keptId, inner);
    // The user keeps the card without reviews instead; the others go in reading order.
    ReviewStore::CardMerge merged;
    QVERIFY2(store->mergeCards(chosen, last, QStringLiteral("All of it?"), {}, &error, &merged), qPrintable(error));
    QCOMPARE(merged.deletedIds, (QStringList{outer, inner}));
    QCOMPARE(merged.passages, 4);
    QCOMPARE(doc.current(), markReviewSourceRanges(relative, mergeNote, {{5, 5}, {7, 10}, {20, 24}, {26, 26}}, last).replacement);
    const auto queued = doc.queued();
    QCOMPARE(queued.at(queued.size() - 3).value(QStringLiteral("payload")).toObject().value(QStringLiteral("card_id")).toString(), outer);
    QCOMPARE(queued.at(queued.size() - 2).value(QStringLiteral("payload")).toObject().value(QStringLiteral("card_id")).toString(), inner);
    QCOMPARE(queued.last().value(QStringLiteral("entity_id")).toString(), last);
    // Each deletion's change follows the one before it.
    const auto change = [&](int fromEnd) { return queued.at(queued.size() - fromEnd).value(QStringLiteral("payload")).toObject().value(QStringLiteral("source_change")).toObject(); };
    QCOMPARE(change(3).value(QStringLiteral("after_sha256")), change(2).value(QStringLiteral("before_sha256")));
    QCOMPARE(change(2).value(QStringLiteral("after_sha256")), change(1).value(QStringLiteral("before_sha256")));
    const auto after = store->cards();
    QVERIFY(cardWithId(after, outer).schedule.deleted);
    QVERIFY(cardWithId(after, inner).schedule.deleted);
    const auto kept = cardWithId(after, last);
    QCOMPARE(kept.schedule.reviewCount, 0);
    // Each card's areas, by its first passage.
    QJsonArray regions = mergeRegions({0.10, 0.20});
    regions.append(mergeRegions({0.50}).first());
    regions.append(mergeRegions({0.80}).first());
    QCOMPARE(kept.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("regions")).toArray(), regions);
    QCOMPARE(kept.pageVisualSha256(), QString(64, QLatin1Char('c')));
}

void ReviewStoreTest::mergingMarkdownCardsRelabelsTheirMarkers()
{
    QTemporaryDir dir;
    MergeFixture doc;
    const QString relative = QStringLiteral("docs-test/notes.md");
    const QByteArray note("# Title\n\nFirst answer paragraph.\n\n![A figure](figure.png)\n\nSecond answer paragraph.\n\nEnd.\n");
    QVERIFY(doc.start(dir.path(), relative, note));
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(doc.mark(second, {{7, 7}}));
    QVERIFY(doc.mark(first, {{3, 3}}));
    doc.card(first, QStringLiteral("First?"), mergeRegions({0.1}), QString(64, QLatin1Char('c')), QStringLiteral("First"));
    doc.card(second, QStringLiteral("Second?"), mergeRegions({0.5}), QString(64, QLatin1Char('d')), QStringLiteral("Second"));
    auto store = doc.store();
    const auto cards = store->cards();
    QString error;
    QVERIFY2(store->mergeCards({cardWithId(cards, first), cardWithId(cards, second)}, first, QStringLiteral("Both?"), {}, &error), qPrintable(error));
    const QByteArray after = doc.current();
    QCOMPARE(after, markReviewSourceRanges(relative, note, {{3, 3}, {7, 7}}, first).replacement);
    QVERIFY(after.contains(QByteArray("<!-- thoughts:v1:end review:" + first.toUtf8() + ":answer:0 -->\n\n![A figure](figure.png)\n\n<!-- thoughts:v1:start review:"
                                      + first.toUtf8() + ":answer:1 -->\nSecond answer paragraph.\n")));
    QVERIFY(!after.contains(second.toUtf8()));
    QVERIFY(cardWithId(store->cards(), second).schedule.deleted);
}

void ReviewStoreTest::mergingCardsRefusesWhatItCannotMergeSafely()
{
    QTemporaryDir dir;
    MergeFixture doc;
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    QVERIFY(doc.start(dir.path(), relative, mergeNote));
    const auto id = [] { return QUuid::createUuid().toString(QUuid::WithoutBraces); };
    const QString first = id(), second = id(), otherDocument = id(), otherFile = id(), pdfOnly = id(), otherVersion = id();
    QVERIFY(doc.mark(otherVersion, {{26, 26}}));
    QVERIFY(doc.mark(second, {{20, 24}}));
    QVERIFY(doc.mark(first, {{5, 10}}));
    QVERIFY(doc.mark(otherDocument, {{4, 4}}));
    doc.card(first, QStringLiteral("First?"), mergeRegions({0.1}), QString(64, QLatin1Char('c')), QStringLiteral("First"));
    doc.card(second, QStringLiteral("Second?"), mergeRegions({0.5}), QString(64, QLatin1Char('d')), QStringLiteral("Second"));
    doc.card(otherDocument, QStringLiteral("Elsewhere?"), mergeRegions({0.05}), QString(64, QLatin1Char('e')), QStringLiteral("Intro"), QStringLiteral("other"));
    doc.card(otherVersion, QStringLiteral("Older PDF?"), mergeRegions({0.8}), QString(64, QLatin1Char('e')), QStringLiteral("Closing"),
             QStringLiteral("paper"), QString(64, QLatin1Char('f')));
    const QByteArray original = doc.source;
    // A card of the same document marked in another file.
    MergeFixture other;
    QVERIFY(other.start(dir.path(), QStringLiteral("docs-test/tex/source/appendix.tex"), mergeNote));
    QVERIFY(other.mark(otherFile, {{18, 18}}));
    other.card(otherFile, QStringLiteral("Appendix?"), mergeRegions({0.3}), QString(64, QLatin1Char('e')), QStringLiteral("Appendix"));
    doc.projection.insert(QStringLiteral("review_card\ncards\n") + otherFile, other.projection.value(QStringLiteral("review_card\ncards\n") + otherFile));
    // A card on the PDF only.
    auto pdfRecord = reviewCard(pdfOnly, QStringLiteral("paper"), QString(64, QLatin1Char('a')));
    doc.projection.insert(QStringLiteral("review_card\ncards\n") + pdfOnly, pdfRecord);
    auto store = doc.store();
    const auto cards = store->cards();
    const auto card = [&cards](const QString &cardId) { return cardWithId(cards, cardId); };
    const auto refusal = [&](const QList<ReviewCard> &chosen) { return store->cardMergeProblem(chosen, {}); };
    QVERIFY(refusal({card(first)}).contains(QStringLiteral("two or more")));
    QVERIFY(refusal({card(first), card(first)}).contains(QStringLiteral("two or more")));
    QVERIFY2(refusal({card(first), card(otherDocument)}).contains(QStringLiteral("one document")), qPrintable(refusal({card(first), card(otherDocument)})));
    QVERIFY(refusal({card(first), card(otherDocument)}).contains(QStringLiteral("other.pdf")));
    QVERIFY(refusal({card(first), card(otherFile)}).contains(QStringLiteral("different source files")));
    QVERIFY(refusal({card(first), card(pdfOnly)}).contains(QStringLiteral("not linked to a source passage")));
    QVERIFY(refusal({card(first), card(otherVersion)}).contains(QStringLiteral("different versions of the PDF")));
    QVERIFY(refusal({card(first), card(second)}).isEmpty());
    QString error;
    // The card to keep must be one of them, and the question must fit.
    QVERIFY(!store->mergeCards({card(first), card(second)}, otherFile, QStringLiteral("Q?"), {}, &error));
    QVERIFY(error.contains(QStringLiteral("card to keep")));
    QVERIFY(!store->mergeCards({card(first), card(second)}, first, QString(501, QLatin1Char('x')), {}, &error));
    QVERIFY(error.contains(QStringLiteral("500")));
    // A copy that changed since it was shown.
    ReviewCard stale = card(first);
    auto primary = stale.record.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("question"), QStringLiteral("Edited elsewhere?"));
    stale.record.insert(QStringLiteral("primary"), primary);
    QVERIFY(refusal({stale, card(second)}).contains(QStringLiteral("changed")));
    // A source edit the server does not have yet.
    {
        QFile append(doc.path);
        QVERIFY(append.open(QIODevice::Append));
        append.write("% A local edit.\n");
    }
    QVERIFY(refusal({card(first), card(second)}).contains(QStringLiteral("changes the server does not have")));
    QVERIFY(!store->mergeCards({card(first), card(second)}, first, QStringLiteral("Q?"), {}, &error));
    QCOMPARE(doc.current(), original + "% A local edit.\n");
    // Back to the version the cards were placed on.
    QVERIFY(doc.save());
    QVERIFY(refusal({card(first), card(second)}).isEmpty());
    // An earlier merge still awaiting recovery.
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("reviews/source-transactions"))));
    QVERIFY(writeObject(dir.filePath(QStringLiteral("reviews/source-transactions/merge-%1.json").arg(id())), {{QStringLiteral("kind"), QStringLiteral("review_card_merge")}}));
    QVERIFY(refusal({card(first), card(second)}).contains(QStringLiteral("awaiting recovery")));
    // Nothing was written by any refusal.
    QCOMPARE(doc.current(), original);
    QVERIFY(doc.queued().isEmpty());
}

void ReviewStoreTest::interruptedMergeIsRecovered()
{
    QTemporaryDir dir;
    MergeFixture doc;
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    QVERIFY(doc.start(dir.path(), relative, mergeNote));
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(doc.mark(second, {{20, 24}}));
    QVERIFY(doc.mark(first, {{5, 10}}));
    const QByteArray original = doc.source;
    doc.card(first, QStringLiteral("First?"), mergeRegions({0.1}), QString(64, QLatin1Char('c')), QStringLiteral("First"));
    doc.card(second, QStringLiteral("Second?"), mergeRegions({0.5}), QString(64, QLatin1Char('d')), QStringLiteral("Second"));
    QByteArray merged;
    QJsonArray records;
    {
        // A merge done elsewhere gives the journal it would leave if interrupted before the source was written.
        QTemporaryDir other;
        MergeFixture copy = doc;
        QVERIFY(copy.start(other.path(), relative, original));
        auto store = copy.store();
        const auto cards = store->cards();
        QString error;
        QVERIFY2(store->mergeCards({cardWithId(cards, first), cardWithId(cards, second)}, first, QStringLiteral("Both?"), {}, &error), qPrintable(error));
        merged = copy.current();
        for (const auto &operation : copy.queued())
            records.append(QJsonObject{{QStringLiteral("relative_path"), operation.value(QStringLiteral("entity_type")).toString() == QLatin1String("review_card")
                                            ? QStringLiteral("cards/%1.json").arg(operation.value(QStringLiteral("entity_id")).toString())
                                            : QStringLiteral("events/mac-okular/%1.json").arg(operation.value(QStringLiteral("entity_id")).toString())},
                                        {QStringLiteral("record"), operation.value(QStringLiteral("payload")).toObject()}});
    }
    QCOMPARE(records.size(), 2);
    QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("reviews/source-transactions"))));
    const QString journal = dir.filePath(QStringLiteral("reviews/source-transactions/merge-%1.json").arg(QUuid::createUuid().toString(QUuid::WithoutBraces)));
    QVERIFY(writeObject(journal, {{QStringLiteral("kind"), QStringLiteral("review_card_merge")}, {QStringLiteral("path"), QFileInfo(doc.path).canonicalFilePath()},
                                  {QStringLiteral("before"), QString::fromLatin1(original.toBase64())}, {QStringLiteral("after"), QString::fromLatin1(merged.toBase64())},
                                  {QStringLiteral("records"), records}}));
    // Opening the store finishes it: the source, then both records in one batch.
    auto store = doc.store();
    QCOMPARE(doc.current(), merged);
    QVERIFY(!QFileInfo::exists(journal));
    const auto queued = doc.queued();
    QCOMPARE(queued.size(), 2);
    QCOMPARE(queued.first().value(QStringLiteral("payload")).toObject().value(QStringLiteral("card_id")).toString(), second);
    QCOMPARE(queued.last().value(QStringLiteral("entity_id")).toString(), first);
    QVERIFY(cardWithId(store->cards(), second).schedule.deleted);
}

void ReviewStoreTest::mergeCommandIsOneRequestWithAReceipt()
{
    QTemporaryDir dir;
    MergeFixture doc;
    const QString relative = QStringLiteral("docs-test/tex/source/main.tex");
    QVERIFY(doc.start(dir.path(), relative, mergeNote));
    const QString first = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString second = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QVERIFY(doc.mark(second, {{20, 24}}));
    QVERIFY(doc.mark(first, {{5, 10}}));
    const QByteArray original = doc.source;
    doc.card(first, QStringLiteral("What opens the method?"), mergeRegions({0.1}), QString(64, QLatin1Char('c')), QStringLiteral("First"));
    doc.card(second, QStringLiteral("What is the signal?"), mergeRegions({0.5}), QString(64, QLatin1Char('d')), QStringLiteral("Second"));
    auto store = doc.store();
    const QJsonObject request{{QStringLiteral("schema_version"), 1}, {QStringLiteral("command"), QStringLiteral("merge-review-cards")},
        {QStringLiteral("card_ids"), QJsonArray{second, first}}, {QStringLiteral("source"), relative},
        {QStringLiteral("expected_source_sha256"), sha256Hex(original)}, {QStringLiteral("request_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    const auto run = [&](QJsonObject changed) {
        for (auto it = changed.begin(); it != changed.end(); ++it) if (it.value().isNull()) changed.remove(it.key());
        return store->sourceCardCommand(changed, {});
    };
    const auto with = [&](const QString &key, const QJsonValue &value) { auto changed = request; changed.insert(key, value); return changed; };
    QCOMPARE(run(with(QStringLiteral("card_ids"), QJsonArray{first})).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(run(with(QStringLiteral("card_ids"), QJsonArray{first, first})).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(run(with(QStringLiteral("card_ids"), QJsonArray{first, second.toUpper()})).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(run(with(QStringLiteral("keep_card_id"), QUuid::createUuid().toString(QUuid::WithoutBraces))).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(run(with(QStringLiteral("unexpected"), 1)).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    QCOMPARE(run(with(QStringLiteral("expected_source_sha256"), QString(64, QLatin1Char('0')))).value(QStringLiteral("error_code")).toString(), QStringLiteral("source_conflict"));
    QCOMPARE(run(with(QStringLiteral("card_ids"), QJsonArray{first, QUuid::createUuid().toString(QUuid::WithoutBraces)})).value(QStringLiteral("error_code")).toString(),
             QStringLiteral("card_unavailable"));
    QCOMPARE(run(with(QStringLiteral("question"), QString(501, QLatin1Char('x')))).value(QStringLiteral("error_code")).toString(), QStringLiteral("merge_refused"));
    QCOMPARE(doc.current(), original);
    QVERIFY(doc.queued().isEmpty());

    // Defaults: the first card in reading order (neither was reviewed), the questions joined.
    const auto result = run(request);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("state")).toString(), QStringLiteral("merged"));
    QCOMPARE(result.value(QStringLiteral("kept_card_id")).toString(), first);
    QCOMPARE(result.value(QStringLiteral("deleted_card_ids")).toArray(), QJsonArray{second});
    QCOMPARE(result.value(QStringLiteral("card_order")).toArray(), (QJsonArray{first, second}));
    QCOMPARE(result.value(QStringLiteral("question")).toString(), QStringLiteral("What opens the method? What is the signal?"));
    QCOMPARE(result.value(QStringLiteral("passages")).toInt(), 2);
    QCOMPARE(result.value(QStringLiteral("source_sha256")).toString(), sha256Hex(doc.current()));
    QCOMPARE(doc.current(), markReviewSourceRanges(relative, mergeNote, {{5, 10}, {20, 24}}, first).replacement);
    QCOMPARE(doc.queued().size(), 2);
    // An uncertain retry replays the result and merges nothing again; other arguments are refused.
    const auto replayed = run(request);
    QVERIFY(replayed.value(QStringLiteral("replayed")).toBool());
    QCOMPARE(replayed.value(QStringLiteral("kept_card_id")).toString(), first);
    QCOMPARE(doc.queued().size(), 2);
    QCOMPARE(run(with(QStringLiteral("question"), QStringLiteral("Other?"))).value(QStringLiteral("error_code")).toString(), QStringLiteral("request_conflict"));
}

void ReviewStoreTest::mergeOfARealSourceCopy()
{
    // A check on a copy of a real note: THOUGHTS_MERGE_SOURCE is the copy's path (its Library path in
    // THOUGHTS_MERGE_SOURCE_ID, ending in .tex or .md), THOUGHTS_MERGE_CARDS the card IDs to merge,
    // the one to keep first. The merged source is written next to the copy (.merged).
    const QString path = qEnvironmentVariable("THOUGHTS_MERGE_SOURCE");
    const QStringList ids = qEnvironmentVariable("THOUGHTS_MERGE_CARDS").split(QLatin1Char(','), Qt::SkipEmptyParts);
    if (path.isEmpty() || ids.size() < 2) QSKIP("Set THOUGHTS_MERGE_SOURCE and THOUGHTS_MERGE_CARDS to check a copy of a real source.");
    const QString relative = qEnvironmentVariable("THOUGHTS_MERGE_SOURCE_ID", QFileInfo(path).fileName());
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray source = file.readAll();
    QHash<QString, QSet<QString>> segments;
    QByteArray unmarked = source;
    for (const QString &cardId : ids) {
        const auto anchor = readReviewSourceAnchor(relative, source, cardId);
        QVERIFY2(anchor.error.isEmpty(), qPrintable(anchor.error));
        for (const auto &value : anchor.anchor.value(QStringLiteral("segments")).toArray())
            segments[cardId].insert(value.toObject().value(QStringLiteral("segment_id")).toString());
        unmarked = stripReviewCardMarkers(relative, unmarked, cardId).text;
    }
    const auto plan = planReviewCardMergeSource(relative, source, ids.first(), ids.mid(1), segments);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    for (const QString &cardId : ids.mid(1)) QVERIFY(!plan.replacement.contains(cardId.toUtf8()));
    int passages = 0;
    for (const auto &cardSegments : std::as_const(segments)) passages += cardSegments.size();
    QCOMPARE(plan.anchor.value(QStringLiteral("segments")).toArray().size(), passages);
    // Only markers changed, and the kept card's change moves only its own.
    QCOMPARE(stripReviewCardMarkers(relative, plan.replacement, ids.first()).text, unmarked);
    QCOMPARE(stripReviewCardMarkers(relative, plan.versions.constLast(), ids.first()).text, unmarked);
    QFile output(path + QStringLiteral(".merged"));
    QVERIFY(output.open(QIODevice::WriteOnly));
    output.write(plan.replacement);
}

void ReviewStoreTest::htmlCardHasNoPdfGeometryAndWaitsForTheServer()
{
    QTemporaryDir dir;
    const QVariantMap document{{QStringLiteral("pdf_stem"), QStringLiteral("paper")},
                               {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                               {QStringLiteral("document_id"), QStringLiteral("c1870925-b9d7-45f5-b112-ba599f2b5af4")}};
    const QJsonObject quote{{QStringLiteral("exact"), QStringLiteral("the answer passage")}, {QStringLiteral("prefix"), QStringLiteral("Before ")}, {QStringLiteral("suffix"), QStringLiteral(" after")}};
    QJsonObject html{{QStringLiteral("created_in"), QStringLiteral("html")}, {QStringLiteral("html_edition_sha256"), QString(64, QLatin1Char('b'))}, {QStringLiteral("text_quote"), quote}};
    ReviewStore store(dir.path());
    ReviewCard card;
    QString error;
    // Markdown: the exact source bytes travel with it.
    html.insert(QStringLiteral("source_range"), QJsonObject{{QStringLiteral("source_file"), QStringLiteral("docs-notes/notes.md")},
                                                           {QStringLiteral("source_sha256"), QString(64, QLatin1Char('c'))},
                                                           {QStringLiteral("start_byte"), 10}, {QStringLiteral("end_byte"), 28}, {QStringLiteral("exact"), true}});
    QVERIFY2(store.createHtmlCard(document, html, QStringLiteral("Which passage?"), &card, &error, 0), qPrintable(error));
    const QJsonObject primary = card.record.value(QStringLiteral("primary")).toObject();
    for (const QString &absent : {QStringLiteral("page"), QStringLiteral("rect"), QStringLiteral("regions"), QStringLiteral("page_visual_sha256"), QStringLiteral("source_anchor")})
        QVERIFY2(!primary.contains(absent), qPrintable(absent));
    QCOMPARE(primary.value(QStringLiteral("created_in")).toString(), QStringLiteral("html"));
    QCOMPARE(primary.value(QStringLiteral("html_edition_sha256")).toString(), QString(64, QLatin1Char('b')));
    QCOMPARE(primary.value(QStringLiteral("text_quote")).toObject(), quote);
    QCOMPARE(primary.value(QStringLiteral("selected_text")).toString(), QStringLiteral("the answer passage"));
    QCOMPARE(primary.value(QStringLiteral("source_range")).toObject().value(QStringLiteral("end_byte")).toInt(), 28);
    QCOMPARE(card.linkingState(), QStringLiteral("pending_server_link"));
    // Stored, listed, kept out of reviews until the server links it.
    QCOMPARE(store.cards().size(), 1);
    QVERIFY(store.dueCards().isEmpty());
    QVERIFY(store.browserCards().first().hasIssues);
    // Without the edition or a quote nothing is saved.
    QJsonObject noEdition = html;
    noEdition.remove(QStringLiteral("html_edition_sha256"));
    QVERIFY(!store.createHtmlCard(document, noEdition, QStringLiteral("Q?"), nullptr, &error));
    QJsonObject noQuote = html;
    noQuote.insert(QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("  ")}});
    QVERIFY(!store.createHtmlCard(document, noQuote, QStringLiteral("Q?"), nullptr, &error));
    QCOMPARE(store.cards().size(), 1);
}

void ReviewStoreTest::htmlAttachmentCardIsFoundByItsText()
{
    QTemporaryDir dir;
    const QVariantMap document{{QStringLiteral("pdf_stem"), QStringLiteral("ideas")},
                               {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                               {QStringLiteral("document_id"), QStringLiteral("c1870925-b9d7-45f5-b112-ba599f2b5af4")}};
    const QJsonObject quote{{QStringLiteral("exact"), QStringLiteral("agents pay a toll of one cent per page")},
                            {QStringLiteral("prefix"), QStringLiteral("has to pay first: ")}, {QStringLiteral("suffix"), QStringLiteral(" before the text")}};
    const QString path = QStringLiteral("docs-ideas/tex/support/html/explainer.html");
    QJsonObject attachment{{QStringLiteral("created_in"), QStringLiteral("html_attachment")},
                           {QStringLiteral("html_attachment"), QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("sha256"), QString(64, QLatin1Char('d'))}}},
                           {QStringLiteral("text_quote"), quote},
                           {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("#simulator")}}}};
    ReviewStore store(dir.path());
    ReviewCard card;
    QString error;
    QVERIFY2(store.createHtmlAttachmentCard(document, attachment, QStringLiteral("What do agents pay?"), &card, &error, 0), qPrintable(error));
    QVERIFY(Thoughts::isHtmlAttachmentCard(card.record));
    const QJsonObject primary = card.record.value(QStringLiteral("primary")).toObject();
    for (const QString &absent : {QStringLiteral("page"), QStringLiteral("rect"), QStringLiteral("regions"), QStringLiteral("page_visual_sha256"), QStringLiteral("source_anchor"),
                                  QStringLiteral("source_range"), QStringLiteral("html_edition_sha256")})
        QVERIFY2(!primary.contains(absent), qPrintable(absent));
    QCOMPARE(primary.value(QStringLiteral("created_in")).toString(), QStringLiteral("html_attachment"));
    QCOMPARE(primary.value(QStringLiteral("html_attachment")).toObject(), QJsonObject({{QStringLiteral("path"), path}, {QStringLiteral("sha256"), QString(64, QLatin1Char('d'))}}));
    QCOMPARE(primary.value(QStringLiteral("text_quote")).toObject(), quote);
    QCOMPARE(primary.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"));
    QCOMPARE(primary.value(QStringLiteral("selected_text")).toString(), quote.value(QStringLiteral("exact")).toString());
    QCOMPARE(primary.value(QStringLiteral("pdf_stem")).toString(), QStringLiteral("ideas"));
    QCOMPARE(card.record.value(QStringLiteral("source_authority")).toString(), QStringLiteral("client-v1"));
    QCOMPARE(card.record.value(QStringLiteral("source_link")).toObject(), QJsonObject({{QStringLiteral("state"), QStringLiteral("html_attachment")}}));
    QCOMPARE(card.linkingState(), QStringLiteral("html_attachment"));
    // Stored, listed and due: no link is waited for, and no PDF place is needed.
    QCOMPARE(store.cards().size(), 1);
    QCOMPARE(store.dueCards().size(), 1);
    QCOMPARE(store.dueCards().first().id(), card.id());
    QVERIFY(store.dueCards().first().regions().isEmpty());
    QVERIFY(!store.browserCards().first().hasIssues);
    QVERIFY(store.browserCards().first().canReview());
    // Its owner PDF can be rebuilt (a new hash) without making the card stale.
    QVERIFY(writeObject(dir.filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")),
                        QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("ideas.pdf")}, {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('e'))}}}}}));
    const QJsonObject projections{{QStringLiteral("review_card\ncards\n") + card.id(), card.record}};
    ReviewStore projected(dir.path(), [projections] { return projections; });
    QCOMPARE(projected.dueCards().size(), 1);
    QVERIFY(projected.browserCards().first().state != ReviewBrowserState::Stale);
    // Invalid paths, hashes and page states are refused; nothing more is saved.
    const auto refused = [&](const QJsonObject &changed) {
        QString why;
        const bool ok = store.createHtmlAttachmentCard(document, changed, QStringLiteral("Q?"), nullptr, &why);
        return !ok && !why.isEmpty();
    };
    for (const QString &badPath : {QStringLiteral("docs-ideas/tex/support/html/../secret.html"), QStringLiteral("ideas/explainer.html"), QStringLiteral("docs-ideas/tex/support/html/page.pdf")}) {
        QJsonObject changed = attachment;
        changed.insert(QStringLiteral("html_attachment"), QJsonObject{{QStringLiteral("path"), badPath}, {QStringLiteral("sha256"), QString(64, QLatin1Char('d'))}});
        QVERIFY2(refused(changed), qPrintable(badPath));
    }
    QJsonObject badSha = attachment;
    badSha.insert(QStringLiteral("html_attachment"), QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("sha256"), QString(64, QLatin1Char('D'))}});
    QVERIFY(refused(badSha));
    QJsonObject badHash = attachment;
    badHash.insert(QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("simulator")}});
    QVERIFY(refused(badHash));
    badHash.insert(QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QString(QStringLiteral("#") + QString(512, QLatin1Char('x')))}});
    QVERIFY(refused(badHash));
    QJsonObject wrongKind = attachment;
    wrongKind.insert(QStringLiteral("created_in"), QStringLiteral("html"));
    QVERIFY(refused(wrongKind));
    QJsonObject noQuote = attachment;
    noQuote.insert(QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral(" ")}});
    QVERIFY(refused(noQuote));
    // No page state: an empty hash.
    QJsonObject noState = attachment;
    noState.remove(QStringLiteral("page_state"));
    ReviewCard plain;
    QVERIFY2(store.createHtmlAttachmentCard(document, noState, QStringLiteral("Q?"), &plain, &error), qPrintable(error));
    QCOMPARE(plain.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QString());
    QCOMPARE(store.cards().size(), 2);
}

void ReviewStoreTest::cardWithoutLocalSourceMapWaitsForTheServer()
{
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), fourParagraphs, fourParagraphPages));
    // The source map of this PDF is not installed on this computer yet.
    QVERIFY(QFile::remove(doc.bundle + QStringLiteral("/documents/paper.pdf.json")));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QList<ReviewArea> areas{{1, LinkedDocument::band(100)}, {2, LinkedDocument::band(200)}};
    auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, areas, id);
    QVERIFY(!plan.error.isEmpty());
    QVERIFY(plan.sourceMapUnavailable);
    QVERIFY(!plan.withoutSource);
    // An area the source map rejects is no case for the server.
    QVERIFY(!planReviewSourceAnchors(dir.path(), dir.filePath(QStringLiteral("elsewhere")), doc.pdfPath, QString(64, QLatin1Char('a')), areas, id).sourceMapUnavailable);
    plan.linkOnServer = true;
    for (const auto &area : areas)
        plan.regions.append(QJsonObject{{QStringLiteral("page"), area.page}, {QStringLiteral("rect"), QJsonArray{area.rect.left(), area.rect.top(), area.rect.right(), area.rect.bottom()}}});
    plan.pageVisualSha256 = fingerprintReviewPage(doc.pdfPath, 1).value(QStringLiteral("page_visual_sha256")).toString();
    ReviewStore store(dir.path());
    ReviewCard card;
    QString error;
    QVERIFY2(store.createManualCard(doc.document, 1, rectList(areas[0].rect), QStringLiteral("Which words?"), plan.pageVisualSha256,
        &card, &error, 0, {}, &plan, id), qPrintable(error));
    QCOMPARE(card.linkingState(), QStringLiteral("pending_server_link"));
    QCOMPARE(card.record.value(QStringLiteral("source_link")).toObject().value(QStringLiteral("reason")).toString(), plan.error);
    QCOMPARE(card.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("regions")).toArray().size(), 2);
    QVERIFY(card.record.value(QStringLiteral("primary")).toObject().value(QStringLiteral("source_anchor")).toObject().isEmpty());
    QVERIFY(!card.record.contains(QStringLiteral("source_change")));
    QCOMPARE(doc.current(), fourParagraphs);
    // Kept out of reviews until the server links it.
    QCOMPARE(store.cards().size(), 1);
    QVERIFY(store.dueCards().isEmpty());
    QVERIFY(store.browserCards().first().hasIssues);
}

namespace {
QByteArray textFallbackFixture(const QString &name)
{
    QFile f(QStringLiteral(KDESRCDIR "data/card-text-fallback/") + name);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
// The PDF text of statistics.tex lines 63-70 as LaTeX sets it, one visual line each.
const QStringList mcNemarLines {
    QStringLiteral("Exact McNemar test"),
    QStringLiteral("The exact McNemar test is the paired permutation test above, applied to binary"),
    QStringLiteral("outcomes. Suppose two models A and B answer the same n questions, and let"),
    QStringLiteral("Xi,Yi \u2208 {0,1} be 1 when A, respectively B, answers question i correctly. The"),
    QStringLiteral("paired differences then take only three values:"),
    QStringLiteral("Di = Yi \u2212 Xi \u2208 {\u22121,0,+1}."),
    QStringLiteral("The n questions fall into four counts:")};
QList<LinkedDocument::Line> visualLines(const QStringList &texts, const QList<int> &sourceLines)
{
    QList<LinkedDocument::Line> lines;
    for (int i = 0; i < texts.size(); ++i) lines.append({texts[i], sourceLines[i], 100.0 + 24 * i});
    return lines;
}
// An area over visual lines first..last of visualLines.
QRectF visualLinesArea(int first, int last)
{
    return QRectF(QPointF(0.0, (100.0 + 24 * first - 18) / 842.0), QPointF(1.0, (100.0 + 24 * last + 8) / 842.0));
}
// An area around the first occurrence of one word on a page.
QRectF wordArea(const QString &pdfPath, int page, const QString &word)
{
    const auto document = Poppler::Document::load(pdfPath);
    const auto pdfPage = document ? document->page(page - 1) : nullptr;
    if (!pdfPage) return {};
    const QSizeF size = pdfPage->pageSizeF();
    for (const auto &box : pdfPage->textList()) {
        if (box->text() != word) continue;
        const QRectF r = box->boundingBox().adjusted(-1, -1, 1, 1);
        return QRectF(r.x() / size.width(), r.y() / size.height(), r.width() / size.width(), r.height() / size.height());
    }
    return {};
}
QStringList pdfWords(const QString &text)
{
    return text.split(QLatin1Char(' '), Qt::SkipEmptyParts);
}
}

void ReviewStoreTest::areaOfAProposalAcceptedSinceTheBuildIsPlacedByText()
{
    // 2026-10-15: the PDF was built while a proposal was pending; accepting it changed only its
    // wrapper lines, so the PDF was kept. SyncTeX put the whole proposal on its closing brace
    // (line 133 of the build's source), and the card failed with "the historical source line
    // could not be mapped safely (selected text: paired)".
    const QByteArray built = textFallbackFixture(QStringLiteral("statistics-built.tex"));
    const QByteArray current = textFallbackFixture(QStringLiteral("statistics-current.tex"));
    QVERIFY(built.contains("\\agentProposedEdit") && !current.contains("\\agentProposedEdit"));
    QCOMPARE(built.split('\n').value(132), QByteArray("}\\end{document}"));
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), current, {{{QStringLiteral("Statistical tests"), 2, 100}},
        visualLines(mcNemarLines, QList<int>(mcNemarLines.size(), 133))}, {}, built));
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    // The heading, the paragraph with its inline math, and the display formula after it.
    const QRectF area = visualLinesArea(0, 5);
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{2, area}}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    const auto expected = markReviewSourceRanges(doc.sourceId, current, {{63, 69}}, id);
    QVERIFY2(expected.error.isEmpty(), qPrintable(expected.error));
    QCOMPARE(plan.original, current);
    QCOMPARE(plan.replacement, expected.replacement);
    QCOMPARE(plan.anchor, expected.anchor);
    QCOMPARE(plan.regions.size(), 1);

    ReviewStore store(dir.path());
    ReviewCard card;
    QString error;
    QVERIFY2(store.createManualCard(doc.document, 2, rectList(area), QStringLiteral("What is the exact McNemar test?"),
        plan.pageVisualSha256, &card, &error, 0, {}, &plan, id), qPrintable(error));
    QCOMPARE(card.linkingState(), QStringLiteral("linked"));
    QCOMPARE(doc.current(), plan.replacement);

    // Placing the card again on the same old PDF moves its markers, by text too.
    const auto moved = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{2, visualLinesArea(1, 5)}}, id, true);
    QVERIFY2(moved.error.isEmpty(), qPrintable(moved.error));
    QCOMPARE(moved.original, plan.replacement);
    QCOMPARE(moved.replacement, markReviewSourceRanges(doc.sourceId, current, {{64, 69}}, id).replacement);
}

void ReviewStoreTest::wordOnTwoNearbyLinesIsPlacedByTheAreasText()
{
    // The PDF rebuilt from the accepted source: the build's source is the current one. SyncTeX
    // gives line 65 for the visual line holding "paired" (line 64); "paired" is also on line 66,
    // at the same distance, so the word alone cannot choose its line.
    const QByteArray current = textFallbackFixture(QStringLiteral("statistics-current.tex"));
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), current, {visualLines(mcNemarLines, {63, 65, 65, 65, 66, 68, 70})}));
    Okular::DocsCommentSynchronizer::Options options;
    options.docsRoot = dir.path();
    options.sourceBundleRoot = doc.bundle;
    options.allowLegacySourceResolution = false;
    Okular::DocsCommentSynchronizer resolver(options);
    const QRectF word = wordArea(doc.pdfPath, 1, QStringLiteral("paired"));
    QVERIFY(!word.isEmpty());
    const auto single = resolver.resolveSource({QStringLiteral("paper"), 1, word.center().x() * 595, word.center().y() * 842,
        QStringLiteral("paired"), doc.pdfPath, true});
    QVERIFY(!single.success);
    QCOMPARE(single.failureReason, QStringLiteral("the historical source line could not be mapped safely"));

    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, visualLinesArea(1, 4)}}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.replacement, markReviewSourceRanges(doc.sourceId, current, {{64, 66}}, id).replacement);
    // The word alone is still refused, with the line mapping's error.
    const auto alone = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, word}}, id);
    QVERIFY2(alone.error.contains(QStringLiteral("could not be mapped safely (selected text: paired)")), qPrintable(alone.error));
    QVERIFY(alone.replacement.isEmpty());
}

void ReviewStoreTest::mathAreaIsPlacedByItsProse()
{
    const QString path = QStringLiteral("docs-concepts/tex/source/statistics.tex");
    const QByteArray current = textFallbackFixture(QStringLiteral("statistics-current.tex"));
    // Lines 79-84: the display formula's PDF text has its letters in another order than its TeX.
    const QStringList meanDifference = pdfWords(QStringLiteral(
        "The mean difference is exactly the difference in accuracy: "
        "Tobs = n 1 n \u2211 i=1 Di = n c\u2212b = accB \u2212 accA. "
        "The a+d concordant questions, which both models get right or both get wrong, have Di = 0, and flipping their sign changes nothing. "
        "So they carry no information about which model is better, and only the nd = b+c discordant questions matter."));
    auto placed = locateReviewTextInSource(path, current, meanDifference);
    QCOMPARE(placed.first, 79);
    QCOMPARE(placed.last, 84);
    QVERIFY(!placed.ambiguous);
    // Lines 89-93: an area ending with a display formula includes it, whole.
    const QStringList pValue = pdfWords(QStringLiteral(
        "This binomial is the permutation null distribution of the previous section in closed form, so all 2n sign flips are covered without enumerating them. "
        "Since |Tperm| \u2265 |Tobs| exactly when C \u2264 min(b,c) or C \u2265 max(b,c), the two-sided p-value is "
        "p = min 1, 2 min(b,c) \u2211 k=0 b+c k 2\u2212(b+c) ."));
    placed = locateReviewTextInSource(path, current, pValue);
    QCOMPARE(placed.first, 89);
    QCOMPARE(placed.last, 93);
    // Inline math whose PDF text has the source's letters: lines 64-66, by letters alone.
    placed = locateReviewTextInSource(path, current, pdfWords(mcNemarLines.mid(1, 4).join(QLatin1Char(' '))));
    QCOMPARE(placed.first, 64);
    QCOMPARE(placed.last, 66);
    // Prose of another passage between the pieces breaks the region.
    const QStringList skipping = pdfWords(QStringLiteral(
        "The mean difference is exactly the difference in accuracy: Tobs = n 1 n \u2211 i=1 Di. "
        "So they carry no information about which model is better, and only the nd = b+c discordant questions matter."));
    QCOMPARE(locateReviewTextInSource(path, current, skipping).first, 0);
    // A piece that is not in the source refuses the whole area.
    QCOMPARE(locateReviewTextInSource(path, current, meanDifference + pdfWords(QStringLiteral("Something nobody ever wrote here."))).first, 0);
}

void ReviewStoreTest::figureLabelsInAnAreaAreSkipped()
{
    // A card around a whole section with a figure (10 November, "5. The number e"): the labels
    // drawn inside the picture are in no source line, but the rest of the area is.
    const QString path = QStringLiteral("docs-concepts/tex/source/logarithm.tex");
    const QByteArray source =
        "Different bases differ only by a constant factor, derived in Section 7.\n"   // 1
        "\n"                                                                          // 2
        "\\subsection*{5. The number \\(e\\) and the natural logarithm}\n"            // 3
        "Compound interest gives a special base.\n"                                   // 4
        "One unit at 100\\% interest per year, paid in \\(n\\) parts, grows to \\((1+1/n)^n\\).\n" // 5
        "Paying ever more often approaches a limit (Bernoulli, 1683):\n"              // 6
        "\\[\n"                                                                       // 7
        "e=\\lim_{n\\to\\infty}\\Bigl(1+\\frac1n\\Bigr)^{n}=2.71828\\ldots\n"         // 8
        "\\]\n"                                                                       // 9
        "\\begin{figure}[H]\n"                                                        // 10
        "\\centering\n"                                                               // 11
        "\\includegraphics[width=0.75\\linewidth]{agent-figures/math-e-compound}\n"   // 12
        "\\caption{One unit at 100\\% interest a year, paid \\(n\\) times, approaches \\(e\\) from below.}\\label{fig:math-e}\n" // 13
        "\\end{figure}\n"                                                             // 14
        "The values grow with \\(n\\) but never pass \\(e\\) (Figure~\\ref{fig:math-e}).\n" // 15
        "The logarithm in base \\(e\\) is the natural logarithm, the definition used today.\n" // 16
        "\n"                                                                          // 17
        "\\paragraph{The logarithm as an area}\n";                                    // 18
    const QString figureLabels = QStringLiteral("3.00 2.93 2.75 e = 2.718 2.715 2.50 2.613 1 CHF at 100% a year, paid n times a year: (1 + 1/n)n ");
    const QStringList area = pdfWords(QStringLiteral(
        "5. The number e and the natural logarithm Compound interest gives a special base. "
        "One unit at 100% interest per year, paid in n parts, grows to (1 + 1/n)n . "
        "Paying ever more often approaches a limit (Bernoulli, 1683): e = lim n\u2192\u221e 1 + 1 n n = 2.71828 . . . ")
        + figureLabels + QStringLiteral(
        "Figure 1: One unit at 100% interest a year, paid n times, approaches e from below. "
        "The values grow with n but never pass e (Figure 1). "
        "The logarithm in base e is the natural logarithm, the definition used today."));
    const auto placed = locateReviewTextInSource(path, source, area);
    QCOMPARE(placed.first, 3);
    QCOMPARE(placed.last, 16);
    QVERIFY(!placed.ambiguous);

    // Without a picture in the source between them, text that is not in the source still
    // refuses the area.
    QByteArray noPicture = source;
    noPicture.replace("\\includegraphics[width=0.75\\linewidth]{agent-figures/math-e-compound}\n", "");
    QCOMPARE(locateReviewTextInSource(path, noPicture, area).first, 0);
}

void ReviewStoreTest::typographicQuotesMatchTheirTexSpelling()
{
    // 11 November ("Model Merging"): TeX prints models' as models’, and the word failed with
    // "the historical source line could not be mapped safely (selected text: models’)".
    // The line is too short for the text fallback, so only the line mapping can place it.
    const QByteArray source("\\documentclass{article}\n\\begin{document}\n"
                            "The models' ``best'' run -- twice.\n\nDelta echo foxtrot.\n\\end{document}\n");
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), source, {{{QStringLiteral("The models\u2019 \u201cbest\u201d run \u2013 twice."), 3, 100},
        {QStringLiteral("Delta echo foxtrot."), 5, 200}}}));
    // The PDF text has a typographic apostrophe (Qt writes U+02BC, pdfTeX U+2019), not the source's.
    QVERIFY(wordArea(doc.pdfPath, 1, QStringLiteral("models'")).isEmpty());
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, LinkedDocument::band(100)}}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.replacement, markReviewSourceRanges(doc.sourceId, source, {{3, 3}}, id).replacement);
}

void ReviewStoreTest::retiredTexLinkTargetsAreNotText()
{
    // The same paragraph: its \TexLink (retired, still in older notes) printed only its last
    // argument, so the text fallback could not place the area either.
    const QString path = QStringLiteral("docs-concepts/tex/source/model_merging.tex");
    const QByteArray source(
        "\\begin{document}\n"
        "Model merging combines several trained networks with the same architecture into a single network.\n"
        "This distinguishes it from ensembling, which averages the models' outputs and runs every model, and from a "
        "\\TexLink{docs-concepts/moe.tex}{doc:moe}{mixture of experts}, which keeps the models apart and learns a router.\n"
        "\n"
        "Merging works only under one condition.\n");
    // pdfTeX's PDF text has U+2019 for the apostrophe; other PDF writers the modifier letter U+02BC.
    for (const QString apostrophe : {QStringLiteral("\u2019"), QStringLiteral("\u02bc")}) {
        const auto placed = locateReviewTextInSource(path, source, pdfWords(QStringLiteral(
            "This distinguishes it from ensembling, which averages the models%1 outputs and runs every model, and from a "
            "mixture of experts, which keeps the models apart and learns a router.").arg(apostrophe)));
        QCOMPARE(placed.first, 3);
        QCOMPARE(placed.last, 3);
        QVERIFY(!placed.ambiguous);
    }
}

void ReviewStoreTest::repeatedTextIsNotPlaced()
{
    const QString path = QStringLiteral("docs-test/tex/source/main.tex");
    const QByteArray prose("\\begin{document}\nThe paired differences then take only three values.\n\nSomething else.\n\n"
                           "The paired differences then take only three values.\n\\end{document}\n");
    auto placed = locateReviewTextInSource(path, prose, pdfWords(QStringLiteral("The paired differences then take only three values.")));
    QCOMPARE(placed.first, 0);
    QVERIFY(placed.ambiguous);
    // The same for prose pieces around math.
    const QByteArray math("\\begin{document}\nThe mean \\(\\frac{1}{n}\\sum_{i=1}^n D_i\\) is the statistic of interest for this test.\n\n"
                          "Something else.\n\nThe mean \\(\\frac{1}{n}\\sum_{i=1}^n D_i\\) is the statistic of interest for this test.\n\\end{document}\n");
    placed = locateReviewTextInSource(path, math, pdfWords(QStringLiteral("The mean 1 n n \u2211 i=1 Di is the statistic of interest for this test.")));
    QCOMPARE(placed.first, 0);
    QVERIFY(placed.ambiguous);
    // Through the card, SyncTeX naming the line between the copies: the line mapping's error
    // stays and nothing is marked.
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), prose, {{{QStringLiteral("The paired differences then take only three values."), 4, 100}}}));
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{1, LinkedDocument::band(100)}},
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QVERIFY(plan.error.contains(QStringLiteral("could not be mapped safely")));
    QVERIFY(plan.replacement.isEmpty());
    QCOMPARE(doc.current(), prose);
}

void ReviewStoreTest::shortTextIsNotPlaced()
{
    const QString path = QStringLiteral("docs-concepts/tex/source/statistics.tex");
    const QByteArray built = textFallbackFixture(QStringLiteral("statistics-built.tex"));
    const QByteArray current = textFallbackFixture(QStringLiteral("statistics-current.tex"));
    QVERIFY(!locateReviewTextInSource(path, current, {QStringLiteral("paired")}).first);
    QVERIFY(!locateReviewTextInSource(path, current, {QStringLiteral("paired")}).ambiguous);
    // Unique, but fewer than 24 letters.
    QVERIFY(!locateReviewTextInSource(path, current, pdfWords(QStringLiteral("Pairing is what makes"))).first);
    // Part of a source line is no answer passage.
    QVERIFY(!locateReviewTextInSource(path, current, pdfWords(QStringLiteral("Comparing the two accuracies as if they came from independent samples"))).first);
    QCOMPARE(locateReviewTextInSource(path, current, pdfWords(QStringLiteral(
        "Comparing the two accuracies as if they came from independent samples would treat the variation in question difficulty as noise, "
        "although both models face the same questions."))).first, 98);
    // The single word of the case of 2026-10-15 still fails as before, and nothing is written.
    QTemporaryDir dir;
    LinkedDocument doc;
    QVERIFY(doc.build(dir.path(), current, {{{QStringLiteral("Statistical tests"), 2, 100}},
        visualLines(mcNemarLines, QList<int>(mcNemarLines.size(), 133))}, {}, built));
    const QRectF word = wordArea(doc.pdfPath, 2, QStringLiteral("paired"));
    QVERIFY(!word.isEmpty());
    const auto plan = planReviewSourceAnchors(dir.path(), doc.bundle, doc.pdfPath, doc.pdfSha, {{2, word}},
        QUuid::createUuid().toString(QUuid::WithoutBraces));
    QCOMPARE(plan.error, QStringLiteral("the historical source line could not be mapped safely (selected text: paired)"));
    QCOMPARE(plan.failedArea, 0);
    QVERIFY(plan.replacement.isEmpty());
    QCOMPARE(doc.current(), current);
}

// thoughts-server protocol/fixtures/review-card-retire.json (section 4): every case in the given
// order and reversed, through the pure fold and through the store's cards.
void ReviewStoreTest::retireFoldMatchesTheConformanceCases()
{
    QFile file(QStringLiteral(KDESRCDIR "data/review-card-retire.json"));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QJsonArray cases = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("cases")).toArray();
    QCOMPARE(cases.size(), 24);
    for (const auto &value : cases) {
        const QJsonObject testCase = value.toObject();
        const QString name = testCase.value(QStringLiteral("name")).toString();
        const QString cardId = testCase.value(QStringLiteral("card_id")).toString();
        const QJsonObject expected = testCase.value(QStringLiteral("expected")).toObject();
        QList<QJsonObject> events;
        for (const auto &event : testCase.value(QStringLiteral("events")).toArray()) events.append(event.toObject());
        QList<QJsonObject> reversed(events.crbegin(), events.crend());
        for (const auto &order : {events, reversed}) {
            const ReviewRetire::State state = ReviewRetire::fold(order).value(cardId);
            const QString folded = state.deleted ? QStringLiteral("deleted") : state.retired ? QStringLiteral("retired") : QStringLiteral("active");
            QVERIFY2(!(state.deleted && state.retired), qPrintable(name));
            QCOMPARE(folded, expected.value(QStringLiteral("state")).toString());
            QCOMPARE(state.retired ? QJsonValue(state.retiredAtText) : QJsonValue(QJsonValue::Null), expected.value(QStringLiteral("retired_at")));
            QCOMPARE(state.retired ? QJsonValue(state.retireEventId) : QJsonValue(QJsonValue::Null), expected.value(QStringLiteral("retire_event_id")));
            if (state.retired) QCOMPARE(state.retiredAt, QDateTime::fromString(expected.value(QStringLiteral("retired_at")).toString(), Qt::ISODateWithMs));
        }

        // The store reads the same state, and the answer fold never sees a retire or restore.
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                            reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
        QList<QJsonObject> answers;
        for (int index = 0; index < events.size(); ++index) {
            QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/%1.json").arg(index)), events.at(index)));
            if (!ReviewRetire::isEvent(events.at(index))) answers.append(events.at(index));
        }
        const auto cards = ReviewStore(temporary.path()).cards();
        QCOMPARE(cards.size(), 1);
        const ReviewCard &card = cards.constFirst();
        const QString stored = card.schedule.deleted ? QStringLiteral("deleted") : card.schedule.retired ? QStringLiteral("retired") : QStringLiteral("active");
        QVERIFY2(stored == expected.value(QStringLiteral("state")).toString(), qPrintable(name));
        QCOMPARE(card.schedule.retired ? QJsonValue(card.schedule.retireEventId) : QJsonValue(QJsonValue::Null), expected.value(QStringLiteral("retire_event_id")));
        const ReviewSchedule withoutRetire = ReviewStore::foldEvents(answers).value(cardId);
        QCOMPARE(card.schedule.reviewCount, withoutRetire.reviewCount);
        QCOMPARE(card.schedule.lastReviewedAt, withoutRetire.lastReviewedAt);
    }
}

void ReviewStoreTest::retireAndRestoreKeepTheScheduleAndLeaveTheQueue()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime now(QDate(2026, 10, 27), QTime(14, 2, 11, 204), QTimeZone::UTC);
    const QString answered = QStringLiteral("00000000-0000-4000-8000-0000000000c1");
    const QString other = QStringLiteral("00000000-0000-4000-8000-0000000000c2");
    for (const QString &cardId : {answered, other})
        QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                            reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
    // Answered ten days ago with a five-day interval: due (overdue) now.
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/test/answer.json")),
                        QJsonObject{{QStringLiteral("schema_version"), 3}, {QStringLiteral("event_id"), QStringLiteral("00000000-0000-4000-8000-0000000000d1")},
                                    {QStringLiteral("card_id"), answered}, {QStringLiteral("rating"), QStringLiteral("custom_interval")},
                                    {QStringLiteral("review_tags"), QJsonArray{QStringLiteral("Recalled")}},
                                    {QStringLiteral("reviewed_at"), now.addDays(-10).toString(Qt::ISODateWithMs)},
                                    {QStringLiteral("scheduled_due_at"), now.addDays(-5).toString(Qt::ISODateWithMs)}}));
    ReviewStore store(temporary.path());
    const auto cardOf = [&store](const QString &id) {
        const auto cards = store.cards();
        return *std::find_if(cards.cbegin(), cards.cend(), [&id](const ReviewCard &card) { return card.id() == id; });
    };
    const auto dueIds = [&store, &now] {
        QStringList ids;
        for (const auto &card : store.dueCards(now)) ids.append(card.id());
        ids.sort();
        return ids;
    };
    const ReviewSchedule before = cardOf(answered).schedule;
    QCOMPARE(before.dueAt, now.addDays(-5));
    QCOMPARE(dueIds(), (QStringList{answered, other}));

    QString error, retireId;
    QVERIFY2(store.retireCard(answered, &error, now, &retireId), qPrintable(error));
    // One queue action, no rating: a random UUIDv4, the card, the time, this device.
    QCOMPARE(QUuid(retireId).version(), QUuid::Random);
    QCOMPARE(retireId, retireId.toLower());
    const QJsonObject retire = readObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular/%1.json").arg(retireId)));
    QCOMPARE(retire.value(QStringLiteral("schema_version")).toInt(), 3);
    QCOMPARE(retire.value(QStringLiteral("event_id")).toString(), retireId);
    QCOMPARE(retire.value(QStringLiteral("card_id")).toString(), answered);
    QCOMPARE(retire.value(QStringLiteral("queue_action")).toString(), QStringLiteral("retire"));
    QCOMPARE(retire.value(QStringLiteral("reviewed_at")).toString(), QStringLiteral("2026-10-27T14:02:11.204Z"));
    QVERIFY(!retire.value(QStringLiteral("origin_id")).toString().isEmpty());
    for (const auto &field : {QStringLiteral("rating"), QStringLiteral("review_tags"), QStringLiteral("skip_id"), QStringLiteral("skip_until"),
                              QStringLiteral("interval_action"), QStringLiteral("retracted_event_id"), QStringLiteral("undone_event_id")})
        QVERIFY2(!retire.contains(field), qPrintable(field));
    QVERIFY(!QDir(QDir(temporary.path()).filePath(QStringLiteral("reviews/queue-actions"))).entryList({QStringLiteral("*.json")}, QDir::Files).size());

    // Retired: the schedule is kept, but the card is never offered.
    ReviewCard retired = cardOf(answered);
    QVERIFY(retired.schedule.retired);
    QCOMPARE(retired.schedule.retiredAt, now);
    QCOMPARE(retired.schedule.retireEventId, retireId);
    QCOMPARE(retired.schedule.dueAt, before.dueAt);
    QCOMPARE(retired.schedule.intervalDays, before.intervalDays);
    QCOMPARE(retired.schedule.reviewCount, before.reviewCount);
    QCOMPARE(retired.currentTags(), QStringList{QStringLiteral("Recalled")});
    QVERIFY(!ReviewStore::isDueNow(retired, now));
    QCOMPARE(dueIds(), QStringList{other});
    QCOMPARE(store.dueCards(now, QStringLiteral("paper")).size(), 1);
    const QList<ReviewCard> active = store.activeCards();
    QVERIFY(std::none_of(active.cbegin(), active.cend(), [&](const ReviewCard &card) { return card.id() == answered; }));
    const auto browser = store.browserCards(now);
    const auto item = std::find_if(browser.cbegin(), browser.cend(), [&](const ReviewBrowserCard &card) { return card.card.id() == answered; });
    QVERIFY(item != browser.cend());
    QCOMPARE(item->state, ReviewBrowserState::Retired);
    QVERIFY(!item->canReview());
    QVERIFY(!item->hasIssues);
    QVERIFY(item->matchesFilter(QStringLiteral("retired")));
    QVERIFY(item->matchesFilter(QStringLiteral("all")));
    for (const auto &filter : {QStringLiteral("new"), QStringLiteral("seen"), QStringLiteral("never"), QStringLiteral("unavailable")})
        QVERIFY2(!item->matchesFilter(filter), qPrintable(filter));
    QVERIFY(!std::find_if(browser.cbegin(), browser.cend(), [&](const ReviewBrowserCard &card) { return card.card.id() == other; })->matchesFilter(QStringLiteral("retired")));
    // Skip all due: a retired card is no target, nor can it be skipped itself.
    ReviewSkipBatch batch;
    QVERIFY(!store.skip(answered, ReviewSkipScope::Card, now.addDays(1), &batch, &error, now));
    QVERIFY2(store.skip(other, ReviewSkipScope::Document, now.addDays(1), &batch, &error, now, true), qPrintable(error));
    QCOMPARE(batch.cardIds, QStringList{other});
    QVERIFY2(store.undoSkip(batch.id, &error, now.addSecs(1)), qPrintable(error));

    // Restore to review: the old schedule; its due date is past, so it is due now.
    QString restoreId;
    QVERIFY2(store.restoreRetiredCard(answered, &error, now.addSecs(60), &restoreId), qPrintable(error));
    const QJsonObject restore = readObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular/%1.json").arg(restoreId)));
    QCOMPARE(restore.value(QStringLiteral("queue_action")).toString(), QStringLiteral("restore"));
    QVERIFY(!restore.contains(QStringLiteral("rating")));
    const ReviewCard restored = cardOf(answered);
    QVERIFY(!restored.schedule.retired);
    QVERIFY(!restored.schedule.retiredAt.isValid());
    QCOMPARE(restored.schedule.dueAt, before.dueAt);
    QCOMPARE(restored.schedule.intervalDays, before.intervalDays);
    QCOMPARE(restored.schedule.reviewCount, before.reviewCount);
    QVERIFY(ReviewStore::isDueNow(restored, now.addSecs(61)));
    QCOMPARE(dueIds(), (QStringList{answered, other}));

    // A clock behind the card's last action still records after it, so the newest action wins.
    QVERIFY2(store.retireCard(answered, &error, now.addSecs(-3600), &retireId), qPrintable(error));
    QCOMPARE(readObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular/%1.json").arg(retireId))).value(QStringLiteral("reviewed_at")).toString(),
             QStringLiteral("2026-10-27T14:03:11.205Z"));
    QVERIFY(cardOf(answered).schedule.retired);

    // Deletion wins and is final; a deleted card cannot be retired or restored.
    QVERIFY2(store.deleteCard(answered, now.addSecs(120), &error), qPrintable(error));
    QVERIFY(cardOf(answered).schedule.deleted);
    QVERIFY(!cardOf(answered).schedule.retired);
    QVERIFY(!store.restoreRetiredCard(answered, &error, now.addSecs(180)));
    QVERIFY(!store.retireCard(answered, &error, now.addSecs(180)));
    QVERIFY(!store.retireCard(QStringLiteral("00000000-0000-4000-8000-0000000000ff"), &error, now));
}

void ReviewStoreTest::retireUndoAndIndex()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDateTime now(QDate(2026, 10, 27), QTime(9, 0), QTimeZone::UTC);
    const QString cardId = QStringLiteral("00000000-0000-4000-8000-0000000000e1");
    QVERIFY(writeObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/cards/%1.json").arg(cardId)),
                        reviewCard(cardId, QStringLiteral("paper"), QString(64, QLatin1Char('a')))));
    ReviewStore store(temporary.path());
    QString error, retireId;
    QVERIFY2(store.retireCard(cardId, &error, now, &retireId), qPrintable(error));
    QVERIFY(store.cards().constFirst().schedule.retired);

    // Undo writes undo_retire naming the retire; the card is active again.
    QVERIFY2(store.undoRetire(retireId, &error, now.addSecs(5)), qPrintable(error));
    QJsonObject undo;
    for (const QString &name : QDir(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular"))).entryList({QStringLiteral("*.json")}, QDir::Files)) {
        const QJsonObject event = readObject(QDir(temporary.path()).filePath(QStringLiteral("reviews/events/mac-okular/") + name));
        if (event.value(QStringLiteral("queue_action")).toString() == QLatin1String("undo_retire")) undo = event;
    }
    QCOMPARE(undo.value(QStringLiteral("undone_event_id")).toString(), retireId);
    QCOMPARE(undo.value(QStringLiteral("card_id")).toString(), cardId);
    QCOMPARE(undo.value(QStringLiteral("schema_version")).toInt(), 3);
    QVERIFY(!undo.contains(QStringLiteral("rating")));
    QVERIFY(!store.cards().constFirst().schedule.retired);
    QCOMPARE(store.dueCards(now.addSecs(6)).size(), 1);
    // Only a retire or a restore can be undone.
    QVERIFY(!store.undoRetire(undo.value(QStringLiteral("event_id")).toString(), &error, now.addSecs(7)));
    QVERIFY(!store.undoRetire(QStringLiteral("00000000-0000-4000-8000-0000000000ff"), &error, now.addSecs(7)));

    // The review index keeps the retirement next to the events (index version 5).
    QVERIFY2(store.retireCard(cardId, &error, now.addSecs(10), &retireId), qPrintable(error));
    const QString path = temporary.filePath(QStringLiteral("server/review-query.sqlite"));
    QVERIFY2(ReviewStore(temporary.path(), {}, {}, true).refreshQueryDatabase(path, &error), qPrintable(error));
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("retire-index"));
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("SELECT card_id, retired_at, retire_event_id FROM review_card_retirements")));
        QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(), cardId);
        QCOMPARE(query.value(1).toString(), QStringLiteral("2026-10-27T09:00:10.000Z"));
        QCOMPARE(query.value(2).toString(), retireId);
        QVERIFY(!query.next());
        QVERIFY(query.exec(QStringLiteral("SELECT queue_action, rating FROM review_events ORDER BY reviewed_at")));
        QStringList actions;
        while (query.next()) {
            QVERIFY(query.value(1).toString().isEmpty());
            actions.append(query.value(0).toString());
        }
        QCOMPARE(actions, (QStringList{QStringLiteral("retire"), QStringLiteral("undo_retire"), QStringLiteral("retire")}));
        QVERIFY(query.exec(QStringLiteral("SELECT value FROM review_meta WHERE key='source_hash'")));
        QVERIFY(query.next());
        QVERIFY(query.value(0).toString().startsWith(QStringLiteral("6:")));
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("retire-index"));

    // list-review-cards says the card is retired.
    const auto listed = store.sourceCardCommand({{QStringLiteral("command"), QStringLiteral("list-review-cards")}, {QStringLiteral("document"), QStringLiteral("paper.pdf")}},
                                             temporary.filePath(QStringLiteral("bundle")));
    QVERIFY2(listed.value(QStringLiteral("accepted")).toBool(), QJsonDocument(listed).toJson().constData());
    const QJsonObject listedCard = listed.value(QStringLiteral("cards")).toArray().at(0).toObject();
    QVERIFY(listedCard.value(QStringLiteral("retired")).toBool());
    QCOMPARE(listedCard.value(QStringLiteral("retired_at")).toString(), QStringLiteral("2026-10-27T09:00:10.000Z"));
}
