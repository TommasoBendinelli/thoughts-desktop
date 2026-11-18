// SPDX-License-Identifier: GPL-2.0-or-later
// Review cards for a whole section: the section's extent in a TeX or Markdown source, in a PDF
// by its outline or its headings, and the card plan for a PDF built from source.
#include "../shell/reviewdocumenttools.h"
#include <KZip>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <poppler-qt6.h>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>

using namespace Thoughts;

namespace
{
QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
bool put(const QString &path, const QByteArray &bytes)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}
QString newId()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// A PDF written by hand: Helvetica lines at given heights (points from the page's top) and an
// optional outline whose entries point at a page and a height.
struct RawLine {
    int page = 1;
    double x = 72, top = 0, size = 10;
    QString text;
};
struct RawEntry {
    QString title;
    int depth = 1, page = 1;
    double top = 0;
};
QByteArray rawPdf(int pages, const QList<RawLine> &lines, const QList<RawEntry> &outline = {})
{
    const double height = 792;
    QList<QByteArray> objects; // objects[i] is object i + 1
    const auto escape = [](QString text) {
        text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
        text.replace(QLatin1Char('('), QStringLiteral("\\("));
        text.replace(QLatin1Char(')'), QStringLiteral("\\)"));
        return text.toLatin1();
    };
    // 1 catalog, 2 pages, 3 font, 4 outline root, then per page: page, content; then entries.
    const int firstPage = 5;
    const int firstEntry = firstPage + 2 * pages;
    QByteArray kids;
    for (int p = 0; p < pages; ++p) kids += QByteArray::number(firstPage + 2 * p) + " 0 R ";
    objects.append(QByteArray("<< /Type /Catalog /Pages 2 0 R") + (outline.isEmpty() ? "" : " /Outlines 4 0 R /PageMode /UseOutlines") + " >>");
    objects.append("<< /Type /Pages /Kids [" + kids + "] /Count " + QByteArray::number(pages) + " >>");
    objects.append("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    // Outline tree: an entry's parent is the nearest entry before it one level up.
    QList<int> parent(outline.size(), -1);
    for (int i = 0; i < outline.size(); ++i)
        for (int j = i - 1; j >= 0; --j)
            if (outline[j].depth == outline[i].depth - 1) { parent[i] = j; break; }
    const auto children = [&](int of) {
        QList<int> found;
        for (int i = 0; i < outline.size(); ++i)
            if (parent[i] == of && (of >= 0 || outline[i].depth == 1)) found.append(i);
        return found;
    };
    const auto ref = [&](int entry) { return QByteArray::number(firstEntry + entry) + " 0 R"; };
    const QList<int> top = children(-1);
    objects.append(top.isEmpty() ? QByteArray("<< /Type /Outlines /Count 0 >>")
                                 : "<< /Type /Outlines /First " + ref(top.first()) + " /Last " + ref(top.last()) + " /Count " + QByteArray::number(outline.size()) + " >>");
    for (int p = 1; p <= pages; ++p) {
        QByteArray content;
        for (const auto &line : lines) {
            if (line.page != p) continue;
            content += "BT /F1 " + QByteArray::number(line.size) + " Tf " + QByteArray::number(line.x) + " " + QByteArray::number(height - line.top - line.size)
                + " Td (" + escape(line.text) + ") Tj ET\n";
        }
        objects.append("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 3 0 R >> >> /Contents "
                       + QByteArray::number(firstPage + 2 * (p - 1) + 1) + " 0 R >>");
        objects.append("<< /Length " + QByteArray::number(content.size()) + " >>\nstream\n" + content + "endstream");
    }
    for (int i = 0; i < outline.size(); ++i) {
        const QList<int> siblings = children(parent[i]);
        const int at = int(siblings.indexOf(i));
        const QList<int> mine = children(i);
        QByteArray entry = "<< /Title (" + escape(outline[i].title) + ") /Parent " + (parent[i] < 0 ? QByteArray("4 0 R") : ref(parent[i]));
        if (at > 0) entry += " /Prev " + ref(siblings[at - 1]);
        if (at + 1 < siblings.size()) entry += " /Next " + ref(siblings[at + 1]);
        if (!mine.isEmpty()) entry += " /First " + ref(mine.first()) + " /Last " + ref(mine.last()) + " /Count " + QByteArray::number(mine.size());
        entry += " /Dest [" + QByteArray::number(firstPage + 2 * (outline[i].page - 1)) + " 0 R /XYZ 0 " + QByteArray::number(height - outline[i].top) + " 0] >>";
        objects.append(entry);
    }
    QByteArray pdf("%PDF-1.4\n");
    QList<int> offsets;
    for (int i = 0; i < objects.size(); ++i) {
        offsets.append(int(pdf.size()));
        pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const int xref = int(pdf.size());
    pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (const int offset : std::as_const(offsets)) pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
    pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
    return pdf;
}

// A line of filler text, long enough to fill a column.
QString filler(int n, int words = 12)
{
    QStringList list;
    for (int i = 0; i < words; ++i) list.append(QStringLiteral("word%1x%2").arg(n).arg(i));
    return list.join(QLatin1Char(' '));
}

// A Library note built from TeX: the PDF (one text line per source line, at given baselines)
// with its verified source information, as the review store tests build it.
struct LinkedNote {
    struct Line { QString text; int sourceLine = 0; double baseline = 0; };
    QString root, pdfPath, bundle, sourceId, sourcePath, pdfSha;
    QByteArray source;
    bool build(const QString &directory, const QByteArray &sourceText, const QList<QList<Line>> &pages, const QByteArray &builtSource = {})
    {
        root = directory;
        pdfPath = QDir(root).filePath(QStringLiteral("current_pdfs/note.pdf"));
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
            }
        }
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
                for (const auto &line : pages[page])
                    if (line.sourceLine > 0) entries += QStringLiteral("h\t%1\t%2\t0\t%3\t600\t14\t4\n").arg(sourceId).arg(line.sourceLine).arg(line.baseline);
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
        const QJsonObject segment{{QStringLiteral("start_page"), 1}, {QStringLiteral("page_count"), int(pages.size())},
            {QStringLiteral("source_available"), true}, {QStringLiteral("lookup_format"), QStringLiteral("thoughts-reverse-synctex-v1")},
            {QStringLiteral("lookup_object"), lookupSha}, {QStringLiteral("lookup_sha256"), lookupSha},
            {QStringLiteral("inputs"), QJsonArray{QJsonObject{{QStringLiteral("synctex_path"), sourceId}, {QStringLiteral("source_file"), sourceId}, {QStringLiteral("source_sha256"), sourceSha}}}}};
        const QJsonObject manifest{{QStringLiteral("schema_version"), 1}, {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
            {QStringLiteral("filename"), QStringLiteral("note.pdf")},
            {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("filename"), QStringLiteral("note.pdf")}, {QStringLiteral("pdf_sha256"), pdfSha},
                {QStringLiteral("page_count"), int(pages.size())}, {QStringLiteral("project"), QStringLiteral("docs-test")}, {QStringLiteral("segments"), QJsonArray{segment}}}},
            {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("path"), sourceId}, {QStringLiteral("sha256"), sourceSha}, {QStringLiteral("object"), sourceSha}}}}};
        const auto artifact = QJsonDocument(manifest).toJson();
        const auto artifactSha = digest(artifact);
        if (!put(bundle + QStringLiteral("/objects/") + artifactSha, artifact)) return false;
        return put(bundle + QStringLiteral("/documents/note.pdf.json"),
                   QJsonDocument(QJsonObject{{QStringLiteral("artifact_sha256"), artifactSha}, {QStringLiteral("schema_version"), 1},
                                             {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact-pointer")},
                                             {QStringLiteral("filename"), QStringLiteral("note.pdf")}, {QStringLiteral("pdf_sha256"), pdfSha}}).toJson());
    }
    // A band around the line drawn at @p baseline, normalized to an A4 page.
    static QRectF band(double baseline)
    {
        return QRectF(QPointF(0.0, (baseline - 14.0) / 842.0), QPointF(1.0, (baseline + 5.0) / 842.0));
    }
};
// A PDF without source: its manifest names no source and no Project.
void writeWithoutSource(const QString &root, const QString &bundle, const QString &pdfPath, const QByteArray &pdf)
{
    QVERIFY(put(pdfPath, pdf));
    const QJsonObject manifest{{QStringLiteral("schema_version"), 1}, {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
        {QStringLiteral("filename"), QFileInfo(pdfPath).fileName()},
        {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("filename"), QFileInfo(pdfPath).fileName()}, {QStringLiteral("pdf_sha256"), digest(pdf)},
            {QStringLiteral("page_count"), 2}, {QStringLiteral("segments"), QJsonArray{}}}},
        {QStringLiteral("sources"), QJsonArray{}}};
    const auto artifact = QJsonDocument(manifest).toJson();
    QVERIFY(put(bundle + QStringLiteral("/objects/") + digest(artifact), artifact));
    QVERIFY(put(bundle + QStringLiteral("/documents/") + QFileInfo(pdfPath).fileName() + QStringLiteral(".json"),
                QJsonDocument(QJsonObject{{QStringLiteral("artifact_sha256"), digest(artifact)}, {QStringLiteral("schema_version"), 1},
                                          {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact-pointer")},
                                          {QStringLiteral("filename"), QFileInfo(pdfPath).fileName()}, {QStringLiteral("pdf_sha256"), digest(pdf)}}).toJson()));
    Q_UNUSED(root)
}
// The area of a hand-written line on a 612 × 792 page.
QRectF lineArea(double top, double size = 10)
{
    return QRectF(QPointF(60.0 / 612, (top - 1) / 792), QPointF(560.0 / 612, (top + size + 3) / 792));
}
}

class ReviewSectionTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void texSectionExtent();
    void texSectionEdges();
    void markdownSectionExtent();
    void sectionWrapsOtherCardsWhole();
    void markdownSectionWrapsOtherCards();
    void outlineSectionAcrossPages();
    void outlineTitlesWithoutNumbers();
    void numberedHeadingsWithoutOutline();
    void largerHeadingsWithoutOutline();
    void twoColumnSection();
    void bodyTextIsNoHeading();
    void sectionCardFromSource();
    void sectionCardWithoutSource();
    void realNoteSection();
};

void ReviewSectionTest::texSectionExtent()
{
    const QByteArray source(
        "\\documentclass{article}\n"                                     // 1
        "\\section{Ignored in the preamble}\n"                           // 2
        "\\begin{document}\n"                                            // 3
        "\\section*{Distributed Training}\n"                             // 4
        "Intro.\n"                                                       // 5
        "\n"                                                             // 6
        "\\subsection*{Global batch, mini-batch and micro-batch}\n"      // 7
        "\\hypertarget{notes-gb}{}\\label{sec:gb}\n"                     // 8
        "\n"                                                             // 9
        "\\paragraph{Pretraining} The global batch is split.\n"          // 10
        "\\begin{figure}[h]\n"                                           // 11
        "\\caption{Copies and micro-batches.}\n"                         // 12
        "\\end{figure}\n"                                                // 13
        "\\subsubsection*{Gradient accumulation}\n"                      // 14
        "Adds the gradients.\n"                                          // 15
        "% a comment before the next subsection\n"                       // 16
        "\n"                                                             // 17
        "\\subsection*{Collective operations}\\hypertarget{notes-c}{}\n" // 18
        "All-reduce.\n"                                                  // 19
        "\\section{Next part}\n"                                         // 20
        "Text.\n"                                                        // 21
        "\\bibliography{refs}\n"                                         // 22
        "\\end{document}\n");                                            // 23
    const QString path = QStringLiteral("docs-test/tex/source/main.tex");
    auto section = reviewSourceSection(path, source, {7});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QVERIFY(section.isHeading);
    QCOMPARE(section.title, QStringLiteral("Global batch, mini-batch and micro-batch"));
    QCOMPARE(section.kind, QStringLiteral("subsection"));
    QCOMPARE(section.headingLine, 7);
    QCOMPARE(section.firstLine, 10); // after its anchor, label and blank line
    QCOMPARE(section.lastLine, 15);  // the subsubsection is inside; the comment and blank line are not
    QCOMPARE(section.nextHeadingLine, 18);

    // A section ends at the next \section, not at its subsections.
    section = reviewSourceSection(path, source, {4});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.firstLine, 5);
    QCOMPARE(section.lastLine, 19);
    QCOMPARE(section.nextHeadingLine, 20);
    // The last section ends before the bibliography.
    section = reviewSourceSection(path, source, {20});
    QCOMPARE(section.firstLine, 21);
    QCOMPARE(section.lastLine, 21);
    // A subsubsection ends at the next subsection.
    section = reviewSourceSection(path, source, {14});
    QCOMPARE(section.kind, QStringLiteral("subsubsection"));
    QCOMPARE(section.firstLine, 15);
    QCOMPARE(section.lastLine, 15);

    // A paragraph heading runs into its text; a body line or the preamble is no heading.
    section = reviewSourceSection(path, source, {10});
    QVERIFY(!section.isHeading);
    QVERIFY(section.error.contains(QStringLiteral("paragraph heading")));
    QVERIFY(!reviewSourceSection(path, source, {15}).isHeading);
    QVERIFY(!reviewSourceSection(path, source, {2}).isHeading);
}

void ReviewSectionTest::texSectionEdges()
{
    const QString path = QStringLiteral("docs-test/tex/source/main.tex");
    const QString inner = newId(), outer = newId();
    // A title over two lines; a card that starts before the heading and one that reaches past the
    // next heading stay out with their markers.
    const QByteArray source = QStringLiteral(
        "\\begin{document}\n"                          // 1
        "\\section{A title that runs\n"                // 2
        "over two lines}\n"                            // 3
        "\\ReviewCardMaskEnd{%1}{0}%\n"                // 4  (its start is above the heading)
        "Body one.\n"                                  // 5
        "\\ReviewCardMaskStart{%2}{0}%\n"              // 6  (its end is past the next heading)
        "\\section{Second}\n"                          // 7
        "Body two.\n"                                  // 8
        "\\ReviewCardMaskEnd{%2}{0}%\n"                // 9
        "\\section{Third} Text on the heading's line.\n" // 10
        "\\section{Empty}\n"                           // 11
        "\n"                                           // 12
        "\\end{document}\n").arg(outer, inner).toUtf8();
    auto section = reviewSourceSection(path, source, {3});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.title, QStringLiteral("A title that runs over two lines"));
    QCOMPARE(section.firstLine, 5);
    QCOMPARE(section.lastLine, 5);
    section = reviewSourceSection(path, source, {10});
    QVERIFY(section.isHeading);
    QVERIFY(section.error.contains(QStringLiteral("starts on its heading's line")));
    section = reviewSourceSection(path, source, {11});
    QVERIFY(section.isHeading);
    QVERIFY(section.error.contains(QStringLiteral("no text below its heading")));
}

void ReviewSectionTest::markdownSectionExtent()
{
    const QByteArray source(
        "# Notes\n"                  // 1
        "\n"                         // 2
        "Intro.\n"                   // 3
        "\n"                         // 4
        "## Global batch {#gb}\n"    // 5
        "\n"                         // 6
        "The global batch.\n"        // 7
        "\n"                         // 8
        "```python\n"                // 9
        "# not a heading\n"          // 10
        "```\n"                      // 11
        "\n"                         // 12
        "### Accumulation\n"         // 13
        "Adds gradients.\n"          // 14
        "\n"                         // 15
        "## Collective operations ##\n" // 16
        "All-reduce.\n"              // 17
        "\n");                       // 18
    const QString path = QStringLiteral("docs-test/notes.md");
    auto section = reviewSourceSection(path, source, {5});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.title, QStringLiteral("Global batch"));
    QCOMPARE(section.level, 2);
    QCOMPARE(section.kind, QStringLiteral("subsection"));
    QCOMPARE(section.firstLine, 7);
    QCOMPARE(section.lastLine, 14); // the code block's "#" line is not a heading
    QCOMPARE(section.nextHeadingLine, 16);
    section = reviewSourceSection(path, source, {16});
    QCOMPARE(section.title, QStringLiteral("Collective operations"));
    QCOMPARE(section.firstLine, 17);
    QCOMPARE(section.lastLine, 17);
    QCOMPARE(section.nextHeadingLine, 0);
    section = reviewSourceSection(path, source, {1});
    QCOMPARE(section.firstLine, 3);
    QCOMPARE(section.lastLine, 17);
    QVERIFY(!reviewSourceSection(path, source, {10}).isHeading);

    // The body right below its heading, without a blank line, is marked after the heading.
    const QByteArray tight("# A\n## B\nText of B.\n\nMore of B.\n## C\nText.\n");
    section = reviewSourceSection(path, tight, {2});
    QCOMPARE(section.firstLine, 3);
    QCOMPARE(section.lastLine, 5);
    const auto plan = markReviewSourceRanges(path, tight, {{3, 5}}, newId(), nullptr, false, true);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
}

void ReviewSectionTest::sectionWrapsOtherCardsWhole()
{
    const QString path = QStringLiteral("docs-test/tex/source/main.tex");
    const QString inner = newId(), section = newId();
    const QByteArray source = QStringLiteral(
        "\\begin{document}\n"                    // 1
        "\\subsection*{Data parallelism}\n"      // 2
        "\\ReviewCardMaskStart{%1}{0}%\n"        // 3
        "Copies of the model.\n"                 // 4
        "\\ReviewCardMaskEnd{%1}{0}%\n"          // 5
        "Each copy gets a mini-batch.\n"         // 6
        "\\subsection*{Tensor parallelism}\n"    // 7
        "Split the layers.\n"                    // 8
        "\\end{document}\n").arg(inner).toUtf8();
    const auto extent = reviewSourceSection(path, source, {2});
    QCOMPARE(extent.firstLine, 3);
    QCOMPARE(extent.lastLine, 6);
    // An ordinary new card cannot hold another card; a section card holds it whole.
    QVERIFY(markReviewSourceRanges(path, source, {{3, 6}}, section).error.contains(QStringLiteral("cannot be nested")));
    const auto plan = markReviewSourceRanges(path, source, {{3, 6}}, section, nullptr, false, true);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QVERIFY(plan.replacement.contains(QStringLiteral("\\ReviewCardMaskStart{%1}{0}%\n\\ReviewCardMaskStart{%2}{0}%\n").arg(section, inner).toUtf8()));
    QVERIFY(plan.replacement.contains(QStringLiteral("Each copy gets a mini-batch.\n\\ReviewCardMaskEnd{%1}{0}%\n").arg(section).toUtf8()));
    // Both cards read back from the marked source.
    const auto outerRead = readReviewSourceAnchor(path, plan.replacement, section);
    QVERIFY2(outerRead.error.isEmpty(), qPrintable(outerRead.error));
    QVERIFY2(readReviewSourceAnchor(path, plan.replacement, inner).error.isEmpty(), "the inner card");
    QVERIFY(reviewCardMarkerProblem(path, plan.replacement, section).isEmpty());
    // Deleting either leaves the other.
    QString error;
    const auto withoutSection = removeReviewCardMarkers(path, plan.replacement, section, {QStringLiteral("0")}, &error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(withoutSection, source);
    // Part of a card is never held: a passage that would cut the inner card is refused.
    const auto cut = markReviewSourceRanges(path, source, {{4, 6}}, section, nullptr, false, true);
    QVERIFY(cut.error.contains(QStringLiteral("only whole")));
}

void ReviewSectionTest::markdownSectionWrapsOtherCards()
{
    const QString path = QStringLiteral("docs-test/notes.md");
    const QString inner = newId(), section = newId();
    const QByteArray source = QStringLiteral(
        "## Data parallelism\n"                                        // 1
        "\n"                                                           // 2
        "<!-- thoughts:v1:start review:%1:answer:0 -->\n"              // 3
        "Copies of the model.\n"                                       // 4
        "<!-- thoughts:v1:end review:%1:answer:0 -->\n"                // 5
        "\n"                                                           // 6
        "Each copy gets a mini-batch.\n"                               // 7
        "\n"                                                           // 8
        "## Tensor parallelism\n"                                      // 9
        "Split the layers.\n").arg(inner).toUtf8();
    const auto extent = reviewSourceSection(path, source, {1});
    QCOMPARE(extent.firstLine, 3);
    QCOMPARE(extent.lastLine, 7);
    QVERIFY(!markReviewSourceRanges(path, source, {{3, 7}}, section).error.isEmpty());
    const auto plan = markReviewSourceRanges(path, source, {{3, 7}}, section, nullptr, false, true);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QVERIFY(readReviewSourceAnchor(path, plan.replacement, section).error.isEmpty());
    QVERIFY(readReviewSourceAnchor(path, plan.replacement, inner).error.isEmpty());
    QVERIFY(markReviewSourceRanges(path, source, {{4, 7}}, section, nullptr, false, true).error.contains(QStringLiteral("already covers")));
}

void ReviewSectionTest::outlineSectionAcrossPages()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("paper.pdf"));
    QList<RawLine> lines{
        {1, 72, 60, 16, QStringLiteral("1 Introduction")},
        {1, 72, 90, 10, filler(1)},
        {1, 72, 104, 10, filler(2)},
        {1, 72, 140, 16, QStringLiteral("2 Method")},
        {1, 72, 170, 10, filler(3)},
        {1, 72, 184, 10, filler(4)},
        {1, 72, 700, 10, filler(5)},
        {2, 72, 60, 13, QStringLiteral("2.1 Data")},
        {2, 72, 86, 10, filler(6)},
        {2, 72, 100, 10, filler(7)},
        {2, 72, 140, 13, QStringLiteral("2.2 Model")},
        {2, 72, 166, 10, filler(8)},
        {2, 72, 220, 16, QStringLiteral("3 Results")},
        {2, 72, 250, 10, filler(9)},
    };
    const QList<RawEntry> outline{{QStringLiteral("1 Introduction"), 1, 1, 55}, {QStringLiteral("2 Method"), 1, 1, 135},
        {QStringLiteral("2.1 Data"), 2, 2, 55}, {QStringLiteral("2.2 Model"), 2, 2, 135}, {QStringLiteral("3 Results"), 1, 2, 215}};
    QVERIFY(put(path, rawPdf(2, lines, outline)));

    // "2 Method" runs over the page break to "3 Results", its subsections included.
    auto section = reviewPdfSection(path, {1, lineArea(140, 16), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QVERIFY(section.fromOutline);
    QCOMPARE(section.title, QStringLiteral("Method"));
    QCOMPARE(section.number, QStringLiteral("2"));
    QCOMPARE(section.level, 1);
    QCOMPARE(section.areas.size(), 2);
    QCOMPARE(section.areas[0].page, 1);
    QVERIFY(section.areas[0].rect.top() * 792 >= 156 - 1 && section.areas[0].rect.top() * 792 <= 170); // below the heading
    QVERIFY(section.areas[0].rect.bottom() * 792 >= 710);                                              // the page's last line
    QCOMPARE(section.areas[1].page, 2);
    QVERIFY(section.areas[1].rect.top() * 792 <= 66);
    QVERIFY(section.areas[1].rect.bottom() * 792 <= 226 && section.areas[1].rect.bottom() * 792 >= 176); // above "3 Results"
    // Only part of the heading selected: the same section.
    section = reviewPdfSection(path, {1, QRectF(QPointF(80.0 / 612, 139.0 / 792), QPointF(150.0 / 612, 159.0 / 792)), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.title, QStringLiteral("Method"));

    // The heading with the text below it is no heading selection.
    section = reviewPdfSection(path, {1, lineArea(140, 16).united(lineArea(170)), true});
    QVERIFY(!section.isHeading);

    // "2.1 Data" ends at "2.2 Model", on its page.
    section = reviewPdfSection(path, {2, lineArea(60, 13), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.number, QStringLiteral("2.1"));
    QCOMPARE(section.level, 2);
    QCOMPARE(section.areas.size(), 1);
    QVERIFY(section.areas[0].rect.top() * 792 >= 73 - 1);
    QVERIFY(section.areas[0].rect.bottom() * 792 <= 146 && section.areas[0].rect.bottom() * 792 >= 110);
    // The last section runs to the end of the document.
    section = reviewPdfSection(path, {2, lineArea(220, 16), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.areas.size(), 1);
    QVERIFY(section.areas[0].rect.bottom() * 792 >= 260 && section.areas[0].rect.bottom() * 792 <= 270);
}

void ReviewSectionTest::outlineTitlesWithoutNumbers()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("paper.pdf"));
    // Unnumbered outline titles; lines of text that mention them are neither the heading nor its end.
    QList<RawLine> lines{
        {1, 72, 60, 16, QStringLiteral("Method")},
        {1, 72, 90, 10, filler(1)},
        {1, 72, 104, 10, QStringLiteral("The method")},
        {1, 72, 118, 10, QStringLiteral("This part ends before the results.")},
        {1, 72, 132, 10, filler(2)},
        {1, 72, 170, 16, QStringLiteral("Results")},
        {1, 72, 200, 10, filler(3)},
    };
    QVERIFY(put(path, rawPdf(1, lines, {{QStringLiteral("Method"), 1, 1, 55}, {QStringLiteral("Results"), 1, 1, 165}})));
    auto section = reviewPdfSection(path, {1, lineArea(60, 16), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QVERIFY(section.fromOutline);
    QCOMPARE(section.areas.size(), 1);
    QVERIFY(section.areas[0].rect.bottom() * 792 >= 142 && section.areas[0].rect.bottom() * 792 <= 176); // at "Results", not at "the results."
    // A short line of text that names the section is no heading of it.
    section = reviewPdfSection(path, {1, lineArea(104), true});
    QVERIFY(!section.isHeading);
}

void ReviewSectionTest::numberedHeadingsWithoutOutline()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("plain.pdf"));
    // Headings at the text's size, found by their numbers; a numbered line of text is no heading.
    QList<RawLine> lines{
        {1, 72, 60, 10, QStringLiteral("2.1 Data")},
        {1, 72, 80, 10, filler(1)},
        {1, 72, 94, 10, QStringLiteral("3 of them ") + filler(2, 10)},
        {1, 72, 108, 10, filler(3)},
        {1, 72, 140, 10, QStringLiteral("2.2 Model")},
        {1, 72, 160, 10, filler(4)},
        {1, 72, 200, 10, QStringLiteral("References")},
        {1, 72, 220, 10, filler(5)},
    };
    QVERIFY(put(path, rawPdf(1, lines)));
    auto section = reviewPdfSection(path, {1, lineArea(60), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QVERIFY(!section.fromOutline);
    QCOMPARE(section.number, QStringLiteral("2.1"));
    QCOMPARE(section.title, QStringLiteral("Data"));
    QCOMPARE(section.areas.size(), 1);
    QVERIFY(section.areas[0].rect.bottom() * 792 <= 144 && section.areas[0].rect.bottom() * 792 >= 118);
    // The last numbered section ends at References.
    section = reviewPdfSection(path, {1, lineArea(140), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QVERIFY(section.areas[0].rect.bottom() * 792 <= 204 && section.areas[0].rect.bottom() * 792 >= 170);
}

void ReviewSectionTest::largerHeadingsWithoutOutline()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("plain.pdf"));
    QList<RawLine> lines{
        {1, 72, 60, 14, QStringLiteral("Global batch")},
        {1, 72, 84, 10, filler(1)},
        {1, 72, 98, 10, filler(2)},
        {1, 72, 112, 12, QStringLiteral("A smaller heading inside")},
        {1, 72, 130, 10, filler(3)},
        {1, 72, 170, 14, QStringLiteral("Collective operations")},
        {1, 72, 194, 10, filler(4)},
    };
    QVERIFY(put(path, rawPdf(1, lines)));
    const auto section = reviewPdfSection(path, {1, lineArea(60, 14), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.title, QStringLiteral("Global batch"));
    QCOMPARE(section.areas.size(), 1);
    QVERIFY(section.areas[0].rect.bottom() * 792 <= 176 && section.areas[0].rect.bottom() * 792 >= 140);
    // The heading with its first line of text is a passage, not a heading.
    QVERIFY(!reviewPdfSection(path, {1, lineArea(60, 14).united(lineArea(84)), true}).isHeading);
}

void ReviewSectionTest::twoColumnSection()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("columns.pdf"));
    QList<RawLine> lines;
    // Left column: text, then "2 Method" halfway down; right column: more of it, then "3 Results".
    for (int i = 0; i < 12; ++i) lines.append({1, 50, 60.0 + 14 * i, 10, filler(i, 5)});
    lines.append({1, 50, 240, 14, QStringLiteral("2 Method")});
    for (int i = 0; i < 12; ++i) lines.append({1, 50, 262.0 + 14 * i, 10, filler(20 + i, 5)});
    for (int i = 0; i < 10; ++i) lines.append({1, 330, 60.0 + 14 * i, 10, filler(40 + i, 5)});
    lines.append({1, 330, 210, 14, QStringLiteral("3 Results")});
    for (int i = 0; i < 12; ++i) lines.append({1, 330, 232.0 + 14 * i, 10, filler(60 + i, 5)});
    const QList<RawEntry> outline{{QStringLiteral("2 Method"), 1, 1, 236}, {QStringLiteral("3 Results"), 1, 1, 206}};
    QVERIFY(put(path, rawPdf(1, lines, outline)));
    const auto section = reviewPdfSection(path, {1, QRectF(QPointF(45.0 / 612, 239.0 / 792), QPointF(150.0 / 612, 257.0 / 792)), true});
    QVERIFY2(section.error.isEmpty(), qPrintable(section.error));
    QCOMPARE(section.areas.size(), 2);
    // The left column below the heading, then the right column above "3 Results"; each as wide as its column.
    QVERIFY(section.areas[0].rect.left() * 612 < 60 && section.areas[0].rect.right() * 612 < 306);
    QVERIFY(section.areas[0].rect.top() * 792 >= 250);
    QVERIFY(section.areas[1].rect.left() * 612 > 306);
    QVERIFY(section.areas[1].rect.top() * 792 <= 66 && section.areas[1].rect.bottom() * 792 <= 214);
}

void ReviewSectionTest::bodyTextIsNoHeading()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("plain.pdf"));
    QList<RawLine> lines{{1, 72, 60, 10, filler(1)}, {1, 72, 74, 10, QStringLiteral("Short line")}, {1, 72, 88, 10, filler(2)}};
    QVERIFY(put(path, rawPdf(1, lines)));
    auto section = reviewPdfSection(path, {1, lineArea(74), true});
    QVERIFY(!section.isHeading);
    section = reviewPdfSection(path, {1, lineArea(60), true});
    QVERIFY(!section.isHeading);
}

void ReviewSectionTest::sectionCardFromSource()
{
    QTemporaryDir dir;
    const QString inner = newId();
    const QByteArray source = QStringLiteral(
        "\\documentclass{article}\n"                              // 1
        "\\begin{document}\n"                                     // 2
        "\\subsection*{Global batch}\n"                           // 3
        "\\hypertarget{gb}{}\n"                                   // 4
        "The global batch is shared by copies.\n"                 // 5
        "\\ReviewCardMaskStart{%1}{0}%\n"                         // 6
        "Each copy cuts its mini-batch into micro-batches.\n"     // 7
        "\\ReviewCardMaskEnd{%1}{0}%\n"                           // 8
        "Gradients are accumulated before the step.\n"            // 9
        "\\subsection*{Collective operations}\n"                  // 10
        "All-reduce sums the gradients.\n"                        // 11
        "\\end{document}\n").arg(inner).toUtf8();
    LinkedNote note;
    QVERIFY(note.build(dir.path(), source,
        {{{QStringLiteral("Global batch"), 3, 100}, {QStringLiteral("The global batch is shared by copies."), 5, 130},
          {QStringLiteral("Each copy cuts its mini-batch into micro-batches."), 7, 150}},
         {{QStringLiteral("Gradients are accumulated before the step."), 9, 60}, {QStringLiteral("Collective operations"), 10, 100},
          {QStringLiteral("All-reduce sums the gradients."), 11, 130}}}));
    const QString id = newId();
    // Identifying the heading is quick and changes nothing.
    auto plan = planReviewSectionCard(dir.path(), note.bundle, note.pdfPath, note.pdfSha, {1, LinkedNote::band(100), true}, id, true);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QVERIFY(plan.isHeading);
    QVERIFY(plan.fromSource);
    QCOMPARE(plan.title, QStringLiteral("Global batch"));
    QCOMPARE(plan.kind, QStringLiteral("subsection"));
    QCOMPARE(plan.firstLine, 5);
    QCOMPARE(plan.lastLine, 9);
    QCOMPARE(plan.wrappedCards, 1);
    QVERIFY(plan.plan.replacement.isEmpty());

    plan = planReviewSectionCard(dir.path(), note.bundle, note.pdfPath, note.pdfSha, {1, LinkedNote::band(100), true}, id);
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QCOMPARE(plan.name(), QStringLiteral("subsection “Global batch”"));
    QCOMPARE(plan.firstPage, 1);
    QCOMPARE(plan.lastPage, 2);
    QCOMPARE(QFileInfo(plan.plan.path).canonicalFilePath(), QFileInfo(note.sourcePath).canonicalFilePath());
    QVERIFY(plan.plan.replacement.contains(QStringLiteral("\\hypertarget{gb}{}\n\\ReviewCardMaskStart{%1}{0}%\nThe global batch").arg(id).toUtf8()));
    QVERIFY(plan.plan.replacement.contains(QStringLiteral("before the step.\n\\ReviewCardMaskEnd{%1}{0}%\n\\subsection*{Collective").arg(id).toUtf8()));
    QCOMPARE(plan.plan.anchor.value(QStringLiteral("segments")).toArray().size(), 1);
    QCOMPARE(plan.plan.pageVisualSha256.size(), 64);
    // Hidden: page 1 below the heading, page 2 above the next heading; the headings stay visible.
    const auto regions = plan.plan.regions;
    QCOMPARE(regions.size(), 2);
    const auto first = regions[0].toObject(), second = regions[1].toObject();
    QCOMPARE(first.value(QStringLiteral("page")).toInt(), 1);
    QCOMPARE(second.value(QStringLiteral("page")).toInt(), 2);
    const auto rect = [](const QJsonObject &region, int i) { return region.value(QStringLiteral("rect")).toArray().at(i).toDouble() * 842; };
    QVERIFY(rect(first, 1) > 100 && rect(first, 1) < 120);  // below "Global batch"
    QVERIFY(rect(first, 3) >= 150);                         // its last line
    QVERIFY(rect(second, 1) < 50);                          // the page's first line
    QVERIFY(rect(second, 3) > 60 && rect(second, 3) < 100); // above "Collective operations"
    QVERIFY(plan.plan.answerAnchor.value(QStringLiteral("normalized_text")).toString().contains(QStringLiteral("Gradients are accumulated")));
    QVERIFY(!plan.plan.answerAnchor.value(QStringLiteral("normalized_text")).toString().contains(QStringLiteral("Collective")));

    // A body line is no heading, nor is the heading with the text below it: an ordinary card is made of them.
    plan = planReviewSectionCard(dir.path(), note.bundle, note.pdfPath, note.pdfSha, {1, LinkedNote::band(130), true}, id, true);
    QVERIFY(!plan.isHeading);
    plan = planReviewSectionCard(dir.path(), note.bundle, note.pdfPath, note.pdfSha, {1, LinkedNote::band(100).united(LinkedNote::band(130)), true}, id, true);
    QVERIFY(!plan.isHeading);
    // The source changed since the build: Sync first.
    QVERIFY(put(note.sourcePath, source + "% edited\n"));
    plan = planReviewSectionCard(dir.path(), note.bundle, note.pdfPath, note.pdfSha, {1, LinkedNote::band(100), true}, id);
    QVERIFY(plan.isHeading);
    QVERIFY(plan.error.contains(QStringLiteral("Sync")));
}

void ReviewSectionTest::sectionCardWithoutSource()
{
    QTemporaryDir dir;
    const QString bundle = dir.filePath(QStringLiteral("bundle"));
    const QString path = dir.filePath(QStringLiteral("current_pdfs/paper.pdf"));
    QList<RawLine> lines{
        {1, 72, 60, 16, QStringLiteral("1 Introduction")},
        {1, 72, 90, 10, filler(1)},
        {1, 72, 140, 16, QStringLiteral("2 Method")},
        {1, 72, 170, 10, filler(3)},
        {2, 72, 60, 10, filler(4)},
        {2, 72, 120, 16, QStringLiteral("3 Results")},
        {2, 72, 150, 10, filler(5)},
    };
    const QByteArray pdf = rawPdf(2, lines, {{QStringLiteral("1 Introduction"), 1, 1, 55}, {QStringLiteral("2 Method"), 1, 1, 135}, {QStringLiteral("3 Results"), 1, 2, 115}});
    writeWithoutSource(dir.path(), bundle, path, pdf);
    const auto plan = planReviewSectionCard(dir.path(), bundle, path, digest(pdf), {1, lineArea(140, 16), true}, newId());
    QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
    QVERIFY(plan.isHeading);
    QVERIFY(!plan.fromSource);
    QVERIFY(plan.plan.withoutSource);
    QCOMPARE(plan.name(), QStringLiteral("section 2 “Method”"));
    QCOMPARE(plan.plan.regions.size(), 2);
    QCOMPARE(plan.firstPage, 1);
    QCOMPARE(plan.lastPage, 2);
    QVERIFY(plan.plan.replacement.isEmpty());
}

// Opt-in, read-only: the section of a heading in a real Library PDF (nothing is written).
// THOUGHTS_SECTION_ROOT (the Library), THOUGHTS_SECTION_BUNDLE, THOUGHTS_SECTION_PDF, THOUGHTS_SECTION_HEADING.
void ReviewSectionTest::realNoteSection()
{
    const QString root = qEnvironmentVariable("THOUGHTS_SECTION_ROOT"), pdfPath = qEnvironmentVariable("THOUGHTS_SECTION_PDF");
    if (root.isEmpty() || pdfPath.isEmpty()) QSKIP("Set THOUGHTS_SECTION_ROOT, THOUGHTS_SECTION_BUNDLE, THOUGHTS_SECTION_PDF and THOUGHTS_SECTION_HEADING.");
    QFile file(pdfPath);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray bytes = file.readAll();
    auto pdf = Poppler::Document::loadFromData(bytes);
    QVERIFY(pdf);
    const QString heading = qEnvironmentVariable("THOUGHTS_SECTION_HEADING");
    for (int i = 0; i < pdf->numPages(); ++i) {
        auto page = pdf->page(i);
        const auto hits = page->search(heading, Poppler::Page::NoSearchFlags);
        if (hits.isEmpty()) continue;
        const QSizeF size = page->pageSizeF();
        const QRectF hit = hits.first();
        const QRectF area(QPointF(hit.left() / size.width(), hit.top() / size.height()), QPointF(hit.right() / size.width(), hit.bottom() / size.height()));
        QElapsedTimer timer;
        timer.start();
        const auto plan = planReviewSectionCard(root, qEnvironmentVariable("THOUGHTS_SECTION_BUNDLE"), pdfPath, digest(bytes), {i + 1, area, true}, newId());
        qInfo().noquote() << "heading" << (i + 1) << area << "in" << timer.elapsed() << "ms:" << plan.name() << "error:" << plan.error
                          << "lines" << plan.firstLine << plan.lastLine << "of" << plan.sourceId << "pages" << plan.firstPage << plan.lastPage
                          << "wrapped" << plan.wrappedCards;
        for (const auto &region : plan.plan.regions) qInfo().noquote() << "  region" << QJsonDocument(region.toObject()).toJson(QJsonDocument::Compact);
        qInfo().noquote() << "  answer starts:" << plan.plan.answerAnchor.value(QStringLiteral("normalized_text")).toString().left(120);
        qInfo().noquote() << "  answer ends:" << plan.plan.answerAnchor.value(QStringLiteral("normalized_text")).toString().right(120);
        QVERIFY2(plan.error.isEmpty(), qPrintable(plan.error));
        return;
    }
    QFAIL("The heading is not in the PDF.");
}

QTEST_MAIN(ReviewSectionTest)
#include "reviewsectiontest.moc"
