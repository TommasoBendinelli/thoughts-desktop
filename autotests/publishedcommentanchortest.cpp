/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <QTest>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>
#include <KZip>
#include "part/docscommentsynchronizer.h"

using Okular::DocsCommentSynchronizer;
class PublishedCommentAnchorTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void sourceIsAuthoritative()
    {
        QTemporaryDir temporary;
        QDir root(temporary.path());
        const QString relative = QStringLiteral("docs-one/tex/source/main.tex");
        const QString path = root.filePath(relative);
        QDir().mkpath(QFileInfo(path).absolutePath());
        const auto write = [&](const QByteArray &bytes) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(); };
        QVERIFY(write("Before \\CommentAnchor{id}{Source wins} after.\n"));
        DocsCommentSynchronizer sync({.docsRoot = root.path(), .commentSourceFiles = QSet<QString>{path}, .commentSourceFilesComplete = true});
        QJsonObject record{{QStringLiteral("comment_id"), QStringLiteral("id")}, {QStringLiteral("contents"), QStringLiteral("Stale cache")},
            {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")},
            {QStringLiteral("source_file"), relative}, {QStringLiteral("anchor_id"), QStringLiteral("id")}}}};
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        QCOMPARE(record.value(QStringLiteral("contents")).toString(), QStringLiteral("Source wins"));
        const auto previous = record;
        record.insert(QStringLiteral("contents"), QStringLiteral("Edited in UI"));
        QVERIFY(sync.sourceAuthoritativeComment(&record, previous, {}, true));
        { QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly)); QVERIFY(f.readAll().contains("{Edited in UI}")); }
        record.insert(QStringLiteral("deleted"), true);
        QVERIFY(sync.sourceAuthoritativeComment(&record, previous, {}, true));
        { QFile f(path); QVERIFY(f.open(QIODevice::ReadOnly)); QCOMPARE(f.readAll(), QByteArray("Before  after.\n")); }
        // A vanished marker keeps the comment with its last verified location.
        record = previous;
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        QVERIFY2(!record.value(QStringLiteral("deleted")).toBool(), QJsonDocument(record).toJson().constData());
        const auto removedScope = record.value(QStringLiteral("scope")).toObject();
        QCOMPARE(removedScope, (QJsonObject{{QStringLiteral("state"), QStringLiteral("needs_attention")}, {QStringLiteral("reason"), QStringLiteral("anchor_removed")},
            {QStringLiteral("anchor_id"), QStringLiteral("id")}, {QStringLiteral("source_file"), relative}, {QStringLiteral("source_line"), 1},
            {QStringLiteral("anchor_kind"), QStringLiteral("CommentAnchor")}, {QStringLiteral("anchor_mirror"), QStringLiteral("Source wins")}}));
        const auto removed = record;
        record = previous;
        QVERIFY(sync.sourceAuthoritativeComment(&record, removed, {}, true));
        QVERIFY(!record.value(QStringLiteral("deleted")).toBool());
        QCOMPARE(record.value(QStringLiteral("scope")).toObject(), removedScope);

        // Edits, deletes and Undo of an anchor-removed comment never touch source or relink.
        const QByteArray unrelated("Plain text that a PDF-geometry relink must not touch.\n");
        QVERIFY(write(unrelated));
        const auto source = [&] { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        auto edited = removed;
        edited.insert(QStringLiteral("contents"), QStringLiteral("Edited while removed"));
        QVERIFY(sync.sourceAuthoritativeComment(&edited, removed, {}, true));
        QCOMPARE(edited.value(QStringLiteral("scope")).toObject(), removedScope);
        QVERIFY(!edited.value(QStringLiteral("deleted")).toBool());
        QCOMPARE(source(), unrelated);
        auto explicitDelete = edited;
        explicitDelete.insert(QStringLiteral("deleted"), true);
        QVERIFY(sync.sourceAuthoritativeComment(&explicitDelete, edited, {}, true));
        QVERIFY(explicitDelete.value(QStringLiteral("deleted")).toBool());
        QCOMPARE(explicitDelete.value(QStringLiteral("scope")).toObject(), removedScope);
        QCOMPARE(source(), unrelated);
        auto undone = edited;
        QVERIFY(sync.sourceAuthoritativeComment(&undone, explicitDelete, {}, true));
        QVERIFY(!undone.value(QStringLiteral("deleted")).toBool());
        QCOMPARE(undone.value(QStringLiteral("scope")).toObject(), removedScope);
        QCOMPARE(source(), unrelated);

        // The marker reappearing by any route relinks the comment and drops the anchor_* keys.
        QVERIFY(write("Back \\CommentAnchor{id}{Reappeared} here.\n"));
        auto relinked = removed;
        QVERIFY(sync.sourceAuthoritativeComment(&relinked, {}, {}, false));
        const auto relinkedScope = relinked.value(QStringLiteral("scope")).toObject();
        QCOMPARE(relinkedScope.value(QStringLiteral("state")).toString(), QStringLiteral("source_linked"));
        QCOMPARE(relinkedScope.value(QStringLiteral("reason")).toString(), QStringLiteral("unique_source_marker"));
        QVERIFY(!relinkedScope.contains(QStringLiteral("anchor_mirror")) && !relinkedScope.contains(QStringLiteral("anchor_kind")));
        QCOMPARE(relinked.value(QStringLiteral("contents")).toString(), QStringLiteral("Reappeared"));

        QVERIFY(write("% \\CommentAnchor{id}{Commented out}\n"));
        record = previous;
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        QVERIFY(!record.value(QStringLiteral("deleted")).toBool());
        QCOMPARE(record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("source_linked"));
    }
    void boundedSourceProjection()
    {
        QTemporaryDir temporary;
        const QDir root(temporary.path());
        const auto write = [&](const QString &path, const QByteArray &bytes) {
            QDir().mkpath(QFileInfo(path).absolutePath());
            QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const QString moved = root.filePath(QStringLiteral("docs-one/moved.tex"));
        const QString outside = root.filePath(QStringLiteral("docs-other/other.tex"));
        QVERIFY(write(moved, "\\CommentAnchor{id}{Moved within project}"));
        QVERIFY(write(outside, "\\CommentAnchor{id}{Outside project}"));
        const QJsonObject original{{QStringLiteral("comment_id"), QStringLiteral("id")}, {QStringLiteral("contents"), QStringLiteral("Old")},
            {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")},
             {QStringLiteral("source_file"), QStringLiteral("docs-one/deleted.tex")}, {QStringLiteral("anchor_id"), QStringLiteral("id")}}}};
        const auto indexed = DocsCommentSynchronizer::sourceCommentFileIndex(moved, {QStringLiteral("id")});
        QCOMPARE(indexed.value(QStringLiteral("markers")).toObject().value(QStringLiteral("id")).toArray().size(), 1);
        QVERIFY(indexed.value(QStringLiteral("mentioned")).toArray().contains(QStringLiteral("id")));
        QVERIFY(DocsCommentSynchronizer::sourceCommentFileIndex(root.filePath(QStringLiteral("missing.tex")), {QStringLiteral("id")}).isEmpty());
        auto record = original;
        DocsCommentSynchronizer sync({.docsRoot = root.path(), .commentSourceFiles = QSet<QString>{moved}, .commentSourceFilesComplete = true});
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        QCOMPARE(record.value(QStringLiteral("contents")).toString(), QStringLiteral("Moved within project"));
        QVERIFY(write(moved, "No marker remains."));
        record = original;
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        QVERIFY(!record.value(QStringLiteral("deleted")).toBool());
        QCOMPARE(record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("reason")).toString(), QStringLiteral("anchor_removed"));
        QCOMPARE(record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("source_file")).toString(), QStringLiteral("docs-one/deleted.tex"));
        record = original;
        DocsCommentSynchronizer incomplete({.docsRoot = root.path(), .commentSourceFiles = QSet<QString>{moved}, .commentSourceFilesComplete = false});
        QVERIFY(incomplete.sourceAuthoritativeComment(&record, {}, {}, false));
        QCOMPARE(record, original);
        record = original;
        DocsCommentSynchronizer unreadable({.docsRoot = root.path(), .commentSourceFiles = QSet<QString>{moved, root.filePath(QStringLiteral("unavailable.tex"))}, .commentSourceFilesComplete = true});
        QVERIFY(unreadable.sourceAuthoritativeComment(&record, {}, {}, false));
        QCOMPARE(record, original);
        QVERIFY(write(moved, "\\CommentHighlight{id}{Updated comment}{Updated passage}"));
        record = original;
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        QCOMPARE(record.value(QStringLiteral("selected_text")).toString(), QStringLiteral("Updated passage"));
    }

    void markdownSourceProjection()
    {
        QTemporaryDir temporary;
        const QString path = temporary.filePath(QStringLiteral("note.md"));
        QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("<!-- thoughts:v1:meta {\"key\":\"comment:id:s1\",\"contents\":\"Markdown edit\"} -->\n<!-- thoughts:v1:point comment:id:s1 -->\n"); file.close();
        QJsonObject record{{QStringLiteral("comment_id"), QStringLiteral("id")},
            {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")}, {QStringLiteral("source_file"), QStringLiteral("note.md")}}}};
        DocsCommentSynchronizer sync({.docsRoot = temporary.path(), .commentSourceFiles = QSet<QString>{path}, .commentSourceFilesComplete = true});
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        QCOMPARE(record.value(QStringLiteral("contents")).toString(), QStringLiteral("Markdown edit"));

        // Editing and deleting a Markdown-linked comment write through to its metadata and tokens.
        const auto source = [&] { QFile f(path); return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray(); };
        const auto write = [&](const QByteArray &bytes) { QFile f(path); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size(); };
        QVERIFY(write("Intro paragraph.\n<!-- thoughts:v1:meta {\"key\":\"comment:id:s1\",\"contents\":\"Markdown edit\",\"revision\":0} -->\n"
                      "Passage <!-- thoughts:v1:start comment:id:s1 -->marked<!-- thoughts:v1:end comment:id:s1 --> text.\nOutro.\n"));
        QVERIFY(sync.sourceAuthoritativeComment(&record, {}, {}, false));
        auto previous = record;
        record.insert(QStringLiteral("contents"), QStringLiteral("Edited \"quoted\" --> text"));
        QVERIFY(sync.sourceAuthoritativeComment(&record, previous, {}, true));
        const auto indexed = DocsCommentSynchronizer::sourceCommentFileIndex(path, {QStringLiteral("id")});
        QCOMPARE(indexed.value(QStringLiteral("markers")).toObject().value(QStringLiteral("id")).toArray().first().toObject().value(QStringLiteral("contents")).toString(),
                 QStringLiteral("Edited \"quoted\" --> text"));
        QVERIFY(source().contains("{\"key\":\"comment:id:s1\",\"contents\":\"Edited \\\"quoted\\\" --\\u003e text\",\"revision\":0}"));
        QCOMPARE(record.value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString(), QStringLiteral("source_linked"));
        previous = record;
        record.insert(QStringLiteral("deleted"), true);
        QVERIFY(sync.sourceAuthoritativeComment(&record, previous, {}, true));
        QCOMPARE(source(), QByteArray("Intro paragraph.\nPassage marked text.\nOutro.\n"));
        QVERIFY(write("<!-- thoughts:v1:meta {\"key\":\"comment:id:s1\",\"contents\":\"Point\"} -->\n<!-- thoughts:v1:point comment:id:s1 -->\nKept.\n"));
        QVERIFY(DocsCommentSynchronizer::removeCommentAnchor(path, QStringLiteral("id")));
        QCOMPARE(source(), QByteArray("Kept.\n"));
        QVERIFY(!DocsCommentSynchronizer::removeCommentAnchor(path, QStringLiteral("id")));
    }

    void restoreRemovedAnchor()
    {
        using Placement = DocsCommentSynchronizer::AnchorRestoreResult::Placement;
        QTemporaryDir temporary;
        const QDir root(temporary.path());
        const QString relative = QStringLiteral("docs-one/tex/source/main.tex");
        const QString path = root.filePath(relative);
        const auto write = [](const QString &file, const QByteArray &bytes) {
            QDir().mkpath(QFileInfo(file).absolutePath()); QFile f(file); return f.open(QIODevice::WriteOnly) && f.write(bytes) == bytes.size();
        };
        const auto read = [](const QString &file) { QFile f(file); return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString(); };
        const auto lines = [&](const QString &file) { return read(file).split(QLatin1Char('\n')); };
        const QByteArray body("\\documentclass{article}\n\\usepackage{manual_style}\n\\begin{document}\nFirst paragraph line.\n"
                              "Second paragraph line.\n\nThird paragraph line.\n\\end{document}\n");
        QVERIFY(write(path, body));
        const auto payload = [&](int line, const QString &kind = QStringLiteral("CommentAnchor"), const QString &file = QString()) {
            return QJsonObject{{QStringLiteral("comment_id"), QStringLiteral("c1")}, {QStringLiteral("contents"), QStringLiteral("Restored text")},
                {QStringLiteral("subtype"), kind == QLatin1String("CommentHighlight") ? QStringLiteral("Highlight") : QStringLiteral("Text")},
                {QStringLiteral("selected_text"), QStringLiteral("Second paragraph")},
                {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QStringLiteral("main")}}},
                {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("needs_attention")}, {QStringLiteral("reason"), QStringLiteral("anchor_removed")},
                    {QStringLiteral("anchor_id"), QStringLiteral("c1")}, {QStringLiteral("source_file"), file.isEmpty() ? relative : file},
                    {QStringLiteral("source_line"), line}, {QStringLiteral("anchor_kind"), kind}, {QStringLiteral("anchor_mirror"), QStringLiteral("Mirror")}}}};
        };
        DocsCommentSynchronizer sync({.docsRoot = root.path(), .commentSourceFiles = QSet<QString>{path}, .commentSourceFilesComplete = true});

        // Exact: the marker lands back at its last known line.
        auto result = sync.restoreRemovedAnchor(payload(5));
        QVERIFY2(result.success, qPrintable(result.message));
        QCOMPARE(result.placement, Placement::Exact);
        QCOMPARE(result.line, 5);
        QCOMPARE(lines(path).at(4), QStringLiteral("\\CommentAnchor{c1}{Restored text}%"));
        QCOMPARE(result.message, QStringLiteral("Anchor restored at docs-one/tex/source/main.tex:5."));
        // Already present: nothing to do.
        result = sync.restoreRemovedAnchor(payload(5));
        QVERIFY(result.success);
        QCOMPARE(result.placement, Placement::AlreadyPresent);
        QCOMPARE(result.message, QStringLiteral("Anchor already present at docs-one/tex/source/main.tex:5."));
        // Malformed: the raw identity remains without a live marker.
        QVERIFY(write(path, QByteArray(body).replace("First paragraph line.", "% \\CommentAnchor{c1}{Commented out}")));
        result = sync.restoreRemovedAnchor(payload(5));
        QVERIFY(!result.success);
        QCOMPARE(result.message, QStringLiteral("The anchor text is still in docs-one/tex/source/main.tex:4 but malformed; fix it by hand."));
        // Nearest line: the line is gone, or cannot hold an anchor.
        QVERIFY(write(path, body));
        result = sync.restoreRemovedAnchor(payload(50));
        QVERIFY(result.success);
        QCOMPARE(result.placement, Placement::NearestLine);
        QCOMPARE(result.line, 7);
        QCOMPARE(result.message, QStringLiteral("Line 50 no longer exists in docs-one/tex/source/main.tex; anchor restored at line 7."));
        QVERIFY(write(path, body));
        result = sync.restoreRemovedAnchor(payload(6));
        QCOMPARE(result.placement, Placement::NearestLine);
        QCOMPARE(result.line, 7);
        QCOMPARE(lines(path).at(7), QStringLiteral("Third paragraph line."));
        // End of file: no line of the body can hold an anchor.
        const QByteArray headings("\\documentclass{article}\n\\usepackage{manual_style}\n\\begin{document}\n\\section{One}\n\n\\section{Two}\n\\end{document}\n");
        QVERIFY(write(path, headings));
        result = sync.restoreRemovedAnchor(payload(5));
        QCOMPARE(result.placement, Placement::EndOfFile);
        QCOMPARE(result.line, 7);
        QCOMPARE(lines(path).at(6), QStringLiteral("\\CommentAnchor{c1}{Restored text}%"));
        QCOMPARE(lines(path).at(7), QStringLiteral("\\end{document}"));
        // Highlight re-wrap, and the point-anchor fallback when the passage is gone.
        QVERIFY(write(path, body));
        result = sync.restoreRemovedAnchor(payload(5, QStringLiteral("CommentHighlight")));
        QVERIFY(result.success);
        QVERIFY(!result.pointAnchorFallback);
        QCOMPARE(result.placement, Placement::Exact);
        QCOMPARE(lines(path).at(4), QStringLiteral("\\CommentHighlight{c1}{Restored text}{Second paragraph} line."));
        QVERIFY(write(path, QByteArray(body).replace("Second paragraph line.", "Rewritten line.")));
        result = sync.restoreRemovedAnchor(payload(5, QStringLiteral("CommentHighlight")));
        QVERIFY(result.success);
        QVERIFY(result.pointAnchorFallback);
        QVERIFY(result.message.endsWith(QStringLiteral("The highlight was restored as a point anchor.")));
        QCOMPARE(lines(path).at(4), QStringLiteral("\\CommentAnchor{c1}{Restored text}%"));
        // Empty contents fall back to the recorded mirror.
        QVERIFY(write(path, body));
        auto mirrorOnly = payload(5);
        mirrorOnly.insert(QStringLiteral("contents"), QString());
        QVERIFY(sync.restoreRemovedAnchor(mirrorOnly).success);
        QCOMPARE(lines(path).at(4), QStringLiteral("\\CommentAnchor{c1}{Mirror}%"));

        // Fallback file: the last known file is gone; the project's main source takes the anchor.
        QVERIFY(write(path, body));
        result = sync.restoreRemovedAnchor(payload(5, QStringLiteral("CommentAnchor"), QStringLiteral("docs-one/tex/source/chapter.tex")));
        QVERIFY2(result.success, qPrintable(result.message));
        QCOMPARE(result.placement, Placement::FallbackFile);
        QCOMPARE(result.line, 8);
        QCOMPARE(result.message, QStringLiteral("docs-one/tex/source/chapter.tex no longer exists; anchor restored at the end of docs-one/tex/source/main.tex."));
        QVERIFY(write(path, body));
        result = sync.restoreRemovedAnchor(payload(5, QStringLiteral("CommentAnchor"), QStringLiteral("docs-two/gone.tex")));
        QVERIFY(!result.success);
        QCOMPARE(result.message, QStringLiteral("The source file no longer exists; link the comment manually."));
        QCOMPARE(read(path).toUtf8(), body);

        // Definition insertion: a document without the comment macros receives them.
        const QString plain = root.filePath(QStringLiteral("docs-three/main.tex"));
        QVERIFY(write(plain, "\\documentclass{article}\n\\begin{document}\nOnly line.\n\\end{document}\n"));
        DocsCommentSynchronizer plainSync({.docsRoot = root.path(), .commentSourceFiles = QSet<QString>{plain}, .commentSourceFilesComplete = true});
        result = plainSync.restoreRemovedAnchor(payload(3, QStringLiteral("CommentAnchor"), QStringLiteral("docs-three/main.tex")));
        QVERIFY2(result.success, qPrintable(result.message));
        QCOMPARE(result.placement, Placement::Exact);
        QVERIFY(read(plain).contains(QStringLiteral("\\DeclareRobustCommand{\\CommentAnchor}[2]")));
        QVERIFY(result.line > 3);
        QCOMPARE(lines(plain).at(result.line - 1), QStringLiteral("\\CommentAnchor{c1}{Restored text}%"));
        QCOMPARE(lines(plain).at(result.line), QStringLiteral("Only line."));

        // Markdown: the metadata and point token go back before the last known line, outside code.
        const QString markdown = root.filePath(QStringLiteral("notes/note.md"));
        QVERIFY(write(markdown, "# Title\n\nPara one.\n```\ncode line\n```\nPara two.\n"));
        DocsCommentSynchronizer markdownSync({.docsRoot = root.path(), .commentSourceFiles = QSet<QString>{markdown}, .commentSourceFilesComplete = true});
        result = markdownSync.restoreRemovedAnchor(payload(3, QStringLiteral("markdown"), QStringLiteral("notes/note.md")));
        QVERIFY2(result.success, qPrintable(result.message));
        QCOMPARE(result.placement, Placement::Exact);
        QCOMPARE(result.line, 3);
        QCOMPARE(lines(markdown).at(2), QStringLiteral("<!-- thoughts:v1:meta {\"key\":\"comment:c1:s1\",\"contents\":\"Restored text\"} -->"));
        QCOMPARE(lines(markdown).at(3), QStringLiteral("<!-- thoughts:v1:point comment:c1:s1 -->"));
        QVERIFY(DocsCommentSynchronizer::removeCommentAnchor(markdown, QStringLiteral("c1")));
        QCOMPARE(read(markdown), QStringLiteral("# Title\n\nPara one.\n```\ncode line\n```\nPara two.\n"));
        result = markdownSync.restoreRemovedAnchor(payload(5, QStringLiteral("markdown"), QStringLiteral("notes/note.md")));
        QVERIFY(result.success);
        QCOMPARE(result.placement, Placement::NearestLine);
        QCOMPARE(result.line, 7);
        QCOMPARE(lines(markdown).at(8), QStringLiteral("Para two."));

        // A sidecar placement record carries its last anchor in its own fields.
        QVERIFY(write(path, body));
        const QJsonObject sidecar{{QStringLiteral("id"), QStringLiteral("c1")}, {QStringLiteral("anchor_id"), QStringLiteral("c1")}, {QStringLiteral("tex_file"), relative},
            {QStringLiteral("tex_line"), 4}, {QStringLiteral("contents"), QStringLiteral("Sidecar")}, {QStringLiteral("subtype"), QStringLiteral("Text")},
            {QStringLiteral("anchor_status"), QStringLiteral("orphaned")}, {QStringLiteral("orphan_reason"), QStringLiteral("anchor_removed")}};
        result = sync.restoreRemovedAnchor(sidecar);
        QVERIFY2(result.success, qPrintable(result.message));
        QCOMPARE(lines(path).at(3), QStringLiteral("\\CommentAnchor{c1}{Sidecar}%"));
    }

    void exactMap_data()
    {
        QTest::addColumn<QString>("change");
        QTest::addColumn<bool>("linked");
        QTest::newRow("three-word-heading-combined-pdf") << QString() << true;
        QTest::newRow("source-passage-moved") << QStringLiteral("moved") << true;
        QTest::newRow("source-passage-deleted") << QStringLiteral("deleted") << false;
        QTest::newRow("source-passage-ambiguous") << QStringLiteral("ambiguous") << false;
        QTest::newRow("wrong-pdf-version") << QStringLiteral("wrong_pdf") << false;
        QTest::newRow("corrupt-map") << QStringLiteral("corrupt_map") << false;
    }
    void exactMap()
    {
        QFETCH(QString, change);
        QFETCH(bool, linked);
        QTemporaryDir temporary;
        QVERIFY(temporary.isValid());
        const QDir docs(temporary.path());
        const QDir bundle(docs.filePath(QStringLiteral("bundle")));
        const auto write = [](const QString &path, const QByteArray &bytes) {
            QDir().mkpath(QFileInfo(path).absolutePath()); QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto digest = [](const QByteArray &bytes) { return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex()); };
        const auto object = [&](const QByteArray &bytes) {
            const QString sha = digest(bytes);
            if (!write(bundle.filePath(QStringLiteral("objects/") + sha), bytes)) return QString();
            return sha;
        };
        const QByteArray pdf("%PDF-1.7\ncombined PDF differs from every component\n");
        const QString pdfSha = digest(pdf), pdfPath = docs.filePath(QStringLiteral("current_pdfs/example.pdf"));
        QVERIFY(write(pdfPath, pdf));
        const QString sourceId = QStringLiteral("docs-one/tex/source/main.tex");
        const QByteArray source("\\documentclass{article}\n\\begin{document}\n\\subsubsection*{Prefix recoverability behaviour}\nOriginal following sentence.\n\\end{document}\n");
        QByteArray current = source;
        if (change == QLatin1String("moved")) current.prepend("% A source edit moved the heading.\n\n");
        if (change == QLatin1String("deleted")) current.replace("Prefix recoverability behaviour", "An entirely different heading");
        if (change == QLatin1String("ambiguous")) current = QByteArrayLiteral("% shifted\n") + source + source;
        QVERIFY(write(docs.filePath(sourceId), current));
        const QString sourceSha = object(source);
        const QString zipPath = docs.filePath(QStringLiteral("lookup.zip"));
        {
            KZip zip(zipPath); QVERIFY(zip.open(QIODevice::WriteOnly));
            QVERIFY(zip.writeFile(QStringLiteral("pages/1.tsv"), QByteArrayLiteral("h\tdocs-one/tex/source/main.tex\t3\t90\t100\t40\t10\t2\n")));
            QVERIFY(zip.close());
        }
        QFile zip(zipPath); QVERIFY(zip.open(QIODevice::ReadOnly));
        const QString lookupSha = object(zip.readAll());
        QJsonObject segment {{QStringLiteral("start_page"),3}, {QStringLiteral("page_count"),2},
            {QStringLiteral("source_available"),true}, {QStringLiteral("fingerprint"),QString(64,QLatin1Char('a'))},
            {QStringLiteral("lookup_format"),QStringLiteral("thoughts-reverse-synctex-v1")},
            {QStringLiteral("lookup_object"),lookupSha}, {QStringLiteral("lookup_sha256"),lookupSha},
            {QStringLiteral("inputs"),QJsonArray {QJsonObject {{QStringLiteral("synctex_path"),sourceId}, {QStringLiteral("source_file"),sourceId}, {QStringLiteral("source_sha256"),sourceSha}}}}};
        QJsonObject manifest {{QStringLiteral("schema_version"),1}, {QStringLiteral("kind"),QStringLiteral("thoughts-document-source-artifact")},
            {QStringLiteral("filename"),QStringLiteral("example.pdf")},
            {QStringLiteral("pdf"),QJsonObject {{QStringLiteral("filename"),QStringLiteral("example.pdf")}, {QStringLiteral("pdf_sha256"),pdfSha},
                {QStringLiteral("page_count"),4}, {QStringLiteral("segments"),QJsonArray {segment}}}},
            {QStringLiteral("sources"),QJsonArray {QJsonObject {{QStringLiteral("path"),sourceId}, {QStringLiteral("sha256"),sourceSha}, {QStringLiteral("object"),sourceSha}}}}};
        const QString manifestSha = object(QJsonDocument(manifest).toJson(QJsonDocument::Compact));
        const QJsonObject pointer {{QStringLiteral("schema_version"),1}, {QStringLiteral("kind"),QStringLiteral("thoughts-document-source-artifact-pointer")},
            {QStringLiteral("filename"),QStringLiteral("example.pdf")}, {QStringLiteral("pdf_sha256"),pdfSha}, {QStringLiteral("artifact_sha256"),manifestSha}};
        QVERIFY(write(bundle.filePath(QStringLiteral("documents/example.pdf.json")),QJsonDocument(pointer).toJson()));
        if (change == QLatin1String("wrong_pdf")) QVERIFY(write(pdfPath,pdf+QByteArrayLiteral("newer PDF")));
        if (change == QLatin1String("corrupt_map")) QVERIFY(write(bundle.filePath(QStringLiteral("objects/")+lookupSha),QByteArrayLiteral("corrupt")));
        DocsCommentSynchronizer synchronizer({.docsRoot=docs.path(),.sourceBundleRoot=bundle.path(),.allowLegacySourceResolution=false});
        const auto resolved = synchronizer.resolveSource({.pdfStem=QStringLiteral("example"),.pageNumber=3,.x=100,.y=100,
            .selectedText=QStringLiteral("Prefix recoverability behaviour"),.pdfPath=pdfPath,.requireExactPassage=true});
        // A short-lived cached scan must preserve the exact same validation and
        // source remapping results, including corrupt and ambiguous artifacts.
        DocsCommentSynchronizer cached({.docsRoot=docs.path(),.sourceBundleRoot=bundle.path(),.allowLegacySourceResolution=false,.cacheSourceResolution=true});
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto mapped = cached.resolveSource({.pdfStem=QStringLiteral("example"),.pageNumber=3,.x=100,.y=100,
                .selectedText=QStringLiteral("Prefix recoverability behaviour"),.pdfPath=pdfPath,.requireExactPassage=true});
            QCOMPARE(mapped.success, resolved.success);
            QCOMPARE(mapped.sourceId, resolved.sourceId);
            QCOMPARE(mapped.line, resolved.line);
            QCOMPARE(mapped.failureReason, resolved.failureReason);
        }
        QCOMPARE(resolved.success,linked);
        if (!linked) QVERIFY(!resolved.failureReason.isEmpty());
        else QCOMPARE(resolved.sourceId,sourceId);
        const QJsonObject comment {{QStringLiteral("schema_version"),1},{QStringLiteral("id"),QStringLiteral("short-heading")},
            {QStringLiteral("revision"),1},{QStringLiteral("updated_at"),QStringLiteral("2026-09-15T12:00:00Z")},
            {QStringLiteral("origin_id"),QStringLiteral("test")},{QStringLiteral("subtype"),QStringLiteral("Highlight")},
            {QStringLiteral("contents"),QStringLiteral("Measure this quantity")},{QStringLiteral("discussion"),true},
            {QStringLiteral("selected_text"),QStringLiteral("Prefix recoverability behaviour")},
            {QStringLiteral("annotation_pdf_sha256"),pdfSha},{QStringLiteral("pdf_sha256"),pdfSha},
            {QStringLiteral("page"),3},{QStringLiteral("rect"),QJsonArray{90,490,130,510}},
            {QStringLiteral("quad_points"),QJsonArray{90,510,130,510,90,490,130,490}}};
        QJsonObject logical{{QStringLiteral("comment_id"), QStringLiteral("local-client")},
            {QStringLiteral("contents"), QStringLiteral("Created offline")}, {QStringLiteral("subtype"), QStringLiteral("Highlight")},
            {QStringLiteral("selected_text"), QStringLiteral("Prefix recoverability behaviour")},
            {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QStringLiteral("example")},
                {QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("raw_pdf_geometry"), QJsonObject{
                    {QStringLiteral("page"), 3}, {QStringLiteral("rect"), QJsonArray{90,490,130,510}}}}}}};
        QVERIFY(synchronizer.sourceAuthoritativeComment(&logical, {},
            [](const QString &,int,QString *) -> std::optional<DocsCommentSynchronizer::PageGeometry> { return DocsCommentSynchronizer::PageGeometry{600,600,0}; }, true));
        QCOMPARE(logical.value(QStringLiteral("scope")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("source_linked"), linked);
        QVERIFY(write(docs.filePath(sourceId), current));
        const QString sidecar=docs.filePath(QStringLiteral("comments/example_comments.jsonl"));
        QVERIFY(write(sidecar,QJsonDocument(comment).toJson(QJsonDocument::Compact)+'\n'));
        const auto result=synchronizer.prepareAnnotation(QStringLiteral("example"),QStringLiteral("short-heading"),
            [](const QString &,int,QString *) -> std::optional<DocsCommentSynchronizer::PageGeometry> { return DocsCommentSynchronizer::PageGeometry{600,600,0}; },pdfPath);
        if (!linked) {
            QFile tex(docs.filePath(sourceId)); QVERIFY(tex.open(QIODevice::ReadOnly));
            QCOMPARE(tex.readAll(), current);
            if (result.success) {
                QFile saved(sidecar); QVERIFY(saved.open(QIODevice::ReadOnly));
                QVERIFY(!saved.readAll().contains("\"anchor_status\":\"anchored\""));
            }
            return;
        }
        QVERIFY2(result.success,qPrintable(result.failureReason));
        QFile saved(sidecar); QVERIFY(saved.open(QIODevice::ReadOnly));
        const QByteArray records=saved.readAll();
        QVERIFY2(records.contains("\"anchor_status\":\"anchored\""),records.constData());
        QVERIFY(records.contains("saved_pdf_source_map"));
        QFile tex(docs.filePath(sourceId)); QVERIFY(tex.open(QIODevice::ReadOnly));
        const QByteArray anchored=tex.readAll();
        QVERIFY(anchored.contains("\\CommentHighlight{short-heading}"));
        QCOMPARE(anchored.count("\\CommentHighlight{short-heading}"),1);
    }
};
QTEST_GUILESS_MAIN(PublishedCommentAnchorTest)
#include "publishedcommentanchortest.moc"
