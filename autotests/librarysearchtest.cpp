/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../shell/agentproposalparser.h"
#include "../shell/librarysearch.h"

#include <QAction>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QListWidget>
#include <QToolButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QSqlDatabase>
#include <QSqlQuery>

using Thoughts::LibrarySearchHit;
using Thoughts::LibrarySearchIndex;
using Thoughts::LibrarySearchPdf;
using Thoughts::LibrarySearchRecord;
using Thoughts::LibrarySearchScope;
using Thoughts::LibrarySearchSource;
using Thoughts::LibrarySearchSourceFolder;
using Thoughts::LibrarySearchSourceUpdate;
using Thoughts::LibrarySearchUpdate;

class LibrarySearchTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testMatchExpressionKeepsOnlyWords();
    void testIndexesPdfPagesCommentsAndCards();
    void testUpdatesOnlyChangedPdfsAndKeepsUnloadedKinds();
    void testRanksExactPhrasesFirst();
    void testRanksWholeWordsAndCapitalsFirst();
    void testFieldOpensTheSelectedResult();
    void testRebuildsAnOlderIndex();
    void testListsTheSourcesSyncKeeps();
    void testVisibleSourceTextHidesMarkersAndProposals();
    void testFindsSourcesByPathAndText();
    void testUpdatesSourcesIncrementally();
    void testScopesSeparateDocumentsAndSources();
    void testFieldSwitchesScope();
    void testIndexesAGeneratedLibrary();

private:
    static QString pdf(const QString &name)
    {
        return QStringLiteral(KDESRCDIR "data/") + name;
    }
    static QList<LibrarySearchRecord> records()
    {
        return {{QStringLiteral("comment"), QStringLiteral("c1"), pdf(QStringLiteral("file2.pdf")), 2, QString(), QStringLiteral("Nice check of the teacher prefill latency"),
                 QStringLiteral("Second file")},
                {QStringLiteral("note"), QStringLiteral("n1"), QString(), 0, QString(), QStringLiteral("Plan for the distillation paper"), QStringLiteral("Note")},
                {QStringLiteral("card"), QStringLiteral("k1"), pdf(QStringLiteral("file1.pdf")), 1, QStringLiteral("What is the main experiment?"),
                 QStringLiteral("Qwen3 normalised score"), QStringLiteral("First file · p. 1")}};
    }
    static QStringList allKinds()
    {
        return {QStringLiteral("comment"), QStringLiteral("note"), QStringLiteral("card")};
    }
    static void write(const QString &path, const QByteArray &content)
    {
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(file.write(content), content.size());
    }
    // Moves a file's modification time, as an edit within the same second could leave it.
    static void touch(const QString &path, int seconds)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadWrite));
        QVERIFY(file.setFileTime(QFileInfo(path).lastModified().addSecs(seconds), QFileDevice::FileModificationTime));
    }
    static QString latexProposal()
    {
        Thoughts::AgentProposedEdit proposal;
        proposal.id = QStringLiteral("p1");
        proposal.createdAt = QStringLiteral("2026-10-14T10:00:00.000Z");
        proposal.conversationId = QStringLiteral("0123abcd-0000-4000-8000-0123456789ab");
        proposal.explanation = QStringLiteral("Clarity matters here");
        proposal.original = QStringLiteral("obsolete wording");
        proposal.replacement = QStringLiteral("improved wording");
        return Thoughts::formatLatexAgentProposal(proposal);
    }
    static QString markdownProposal()
    {
        Thoughts::AgentProposedEdit proposal;
        proposal.id = QStringLiteral("p2");
        proposal.createdAt = QStringLiteral("2026-10-14T10:00:00.000Z");
        proposal.conversationId = QStringLiteral("0123abcd-0000-4000-8000-0123456789ab");
        proposal.explanation = QStringLiteral("Shorter is better");
        proposal.original = QStringLiteral("verbose paragraph");
        proposal.replacement = QStringLiteral("crisp paragraph");
        return Thoughts::formatMarkdownAgentProposal(proposal);
    }
    // A small Library: one connected folder with ignored, binary and text files.
    static void makeLibrary(const QString &root)
    {
        const QString folder = root + QStringLiteral("/docs-paper");
        write(folder + QStringLiteral("/.ThoughtIgnore"), "build/\narchive/\n*.log\n");
        write(folder + QStringLiteral("/tex/main.tex"),
              QStringLiteral("\\documentclass{article}\n"
                             "\\begin{document}\n"
                             "\\input{benchmark}\n"
                             "\n"
                             "We train with gradient descent on the toy task.\n"
                             "\\ReviewCardMaskStart{card-42}{0}%\n"
                             "The answer is forty two.\n"
                             "\\ReviewCardMaskEnd{card-42}{0}%\n"
                             "This sentence has %1 in it.\n"
                             "\\end{document}\n")
                  .arg(latexProposal())
                  .toUtf8());
        write(folder + QStringLiteral("/tex/benchmark.tex"), "\\section{Results}\nScores on the leaderboard table.\n");
        write(folder + QStringLiteral("/refs.bib"), "@article{smith2020, title={Quantum gardening}}\n");
        write(folder + QStringLiteral("/notes.md"),
              QStringLiteral("# Notes\n\n<!-- thoughts:v1:start review:card-7:answer:0 -->\nMarkdown answer about lighthouses.\n"
                             "<!-- thoughts:v1:end review:card-7:answer:0 -->\n\n<!-- a private remark about otters -->\n\n%1")
                  .arg(markdownProposal())
                  .toUtf8());
        write(folder + QStringLiteral("/figure.png"), QByteArray("\x89PNG\r\n\x1a\n\0\0gradient", 16));
        write(folder + QStringLiteral("/build/main.tex"), "Build output mentions gradient descent too.\n");
        write(folder + QStringLiteral("/archive/old.tex"), "Archived gradient descent draft.\n");
        write(folder + QStringLiteral("/run.log"), "gradient descent log\n");
    }
    static QList<LibrarySearchSourceFolder> folders()
    {
        return {{QStringLiteral("docs-paper"), QStringLiteral("Paper")}};
    }
    static QStringList relativePaths(const QList<LibrarySearchSource> &sources)
    {
        QStringList paths;
        for (const LibrarySearchSource &source : sources) paths.append(source.relativePath);
        paths.sort();
        return paths;
    }
    static QStringList kinds(const QList<LibrarySearchHit> &hits)
    {
        QStringList result;
        for (const LibrarySearchHit &hit : hits) result.append(hit.kind);
        return result;
    }
};

void LibrarySearchTest::testMatchExpressionKeepsOnlyWords()
{
    QCOMPARE(LibrarySearchIndex::matchExpression(QStringLiteral("  teacher \"pre")), QStringLiteral("\"teacher\" \"pre\"*"));
    QCOMPARE(LibrarySearchIndex::matchExpression(QStringLiteral("a-b OR c*")), QStringLiteral("\"a\" \"b\" \"OR\" \"c\"*"));
    QVERIFY(LibrarySearchIndex::matchExpression(QStringLiteral(" \"*() ")).isEmpty());
}

void LibrarySearchTest::testIndexesPdfPagesCommentsAndCards()
{
    QTemporaryDir dir;
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    QVERIFY(index.isEmpty());
    QVERIFY(index.search(QStringLiteral("anything")).isEmpty());
    const QList<LibrarySearchPdf> pdfs{{pdf(QStringLiteral("file1.pdf")), QStringLiteral("First file"), QStringLiteral("Tests"), QStringLiteral("a")},
                                       {pdf(QStringLiteral("file2.pdf")), QStringLiteral("Second file"), QStringLiteral("Tests"), QStringLiteral("b")}};
    LibrarySearchUpdate summary;
    QString error;
    QVERIFY2(index.update(pdfs, records(), allKinds(), &summary, &error), qPrintable(error));
    QCOMPARE(summary.indexedPdfs, 2);
    QCOMPARE(summary.records, 3);
    QVERIFY(!index.isEmpty());

    // PDF text matches by document, naming its best page, and words match as prefixes.
    QList<LibrarySearchHit> hits = index.search(QStringLiteral("roc"));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().kind, QStringLiteral("document"));
    QCOMPARE(hits.first().pdfPath, pdf(QStringLiteral("file2.pdf")));
    QCOMPARE(hits.first().page, 2);
    QCOMPARE(hits.first().matchingPages, 1);
    QCOMPARE(hits.first().title, QStringLiteral("Second file"));
    QVERIFY(hits.first().snippet.contains(QChar(1)));
    // Every word must occur.
    QVERIFY(index.search(QStringLiteral("kde hola")).isEmpty());
    QCOMPARE(index.search(QStringLiteral("random silly")).size(), 1);

    // Document names, comments, notes and cards.
    hits = index.search(QStringLiteral("second"));
    QCOMPARE(hits.first().kind, QStringLiteral("document"));
    QCOMPARE(index.search(QStringLiteral("prefill")).first().ref, QStringLiteral("c1"));
    QCOMPARE(index.search(QStringLiteral("distil")).first().kind, QStringLiteral("note"));
    hits = index.search(QStringLiteral("normalised"));
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().kind, QStringLiteral("card"));
    QCOMPARE(hits.first().title, QStringLiteral("What is the main experiment?"));
    // Without a Library name, a PDF's own title names it everywhere.
    QCOMPARE(hits.first().context, QStringLiteral("Silly File · p. 1"));
    QCOMPARE(index.search(QStringLiteral("silly file")).first().kind, QStringLiteral("document"));
    QVERIFY(index.search(QStringLiteral("first")).isEmpty());
    QCOMPARE(index.search(QStringLiteral("experiment")).first().ref, QStringLiteral("k1"));
    // Finished words match by stem; only the last word is a prefix.
    QCOMPARE(index.search(QStringLiteral("prefills lat")).first().ref, QStringLiteral("c1"));
    QVERIFY(index.search(QStringLiteral("pre latency")).isEmpty());
}

void LibrarySearchTest::testRanksExactPhrasesFirst()
{
    QTemporaryDir dir;
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    const QList<LibrarySearchRecord> items{
        {QStringLiteral("comment"), QStringLiteral("c1"), QString(), 0, QString(), QStringLiteral("What next? The plan is unclear, as is what the team wants."), QString()},
        {QStringLiteral("comment"), QStringLiteral("c2"), QString(), 0, QString(), QStringLiteral("I asked what is the plan for Monday."), QString()},
        {QStringLiteral("card"), QStringLiteral("k1"), QString(), 0, QStringLiteral("What is the plan?"), QString(), QString()}};
    QVERIFY(index.update({}, items, allKinds(), nullptr, nullptr));
    const QList<LibrarySearchHit> hits = index.search(QStringLiteral("what is the plan"));
    QCOMPARE(hits.size(), 3);
    // The card's question is the phrase itself, so its group leads; within
    // comments the exact phrase comes before the scattered words.
    QCOMPARE(hits.at(0).ref, QStringLiteral("k1"));
    QCOMPARE(hits.at(1).ref, QStringLiteral("c2"));
    QCOMPARE(hits.at(2).ref, QStringLiteral("c1"));
}

void LibrarySearchTest::testRanksWholeWordsAndCapitalsFirst()
{
    QTemporaryDir dir;
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    const auto page = [](const QString &pdfPath, int number, const QString &body) {
        return LibrarySearchRecord{QStringLiteral("page"), pdfPath + QString::number(number), pdfPath, number, QString(), body, QString()};
    };
    const QString method = QStringLiteral("/method.pdf");
    const QString menu = QStringLiteral("/menu.pdf");
    const QString basin = QStringLiteral("/basin.pdf");
    const QString paper = QStringLiteral("/paper.pdf");
    const QString survey = QStringLiteral("/survey.pdf");
    QList<LibrarySearchRecord> items{
        page(basin, 1, QStringLiteral("barrier barrier barrier barrier barrier barrier")),
        page(menu, 1, QStringLiteral("the menu bar sits at the top of the screen")),
        page(method, 3, QStringLiteral("A long page with a barrier first, that names the BAR method once, then a bar, among many other words of text")),
        {QStringLiteral("document"), survey, survey, 0, QStringLiteral("BAR in practice"), QStringLiteral("Reading"), QStringLiteral("Reading")},
    };
    for (int number = 1; number <= 5; ++number) items.append(page(paper, number, QStringLiteral("BAR BAR BAR")));
    QVERIFY(index.update({}, items, {QStringLiteral("page"), QStringLiteral("document")}, nullptr, nullptr));
    const auto paths = [](const QList<LibrarySearchHit> &hits) {
        QStringList result;
        for (const LibrarySearchHit &hit : hits) result.append(hit.pdfPath);
        return result;
    };

    // One row for each document. Capitals mean those letters; a name holding the words comes first.
    QList<LibrarySearchHit> hits = index.search(QStringLiteral("BAR"));
    QCOMPARE(paths(hits), (QStringList{survey, paper, method, menu, basin}));
    QCOMPARE(hits.at(0).matchingPages, 0);
    QCOMPARE(hits.at(0).page, 0);
    QCOMPARE(hits.at(1).matchingPages, 5);
    QCOMPARE(hits.at(1).matches, 15);
    // The row opens the page with the word and shows it, not the first word starting like it.
    QCOMPARE(hits.at(2).matchingPages, 1);
    // Only the capitals count, not the bar or the barrier on the same page.
    QCOMPARE(hits.at(2).matches, 1);
    QCOMPARE(hits.at(2).page, 3);
    QVERIFY2(hits.at(2).snippet.contains(QStringLiteral("\x01" "BAR\x02")), qPrintable(hits.at(2).snippet));

    // Lower case is no choice of letters: whole words first, the most pages among them.
    hits = index.search(QStringLiteral("bar"));
    QCOMPARE(hits.size(), 5);
    QCOMPARE(hits.at(0).pdfPath, survey);
    QCOMPARE(hits.at(1).pdfPath, paper);
    QCOMPARE(hits.at(2).pdfPath, method);
    QCOMPARE(hits.at(2).matches, 2);
    QCOMPARE(hits.constLast().pdfPath, basin);

    // A word still being typed finds what starts with it.
    hits = index.search(QStringLiteral("barr"));
    QCOMPARE(hits.size(), 2);
    QVERIFY(hits.constFirst().snippet.contains(QStringLiteral("\x01" "barrier\x02")));

    // At most 20 documents, the best ones.
    QList<LibrarySearchRecord> many;
    for (int number = 0; number < 25; ++number) many.append(page(QStringLiteral("/many%1.pdf").arg(number), 1, QStringLiteral("lighthouse")));
    QVERIFY(index.update({}, many, {QStringLiteral("page")}, nullptr, nullptr));
    QCOMPARE(index.search(QStringLiteral("lighthouse")).size(), 20);
}

void LibrarySearchTest::testUpdatesOnlyChangedPdfsAndKeepsUnloadedKinds()
{
    QTemporaryDir dir;
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    QList<LibrarySearchPdf> pdfs{{pdf(QStringLiteral("file1.pdf")), QStringLiteral("First file"), QString(), QStringLiteral("a")},
                                 {pdf(QStringLiteral("file2.pdf")), QStringLiteral("Second file"), QString(), QStringLiteral("b")}};
    LibrarySearchUpdate summary;
    QVERIFY(index.update(pdfs, records(), allKinds(), &summary, nullptr));
    QVERIFY(index.update(pdfs, records(), allKinds(), &summary, nullptr));
    QCOMPARE(summary.indexedPdfs, 0);

    // A Library name wins over the PDF's title, and a rename needs no re-extraction.
    pdfs[0].libraryTitle = QStringLiteral("Renamed file");
    QVERIFY(index.update(pdfs, records(), allKinds(), &summary, nullptr));
    QCOMPARE(summary.indexedPdfs, 0);
    QCOMPARE(index.search(QStringLiteral("renamed")).first().kind, QStringLiteral("document"));
    // The old name is only the PDF's text now.
    QVERIFY(index.search(QStringLiteral("silly file")).first().matchingPages > 0);
    QCOMPARE(index.search(QStringLiteral("normalised")).first().context, QStringLiteral("Renamed file · p. 1"));

    // A changed identity re-extracts that PDF only; an unlisted PDF is dropped.
    pdfs[0].identity = QStringLiteral("a2");
    pdfs.removeLast();
    // Cards have not loaded: their stored records stay.
    QVERIFY(index.update(pdfs, {records().first()}, {QStringLiteral("comment"), QStringLiteral("note")}, &summary, nullptr));
    QCOMPARE(summary.indexedPdfs, 1);
    QCOMPARE(summary.removedPdfs, 1);
    QVERIFY(index.search(QStringLiteral("rocks")).isEmpty());
    QCOMPARE(index.search(QStringLiteral("renamed")).first().kind, QStringLiteral("document"));
    QVERIFY(index.search(QStringLiteral("distillation")).isEmpty());
    QCOMPARE(index.search(QStringLiteral("normalised")).size(), 1);
}

void LibrarySearchTest::testFieldOpensTheSelectedResult()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("search.sqlite"));
    QVERIFY(LibrarySearchIndex(path).update({{pdf(QStringLiteral("file2.pdf")), QStringLiteral("Second file"), QString(), QStringLiteral("b")}}, records(),
                                            allKinds(), nullptr, nullptr));
    QWidget window;
    window.resize(800, 600);
    auto *field = new Thoughts::LibrarySearchField(path, &window);
    field->setGeometry(10, 10, 300, 28);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    field->setFocus();
    QSignalSpy activated(field, &Thoughts::LibrarySearchField::hitActivated);
    QTest::keyClicks(field, QStringLiteral("nice"));
    // Results follow a short typing delay.
    QListWidget *results = nullptr;
    QTRY_VERIFY((results = window.findChild<QListWidget *>(QStringLiteral("librarySearchResults"))) && results->isVisible());
    // Two groups of one result each; the first result is selected and ↑/↓ skip headings.
    QCOMPARE(results->count(), 4);
    QCOMPARE(results->currentRow(), 1);
    // A document's row says how often and on how many pages.
    const auto rowHtml = [results](int row) {
        return results->item(row)->data(Qt::UserRole + 2).toString();
    };
    QVERIFY2(rowHtml(1).contains(QStringLiteral(" match · 1 page")) || rowHtml(3).contains(QStringLiteral(" match · 1 page"))
                 || rowHtml(1).contains(QStringLiteral(" matches · 1 page")) || rowHtml(3).contains(QStringLiteral(" matches · 1 page")),
             qPrintable(rowHtml(1) + QLatin1Char('\n') + rowHtml(3)));
    QTest::keyClick(field, Qt::Key_Down);
    QCOMPARE(results->currentRow(), 3);
    QTest::keyClick(field, Qt::Key_Up);
    QCOMPARE(results->currentRow(), 1);
    // Return opens the selected result.
    field->clear();
    QTest::keyClicks(field, QStringLiteral("check"));
    QTRY_COMPARE(results->count(), 2);
    QTest::keyClick(field, Qt::Key_Return);
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.first().at(0).value<LibrarySearchHit>().ref, QStringLiteral("c1"));
    QCOMPARE(activated.first().at(1).toString(), QStringLiteral("check"));
    QVERIFY(!results->isVisible());
    QVERIFY(field->hasFocus());
    QTest::keyClick(field, Qt::Key_Escape);
    QVERIFY(field->text().isEmpty());
}

void LibrarySearchTest::testRebuildsAnOlderIndex()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("search.sqlite"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("old-index"));
        db.setDatabaseName(path);
        QVERIFY(db.open());
        QSqlQuery query(db);
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT NOT NULL)")));
        QVERIFY(query.exec(QStringLiteral("INSERT INTO meta VALUES ('schema', '1')")));
        QVERIFY(query.exec(QStringLiteral("CREATE TABLE indexed_pdfs(pdf_path TEXT PRIMARY KEY, identity TEXT NOT NULL)")));
        QVERIFY(query.exec(QStringLiteral("CREATE VIRTUAL TABLE entries USING fts5(kind UNINDEXED, body)")));
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("old-index"));
    QString error;
    QVERIFY2(LibrarySearchIndex(path).update({{pdf(QStringLiteral("file2.pdf")), QStringLiteral("Second file"), QString(), QStringLiteral("b")}}, records(),
                                             allKinds(), nullptr, &error),
             qPrintable(error));
    QCOMPARE(LibrarySearchIndex(path).search(QStringLiteral("rocks")).size(), 1);
}

void LibrarySearchTest::testListsTheSourcesSyncKeeps()
{
    QTemporaryDir dir;
    makeLibrary(dir.path());
    const QList<LibrarySearchSource> sources = LibrarySearchIndex::listSources(dir.path(), folders());
    // .ThoughtIgnore keeps build/, archive/ and logs out; images are not text.
    QCOMPARE(relativePaths(sources),
             (QStringList{QStringLiteral("docs-paper/.ThoughtIgnore"), QStringLiteral("docs-paper/notes.md"), QStringLiteral("docs-paper/refs.bib"),
                          QStringLiteral("docs-paper/tex/benchmark.tex"), QStringLiteral("docs-paper/tex/main.tex")}));
    for (const LibrarySearchSource &source : sources) {
        QCOMPARE(source.project, QStringLiteral("Paper"));
        QVERIFY(QFileInfo(source.path).isAbsolute());
        QVERIFY(!source.identity.isEmpty());
    }
    // A new rule applies at the next listing; unknown folders list nothing.
    write(dir.filePath(QStringLiteral("docs-paper/.ThoughtIgnore")), "build/\narchive/\n*.log\n*.bib\n");
    QVERIFY(!relativePaths(LibrarySearchIndex::listSources(dir.path(), folders())).contains(QStringLiteral("docs-paper/refs.bib")));
    QVERIFY(LibrarySearchIndex::listSources(dir.path(), {{QStringLiteral("docs-missing"), QStringLiteral("Gone")}}).isEmpty());
    // The inventory cache of the Library is only read.
    QVERIFY(!QFileInfo::exists(dir.filePath(QStringLiteral(".thoughts/library-source-inventory.json"))));
}

void LibrarySearchTest::testVisibleSourceTextHidesMarkersAndProposals()
{
    const QString tex = QStringLiteral("Intro line.\n\\ReviewCardMaskStart{card-42}{0}%\nAnswer text.\n\\ReviewCardMaskEnd{card-42}{0}%\nWe say %1 now.\n")
                            .arg(latexProposal());
    const QString visibleTex = LibrarySearchIndex::visibleSourceText(QStringLiteral("/x/main.tex"), tex);
    // Same length and lines, so offsets and line numbers still point into the file.
    QCOMPARE(visibleTex.size(), tex.size());
    QCOMPARE(visibleTex.count(QLatin1Char('\n')), tex.count(QLatin1Char('\n')));
    QVERIFY(visibleTex.contains(QStringLiteral("Answer text.")));
    QVERIFY(visibleTex.contains(QStringLiteral("improved wording")));
    for (const QString &hidden : {QStringLiteral("ReviewCardMask"), QStringLiteral("card-42"), QStringLiteral("agentProposedEdit"),
                                  QStringLiteral("obsolete"), QStringLiteral("Clarity"), QStringLiteral("conversation_id")}) {
        QVERIFY2(!visibleTex.contains(hidden), qPrintable(hidden));
    }
    QCOMPARE(visibleTex.split(QLatin1Char('\n')).at(1).trimmed(), QString());

    const QString md = QStringLiteral("<!-- thoughts:v1:start review:card-7:answer:0 -->\nLighthouses.\n<!-- thoughts:v1:end review:card-7:answer:0 -->\n"
                                      "<!-- my own remark -->\n%1")
                           .arg(markdownProposal());
    const QString visibleMd = LibrarySearchIndex::visibleSourceText(QStringLiteral("/x/notes.md"), md);
    QCOMPARE(visibleMd.size(), md.size());
    QCOMPARE(visibleMd.count(QLatin1Char('\n')), md.count(QLatin1Char('\n')));
    QVERIFY(visibleMd.contains(QStringLiteral("Lighthouses.")));
    QVERIFY(visibleMd.contains(QStringLiteral("crisp paragraph")));
    QVERIFY(visibleMd.contains(QStringLiteral("<!-- my own remark -->")));
    for (const QString &hidden : {QStringLiteral("thoughts:"), QStringLiteral("card-7"), QStringLiteral("verbose"), QStringLiteral("Shorter")}) {
        QVERIFY2(!visibleMd.contains(hidden), qPrintable(hidden));
    }
    // Other files are searched as written.
    QCOMPARE(LibrarySearchIndex::visibleSourceText(QStringLiteral("/x/style.sty"), tex), tex);
}

void LibrarySearchTest::testFindsSourcesByPathAndText()
{
    QTemporaryDir dir;
    makeLibrary(dir.path());
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    QVERIFY(!index.hasSources());
    LibrarySearchSourceUpdate summary;
    QString error;
    QVERIFY2(index.updateSources(LibrarySearchIndex::listSources(dir.path(), folders()), &summary, &error), qPrintable(error));
    QVERIFY(index.hasSources());
    QCOMPARE(summary.files, 5);
    QCOMPARE(summary.read, 5);

    // A file name: the file itself leads, then lines that mention it.
    QList<LibrarySearchHit> hits = index.search(QStringLiteral("benchmark.tex"), LibrarySearchScope::Sources);
    QVERIFY(!hits.isEmpty());
    QCOMPARE(hits.first().kind, QStringLiteral("source"));
    QCOMPARE(hits.first().title, QStringLiteral("docs-paper/tex/benchmark.tex"));
    QCOMPARE(hits.first().context, QStringLiteral("Paper"));
    QCOMPARE(hits.first().sourcePath, QFileInfo(dir.filePath(QStringLiteral("docs-paper/tex/benchmark.tex"))).absoluteFilePath());
    QVERIFY(hits.first().snippet.contains(QChar(1)));
    hits = index.search(QStringLiteral("benchmark"), LibrarySearchScope::Sources);
    QCOMPARE(hits.first().kind, QStringLiteral("source"));
    QVERIFY(kinds(hits).contains(QStringLiteral("source-line")));

    // Text: the exact line, with the matched words marked.
    hits = index.search(QStringLiteral("gradient descent"), LibrarySearchScope::Sources);
    QCOMPARE(hits.size(), 1); // build/, archive/ and logs are ignored
    QCOMPARE(hits.first().kind, QStringLiteral("source-line"));
    QCOMPARE(hits.first().title, QStringLiteral("docs-paper/tex/main.tex"));
    QCOMPARE(hits.first().line, 5);
    QCOMPARE(QString(hits.first().snippet).remove(QChar(1)).remove(QChar(2)), QStringLiteral("We train with gradient descent on the toy task."));
    // Within a block the line with the most matched words wins.
    hits = index.search(QStringLiteral("forty two"), LibrarySearchScope::Sources);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().line, 7);
    QCOMPARE(index.search(QStringLiteral("quantum gardening"), LibrarySearchScope::Sources).first().line, 1);
    QCOMPARE(index.search(QStringLiteral("leaderboard"), LibrarySearchScope::Sources).first().line, 2);

    // Proposals show their replacement; markers and hidden text are not found.
    hits = index.search(QStringLiteral("improved wording"), LibrarySearchScope::Sources);
    QCOMPARE(hits.size(), 1);
    // The wrapper's explanation ends with a line break, so the replacement is on the next line.
    QCOMPARE(hits.first().line, 10);
    QCOMPARE(QString(hits.first().snippet).remove(QChar(1)).remove(QChar(2)), QStringLiteral("improved wording  in it."));
    QVERIFY(!QString(hits.first().snippet).contains(QStringLiteral("agentProposedEdit")));
    for (const QString &hidden : {QStringLiteral("obsolete"), QStringLiteral("clarity"), QStringLiteral("ReviewCardMaskStart"), QStringLiteral("card"),
                                  QStringLiteral("verbose"), QStringLiteral("shorter")}) {
        QVERIFY2(index.search(hidden, LibrarySearchScope::Sources).isEmpty(), qPrintable(hidden));
    }
    QCOMPARE(index.search(QStringLiteral("crisp"), LibrarySearchScope::Sources).size(), 1);
    QCOMPARE(index.search(QStringLiteral("lighthouses"), LibrarySearchScope::Sources).first().line, 4);
    // Plain HTML comments are the author's own text.
    QCOMPARE(index.search(QStringLiteral("otters"), LibrarySearchScope::Sources).size(), 1);
}

void LibrarySearchTest::testUpdatesSourcesIncrementally()
{
    QTemporaryDir dir;
    makeLibrary(dir.path());
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    LibrarySearchSourceUpdate summary;
    const auto update = [&] {
        QString error;
        QVERIFY2(index.updateSources(LibrarySearchIndex::listSources(dir.path(), folders()), &summary, &error), qPrintable(error));
    };
    update();
    QCOMPARE(summary.read, 5);
    // Nothing changed: nothing is read.
    update();
    QCOMPARE(summary.read, 0);
    QCOMPARE(summary.removed, 0);

    // An edit re-reads that file only.
    const QString benchmark = dir.filePath(QStringLiteral("docs-paper/tex/benchmark.tex"));
    write(benchmark, "\\section{Results}\nScores on the podium table.\n");
    touch(benchmark, 5);
    update();
    QCOMPARE(summary.read, 1);
    QCOMPARE(summary.unchanged, 0);
    QVERIFY(index.search(QStringLiteral("leaderboard"), LibrarySearchScope::Sources).isEmpty());
    QCOMPARE(index.search(QStringLiteral("podium"), LibrarySearchScope::Sources).first().line, 2);

    // A touched but unchanged file keeps its rows.
    touch(benchmark, 5);
    update();
    QCOMPARE(summary.read, 1);
    QCOMPARE(summary.unchanged, 1);
    QCOMPARE(index.search(QStringLiteral("podium"), LibrarySearchScope::Sources).size(), 1);

    // A deleted file is dropped.
    QVERIFY(QFile::remove(dir.filePath(QStringLiteral("docs-paper/refs.bib"))));
    update();
    QCOMPARE(summary.removed, 1);
    QVERIFY(index.search(QStringLiteral("gardening"), LibrarySearchScope::Sources).isEmpty());
    QVERIFY(index.search(QStringLiteral("refs"), LibrarySearchScope::Sources).isEmpty());

    // A renamed file is found under its new path only.
    QVERIFY(QFile::rename(dir.filePath(QStringLiteral("docs-paper/notes.md")), dir.filePath(QStringLiteral("docs-paper/ideas.md"))));
    update();
    QCOMPARE(summary.removed, 1);
    QCOMPARE(summary.read, 1);
    QList<LibrarySearchHit> hits = index.search(QStringLiteral("lighthouses"), LibrarySearchScope::Sources);
    QCOMPARE(hits.size(), 1);
    QCOMPARE(hits.first().title, QStringLiteral("docs-paper/ideas.md"));
    QVERIFY(index.search(QStringLiteral("notes.md"), LibrarySearchScope::Sources).isEmpty());
    QCOMPARE(index.search(QStringLiteral("ideas"), LibrarySearchScope::Sources).first().kind, QStringLiteral("source"));

    // A file that becomes ignored leaves the index.
    write(dir.filePath(QStringLiteral("docs-paper/.ThoughtIgnore")), "build/\narchive/\n*.log\ntex/\n");
    update();
    QCOMPARE(summary.removed, 2);
    QVERIFY(index.search(QStringLiteral("gradient"), LibrarySearchScope::Sources).isEmpty());
    // A project rename relabels files without changing them.
    QVERIFY(index.updateSources(LibrarySearchIndex::listSources(dir.path(), {{QStringLiteral("docs-paper"), QStringLiteral("Report")}}), &summary, nullptr));
    QCOMPARE(index.search(QStringLiteral("lighthouses"), LibrarySearchScope::Sources).first().context, QStringLiteral("Report"));
}

void LibrarySearchTest::testScopesSeparateDocumentsAndSources()
{
    QTemporaryDir dir;
    makeLibrary(dir.path());
    write(dir.filePath(QStringLiteral("docs-paper/tex/prefill.tex")), "The teacher prefill latency matters.\n");
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    QVERIFY(index.update({{pdf(QStringLiteral("file2.pdf")), QStringLiteral("Second file"), QString(), QStringLiteral("b")}}, records(), allKinds(), nullptr,
                         nullptr));
    // Documents only: sources never indexed yet do not get in the way.
    QCOMPARE(kinds(index.search(QStringLiteral("prefill"))), QStringList{QStringLiteral("comment")});
    QCOMPARE(kinds(index.search(QStringLiteral("prefill"), LibrarySearchScope::Sources)), QStringList());
    QVERIFY(index.updateSources(LibrarySearchIndex::listSources(dir.path(), folders()), nullptr, nullptr));
    QCOMPARE(kinds(index.search(QStringLiteral("prefill"), LibrarySearchScope::Documents)), QStringList{QStringLiteral("comment")});
    QCOMPARE(kinds(index.search(QStringLiteral("teacher prefill"), LibrarySearchScope::Sources)),
             (QStringList{QStringLiteral("source-line")}));
    QStringList all = kinds(index.search(QStringLiteral("prefill"), LibrarySearchScope::All));
    all.sort();
    QCOMPARE(all, (QStringList{QStringLiteral("comment"), QStringLiteral("source"), QStringLiteral("source-line")}));
    // Updating PDFs keeps the sources and the other way round.
    QVERIFY(index.update({}, {}, allKinds(), nullptr, nullptr));
    QCOMPARE(kinds(index.search(QStringLiteral("teacher prefill"), LibrarySearchScope::Sources)), QStringList{QStringLiteral("source-line")});
    QVERIFY(index.search(QStringLiteral("prefill"), LibrarySearchScope::Documents).isEmpty());
}

void LibrarySearchTest::testFieldSwitchesScope()
{
    QTemporaryDir dir;
    makeLibrary(dir.path());
    write(dir.filePath(QStringLiteral("docs-paper/tex/prefill.tex")), "Line one.\nThe teacher prefill latency matters.\n");
    const QString path = dir.filePath(QStringLiteral("search.sqlite"));
    QVERIFY(LibrarySearchIndex(path).update({{pdf(QStringLiteral("file2.pdf")), QStringLiteral("Second file"), QString(), QStringLiteral("b")}}, records(),
                                            allKinds(), nullptr, nullptr));
    QVERIFY(LibrarySearchIndex(path).updateSources(LibrarySearchIndex::listSources(dir.path(), folders()), nullptr, nullptr));
    QWidget window;
    window.resize(800, 600);
    auto *field = new Thoughts::LibrarySearchField(path, &window);
    field->setGeometry(10, 10, 400, 28);
    window.show();
    QVERIFY(QTest::qWaitForWindowExposed(&window));
    auto *button = field->findChild<QToolButton *>(QStringLiteral("librarySearchScope"));
    QVERIFY(button);
    QVERIFY(button->text().startsWith(QStringLiteral("Documents")));
    // Typed text starts to the right of the scope button.
    QVERIFY(field->textMargins().left() >= button->width());
    field->setFocus();
    QSignalSpy activated(field, &Thoughts::LibrarySearchField::hitActivated);
    QSignalSpy scopes(field, &Thoughts::LibrarySearchField::scopeChanged);
    QTest::keyClicks(field, QStringLiteral("teacher prefill"));
    QListWidget *results = nullptr;
    QTRY_VERIFY((results = window.findChild<QListWidget *>(QStringLiteral("librarySearchResults"))) && results->isVisible());
    const auto rows = [&results] {
        QStringList text;
        for (int row = 0; row < results->count(); ++row) text.append(results->item(row)->data(Qt::UserRole + 2).toString());
        return text.join(QLatin1Char('\n'));
    };
    QCOMPARE(results->count(), 2);
    QVERIFY(rows().contains(QStringLiteral("Comments and notes")));
    QVERIFY(!rows().contains(QStringLiteral("Source text")));

    // ⌘2 searches the sources at once, keeping the query.
    QTest::keyClick(field, Qt::Key_2, Qt::ControlModifier);
    QCOMPARE(field->scope(), LibrarySearchScope::Sources);
    QCOMPARE(scopes.size(), 1);
    QCOMPARE(field->text(), QStringLiteral("teacher prefill"));
    QVERIFY(results->isVisible());
    QVERIFY(rows().contains(QStringLiteral("Source text")));
    QVERIFY(rows().contains(QStringLiteral("Line 2")));
    QVERIFY(!rows().contains(QStringLiteral("Comments and notes")));
    QVERIFY(button->text().startsWith(QStringLiteral("Sources")));
    const QString screenshot = qEnvironmentVariable("THOUGHTS_LIBRARY_SEARCH_SCREENSHOT");
    if (!screenshot.isEmpty()) QVERIFY(window.grab().save(screenshot));

    // The menu's "All" shows both.
    auto *all = button->findChild<QAction *>(QStringLiteral("librarySearchScope-all"));
    QVERIFY(all);
    all->trigger();
    QCOMPARE(field->scope(), LibrarySearchScope::All);
    QVERIFY(all->isChecked());
    QTRY_VERIFY(results->isVisible());
    QVERIFY(rows().contains(QStringLiteral("Source text")));
    QVERIFY(rows().contains(QStringLiteral("Comments and notes")));

    // Return on a source line opens that file at the line.
    QTest::keyClick(field, Qt::Key_1, Qt::ControlModifier);
    QCOMPARE(field->scope(), LibrarySearchScope::Documents);
    QTest::keyClick(field, Qt::Key_2, Qt::ControlModifier);
    QTRY_VERIFY(results->isVisible());
    QTest::keyClick(field, Qt::Key_Return);
    QCOMPARE(activated.size(), 1);
    const LibrarySearchHit hit = activated.first().at(0).value<LibrarySearchHit>();
    QCOMPARE(hit.kind, QStringLiteral("source-line"));
    QCOMPARE(hit.line, 2);
    QCOMPARE(hit.sourcePath, QFileInfo(dir.filePath(QStringLiteral("docs-paper/tex/prefill.tex"))).absoluteFilePath());
    QCOMPARE(scopes.size(), 4);
    // Choosing the current scope again changes nothing.
    field->setScope(LibrarySearchScope::Sources);
    QCOMPARE(scopes.size(), 4);
}

void LibrarySearchTest::testIndexesAGeneratedLibrary()
{
    // About the size of a real Library: 400 TeX/Markdown/BibTeX files in 8 projects.
    QTemporaryDir dir;
    QList<LibrarySearchSourceFolder> projects;
    const QStringList vocabulary = QStringLiteral("model training gradient benchmark latency teacher student evaluation dataset results "
                                                  "baseline ablation figure table section appendix proof lemma theorem method")
                                       .split(QLatin1Char(' '));
    qint64 bytes = 0;
    for (int project = 0; project < 8; ++project) {
        const QString owner = QStringLiteral("docs-project%1").arg(project);
        projects.append({owner, QStringLiteral("Project %1").arg(project)});
        write(dir.filePath(owner + QStringLiteral("/.ThoughtIgnore")), "build/\n");
        for (int file = 0; file < 50; ++file) {
            const QString extension = file % 10 == 0 ? QStringLiteral("md") : file % 10 == 1 ? QStringLiteral("bib") : QStringLiteral("tex");
            QString text;
            for (int line = 0; line < 120; ++line) {
                if (line % 7 == 6) {
                    text += QLatin1Char('\n');
                    continue;
                }
                for (int word = 0; word < 12; ++word) {
                    text += vocabulary.at((project * 31 + file * 17 + line * 7 + word * 3) % vocabulary.size()) + QLatin1Char(' ');
                }
                text += QStringLiteral("unique%1x%2x%3\n").arg(project).arg(file).arg(line);
            }
            const QByteArray content = text.toUtf8();
            bytes += content.size();
            write(dir.filePath(owner + QStringLiteral("/sec/file%1.%2").arg(file).arg(extension)), content);
        }
        write(dir.filePath(owner + QStringLiteral("/build/out.tex")), "ignored\n");
    }
    const LibrarySearchIndex index(dir.filePath(QStringLiteral("search.sqlite")));
    QElapsedTimer timer;
    timer.start();
    QList<LibrarySearchSource> sources = LibrarySearchIndex::listSources(dir.path(), projects);
    const qint64 firstScanMs = timer.restart();
    QCOMPARE(sources.size(), 8 * 51); // 50 files and the .ThoughtIgnore of each project
    LibrarySearchSourceUpdate first;
    QString error;
    QVERIFY2(index.updateSources(sources, &first, &error), qPrintable(error));
    const qint64 firstMs = timer.restart();
    sources = LibrarySearchIndex::listSources(dir.path(), projects);
    const qint64 scanMs = timer.restart();
    LibrarySearchSourceUpdate again;
    QVERIFY(index.updateSources(sources, &again, nullptr));
    const qint64 againMs = timer.restart();
    QCOMPARE(again.read, 0);
    const QString edited = dir.filePath(QStringLiteral("docs-project3/sec/file7.tex"));
    write(edited, "A freshly edited paragraph about lighthouses.\n");
    touch(edited, 5);
    LibrarySearchSourceUpdate edit;
    QVERIFY(index.updateSources(LibrarySearchIndex::listSources(dir.path(), projects), &edit, nullptr));
    const qint64 editMs = timer.restart();
    QCOMPARE(edit.read, 1);
    const QList<LibrarySearchHit> hits = index.search(QStringLiteral("teacher latency"), LibrarySearchScope::Sources);
    const qint64 queryMs = timer.restart();
    QVERIFY(!hits.isEmpty());
    QCOMPARE(index.search(QStringLiteral("unique3x9x50"), LibrarySearchScope::Sources).first().line, 51);
    QCOMPARE(index.search(QStringLiteral("lighthouses"), LibrarySearchScope::All).first().line, 1);
    qInfo().noquote() << QStringLiteral("generated Library: %1 files, %2 KB, %3 blocks; first build scan %4 ms + read %5 ms + write %6 ms (total %7 ms); "
                                        "no-change rescan %8 ms + update %9 ms; one edit %10 ms; query %11 ms")
                             .arg(first.files)
                             .arg(bytes / 1024)
                             .arg(first.chunks)
                             .arg(firstScanMs)
                             .arg(first.readMs)
                             .arg(first.writeMs)
                             .arg(firstScanMs + firstMs)
                             .arg(scanMs)
                             .arg(againMs)
                             .arg(editMs)
                             .arg(queryMs);
    // Generous bounds; the numbers above are what matters.
    QVERIFY(firstMs < 30000);
    QVERIFY(againMs < 5000);
}

QTEST_MAIN(LibrarySearchTest)
#include "librarysearchtest.moc"
