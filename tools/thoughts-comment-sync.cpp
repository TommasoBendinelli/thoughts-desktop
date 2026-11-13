/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "part/docscommentsynchronizer.h"

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTextStream>

#include <poppler-qt6.h>

#include <memory>
#include <optional>

namespace
{

QHash<QString, QByteArray> managedSourceSnapshot(const QString &docsRoot)
{
    QHash<QString, QByteArray> snapshot;
    const QDir root(docsRoot);
    const QFileInfoList projects = root.entryInfoList({QStringLiteral("docs-*")}, QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &project : projects) {
        QDirIterator files(project.absoluteFilePath(), {QStringLiteral("*.tex")}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = QFileInfo(files.next()).absoluteFilePath();
            QFile file(path);
            if (file.open(QIODevice::ReadOnly)) {
                snapshot.insert(path, file.readAll());
            }
        }
    }
    return snapshot;
}

QJsonArray changedSourcePaths(const QString &docsRoot, const QHash<QString, QByteArray> &before)
{
    const QHash<QString, QByteArray> after = managedSourceSnapshot(docsRoot);
    QSet<QString> paths;
    for (auto iterator = before.cbegin(); iterator != before.cend(); ++iterator) {
        paths.insert(iterator.key());
    }
    for (auto iterator = after.cbegin(); iterator != after.cend(); ++iterator) {
        paths.insert(iterator.key());
    }
    QStringList changed;
    for (const QString &path : paths) {
        if (before.value(path) != after.value(path)) {
            changed.append(QDir(docsRoot).relativeFilePath(path));
        }
    }
    changed.sort();
    QJsonArray result;
    for (const QString &path : changed) {
        result.append(path);
    }
    return result;
}

int normalizedRotation(Poppler::Page::Orientation orientation)
{
    switch (orientation) {
    case Poppler::Page::Landscape:
        return 1;
    case Poppler::Page::Portrait:
        return 0;
    case Poppler::Page::Seascape:
        return 3;
    case Poppler::Page::UpsideDown:
        return 2;
    }
    return 0;
}

class PdfGeometryReader
{
public:
    std::optional<Okular::DocsCommentSynchronizer::PageGeometry> geometry(const QString &pdfPath, int pageNumber, QString *error)
    {
        if (!m_pages.contains(pdfPath)) {
            std::unique_ptr<Poppler::Document> document(Poppler::Document::load(pdfPath));
            if (!document) {
                if (error) {
                    *error = QStringLiteral("could not open PDF: %1").arg(pdfPath);
                }
                return std::nullopt;
            }
            QList<Okular::DocsCommentSynchronizer::PageGeometry> pages;
            pages.reserve(document->numPages());
            for (int index = 0; index < document->numPages(); ++index) {
                std::unique_ptr<Poppler::Page> page(document->page(index));
                if (!page) {
                    if (error) {
                        *error = QStringLiteral("could not read page %1 from %2").arg(index + 1).arg(pdfPath);
                    }
                    return std::nullopt;
                }
                const QSizeF size = page->pageSizeF();
                pages.append({
                    .width = size.width(),
                    .height = size.height(),
                    .rotation = normalizedRotation(page->orientation()),
                });
            }
            m_pages.insert(pdfPath, pages);
        }

        const QList<Okular::DocsCommentSynchronizer::PageGeometry> &pages = m_pages[pdfPath];
        if (pageNumber < 1 || pageNumber > pages.size()) {
            if (error) {
                *error = QStringLiteral("page %1 is outside %2").arg(pageNumber).arg(pdfPath);
            }
            return std::nullopt;
        }
        return pages.at(pageNumber - 1);
    }

private:
    QHash<QString, QList<Okular::DocsCommentSynchronizer::PageGeometry>> m_pages;
};

}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("thoughts-comment-sync"));
    QCoreApplication::setApplicationVersion(QStringLiteral("1"));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Prepare canonical docs sidecars with Thoughts' native comment synchronizer."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("command"), QStringLiteral("Command to run: prepare-all, prepare-one, or resolve-source."));
    const QCommandLineOption docsRootOption({QStringLiteral("d"), QStringLiteral("docs-root")}, QStringLiteral("Root of the docs workspace."), QStringLiteral("path"));
    const QCommandLineOption stemOption(QStringLiteral("stem"), QStringLiteral("Prepare only one canonical PDF stem."), QStringLiteral("stem"));
    const QCommandLineOption annotationIdOption(QStringLiteral("annotation-id"), QStringLiteral("Prepare only this stable annotation identifier."), QStringLiteral("id"));
    const QCommandLineOption excludeStemOption(QStringLiteral("exclude-stem"), QStringLiteral("Do not prepare this canonical PDF stem; may be repeated."), QStringLiteral("stem"));
    const QCommandLineOption synctexOption(QStringLiteral("synctex"), QStringLiteral("Explicit SyncTeX executable."), QStringLiteral("path"));
    const QCommandLineOption timeoutOption(QStringLiteral("timeout-ms"), QStringLiteral("Per-lookup SyncTeX timeout in milliseconds."), QStringLiteral("milliseconds"), QStringLiteral("1000"));
    const QCommandLineOption sourceBundleRootOption(QStringLiteral("source-bundle-root"), QStringLiteral("Installed verified source-artifact bundle root."), QStringLiteral("path"));
    const QCommandLineOption pdfOption(QStringLiteral("pdf"), QStringLiteral("Exact PDF path used to verify a published source artifact."), QStringLiteral("path"));
    const QCommandLineOption artifactOnlyOption(QStringLiteral("artifact-only"), QStringLiteral("Resolve only from the installed verified source artifact."));
    const QCommandLineOption pdfStemOption(QStringLiteral("pdf-stem"), QStringLiteral("Canonical PDF stem for source resolution."), QStringLiteral("stem"));
    const QCommandLineOption pageOption(QStringLiteral("page"), QStringLiteral("One-based combined PDF page."), QStringLiteral("page"));
    const QCommandLineOption xOption(QStringLiteral("x"), QStringLiteral("PDF x coordinate."), QStringLiteral("points"));
    const QCommandLineOption yOption(QStringLiteral("y"), QStringLiteral("PDF y coordinate."), QStringLiteral("points"));
    const QCommandLineOption selectedTextOption(QStringLiteral("selected-text"), QStringLiteral("Selected PDF text used for safe source-line recovery."), QStringLiteral("text"));
    const QCommandLineOption jsonOption(QStringLiteral("json"), QStringLiteral("Write a machine-readable prepare result to stdout."));
    parser.addOption(docsRootOption);
    parser.addOption(stemOption);
    parser.addOption(annotationIdOption);
    parser.addOption(excludeStemOption);
    parser.addOption(synctexOption);
    parser.addOption(timeoutOption);
    parser.addOption(sourceBundleRootOption);
    parser.addOption(pdfOption);
    parser.addOption(artifactOnlyOption);
    parser.addOption(pdfStemOption);
    parser.addOption(pageOption);
    parser.addOption(xOption);
    parser.addOption(yOption);
    parser.addOption(selectedTextOption);
    parser.addOption(jsonOption);
    parser.process(application);

    QTextStream out(stdout);
    QTextStream err(stderr);
    const QStringList positional = parser.positionalArguments();
    if (positional.size() != 1 || !parser.isSet(docsRootOption)) {
        err << "error: usage: thoughts-comment-sync <prepare-all|prepare-one|resolve-source> --docs-root PATH\n";
        parser.showHelp(2);
    }
    const QString command = positional.constFirst();
    if (command != QLatin1String("prepare-all") && command != QLatin1String("prepare-one") && command != QLatin1String("resolve-source")) {
        err << "error: unknown command: " << command << '\n';
        return 2;
    }

    bool timeoutOk = false;
    const int timeoutMs = parser.value(timeoutOption).toInt(&timeoutOk);
    if (!timeoutOk || timeoutMs < 1) {
        err << "error: --timeout-ms must be a positive integer\n";
        return 2;
    }
    const QString docsRoot = QFileInfo(parser.value(docsRootOption)).absoluteFilePath();
    if (!QFileInfo(docsRoot).isDir()) {
        err << "error: docs root is not a directory: " << docsRoot << '\n';
        return 2;
    }

    Okular::DocsCommentSynchronizer synchronizer({
        .docsRoot = docsRoot,
        .sourceBundleRoot = parser.value(sourceBundleRootOption),
        .synctexExecutable = parser.value(synctexOption),
        .processEnvironment = QProcessEnvironment::systemEnvironment(),
        .synctexTimeoutMs = timeoutMs,
        .lockTimeoutMs = 2000,
        .allowLegacySourceResolution = !parser.isSet(artifactOnlyOption),
    });
    if (command == QLatin1String("resolve-source")) {
        bool pageOk = false;
        bool xOk = false;
        bool yOk = false;
        const int page = parser.value(pageOption).toInt(&pageOk);
        const double x = parser.value(xOption).toDouble(&xOk);
        const double y = parser.value(yOption).toDouble(&yOk);
        if (parser.value(pdfStemOption).isEmpty() || !pageOk || page < 1 || !xOk || !yOk) {
            err << "error: resolve-source requires --pdf-stem, --page, --x, and --y\n";
            return 2;
        }
        const auto result = synchronizer.resolveSource({
            .pdfStem = parser.value(pdfStemOption),
            .pageNumber = page,
            .x = x,
            .y = y,
            .selectedText = parser.value(selectedTextOption),
            .pdfPath = parser.value(pdfOption),
        });
        if (!result.success) {
            err << "error: " << result.failureReason << '\n';
            return 1;
        }
        const QJsonObject payload {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("source_file"), result.sourceId},
            {QStringLiteral("source_path"), result.sourcePath},
            {QStringLiteral("source_line"), result.line},
            {QStringLiteral("source_column"), result.column},
            {QStringLiteral("source_build_id"), result.buildId},
        };
        out << QJsonDocument(payload).toJson(QJsonDocument::Compact) << '\n';
        return 0;
    }
    PdfGeometryReader geometryReader;
    const auto geometry = [&geometryReader](const QString &pdfPath, int pageNumber, QString *error) { return geometryReader.geometry(pdfPath, pageNumber, error); };
    const QStringList excludedStemValues = parser.values(excludeStemOption);
    const QSet<QString> excludedStems(excludedStemValues.cbegin(), excludedStemValues.cend());
    const QHash<QString, QByteArray> sourceBefore = parser.isSet(jsonOption) ? managedSourceSnapshot(docsRoot) : QHash<QString, QByteArray>();
    if (command == QLatin1String("prepare-one")) {
        const QString stem = parser.value(stemOption);
        const QString annotationId = parser.value(annotationIdOption);
        if (stem.isEmpty() || annotationId.isEmpty()) {
            err << "error: prepare-one requires --stem and --annotation-id\n";
            return 2;
        }
        const Okular::DocsCommentSynchronizer::Result result = synchronizer.prepareAnnotation(stem, annotationId, geometry, parser.value(pdfOption));
        if (parser.isSet(jsonOption)) {
            const QJsonObject payload {
                {QStringLiteral("schema_version"), 1},
                {QStringLiteral("success"), result.success},
                {QStringLiteral("failure_reason"), result.failureReason},
                {QStringLiteral("pdf_stem"), stem},
                {QStringLiteral("annotation_id"), annotationId},
                {QStringLiteral("visible_comments"), result.visibleComments},
                {QStringLiteral("anchors_inserted"), result.anchorsInserted},
                {QStringLiteral("anchor_mirrors_updated"), result.anchorMirrorsUpdated},
                {QStringLiteral("source_anchor_deletions"), result.sourceAnchorDeletions},
                {QStringLiteral("source_anchors_orphaned"), result.sourceAnchorsOrphaned},
                {QStringLiteral("changed_source_paths"), changedSourcePaths(docsRoot, sourceBefore)},
                {QStringLiteral("total_ms"), result.totalMs},
            };
            out << QJsonDocument(payload).toJson(QJsonDocument::Compact) << '\n';
        } else if (result.success) {
            out << stem << ": annotation=" << annotationId << " inserted=" << result.anchorsInserted << " mirrors=" << result.anchorMirrorsUpdated << " total-ms=" << result.totalMs << '\n';
        }
        if (!result.success) {
            err << "error: " << result.failureReason << '\n';
            return 1;
        }
        return 0;
    }
    const Okular::DocsCommentSynchronizer::BatchResult result = synchronizer.prepareAllSidecars(geometry, parser.value(stemOption), excludedStems);

    if (parser.isSet(jsonOption)) {
        QJsonArray documents;
        for (const Okular::DocsCommentSynchronizer::DocumentResult &document : result.documents) {
            documents.append(QJsonObject {
                {QStringLiteral("pdf_stem"), document.pdfStem},
                {QStringLiteral("success"), document.result.success},
                {QStringLiteral("failure_reason"), document.result.failureReason},
                {QStringLiteral("visible_comments"), document.result.visibleComments},
                {QStringLiteral("anchors_inserted"), document.result.anchorsInserted},
                {QStringLiteral("anchor_mirrors_updated"), document.result.anchorMirrorsUpdated},
                {QStringLiteral("source_comments_moved"), document.result.sourceCommentsMoved},
                {QStringLiteral("source_anchor_deletions"), document.result.sourceAnchorDeletions},
                {QStringLiteral("source_anchors_orphaned"), document.result.sourceAnchorsOrphaned},
                {QStringLiteral("total_ms"), document.result.totalMs},
            });
        }
        const QJsonObject payload {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("success"), result.success},
            {QStringLiteral("failure_reason"), result.failureReason},
            {QStringLiteral("documents"), documents},
            {QStringLiteral("changed_source_paths"), changedSourcePaths(docsRoot, sourceBefore)},
            {QStringLiteral("total_ms"), result.totalMs},
        };
        out << QJsonDocument(payload).toJson(QJsonDocument::Compact) << '\n';
    } else for (const Okular::DocsCommentSynchronizer::DocumentResult &document : result.documents) {
        if (!document.result.success) {
            err << document.pdfStem << ": " << document.result.failureReason << '\n';
            continue;
        }
        out << document.pdfStem << ": comments=" << document.result.visibleComments << " markup=" << document.result.plainMarkups << " inserted=" << document.result.anchorsInserted << " mirrors=" << document.result.anchorMirrorsUpdated
            << " source-text-imports=" << document.result.sourceCommentEditsImported << " moved=" << document.result.sourceCommentsMoved << " resolved=" << document.result.sourceAnchorDeletions << " load-ms=" << document.result.loadMs
            << " anchors-ms=" << document.result.anchorMs << " write-ms=" << document.result.writeMs << " total-ms=" << document.result.totalMs << '\n';
    }
    if (!result.success) {
        err << "error: " << result.failureReason << '\n';
        return 1;
    }
    if (!parser.isSet(jsonOption)) {
        out << "Prepared " << result.documents.size() << " sidecar(s); comments=" << result.visibleComments << " markup=" << result.plainMarkups << " inserted=" << result.anchorsInserted << " mirrors=" << result.anchorMirrorsUpdated
            << " source-text-imports=" << result.sourceCommentEditsImported << " moved=" << result.sourceCommentsMoved << " resolved=" << result.sourceAnchorDeletions << " total-ms=" << result.totalMs << '\n';
    }
    return 0;
}
