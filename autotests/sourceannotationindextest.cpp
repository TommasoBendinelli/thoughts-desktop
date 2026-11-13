/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <QTest>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include "../part/docscommentsynchronizer.h"
#include "../part/sourceannotationindex.h"
#include "../shell/reviewdocumenttools.h"

class SourceAnnotationIndexTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void allTypesAndInclusiveLines();
    void movesAmbiguityAndIncompleteCoverage();
    void literalAndMalformedMarkers();
    void singleRangeAndLegacyRead();
    void markdownRangeAndReviewEvidence();
    void legacyPointAnchorIsAComment();
    void commentLocationIgnoresMarkupType();
};

static QJsonObject scan(const QByteArray &bytes, const QString &suffix = QStringLiteral(".tex")) {
    QTemporaryDir dir;
    const auto path = dir.filePath(QStringLiteral("source") + suffix);
    QFile file(path); if (!file.open(QIODevice::WriteOnly)) return {};
    file.write(bytes); file.close();
    return Okular::DocsCommentSynchronizer::sourceCommentFileIndex(path, {});
}
static QJsonObject indexOf(QJsonObject file, const QString &path = QStringLiteral("docs-test/tex/source/a.tex")) {
    file.insert(QStringLiteral("project"), QStringLiteral("docs-test"));
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("files"), QJsonObject{{path, file}}},
        {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-test"), true}}}};
}
static QJsonObject locate(const QJsonObject &index, const QString &type, const QString &id) {
    return Thoughts::annotationSourceLocation(index, type, id, QStringLiteral("docs-test"));
}

void SourceAnnotationIndexTest::allTypesAndInclusiveLines() {
    const auto file = scan("\\CommentAnchor{comment}{A note.}\n"
        "\\CommentHighlight{highlight}{Explanation.}{First\nsecond}\n"
        "\\CommentStrikethrough{strike}{Reason.}{Old}\n"
        "\\ReviewCardMaskStart{card}{0}%\nAnswer.\n\\ReviewCardMaskEnd{card}{0}%\n"
        "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{proposal}{Reason.}{Old\ntext}{New\ntext}\n");
    QVERIFY(file.value(QStringLiteral("valid")).toBool());
    const auto index = indexOf(file);
    for (const auto &pair : {qMakePair("comment", "comment"), qMakePair("highlight", "highlight"), qMakePair("strikethrough", "strike"), qMakePair("review_card", "card"), qMakePair("proposal", "proposal")})
        QCOMPARE(locate(index, QLatin1String(pair.first), QLatin1String(pair.second)).value(QStringLiteral("state")).toString(), QStringLiteral("linked"));
    QCOMPARE(locate(index, QStringLiteral("comment"), QStringLiteral("comment")).value(QStringLiteral("end_line")).toInt(), 1);
    QCOMPARE(locate(index, QStringLiteral("highlight"), QStringLiteral("highlight")).value(QStringLiteral("end_line")).toInt(), 3);
    QCOMPARE(locate(index, QStringLiteral("review_card"), QStringLiteral("card")).value(QStringLiteral("start_line")).toInt(), 5);
    QCOMPARE(locate(index, QStringLiteral("review_card"), QStringLiteral("card")).value(QStringLiteral("end_line")).toInt(), 7);
    QCOMPARE(locate(index, QStringLiteral("proposal"), QStringLiteral("proposal")).value(QStringLiteral("end_line")).toInt(), 10);
}

void SourceAnnotationIndexTest::movesAmbiguityAndIncompleteCoverage() {
    const auto original = scan("\\CommentAnchor{same}{Text.}\n");
    const auto moved = indexOf(original, QStringLiteral("docs-test/tex/source/b.tex"));
    QCOMPARE(locate(moved, QStringLiteral("comment"), QStringLiteral("same")).value(QStringLiteral("source_file")).toString(), QStringLiteral("docs-test/tex/source/b.tex"));
    auto duplicate = moved;
    auto files = moved.value(QStringLiteral("files")).toObject();
    files.insert(QStringLiteral("docs-test/tex/source/a.tex"), files.begin().value());
    duplicate.insert(QStringLiteral("files"), files);
    QCOMPARE(locate(duplicate, QStringLiteral("comment"), QStringLiteral("same")).value(QStringLiteral("state")).toString(), QStringLiteral("ambiguous"));
    const auto selected = Thoughts::annotationSourceLocation(duplicate, QStringLiteral("comment"), QStringLiteral("same"),
        QStringLiteral("docs-test"), QStringLiteral("docs-test/tex/source/a.tex"));
    QCOMPARE(selected.value(QStringLiteral("state")).toString(), QStringLiteral("linked"));
    QCOMPARE(selected.value(QStringLiteral("source_file")).toString(), QStringLiteral("docs-test/tex/source/a.tex"));
    duplicate.insert(QStringLiteral("projects"), QJsonObject());
    QCOMPARE(locate(duplicate, QStringLiteral("comment"), QStringLiteral("same")).value(QStringLiteral("state")).toString(), QStringLiteral("unavailable"));
    QCOMPARE(locate(indexOf(scan("Empty.\n")), QStringLiteral("comment"), QStringLiteral("same")).value(QStringLiteral("state")).toString(), QStringLiteral("missing"));
    QCOMPARE(Thoughts::annotationSourceLocation(moved, QStringLiteral("comment"), QStringLiteral("same"), QStringLiteral("other-project")).value(QStringLiteral("state")).toString(), QStringLiteral("unavailable"));
}

void SourceAnnotationIndexTest::literalAndMalformedMarkers() {
    const auto definitions = scan("\\providecommand{\\ReviewCardMaskStart}[2]{}\n"
        "\\providecommand{\\agentProposedEdit}[5][]{#5}\n"
        "\\def\\ReviewCardMaskEnd#1#2{}\n"
        "\\newcommand{\\example}{\\ReviewCardMaskStart{example}{0}}\n"
        "\\CommentAnchor{live}{Text.}\n");
    QVERIFY(definitions.value(QStringLiteral("valid")).toBool());
    QCOMPARE(definitions.value(QStringLiteral("locations")).toObject().size(), 1);
    const auto literal = scan("\\begin{DocCode}\n\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{example}{Why}{Old}{New}\n\\ReviewCardMaskStart{card}{0}\n\\end{DocCode}\n"
        "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{live}{Why}{\\CommentAnchor{old}{Old}}{\\CommentAnchor{new}{New}}\n");
    QVERIFY(literal.value(QStringLiteral("valid")).toBool());
    const auto locations = literal.value(QStringLiteral("locations")).toObject();
    QVERIFY(!locations.contains(QStringLiteral("proposal\nexample")));
    QVERIFY(!locations.contains(QStringLiteral("comment\nold")));
    QVERIFY(locations.contains(QStringLiteral("comment\nnew")));
    QVERIFY(!scan("\\ReviewCardMaskStart{card}{0}\nAnswer.\n").value(QStringLiteral("valid")).toBool());
    QVERIFY(!scan("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{broken}{Why}{Old}{\n").value(QStringLiteral("valid")).toBool());
}

void SourceAnnotationIndexTest::singleRangeAndLegacyRead() {
    const QString id = QStringLiteral("aaaaaaaa-aaaa-4aaa-aaaa-aaaaaaaaaaaa");
    const QByteArray text("First.\n\nLast.\n");
    const auto plan = Thoughts::markReviewSourceLines(QStringLiteral("source.tex"), text, {1,3}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.anchor.value(QStringLiteral("segments")).toArray().size(), 1);
    QVERIFY(Thoughts::readReviewSourceAnchor(QStringLiteral("source.tex"), plan.replacement, id).error.isEmpty());
    QVERIFY(!Thoughts::markReviewSourceLines(QStringLiteral("source.tex"), "First.\nUnselected.\nLast.\n", {1,3}, id).error.isEmpty());
    const auto start = "\\ReviewCardMaskStart{" + id.toUtf8();
    const auto end = "\\ReviewCardMaskEnd{" + id.toUtf8();
    const QByteArray legacy = start + "}{0}%\nFirst.\n" + end + "}{0}%\nUnselected.\n" + start + "}{1}%\nLast.\n" + end + "}{1}%\n";
    const auto parsed = Thoughts::readReviewSourceAnchor(QStringLiteral("source.tex"), legacy, id);
    QVERIFY2(parsed.error.isEmpty(), qPrintable(parsed.error));
    QCOMPARE(parsed.anchor.value(QStringLiteral("segments")).toArray().size(), 2);
}
void SourceAnnotationIndexTest::markdownRangeAndReviewEvidence() {
    const auto file = scan("<!-- thoughts:v1:meta {\"key\":\"comment:note:s1\",\"contents\":\"Note.\"} -->\n"
        "<!-- thoughts:v1:start comment:note:s1 -->\nFirst.\nSecond.\n<!-- thoughts:v1:end comment:note:s1 -->\n"
        "<!-- thoughts:v1:start review:card:answer:0 -->\nAnswer.\n<!-- thoughts:v1:end review:card:answer:0 -->\n", QStringLiteral(".md"));
    QVERIFY(file.value(QStringLiteral("valid")).toBool());
    const auto location = locate(indexOf(file), QStringLiteral("comment"), QStringLiteral("note"));
    QCOMPARE(location.value(QStringLiteral("start_line")).toInt(), 2);
    QCOMPARE(location.value(QStringLiteral("end_line")).toInt(), 5);
    QCOMPARE(file.value(QStringLiteral("reviews")).toObject().value(QStringLiteral("card")).toArray().first().toObject().value(QStringLiteral("content_sha256")).toString(),
        QStringLiteral("8b50097a1438db130a9e852c0f9ff29a98b4f1f74e09ef5d73329e29620357d9"));
}
// The August 2026 anchor worker wrote "\CommentAnchor{id}{text}{}%". Its empty group made the
// index file the marker as a highlight, so the Text comment showed source_marker_unavailable
// as soon as its file changed (thoughts-29e44bed, thoughts-93f64fca, thoughts-89aa09fd).
void SourceAnnotationIndexTest::legacyPointAnchorIsAComment() {
    const auto file = scan("\\begin{document}\n"
        "\\CommentAnchor{thoughts-good}{Good businesses ideas}{}%\n"
        "Extracting the mean.\n"
        "\\agentProposedEdit[created=2026-10-11T23:12:53.632Z,conversation_id=79fb9eb8-5dd0-5ee8-a662-fc977691d182]{agent-4ac324cc}{Remove a leftover marker.\n"
        "}{\\CommentAnchor{9aa9a2e2}{Old}{}%\n"
        "}{}\\CommentAnchor{thoughts-arousal}{Arousal}{}%\n"
        "\\CommentAnchor{legacy-highlight}{Note.}{marked words}\n"
        "\\end{document}");
    QVERIFY(file.value(QStringLiteral("valid")).toBool());
    QCOMPARE(file.value(QStringLiteral("annotation_parser_version")).toInt(), Thoughts::annotationParserVersion);
    const auto locations = file.value(QStringLiteral("locations")).toObject();
    QVERIFY(locations.contains(QStringLiteral("comment\nthoughts-good")));
    QVERIFY(locations.contains(QStringLiteral("comment\nthoughts-arousal")));
    QVERIFY(!locations.contains(QStringLiteral("highlight\nthoughts-good")));
    QVERIFY(!locations.contains(QStringLiteral("highlight\nthoughts-arousal")));
    QVERIFY(locations.contains(QStringLiteral("highlight\nlegacy-highlight"))); // a non-empty body still marks words
    const auto index = indexOf(file);
    for (const auto &[id, line] : {std::pair{QStringLiteral("thoughts-good"), 2}, std::pair{QStringLiteral("thoughts-arousal"), 6}}) {
        const auto location = locate(index, QStringLiteral("comment"), id);
        QCOMPARE(location.value(QStringLiteral("state")).toString(), QStringLiteral("linked"));
        QCOMPARE(location.value(QStringLiteral("start_line")).toInt(), line);
        QCOMPARE(location.value(QStringLiteral("end_line")).toInt(), line);
    }
    const auto markers = file.value(QStringLiteral("markers")).toObject();
    QVERIFY(!markers.value(QStringLiteral("thoughts-arousal")).toArray().first().toObject().contains(QStringLiteral("selected_text")));
    QCOMPARE(markers.value(QStringLiteral("legacy-highlight")).toArray().first().toObject().value(QStringLiteral("selected_text")).toString(), QStringLiteral("marked words"));
}

// The ID links a comment to its marker; the marker's markup need not match the PDF subtype.
void SourceAnnotationIndexTest::commentLocationIgnoresMarkupType() {
    const auto markdown = indexOf(scan("Intro.\n\n<!-- thoughts:v1:meta {\"key\":\"comment:thoughts-fuser:s1\",\"contents\":\"What is fuser?\",\"revision\":0} -->\n\n"
        "- If <!-- thoughts:v1:start comment:thoughts-fuser:s1 -->fusers<!-- thoughts:v1:end comment:thoughts-fuser:s1 --> can't tell.\n", QStringLiteral(".md")),
        QStringLiteral("docs-test/notes.md"));
    // Markdown markers carry no markup type: looked up as a highlight, the marker was never found.
    QVERIFY(locate(markdown, QStringLiteral("highlight"), QStringLiteral("thoughts-fuser")).value(QStringLiteral("state")).toString() != QLatin1String("linked"));
    auto location = Thoughts::commentSourceLocation(markdown, QStringLiteral("Highlight"), QStringLiteral("thoughts-fuser"), QStringLiteral("docs-test"));
    QCOMPARE(location.value(QStringLiteral("state")).toString(), QStringLiteral("linked"));
    QCOMPARE(location.value(QStringLiteral("start_line")).toInt(), 5);
    QCOMPARE(location.value(QStringLiteral("source_file")).toString(), QStringLiteral("docs-test/notes.md"));
    const auto tex = indexOf(scan("\\CommentHighlight{marked}{Why.}{words}\n\\CommentAnchor{point}{Note.}%\n"));
    QCOMPARE(Thoughts::commentSourceLocation(tex, QStringLiteral("Text"), QStringLiteral("marked"), QStringLiteral("docs-test")).value(QStringLiteral("state")).toString(), QStringLiteral("linked"));
    QCOMPARE(Thoughts::commentSourceLocation(tex, QStringLiteral("Highlight"), QStringLiteral("point"), QStringLiteral("docs-test")).value(QStringLiteral("start_line")).toInt(), 2);
    QCOMPARE(Thoughts::commentSourceLocation(tex, QStringLiteral("Text"), QStringLiteral("absent"), QStringLiteral("docs-test")).value(QStringLiteral("state")).toString(), QStringLiteral("missing"));
}
QTEST_GUILESS_MAIN(SourceAnnotationIndexTest)
#include "sourceannotationindextest.moc"
