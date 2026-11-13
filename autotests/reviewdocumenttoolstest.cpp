// SPDX-License-Identifier: GPL-2.0-or-later
#include "shell/reviewdocumenttools.h"
#include "part/reviewplacement.h"
#include "part/agentpromptsettings.h"
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTest>
#include <QTemporaryDir>
#include <QPdfWriter>
#include <QPainter>
#include <QJsonArray>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QUuid>
#include <QDir>
#include <QJsonObject>
class ReviewDocumentToolsTest : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void fingerprintsAndUniqueRelocation();
    void markdownPlacementRequiresMatchingIdentity();
    void sourceMarkersPreserveBytesAndRejectHiddenText();
    void markdownManualMarkersRequireOneVisiblePassage();
    void texMarkersReadAcrossPendingProposals();
    void visualMarkdownSelectionUsesMarkdownMarkers();
    void cardAgentDescriptionQuotesSourceMarkers();
    void cardAgentDescriptionFallsBackToPdfText();
    void agentPromptTemplatesRender();
    void agentPromptSettingsPersistAndReset();
    void severalPassagesUseNumberedMarkers();
};
static void pdf(const QString &path, const QStringList &pages) {
    QPdfWriter writer(path); writer.setResolution(72);
    QPainter painter(&writer);
    for (int i = 0; i < pages.size(); ++i) { if (i) writer.newPage(); painter.drawText(QPointF(100,100), pages[i]); }
}
void ReviewDocumentToolsTest::fingerprintsAndUniqueRelocation() {
    QTemporaryDir dir;
    const auto first=dir.filePath(QStringLiteral("first.pdf")), moved=dir.filePath(QStringLiteral("moved.pdf")), duplicate=dir.filePath(QStringLiteral("duplicate.pdf"));
    pdf(first,{QStringLiteral("Answer one")});
    pdf(moved,{QStringLiteral("Different page"),QStringLiteral("Answer one")});
    pdf(duplicate,{QStringLiteral("Answer one"),QStringLiteral("Answer one")});
    const auto result=Thoughts::fingerprintReviewPage(first,1);
    QVERIFY2(!result.contains(QStringLiteral("error")),qPrintable(result.value(QStringLiteral("error")).toString()));
    const auto hash=result.value(QStringLiteral("page_visual_sha256")).toString(); QCOMPARE(hash.size(),64);
    QCOMPARE(Thoughts::locateReviewPage(moved,hash).value(QStringLiteral("page")).toInt(),2);
    QVERIFY(Thoughts::locateReviewPage(duplicate,hash).contains(QStringLiteral("error")));
    QVERIFY(Thoughts::fingerprintReviewPage(first,1,QString(64,QLatin1Char('a'))).contains(QStringLiteral("error")));
    QVERIFY(Thoughts::fingerprintReviewPage(first,2).contains(QStringLiteral("error")));
}
void ReviewDocumentToolsTest::sourceMarkersPreserveBytesAndRejectHiddenText() {
    const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray source("\\documentclass{article}\n\\begin{document}\nFirst answer.\nSecond answer.\n\\end{document}\n");
    const auto plan=Thoughts::markReviewSourceLines(QStringLiteral("docs-test/tex/source/main.tex"),source,{3,4},id);
    QVERIFY2(plan.error.isEmpty(),qPrintable(plan.error));
    QCOMPARE(plan.anchor.value(QStringLiteral("segments")).toArray().size(),1);
    QByteArray recovered=plan.replacement;
    recovered.replace(QByteArray(QByteArray("\\ReviewCardMaskStart{")+id.toUtf8()+"}{0}%\n"), QByteArray());
    recovered.replace(QByteArray(QByteArray("\\ReviewCardMaskEnd{")+id.toUtf8()+"}{0}%\n"), QByteArray());
    QCOMPARE(recovered,source);
    QVERIFY(!Thoughts::markReviewSourceLines(QStringLiteral("a.tex"),source,{1},id).error.isEmpty());
    QVERIFY(!Thoughts::markReviewSourceLines(QStringLiteral("a.tex"),QByteArray("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{x}{comment}{\nHidden.\n}{\nVisible.\n}\n"),{2},id).error.isEmpty());
    QVERIFY(!Thoughts::markReviewSourceLines(QStringLiteral("a.tex"),source,{99},id).error.isEmpty());
}
void ReviewDocumentToolsTest::markdownManualMarkersRequireOneVisiblePassage() {
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = QStringLiteral("docs-test/markdown/source/main.md");
    const QByteArray start = QStringLiteral("<!-- thoughts:v1:start review:%1:answer:0 -->\n").arg(id).toUtf8();
    const QByteArray end = QStringLiteral("<!-- thoughts:v1:end review:%1:answer:0 -->\n").arg(id).toUtf8();
    const QByteArray marked = "# Heading\n\n" + start + "Answer.\n" + end;
    const auto plan = Thoughts::readReviewSourceAnchor(path, marked, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.replacement, marked);
    const auto segment = plan.anchor.value(QStringLiteral("segments")).toArray().first().toObject();
    QCOMPARE(segment.value(QStringLiteral("line_start")).toInt(), 4);
    QCOMPARE(segment.value(QStringLiteral("line_end")).toInt(), 5);
    QCOMPARE(segment.value(QStringLiteral("content_sha256")).toString(),
        QString::fromLatin1(QCryptographicHash::hash("Answer.\n", QCryptographicHash::Sha256).toHex()));
    QVERIFY(!Thoughts::readReviewSourceAnchor(path, marked + start + "Again.\n" + end, id).error.isEmpty());
    QVERIFY(!Thoughts::readReviewSourceAnchor(path, "```html\n" + start + "Hidden.\n" + end + "```\n", id).error.isEmpty());
    QVERIFY(!Thoughts::readReviewSourceAnchor(path, start + "\n" + end, id).error.isEmpty());
}
void ReviewDocumentToolsTest::texMarkersReadAcrossPendingProposals() {
    // A pending proposal holds its original and its replacement; each ends the
    // same environment. Card markers elsewhere in the file must still read, and
    // nothing may be marked inside the proposal.
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString path = QStringLiteral("docs-test/tex/source/main.tex");
    const QByteArray proposal =
        "\\begin{enumerate}\n"
        "\\item First.\n"
        "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{agent-x}{Rewrite the end.}{\\item Old.\n"
        "\\end{enumerate}\n"
        "}{\\item New.\n"
        "\\end{enumerate}\n"
        "}\n"
        "After.\n";
    const QByteArray body = "\\begin{document}\nTitle.\n\n";
    const QByteArray marked = body.left(17) + "\\ReviewCardMaskStart{" + id.toUtf8() + "}{0}%\nTitle.\n\\ReviewCardMaskEnd{" + id.toUtf8() + "}{0}%\n\n" + proposal + "\\end{document}\n";
    const auto plan = Thoughts::readReviewSourceAnchor(path, marked, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.replacement, marked);
    const QByteArray unmarked = body + proposal + "\\end{document}\n";
    QVERIFY2(Thoughts::markReviewSourceLines(path, unmarked, {2}, id).error.isEmpty(), "a passage before the proposal can be marked");
    QVERIFY2(Thoughts::markReviewSourceLines(path, unmarked, {11}, id).error.isEmpty(), "a passage after the proposal can be marked");
    for (const int line : {7, 8})
        QVERIFY2(!Thoughts::markReviewSourceLines(path, unmarked, {line}, id).error.isEmpty(), "nothing inside the proposal can be marked");
    // A real structural problem is named instead of blaming the markers.
    const auto broken = Thoughts::readReviewSourceAnchor(path, marked + "\\end{itemize}\n", id);
    QVERIFY(broken.error.contains(QStringLiteral("environments are not balanced")));
}
static bool writeFile(const QString &path, const QByteArray &bytes) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) return false;
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
void ReviewDocumentToolsTest::cardAgentDescriptionQuotesSourceMarkers() {
    QTemporaryDir root;
    const QString id = QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566");
    const QString pdfPath = root.filePath(QStringLiteral("current_pdfs/paper.pdf"));
    QVERIFY(QDir().mkpath(QFileInfo(pdfPath).absolutePath()));
    pdf(pdfPath, {QStringLiteral("One"), QStringLiteral("Two"), QStringLiteral("Three")});
    QVERIFY(writeFile(root.filePath(QStringLiteral("papers/source/main.tex")), QStringLiteral(
        "\\section{Intro}\n"
        "\\ReviewCardMaskStart{%1}{0}%\n"
        "First answer line.   \n"
        "  Indented second line.\n"
        "\\ReviewCardMaskEnd{%1}{0}%\n"
        "Between.\n"
        "\\ReviewCardMaskStart{%1}{1}%\n"
        "Third.\n"
        "\\ReviewCardMaskEnd{%1}{1}%\n").arg(id).toUtf8()));
    const QJsonArray projects {QJsonObject {{QStringLiteral("id"), QStringLiteral("papers")}, {QStringLiteral("name"), QStringLiteral("Papers")},
        {QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")},
            {QStringLiteral("source_file"), QStringLiteral("papers/source/main.tex")}, {QStringLiteral("document_id"), QStringLiteral("doc-1")}}}}}};
    Thoughts::ReviewCardAgentInput card;
    card.cardId = id;
    card.question = QStringLiteral("  What does\nthe  intro say?");
    card.note = QStringLiteral("Check   the\tproof.");
    card.pdfPath = pdfPath;
    card.documentName = QStringLiteral("Paper");
    card.regions = QJsonArray {QJsonObject {{QStringLiteral("page"), 3}, {QStringLiteral("rect"), QJsonArray {0.1, 0.1, 0.5, 0.2}}},
                               QJsonObject {{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QJsonArray {0.1, 0.1, 0.5, 0.2}}}};
    // Markdown passages: blank lines just inside markers are dropped from range and text, inner
    // blank lines and indentation stay, and a marker mid-line keeps only the text between markers.
    QVERIFY(writeFile(root.filePath(QStringLiteral("papers/source/notes.md")), QStringLiteral(
        "# Notes\n"
        "<!-- thoughts:v1:start review:%1:answer:0 -->\n"
        "\n"
        "The answer   starts here,   \n"
        "\n"
        "  and ends here.\n"
        "\n"
        "<!-- thoughts:v1:end review:%1:answer:0 -->\n"
        "Inline: <!-- thoughts:v1:start review:%1:answer:1 -->just this<!-- thoughts:v1:end review:%1:answer:1 --> rest.\n"
        "Lead <!-- thoughts:v1:start review:%1:answer:2 -->first part\n"
        "second part  <!-- thoughts:v1:end review:%1:answer:2 --> tail\n"
        "<!-- thoughts:v1:start review:%1:answer:3 -->\n"
        "\n"
        "<!-- thoughts:v1:end review:%1:answer:3 -->\n").arg(id).toUtf8()));
    // The same file listed twice is read once; each file is named once.
    card.sourceFiles = {QStringLiteral("papers/source/main.tex"), QStringLiteral("papers/source/notes.md"), QStringLiteral("papers/source/main.tex")};
    const QString source = root.filePath(QStringLiteral("papers/source/main.tex"));
    const QString notes = root.filePath(QStringLiteral("papers/source/notes.md"));
    // The default names where the answer is but never quotes it.
    QCOMPARE(Thoughts::reviewCardAgentDescription(root.path(), projects, card), QStringLiteral(
        "Thoughts document: \"Paper\"\n"
        "Project: Papers\n"
        "Source: %1  (LaTeX)\n"
        "To change main.tex, create proposals with the Thoughts create_proposals tool (several edits at once) or "
        "`thoughtsctl create-proposals`; don't edit the source directly unless asked, and never edit the PDF.\n"
        "Built PDF: %2  (3 pages)\n"
        "Document ID: doc-1\n"
        "Review card: %3\n"
        "Question: \"What does the intro say?\"\n"
        "Note: \"Check the proof.\"\n"
        "Answer pages: 2–3\n"
        "Answer source: %1, lines 3–4; 8–8; %4, lines 4–6; 9–9; 10–11\n").arg(source, pdfPath, id, notes));
    // {answer_text} brings the answer back for a template that asks for it.
    QCOMPARE(Thoughts::reviewCardAgentDescription(root.path(), projects, card, QStringLiteral("Card {card_id}\nAnswer text:\n{answer_text}\n")),
             QStringLiteral(
        "Card %1\n"
        "Answer text:\n"
        "First answer line.\n"
        "  Indented second line.\n"
        "\n"
        "Third.\n"
        "\n"
        "The answer   starts here,\n"
        "\n"
        "  and ends here.\n"
        "\n"
        "just this\n"
        "\n"
        "first part\n"
        "second part\n").arg(id));

    // Only Markdown, a single page and no note.
    card.sourceFiles = {QStringLiteral("papers/source/notes.md")};
    card.note.clear();
    card.regions = QJsonArray {QJsonObject {{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QJsonArray {0.1, 0.1, 0.5, 0.2}}}};
    const QString markdown = Thoughts::reviewCardAgentDescription(root.path(), projects, card);
    QVERIFY(markdown.contains(QStringLiteral("  (3 pages)\n")));
    QVERIFY(!markdown.contains(QStringLiteral("Note:")));
    QVERIFY2(markdown.endsWith(QStringLiteral("Answer pages: 1\nAnswer source: %1, lines 4–6; 9–9; 10–11\n").arg(notes)), qPrintable(markdown));
    QVERIFY(!markdown.contains(QStringLiteral("The answer")));
}
void ReviewDocumentToolsTest::cardAgentDescriptionFallsBackToPdfText() {
    QTemporaryDir root;
    const QString pdfPath = root.filePath(QStringLiteral("current_pdfs/plain.pdf"));
    QVERIFY(QDir().mkpath(QFileInfo(pdfPath).absolutePath()));
    {
        QPdfWriter writer(pdfPath);
        writer.setResolution(72);
        QPainter painter(&writer);
        painter.drawText(QPointF(100, 100), QStringLiteral("Outside the region"));
        writer.newPage();
        painter.drawText(QPointF(100, 300), QStringLiteral("Second   region"));
        painter.drawText(QPointF(100, 100), QStringLiteral("Alpha beta"));
        painter.drawText(QPointF(100, 130), QStringLiteral("Gamma"));
    }
    Thoughts::ReviewCardAgentInput card;
    card.cardId = QStringLiteral("card-1");
    card.question = QStringLiteral("Which letters?");
    card.pdfPath = pdfPath;
    // Listed out of order; the text follows reading order. Rects are fractions of the page.
    card.regions = QJsonArray {QJsonObject {{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QJsonArray {0.05, 0.3, 0.9, 0.4}}},
                               QJsonObject {{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QJsonArray {0.05, 0.05, 0.9, 0.2}}}};
    card.sourceFiles = {QStringLiteral("missing/source.tex")};
    QFile built(pdfPath);
    QVERIFY(built.open(QIODevice::ReadOnly));
    card.pdfSha256 = QString::fromLatin1(QCryptographicHash::hash(built.readAll(), QCryptographicHash::Sha256).toHex());
    const QString text = Thoughts::reviewCardAgentDescription(root.path(), {}, card);
    QVERIFY2(text.startsWith(QStringLiteral("Thoughts document: \"plain.pdf\"\nBuilt PDF: %1  (2 pages)\n").arg(pdfPath)), qPrintable(text));
    QVERIFY2(text.endsWith(QStringLiteral("Review card: card-1\nQuestion: \"Which letters?\"\nAnswer pages: 2\n")), qPrintable(text));
    QVERIFY(!text.contains(QStringLiteral("Answer source:")));
    QVERIFY(!text.contains(QStringLiteral("Alpha")));
    // With {answer_text}: the PDF text in the answer regions, in reading order.
    const QString answerTemplate = QStringLiteral("Answer pages: {answer_pages}\nAnswer text:\n{answer_text}\n");
    QCOMPARE(Thoughts::reviewCardAgentDescription(root.path(), {}, card, answerTemplate),
             QStringLiteral("Answer pages: 2\nAnswer text:\nAlpha beta\nGamma\nSecond region\n"));
    // Another build of the PDF: its regions may cover other text, so there is no answer text.
    card.pdfSha256 = QString(64, QLatin1Char('a'));
    QCOMPARE(Thoughts::reviewCardAgentDescription(root.path(), {}, card, answerTemplate), QStringLiteral("Answer pages: 2\nAnswer text:\n"));
    // Nothing to quote: the PDF is gone, so there are no page count or text either beyond the card's own page.
    card.pdfPath = root.filePath(QStringLiteral("current_pdfs/gone.pdf"));
    card.regions = {};
    card.page = 6;
    const QString bare = Thoughts::reviewCardAgentDescription(root.path(), {}, card);
    QVERIFY2(bare.startsWith(QStringLiteral("Thoughts document: \"gone.pdf\"\nReview card: card-1\nQuestion: \"Which letters?\"\nAnswer pages: 6\n")),
             qPrintable(bare));
}

void ReviewDocumentToolsTest::agentPromptTemplatesRender() {
    using namespace Thoughts::AgentPrompts;
    const QHash<QString, QString> fields {
        {QStringLiteral("card_id"), QStringLiteral("card-1")},
        {QStringLiteral("question"), QStringLiteral("Explain GRPO")},
        {QStringLiteral("note"), QStringLiteral("   ")},
        {QStringLiteral("answer_text"), QStringLiteral("Line one\nLine two\n\n")},
    };
    // A line with an empty or missing placeholder is left out; other braces stay as written.
    QCOMPARE(render(QStringLiteral("Card {card_id}: \"{question}\"\r\nNote: {note}\nPages: {answer_pages}\n"
                                   "Keep \\frac{a}{b} and {unknown}\n{answer_text}\nEnd\n\n\n"), fields),
             QStringLiteral("Card card-1: \"Explain GRPO\"\nKeep \\frac{a}{b} and {unknown}\nLine one\nLine two\nEnd\n"));
    QCOMPARE(render(QStringLiteral("{note}"), fields), QString());
    // {copy_for_agent} and the photo folder (the physical studying template's) are no longer placeholders.
    const QString agent = QStringLiteral("Card {card_id}\n{copy_for_agent}\n");
    QCOMPARE(renderForCard(fields, agent), QStringLiteral("Card card-1\n{copy_for_agent}\n"));
    // The answer being given now follows the card lines, also in a template saved before
    // {current_review} existed; a template naming it places it.
    auto giving = fields;
    giving.insert(QStringLiteral("current_review"), QStringLiteral("Current review (being given now, not saved yet):\n- Again in: 4 weeks\n"));
    QCOMPARE(renderForCard(giving, QStringLiteral("Card {card_id}\n")),
             QStringLiteral("Card card-1\nCurrent review (being given now, not saved yet):\n- Again in: 4 weeks\n"));
    QCOMPARE(renderForCard(giving, QStringLiteral("{current_review}\nCard {card_id}\n")),
             QStringLiteral("Current review (being given now, not saved yet):\n- Again in: 4 weeks\nCard card-1\n"));
    QVERIFY(defaultTemplate(Kind::Agent).endsWith(QStringLiteral("{answer_feedback}\n{current_review}\n")));
    // The defaults use only known placeholders and leave the answer out.
    QStringList known;
    for (const auto &placeholder : placeholders()) known.append(placeholder.name);
    static const QRegularExpression pattern(QStringLiteral("\\{([a-z_]+)\\}"));
    auto matches = pattern.globalMatch(defaultTemplate(Kind::Agent));
    while (matches.hasNext()) {
        const QString name = matches.next().captured(1);
        QVERIFY2(known.contains(name), qPrintable(name));
    }
    QVERIFY(!usesPlaceholder(defaultTemplate(Kind::Agent), QStringLiteral("answer_text")));
}

void ReviewDocumentToolsTest::agentPromptSettingsPersistAndReset() {
    using namespace Thoughts::AgentPrompts;
    QStandardPaths::setTestModeEnabled(true);
    KConfigGroup(KSharedConfig::openConfig(), configGroup()).deleteGroup();
    QCOMPARE(savedTemplate(Kind::Agent), defaultTemplate(Kind::Agent));
    saveTemplate(Kind::Agent, QStringLiteral("Line one\n\nLine {card_id}\n"));
    KSharedConfig::openConfig()->reparseConfiguration();
    QCOMPARE(savedTemplate(Kind::Agent), QStringLiteral("Line one\n\nLine {card_id}\n"));
    // Blank or the default: back to the default, with no entry left behind.
    saveTemplate(Kind::Agent, QStringLiteral("  \n"));
    KSharedConfig::openConfig()->reparseConfiguration();
    const KConfigGroup group(KSharedConfig::openConfig(), configGroup());
    QVERIFY(!group.hasKey(configKey(Kind::Agent)));
    QCOMPARE(savedTemplate(Kind::Agent), defaultTemplate(Kind::Agent));
    // The default before 2026-10-22 ended with a "don't reveal the answer" line: a saved copy of it reads as today's
    // default, and saving it leaves no entry.
    QString beforeCurrentReview = defaultTemplate(Kind::Agent);
    beforeCurrentReview.remove(QStringLiteral("{current_review}\n"));
    const QString previousDefault = beforeCurrentReview
        + QStringLiteral("I'm studying this card. Don't reveal, quote or hint at the answer until I have given my own answer or ask for it; "
                         "the answer pages and source lines above are for looking it up then.\n");
    QVERIFY(defaultTemplate(Kind::Agent).endsWith(QStringLiteral("{answer_feedback}\n{current_review}\n")));
    QVERIFY(beforeCurrentReview.endsWith(QStringLiteral("{answer_feedback}\n")));
    // The default up to 2026-10-26 (no {current_review} line) reads as today's default too.
    KConfigGroup(KSharedConfig::openConfig(), configGroup()).writeEntry(configKey(Kind::Agent), beforeCurrentReview);
    QCOMPARE(savedTemplate(Kind::Agent), defaultTemplate(Kind::Agent));
    QVERIFY(!defaultTemplate(Kind::Agent).contains(QStringLiteral("I'm studying")));
    KConfigGroup(KSharedConfig::openConfig(), configGroup()).writeEntry(configKey(Kind::Agent), previousDefault);
    QCOMPARE(savedTemplate(Kind::Agent), defaultTemplate(Kind::Agent));
    saveTemplate(Kind::Agent, previousDefault);
    KSharedConfig::openConfig()->reparseConfiguration();
    QVERIFY(!KConfigGroup(KSharedConfig::openConfig(), configGroup()).hasKey(configKey(Kind::Agent)));
    QCOMPARE(savedTemplate(Kind::Agent), defaultTemplate(Kind::Agent));
    // An edit of it is the user's own template, kept as written.
    const QString edited = previousDefault + QStringLiteral("Quiz me.\n");
    saveTemplate(Kind::Agent, edited);
    KSharedConfig::openConfig()->reparseConfiguration();
    QCOMPARE(savedTemplate(Kind::Agent), edited);
    KConfigGroup(KSharedConfig::openConfig(), configGroup()).deleteGroup();
}
QTEST_MAIN(ReviewDocumentToolsTest)
#include "reviewdocumenttoolstest.moc"

void ReviewDocumentToolsTest::markdownPlacementRequiresMatchingIdentity() {
    QJsonObject proof {
        {QStringLiteral("resolver_version"), 4}, {QStringLiteral("verified"), true},
        {QStringLiteral("candidate_count"), 1}, {QStringLiteral("occurrence"), 1},
        {QStringLiteral("source_format"), QStringLiteral("markdown")},
        {QStringLiteral("method"), QStringLiteral("markdown_text_fragment")},
        {QStringLiteral("renderer_evidence_sha256"), QString(64, QLatin1Char('a'))},
        {QStringLiteral("fragment_id"), QStringLiteral("thoughts-md-fragment-1")},
        {QStringLiteral("marker_ids"), QJsonArray {QStringLiteral("comment:c1:s1")}},
    };
    QVERIFY(Thoughts::hasCompilerPlacementEvidence(proof, QStringLiteral("comment:c1"), QStringLiteral("exact_text")));
    QVERIFY(!Thoughts::hasCompilerPlacementEvidence(proof, QStringLiteral("comment:c2"), QStringLiteral("exact_text")));
    QVERIFY(!Thoughts::hasCompilerPlacementEvidence(proof, QStringLiteral("comment:c1"), QStringLiteral("exact_point")));
    proof.insert(QStringLiteral("renderer_evidence_sha256"), QString());
    QVERIFY(!Thoughts::hasCompilerPlacementEvidence(proof, QStringLiteral("comment:c1"), QStringLiteral("exact_text")));
}

void ReviewDocumentToolsTest::visualMarkdownSelectionUsesMarkdownMarkers() {
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QByteArray source("#### Response\nWe thank the reviewer.\nWe report 50% improvement with {literal braces}.\n\nNext paragraph.\n");
    for (const auto &extension : {QStringLiteral(".md"), QStringLiteral(".markdown")}) {
        const QString path = QStringLiteral("docs-test/response") + extension;
        const auto plan = Thoughts::markReviewSourceLines(path, source, {3, 1, 2, 2}, id);
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        const QByteArray start = "<!-- thoughts:v1:start review:" + id.toUtf8() + ":answer:0 -->\n";
        const QByteArray end = "<!-- thoughts:v1:end review:" + id.toUtf8() + ":answer:0 -->\n";
        QVERIFY(plan.replacement.startsWith(start));
        QVERIFY(!plan.replacement.contains("\\ReviewCardMask"));
        QByteArray recovered = plan.replacement;
        recovered.replace(start, ""); recovered.replace(end, "");
        QCOMPARE(recovered, source);
        QVERIFY(Thoughts::readReviewSourceAnchor(path, plan.replacement, id).error.isEmpty());
        const QByteArray afterList = "1. Reviewer comment.\n\n" + source;
        const auto following = Thoughts::markReviewSourceLines(path, afterList, {3, 4, 5}, id);
        QVERIFY2(following.error.isEmpty(), qPrintable(following.error));
        QVERIFY(!Thoughts::markReviewSourceLines(path, source, {1, 3}, id).error.isEmpty());
        QVERIFY(!Thoughts::markReviewSourceLines(path, source, {2}, id).error.isEmpty());
        QVERIFY(!Thoughts::markReviewSourceLines(path, "```\nLiteral text.\n```\n", {2}, id).error.isEmpty());
    }
}

void ReviewDocumentToolsTest::severalPassagesUseNumberedMarkers() {
    const auto id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString markdown = QStringLiteral("docs-test/notes.md");
    const QByteArray source("# Notes\n\nFirst answer.\n\nBetween.\n\nSecond answer\nspans two lines.\n\nEnd.\n");
    // Ranges in any order are numbered in file order.
    const auto plan = Thoughts::markReviewSourceRanges(markdown, source, {{7, 8}, {3, 3}}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    const auto marker = [&id](const char *kind, int segment) {
        return QStringLiteral("<!-- thoughts:v1:%1 review:%2:answer:%3 -->\n").arg(QLatin1String(kind), id).arg(segment).toUtf8();
    };
    QCOMPARE(plan.replacement, QByteArray("# Notes\n\n" + marker("start", 0) + "First answer.\n" + marker("end", 0) + "\nBetween.\n\n"
        + marker("start", 1) + "Second answer\nspans two lines.\n" + marker("end", 1) + "\nEnd.\n"));
    const auto read = Thoughts::readReviewSourceAnchor(markdown, plan.replacement, id);
    QVERIFY2(read.error.isEmpty(), qPrintable(read.error));
    const auto segments = read.anchor.value(QStringLiteral("segments")).toArray();
    QCOMPARE(segments.size(), 2);
    QCOMPARE(segments[1].toObject().value(QStringLiteral("segment_id")).toString(), QStringLiteral("1"));
    QCOMPARE(segments[1].toObject().value(QStringLiteral("line_start")).toInt(), 10);
    QCOMPARE(segments[1].toObject().value(QStringLiteral("content_sha256")).toString(),
        QString::fromLatin1(QCryptographicHash::hash("Second answer\nspans two lines.\n", QCryptographicHash::Sha256).toHex()));
    QCOMPARE(read.anchor, plan.anchor);
    const auto strip = Thoughts::stripReviewCardMarkers(markdown, plan.replacement, id);
    QVERIFY2(strip.error.isEmpty(), qPrintable(strip.error));
    QCOMPARE(strip.text, source);
    QCOMPARE(strip.removed, 4);
    QCOMPARE(strip.lineMap.value(3), 3);
    QCOMPARE(strip.lineMap.value(2), 0);
    // Out-of-order numbers, a repeated number, or a half pair are not this card's canonical markers.
    QByteArray swapped = plan.replacement;
    swapped.replace(":answer:0 -->", ":answer:9 -->").replace(":answer:1 -->", ":answer:0 -->").replace(":answer:9 -->", ":answer:1 -->");
    QVERIFY(!Thoughts::readReviewSourceAnchor(markdown, swapped, id).error.isEmpty());
    QVERIFY(!Thoughts::stripReviewCardMarkers(markdown, plan.replacement + marker("start", 2), id).error.isEmpty());
    // Overlapping or another card's passages are refused for the range at fault.
    int failed = -2;
    QVERIFY(!Thoughts::markReviewSourceRanges(markdown, source, {{3, 3}, {3, 5}}, id, &failed).error.isEmpty());
    QCOMPARE(failed, 1);
    const auto other = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto taken = Thoughts::markReviewSourceRanges(markdown, source, {{7, 8}}, other);
    QVERIFY(Thoughts::markReviewSourceRanges(markdown, taken.replacement, {{3, 3}}, id).error.isEmpty());
    QVERIFY(!Thoughts::markReviewSourceRanges(markdown, taken.replacement, {{3, 3}, {9, 11}}, id, &failed).error.isEmpty());
    QCOMPARE(failed, 1);

    // TeX: adjacent passages stay separate segments and read back exactly.
    const QString tex = QStringLiteral("docs-test/tex/source/main.tex");
    const QByteArray body("\\begin{document}\nOne.\nTwo.\n\nThree.\n\\end{document}\n");
    const auto adjacent = Thoughts::markReviewSourceRanges(tex, body, {{3, 3}, {2, 2}, {5, 5}}, id);
    QVERIFY2(adjacent.error.isEmpty(), qPrintable(adjacent.error));
    QCOMPARE(adjacent.anchor.value(QStringLiteral("segments")).toArray().size(), 3);
    const auto texRead = Thoughts::readReviewSourceAnchor(tex, adjacent.replacement, id);
    QVERIFY2(texRead.error.isEmpty(), qPrintable(texRead.error));
    QCOMPARE(texRead.anchor, adjacent.anchor);
    QCOMPARE(Thoughts::stripReviewCardMarkers(tex, adjacent.replacement, id).text, body);
}
