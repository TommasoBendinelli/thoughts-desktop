/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/reviewknowledge.h"
#include "../shell/reviewstore.h"
#include "../part/documentfeedback.h"
#include "../part/reviewcardfeedback.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QMap>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

using namespace Thoughts;
using namespace Thoughts::ReviewKnowledge;

namespace
{
const QDateTime now(QDate(2026, 10, 12), QTime(12, 0), QTimeZone::UTC);
const QString grpo = QStringLiteral("00000000-0000-4000-8000-0000000000a1");
const QString baseline = QStringLiteral("00000000-0000-4000-8000-0000000000a2");
const QString tensor = QStringLiteral("00000000-0000-4000-8000-0000000000a3");
const QString deletedRecord = QStringLiteral("00000000-0000-4000-8000-0000000000a4");
const QString deletedByEvent = QStringLiteral("00000000-0000-4000-8000-0000000000a5");
const QString ppo = QStringLiteral("00000000-0000-4000-8000-0000000000a6");
const QString kl = QStringLiteral("00000000-0000-4000-8000-0000000000a7");

QString iso(const QDateTime &value)
{
    return value.toUTC().toString(Qt::ISODateWithMs);
}

QJsonObject card(const QString &id, const QString &pdfStem, const QString &question, const QString &sourceFile = {}, int line = 0)
{
    QJsonObject primary{{QStringLiteral("pdf_stem"), pdfStem}, {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                        {QStringLiteral("page"), 1}, {QStringLiteral("rect"), QJsonArray{0.1, 0.2, 0.8, 0.6}},
                        {QStringLiteral("page_visual_sha256"), QString(64, QLatin1Char('b'))}, {QStringLiteral("question"), question},
                        {QStringLiteral("document_id"), QString(QStringLiteral("doc-") + pdfStem)}};
    if (!sourceFile.isEmpty()) {
        primary.insert(QStringLiteral("source_anchor"), QJsonObject{{QStringLiteral("version"), 2}, {QStringLiteral("segments"), QJsonArray{QJsonObject{
            {QStringLiteral("segment_id"), QStringLiteral("0")}, {QStringLiteral("source_file"), sourceFile},
            {QStringLiteral("line_start"), line}, {QStringLiteral("line_end"), line}}}}});
    }
    return {{QStringLiteral("schema_version"), 3}, {QStringLiteral("card_type"), QStringLiteral("visual_region")}, {QStringLiteral("card_id"), id},
            {QStringLiteral("status"), QStringLiteral("approved")}, {QStringLiteral("revision"), 1}, {QStringLiteral("primary"), primary},
            {QStringLiteral("initial_due_at"), iso(now.addDays(5))}};
}

QJsonObject answer(const QString &eventId, const QString &cardId, int daysAgo, double intervalDays, const QJsonValue &tags,
                   const QJsonObject &notes = {}, const QString &action = {})
{
    const QDateTime at = now.addDays(-daysAgo);
    QJsonObject event{{QStringLiteral("event_id"), eventId}, {QStringLiteral("card_id"), cardId}, {QStringLiteral("origin_id"), QStringLiteral("test")},
                      {QStringLiteral("reviewed_at"), iso(at)}, {QStringLiteral("rating"), QStringLiteral("custom_interval")},
                      {QStringLiteral("scheduler_version"), 2}, {QStringLiteral("schema_version"), 1},
                      {QStringLiteral("scheduled_due_at"), iso(at.addMSecs(qint64(intervalDays * 86400000)))}};
    if (!tags.isUndefined()) event.insert(QStringLiteral("review_tags"), tags);
    if (!notes.isEmpty()) event.insert(QStringLiteral("review_group_notes"), notes);
    if (!action.isEmpty()) event.insert(QStringLiteral("review_action"), action);
    return event;
}

bool write(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

// A Library project as Thoughts stores it: the catalogue plus the project's own configuration.
void writeProjects(const QString &root, const QList<QPair<QString, QStringList>> &projects)
{
    QJsonArray catalogue;
    for (const auto &[name, stems] : projects) {
        const QString id = QStringLiteral("source:docs-") + name.toLower();
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex());
        const QString folder = QStringLiteral("00000000-0000-4000-9000-%1").arg(catalogue.size() + 1, 12, 10, QLatin1Char('0'));
        QJsonArray declarations, documents;
        int index = 0;
        for (const QString &stem : stems) {
            const QString documentId = QStringLiteral("00000000-0000-4000-a%1-%2").arg(catalogue.size() + 1, 3, 10, QLatin1Char('0')).arg(++index, 12, 10, QLatin1Char('0'));
            declarations.append(QJsonObject{{QStringLiteral("document_id"), documentId}, {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), true},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), QString(stem + QStringLiteral(".pdf"))}}}});
            documents.append(QJsonObject{{QStringLiteral("document_id"), documentId}, {QStringLiteral("filename"), QString(stem + QStringLiteral(".pdf"))},
                {QStringLiteral("name"), stem == QLatin1String("rl_grpo") ? QStringLiteral("GRPO notes") : stem}});
        }
        const QJsonObject configuration{{QStringLiteral("schema_version"), 5}, {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
            {QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("default_folder_id"), folder},
            {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("folder_id"), folder},
                {QStringLiteral("path"), QStringLiteral(".thoughts/projects/%1/documents").arg(hash)},
                {QStringLiteral("document_id"), QStringLiteral("00000000-0000-4000-b000-%1").arg(catalogue.size() + 1, 12, 10, QLatin1Char('0'))}}}},
            {QStringLiteral("documents"), declarations}};
        QVERIFY(write(QDir(root).filePath(QStringLiteral(".thoughts/projects/%1/documents/thoughts-project.json").arg(hash)), QJsonDocument(configuration).toJson()));
        catalogue.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("revision"), 1},
            {QStringLiteral("documents"), documents}, {QStringLiteral("sources"), QJsonArray{}}});
    }
    QVERIFY(write(QDir(root).filePath(QStringLiteral(".thoughts/library-projects.json")),
                  QJsonDocument(QJsonObject{{QStringLiteral("projects"), catalogue}, {QStringLiteral("pending"), QJsonArray{}}}).toJson()));
}

QString longParallelText()
{
    QString text;
    for (int i = 0; i < 40; ++i) text += QStringLiteral("Tensor parallel layers split each weight matrix across devices. ");
    return text + QStringLiteral("Every layer ends with an allreduce of the partial outputs.");
}

// The Library: two projects, a TeX and a Markdown source with markers, and the projections.
QJsonObject fixture(const QString &docs)
{
    writeProjects(docs, {{QStringLiteral("RL"), {QStringLiteral("rl_grpo"), QStringLiteral("rl_ppo")}}, {QStringLiteral("Systems"), {QStringLiteral("sys_parallel")}}});
    write(QDir(docs).filePath(QStringLiteral("rl/grpo.tex")),
          QStringLiteral("\\section{GRPO}\n\\ReviewCardMaskStart{%1}{0}%\nThe advantage normalizes each reward by the group mean and standard deviation.\n\\ReviewCardMaskEnd{%1}{0}%\n")
              .arg(grpo).toUtf8());
    write(QDir(docs).filePath(QStringLiteral("sys/parallel.md")),
          QStringLiteral("# Parallelism\n<!-- thoughts:v1:start review:%1:answer:0 -->\n%2\n<!-- thoughts:v1:end review:%1:answer:0 -->\n")
              .arg(tensor, longParallelText()).toUtf8());

    QJsonObject projections;
    const auto add = [&](const QString &prefix, const QString &id, const QJsonObject &record) { projections.insert(prefix + id, record); };
    const QString cards = QStringLiteral("review_card\ncards\n"), events = QStringLiteral("review_event\nevents\n");
    add(cards, grpo, card(grpo, QStringLiteral("rl_grpo"), QStringLiteral("How does GRPO compute the group-relative advantage?"), QStringLiteral("rl/grpo.tex"), 3));
    auto baselineCard = card(baseline, QStringLiteral("rl_grpo"), QStringLiteral("Why is a baseline needed?"));
    baselineCard.insert(QStringLiteral("note"), QStringLiteral("Compare with the GRPO advantage"));
    baselineCard.insert(QStringLiteral("initial_due_at"), iso(now.addDays(-3)));
    auto primary = baselineCard.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("answer_anchor"), QJsonObject{{QStringLiteral("normalized_text"), QStringLiteral("a baseline reduces the variance of the gradient")}});
    baselineCard.insert(QStringLiteral("primary"), primary);
    add(cards, baseline, baselineCard);
    add(cards, tensor, card(tensor, QStringLiteral("sys_parallel"), QStringLiteral("What does tensor parallelism split?"), QStringLiteral("sys/parallel.md"), 3));
    auto deleted = card(deletedRecord, QStringLiteral("rl_grpo"), QStringLiteral("GRPO card deleted in its record"));
    deleted.insert(QStringLiteral("deleted"), true);
    add(cards, deletedRecord, deleted);
    add(cards, deletedByEvent, card(deletedByEvent, QStringLiteral("rl_grpo"), QStringLiteral("GRPO card deleted by an event")));
    add(cards, ppo, card(ppo, QStringLiteral("rl_ppo"), QStringLiteral("What does the PPO clipping ratio bound?")));
    add(cards, kl, card(kl, QStringLiteral("rl_ppo"), QStringLiteral("Why add a KL penalty to the reward?")));

    // GRPO: half-known, then known under a tag merged into Memorized.
    add(events, QStringLiteral("e1"), answer(QStringLiteral("e1"), grpo, 30, 5, QJsonArray{QStringLiteral("Kind of memorized"), QStringLiteral("interesting")},
                                            QJsonObject{{QStringLiteral("g-understanding"), QStringLiteral("almost")},
                                                        {QStringLiteral("note"), QStringLiteral("mixed up the baseline")}}));
    add(events, QStringLiteral("e2"), answer(QStringLiteral("e2"), grpo, 3, 25, QJsonArray{QStringLiteral("Got it")}));
    // Tensor parallelism: right long ago (legacy, no tags), now wrong.
    add(events, QStringLiteral("e3"), answer(QStringLiteral("e3"), tensor, 40, 7, QJsonValue::Undefined, {}, QStringLiteral("correct")));
    add(events, QStringLiteral("e4"), answer(QStringLiteral("e4"), tensor, 1, 0.9, QJsonArray{QStringLiteral("wrong")}));
    // PPO: answered without a tag, with a long interval.
    add(events, QStringLiteral("e5"), answer(QStringLiteral("e5"), ppo, 5, 40, QJsonArray{}));
    // KL: half understood.
    add(events, QStringLiteral("e6"), answer(QStringLiteral("e6"), kl, 4, 7, QJsonArray{QStringLiteral("I kind of get it")}));
    add(events, QStringLiteral("e7"), QJsonObject{{QStringLiteral("event_id"), QStringLiteral("e7")}, {QStringLiteral("card_id"), deletedByEvent},
        {QStringLiteral("origin_id"), QStringLiteral("test")}, {QStringLiteral("reviewed_at"), iso(now.addDays(-1))}, {QStringLiteral("rating"), QStringLiteral("delete")}});

    projections.insert(QStringLiteral("review_tag_merge\nreview-tags\nm1"), QJsonObject{{QStringLiteral("merge_id"), QStringLiteral("m1")},
        {QStringLiteral("from"), QJsonArray{QStringLiteral("Got it")}}, {QStringLiteral("to"), QStringLiteral("Memorized")},
        {QStringLiteral("created_at"), QStringLiteral("2026-10-01T00:00:00Z")}, {QStringLiteral("origin_id"), QStringLiteral("test")}});
    projections.insert(QStringLiteral("review_tag_groups\nreview-tags\ngroups"), QJsonObject{{QStringLiteral("layout_id"), QStringLiteral("groups")},
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("updated_at"), QStringLiteral("2026-10-01T00:00:00.000Z")}, {QStringLiteral("origin_id"), QStringLiteral("test")},
        {QStringLiteral("groups"), QJsonArray{
            QJsonObject{{QStringLiteral("group_id"), QStringLiteral("g-understanding")}, {QStringLiteral("name"), QStringLiteral("Understanding")},
                {QStringLiteral("tags"), QJsonArray{QStringLiteral("correct"), QStringLiteral("wrong"), QStringLiteral("Memorized"), QStringLiteral("Kind of memorized"),
                                                    QStringLiteral("I kind of get it"), QStringLiteral("Totally new tag")}}},
            QJsonObject{{QStringLiteral("group_id"), QStringLiteral("g-interest")}, {QStringLiteral("name"), QStringLiteral("Interest")},
                {QStringLiteral("tags"), QJsonArray{QStringLiteral("interesting")}}}}}});
    return projections;
}

QStringList ids(const QJsonObject &result)
{
    QStringList list;
    for (const auto &value : result.value(QStringLiteral("cards")).toArray()) list.append(value.toObject().value(QStringLiteral("card_id")).toString());
    return list;
}

QStringList strings(const QJsonValue &value)
{
    QStringList list;
    for (const auto &item : value.toArray()) list.append(item.toString());
    return list;
}

// Every file below @p root with its size, time and content hash.
QMap<QString, QString> filesBelow(const QString &root)
{
    QMap<QString, QString> files;
    QDirIterator iterator(root, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    while (iterator.hasNext()) {
        const QString path = iterator.next();
        QFile file(path);
        QByteArray hash;
        if (file.open(QIODevice::ReadOnly)) hash = QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256).toHex();
        files.insert(path, QStringLiteral("%1 %2 %3").arg(QFileInfo(path).size()).arg(QFileInfo(path).lastModified().toMSecsSinceEpoch()).arg(QString::fromLatin1(hash)));
    }
    return files;
}
}

class ReviewKnowledgeTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void searchRanksMatchesAndExcludesDeletedCards();
    void searchFilters();
    void searchRejectsBadRequests();
    void profileClassifiesByTagsRecencyAndInterval();
    void levelsFileIsOnlyWrittenOnRequest();
    void defaultLevelsReadTheSliderSteps();
    void levelsFileFollowsATagReorganization();
    void readingTheIndexWritesNothing();
    void readingTheIndexUsesAnExistingLog();
    void cardsCarryTheirAnswerFeedback();
    void cardsShareTheAnswerFeedbackOfTheirDocument();
    void retiredCardsAreReportedButNeverDue();
    void historyCarriesTheBeforeStep();
    void fullAnswerIsTheWholePassage();
    void documentFeedbackComesWithItsPage();
};

void ReviewKnowledgeTest::searchRanksMatchesAndExcludesDeletedCards()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    const Library library = libraryFromSnapshot({fixture(docs), QStringLiteral("2026-10-12T11:00:00.000Z"), true}, docs);
    QCOMPARE(library.cards.size(), 5);

    const auto result = search(library, {{QStringLiteral("query"), QStringLiteral("GRPO advantage")}}, now);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    // Both words in the question beat a mention in the note; deleted cards never appear.
    QCOMPARE(ids(result), (QStringList{grpo, baseline}));
    QCOMPARE(result.value(QStringLiteral("matched")).toInt(), 2);
    QCOMPARE(strings(result.value(QStringLiteral("query_words"))), (QStringList{QStringLiteral("grpo"), QStringLiteral("advantage")}));

    const auto first = result.value(QStringLiteral("cards")).toArray().at(0).toObject();
    QCOMPARE(first.value(QStringLiteral("project")).toString(), QStringLiteral("RL"));
    QCOMPARE(first.value(QStringLiteral("document")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("GRPO notes"));
    QCOMPARE(first.value(QStringLiteral("document")).toObject().value(QStringLiteral("filename")).toString(), QStringLiteral("rl_grpo.pdf"));
    const auto source = first.value(QStringLiteral("source")).toObject();
    QCOMPARE(source.value(QStringLiteral("path")).toString(), QDir(docs).filePath(QStringLiteral("rl/grpo.tex")));
    QCOMPARE(source.value(QStringLiteral("lines")).toArray(), (QJsonArray{QJsonValue(QJsonArray{3, 3})}));
    QVERIFY(source.value(QStringLiteral("markers_found")).toBool());
    // "Got it" was merged into Memorized.
    QCOMPARE(strings(first.value(QStringLiteral("current_tags"))), QStringList{QStringLiteral("Memorized")});
    QCOMPARE(first.value(QStringLiteral("interval_days")).toInt(), 25);
    QCOMPARE(first.value(QStringLiteral("review_count")).toInt(), 2);
    QCOMPARE(first.value(QStringLiteral("wrong_count")).toInt(), 0);
    QCOMPARE(first.value(QStringLiteral("status")).toString(), QStringLiteral("scheduled"));
    QCOMPARE(first.value(QStringLiteral("last_reviewed_at")).toString(), iso(now.addDays(-3)));
    const auto answerText = first.value(QStringLiteral("answer")).toObject();
    QCOMPARE(answerText.value(QStringLiteral("text")).toString(), QStringLiteral("The advantage normalizes each reward by the group mean and standard deviation."));
    QCOMPARE(answerText.value(QStringLiteral("from")).toString(), QStringLiteral("source"));
    const auto history = first.value(QStringLiteral("history")).toArray();
    QCOMPARE(history.size(), 2);
    QCOMPARE(history.at(0).toObject().value(QStringLiteral("reviewed_at")).toString(), iso(now.addDays(-3)));
    QCOMPARE(strings(history.at(0).toObject().value(QStringLiteral("tags"))), QStringList{QStringLiteral("Memorized")});
    QCOMPARE(history.at(0).toObject().value(QStringLiteral("interval_days")).toDouble(), 25.0);
    QCOMPARE(strings(history.at(1).toObject().value(QStringLiteral("tags"))), (QStringList{QStringLiteral("Kind of memorized"), QStringLiteral("interesting")}));
    QCOMPARE(history.at(1).toObject().value(QStringLiteral("group_notes")).toObject(), (QJsonObject{{QStringLiteral("Understanding"), QStringLiteral("almost")}}));
    // The answer's own note (key "note") is not a group's note.
    QCOMPARE(history.at(1).toObject().value(QStringLiteral("note")).toString(), QStringLiteral("mixed up the baseline"));
    QVERIFY(!history.at(0).toObject().contains(QStringLiteral("note")));

    // Without a source passage or PDF the answer comes from the card's answer anchor.
    const auto second = result.value(QStringLiteral("cards")).toArray().at(1).toObject();
    QCOMPARE(second.value(QStringLiteral("note")).toString(), QStringLiteral("Compare with the GRPO advantage"));
    QCOMPARE(second.value(QStringLiteral("answer")).toObject().value(QStringLiteral("from")).toString(), QStringLiteral("answer_anchor"));
    QCOMPARE(second.value(QStringLiteral("status")).toString(), QStringLiteral("overdue"));
    QCOMPARE(second.value(QStringLiteral("overdue_days")).toInt(), 3);
    QVERIFY(!second.value(QStringLiteral("answered")).toBool());
    QVERIFY(second.value(QStringLiteral("source")).isNull());

    // A long answer is cut to an excerpt around the word searched for.
    const auto parallel = search(library, {{QStringLiteral("query"), QStringLiteral("allreduce")}}, now);
    QCOMPARE(ids(parallel), QStringList{tensor});
    const auto excerpt = parallel.value(QStringLiteral("cards")).toArray().at(0).toObject().value(QStringLiteral("answer")).toObject();
    QVERIFY(excerpt.value(QStringLiteral("truncated")).toBool());
    QVERIFY(excerpt.value(QStringLiteral("text")).toString().size() <= 602);
    QVERIFY(excerpt.value(QStringLiteral("text")).toString().startsWith(QStringLiteral("…")));
    QVERIFY(excerpt.value(QStringLiteral("text")).toString().contains(QStringLiteral("allreduce")));
    // A legacy answer without tags counts as Correct; the latest one says Wrong.
    const auto tensorCard = parallel.value(QStringLiteral("cards")).toArray().at(0).toObject();
    QCOMPARE(strings(tensorCard.value(QStringLiteral("current_tags"))), QStringList{QStringLiteral("Wrong")});
    QCOMPARE(tensorCard.value(QStringLiteral("wrong_count")).toInt(), 1);
    QCOMPARE(strings(tensorCard.value(QStringLiteral("history")).toArray().at(1).toObject().value(QStringLiteral("tags"))), QStringList{QStringLiteral("Correct")});

    // Plural query words find singular text; limit and the include flags shape the output.
    const auto limited = search(library, {{QStringLiteral("query"), QStringLiteral("advantages")}, {QStringLiteral("limit"), 1},
                                          {QStringLiteral("include_answer"), false}, {QStringLiteral("include_history"), false}}, now);
    QCOMPARE(ids(limited), QStringList{grpo});
    QCOMPARE(limited.value(QStringLiteral("matched")).toInt(), 2);
    QVERIFY(!limited.value(QStringLiteral("cards")).toArray().at(0).toObject().contains(QStringLiteral("answer")));
    QVERIFY(!limited.value(QStringLiteral("cards")).toArray().at(0).toObject().contains(QStringLiteral("history")));
    // Names match too: the project.
    QCOMPARE(search(library, {{QStringLiteral("query"), QStringLiteral("systems")}}, now).value(QStringLiteral("matched")).toInt(), 1);
    QCOMPARE(search(library, {{QStringLiteral("query"), QStringLiteral("deleted")}}, now).value(QStringLiteral("matched")).toInt(), 0);
    QCOMPARE(result.value(QStringLiteral("tag_groups")).toArray().size(), 2);
}

void ReviewKnowledgeTest::searchFilters()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    const Library library = libraryFromSnapshot({fixture(docs), {}, true}, docs);
    const auto found = [&](const QJsonObject &request) {
        auto list = ids(search(library, request, now));
        list.sort();
        return list;
    };
    const auto sorted = [](QStringList list) { list.sort(); return list; };
    QCOMPARE(found({}), sorted({grpo, baseline, tensor, ppo, kl}));
    QCOMPARE(found({{QStringLiteral("tags"), QJsonArray{QStringLiteral("memorized")}}}), QStringList{grpo});
    // An old tag name finds the tag it was merged into; "Wrong" is the built-in tag.
    QCOMPARE(found({{QStringLiteral("tags"), QJsonArray{QStringLiteral("got it"), QStringLiteral("Wrong")}}}), sorted({grpo, tensor}));
    QCOMPARE(found({{QStringLiteral("tag_groups"), QJsonArray{QStringLiteral("understanding")}}}), sorted({grpo, tensor, kl}));
    QCOMPARE(found({{QStringLiteral("tag_groups"), QJsonArray{QStringLiteral("Interest")}}}), QStringList{});
    QCOMPARE(found({{QStringLiteral("project"), QStringLiteral("systems")}}), QStringList{tensor});
    QCOMPARE(found({{QStringLiteral("document"), QStringLiteral("GRPO notes")}}), sorted({grpo, baseline}));
    QCOMPARE(found({{QStringLiteral("document"), QStringLiteral("rl_grpo.pdf")}}), sorted({grpo, baseline}));
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("new")}}), QStringList{baseline});
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("overdue")}}), QStringList{baseline});
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("due")}}), sorted({baseline, tensor}));
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("untagged_last")}}), QStringList{ppo});
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("reviewed")}}), sorted({grpo, tensor, ppo, kl}));
    QCOMPARE(ids(search(library, {{QStringLiteral("sort"), QStringLiteral("last_reviewed")}}, now)), (QStringList{tensor, grpo, kl, ppo, baseline}));
    QCOMPARE(ids(search(library, {{QStringLiteral("sort"), QStringLiteral("due")}}, now)).first(), baseline);
}

void ReviewKnowledgeTest::searchRejectsBadRequests()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    const Library library = libraryFromSnapshot({fixture(docs), {}, true}, docs);
    const auto code = [&](const QJsonObject &request) { return search(library, request, now).value(QStringLiteral("error_code")).toString(); };
    QCOMPARE(code({{QStringLiteral("project"), QStringLiteral("Nope")}}), QStringLiteral("unknown_project"));
    QCOMPARE(strings(search(library, {{QStringLiteral("project"), QStringLiteral("Nope")}}, now).value(QStringLiteral("projects"))),
             (QStringList{QStringLiteral("RL"), QStringLiteral("Systems")}));
    QCOMPARE(code({{QStringLiteral("document"), QStringLiteral("nope.pdf")}}), QStringLiteral("unknown_document"));
    QCOMPARE(code({{QStringLiteral("tag_groups"), QJsonArray{QStringLiteral("Nope")}}}), QStringLiteral("unknown_tag_group"));
    QCOMPARE(code({{QStringLiteral("state"), QStringLiteral("forgotten")}}), QStringLiteral("invalid_request"));
    QCOMPARE(code({{QStringLiteral("sort"), QStringLiteral("random")}}), QStringLiteral("invalid_request"));
    QCOMPARE(code({{QStringLiteral("limit"), 0}}), QStringLiteral("invalid_request"));
    QCOMPARE(code({{QStringLiteral("limit"), 201}}), QStringLiteral("invalid_request"));
    QCOMPARE(code({{QStringLiteral("limit"), 1.5}}), QStringLiteral("invalid_request"));
    QCOMPARE(code({{QStringLiteral("query"), 3}}), QStringLiteral("invalid_request"));
    QCOMPARE(code({{QStringLiteral("include_answer"), QStringLiteral("yes")}}), QStringLiteral("invalid_request"));
    QCOMPARE(code({{QStringLiteral("cards"), true}}), QStringLiteral("invalid_request"));
    QVERIFY(!search(library, {{QStringLiteral("cards"), true}}, now).value(QStringLiteral("accepted")).toBool());
    QCOMPARE(profile(library, {}, defaultLevels(), now).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
    // Before Thoughts stores tag groups in the index, a group filter says so.
    const Library older = libraryFromSnapshot({QJsonObject{}, {}, false}, docs);
    QVERIFY(search(older, {{QStringLiteral("tag_groups"), QJsonArray{QStringLiteral("Understanding")}}}, now)
                .value(QStringLiteral("message")).toString().contains(QStringLiteral("No tag groups are known yet")));
}

void ReviewKnowledgeTest::profileClassifiesByTagsRecencyAndInterval()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    const Library library = libraryFromSnapshot({fixture(docs), {}, true}, docs);
    // The mapping of the tags before the 2026-10-15 reorganization (the default until then).
    const Levels legacy{{{QStringLiteral("correct"), QStringLiteral("solid")}, {QStringLiteral("Memorized"), QStringLiteral("solid")},
                         {QStringLiteral("Kind of memorized"), QStringLiteral("shaky")}, {QStringLiteral("I kind of get it"), QStringLiteral("shaky")},
                         {QStringLiteral("wrong"), QStringLiteral("unknown")}},
                        QStringLiteral("legacy mapping")};
    // The project name matches every RL card.
    const auto result = profile(library, {{QStringLiteral("topic"), QStringLiteral("RL")}}, legacy, now);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("counts")).toObject(), (QJsonObject{{QStringLiteral("solid"), 2}, {QStringLiteral("shaky"), 1},
                                                                            {QStringLiteral("unknown"), 0}, {QStringLiteral("unreviewed"), 1}}));
    const auto cards = result.value(QStringLiteral("cards")).toObject();
    const auto reasonOf = [&](const QString &level, const QString &id) {
        for (const auto &value : cards.value(level).toArray())
            if (value.toObject().value(QStringLiteral("card_id")).toString() == id) return value.toObject().value(QStringLiteral("reason")).toString();
        return QStringLiteral("<missing>");
    };
    // Memorized now, Kind of memorized before: the latest answer weighs most.
    QCOMPARE(reasonOf(QStringLiteral("solid"), grpo), QStringLiteral("last 2 answers: Memorized, Kind of memorized; interval 25 d"));
    // No understanding tag, but a long interval.
    QCOMPARE(reasonOf(QStringLiteral("solid"), ppo), QStringLiteral("1 answer, none with an understanding tag; interval 40 d"));
    QCOMPARE(reasonOf(QStringLiteral("shaky"), kl), QStringLiteral("last answer: I kind of get it; interval 7 d"));
    QCOMPARE(reasonOf(QStringLiteral("unreviewed"), baseline), QStringLiteral("never answered; overdue 3 d"));
    const QStringList summary = result.value(QStringLiteral("summary")).toString().split(QLatin1Char('\n'));
    QCOMPARE(summary.first(), QStringLiteral("\"RL\": 4 matching review cards: 2 solid, 1 shaky, 0 not understood, 1 not yet reviewed."));
    QVERIFY(summary.size() >= 3 && summary.size() <= 5);
    QVERIFY(summary.at(2).startsWith(QStringLiteral("Shaky: Why add a KL penalty")));
    const auto mapping = result.value(QStringLiteral("mapping")).toObject();
    QCOMPARE(mapping.value(QStringLiteral("source")).toString(), QStringLiteral("legacy mapping"));
    QCOMPARE(mapping.value(QStringLiteral("levels")).toObject().value(QStringLiteral("Correct")).toString(), QStringLiteral("solid"));
    QCOMPARE(mapping.value(QStringLiteral("levels")).toObject().value(QStringLiteral("Wrong")).toString(), QStringLiteral("unknown"));
    QCOMPARE(strings(mapping.value(QStringLiteral("unmapped_understanding_tags"))), QStringList{QStringLiteral("Totally new tag")});

    // Wrong now after Correct long ago: not understood.
    const auto tensorProfile = profile(library, {{QStringLiteral("topic"), QStringLiteral("tensor parallelism")}}, legacy, now);
    QCOMPARE(tensorProfile.value(QStringLiteral("counts")).toObject().value(QStringLiteral("unknown")).toInt(), 1);
    QVERIFY(tensorProfile.value(QStringLiteral("cards")).toObject().value(QStringLiteral("unknown")).toArray().at(0).toObject()
                 .value(QStringLiteral("reason")).toString().startsWith(QStringLiteral("last 2 answers: Wrong, Correct")));

    // The user's own levels replace the defaults; the scope narrows the cards.
    Levels custom{{{QStringLiteral("I kind of get it"), QStringLiteral("unknown")}, {QStringLiteral("memorized"), QStringLiteral("ignore")}}, QStringLiteral("/levels.json")};
    const auto customized = profile(library, {{QStringLiteral("topic"), QStringLiteral("RL")}, {QStringLiteral("document"), QStringLiteral("rl_ppo")}}, custom, now);
    QCOMPARE(customized.value(QStringLiteral("counts")).toObject(), (QJsonObject{{QStringLiteral("solid"), 1}, {QStringLiteral("shaky"), 0},
                                                                                {QStringLiteral("unknown"), 1}, {QStringLiteral("unreviewed"), 0}}));
    QCOMPARE(customized.value(QStringLiteral("mapping")).toObject().value(QStringLiteral("source")).toString(), QStringLiteral("/levels.json"));

    const auto nothing = profile(library, {{QStringLiteral("topic"), QStringLiteral("diffusion")}}, legacy, now);
    QVERIFY(nothing.value(QStringLiteral("accepted")).toBool());
    QCOMPARE(nothing.value(QStringLiteral("summary")).toString(), QStringLiteral("No review cards match \"diffusion\": there is no record of what the user knows about it."));
}

void ReviewKnowledgeTest::levelsFileIsOnlyWrittenOnRequest()
{
    QTemporaryDir temporary;
    const QString path = temporary.filePath(QStringLiteral("data/knowledge-tag-levels.json"));
    Levels levels;
    QString error;
    QVERIFY(loadLevels(path, &levels, &error));
    QCOMPARE(levels.source, QStringLiteral("default mapping"));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(!QFileInfo::exists(QFileInfo(path).absolutePath()));
    QVERIFY2(initLevels(path, &error), qPrintable(error));
    QVERIFY(!initLevels(path, &error));
    QVERIFY(error.contains(QStringLiteral("already exists")));
    QVERIFY(loadLevels(path, &levels, &error));
    QCOMPARE(levels.source, path);
    QCOMPARE(levels.tags.size(), defaultLevels().tags.size());
    QVERIFY(write(path, R"({"schema_version": 1, "levels": {"Correct": "great"}})"));
    QVERIFY(!loadLevels(path, &levels, &error));
    QVERIFY(error.contains(QStringLiteral("solid, shaky, unknown, ignore")));
    QVERIFY(write(path, "not json"));
    QVERIFY(!loadLevels(path, &levels, &error));
}

void ReviewKnowledgeTest::defaultLevelsReadTheSliderSteps()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    QJsonObject projections = fixture(docs);
    // The reorganization merged the old tags into the steps of the Understanding and Recall sliders.
    const auto merge = [&projections](const QString &id, const QString &to, const QStringList &from) {
        projections.insert(QStringLiteral("review_tag_merge\nreview-tags\n") + id, QJsonObject{{QStringLiteral("merge_id"), id},
            {QStringLiteral("from"), QJsonArray::fromStringList(from)}, {QStringLiteral("to"), to},
            {QStringLiteral("created_at"), QStringLiteral("2026-10-15T00:00:00Z")}, {QStringLiteral("origin_id"), QStringLiteral("test")}});
    };
    merge(QStringLiteral("r1"), QStringLiteral("Recalled"), {QStringLiteral("correct")});
    merge(QStringLiteral("r2"), QStringLiteral("Forgot"), {QStringLiteral("wrong")});
    merge(QStringLiteral("r3"), QStringLiteral("Know by heart"), {QStringLiteral("Memorized")});
    merge(QStringLiteral("r4"), QStringLiteral("Partly"), {QStringLiteral("I kind of get it")});
    merge(QStringLiteral("r5"), QStringLiteral("Partly recalled"), {QStringLiteral("Kind of memorized")});
    const Library library = libraryFromSnapshot({projections, {}, true}, docs);
    const auto result = profile(library, {{QStringLiteral("topic"), QStringLiteral("RL")}}, defaultLevels(), now);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("counts")).toObject(), (QJsonObject{{QStringLiteral("solid"), 2}, {QStringLiteral("shaky"), 1},
                                                                            {QStringLiteral("unknown"), 0}, {QStringLiteral("unreviewed"), 1}}));
    const QJsonObject levels = result.value(QStringLiteral("mapping")).toObject().value(QStringLiteral("levels")).toObject();
    QCOMPARE(levels.value(QStringLiteral("Know by heart")).toString(), QStringLiteral("solid"));
    QCOMPARE(levels.value(QStringLiteral("Forgot")).toString(), QStringLiteral("unknown"));
    QVERIFY(!levels.contains(QStringLiteral("Correct")));
    // Forgot after Recalled long ago: not understood; the old answers read under the new names.
    const auto tensorResult = profile(library, {{QStringLiteral("topic"), QStringLiteral("tensor parallelism")}}, defaultLevels(), now);
    QCOMPARE(tensorResult.value(QStringLiteral("counts")).toObject().value(QStringLiteral("unknown")).toInt(), 1);
    QVERIFY(tensorResult.value(QStringLiteral("cards")).toObject().value(QStringLiteral("unknown")).toArray().at(0).toObject()
                .value(QStringLiteral("reason")).toString().startsWith(QStringLiteral("last 2 answers: Forgot, Recalled")));
    // An answer tagged with the old built-in still counts as a wrong answer.
    const auto found = search(library, {{QStringLiteral("query"), QStringLiteral("allreduce")}}, now);
    int wrongCount = -1;
    for (const auto &value : found.value(QStringLiteral("cards")).toArray())
        if (value.toObject().value(QStringLiteral("card_id")).toString() == tensor) wrongCount = value.toObject().value(QStringLiteral("wrong_count")).toInt();
    QCOMPARE(wrongCount, 1);
}

void ReviewKnowledgeTest::levelsFileFollowsATagReorganization()
{
    QTemporaryDir temporary;
    const QString path = temporary.filePath(QStringLiteral("knowledge-tag-levels.json"));
    QString backup, error;
    // Without a file nothing is written: the default mapping applies.
    QVERIFY2(updateLevels(path, {{QStringLiteral("Recalled"), QStringLiteral("solid")}}, {QStringLiteral("correct")}, &backup, &error), qPrintable(error));
    QVERIFY(!QFileInfo::exists(path));
    QVERIFY(backup.isEmpty());
    QVERIFY(write(path, R"({"schema_version": 1, "levels": {"Correct": "solid", "Memorized": "solid", "My own": "shaky", "Partly": "solid"}})"));
    QVERIFY2(updateLevels(path, {{QStringLiteral("Partly"), QStringLiteral("shaky")}, {QStringLiteral("Recalled"), QStringLiteral("solid")}},
                          {QStringLiteral("correct"), QStringLiteral("Memorized")}, &backup, &error), qPrintable(error));
    QVERIFY(QFileInfo::exists(backup));
    QVERIFY(backup.startsWith(path + QStringLiteral(".backup-")));
    Levels levels;
    QVERIFY2(loadLevels(path, &levels, &error), qPrintable(error));
    QHash<QString, QString> mapped;
    for (const auto &[tag, level] : levels.tags) mapped.insert(tag, level);
    QCOMPARE(mapped, (QHash<QString, QString>{{QStringLiteral("Partly"), QStringLiteral("shaky")}, {QStringLiteral("Recalled"), QStringLiteral("solid")},
                                             {QStringLiteral("My own"), QStringLiteral("shaky")}}));
    QVERIFY(loadLevels(backup, &levels, &error));
    QCOMPARE(levels.tags.size(), 4);
}

void ReviewKnowledgeTest::readingTheIndexWritesNothing()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    const QJsonObject projections = fixture(docs);
    const QString server = temporary.filePath(QStringLiteral("server"));
    const QString path = QDir(server).filePath(QStringLiteral("review-query.sqlite"));
    QString error;
    {
        ReviewStore store(docs, [projections] { return projections; }, {}, true);
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    }
    QVERIFY(!QFileInfo::exists(path + QStringLiteral("-wal")));
    const auto before = filesBelow(server);
    const auto docsBefore = filesBelow(docs);
    // Nothing can be created there.
    QVERIFY(QFile::setPermissions(server, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
    IndexSnapshot snapshot;
    const bool read = readIndex(path, &snapshot, &error);
    QFile::setPermissions(server, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QVERIFY2(read, qPrintable(error));
    QCOMPARE(filesBelow(server), before);
    QVERIFY(snapshot.hasTagRecords);
    QVERIFY(!snapshot.indexedAt.isEmpty());
    QVERIFY(snapshot.projections.contains(QString(QStringLiteral("review_card\ncards\n") + grpo)));
    QVERIFY(snapshot.projections.contains(QStringLiteral("review_event\nevents\ne2")));
    QVERIFY(snapshot.projections.contains(QStringLiteral("review_tag_merge\nreview-tags\nm1")));
    QVERIFY(snapshot.projections.contains(QStringLiteral("review_tag_groups\nreview-tags\ngroups")));

    // The whole command, as thoughtsctl runs it, gives what the projections give.
    const QString levels = temporary.filePath(QStringLiteral("data/knowledge-tag-levels.json"));
    const auto result = run(QStringLiteral("knowledge-profile"), {{QStringLiteral("topic"), QStringLiteral("GRPO")}}, path, docs, levels, now);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    QCOMPARE(result.value(QStringLiteral("counts")).toObject().value(QStringLiteral("solid")).toInt(), 1);
    QCOMPARE(run(QStringLiteral("search-review-cards"), {{QStringLiteral("tags"), QJsonArray{QStringLiteral("Memorized")}}}, path, docs, levels, now)
                 .value(QStringLiteral("returned")).toInt(), 1);
    QCOMPARE(filesBelow(server), before);
    QCOMPARE(filesBelow(docs), docsBefore);
    QVERIFY(!QFileInfo::exists(levels));
    QVERIFY(!QFileInfo::exists(QFileInfo(levels).absolutePath()));

    // A missing index is reported, not created.
    const QString missing = temporary.filePath(QStringLiteral("absent/review-query.sqlite"));
    QString code;
    QVERIFY(!readIndex(missing, &snapshot, &error, &code));
    QCOMPARE(code, QStringLiteral("index_unavailable"));
    QVERIFY(!QFileInfo::exists(QFileInfo(missing).absolutePath()));
}

void ReviewKnowledgeTest::cardsCarryTheirAnswerFeedback()
{
    // C10: every card has `feedback` (§7), through the index as thoughtsctl reads it.
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    QJsonObject projections = fixture(docs);
    const auto feedback = [&](const QString &cardId, const QString &polarity, const QString &quote, const QString &createdAt, const QString &pdfSha256) {
        const QJsonObject anchor = ReviewCardFeedback::anchor(quote, QString(), QString(), QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848"), pdfSha256,
                                                              ReviewCardFeedback::regionsJson({{2, QRectF(QPointF(0.1, 0.2), QPointF(0.5, 0.25))}}), QStringLiteral("0"),
                                                              QString(64, QLatin1Char('c')));
        const QJsonObject record = ReviewCardFeedback::create(cardId, polarity, QStringLiteral("Comment on ") + quote, anchor, QStringLiteral("android-pixel"),
                                                              QDateTime::fromString(createdAt, Qt::ISODateWithMs));
        projections.insert(ReviewCardFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
        return record;
    };
    const QJsonObject later = feedback(grpo, QStringLiteral("negative"), QStringLiteral("standard deviation"), QStringLiteral("2026-10-20T11:00:00.000Z"), QString(64, QLatin1Char('d')));
    const QJsonObject earlier = feedback(grpo, QStringLiteral("positive"), QStringLiteral("group mean"), QStringLiteral("2026-10-20T10:00:00.000Z"), QString(64, QLatin1Char('a')));
    QJsonObject gone = feedback(grpo, QStringLiteral("positive"), QStringLiteral("gone"), QStringLiteral("2026-10-20T09:00:00.000Z"), QString(64, QLatin1Char('a')));
    gone = ReviewCardFeedback::remove(gone, QStringLiteral("mac-okular"));
    projections.insert(ReviewCardFeedback::projectionKey(gone.value(QStringLiteral("feedback_id")).toString()), gone);
    // Feedback of a deleted card is hidden with the card.
    feedback(deletedRecord, QStringLiteral("positive"), QStringLiteral("deleted card"), QStringLiteral("2026-10-20T09:00:00.000Z"), QString(64, QLatin1Char('a')));

    const QString path = temporary.filePath(QStringLiteral("server/review-query.sqlite"));
    QString error;
    {
        ReviewStore store(docs, [projections] { return projections; }, {}, true);
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    }
    const QString levels = temporary.filePath(QStringLiteral("data/knowledge-tag-levels.json"));
    const auto byId = [](const QJsonObject &result, const QString &id) {
        for (const QJsonValue &value : result.value(QStringLiteral("cards")).toArray())
            if (value.toObject().value(QStringLiteral("card_id")).toString() == id) return value.toObject();
        return QJsonObject();
    };
    const auto result = run(QStringLiteral("search-review-cards"), {{QStringLiteral("query"), QStringLiteral("GRPO advantage")}}, path, docs, levels, now);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    const QJsonArray list = byId(result, grpo).value(QStringLiteral("feedback")).toArray();
    QCOMPARE(list.size(), 2);
    QCOMPARE(list.at(0).toObject(), (QJsonObject{{QStringLiteral("feedback_id"), earlier.value(QStringLiteral("feedback_id"))}, {QStringLiteral("polarity"), QStringLiteral("positive")},
                                                 {QStringLiteral("quote"), QStringLiteral("group mean")}, {QStringLiteral("comment"), QStringLiteral("Comment on group mean")},
                                                 {QStringLiteral("created_at"), QStringLiteral("2026-10-20T10:00:00.000Z")},
                                                 {QStringLiteral("updated_at"), QStringLiteral("2026-10-20T10:00:00.000Z")}, {QStringLiteral("origin"), QStringLiteral("android-pixel")},
                                                 {QStringLiteral("anchor_state"), QStringLiteral("exact")}, {QStringLiteral("page"), 2},
                                                 {QStringLiteral("segment_id"), QStringLiteral("0")}, {QStringLiteral("source_changed"), QJsonValue::Null},
                                                 {QStringLiteral("card_id"), grpo}, {QStringLiteral("shared"), false}}));
    QCOMPARE(list.at(1).toObject().value(QStringLiteral("anchor_state")).toString(), QStringLiteral("pdf_changed"));
    QCOMPARE(list.at(1).toObject().value(QStringLiteral("feedback_id")), later.value(QStringLiteral("feedback_id")));
    // Cards without feedback have an empty list.
    QCOMPARE(byId(result, baseline).value(QStringLiteral("feedback")).toArray(), QJsonArray());
    // include_deleted adds the deleted record, with deleted_at.
    const auto withDeleted = run(QStringLiteral("search-review-cards"), {{QStringLiteral("query"), QStringLiteral("GRPO advantage")}, {QStringLiteral("include_deleted"), true}},
                                 path, docs, levels, now);
    const QJsonArray all = byId(withDeleted, grpo).value(QStringLiteral("feedback")).toArray();
    QCOMPARE(all.size(), 3);
    QCOMPARE(all.at(0).toObject().value(QStringLiteral("deleted_at")), gone.value(QStringLiteral("deleted_at")));
    QCOMPARE(run(QStringLiteral("search-review-cards"), {{QStringLiteral("include_deleted"), QStringLiteral("yes")}}, path, docs, levels, now)
                 .value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));

    // An index written before review_feedback existed: no feedback, no error.
    {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("old-index"));
        database.setDatabaseName(path);
        QVERIFY(database.open());
        QSqlQuery query(database);
        QVERIFY(query.exec(QStringLiteral("DROP TABLE review_feedback")));
        query.finish();
        database.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("old-index"));
    const auto old = run(QStringLiteral("search-review-cards"), {{QStringLiteral("query"), QStringLiteral("GRPO advantage")}}, path, docs, levels, now);
    QVERIFY2(old.value(QStringLiteral("accepted")).toBool(), QJsonDocument(old).toJson().constData());
    QCOMPARE(byId(old, grpo).value(QStringLiteral("feedback")).toArray(), QJsonArray());
}

void ReviewKnowledgeTest::cardsShareTheAnswerFeedbackOfTheirDocument()
{
    // Feedback is shared by the cards of a document: a card also lists the other cards' feedback
    // whose passage lies in its answer, with card_id (the card it was made on) and shared: true.
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    QJsonObject projections = fixture(docs);
    const QString document = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848");
    // GRPO and baseline: one document (rl_grpo, PDF 'a…', answer page 1 from 0.2 to 0.6).
    for (const QString &id : {grpo, baseline}) {
        const QString key = QStringLiteral("review_card\ncards\n") + id;
        QJsonObject record = projections.value(key).toObject();
        QJsonObject primary = record.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("pdf_document_id"), document);
        record.insert(QStringLiteral("primary"), primary);
        projections.insert(key, record);
    }
    const auto feedback = [&](const QString &cardId, const QString &quote, const QString &createdAt, const QString &pdfSha256, int page,
                              const QString &documentId = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848")) {
        const QJsonObject anchor = ReviewCardFeedback::anchor(quote, QString(), QString(), documentId, pdfSha256,
                                                              ReviewCardFeedback::regionsJson({{page, QRectF(QPointF(0.2, 0.3), QPointF(0.5, 0.32))}}));
        const QJsonObject record = ReviewCardFeedback::create(cardId, QStringLiteral("negative"), QString(), anchor, QStringLiteral("android-pixel"),
                                                              QDateTime::fromString(createdAt, Qt::ISODateWithMs));
        projections.insert(ReviewCardFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
        return record;
    };
    const QString shown(64, QLatin1Char('a')), earlierPdf(64, QLatin1Char('d'));
    // Made on the baseline card:
    // on GRPO's PDF, inside GRPO's answer regions: shared, exact;
    const QJsonObject inside = feedback(baseline, QStringLiteral("some passage"), QStringLiteral("2026-10-20T09:00:00.000Z"), shown, 1);
    // on GRPO's PDF but outside its regions (page 2), though the quote is in its answer: not shared;
    const QJsonObject outside = feedback(baseline, QStringLiteral("group mean"), QStringLiteral("2026-10-20T09:10:00.000Z"), shown, 2);
    // on an earlier PDF, the quote found in GRPO's answer text (its source passage here): relocated;
    const QJsonObject relocated = feedback(baseline, QStringLiteral("group mean and standard"), QStringLiteral("2026-10-20T09:20:00.000Z"), earlierPdf, 1);
    // on an earlier PDF, the quote not in GRPO's answer: not shared;
    const QJsonObject elsewhere = feedback(baseline, QStringLiteral("variance of the gradient"), QStringLiteral("2026-10-20T09:30:00.000Z"), earlierPdf, 1);
    // another document's feedback, though placed inside GRPO's regions: not shared;
    const QJsonObject otherDocument = feedback(baseline, QStringLiteral("some passage"), QStringLiteral("2026-10-20T09:40:00.000Z"), shown, 1,
                                               QStringLiteral("5b0f6c1e-2a47-4d1b-9c35-7e0a4f2b9d13"));
    // deleted: not shared, even with include_deleted.
    QJsonObject gone = feedback(baseline, QStringLiteral("some passage"), QStringLiteral("2026-10-20T09:50:00.000Z"), shown, 1);
    gone = ReviewCardFeedback::remove(gone, QStringLiteral("mac-okular"));
    projections.insert(ReviewCardFeedback::projectionKey(gone.value(QStringLiteral("feedback_id")).toString()), gone);
    // A deleted card's, inside GRPO's answer: hidden with its card.
    feedback(deletedRecord, QStringLiteral("some passage"), QStringLiteral("2026-10-20T09:55:00.000Z"), shown, 1);
    // GRPO's own record.
    const QJsonObject own = feedback(grpo, QStringLiteral("group mean"), QStringLiteral("2026-10-20T08:00:00.000Z"), shown, 1);

    const QString path = temporary.filePath(QStringLiteral("server/review-query.sqlite"));
    QString error;
    {
        ReviewStore store(docs, [projections] { return projections; }, {}, true);
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    }
    const QString levels = temporary.filePath(QStringLiteral("data/knowledge-tag-levels.json"));
    const auto byId = [](const QJsonObject &result, const QString &id) {
        for (const QJsonValue &value : result.value(QStringLiteral("cards")).toArray())
            if (value.toObject().value(QStringLiteral("card_id")).toString() == id) return value.toObject();
        return QJsonObject();
    };
    const auto summary = [](const QJsonArray &list) {
        QStringList result;
        for (const QJsonValue &value : list) {
            const QJsonObject item = value.toObject();
            result.append(QStringLiteral("%1 %2 %3 %4").arg(item.value(QStringLiteral("feedback_id")).toString(), item.value(QStringLiteral("card_id")).toString(),
                                                             item.value(QStringLiteral("shared")).toBool() ? QStringLiteral("shared") : QStringLiteral("own"),
                                                             item.value(QStringLiteral("anchor_state")).toString()));
        }
        return result;
    };
    const auto id = [](const QJsonObject &record) { return record.value(QStringLiteral("feedback_id")).toString(); };
    for (const bool includeDeleted : {false, true}) {
        const auto result = run(QStringLiteral("search-review-cards"), {{QStringLiteral("query"), QStringLiteral("GRPO advantage")}, {QStringLiteral("include_deleted"), includeDeleted}},
                                path, docs, levels, now);
        QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
        // GRPO: its own, then the baseline card's inside its answer, in created_at order.
        QCOMPARE(summary(byId(result, grpo).value(QStringLiteral("feedback")).toArray()),
                 (QStringList{id(own) + QLatin1Char(' ') + grpo + QStringLiteral(" own exact"),
                              id(inside) + QLatin1Char(' ') + baseline + QStringLiteral(" shared exact"),
                              id(relocated) + QLatin1Char(' ') + baseline + QStringLiteral(" shared relocated")}));
        // The baseline card keeps all of its own; GRPO's record is not in its answer text.
        QStringList baselineOwn{id(inside) + QLatin1Char(' ') + baseline + QStringLiteral(" own exact"),
                                id(outside) + QLatin1Char(' ') + baseline + QStringLiteral(" own exact"),
                                id(relocated) + QLatin1Char(' ') + baseline + QStringLiteral(" own pdf_changed"),
                                id(elsewhere) + QLatin1Char(' ') + baseline + QStringLiteral(" own pdf_changed"),
                                id(otherDocument) + QLatin1Char(' ') + baseline + QStringLiteral(" own exact")};
        if (includeDeleted) baselineOwn.append(id(gone) + QLatin1Char(' ') + baseline + QStringLiteral(" own exact"));
        // GRPO's own record lies in the baseline card's regions on the same PDF: shared there too.
        baselineOwn.prepend(id(own) + QLatin1Char(' ') + grpo + QStringLiteral(" shared exact"));
        QCOMPARE(summary(byId(result, baseline).value(QStringLiteral("feedback")).toArray()), baselineOwn);
        // Another document's cards get none of it.
        QCOMPARE(byId(result, kl).value(QStringLiteral("feedback")).toArray(), QJsonArray());
    }
    // The shared item has every §7 field.
    const auto result = run(QStringLiteral("search-review-cards"), {{QStringLiteral("query"), QStringLiteral("GRPO advantage")}}, path, docs, levels, now);
    QCOMPARE(byId(result, grpo).value(QStringLiteral("feedback")).toArray().at(1).toObject(),
             (QJsonObject{{QStringLiteral("feedback_id"), id(inside)}, {QStringLiteral("polarity"), QStringLiteral("negative")},
                          {QStringLiteral("quote"), QStringLiteral("some passage")}, {QStringLiteral("comment"), QString()},
                          {QStringLiteral("created_at"), QStringLiteral("2026-10-20T09:00:00.000Z")}, {QStringLiteral("updated_at"), QStringLiteral("2026-10-20T09:00:00.000Z")},
                          {QStringLiteral("origin"), QStringLiteral("android-pixel")}, {QStringLiteral("anchor_state"), QStringLiteral("exact")}, {QStringLiteral("page"), 1},
                          {QStringLiteral("segment_id"), QJsonValue::Null}, {QStringLiteral("source_changed"), QJsonValue::Null},
                          {QStringLiteral("card_id"), baseline}, {QStringLiteral("shared"), true}}));

    // thoughtsctl list-review-cards (the live store) lists it the same way.
    ReviewStore store(docs, [projections] { return projections; }, {}, true);
    const QJsonObject listed = store.sourceCardCommand({{QStringLiteral("command"), QStringLiteral("list-review-cards")}, {QStringLiteral("document"), QStringLiteral("rl_grpo.pdf")}},
                                                       temporary.filePath(QStringLiteral("bundle")));
    QVERIFY2(listed.value(QStringLiteral("accepted")).toBool(), QJsonDocument(listed).toJson().constData());
    QCOMPARE(summary(byId(listed, grpo).value(QStringLiteral("feedback")).toArray()),
             (QStringList{id(own) + QLatin1Char(' ') + grpo + QStringLiteral(" own exact"),
                          id(inside) + QLatin1Char(' ') + baseline + QStringLiteral(" shared exact"),
                          id(relocated) + QLatin1Char(' ') + baseline + QStringLiteral(" shared relocated")}));
}

void ReviewKnowledgeTest::readingTheIndexUsesAnExistingLog()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    const QJsonObject projections = fixture(docs);
    const QString path = temporary.filePath(QStringLiteral("server/review-query.sqlite"));
    QString error;
    {
        ReviewStore store(docs, [projections] { return projections; }, {}, true);
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    }
    // A writer that keeps its log open, with a change only in the log.
    {
        QSqlDatabase writer = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("writer"));
        writer.setDatabaseName(path);
        QVERIFY(writer.open());
        QSqlQuery query(writer);
        QVERIFY(query.exec(QStringLiteral("PRAGMA journal_mode=WAL")));
        QVERIFY(query.exec(QStringLiteral("PRAGMA wal_autocheckpoint=0")));
        QVERIFY(query.exec(QStringLiteral("UPDATE review_meta SET value='in the log' WHERE key='indexed_at_utc'")));
        QVERIFY(QFileInfo::exists(path + QStringLiteral("-wal")));
        QVERIFY(QFileInfo::exists(path + QStringLiteral("-shm")));
        const auto before = filesBelow(QFileInfo(path).absolutePath());
        IndexSnapshot snapshot;
        QVERIFY2(readIndex(path, &snapshot, &error), qPrintable(error));
        QCOMPARE(snapshot.indexedAt, QStringLiteral("in the log"));
        QVERIFY(snapshot.projections.contains(QString(QStringLiteral("review_card\ncards\n") + grpo)));
        const auto after = filesBelow(QFileInfo(path).absolutePath());
        // No file appears; the database and its log are unchanged (the shared-memory index is
        // SQLite's own coordination file).
        QCOMPARE(after.keys(), before.keys());
        QCOMPARE(after.value(path), before.value(path));
        QCOMPARE(after.value(path + QStringLiteral("-wal")), before.value(path + QStringLiteral("-wal")));
        writer.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("writer"));
}

QTEST_GUILESS_MAIN(ReviewKnowledgeTest)
#include "reviewknowledgetest.moc"

// Retired cards (protocol/review-card-retire.md section 7): status "retired" with retired_at,
// their own state filter, never due or overdue, and their history still counts.
void ReviewKnowledgeTest::retiredCardsAreReportedButNeverDue()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    QJsonObject projections = fixture(docs);
    const auto queueAction = [&](const QString &eventId, const QString &cardId, const QString &action, int hoursAgo, const QString &undone = {}) {
        QJsonObject event{{QStringLiteral("schema_version"), 3}, {QStringLiteral("event_id"), eventId}, {QStringLiteral("card_id"), cardId},
                          {QStringLiteral("queue_action"), action}, {QStringLiteral("reviewed_at"), iso(now.addSecs(-3600 * hoursAgo))},
                          {QStringLiteral("origin_id"), QStringLiteral("test")}};
        if (!undone.isEmpty()) event.insert(QStringLiteral("undone_event_id"), undone);
        projections.insert(QStringLiteral("review_event\nevents\n") + eventId, event);
    };
    // The overdue new baseline card and the shaky KL card are retired; KL is restored again;
    // the retire of the due tensor card is undone.
    queueAction(QStringLiteral("r1"), baseline, QStringLiteral("retire"), 24);
    queueAction(QStringLiteral("r2"), kl, QStringLiteral("retire"), 5);
    queueAction(QStringLiteral("r3"), kl, QStringLiteral("restore"), 4);
    queueAction(QStringLiteral("r4"), tensor, QStringLiteral("retire"), 3);
    queueAction(QStringLiteral("r5"), tensor, QStringLiteral("undo_retire"), 2, QStringLiteral("r4"));
    const Library library = libraryFromSnapshot({projections, {}, true}, docs);
    QCOMPARE(library.cards.size(), 5);
    const auto found = [&](const QJsonObject &request) {
        auto list = ids(search(library, request, now));
        list.sort();
        return list;
    };
    const auto sorted = [](QStringList list) { list.sort(); return list; };
    QCOMPARE(found({}), sorted({grpo, baseline, tensor, ppo, kl}));
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("retired")}}), QStringList{baseline});
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("active")}}), sorted({grpo, tensor, ppo, kl}));
    // New, due and overdue leave retired cards out.
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("new")}}), QStringList{});
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("overdue")}}), QStringList{});
    QCOMPARE(found({{QStringLiteral("state"), QStringLiteral("due")}}), QStringList{tensor});

    const auto result = search(library, {{QStringLiteral("query"), QStringLiteral("baseline needed")}}, now);
    const QJsonObject retired = result.value(QStringLiteral("cards")).toArray().at(0).toObject();
    QCOMPARE(retired.value(QStringLiteral("card_id")).toString(), baseline);
    QCOMPARE(retired.value(QStringLiteral("status")).toString(), QStringLiteral("retired"));
    QCOMPARE(retired.value(QStringLiteral("retired_at")).toString(), iso(now.addDays(-1)));
    QVERIFY(retired.value(QStringLiteral("due_at")).isNull());
    QVERIFY(!retired.contains(QStringLiteral("overdue_days")));
    const auto statusOf = [&](const QString &id) {
        for (const auto &value : search(library, {}, now).value(QStringLiteral("cards")).toArray())
            if (value.toObject().value(QStringLiteral("card_id")).toString() == id) return value.toObject();
        return QJsonObject();
    };
    QCOMPARE(statusOf(kl).value(QStringLiteral("status")).toString(), QStringLiteral("scheduled"));
    QVERIFY(!statusOf(kl).contains(QStringLiteral("retired_at")));
    QCOMPARE(statusOf(tensor).value(QStringLiteral("status")).toString(), QStringLiteral("due"));
    // Cards never asked again come last when sorted by due date.
    QCOMPARE(ids(search(library, {{QStringLiteral("sort"), QStringLiteral("due")}}, now)).last(), baseline);

    // The profile keeps a retired card's history, without calling it overdue.
    const auto profiled = profile(library, {{QStringLiteral("topic"), QStringLiteral("baseline")}}, defaultLevels(), now);
    QCOMPARE(profiled.value(QStringLiteral("counts")).toObject().value(QStringLiteral("unreviewed")).toInt(), 1);
    QCOMPARE(profiled.value(QStringLiteral("cards")).toObject().value(QStringLiteral("unreviewed")).toArray().at(0).toObject().value(QStringLiteral("reason")).toString(),
             QStringLiteral("never answered; retired"));

    // Through the review index, as thoughtsctl reads it.
    const QString path = temporary.filePath(QStringLiteral("server/review-query.sqlite"));
    QString error;
    {
        ReviewStore store(docs, [projections] { return projections; }, {}, true);
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    }
    const auto viaIndex = run(QStringLiteral("search-review-cards"), {{QStringLiteral("state"), QStringLiteral("retired")}}, path, docs,
                              temporary.filePath(QStringLiteral("levels.json")), now);
    QCOMPARE(ids(viaIndex), QStringList{baseline});
    QCOMPARE(viaIndex.value(QStringLiteral("cards")).toArray().at(0).toObject().value(QStringLiteral("retired_at")).toString(), iso(now.addDays(-1)));
}

// review-phases spec: an answer given in two steps exports what was picked and noted before the
// answer was shown, and the agent's prediction; older answers export as before.
void ReviewKnowledgeTest::historyCarriesTheBeforeStep()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    QJsonObject projections = fixture(docs);
    QJsonObject event = answer(QStringLiteral("e8"), kl, 1, 10, QJsonArray{QStringLiteral("Meh"), QStringLiteral("Memorized")},
                               QJsonObject{{QStringLiteral("before_note"), QStringLiteral("think I know it")}, {QStringLiteral("note"), QStringLiteral("I did")}});
    // A before tag merged since ("Got it") is exported under its new name.
    event.insert(QStringLiteral("review_phases"), QJsonObject{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("before"), QJsonObject{{QStringLiteral("tags"), QJsonArray{QStringLiteral("Meh"), QStringLiteral("Got it")}},
                                               {QStringLiteral("answer_shown_at"), iso(now.addDays(-1).addSecs(-30))}}},
        {QStringLiteral("after"), QJsonObject{{QStringLiteral("tags"), QJsonArray{QStringLiteral("Memorized")}}}},
        {QStringLiteral("agent_prediction"), QJsonObject{{QStringLiteral("tag"), QStringLiteral("Kind of memorized")}, {QStringLiteral("reason"), QStringLiteral("Shaky last time.")},
                                                         {QStringLiteral("group_id"), QStringLiteral("g-understanding")}, {QStringLiteral("model"), QStringLiteral("m")}}}});
    projections.insert(QStringLiteral("review_event\nevents\ne8"), event);
    const Library library = libraryFromSnapshot({projections, {}, true}, docs);
    const auto result = search(library, {{QStringLiteral("query"), QStringLiteral("KL penalty")}}, now);
    QCOMPARE(ids(result), QStringList{kl});
    const auto history = result.value(QStringLiteral("cards")).toArray().at(0).toObject().value(QStringLiteral("history")).toArray();
    QCOMPARE(history.size(), 2);
    const QJsonObject latest = history.at(0).toObject();
    QCOMPARE(strings(latest.value(QStringLiteral("tags"))), (QStringList{QStringLiteral("Meh"), QStringLiteral("Memorized")}));
    QCOMPARE(strings(latest.value(QStringLiteral("before_tags"))), (QStringList{QStringLiteral("Meh"), QStringLiteral("Memorized")}));
    QCOMPARE(latest.value(QStringLiteral("before_note")).toString(), QStringLiteral("think I know it"));
    QCOMPARE(latest.value(QStringLiteral("note")).toString(), QStringLiteral("I did"));
    // The before note is not a group's note.
    QVERIFY(!latest.contains(QStringLiteral("group_notes")));
    QCOMPARE(latest.value(QStringLiteral("agent_prediction")).toObject(),
             (QJsonObject{{QStringLiteral("tag"), QStringLiteral("Kind of memorized")}, {QStringLiteral("reason"), QStringLiteral("Shaky last time.")}}));
    // The older answer has none of them.
    const QJsonObject older = history.at(1).toObject();
    for (const char *field : {"before_tags", "before_note", "agent_prediction"}) QVERIFY(!older.contains(QString::fromLatin1(field)));
}

void ReviewKnowledgeTest::fullAnswerIsTheWholePassage()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    const QJsonObject projections = fixture(docs);
    const QString path = temporary.filePath(QStringLiteral("server/review-query.sqlite"));
    QString error;
    {
        ReviewStore store(docs, [projections] { return projections; }, {}, true);
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    }
    const QString levels = temporary.filePath(QStringLiteral("data/knowledge-tag-levels.json"));
    const auto answerOf = [](const QJsonObject &result) {
        return result.value(QStringLiteral("cards")).toArray().first().toObject().value(QStringLiteral("answer")).toObject();
    };
    const QJsonObject excerpt = answerOf(run(QStringLiteral("search-review-cards"), {{QStringLiteral("query"), QStringLiteral("tensor parallelism split")}}, path, docs, levels, now));
    QVERIFY(excerpt.value(QStringLiteral("truncated")).toBool());
    QVERIFY(excerpt.value(QStringLiteral("text")).toString().size() <= 600);
    const QJsonObject whole = answerOf(run(QStringLiteral("search-review-cards"),
                                           {{QStringLiteral("query"), QStringLiteral("tensor parallelism split")}, {QStringLiteral("full_answer"), true}}, path, docs, levels, now));
    QCOMPARE(whole.value(QStringLiteral("truncated")).toBool(), false);
    QCOMPARE(whole.value(QStringLiteral("text")).toString(), longParallelText());
    QCOMPARE(run(QStringLiteral("search-review-cards"), {{QStringLiteral("full_answer"), 1}}, path, docs, levels, now).value(QStringLiteral("error_code")).toString(),
             QStringLiteral("invalid_request"));
}

namespace
{
// A one-page PDF (Helvetica) with @p line on it.
QByteArray onePagePdf(const QByteArray &line)
{
    const QByteArray content = "BT /F1 12 Tf 72 720 Td (" + line + ") Tj ET";
    const QList<QByteArray> objects{"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
                                    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>",
                                    "<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "\nendstream",
                                    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    QByteArray pdf = "%PDF-1.4\n";
    QList<qsizetype> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(pdf.size());
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects.at(i) + "\nendobj\n";
    }
    const qsizetype xref = pdf.size();
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (qsizetype offset : offsets) pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}
}

void ReviewKnowledgeTest::documentFeedbackComesWithItsPage()
{
    QTemporaryDir temporary;
    const QString docs = temporary.filePath(QStringLiteral("docs"));
    QJsonObject projections = fixture(docs);
    // GRPO notes (the first document of RL) is now the PDF below; an earlier version is gone.
    const QString documentId = QStringLiteral("00000000-0000-4000-a001-000000000001");
    const QByteArray pdf = onePagePdf("The score matrix has one entry per query key pair.");
    const QString current = QString::fromLatin1(QCryptographicHash::hash(pdf, QCryptographicHash::Sha256).toHex());
    const QString earlier = QString(64, QLatin1Char('e'));
    const QString pdfPath = temporary.filePath(QStringLiteral("versions/current.pdf"));
    QVERIFY(write(pdfPath, pdf));
    {
        QFile file(QDir(docs).filePath(QStringLiteral(".thoughts/library-projects.json")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonObject catalogue = QJsonDocument::fromJson(file.readAll()).object();
        file.close();
        QJsonArray projects = catalogue.value(QStringLiteral("projects")).toArray();
        QJsonObject rl = projects.at(0).toObject();
        QJsonArray documents = rl.value(QStringLiteral("documents")).toArray();
        QJsonObject grpoNotes = documents.at(0).toObject();
        QCOMPARE(grpoNotes.value(QStringLiteral("document_id")).toString(), documentId);
        grpoNotes.insert(QStringLiteral("sha256"), current);
        documents.replace(0, grpoNotes);
        rl.insert(QStringLiteral("documents"), documents);
        projects.replace(0, rl);
        catalogue.insert(QStringLiteral("projects"), projects);
        QVERIFY(write(file.fileName(), QJsonDocument(catalogue).toJson()));
    }
    const auto at = [](const char *time) { return QDateTime::fromString(QString::fromLatin1(time), Qt::ISODateWithMs); };
    const auto add = [&](const QJsonObject &record) {
        projections.insert(DocumentFeedback::projectionKey(record.value(QStringLiteral("feedback_id")).toString()), record);
        return record.value(QStringLiteral("feedback_id")).toString();
    };
    const auto passage = [&](const QString &exact, const QString &sha256) {
        return ReviewCardFeedback::anchor(exact, QString(), QString(), documentId, sha256, ReviewCardFeedback::regionsJson({{1, QRectF(0.1, 0.1, 0.5, 0.05)}}));
    };
    const QString dislike = add(DocumentFeedback::create(QStringLiteral("negative"), QStringLiteral("Define the score matrix first"),
                                                         passage(QStringLiteral("one entry per query"), current), QStringLiteral("mac-okular"), at("2026-10-10T09:00:00.000Z")));
    const QString question = add(DocumentFeedback::withKind(DocumentFeedback::create(QStringLiteral("neutral"), QStringLiteral("What is a key?"),
                                                                                    passage(QStringLiteral("an old sentence"), earlier), QStringLiteral("mac-okular"),
                                                                                    at("2026-10-09T09:00:00.000Z")),
                                                            DocumentFeedback::question()));
    DocumentFeedback::Stroke stroke{{QPointF(0.1, 0.1), QPointF(0.3, 0.2)}, QStringLiteral("#ff0000")};
    QJsonObject drawing = DocumentFeedback::createDrawing(1, {stroke}, documentId, current, QStringLiteral("mac-okular"), at("2026-10-11T09:00:00.000Z"));
    drawing.insert(QStringLiteral("comment"), QStringLiteral("axis missing"));
    const QString drawn = add(drawing);
    // A comment never goes to agents, and a deleted record is gone.
    add(DocumentFeedback::withKind(DocumentFeedback::create(QStringLiteral("neutral"), QStringLiteral("note to self"), passage(QStringLiteral("score"), current),
                                                            QStringLiteral("mac-okular"), at("2026-10-11T10:00:00.000Z")),
                                   DocumentFeedback::other()));
    add(DocumentFeedback::remove(DocumentFeedback::create(QStringLiteral("positive"), QString(), passage(QStringLiteral("matrix"), current), QStringLiteral("mac-okular"),
                                                          at("2026-10-11T11:00:00.000Z")),
                                 QStringLiteral("mac-okular")));

    const QString path = temporary.filePath(QStringLiteral("server/review-query.sqlite"));
    QString error;
    {
        ReviewStore store(docs, [projections] { return projections; }, {}, true);
        QVERIFY2(store.refreshQueryDatabase(path, &error), qPrintable(error));
    }
    Library library;
    QJsonObject failure;
    QVERIFY2(readLibrary(path, docs, &library, &failure), QJsonDocument(failure).toJson().constData());
    QStringList asked;
    const VersionPath versionPath = [&](const QString &sha256, QString *unavailable) {
        asked.append(sha256);
        if (sha256 == current) return pdfPath;
        *unavailable = QStringLiteral("not here");
        return QString();
    };
    const QString images = temporary.filePath(QStringLiteral("images"));
    const QJsonObject result = documentFeedback(library, {{QStringLiteral("images_dir"), images}}, versionPath);
    QVERIFY2(result.value(QStringLiteral("accepted")).toBool(), QJsonDocument(result).toJson().constData());
    const QJsonArray feedback = result.value(QStringLiteral("feedback")).toArray();
    QCOMPARE(feedback.size(), 3);
    // Newest first.
    QCOMPARE(feedback.at(0).toObject().value(QStringLiteral("feedback_id")).toString(), drawn);
    QCOMPARE(feedback.at(1).toObject().value(QStringLiteral("feedback_id")).toString(), dislike);
    QCOMPARE(feedback.at(2).toObject().value(QStringLiteral("feedback_id")).toString(), question);
    const QJsonObject disliked = feedback.at(1).toObject();
    QCOMPARE(disliked.value(QStringLiteral("kind")).toString(), QStringLiteral("dislike"));
    QCOMPARE(disliked.value(QStringLiteral("comment")).toString(), QStringLiteral("Define the score matrix first"));
    QCOMPARE(disliked.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), QStringLiteral("one entry per query"));
    QCOMPARE(disliked.value(QStringLiteral("document")).toObject().value(QStringLiteral("name")).toString(), QStringLiteral("GRPO notes"));
    QCOMPARE(disliked.value(QStringLiteral("document")).toObject().value(QStringLiteral("project")).toString(), QStringLiteral("RL"));
    QCOMPARE(disliked.value(QStringLiteral("pdf_sha256")).toString(), current);
    QVERIFY(disliked.value(QStringLiteral("current_version")).toBool());
    QCOMPARE(disliked.value(QStringLiteral("pages")).toArray(), QJsonArray{1});
    const QJsonObject asking = feedback.at(2).toObject();
    QCOMPARE(asking.value(QStringLiteral("kind")).toString(), QStringLiteral("question"));
    QCOMPARE(asking.value(QStringLiteral("resolved")).toBool(true), false);
    QCOMPARE(asking.value(QStringLiteral("current_version")).toBool(true), false);
    const QJsonObject drew = feedback.at(0).toObject();
    QCOMPARE(drew.value(QStringLiteral("kind")).toString(), QStringLiteral("drawing"));
    QCOMPARE(drew.value(QStringLiteral("strokes")).toInt(), 1);
    QVERIFY(!drew.contains(QStringLiteral("quote")));
    QVERIFY(QFileInfo(drew.value(QStringLiteral("image")).toString()).isFile());
    QCOMPARE(QFileInfo(drew.value(QStringLiteral("image")).toString()).absolutePath(), QFileInfo(images).absoluteFilePath());
    // Each version is asked for once, in the order of its newest feedback: the page as that version reads.
    QCOMPARE(asked, (QStringList{current, earlier}));
    QCOMPARE(documentFeedbackVersions(library, {}), asked);
    const QJsonArray versions = result.value(QStringLiteral("versions")).toArray();
    QCOMPARE(versions.size(), 2);
    QVERIFY(versions.at(0).toObject().value(QStringLiteral("available")).toBool());
    const QJsonArray pages = versions.at(0).toObject().value(QStringLiteral("pages")).toArray();
    QCOMPARE(pages.size(), 1);
    QCOMPARE(pages.at(0).toObject().value(QStringLiteral("page")).toInt(), 1);
    QVERIFY2(pages.at(0).toObject().value(QStringLiteral("text")).toString().contains(QStringLiteral("one entry per query")),
             qPrintable(pages.at(0).toObject().value(QStringLiteral("text")).toString()));
    QCOMPARE(versions.at(1).toObject().value(QStringLiteral("available")).toBool(true), false);
    QCOMPARE(versions.at(1).toObject().value(QStringLiteral("unavailable")).toString(), QStringLiteral("not here"));

    // since keeps the newer feedback; include_pages false reads no PDF.
    asked.clear();
    const QJsonObject newer = documentFeedback(library, {{QStringLiteral("since"), QStringLiteral("2026-10-10T00:00:00Z")}, {QStringLiteral("include_pages"), false}}, versionPath);
    QCOMPARE(newer.value(QStringLiteral("returned")).toInt(), 2);
    QVERIFY(asked.isEmpty());
    QVERIFY(!newer.value(QStringLiteral("versions")).toArray().at(0).toObject().contains(QStringLiteral("pages")));
    QCOMPARE(documentFeedback(library, {{QStringLiteral("since"), QStringLiteral("yesterday")}}, versionPath).value(QStringLiteral("error_code")).toString(),
             QStringLiteral("invalid_request"));
    QCOMPARE(documentFeedback(library, {{QStringLiteral("limit"), 3}}, versionPath).value(QStringLiteral("error_code")).toString(), QStringLiteral("invalid_request"));
}

