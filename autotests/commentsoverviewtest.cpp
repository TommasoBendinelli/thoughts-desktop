#include <thoughts/commentservice.h>
/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "../shell/commentsoverview.h"
#include "../part/agentconversations.h"
#include "../part/commentmissingstate.h"
#include "../shell/reviewoverview.h"
#include "../shell/rowbrowsertree.h"
#include "../shell/agentproposalparser.h"
#include "../part/sourceannotationindex.h"
#include "../part/proposalhistory.h"
#include "../part/currentannotationindex.h"
#include "../shell/commentcodexlauncher.h"
#include "../shell/documentsyncpresentation.h"
#include "../part/docscommentsynchronizer.h"
#include "../part/libraryprojectstore.h"
#include "../part/librarysourceinventory.h"
#include "../shell/librarymodificationdates.h"
#include "../shell/documentmanagementdialog.h"
#include "../shell/developerlog.h"
#include "../shell/librarysearch.h"
#include "../part/thoughtsserveraddress.h"

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFile>
#include <QFormLayout>
#include <QGroupBox>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QLocale>
#include <QLockFile>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPersistentModelIndex>
#include <QSortFilterProxyModel>
#include <QProcess>
#include <QPdfWriter>
#include <QPainter>
#include <QTreeWidgetItemIterator>
#include <QProgressBar>
#include <QPointer>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalSpy>
#include <QDesktopServices>
#include <QSplitter>
#include <QStandardPaths>
#include <QStandardItemModel>
#include <QTreeView>
#include <QTreeWidget>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include "../part/sourcedocumentmove.h"
#include <QUuid>
#include <tuple>
#include "../part/documentfeedback.h"
#include "../part/reviewcardfeedback.h"

#include <QTest>
#include <QTabWidget>
#include <QTextCursor>
#include <QTimeZone>
#include <QTimer>
#include <QTextBrowser>
#include <QWheelEvent>

#include <KConfigGroup>
#include <KSharedConfig>

#include <algorithm>
#include <numeric>
#include <time.h>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace
{

// Process pending layout requests, which climb one widget level per pass, so
// that a window has grown to fit newly shown content.
void settleLayouts()
{
    for (int pass = 0; pass < 3; ++pass) {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
        QCoreApplication::processEvents();
    }
}

// Squeeze the overview to its narrowest width and name every visible control
// in the details panel that ends up narrower than its contents need.
QStringList clippedDetailsControls(QWidget &overview)
{
    const QSize originalSize = overview.size();
    const auto restoreSize = qScopeGuard([&overview, originalSize] { overview.resize(originalSize); });
    settleLayouts();
    overview.resize(1, overview.height());
    settleLayouts();
    auto *details = overview.findChild<QWidget *>(QStringLiteral("commentsOverviewDetails"));
    if (!details) {
        return {QStringLiteral("commentsOverviewDetails is missing")};
    }
    QStringList clipped;
    for (QWidget *control : details->findChildren<QWidget *>()) {
        const bool checked = qobject_cast<QAbstractButton *>(control) || qobject_cast<QAbstractSpinBox *>(control)
            || qobject_cast<QComboBox *>(control) || qobject_cast<QLabel *>(control);
        // Skip the private parts of composite widgets, such as a line edit's clear button.
        QWidget *parent = control->parentWidget();
        const bool internal = qobject_cast<QLineEdit *>(parent) || qobject_cast<QAbstractSpinBox *>(parent) || qobject_cast<QComboBox *>(parent);
        if (!checked || internal || !control->isVisible() || control->sizePolicy().horizontalPolicy() == QSizePolicy::Ignored) {
            continue;
        }
        if (control->width() < control->minimumSizeHint().width()) {
            const QString text = control->property("text").toString();
            clipped.append(QStringLiteral("%1 \"%2\" %3<%4").arg(control->objectName(), text).arg(control->width()).arg(control->minimumSizeHint().width()));
        }
    }
    return clipped;
}

bool writeFile(const QString &path, const QByteArray &contents)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) && file.write(contents) == contents.size();
}

QByteArray fileContents(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool registerSourceProject(const QString &root, const QString &id, const QString &name, const QStringList &paths)
{
    auto config = Thoughts::ProjectConfiguration::empty(root, id, name);
    auto folders = config.value(QStringLiteral("source_folders")).toArray();
    for (const QString &path : paths)
        folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), path}});
    config.insert(QStringLiteral("source_folders"), folders);
    return Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root, id), config).isEmpty();
}

// Library projects come only from current Project configurations. Declare a
// catalogue project's connected folders and documents so the catalogue entry
// supplies the published details of each declared document.
bool registerCatalogueProject(const QString &root, const QJsonObject &project)
{
    const QString id = project.value(QStringLiteral("id")).toString();
    auto config = Thoughts::ProjectConfiguration::empty(root, id, project.value(QStringLiteral("name")).toString());
    auto folders = config.value(QStringLiteral("source_folders")).toArray();
    QHash<QString, QString> folderIds;
    const auto connect = [&](const QString &owner) {
        if (owner.isEmpty() || folderIds.contains(owner)) return;
        folderIds.insert(owner, QUuid::createUuid().toString(QUuid::WithoutBraces));
        folders.append(QJsonObject{{QStringLiteral("folder_id"), folderIds.value(owner)},
            {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), owner}});
    };
    connect(project.value(QStringLiteral("source_project")).toString());
    for (const auto &folder : project.value(QStringLiteral("source_folders")).toArray())
        connect(folder.toObject().value(QStringLiteral("project")).toString());
    QJsonArray documents;
    for (const auto &value : project.value(QStringLiteral("documents")).toArray()) {
        const auto document = value.toObject();
        QString source = document.value(QStringLiteral("source_file")).toString();
        if (source.isEmpty() && document.value(QStringLiteral("id")).toString().startsWith(QLatin1String("docs-")))
            source = document.value(QStringLiteral("id")).toString();
        QJsonObject declaration;
        if (source.isEmpty()) {
            const QString filename = document.value(QStringLiteral("filename")).toString(QFileInfo(document.value(QStringLiteral("id")).toString()).fileName());
            declaration = {{QStringLiteral("format"), QStringLiteral("pdf")},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), config.value(QStringLiteral("default_folder_id"))}, {QStringLiteral("path"), filename}}}};
        } else {
            const QString owner = source.section(QLatin1Char('/'), 0, 0), path = source.section(QLatin1Char('/'), 1);
            connect(owner);
            declaration = {{QStringLiteral("format"), path.endsWith(QLatin1String(".md")) ? QStringLiteral("markdown") : QStringLiteral("latex")},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folderIds.value(owner)}, {QStringLiteral("path"), path}}}};
        }
        declaration.insert(QStringLiteral("document_id"), document.value(QStringLiteral("document_id")).toString(QUuid::createUuid().toString(QUuid::WithoutBraces)));
        declaration.insert(QStringLiteral("included"), true);
        documents.append(declaration);
    }
    config.insert(QStringLiteral("source_folders"), folders);
    config.insert(QStringLiteral("documents"), documents);
    return Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root, id), config).isEmpty();
}

// Library rows are sorted, so look children up by their label.
QTreeWidgetItem *childNamed(QTreeWidgetItem *parent, const QString &text)
{
    for (int i = 0; parent && i < parent->childCount(); ++i)
        if (parent->child(i)->text(0) == text) return parent->child(i);
    return nullptr;
}

// The Project's default source folder, which also holds its imported PDFs.
QString defaultSourceFolder(const QString &root, const QString &id)
{
    return QFileInfo(Thoughts::ProjectConfiguration::path(root, id)).absolutePath();
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

QByteArray jsonLine(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

QJsonObject layout(const QString &stem)
{
    QJsonObject result;
    result.insert(QStringLiteral("version"), 1);
    result.insert(QStringLiteral("pdf_stem"), QJsonValue(QString(stem + QStringLiteral("_combined"))));
    result.insert(QStringLiteral("sources"), QJsonArray {QJsonObject {{QStringLiteral("source_id"), QJsonValue(QStringLiteral("docs-test/tex/source/main.tex"))}}});
    return result;
}

QJsonObject comment(const QString &id, const QString &contents, int page, const QString &updatedAt, const QString &anchorStatus)
{
    return {
        {QStringLiteral("schema_version"), 4},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("id"), id},
        {QStringLiteral("contents"), contents},
        {QStringLiteral("author"), QStringLiteral("Alex")},
        {QStringLiteral("page"), page},
        {QStringLiteral("subtype"), QStringLiteral("Text")},
        {QStringLiteral("created"), updatedAt},
        {QStringLiteral("modified"), updatedAt},
        {QStringLiteral("updated_at"), updatedAt},
        {QStringLiteral("anchor_status"), anchorStatus},
        {QStringLiteral("tex_file"), QStringLiteral("docs-test/tex/source/main.tex")},
        {QStringLiteral("tex_line"), page + 10},
    };
}

bool writeAgentProposalBundle(const QString &root, const QString &pdfFilename, const QString &proposalId, const QString &artifactPdfSha256 = {})
{
    const QByteArray pdfContents = readFile(root + QStringLiteral("/current_pdfs/") + pdfFilename);
    const QString pdfSha256 = artifactPdfSha256.isEmpty()
        ? QString::fromLatin1(QCryptographicHash::hash(pdfContents, QCryptographicHash::Sha256).toHex())
        : artifactPdfSha256;
    if (pdfContents.isEmpty() || pdfSha256.size() != 64) {
        return false;
    }
    const QJsonObject fragment {{QStringLiteral("pdf_filename"), pdfFilename},
                                {QStringLiteral("page"), 1},
                                {QStringLiteral("left"), 72.0},
                                {QStringLiteral("top"), 100.0},
                                {QStringLiteral("right"), 300.0},
                                {QStringLiteral("bottom"), 112.0}};
    const QJsonObject placement {{QStringLiteral("status"), QStringLiteral("anchored")},
                                 {QStringLiteral("quad_fragments"), QJsonArray {fragment}}};
    const QJsonObject proposal {
        {QStringLiteral("id"), proposalId},
        {QStringLiteral("comment"), QStringLiteral("This sentence is redundant.")},
        {QStringLiteral("body_preview"), QStringLiteral("Remove this whole sentence.")},
        {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
        {QStringLiteral("source_sha256"), QString(64, QLatin1Char('b'))},
        {QStringLiteral("line"), 12},
        {QStringLiteral("parent_id"), QJsonValue::Null},
        {QStringLiteral("suppressed"), false},
        {QStringLiteral("pdf_filenames"), QJsonArray {pdfFilename}},
        {QStringLiteral("placement"), placement},
    };
    const QString proposalObject = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(proposal).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    const QJsonObject manifest {{QStringLiteral("schema_version"), 1},
                                {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), pdfFilename},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), pdfFilename}, {QStringLiteral("pdf_sha256"), pdfSha256}}},
                                {QStringLiteral("sources"), QJsonArray()},
                                {QStringLiteral("agent_proposals"), QJsonArray {proposal}},
                                {QStringLiteral("proposal_objects"), QJsonArray {QJsonObject {{QStringLiteral("id"), proposalId}, {QStringLiteral("object"), proposalObject}}}}};
    const QByteArray contents = QJsonDocument(manifest).toJson(QJsonDocument::Compact) + '\n';
    const QString bundleId = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    const QString bundleRoot = root + QStringLiteral("/.runtime/target/source-bundle");
    return writeFile(bundleRoot + QStringLiteral("/objects/") + bundleId, contents)
        && writeFile(bundleRoot + QStringLiteral("/documents/") + pdfFilename + QStringLiteral(".json"),
                     QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1},
                                                {QStringLiteral("artifact_sha256"), bundleId},
                                                {QStringLiteral("filename"), pdfFilename},
                                                {QStringLiteral("pdf_sha256"), pdfSha256}})
                         .toJson());
}

bool writeReviewCommentBundle(const QString &root, const QString &pdfFilename, const QJsonArray &reviewComments)
{
    const QByteArray pdfContents = readFile(root + QStringLiteral("/current_pdfs/") + pdfFilename);
    const QString pdfSha256 = QString::fromLatin1(QCryptographicHash::hash(pdfContents, QCryptographicHash::Sha256).toHex());
    if (pdfContents.isEmpty()) {
        return false;
    }
    const QString sourcePath = QStringLiteral("docs-test/tex/source/main.tex");
    const QJsonObject manifest {{QStringLiteral("schema_version"), 1},
                                {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), pdfFilename},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), pdfFilename}, {QStringLiteral("pdf_sha256"), pdfSha256}}},
                                {QStringLiteral("sources"), QJsonArray {QJsonObject {{QStringLiteral("path"), sourcePath}, {QStringLiteral("sha256"), QString(64, QLatin1Char('b'))}}}},
                                {QStringLiteral("agent_proposals"), QJsonArray()},
                                {QStringLiteral("proposal_objects"), QJsonArray()},
                                {QStringLiteral("review_comments_schema_version"), 1},
                                {QStringLiteral("review_comments"), reviewComments}};
    const QByteArray contents = QJsonDocument(manifest).toJson(QJsonDocument::Compact) + '\n';
    const QString bundleId = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    const QString bundleRoot = root + QStringLiteral("/.runtime/target/source-bundle");
    return writeFile(bundleRoot + QStringLiteral("/objects/") + bundleId, contents)
        && writeFile(bundleRoot + QStringLiteral("/documents/") + pdfFilename + QStringLiteral(".json"),
                     QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1},
                                                {QStringLiteral("artifact_sha256"), bundleId},
                                                {QStringLiteral("filename"), pdfFilename},
                                                {QStringLiteral("pdf_sha256"), pdfSha256}})
                         .toJson());
}

bool writeLogicalCommentBundle(const QString &root, const QString &pdfFilename, const QJsonArray &logicalComments)
{
    const QByteArray pdfContents = readFile(root + QStringLiteral("/current_pdfs/") + pdfFilename);
    const QString pdfSha256 = QString::fromLatin1(QCryptographicHash::hash(pdfContents, QCryptographicHash::Sha256).toHex());
    if (pdfContents.isEmpty()) {
        return false;
    }
    QJsonArray records;
    QJsonArray correspondences;
    for (const auto &value : logicalComments) {
        QJsonObject record = value.toObject();
        const QString id = record.value(QStringLiteral("comment_id")).toString();
        correspondences.append(QJsonObject {{QStringLiteral("schema_version"), 2},
            {QStringLiteral("review_key"), QString(QStringLiteral("comment:") + id)},
            {QStringLiteral("state"), QStringLiteral("exact_point")},
            {QStringLiteral("locations"), record.take(QStringLiteral("locations"))}});
        records.append(record);
    }
    const QJsonObject manifest {{QStringLiteral("schema_version"), 1},
                                {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), pdfFilename},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), pdfFilename}, {QStringLiteral("pdf_sha256"), pdfSha256}}},
                                {QStringLiteral("sources"), QJsonArray()},
                                {QStringLiteral("agent_proposals"), QJsonArray()},
                                {QStringLiteral("proposal_objects"), QJsonArray()},
                                {QStringLiteral("review_comments_schema_version"), 1},
                                {QStringLiteral("review_comments"), QJsonArray()},
                                {QStringLiteral("logical_comments_schema_version"), 1},
                                {QStringLiteral("logical_comments"), records},
                                {QStringLiteral("review_correspondence_schema_version"), 2},
                                {QStringLiteral("review_correspondences"), correspondences}};
    const QByteArray contents = QJsonDocument(manifest).toJson(QJsonDocument::Compact) + '\n';
    const QString bundleId = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    const QString bundleRoot = root + QStringLiteral("/.runtime/target/source-bundle");
    return writeFile(bundleRoot + QStringLiteral("/objects/") + bundleId, contents)
        && writeFile(bundleRoot + QStringLiteral("/documents/") + pdfFilename + QStringLiteral(".json"),
                     QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1},
                                                {QStringLiteral("artifact_sha256"), bundleId},
                                                {QStringLiteral("filename"), pdfFilename},
                                                {QStringLiteral("pdf_sha256"), pdfSha256}})
                         .toJson());
}

bool writeAgentEditProposalBundle(const QString &root,
                                  const QString &pdfFilename,
                                  const QString &proposalId,
                                  const QString &beforePreview = QStringLiteral("Standard SDPO"),
                                  const QString &afterPreview = QStringLiteral("On-policy SDPO"),
                                  bool includeSource = false,
                                  bool includeV2Correspondence = false)
{
    const QByteArray pdfContents = readFile(root + QStringLiteral("/current_pdfs/") + pdfFilename);
    const QString pdfSha256 = QString::fromLatin1(QCryptographicHash::hash(pdfContents, QCryptographicHash::Sha256).toHex());
    if (pdfContents.isEmpty()) {
        return false;
    }
    const auto fragment = [&pdfFilename](double left, double right) {
        return QJsonObject {{QStringLiteral("pdf_filename"), pdfFilename},
                            {QStringLiteral("page"), 1},
                            {QStringLiteral("left"), left},
                            {QStringLiteral("top"), 100.0},
                            {QStringLiteral("right"), right},
                            {QStringLiteral("bottom"), 112.0}};
    };
    const QJsonObject placement {
        {QStringLiteral("status"), QStringLiteral("anchored")},
        {QStringLiteral("regions"),
         QJsonArray {QJsonObject {{QStringLiteral("role"), QStringLiteral("before")}, {QStringLiteral("quad_fragments"), QJsonArray {fragment(72.0, 180.0)}}},
                     QJsonObject {{QStringLiteral("role"), QStringLiteral("after")}, {QStringLiteral("quad_fragments"), QJsonArray {fragment(200.0, 340.0)}}}}},
    };
    const QJsonObject proposal {
        {QStringLiteral("id"), proposalId},
        {QStringLiteral("kind"), QStringLiteral("edit")},
        {QStringLiteral("comment"), QStringLiteral("The objective label was stale.")},
        {QStringLiteral("body_preview"), beforePreview},
        {QStringLiteral("before"), QJsonObject {{QStringLiteral("preview"), beforePreview}}},
        {QStringLiteral("after"), QJsonObject {{QStringLiteral("preview"), afterPreview}}},
        {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
        {QStringLiteral("source_sha256"), QString(64, QLatin1Char('b'))},
        {QStringLiteral("line"), 12},
        {QStringLiteral("parent_id"), QJsonValue::Null},
        {QStringLiteral("suppressed"), false},
        {QStringLiteral("pdf_filenames"), QJsonArray {pdfFilename}},
        {QStringLiteral("placement"), placement},
    };
    const QString proposalObject = QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(proposal).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex());
    QJsonObject manifest {{QStringLiteral("schema_version"), 1},
                                {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), pdfFilename},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), pdfFilename}, {QStringLiteral("pdf_sha256"), pdfSha256}}},
                                {QStringLiteral("sources"), includeSource ? QJsonArray {QJsonObject {{QStringLiteral("path"), QStringLiteral("docs-test/tex/source/main.tex")}}} : QJsonArray()},
                                {QStringLiteral("agent_proposals"), QJsonArray {proposal}},
                                {QStringLiteral("proposal_objects"), QJsonArray {QJsonObject {{QStringLiteral("id"), proposalId}, {QStringLiteral("object"), proposalObject}}}}};
    if (includeV2Correspondence) {
        const QString sourceBundleSha256(64, QLatin1Char('c'));
        const QJsonObject proof {{QStringLiteral("resolver_version"), 2},
                                 {QStringLiteral("source_sha256"), QString(64, QLatin1Char('b'))},
                                 {QStringLiteral("source_bundle_sha256"), sourceBundleSha256},
                                 {QStringLiteral("build_sha256"), QString(64, QLatin1Char('f'))},
                                 {QStringLiteral("pdf_sha256"), pdfSha256},
                                 {QStringLiteral("method"), QStringLiteral("named_span_synctex")},
                                 {QStringLiteral("extracted_text_sha256"), QString(64, QLatin1Char('d'))},
                                 {QStringLiteral("uniqueness"), QStringLiteral("unique")},
                                 {QStringLiteral("candidate_count"), 1},
                                 {QStringLiteral("verified"), true},
                                 {QStringLiteral("reason"), QStringLiteral("complete named span")}};
        const QJsonObject sourceTarget {
            {QStringLiteral("role"), QStringLiteral("after")},
            {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
            {QStringLiteral("source_sha256"), QString(64, QLatin1Char('b'))},
            {QStringLiteral("byte_start"), 0},
            {QStringLiteral("byte_end"), 7},
            {QStringLiteral("line_start"), 1},
            {QStringLiteral("line_end"), 1},
            {QStringLiteral("column_start"), 1},
            {QStringLiteral("column_end"), 8},
            {QStringLiteral("text_sha256"), QString(64, QLatin1Char('e'))},
            {QStringLiteral("render_intent"), QStringLiteral("text")},
            {QStringLiteral("anchor_id"), proposalId},
        };
        const QJsonObject location {
            {QStringLiteral("pdf_filename"), pdfFilename},
            {QStringLiteral("pdf_sha256"), pdfSha256},
            {QStringLiteral("source_bundle_sha256"), sourceBundleSha256},
            {QStringLiteral("build_sha256"), QString(64, QLatin1Char('f'))},
            {QStringLiteral("page_index"), 0},
            {QStringLiteral("page"), 1},
            {QStringLiteral("role"), QStringLiteral("after")},
            {QStringLiteral("placement_kind"), QStringLiteral("exact_text")},
            {QStringLiteral("regions"),
             QJsonArray {QJsonArray {0.1, 0.2, 0.7, 0.22},
                         QJsonArray {0.1, 0.23, 0.8, 0.25}}},
            {QStringLiteral("point"), QJsonArray()},
            {QStringLiteral("proof"), proof},
        };
        QJsonObject correspondence;
        correspondence.insert(QStringLiteral("schema_version"), 2);
        correspondence.insert(QStringLiteral("review_key"), QString(QStringLiteral("proposal:") + proposalId));
        correspondence.insert(QStringLiteral("kind"), QStringLiteral("agent_edit"));
        correspondence.insert(QStringLiteral("item_id"), proposalId);
        correspondence.insert(QStringLiteral("state"), QStringLiteral("exact_text"));
        correspondence.insert(QStringLiteral("reason"), QStringLiteral("Complete rendered replacement verified."));
        correspondence.insert(QStringLiteral("source_bundle_sha256"), sourceBundleSha256);
        correspondence.insert(QStringLiteral("source_targets"), QJsonArray {sourceTarget});
        correspondence.insert(QStringLiteral("locations"), QJsonArray {location});
        correspondence.insert(QStringLiteral("proof"), proof);
        manifest.insert(QStringLiteral("review_correspondence_schema_version"), 2);
        manifest.insert(QStringLiteral("review_correspondences"), QJsonArray {correspondence});
    }
    const QByteArray contents = QJsonDocument(manifest).toJson(QJsonDocument::Compact) + '\n';
    const QString bundleId = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    const QString bundleRoot = root + QStringLiteral("/.runtime/target/source-bundle");
    return writeFile(bundleRoot + QStringLiteral("/objects/") + bundleId, contents)
        && writeFile(bundleRoot + QStringLiteral("/documents/") + pdfFilename + QStringLiteral(".json"),
                     QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1},
                                                {QStringLiteral("artifact_sha256"), bundleId},
                                                {QStringLiteral("filename"), pdfFilename},
                                                {QStringLiteral("pdf_sha256"), pdfSha256}})
                         .toJson());
}

// The navigation tree keeps its item kinds file-local, so the tests name them
// positionally as the implementation declares them.
enum NavigationItemKind {
    NavigationAllCommentsItem = 0,
    NavigationSectionHeadingItem = 1,
    NavigationDocumentEntry = 2,
    NavigationComponentEntry = 3,
    NavigationThreadGroupEntry = 4,
    NavigationProjectEntry = 5,
};

int navigationItemKind(const QTreeWidgetItem *item)
{
    return item ? item->data(0, Qt::UserRole + 2).toInt() : NavigationAllCommentsItem;
}

// Documents live under their project container, so look through the whole tree.
QTreeWidgetItem *documentItem(QTreeWidget *documents, const QString &stem)
{
    for (QTreeWidgetItemIterator it(documents); *it; ++it) {
        QTreeWidgetItem *item = *it;
        if (navigationItemKind(item) == NavigationDocumentEntry && item->data(0, Qt::UserRole).toString() == stem) {
            return item;
        }
    }
    return nullptr;
}

// A document is no longer a top-level row, so report its container's row for
// callers that still navigate through top-level indices.
int documentRow(QTreeWidget *documents, const QString &stem)
{
    QTreeWidgetItem *item = documentItem(documents, stem);
    if (!item) {
        return -1;
    }
    for (int row = 0; row < documents->topLevelItemCount(); ++row) {
        if (documents->topLevelItem(row) == item || documents->topLevelItem(row) == item->parent()) {
            return row;
        }
    }
    return -1;
}

}

class CommentsOverviewTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void init();
    void testProjectAttachmentManifestMutation();
    void testEmptyLibraryProjectPersistence();
    void testLibraryRequiresCurrentProjectManifest();
    void testProjectSizeIncludesAllSourcesAndUpdatesInPlace();
    void testProjectManifestRepairAndUniformIcons();
    void testRemovedProjectConfigurationIsRetired();
    void testCreatedProjectManifestFollowsFedora();
    void testLibraryDateFormatting();
    void testLibraryMetadataColumns();
    void testLibraryPagesByKindAndDueCards();
    void testLibraryHeaderColumnsMenu();
    void testProjectPagesCountBuiltDocuments();
    void testLibraryContentModificationDates();
    void testLibraryModificationDatesSavesPartialProgress();
    void testImportedProjectOpensPublishedPdf();
    void testMixedProjectDocumentsMenuWithoutTarget();
    void testImportedMarkdownFolderNavigation();
    void testHtmlSourceOpening();
    void testImportedDocumentSizes_data();
    void testImportedDocumentSizes();
    void testPendingMarkdownFolderSources();
    void testCatalogueSourceFolderMenu();
    void testRemoveSourceFolder();
    void testRenameSourceFolder();
    void testMoveSourceFolderPreview();
    void testRealImportedSourceLayout();
    void testConfiguredSourceRootsAndCachedHierarchy();
    void testImportedIgnoreFilesStayWithTheirRepositories();
    void testFailedProjectShowsWarningInLibrary();
    void testSourceFileRenameAndDelete();
    void testWebsiteImportPersistence();
    void testServerImportSourceLink();
    void testWebsiteImportMenu_data();
    void testWebsiteImportMenu();
    void testLibraryDocumentRename();
    void testDocumentPublicationMenu();
    void testNavigationUsesPreparedSettings();
    void testSettingsChangeRejectsPendingReload();
    void testDocumentBuildSelection();
    void testManageDocuments();
    void testManageDocumentsReusesRemovedIdentity();
    void testSingleProjectDocumentManagement();
    void testManageDefaultFolderDocuments();
    void testPdfOnlyDocumentActions();
    void testMoveDocumentDialog();
    void testMoveSourceDocumentDialog();
    void testMoveSourceDocumentStaysInItsTwoFolders();
    void testUnlinkedDocumentMenu();
    void testDocumentSettingsTransaction();
    void testProjectMenuCopiesProjectName();
    void testLibrarySummarySaysLoadingUntilFirstLoad();
    void testLibraryMenuSurvivesRefresh_data();
    void testLibraryMenuSurvivesRefresh();
    void testLibraryDocumentDoubleClickWithoutPage();
    void testLibrarySearchFindsSourceLines();
    void testLibrarySearchDocumentHitsOpenTheirSearch();
    void testSourceProjectCatalogueOrder_data();
    void testSourceProjectCatalogueOrder();
    void testStandaloneNoteWithoutDocuments();
    void testStandaloneNoteCreation();
    void testNoteAutosaveRejectsStaleRefresh();
    void testNoteUndoPreservesCalendar();
    void testStandaloneNoteAttachmentOpen();
    void testStandaloneNoteDeleteActions_data();
    void testStandaloneNoteDeleteActions();
    void testNotesNavigationCountsAndMutations();
    void testTodosHiddenUntilTheirDate();
    void testDiaryEntriesListedInDiaryNotTodos();
    void testOverviewTabs();
    void testReviewCardsRefreshWhenSourceLocationsChange();
    void testPersistentBrowsersKeepState();
    void testSyncStateChangeUpdatesDocumentRowsInPlace();
    void testTabSwitchScales_data();
    void testTabSwitchScales();
    void testTrackedLayoutLoading();
    void testSidecarSyncConflictsRemainVisible();
    void testAgentProposalRowsFiltersAndResolution();
    void testUnpublishedRegisteredProposalVisibleWithoutPdf();
    void testConnectedSourceProposalsUseLibraryProjectBeforeSync();
    void testAgentProposalRowsRejectStalePdfArtifact();
    void testCanonicalCommentRecordsOwnCatalogue();
    void testUnplacedCommentDoesNotBlockPreparation();
    void testPdfOnlyCommentOriginPage();
    void testLogicalCommentConsolidatesRevisionBoundLocations();
    void testAgentEditProposalSchemaThreeLoading();
    void testProofCarryingProposalShowsCompletePlacementEvidence();
    void testAgentAdditionProposalDetailUsesEmptyMarker();
    void testLiveTexProposalReplacesArtifactAndResolvesLocally();
    void testLiveProposalAcceptanceIsAsyncAndIncremental();
    void testAsyncProposalConflictPreservesSource();
    void testProjectCommentLoadIgnoresBrokenOtherProject();
    void testProposalPageSkipsLaTeXCopies();
    void testLiveTexProposalRefreshesWithoutPdfBuild();
    void testLiveTexProposalDoubleClickRequestsPdfFocus();
    void testAgentProposalParserRejectsNestedMacros();
    void testAgentProposalParserIgnoresDefinitions();
    void testGuardedAgentProposalResolver();
    void testDurableProposalHistoryAndRecovery();
    void testProposalResolvedOutsideThoughtsIsClosed();
    void testIgnoredHistoricalCopiesDoNotCreateProposalEvents();
    void testDuplicateProposalDoesNotManufactureMoves();
    void testProposalHistoryAppendPreservesLegacyJournal();
    void testCurrentAnnotationIndexInvalidatesOnRepositoryChange();
    void testCurrentAnnotationIndexJsonMatchesQt();
    void testProposalCurrentIgnoresUnsavedHistory();
    void testUnbuiltProposalRefreshesOnlyOnDiscovery();
    void testLogicalDatesUseSharedPolicy();
    void testNotesBrowserShowsOnlyNoteColumns();
    void testReplyThreadsMatchMjPresentation();
    void testReplyThreadOrphansRemainReachable();
    void testDocumentSectionsAndActivation();
    void testLibraryProjectHierarchyDrivesNavigation();
    void testProjectFilteringAndSelection();
    void testDocumentInformationMetricsAndDialog();
    void testLibraryDocumentComponentsAndLocations();
    void testProjectDocumentCommentNavigation();
    void testDocumentInformationRejectsInvalidMetrics();
    void testLibraryBuildWarnings();
    void testUncheckedDocumentIsNoBuildTargetAndHidden();
    void testSyncControlsAndFedoraProjectionOverlay();
    void testSyncFailureKinds();
    void testPendingLibraryRemovalProjectsImmediately();
    void testFedoraDetailsDialog();
    void testServerDialogSavesAddress();
    void testServerDialogAsksForTheTokenOfAnotherServer();
    void testDocumentSyncPresentationAggregation();
    void testDocumentSyncActivityPresentation();
    void testSyncOutcomeSummary();
    void testSourceConflictReportsMergeNeeded();
    void testMergeNeededOffersOnlyShowMerge();
    void testSyncOutcomeUsesFinalBuildSnapshot();
    void testCommentIssueFilter();
    void testCommentsOnPassagesAreListed();
    void testAnchorRemovedCommentsAreIssuesOnly();
    void testCommentMissingMarkersAreIssuesOnly();
    void testRemovedProposalsAreIssuesOnly();
    void testMixedSelectionDecidesItsProposals();
    void testMixedArtifactSelectionDecidesOneBundle();
    void testBatchDecidesMoveAcrossSources();
    void testPlainTextMarkupIsNotAComment();
    void testPriorityLocationLoadingEditingAndFiltering();
    void testAtomicMetadataUpdatePreservesUnknownFields();
    void testChatGptUrlValidationAndDatePreservingUpdate();
    void testOrphanArchive();
    void testWatcherDoesNotSelfTrigger();
    void testFilteringActivationAndRefresh();
    void testRevealCommentClearsFiltersAndSelectsExactRecord();
    void testShiftSelectsCommentRange();
    void testDoubleClickNavigationWaitsForRelease();
    void testRowRevealPreservesHorizontalPosition();
    void testReloadPreservesScrollPosition();
    void testStartupCombinesLibraryLoads();
    void testSourceEditReloadsOnlyItsDocuments();
    void testCatalogueChangeDuringAFullReloadOfIt();
    void testSourceEditRebuildsLibraryTreeOnce();
    void testLibraryChangeBurstRebuildsTreeOnce();
    void testLibraryMarksFailedDocuments();
    void testLibraryBuildWarningsKeepRebuildCost();
    void testThoughtIgnoreDecidesSourceRescans();
    void testReloadBatchesLibraryRefresh();
    void testSummaryRefreshIsDeferredAndBatched();
    void testSyncKeepsContentStable_data();
    void testSyncKeepsContentStable();
    void testSyncProgressPreservesNavigationModels();
    void testLargePlacementRefresh();
    void testAutosavePreservesEditorState();
    void testAutosaveNeverBlocksUi();
    void testCommentDetailsAndActions();
    void testReplyActions();
    void testOpenCodexAction();
    void testOpenSourceAction();
    void testCommentCodexLauncher();
    void testCommentCodexBypassIsOffUnlessTurnedOn();
    void testCommentCodexTerminalScriptCapturesSession();
    void testDeleteUndoRedoRestoresSelection();
    void testDeleteKeepsUnrelatedRows();
    void testHistoryInvalidationIsPerDocument();
    void testLibraryAgentConversations();

public Q_SLOTS:
    // QDesktopServices handler: records the conversation links the Library opens.
    void conversationUrlHandler(const QUrl &url)
    {
        m_openedUrls.append(url);
    }

private:
    QList<QUrl> m_openedUrls;
};

void CommentsOverviewTest::testStandaloneNoteWithoutDocuments()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    const QJsonObject note {{QStringLiteral("schema_version"), 1},
                            {QStringLiteral("comment_id"), QStringLiteral("note-one")},
                            {QStringLiteral("thread_root_id"), QStringLiteral("note-one")},
                            {QStringLiteral("revision"), 1},
                            {QStringLiteral("contents"), QStringLiteral("An independent idea.")},
                            {QStringLiteral("subtype"), QStringLiteral("Note")},
                            {QStringLiteral("origin"), QJsonValue::Null},
                            {QStringLiteral("project"), QJsonValue::Null},
                            {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("standalone")}}}};
    QJsonObject projections {{QStringLiteral("logical_comment\ncomments\nnote-one"), note}};
    const auto documents = Thoughts::loadTrackedCommentDocuments(root.path(), projections);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.first().displayName, QStringLiteral("No project"));
    const auto comment = documents.first().comments.first();
    QVERIFY(comment.documentStem.isEmpty());
    QVERIFY(comment.pdfPath.isEmpty());
    QCOMPARE(comment.page, 0);
    QCOMPARE(comment.anchorStatus, QStringLiteral("standalone"));
    Thoughts::CommentsOverview overview(root.path(), nullptr, {}, {}, [&] { return projections; });
    overview.setFedoraProjections(projections);
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("notesOverviewTable"));
    QVERIFY(table);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 5).data().toString(), QStringLiteral("Note"));
    auto *filter = overview.findChild<QComboBox *>(QStringLiteral("notesOverviewTypeFilter"));
    QVERIFY(filter);
    QVERIFY(filter->isHidden());
    QCOMPARE(filter->findData(QStringLiteral("note")), -1);
    // Notes have no source, Codex task or ChatGPT chat to open.
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    for (const QString &name : {QStringLiteral("commentsOverviewOpenCodex"), QStringLiteral("commentsOverviewChatGptUrl"),
                                QStringLiteral("commentsOverviewOpenChatGpt"), QStringLiteral("commentsOverviewOpenSource")}) {
        auto *field = overview.findChild<QWidget *>(name);
        QVERIFY2(field, qPrintable(name));
        QTRY_VERIFY2(!field->isVisible(), qPrintable(name));
    }
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    QCOMPARE(overview.m_table->model()->rowCount(), 0);
    QCOMPARE(table->model()->rowCount(), 1);
}

void CommentsOverviewTest::testNoteAutosaveRejectsStaleRefresh()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject note {{QStringLiteral("schema_version"), 1},
        {QStringLiteral("comment_id"), QStringLiteral("note-race")},
        {QStringLiteral("thread_root_id"), QStringLiteral("note-race")},
        {QStringLiteral("revision"), 2}, {QStringLiteral("contents"), QStringLiteral("Old text")},
        {QStringLiteral("subtype"), QStringLiteral("Note")}, {QStringLiteral("origin"), QJsonValue::Null},
        {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("standalone")}}}};
    const QString key = QStringLiteral("logical_comment\ncomments\nnote-race");
    Thoughts::CommentsOverview overview(root.path());
    overview.setFedoraProjections({{key, note}});
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    overview.show();
    QTRY_COMPARE(overview.m_table->model()->rowCount(), 1);
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    overview.selectComment(QString(), QStringLiteral("note-race"));
    QVERIFY(overview.m_currentComment.isValid());
    const auto staleDocuments = overview.m_documents;
    const auto staleGeneration = overview.m_commentGeneration;
    QSignalSpy mutations(&overview, &Thoughts::CommentsOverview::logicalCommentMutationRequested);
    QVERIFY(overview.m_noteDateFields->isVisible());
    overview.m_noteDueDate->setText(QStringLiteral("2026-10-02"));
    overview.m_commentEditor->setPlainText(QStringLiteral("Latest complete note"));
    overview.saveSelectedComment();
    QCOMPARE(mutations.size(), 1);
    const auto saved = QJsonObject::fromVariantMap(mutations.first().first().toMap());
    QCOMPARE(saved.value(QStringLiteral("revision")).toInt(), 3);
    QCOMPARE(saved.value(QStringLiteral("calendar_date")).toString(), QStringLiteral("2026-10-02"));
    QVERIFY(saved.value(QStringLiteral("project")).isNull());
    QVERIFY(!overview.m_editorDirty);
    const auto cursor = overview.m_commentEditor->textCursor().position();
    overview.finishFullReload(staleDocuments, true, staleGeneration);
    QCOMPARE(overview.m_commentEditor->toPlainText(), QStringLiteral("Latest complete note"));
    QCOMPARE(overview.m_commentEditor->textCursor().position(), cursor);
    QCOMPARE(overview.m_documents.first().comments.first().logicalPayload.value(QStringLiteral("revision")).toInt(), 3);
    overview.m_reloadTimer->stop();
    overview.m_fullReloadScheduled = false;
    auto echo = saved;
    echo.insert(QStringLiteral("content_updated_at"), saved.value(QStringLiteral("updated_at")));
    overview.setFedoraProjections({{key, echo}});
    QVERIFY(!overview.m_fullReloadRunning);
    QVERIFY(!overview.m_fullReloadScheduled);
    overview.m_commentEditor->setPlainText(QStringLiteral("Next edit"));
    overview.saveSelectedComment();
    QCOMPARE(QJsonObject::fromVariantMap(mutations.last().first().toMap()).value(QStringLiteral("revision")).toInt(), 4);
    // Historical undo content must still extend the latest revision.
    const auto undo = overview.logicalMutationPayload(note, overview.currentEditorState(), false);
    QCOMPARE(undo.value(QStringLiteral("revision")).toInt(), 5);
    overview.m_noteDueDate->clear();
    overview.saveSelectedComment();
    const auto cleared = QJsonObject::fromVariantMap(mutations.last().first().toMap());
    QVERIFY(cleared.value(QStringLiteral("calendar_date")).toString().isEmpty());
    QVERIFY(cleared.value(QStringLiteral("calendar_time")).toString().isEmpty());
    overview.m_noteDueDate->setText(QStringLiteral("not a date"));
    QString error;
    overview.currentMetadataPatch(&error);
    QVERIFY(!error.isEmpty());
    overview.m_noteDueDate->clear();
}

void CommentsOverviewTest::testNoteUndoPreservesCalendar()
{
    QTemporaryDir root;
    Thoughts::CommentsOverview overview(root.path());
    Thoughts::CommentsOverview::HistoryEntry entry;
    entry.operation = Thoughts::CommentsOverview::DeleteOperation;
    entry.logicalPayload = QJsonObject{{QStringLiteral("schema_version"), 1},
        {QStringLiteral("comment_id"), QStringLiteral("dated-note")},
        {QStringLiteral("subtype"), QStringLiteral("Note")},
        {QStringLiteral("revision"), 2},
        {QStringLiteral("calendar_date"), QStringLiteral("2026-10-02")},
        {QStringLiteral("calendar_time"), QStringLiteral("09:30")},
        {QStringLiteral("calendar_zone_id"), QStringLiteral("Europe/Paris")}};
    overview.m_undoHistory.append(entry);
    QSignalSpy mutations(&overview, &Thoughts::CommentsOverview::logicalCommentMutationRequested);
    overview.undoOverviewChange();
    QCOMPARE(mutations.size(), 1);
    const auto restored = QJsonObject::fromVariantMap(mutations.first().first().toMap());
    for (const auto &key : {QStringLiteral("calendar_date"), QStringLiteral("calendar_time"), QStringLiteral("calendar_zone_id")}) {
        QCOMPARE(restored.value(key), entry.logicalPayload.value(key));
    }
}

void CommentsOverviewTest::testStandaloneNoteCreation()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Thoughts::CommentsOverview overview(root.path());
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    overview.show();

    auto *button = overview.findChild<QPushButton *>(QStringLiteral("notesOverviewAddNote"));
    QVERIFY(button);
    QVERIFY(button->isVisible());
    QCOMPARE(button->text(), QStringLiteral("+"));
    QCOMPARE(button->accessibleName(), QStringLiteral("Add To-do"));

    QSignalSpy mutationRequested(&overview, &Thoughts::CommentsOverview::logicalCommentMutationRequested);
    QTimer::singleShot(0, &overview, [] {
        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        auto *editor = dialog->findChild<QPlainTextEdit *>();
        QVERIFY(editor);
        editor->setPlainText(QStringLiteral("A new standalone thought."));
        dialog->accept();
    });
    QTest::mouseClick(button, Qt::LeftButton);

    QCOMPARE(mutationRequested.size(), 1);
    const QVariantList arguments = mutationRequested.constFirst();
    const QVariantMap payload = arguments.at(0).toMap();
    const QString id = payload.value(QStringLiteral("comment_id")).toString();
    QVERIFY(id.startsWith(QStringLiteral("thoughts-")));
    QCOMPARE(payload.value(QStringLiteral("thread_root_id")).toString(), id);
    QVERIFY(payload.value(QStringLiteral("parent_id")).isNull());
    QCOMPARE(payload.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(payload.value(QStringLiteral("contents")).toString(), QStringLiteral("A new standalone thought."));
    QCOMPARE(payload.value(QStringLiteral("subtype")).toString(), QStringLiteral("Note"));
    QCOMPARE(payload.value(QStringLiteral("priority")).toString(), QStringLiteral("normal"));
    QVERIFY(payload.value(QStringLiteral("origin")).isNull());
    QVERIFY(payload.value(QStringLiteral("project")).isNull());
    QVERIFY(!payload.contains(QStringLiteral("linking")));
    QCOMPARE(payload.value(QStringLiteral("scope")).toMap().value(QStringLiteral("state")).toString(), QStringLiteral("standalone"));
    QCOMPARE(arguments.at(1).toInt(), static_cast<int>(Thoughts::CommentsOverview::CreateOperation));
    QVERIFY(arguments.at(2).toString().isEmpty());
    QCOMPARE(arguments.at(3).toString(), id);
}

void CommentsOverviewTest::testStandaloneNoteAttachmentOpen()
{
    QTemporaryDir root;
    const QJsonObject attachment {{QStringLiteral("id"), QStringLiteral("photo-one")},
        {QStringLiteral("name"), QStringLiteral("Picture.jpg")}, {QStringLiteral("mime_type"), QStringLiteral("image/jpeg")},
        {QStringLiteral("object_sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("size_bytes"), 123}};
    const QJsonObject note {{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), QStringLiteral("note-photo")},
        {QStringLiteral("thread_root_id"), QStringLiteral("note-photo")}, {QStringLiteral("revision"), 1},
        {QStringLiteral("contents"), QString()}, {QStringLiteral("subtype"), QStringLiteral("Note")},
        {QStringLiteral("origin"), QJsonValue::Null}, {QStringLiteral("project"), QJsonValue::Null},
        {QStringLiteral("attachments"), QJsonArray {attachment}},
        {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("standalone")}}}};
    const QJsonObject projections {{QStringLiteral("logical_comment\ncomments\nnote-photo"), note}};
    Thoughts::CommentsOverview overview(root.path(), nullptr, {}, {}, [&] { return projections; });
    overview.setFedoraProjections(projections);
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("notesOverviewTable"));
    QVERIFY(table);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    table->setCurrentIndex(table->model()->index(0, 0));
    auto *attachments = overview.findChild<QWidget *>(QStringLiteral("commentsOverviewNoteAttachments"));
    QVERIFY(attachments);
    QTRY_VERIFY(!attachments->isHidden());
    auto buttons = attachments->findChildren<QPushButton *>();
    QCOMPARE(buttons.size(), 1);
    QVERIFY(buttons.first()->text().contains(QStringLiteral("Picture.jpg")));
    QSignalSpy requested(&overview, &Thoughts::CommentsOverview::noteAttachmentOpenRequested);
    buttons.first()->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(QJsonObject::fromVariantMap(requested.first().first().toMap()), attachment);
}

void CommentsOverviewTest::testStandaloneNoteDeleteActions_data()
{
    QTest::addColumn<int>("selectedCount");
    QTest::newRow("single") << 1;
    QTest::newRow("batch") << 2;
}

void CommentsOverviewTest::testStandaloneNoteDeleteActions()
{
    QFETCH(int, selectedCount);
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QJsonObject projections;
    for (int i = 0; i < 3; ++i) {
        const QString id = QStringLiteral("note-%1").arg(i);
        projections.insert(QStringLiteral("logical_comment\ncomments\n") + id,
                           QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), id},
                                        {QStringLiteral("thread_root_id"), id}, {QStringLiteral("revision"), 1},
                                        {QStringLiteral("contents"), QStringLiteral("Independent note %1.").arg(i)},
                                        {QStringLiteral("subtype"), QStringLiteral("Note")}, {QStringLiteral("deleted"), false},
                                        {QStringLiteral("origin"), QJsonValue::Null},
                                        {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("standalone")}}}});
    }
    Thoughts::CommentsOverview overview(root.path(), nullptr, {}, {}, [&] { return projections; });
    overview.setFedoraProjections(projections);
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("notesOverviewTable"));
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("notesOverviewDocuments"));
    auto *button = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDelete"));
    QVERIFY(table);
    QVERIFY(tree);
    QVERIFY(button);
    QTRY_COMPARE(table->model()->rowCount(), 3);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    if (selectedCount == 2) {
        table->selectionModel()->select(table->model()->index(1, 0), QItemSelectionModel::Select | QItemSelectionModel::Rows);
    }
    if (selectedCount == 1) { QVERIFY(button->isEnabled()); }
    const QString survivor = table->model()->index(2, 0).data().toString();
    Thoughts::CommentService service([&] {
        QList<Thoughts::CommentRecord> records;
        for (auto it = projections.begin(); it != projections.end(); ++it) {
            const auto payload = it.value().toObject();
            records.append({payload.value(QStringLiteral("comment_id")).toString(), {}, payload});
        }
        return records;
    }, [&](const auto &changes, QString *) {
        for (const auto &change : changes) projections.insert(QStringLiteral("logical_comment\ncomments\n") + change.after.id, change.after.payload);
        return true;
    }, [] { return QStringLiteral("2026-09-12T14:00:00Z"); }, QStringLiteral("test"));
    QSignalSpy requests(&overview, &Thoughts::CommentsOverview::commentsDeleteRequested);
    connect(&overview, &Thoughts::CommentsOverview::commentsDeleteRequested, &overview, [&](QList<Thoughts::TrackedCommentTarget> targets) {
        QStringList ids;
        for (const auto &target : targets) { QVERIFY(target.pdfPath.isEmpty()); ids.append(target.annotationId); }
        const auto result = service.deleteComments(ids);
        QVERIFY(result.success);
        for (auto &target : targets) target.serviceToken = result.historyToken;
        overview.setFedoraProjections(projections);
        overview.completeBatchDelete(targets, {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    });
    const auto history = [&](const QList<Thoughts::TrackedCommentTarget> &targets, bool undo) {
        const auto result = undo ? service.undo(targets.first().serviceToken) : service.redo(targets.first().serviceToken);
        QVERIFY(result.success);
        overview.setFedoraProjections(projections);
        overview.completeBatchHistoryOperation(undo ? Thoughts::CommentsOverview::UndoOperation : Thoughts::CommentsOverview::RedoOperation, targets, {{QStringLiteral("success"), true}});
    };
    connect(&overview, &Thoughts::CommentsOverview::commentsUndoRequested, &overview, [&](const auto &targets) { history(targets, true); });
    connect(&overview, &Thoughts::CommentsOverview::commentsRedoRequested, &overview, [&](const auto &targets) { history(targets, false); });
    if (selectedCount == 2) QTimer::singleShot(0, &overview, [] {
        auto *dialog = qobject_cast<QMessageBox *>(QApplication::activeModalWidget());
        if (dialog) for (auto *action : dialog->buttons()) if (dialog->buttonRole(action) == QMessageBox::DestructiveRole) { action->click(); return; }
    });
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(requests.size(), 1);
    overview.reload();
    QTRY_COMPARE(table->model()->rowCount(), 3 - selectedCount);
    QCOMPARE(table->model()->index(table->model()->rowCount() - 1, 0).data().toString(), survivor);
    overview.undoOverviewChange(); overview.reload();
    QTRY_COMPARE(table->model()->rowCount(), 3);
    overview.redoOverviewChange(); overview.reload();
    QTRY_COMPARE(table->model()->rowCount(), 3 - selectedCount);

}

void CommentsOverviewTest::testReviewCardsRefreshWhenSourceLocationsChange()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Thoughts::CommentsOverview overview(root.path());
    QSignalSpy refreshed(overview.m_reviewCardsWidget, &Thoughts::ReviewCardsWidget::countsChanged);
    QTRY_VERIFY(!refreshed.isEmpty());
    refreshed.clear();

    const QJsonObject sourceLocations {{QStringLiteral("schema_version"), 1},
        {QStringLiteral("projects"), QJsonObject{{QStringLiteral("docs-concepts"), true}}},
        {QStringLiteral("files"), QJsonObject{}}};
    Thoughts::installSourceLocationIndex(root.path(), sourceLocations, {});
    overview.setFedoraProjections({});
    QTRY_VERIFY(!refreshed.isEmpty());
    QCOMPARE(overview.m_reviewSourceLocationIndex, sourceLocations);
}

// A Sync that starts or ends changes the Sync state of every document at once.
// The Library tree then changes its rows' tooltips and icons where they are; it
// is not rebuilt, which kept the window from answering.
void CommentsOverviewTest::testSyncStateChangeUpdatesDocumentRowsInPlace()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral(".runtime")));
    Thoughts::CommentsOverview overview(root.path());
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    overview.m_catalogPollTimer->stop();
    constexpr int documentCount = 150;
    constexpr int segmentCount = 12;
    QList<Thoughts::TrackedCommentDocument> documents;
    for (int index = 0; index < documentCount; ++index) {
        Thoughts::TrackedCommentDocument document;
        document.stem = QStringLiteral("paper-%1").arg(index);
        document.displayName = QStringLiteral("Paper %1").arg(index);
        document.libraryFilename = document.stem + QStringLiteral(".pdf");
        document.pdfPath = QDir(root.path()).filePath(QStringLiteral("current_pdfs/") + document.libraryFilename);
        const bool sourceLinked = index % 3 != 0;
        document.state = sourceLinked ? Thoughts::TrackedCommentDocument::SourceLinked : Thoughts::TrackedCommentDocument::PdfOnly;
        document.libraryKind = sourceLinked ? QStringLiteral("source_linked") : QStringLiteral("pdf_only");
        document.libraryGenerationCurrent = true;
        document.navigationProjectId = QStringLiteral("project-%1").arg(index % 10);
        document.navigationProjectName = QStringLiteral("Project %1").arg(index % 10);
        for (int segment = 0; sourceLinked && segment < segmentCount; ++segment) {
            document.librarySegments.append(QJsonObject {{QStringLiteral("source_id"), QStringLiteral("docs-sam/tex/source/%1/part-%2.tex").arg(document.stem).arg(segment)},
                                                         {QStringLiteral("title"), QStringLiteral("Part %1").arg(segment)},
                                                         {QStringLiteral("start_page"), 1 + segment * 4},
                                                         {QStringLiteral("page_count"), 4}});
        }
        for (int thread = 0; thread < 20; ++thread) {
            Thoughts::TrackedComment comment;
            comment.id = comment.threadRootId = QStringLiteral("%1-thread-%2").arg(document.stem).arg(thread);
            comment.documentStem = document.stem;
            comment.pdfPath = document.pdfPath;
            comment.contents = QStringLiteral("Comment %1").arg(thread);
            comment.logicalComment = true;
            comment.page = 1 + thread * 2;
            document.comments.append(comment);
        }
        documents.append(document);
    }
    overview.m_documents = documents;
    QElapsedTimer timer;
    timer.start();
    overview.populateDocumentList(QString());
    const qint64 firstBuildMs = timer.restart();
    overview.populateDocumentList(QString());
    const qint64 rebuildMs = timer.elapsed();

    QTreeWidget *tree = overview.m_commentsBrowser.documentList;
    QTreeWidget *otherTree = overview.m_notesBrowser.documentList;
    const auto rowOf = [](QTreeWidget *widget, const QString &stem) -> QTreeWidgetItem * {
        for (QTreeWidgetItemIterator it(widget); *it; ++it) {
            // A document's own row, not one of its components.
            if ((*it)->data(0, Qt::UserRole).toString() == stem && (*it)->parent() && !(*it)->parent()->parent()) {
                return *it;
            }
        }
        return nullptr;
    };
    const auto snapshotOf = [](QTreeWidget *widget) {
        QStringList rows;
        for (QTreeWidgetItemIterator it(widget); *it; ++it) {
            rows.append(QStringLiteral("%1|%2|%3|%4|%5").arg((*it)->text(0), (*it)->text(1), (*it)->toolTip(0), (*it)->toolTip(1), (*it)->icon(0).isNull() ? QStringLiteral("-") : QStringLiteral("icon")));
        }
        return rows;
    };
    int rows = 0;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) {
        ++rows;
    }
    qInfo("Library tree of %d documents (%d rows): first build %lld ms, rebuild %lld ms", documentCount, rows, firstBuildMs, rebuildMs);
    QVERIFY(rows > documentCount * 8);
    QTreeWidgetItem *linked = rowOf(tree, QStringLiteral("paper-1"));
    QTreeWidgetItem *pdfOnly = rowOf(tree, QStringLiteral("paper-3"));
    QVERIFY(linked);
    QVERIFY(pdfOnly);
    QCOMPARE(linked->childCount(), segmentCount);
    QVERIFY(linked->toolTip(0).contains(QStringLiteral("Sync: Not checked")));
    QCOMPARE(linked->text(1), QStringLiteral("20"));
    QCOMPARE(linked->toolTip(1), QStringLiteral("20 comment threads"));
    QCOMPARE(linked->child(0)->toolTip(1), QStringLiteral("2 comment threads"));
    QCOMPARE(linked->child(segmentCount - 1)->toolTip(1), QStringLiteral("0 comment threads"));

    // A Sync starts: every document changes state.
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy otherResets(otherTree->model(), &QAbstractItemModel::modelReset);
    QJsonObject syncing;
    for (const Thoughts::TrackedCommentDocument &document : std::as_const(documents)) {
        syncing.insert(document.libraryFilename, QJsonObject {{QStringLiteral("status"), QStringLiteral("Syncing…")}, {QStringLiteral("state"), QStringLiteral("syncing")}});
    }
    const QJsonObject clean {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}};
    timer.restart();
    overview.setSyncSnapshot(syncing, {}, clean, {});
    const qint64 changeMs = timer.elapsed();
    qInfo("Sync state of %d documents changed: %lld ms", documentCount, changeMs);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(otherResets.count(), 0);
    QCOMPARE(rowOf(tree, QStringLiteral("paper-1")), linked);
    QVERIFY(linked->toolTip(0).contains(QStringLiteral("Sync: Syncing…")));
    QVERIFY(pdfOnly->toolTip(0).contains(QStringLiteral("Sync: Syncing…")));
    QVERIFY(pdfOnly->toolTip(0).contains(QStringLiteral("Source layout: not linked")));
    QVERIFY(rowOf(otherTree, QStringLiteral("paper-1"))->toolTip(0).contains(QStringLiteral("Sync: Syncing…")));

    // One document fails; the others are up to date. Pending changes alone change no row.
    QJsonObject ended;
    for (const Thoughts::TrackedCommentDocument &document : std::as_const(documents)) {
        ended.insert(document.libraryFilename, QJsonObject {{QStringLiteral("status"), QStringLiteral("Up to date")}, {QStringLiteral("state"), QStringLiteral("current")}});
    }
    ended.insert(QStringLiteral("paper-1.pdf"), QJsonObject {{QStringLiteral("status"), QStringLiteral("PDF build failed")}, {QStringLiteral("state"), QStringLiteral("error")},
                                                             {QStringLiteral("reason"), QStringLiteral("An undefined control sequence in part-2.tex.")}});
    overview.setSyncSnapshot(ended, {}, clean, {});
    QVERIFY(linked->toolTip(0).contains(QStringLiteral("Sync: PDF build failed\nAn undefined control sequence in part-2.tex.")));
    QVERIFY(pdfOnly->toolTip(0).contains(QStringLiteral("Sync: Up to date")));
    const QStringList afterChange = snapshotOf(tree);
    const QStringList otherAfterChange = snapshotOf(otherTree);
    overview.setSyncSnapshot(ended, {}, QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 3}}, {});
    QCOMPARE(snapshotOf(tree), afterChange);
    QCOMPARE(resets.count(), 0);
    QCOMPARE(otherResets.count(), 0);

    // A rebuild shows exactly what the rows were changed to.
    overview.populateDocumentList(QString());
    QCOMPARE(resets.count(), 1);
    QCOMPARE(snapshotOf(tree), afterChange);
    QCOMPARE(snapshotOf(otherTree), otherAfterChange);
    linked = rowOf(tree, QStringLiteral("paper-1"));
    QVERIFY(linked);

    // While a Sync runs the tree waits, as before.
    overview.syncStarted();
    overview.setSyncSnapshot(syncing, {}, clean, {});
    QVERIFY(linked->toolTip(0).contains(QStringLiteral("Sync: PDF build failed")));
    QCOMPARE(resets.count(), 1);
    // Generous guards; the reported times are the stricter local check.
    QVERIFY2(changeMs < 500, "A change of the documents' Sync state must not rebuild the Library tree");
    QVERIFY2(rebuildMs < 3000, "Rebuilding the Library tree took too long");
}

void CommentsOverviewTest::testPersistentBrowsersKeepState()
{
    QTemporaryDir root;
    Thoughts::CommentsOverview overview(root.path());
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    overview.m_catalogPollTimer->stop();
    Thoughts::TrackedCommentDocument document;
    document.displayName = QStringLiteral("No project");
    for (int i = 0; i < 200; ++i) {
        Thoughts::TrackedComment comment;
        comment.id = comment.threadRootId = QString::number(i);
        comment.contents = QStringLiteral("Entry %1").arg(i);
        comment.author = QStringLiteral("Alex");
        comment.logicalComment = true;
        if (i % 2) { comment.logicalPayload = {{QStringLiteral("subtype"), QStringLiteral("Note")}}; }
        document.comments.append(comment);
    }
    auto reply = document.comments.first();
    reply.id = QStringLiteral("reply");
    reply.parentId = reply.threadRootId;
    reply.reply = true;
    reply.contents = QStringLiteral("Thread reply");
    document.comments.append(reply);
    overview.m_documents = {document};
    overview.populateDocumentList(QString());
    overview.populateComments();
    overview.applyFilters();
    auto &comments = overview.m_commentsBrowser;
    auto &notes = overview.m_notesBrowser;
    QVERIFY(comments.table != notes.table);
    QCOMPARE(comments.proxy->sourceModel(), notes.proxy->sourceModel());
    QCOMPARE(comments.proxy->rowCount(), 100);
    QCOMPARE(notes.proxy->rowCount(), 100);
    QVERIFY(comments.table->uniformRowHeights());
    QVERIFY(notes.table->uniformRowHeights());
    // Each tab keeps its own search, project scope, selection and expansion.
    comments.search->setText(QStringLiteral("Entry"));
    auto current = comments.proxy->index(0, 0);
    comments.table->setCurrentIndex(current);
    comments.table->expand(current);
    QPersistentModelIndex persistentCurrent(current);
    comments.table->verticalScrollBar()->setValue(20);
    const int scroll = comments.table->verticalScrollBar()->value();
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    notes.search->setText(QStringLiteral("Entry 1"));
    notes.documentList->setCurrentItem(notes.documentList->topLevelItem(1)->child(0));
    notes.table->setCurrentIndex(notes.proxy->index(0, 0));
    QPersistentModelIndex noteCurrent(notes.table->currentIndex());
    const int noteCount = notes.proxy->rowCount();
    const QString screenshotDirectory = qEnvironmentVariable("THOUGHTS_TAB_SCREENSHOTS");
    if (!screenshotDirectory.isEmpty()) { QVERIFY(overview.grab().save(screenshotDirectory + QStringLiteral("/notes.png"))); }
    QSignalSpy libraryResets(overview.m_libraryTree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy commentRemovals(comments.proxy, &QAbstractItemModel::rowsRemoved);
    QSignalSpy noteRemovals(notes.proxy, &QAbstractItemModel::rowsRemoved);
    QSignalSpy commentInsertions(comments.proxy, &QAbstractItemModel::rowsInserted);
    QSignalSpy noteInsertions(notes.proxy, &QAbstractItemModel::rowsInserted);
    for (int i = 0; i < 10; ++i) {
        overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
        if (i == 0 && !screenshotDirectory.isEmpty()) { QVERIFY(overview.grab().save(screenshotDirectory + QStringLiteral("/comments.png"))); }
        QVERIFY(!overview.m_details->isHidden());
        QVERIFY(overview.m_details->width() > 0);
        QCOMPARE(comments.search->text(), QStringLiteral("Entry"));
        QCOMPARE(comments.table->currentIndex(), QModelIndex(persistentCurrent));
        QVERIFY(comments.table->isExpanded(persistentCurrent));
        QCOMPARE(comments.table->verticalScrollBar()->value(), scroll);
        overview.m_tabs->setCurrentWidget(overview.m_notesPage);
        QCOMPARE(notes.search->text(), QStringLiteral("Entry 1"));
        QCOMPARE(notes.table->currentIndex(), QModelIndex(noteCurrent));
        QCOMPARE(notes.proxy->rowCount(), noteCount);
    }
    QCOMPARE(libraryResets.size(), 0);
    QCOMPARE(commentRemovals.size(), 0);
    QCOMPARE(noteRemovals.size(), 0);
    QCOMPARE(commentInsertions.size(), 0);
    QCOMPARE(noteInsertions.size(), 0);
    // An incoming edit changes cells without removing either tab's stable rows.
    QSignalSpy changes(overview.m_model, &QAbstractItemModel::dataChanged);
    overview.m_documents[0].comments[0].contents = QStringLiteral("Entry updated remotely");
    overview.populateComments();
    overview.updateCommentSummary();
    QVERIFY(changes.size() > 0);
    QVERIFY(changes.size() <= overview.m_model->columnCount());
    QCOMPARE(commentRemovals.size(), 0);
    QCOMPARE(noteRemovals.size(), 0);
    QVERIFY(persistentCurrent.isValid());
    QVERIFY(noteCurrent.isValid());
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    QCOMPARE(overview.m_commentEditor->toPlainText(), QStringLiteral("Entry updated remotely"));
    QCOMPARE(overview.m_table->currentIndex(), QModelIndex(persistentCurrent));
    // A later authoritative snapshot can revert an optimistic local row edit.
    overview.m_changingSelection = true;
    overview.m_commentEditor->setPlainText(QStringLiteral("Local optimistic text"));
    overview.m_changingSelection = false;
    overview.applyEditorToCurrentRow();
    overview.m_autoSaveTimer->stop();
    overview.m_editorDirty = false;
    overview.m_documents[0].comments[0].contents = QStringLiteral("Entry updated remotely");
    overview.populateComments();
    overview.loadComment(persistentCurrent);
    QCOMPARE(overview.m_commentEditor->toPlainText(), QStringLiteral("Entry updated remotely"));
    // Dirty edits cannot be discarded by switching while persistence is pending.
    overview.setOperationPending(true);
    overview.m_commentEditor->setPlainText(QStringLiteral("Unsaved local draft"));
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    QCOMPARE(overview.m_tabs->currentWidget(), overview.m_commentsPage);
    QCOMPARE(overview.m_commentEditor->toPlainText(), QStringLiteral("Unsaved local draft"));
    overview.m_editorDirty = false;
    overview.m_autoSaveTimer->stop();
    overview.setOperationPending(false);
}

void CommentsOverviewTest::testTabSwitchScales_data()
{
    QTest::addColumn<int>("records");
    QTest::newRow("1000") << 1000;
    QTest::newRow("10000") << 10000;
    if (qEnvironmentVariableIsSet("THOUGHTS_LARGE_TAB_BENCHMARK")) { QTest::newRow("100000") << 100000; }
}

void CommentsOverviewTest::testTabSwitchScales()
{
    QFETCH(int, records);
    QTemporaryDir root;
    // The first reload creates .runtime/ (proposal history index); where only the Library folder
    // itself is watched (Linux), that change could start a second reload during the timing.
    QVERIFY(QDir(root.path()).mkpath(QStringLiteral(".runtime")));
    Thoughts::CommentsOverview overview(root.path());
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    overview.m_catalogPollTimer->stop();
    Thoughts::TrackedCommentDocument document;
    document.displayName = QStringLiteral("No project");
    for (int i = 0; i < records; ++i) {
        Thoughts::TrackedComment comment;
        comment.id = comment.threadRootId = QString::number(i);
        comment.contents = QStringLiteral("A realistic single-line comment or note %1.").arg(i);
        comment.author = QStringLiteral("Alex");
        comment.logicalComment = true;
        if (i % 2) { comment.logicalPayload = {{QStringLiteral("subtype"), QStringLiteral("Note")}}; }
        document.comments.append(comment);
    }
    overview.m_documents = {document};
    overview.populateDocumentList(QString());
    overview.populateComments();
    overview.applyFilters();
    QCOMPARE(overview.m_commentsBrowser.proxy->rowCount(), records / 2);
    QCOMPARE(overview.m_notesBrowser.proxy->rowCount(), records / 2);
    // Warm both views, then include event processing and painting in the timing.
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    QCoreApplication::processEvents();
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    QCoreApplication::processEvents();
    QSignalSpy resets(overview.m_libraryTree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy removed(overview.m_commentsBrowser.proxy, &QAbstractItemModel::rowsRemoved);
    QSignalSpy inserted(overview.m_commentsBrowser.proxy, &QAbstractItemModel::rowsInserted);
    qint64 worst = 0;
    QElapsedTimer total;
    total.start();
    for (int i = 0; i < 20; ++i) {
        QElapsedTimer timer;
        timer.start();
        overview.m_tabs->setCurrentWidget(i % 2 ? overview.m_commentsPage : overview.m_notesPage);
        QCoreApplication::processEvents();
        worst = qMax(worst, timer.nsecsElapsed());
    }
    qInfo("%d records: warm tab switch mean %.2f ms, worst %.2f ms", records, total.nsecsElapsed() / 20.0e6, worst / 1.0e6);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(removed.size(), 0);
    QCOMPARE(inserted.size(), 0);
    // Generous CI guard; reported measurements provide the stricter local check.
    QVERIFY2(worst < 500000000, "A warm tab switch blocked the event loop for over 500 ms");
}

void CommentsOverviewTest::testOverviewTabs()
{
    QTemporaryDir root;
    Thoughts::CommentsOverview overview(root.path());
    QCOMPARE(overview.m_tabs->count(), 5);
    QCOMPARE(overview.m_tabs->tabText(0), QStringLiteral("Projects"));
    QCOMPARE(overview.m_tabs->tabText(1), QStringLiteral("To-dos"));
    QCOMPARE(overview.m_tabs->tabText(2), QStringLiteral("Diary"));
    QCOMPARE(overview.m_tabs->tabText(3), QStringLiteral("Annotations"));
    QCOMPARE(overview.m_tabs->tabText(4), QStringLiteral("Review"));
    QCOMPARE(overview.m_typeFilter->itemText(4), QStringLiteral("Comments"));
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    QVERIFY(overview.m_notesPage->isAncestorOf(overview.m_table));
    QVERIFY(overview.m_typeFilter->isHidden());
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    QVERIFY(overview.m_commentsPage->isAncestorOf(overview.m_table));
    QVERIFY(!overview.m_typeFilter->isHidden());
    overview.setOperationPending(true);
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    QCOMPARE(overview.m_tabs->currentWidget(), overview.m_commentsPage);
    overview.setOperationPending(false);
}

void CommentsOverviewTest::testTodosHiddenUntilTheirDate()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Thoughts::CommentsOverview overview(root.path());
    Thoughts::TrackedCommentDocument unassigned;
    unassigned.displayName = QStringLiteral("No project");
    const auto todo = [&unassigned](const QString &id, const QString &text, const QString &visibleFrom) {
        Thoughts::TrackedComment result;
        result.id = result.threadRootId = id;
        result.contents = text;
        result.logicalComment = true;
        result.logicalPayload = {{QStringLiteral("subtype"), QStringLiteral("Note")}, {QStringLiteral("visible_from"), visibleFrom}};
        return result;
    };
    unassigned.comments = {todo(QStringLiteral("now"), QStringLiteral("Call the podologist\nthe one in Giussano"), QString()),
                           todo(QStringLiteral("past"), QStringLiteral("Already due"), QDate::currentDate().toString(Qt::ISODate)),
                           todo(QStringLiteral("later"), QStringLiteral("Book flights"), QDate::currentDate().addDays(3).toString(Qt::ISODate))};
    overview.m_documents = {unassigned};
    overview.populateDocumentList(QString());
    overview.populateComments();
    overview.applyFilters();
    auto &notes = overview.m_notesBrowser;
    // Rows show the to-do's first line only, so each row stays one line tall.
    const auto shown = [&notes] {
        QStringList texts;
        for (int row = 0; row < notes.proxy->rowCount(); ++row) texts.append(notes.proxy->index(row, 0).data().toString());
        texts.sort();
        return texts;
    };
    QCOMPARE(shown(), (QStringList{QStringLiteral("Already due"), QStringLiteral("Call the podologist …")}));
    QVERIFY2(notes.summary->text().startsWith(QStringLiteral("2 to-dos · 1 hidden until later")), qPrintable(notes.summary->text()));
    auto *showHidden = overview.findChild<QCheckBox *>(QStringLiteral("notesOverviewShowHidden"));
    QVERIFY(showHidden);
    showHidden->setChecked(true);
    const QStringList all = shown();
    QCOMPARE(all.size(), 3);
    QVERIFY2(all.first().startsWith(QStringLiteral("Already due")) && all.at(1).startsWith(QStringLiteral("Book flights  ·  hidden until ")), qPrintable(all.join(QLatin1Char('|'))));
    showHidden->setChecked(false);
    QCOMPARE(notes.proxy->rowCount(), 2);
}

void CommentsOverviewTest::testDiaryEntriesListedInDiaryNotTodos()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Thoughts::CommentsOverview overview(root.path());
    Thoughts::TrackedCommentDocument unassigned;
    unassigned.displayName = QStringLiteral("No project");
    const auto note = [](const QString &id, const QString &text, const QString &diaryAt) {
        Thoughts::TrackedComment result;
        result.id = result.threadRootId = id;
        result.contents = text;
        result.logicalComment = true;
        result.logicalPayload = {{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), id}, {QStringLiteral("revision"), 1},
                                 {QStringLiteral("subtype"), QStringLiteral("Note")}, {QStringLiteral("contents"), text}};
        if (!diaryAt.isEmpty()) {
            result.logicalPayload.insert(QStringLiteral("entry_kind"), QStringLiteral("diary"));
            result.logicalPayload.insert(QStringLiteral("diary_at"), diaryAt);
            result.logicalPayload.insert(QStringLiteral("diary_zone_id"), QStringLiteral("Europe/Zurich"));
            result.logicalPayload.insert(QStringLiteral("phone_only_key"), QStringLiteral("kept"));
        }
        return result;
    };
    const QString today = QDate::currentDate().toString(Qt::ISODate);
    const QString yesterday = QDate::currentDate().addDays(-1).toString(Qt::ISODate);
    unassigned.comments = {note(QStringLiteral("todo"), QStringLiteral("Buy milk"), QString()),
                           note(QStringLiteral("morning"), QStringLiteral("Coffee by the lake"), today + QStringLiteral("T08:15:00+02:00")),
                           note(QStringLiteral("afternoon"), QStringLiteral("Wrote the diary tab\nand tested it"), today + QStringLiteral("T14:03:27+02:00")),
                           note(QStringLiteral("before"), QStringLiteral("Long walk"), yesterday + QStringLiteral("T21:40:00+02:00"))};
    overview.m_documents = {unassigned};
    overview.populateDocumentList(QString());
    overview.populateComments();
    overview.applyFilters();

    // To-dos lists and counts the to-do only.
    auto &notes = overview.m_notesBrowser;
    QCOMPARE(notes.proxy->rowCount(), 1);
    QCOMPARE(notes.proxy->index(0, 0).data().toString(), QStringLiteral("Buy milk"));
    QVERIFY2(notes.summary->text().startsWith(QStringLiteral("1 to-do")), qPrintable(notes.summary->text()));
    QCOMPARE(overview.m_commentsBrowser.proxy->rowCount(), 0);

    // Diary: newest first, under day headings, each entry with its time.
    QCOMPARE(overview.m_tabs->tabText(overview.m_tabs->indexOf(overview.m_diaryPage)), QStringLiteral("Diary"));
    auto *list = overview.findChild<QTreeWidget *>(QStringLiteral("diaryOverviewEntries"));
    QVERIFY(list);
    QCOMPARE(list->topLevelItemCount(), 2);
    QTreeWidgetItem *todayItem = list->topLevelItem(0);
    QCOMPARE(todayItem->text(0), QStringLiteral("Today"));
    QCOMPARE(todayItem->childCount(), 2);
    QCOMPARE(todayItem->child(0)->text(0), QStringLiteral("14:03"));
    QCOMPARE(todayItem->child(0)->text(1), QStringLiteral("Wrote the diary tab\nand tested it"));
    QCOMPARE(todayItem->child(1)->text(0), QStringLiteral("08:15"));
    QCOMPARE(list->topLevelItem(1)->text(0), QStringLiteral("Yesterday"));
    QCOMPARE(list->topLevelItem(1)->child(0)->text(1), QStringLiteral("Long walk"));
    QCOMPARE(overview.m_diarySummary->text(), QStringLiteral("3 diary entries"));

    // Enter in the field saves a new entry; Shift+Enter starts a new line.
    overview.m_tabs->setCurrentWidget(overview.m_diaryPage);
    overview.show();
    overview.activateWindow();
    QApplication::setActiveWindow(&overview);
    auto *input = overview.findChild<QPlainTextEdit *>(QStringLiteral("diaryOverviewInput"));
    QVERIFY(input);
    QSignalSpy mutations(&overview, &Thoughts::CommentsOverview::logicalCommentMutationRequested);
    input->setFocus();
    QTest::keyClicks(input, QStringLiteral("First line"));
    QTest::keyClick(input, Qt::Key_Return, Qt::ShiftModifier);
    QTest::keyClicks(input, QStringLiteral("second line"));
    QCOMPARE(mutations.size(), 0);
    const QDateTime before = QDateTime::currentDateTime().addSecs(-1);
    QTest::keyClick(input, Qt::Key_Return);
    QCOMPARE(mutations.size(), 1);
    const QVariantMap created = mutations.constFirst().at(0).toMap();
    QCOMPARE(mutations.constFirst().at(1).toInt(), static_cast<int>(Thoughts::CommentsOverview::CreateOperation));
    QCOMPARE(created.value(QStringLiteral("contents")).toString(), QStringLiteral("First line\nsecond line"));
    QCOMPARE(created.value(QStringLiteral("subtype")).toString(), QStringLiteral("Note"));
    QCOMPARE(created.value(QStringLiteral("scope")).toMap().value(QStringLiteral("state")).toString(), QStringLiteral("standalone"));
    QCOMPARE(created.value(QStringLiteral("entry_kind")).toString(), QStringLiteral("diary"));
    const QString diaryAt = created.value(QStringLiteral("diary_at")).toString();
    QVERIFY2(QRegularExpression(QStringLiteral("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\d[+-]\\d\\d:\\d\\d$")).match(diaryAt).hasMatch(), qPrintable(diaryAt));
    const QDateTime at = QDateTime::fromString(diaryAt, Qt::ISODate);
    QVERIFY(at.isValid());
    QVERIFY(at >= before && at <= QDateTime::currentDateTime().addSecs(1));
    QCOMPARE(at.offsetFromUtc(), QDateTime::currentDateTime().offsetFromUtc());
    QCOMPARE(created.value(QStringLiteral("diary_zone_id")).toString(), QString::fromUtf8(QTimeZone::systemTimeZoneId()));
    const QString createdId = created.value(QStringLiteral("comment_id")).toString();
    QCOMPARE(input->toPlainText(), QStringLiteral("First line\nsecond line"));
    overview.completeOperation(Thoughts::CommentsOverview::CreateOperation, QString(), createdId,
                               {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    QVERIFY(input->toPlainText().isEmpty());
    QVERIFY(!overview.m_operationPending);

    // Editing changes the text only; the entry keeps its time and phone keys.
    mutations.clear();
    QVERIFY(overview.saveDiaryEntryText(QStringLiteral("afternoon"), QStringLiteral("Wrote and tested the diary tab")));
    QCOMPARE(mutations.size(), 1);
    QCOMPARE(mutations.constFirst().at(1).toInt(), static_cast<int>(Thoughts::CommentsOverview::EditOperation));
    const QVariantMap edited = mutations.constFirst().at(0).toMap();
    QCOMPARE(edited.value(QStringLiteral("contents")).toString(), QStringLiteral("Wrote and tested the diary tab"));
    QCOMPARE(edited.value(QStringLiteral("diary_at")).toString(), today + QStringLiteral("T14:03:27+02:00"));
    QCOMPARE(edited.value(QStringLiteral("entry_kind")).toString(), QStringLiteral("diary"));
    QCOMPARE(edited.value(QStringLiteral("phone_only_key")).toString(), QStringLiteral("kept"));
    QCOMPARE(edited.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(list->topLevelItem(0)->child(0)->text(0), QStringLiteral("14:03"));
    QCOMPARE(list->topLevelItem(0)->child(0)->text(1), QStringLiteral("Wrote and tested the diary tab"));
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation, QString(), QStringLiteral("afternoon"),
                               {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    QCOMPARE(notes.proxy->rowCount(), 1);

    // Delete removes the selected entry the way a to-do is deleted.
    QSignalSpy deletions(&overview, &Thoughts::CommentsOverview::commentsDeleteRequested);
    list->setFocus();
    list->setCurrentItem(list->topLevelItem(1)->child(0));
    QTest::keyClick(list, Qt::Key_Delete);
    QCOMPARE(deletions.size(), 1);
    const auto targets = deletions.constFirst().at(0).value<QList<Thoughts::TrackedCommentTarget>>();
    QCOMPARE(targets.size(), 1);
    QCOMPARE(targets.constFirst().annotationId, QStringLiteral("before"));
    overview.completeBatchDelete(targets, {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    QCOMPARE(list->topLevelItemCount(), 1);
    QCOMPARE(overview.m_diarySummary->text(), QStringLiteral("2 diary entries"));
    QCOMPARE(notes.proxy->rowCount(), 1);
}

void CommentsOverviewTest::testNotesNavigationCountsAndMutations()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Thoughts::CommentsOverview overview(root.path());
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *summary = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewSummary"));
    auto *search = overview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewSearch"));
    QVERIFY(tree);
    QVERIFY(table);
    QVERIFY(summary);
    QVERIFY(search);
    Thoughts::TrackedCommentDocument unassigned, project;
    unassigned.displayName = QStringLiteral("No project");
    project.stem = QStringLiteral("paper");
    project.displayName = QStringLiteral("Paper");
    project.pdfPath = root.path() + QStringLiteral("/current_pdfs/paper.pdf");
    const auto item = [](const QString &id, const QString &text, const Thoughts::TrackedCommentDocument &doc, bool note = false, bool proposal = false) {
        Thoughts::TrackedComment result;
        result.id = result.threadRootId = id;
        result.contents = text;
        result.documentStem = doc.stem;
        result.pdfPath = doc.pdfPath;
        result.agentProposal = proposal;
        if (note) {
            result.logicalComment = true;
            result.logicalPayload = {{QStringLiteral("subtype"), QStringLiteral("Note")}};
        }
        return result;
    };
    unassigned.comments = {item(QStringLiteral("note-one"), QStringLiteral("Keyboard idea"), unassigned, true),
                           item(QStringLiteral("note-two"), QStringLiteral("Research idea"), unassigned, true),
                           item(QStringLiteral("shared-id"), QStringLiteral("Unassigned comment"), unassigned)};
    auto reply = item(QStringLiteral("reply"), QStringLiteral("A reply"), unassigned);
    reply.threadRootId = reply.parentId = QStringLiteral("note-one");
    reply.reply = true;
    unassigned.comments.append(reply);
    // Reused sidecar IDs must not let project rows leak into No project.
    project.comments = {item(QStringLiteral("shared-id"), QStringLiteral("Project comment"), project),
                        item(QStringLiteral("proposal"), QStringLiteral("Agent edit"), project, false, true)};
    overview.m_documents = {unassigned, project};
    overview.populateDocumentList(QString());
    overview.populateComments();
    overview.applyFilters();
    QCOMPARE(tree->columnCount(), 4);
    QCOMPARE(tree->headerItem()->text(3), QStringLiteral("To-dos"));
    auto *all = tree->topLevelItem(0);
    auto *none = tree->topLevelItem(1);
    auto *projects = tree->topLevelItem(2);
    QCOMPARE(all->text(0), QStringLiteral("All items"));
    QCOMPARE(all->text(1), QStringLiteral("2"));
    QCOMPARE(all->text(2), QStringLiteral("1"));
    QCOMPARE(all->text(3), QStringLiteral("2"));
    QCOMPARE(overview.commentCount(), 5);
    QCOMPARE(table->model()->rowCount(), 3);
    QVERIFY(summary->text().startsWith(QStringLiteral("3 items")));
    QVERIFY(summary->text().contains(QStringLiteral("2 comments · 1 agent proposal")));
    QCOMPARE(none->text(1), QStringLiteral("1"));
    QCOMPARE(none->text(3), QStringLiteral("2"));
    tree->setCurrentItem(none);
    QCOMPARE(table->model()->rowCount(), 1);
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    tree = overview.m_documentList;
    table = overview.m_table;
    summary = overview.m_summary;
    search = overview.m_search;
    none = tree->topLevelItem(1);
    projects = tree->topLevelItem(2);
    QCOMPARE(table->model()->rowCount(), 2);
    QCOMPARE(table->model()->rowCount(table->model()->index(0, 0)), 1);
    QVERIFY(none->child(1)->isHidden());
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    tree = overview.m_documentList;
    table = overview.m_table;
    summary = overview.m_summary;
    search = overview.m_search;
    none = tree->topLevelItem(1);
    projects = tree->topLevelItem(2);
    QVERIFY(overview.revealComment(QString(), QStringLiteral("note-one")));
    tree = overview.m_documentList;
    table = overview.m_table;
    summary = overview.m_summary;
    search = overview.m_search;
    none = tree->topLevelItem(1);
    projects = tree->topLevelItem(2);
    QCOMPARE(overview.m_tabs->currentWidget(), overview.m_notesPage);
    tree->setCurrentItem(none->child(0));
    QCOMPARE(table->model()->rowCount(), 2);
    search->setText(QStringLiteral("Keyboard"));
    QCOMPARE(table->model()->rowCount(), 1);
    search->clear();
    overview.populateDocumentList(QString());
    overview.applyFilters();
    QCOMPARE(tree->currentItem()->text(0), QStringLiteral("To-dos"));
    QCOMPARE(table->model()->rowCount(), 2);
    none = tree->topLevelItem(1);
    projects = tree->topLevelItem(2);
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    tree = overview.m_documentList;
    table = overview.m_table;
    summary = overview.m_summary;
    search = overview.m_search;
    none = tree->topLevelItem(1);
    projects = tree->topLevelItem(2);
    tree->setCurrentItem(none->child(1));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Unassigned comment"));
    tree->setCurrentItem(projects);
    QCOMPARE(table->model()->rowCount(), 2);
    overview.m_tabs->setCurrentWidget(overview.m_notesPage);
    tree = overview.m_documentList;
    table = overview.m_table;
    summary = overview.m_summary;
    search = overview.m_search;
    none = tree->topLevelItem(1);
    projects = tree->topLevelItem(2);
    tree->setCurrentItem(none->child(0));
    // The fast deletion path must update Notes, not the Comments column.
    overview.removeTargetsFromModel({{QString(), QStringLiteral("note-two"), 0, 0}});
    QCOMPARE(tree->currentItem(), none->child(0));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(none->text(3), QStringLiteral("1"));
    QCOMPARE(none->text(1), QStringLiteral("1"));
    QCOMPARE(tree->topLevelItem(0)->text(3), QStringLiteral("1"));
    QCOMPARE(overview.commentCount(), 4);
    // Resolving a proposal must update the header, tree and visible model together.
    overview.finishAgentProposalResolution({QStringLiteral("proposal")}, QStringLiteral("accept"), QStringLiteral("applied"), QString());
    tree->setCurrentItem(tree->topLevelItem(0));
    overview.applyFilters();
    QCOMPARE(overview.commentCount(), 3);
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(2), QStringLiteral("0"));
    QVERIFY(summary->text().contains(QStringLiteral("1 to-do")));
    // The last note can disappear while the unassigned comment remains reachable.
    tree->setCurrentItem(tree->topLevelItem(1)->child(0));
    overview.removeTargetsFromModel({{QString(), QStringLiteral("note-one"), 0, 0}, {QString(), QStringLiteral("reply"), 0, 0}});
    QCOMPARE(table->model()->rowCount(), 0);
    QCOMPARE(tree->topLevelItem(1)->text(1), QStringLiteral("1"));
    QCOMPARE(tree->topLevelItem(1)->text(3), QStringLiteral("0"));
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    tree = overview.m_documentList;
    table = overview.m_table;
    summary = overview.m_summary;
    search = overview.m_search;
    none = tree->topLevelItem(1);
    projects = tree->topLevelItem(2);
    tree->setCurrentItem(tree->topLevelItem(1)->child(1));
    QCOMPARE(table->model()->rowCount(), 1);
}

void CommentsOverviewTest::initTestCase()
{
    QStandardPaths::setTestModeEnabled(true);
    const QString iconPath = QFINDTESTDATA("../icons/32-apps-thoughts.png");
    QVERIFY(!iconPath.isEmpty());
    QApplication::setWindowIcon(QIcon(iconPath));
}

void CommentsOverviewTest::init()
{
    KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("CommentsOverview"));
    config.deleteGroup();
    KConfigGroup legacyConfig(KSharedConfig::openConfig(), QStringLiteral("AllCommentsDashboard"));
    legacyConfig.deleteGroup();
    config.sync();
}

void CommentsOverviewTest::testLibraryDateFormatting()
{
    QTemporaryDir root;
    Thoughts::CommentsOverview overview(root.path());
    overview.m_catalogPollTimer->stop();
    auto *tree = overview.m_libraryTree;
    tree->clear();
    const QList<QDateTime> dates{QDateTime(QDate::currentDate(), QTime(11, 0)),
        QDateTime(QDate::currentDate().addDays(-1), QTime(10, 0)),
        QDateTime(QDate(2025, 1, 15), QTime(9, 30)), QDateTime()};
    QList<QTreeWidgetItem *> items;
    for (int repeat = 0; repeat < 20; ++repeat) for (int i = 0; i < dates.size(); ++i) {
        auto *item = new QTreeWidgetItem(tree);
        const auto path = QStringLiteral("test-%1-%2").arg(repeat).arg(i);
        item->setData(0, Qt::UserRole + 1, path);
        overview.m_libraryModificationDates.insert(path, dates[i]);
        items.append(item);
    }
    overview.applyLibraryModificationDates();
    for (int i = 0; i < items.size(); ++i) {
        const auto date = dates[i % dates.size()];
        QCOMPARE(items[i]->data(1, Qt::UserRole + 100).toLongLong(), date.isValid() ? date.toMSecsSinceEpoch() : qint64(-1));
        if (date.isValid()) QCOMPARE(items[i]->toolTip(1), QStringLiteral("Modified: ") + QLocale().toString(date.toLocalTime(), QLocale::LongFormat));
        else QCOMPARE(items[i]->text(1), QString::fromUtf8("—"));
    }
    QCOMPARE(items[0]->text(1), QStringLiteral("Today, 11:00"));
    QCOMPARE(items[1]->text(1), QStringLiteral("Yesterday"));
    QCOMPARE(items[2]->text(1), QLocale().toString(dates[2].date(), QStringLiteral("d MMM yyyy")));
}

void CommentsOverviewTest::testProjectPagesCountBuiltDocuments()
{
    QTemporaryDir directory;
    const QString root = directory.path();
    const QString projectId = QStringLiteral("test-partially-built");
    const QString folderId = QStringLiteral("10000000-0000-4000-8000-000000000001");
    const QString builtId = QStringLiteral("20000000-0000-4000-8000-000000000001");
    const QString pendingId = QStringLiteral("20000000-0000-4000-8000-000000000002");
    const QString path = Thoughts::ProjectConfiguration::path(root, projectId);
    const QString folder = QDir(root).relativeFilePath(QFileInfo(path).path());
    const auto declaration = [&](const QString &id, const QString &file) {
        return QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("format"), QStringLiteral("pdf")},
            {QStringLiteral("included"), true}, {QStringLiteral("source"), QJsonObject{
                {QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), file}}}};
    };
    const QJsonObject manifest{{QStringLiteral("schema_version"), 5}, {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
        {QStringLiteral("id"), projectId}, {QStringLiteral("name"), QStringLiteral("Concepts")},
        {QStringLiteral("default_folder_id"), folderId}, {QStringLiteral("source_folders"), QJsonArray{QJsonObject{
            {QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), folder},
            {QStringLiteral("document_id"), QStringLiteral("30000000-0000-4000-8000-000000000001")}}}},
        {QStringLiteral("documents"), QJsonArray{declaration(builtId, QStringLiteral("built.pdf")), declaration(pendingId, QStringLiteral("pending.pdf"))}}};
    QVERIFY(Thoughts::ProjectConfiguration::save(path, manifest).isEmpty());
    const QJsonObject built{{QStringLiteral("document_id"), builtId}, {QStringLiteral("id"), QStringLiteral("built.pdf")},
        {QStringLiteral("name"), QStringLiteral("Built")}, {QStringLiteral("filename"), QStringLiteral("concepts_built.pdf")}};
    const QJsonObject pending{{QStringLiteral("document_id"), pendingId}, {QStringLiteral("id"), QStringLiteral("pending.pdf")},
        {QStringLiteral("name"), QStringLiteral("Pending")}, {QStringLiteral("filename"), QStringLiteral("concepts_pending.pdf")}};
    // A source project's target is its reading, not one PDF.
    const QJsonObject project{{QStringLiteral("id"), projectId}, {QStringLiteral("target"), QString()},
        {QStringLiteral("documents"), QJsonArray{built, pending}}};
    QVERIFY(writeFile(root + QStringLiteral("/.thoughts/library-projects.json"), QJsonDocument(QJsonObject{
        {QStringLiteral("projects"), QJsonArray{project}}}).toJson()));
    Thoughts::CommentsOverview overview(root);
    overview.m_catalogPollTimer->stop();
    Thoughts::TrackedCommentDocument publication;
    publication.libraryFilename = QStringLiteral("concepts_built.pdf");
    publication.libraryPageCount = 9;
    publication.libraryBuiltAt = QDateTime(QDate::currentDate().addDays(-1), QTime(12, 0));
    overview.m_documents = {publication};
    overview.updateLibraryTree();
    QCOMPARE(overview.m_libraryTree->topLevelItemCount(), 1);
    auto *projectRow = overview.m_libraryTree->topLevelItem(0);
    // A document not built yet does not hide the pages of the built ones.
    QCOMPARE(projectRow->text(5), QStringLiteral("9"));
    QCOMPARE(projectRow->toolTip(5), QStringLiteral("Pages of the built PDFs; 1 document is not built yet."));
}

void CommentsOverviewTest::testLibraryMetadataColumns()
{
    QTemporaryDir directory;
    const QString root = directory.path();
    const QString projectId = QStringLiteral("test-library-columns");
    const QString folderId = QStringLiteral("10000000-0000-4000-8000-000000000001");
    const QString firstId = QStringLiteral("20000000-0000-4000-8000-000000000001");
    const QString secondId = QStringLiteral("20000000-0000-4000-8000-000000000002");
    const QString thirdId = QStringLiteral("20000000-0000-4000-8000-000000000003");
    const QString path = Thoughts::ProjectConfiguration::path(root, projectId);
    const QString folder = QDir(root).relativeFilePath(QFileInfo(path).path());
    const auto declaration = [&](const QString &id, const QString &file, bool included) {
        return QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("format"), QStringLiteral("pdf")},
            {QStringLiteral("included"), included}, {QStringLiteral("source"), QJsonObject{
                {QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), file}}}};
    };
    const QJsonObject manifest{{QStringLiteral("schema_version"), 5}, {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
        {QStringLiteral("id"), projectId}, {QStringLiteral("name"), QStringLiteral("Example")},
        {QStringLiteral("default_folder_id"), folderId}, {QStringLiteral("source_folders"), QJsonArray{QJsonObject{
            {QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), folder},
            {QStringLiteral("document_id"), QStringLiteral("30000000-0000-4000-8000-000000000001")}}}},
        {QStringLiteral("documents"), QJsonArray{declaration(firstId, QStringLiteral("a.pdf"), true),
            declaration(secondId, QStringLiteral("b.pdf"), true), declaration(thirdId, QStringLiteral("c.pdf"), false)}}};
    QVERIFY(Thoughts::ProjectConfiguration::save(path, manifest).isEmpty());
    const QJsonObject first{{QStringLiteral("document_id"), firstId}, {QStringLiteral("id"), QStringLiteral("third_party/a.pdf")},
        {QStringLiteral("name"), QStringLiteral("A")}, {QStringLiteral("filename"), QStringLiteral("combined.pdf")},
        {QStringLiteral("start_page"), 1}, {QStringLiteral("page_count"), 5}};
    const QJsonObject second{{QStringLiteral("document_id"), secondId}, {QStringLiteral("id"), QStringLiteral("third_party/b.pdf")},
        {QStringLiteral("name"), QStringLiteral("B")}, {QStringLiteral("filename"), QStringLiteral("combined.pdf")},
        {QStringLiteral("start_page"), 6}, {QStringLiteral("page_count"), 7}};
    const QJsonObject project{{QStringLiteral("id"), projectId}, {QStringLiteral("target"), QStringLiteral("combined.pdf")},
        {QStringLiteral("documents"), QJsonArray{first, second}}};
    QVERIFY(writeFile(root + QStringLiteral("/.thoughts/library-projects.json"), QJsonDocument(QJsonObject{
        {QStringLiteral("projects"), QJsonArray{project}}}).toJson()));
    Thoughts::CommentsOverview overview(root);
    overview.m_catalogPollTimer->stop();
    Thoughts::TrackedCommentDocument publication;
    publication.libraryFilename = QStringLiteral("combined.pdf");
    publication.libraryPageCount = 12;
    publication.libraryBuiltAt = QDateTime(QDate::currentDate().addDays(-1), QTime(12, 0));
    overview.m_documents = {publication};
    overview.updateLibraryTree();
    auto *tree = overview.m_libraryTree;
    QCOMPARE(tree->columnCount(), 11);
    QCOMPARE(tree->headerItem()->text(1), QStringLiteral("Date"));
    QCOMPARE(tree->headerItem()->text(4), QStringLiteral("Review cards"));
    QCOMPARE(tree->headerItem()->text(5), QStringLiteral("Pages"));
    QCOMPARE(tree->headerItem()->text(6), QStringLiteral("Files"));
    QCOMPARE(tree->header()->visualIndex(6), tree->header()->visualIndex(3) + 1);
    QCOMPARE(tree->topLevelItemCount(), 1);
    auto *projectRow = tree->topLevelItem(0);
    auto *documents = projectRow->child(0);
    QCOMPARE(documents->childCount(), 2);
    QCOMPARE(projectRow->text(5), QStringLiteral("12"));
    QCOMPARE(documents->child(0)->text(5), QStringLiteral("5"));
    QCOMPARE(documents->child(1)->text(5), QStringLiteral("7"));
    QCOMPARE(projectRow->text(1), QStringLiteral("Yesterday"));
    QVERIFY(projectRow->toolTip(1).startsWith(QStringLiteral("Last built:")));
    QCOMPARE(documents->child(0)->text(1), QString::fromUtf8("—"));
    QCOMPARE(projectRow->text(4), QString::fromUtf8("—"));
    const auto card = [](const QString &id, const QString &document, bool deleted = false) {
        Thoughts::ReviewBrowserCard value;
        value.card.record = {{QStringLiteral("card_id"), id}, {QStringLiteral("status"), QStringLiteral("approved")},
            {QStringLiteral("primary"), QJsonObject{{QStringLiteral("document_id"), document}}}};
        value.card.schedule.deleted = deleted;
        return value;
    };
    const auto a = card(QStringLiteral("a"), firstId);
    overview.m_reviewCardsWidget->applyCards({a, a, card(QStringLiteral("deleted"), firstId, true),
        card(QStringLiteral("excluded-document"), thirdId)}, {});
    QCOMPARE(projectRow->text(4), QStringLiteral("2"));
    QCOMPARE(documents->child(0)->text(4), QStringLiteral("1"));
    QCOMPARE(documents->child(1)->text(4), QStringLiteral("0"));
    QCOMPARE(documents->text(4), QString());
    auto *sources = projectRow->child(1);
    QCOMPARE(sources->text(4), QString());
    QCOMPARE(sources->text(5), QString());
    // Files counts source files only; the project row repeats the Sources total.
    QCOMPARE(documents->text(6), QString());
    QCOMPARE(documents->child(0)->text(6), QString());
    QVERIFY(!sources->text(6).isEmpty());
    QCOMPARE(projectRow->text(6), sources->text(6));
    // Source changes must not replace the build date on the project.
    auto *source = sources->child(0);
    source->setData(0, Qt::UserRole + 115, QStringLiteral("source-path"));
    overview.m_libraryModificationDates.insert(QStringLiteral("source-path"), QDateTime::currentDateTime());
    overview.applyLibraryModificationDates();
    QCOMPARE(projectRow->text(1), QStringLiteral("Yesterday"));
    overview.m_reviewCardsWidget->applyCards({card(QStringLiteral("a"), secondId)}, {});
    QCOMPARE(projectRow->text(4), QStringLiteral("1"));
    QCOMPARE(documents->child(0)->text(4), QStringLiteral("0"));
    QCOMPARE(documents->child(1)->text(4), QStringLiteral("1"));
    auto legacy = card(QStringLiteral("legacy"), QStringLiteral("old-combined-id"));
    legacy.card.sourceLocation = {{QStringLiteral("state"), QStringLiteral("linked")},
        {QStringLiteral("source_file"), QStringLiteral("third_party/b.pdf")}};
    overview.m_reviewCardsWidget->applyCards({legacy}, {});
    QCOMPARE(projectRow->text(4), QStringLiteral("1"));
    QCOMPARE(documents->child(0)->text(4), QStringLiteral("0"));
    QCOMPARE(documents->child(1)->text(4), QStringLiteral("1"));
    legacy.card.sourceLocation = {};
    auto primary = legacy.card.record.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("pdf_stem"), QStringLiteral("combined"));
    legacy.card.record.insert(QStringLiteral("primary"), primary);
    overview.m_reviewCardsWidget->applyCards({legacy}, {});
    QCOMPARE(projectRow->text(4), QStringLiteral("1"));
    QCOMPARE(documents->child(0)->text(4), QString::fromUtf8("—"));
    overview.m_reviewCardsWidget->applyCards({}, {QStringLiteral("Read failed")});
    QCOMPARE(projectRow->text(4), QString::fromUtf8("—"));
    overview.m_documents.clear();
    overview.updateLibraryTree();
    QCOMPARE(tree->topLevelItem(0)->text(5), QString::fromUtf8("—"));
    QCOMPARE(tree->topLevelItem(0)->text(1), QString::fromUtf8("—"));
}

namespace {
// Writes a Project whose published documents are built from a LaTeX source, imported as a PDF,
// and of a kind not known yet, plus an unpublished LaTeX document.
QString writePageKindsProject(const QString &root)
{
    const QString projectId = QStringLiteral("test-page-kinds");
    const QString folderId = QStringLiteral("10000000-0000-4000-8000-000000000001");
    const QString path = Thoughts::ProjectConfiguration::path(root, projectId);
    const QString folder = QDir(root).relativeFilePath(QFileInfo(path).path());
    QJsonArray declarations;
    // The fourth document is unchecked, so it stays in the Project unpublished.
    for (int i = 1; i <= 4; ++i)
        declarations.append(QJsonObject{{QStringLiteral("document_id"), QStringLiteral("20000000-0000-4000-8000-00000000000%1").arg(i)},
            {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), i != 4}, {QStringLiteral("source"), QJsonObject{
                {QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), QStringLiteral("document-%1.pdf").arg(i)}}}});
    const QJsonObject manifest{{QStringLiteral("schema_version"), 5}, {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
        {QStringLiteral("id"), projectId}, {QStringLiteral("name"), QStringLiteral("Kinds")},
        {QStringLiteral("default_folder_id"), folderId}, {QStringLiteral("source_folders"), QJsonArray{QJsonObject{
            {QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), folder},
            {QStringLiteral("document_id"), QStringLiteral("30000000-0000-4000-8000-000000000001")}}}},
        {QStringLiteral("documents"), declarations}};
    if (!Thoughts::ProjectConfiguration::save(path, manifest).isEmpty()) return {};
    const auto document = [](int index, const QString &id, const QString &filename, int pages) {
        return QJsonObject{{QStringLiteral("document_id"), QStringLiteral("20000000-0000-4000-8000-00000000000%1").arg(index)},
            {QStringLiteral("id"), id}, {QStringLiteral("name"), QFileInfo(filename).completeBaseName()},
            {QStringLiteral("filename"), filename}, {QStringLiteral("page_count"), pages}};
    };
    QJsonObject lecture = document(1, QStringLiteral("notes/lecture.tex"), QStringLiteral("kinds_lecture.pdf"), 154);
    lecture.insert(QStringLiteral("source_file"), QStringLiteral("notes/lecture.tex"));
    QJsonObject paper = document(2, QStringLiteral("paper.pdf"), QStringLiteral("paper.pdf"), 201);
    paper.insert(QStringLiteral("imported"), true);
    // No source file and no ".pdf" id: its kind comes from the Library index.
    const QJsonObject other = document(3, QStringLiteral("other"), QStringLiteral("other.pdf"), 12);
    QJsonObject draft = document(4, QStringLiteral("notes/draft.tex"), QStringLiteral("kinds_draft.pdf"), 40);
    draft.insert(QStringLiteral("source_file"), QStringLiteral("notes/draft.tex"));
    draft.insert(QStringLiteral("published"), false);
    const QJsonObject project{{QStringLiteral("id"), projectId}, {QStringLiteral("target"), QString()},
        {QStringLiteral("documents"), QJsonArray{lecture, paper, other}}, {QStringLiteral("unpublished_documents"), QJsonArray{draft}}};
    if (!writeFile(root + QStringLiteral("/.thoughts/library-projects.json"), QJsonDocument(QJsonObject{
            {QStringLiteral("projects"), QJsonArray{project}}}).toJson())) return {};
    return projectId;
}

// The Library header's sections as "visible" and "hidden" lists of header labels, in visual order.
QPair<QStringList, QStringList> libraryColumns(QTreeWidget *tree)
{
    QStringList visible, hidden;
    for (int visual = 0; visual < tree->header()->count(); ++visual) {
        const int column = tree->header()->logicalIndex(visual);
        (tree->header()->isSectionHidden(column) ? hidden : visible).append(tree->headerItem()->text(column));
    }
    return {visible, hidden};
}

QAction *menuAction(QMenu *menu, const QString &text)
{
    for (QAction *action : menu->actions())
        if (action->text() == text) return action;
    return nullptr;
}
}

void CommentsOverviewTest::testLibraryPagesByKindAndDueCards()
{
    QTemporaryDir directory;
    const QString root = directory.path();
    QVERIFY(!writePageKindsProject(root).isEmpty());
    Thoughts::CommentsOverview overview(root);
    overview.m_catalogPollTimer->stop();
    overview.m_documents.clear();
    overview.updateLibraryTree();
    auto *tree = overview.m_libraryTree;
    QCOMPARE(tree->headerItem()->text(7), QStringLiteral("Source pages"));
    QCOMPARE(tree->headerItem()->text(8), QStringLiteral("PDF pages"));
    QCOMPARE(tree->headerItem()->text(9), QStringLiteral("Due cards"));
    QCOMPARE(tree->headerItem()->text(10), QStringLiteral("Documents"));
    QCOMPARE(tree->topLevelItemCount(), 1);
    auto *projectRow = tree->topLevelItem(0);
    auto *documents = projectRow->child(0);
    const auto row = [&documents](const QString &name) -> QTreeWidgetItem * {
        for (int i = 0; i < documents->childCount(); ++i)
            if (documents->child(i)->text(0) == name) return documents->child(i);
        return nullptr;
    };
    QVERIFY(row(QStringLiteral("kinds_lecture")) && row(QStringLiteral("paper")) && row(QStringLiteral("other")) && row(QStringLiteral("kinds_draft")));
    // Each document's pages go to the column of its kind, as its link symbol shows it.
    QCOMPARE(row(QStringLiteral("kinds_lecture"))->text(7), QStringLiteral("154"));
    QCOMPARE(row(QStringLiteral("kinds_lecture"))->text(8), QString());
    QCOMPARE(row(QStringLiteral("paper"))->text(7), QString());
    QCOMPARE(row(QStringLiteral("paper"))->text(8), QStringLiteral("201"));
    QCOMPARE(row(QStringLiteral("other"))->text(7), QString::fromUtf8("—"));
    QCOMPARE(row(QStringLiteral("other"))->text(8), QString::fromUtf8("—"));
    QCOMPARE(row(QStringLiteral("other"))->text(5), QStringLiteral("12"));
    QCOMPARE(row(QStringLiteral("kinds_draft"))->text(7), QStringLiteral("40"));
    // The project sums its published documents; pages of an unknown kind count in neither column.
    QCOMPARE(projectRow->text(7), QStringLiteral("154"));
    QCOMPARE(projectRow->text(8), QStringLiteral("201"));
    QCOMPARE(projectRow->toolTip(7), QStringLiteral("12 pages are in PDFs whose sources are not known yet."));
    QCOMPARE(projectRow->toolTip(8), projectRow->toolTip(7));
    QCOMPARE(projectRow->data(7, Qt::UserRole + 100).toLongLong(), qint64(154));
    QCOMPARE(projectRow->text(10), QStringLiteral("3"));
    // Once the Library index says the PDF has no sources, its pages are PDF pages.
    Thoughts::TrackedCommentDocument indexed;
    indexed.libraryFilename = QStringLiteral("other.pdf");
    indexed.libraryKind = QStringLiteral("unlinked");
    overview.m_documents = {indexed};
    overview.updateLibraryTree();
    projectRow = tree->topLevelItem(0);
    QCOMPARE(projectRow->text(7), QStringLiteral("154"));
    QCOMPARE(projectRow->text(8), QStringLiteral("213"));
    QVERIFY(projectRow->toolTip(8).isEmpty());

    // Due cards count the project's cards the review queue would offer now.
    const auto card = [](const QString &id, int document, bool due) {
        Thoughts::ReviewBrowserCard value;
        value.card.record = {{QStringLiteral("card_id"), id}, {QStringLiteral("status"), QStringLiteral("approved")},
            {QStringLiteral("primary"), QJsonObject{{QStringLiteral("document_id"), QStringLiteral("20000000-0000-4000-8000-00000000000%1").arg(document)}}}};
        if (!due) {
            value.state = Thoughts::ReviewBrowserState::Future;
            value.card.schedule.reviewed = true;
            value.card.schedule.intervalDays = 5;
            value.card.schedule.dueAt = QDateTime::currentDateTimeUtc().addDays(5);
        }
        return value;
    };
    QCOMPARE(projectRow->text(9), QString::fromUtf8("—"));
    overview.m_reviewCardsWidget->applyCards({card(QStringLiteral("due"), 1, true), card(QStringLiteral("later"), 1, false),
        card(QStringLiteral("paper-due"), 2, true)}, {});
    QCOMPARE(overview.m_reviewCardsWidget->dueCardCount(), 2);
    QCOMPARE(projectRow->text(4), QStringLiteral("3"));
    QCOMPARE(projectRow->text(9), QStringLiteral("2"));
    documents = projectRow->child(0);
    QCOMPARE(row(QStringLiteral("kinds_lecture"))->text(4), QStringLiteral("2"));
    QCOMPARE(row(QStringLiteral("kinds_lecture"))->text(9), QStringLiteral("1"));
    QCOMPARE(row(QStringLiteral("paper"))->text(9), QStringLiteral("1"));
    QCOMPARE(row(QStringLiteral("other"))->text(9), QStringLiteral("0"));
    overview.m_reviewCardsWidget->applyCards({}, {QStringLiteral("Read failed")});
    QCOMPARE(projectRow->text(9), QString::fromUtf8("—"));
}

void CommentsOverviewTest::testLibraryHeaderColumnsMenu()
{
    QTemporaryDir directory;
    const QString root = directory.path();
    QVERIFY(!writePageKindsProject(root).isEmpty());
    const QStringList defaultVisible{QStringLiteral("Project"), QStringLiteral("Date"), QStringLiteral("Size"), QStringLiteral("Files"),
        QStringLiteral("Review cards"), QStringLiteral("Pages"), QStringLiteral("Source pages"), QStringLiteral("PDF pages")};
    const QStringList defaultHidden{QStringLiteral("Documents"), QStringLiteral("Due cards"), QStringLiteral("Tracked sources")};
    const auto saved = [] {
        return KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("CommentsOverview")).readEntry("LibraryHeaderStateV1", QByteArray());
    };
    // Opens the Library header menu and hands it to @p use, which may trigger its actions.
    const auto withLibraryHeaderMenu = [](Thoughts::CommentsOverview &overview, const std::function<void(QMenu *)> &use) {
        bool opened = false;
        QTimer::singleShot(0, &overview, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            opened = true;
            // Never leave a popup open, even when a check below fails.
            QTimer::singleShot(3000, menu, &QMenu::close);
            use(menu);
            menu->close();
        });
        overview.showLibraryHeaderMenu(QPoint(4, 4));
        QVERIFY(opened);
    };
    {
        Thoughts::CommentsOverview overview(root);
        overview.m_catalogPollTimer->stop();
        overview.updateLibraryTree();
        auto *tree = overview.m_libraryTree;
        QCOMPARE(libraryColumns(tree), qMakePair(defaultVisible, defaultHidden));
        // Headers move by dragging, except Project, which holds the tree.
        QVERIFY(tree->header()->sectionsMovable());
        QVERIFY(!tree->header()->isFirstSectionMovable());
        QCOMPARE(tree->header()->contextMenuPolicy(), Qt::CustomContextMenu);
        QVERIFY(saved().isEmpty());

        // The menu lists every column with a checkbox, in the default order; Project cannot be hidden.
        withLibraryHeaderMenu(overview, [](QMenu *menu) {
            QStringList texts;
            for (QAction *action : menu->actions()) texts.append(action->isSeparator() ? QStringLiteral("|") : action->text());
            QCOMPARE(texts, (QStringList{QStringLiteral("Project"), QStringLiteral("Date"), QStringLiteral("Size"), QStringLiteral("Files"),
                QStringLiteral("Documents"), QStringLiteral("Review cards"), QStringLiteral("Due cards"), QStringLiteral("Pages"),
                QStringLiteral("Source pages"), QStringLiteral("PDF pages"), QStringLiteral("|"), QStringLiteral("Restore default columns")}));
            QAction *project = menuAction(menu, QStringLiteral("Project"));
            QVERIFY(project->isCheckable() && project->isChecked() && !project->isEnabled());
            QVERIFY(menuAction(menu, QStringLiteral("PDF pages"))->isChecked());
            QVERIFY(!menuAction(menu, QStringLiteral("Due cards"))->isChecked());
            QCOMPARE(menuAction(menu, QStringLiteral("Source pages"))->toolTip(), QStringLiteral("Pages of PDFs built from linked sources (LaTeX, Markdown)."));
            QVERIFY(menu->toolTipsVisible());
            menuAction(menu, QStringLiteral("Due cards"))->trigger();
            menuAction(menu, QStringLiteral("PDF pages"))->trigger();
        });
        QVERIFY(!tree->isColumnHidden(9));
        QVERIFY(tree->isColumnHidden(8));
        QVERIFY(!saved().isEmpty());
        // Rows sorted by a column go back to Project order when it is hidden.
        tree->sortByColumn(7, Qt::DescendingOrder);
        withLibraryHeaderMenu(overview, [](QMenu *menu) { menuAction(menu, QStringLiteral("Source pages"))->trigger(); });
        QVERIFY(tree->isColumnHidden(7));
        QCOMPARE(tree->header()->sortIndicatorSection(), 0);
        QCOMPARE(tree->header()->sortIndicatorOrder(), Qt::AscendingOrder);
        // Dragging Pages after Project ends as this move; it is saved too.
        const QByteArray beforeMove = saved();
        tree->header()->moveSection(tree->header()->visualIndex(5), 1);
        QVERIFY(saved() != beforeMove);
        QCOMPARE(libraryColumns(tree).first, (QStringList{QStringLiteral("Project"), QStringLiteral("Pages"), QStringLiteral("Date"),
            QStringLiteral("Size"), QStringLiteral("Files"), QStringLiteral("Review cards"), QStringLiteral("Due cards")}));
    }
    // The next start shows the same columns in the same order, sorted by Project as before.
    {
        Thoughts::CommentsOverview overview(root);
        overview.m_catalogPollTimer->stop();
        overview.updateLibraryTree();
        auto *tree = overview.m_libraryTree;
        QCOMPARE(libraryColumns(tree).first, (QStringList{QStringLiteral("Project"), QStringLiteral("Pages"), QStringLiteral("Date"),
            QStringLiteral("Size"), QStringLiteral("Files"), QStringLiteral("Review cards"), QStringLiteral("Due cards")}));
        QCOMPARE(tree->header()->sortIndicatorSection(), 0);
        QCOMPARE(tree->header()->sectionResizeMode(0), QHeaderView::Stretch);
        QCOMPARE(tree->header()->sectionResizeMode(9), QHeaderView::ResizeToContents);
        QVERIFY(!tree->header()->isFirstSectionMovable());
        // Restore default columns brings back the default order and columns and forgets the saved ones.
        withLibraryHeaderMenu(overview, [](QMenu *menu) {
            QVERIFY(menuAction(menu, QStringLiteral("Due cards"))->isChecked());
            QVERIFY(!menuAction(menu, QStringLiteral("Source pages"))->isChecked());
            menuAction(menu, QStringLiteral("Restore default columns"))->trigger();
        });
        QCOMPARE(libraryColumns(tree), qMakePair(defaultVisible, defaultHidden));
        QVERIFY(saved().isEmpty());
        // A saved layout cannot hide Project or move it from the front.
        tree->header()->moveSection(0, 4);
        tree->header()->setSectionHidden(0, true);
        tree->header()->setSectionHidden(2, false);
        overview.saveLibraryColumns();
    }
    Thoughts::CommentsOverview overview(root);
    overview.m_catalogPollTimer->stop();
    auto *header = overview.m_libraryTree->header();
    QCOMPARE(header->logicalIndex(0), 0);
    QVERIFY(!header->isSectionHidden(0));
    QVERIFY(header->isSectionHidden(2));
}

void CommentsOverviewTest::testLibraryContentModificationDates()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString source = temporary.filePath(QStringLiteral("source.tex"));
    const QDateTime yesterday(QDate::currentDate().addDays(-1), QTime(10, 0));
    const QDateTime today(QDate::currentDate(), QTime(11, 0));
    const auto writeAt = [&](const QByteArray &bytes, const QDateTime &date) {
        if (!writeFile(source, bytes)) return false;
        QFile file(source);
        return file.open(QIODevice::ReadWrite) && file.setFileTime(date, QFileDevice::FileModificationTime);
    };
    QVERIFY(writeAt(QByteArrayLiteral("Original source.\n"), yesterday));
    const QHash<QString, bool> sources {{source, false}};
    QCOMPARE(Thoughts::libraryModificationDates(temporary.path(), sources).value(source), yesterday);
    QVERIFY(writeAt(QByteArrayLiteral("Original source.\n"), today));
    QCOMPARE(Thoughts::libraryModificationDates(temporary.path(), sources).value(source), today);
    QVERIFY(QFile::remove(source));
    QVERIFY(!Thoughts::libraryModificationDates(temporary.path(), sources).contains(source));

    const QString pdf = temporary.filePath(QStringLiteral("imported.pdf"));
    QVERIFY(writeFile(pdf, QByteArrayLiteral("%PDF-test")));
    const QHash<QString, bool> pdfs {{pdf, true}};
    QVERIFY(!Thoughts::libraryModificationDates(temporary.path(), pdfs).contains(pdf));
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral(".thoughts/library-modified.json"))));
}

void CommentsOverviewTest::testLibraryModificationDatesSavesPartialProgress()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString first = temporary.filePath(QStringLiteral("first.tex"));
    const QString second = temporary.filePath(QStringLiteral("second.tex"));
    QVERIFY(writeFile(first, QByteArrayLiteral("First.\n")));
    QVERIFY(writeFile(second, QByteArrayLiteral("Second.\n")));
    const QHash<QString, bool> paths {{first, false}, {second, false}};
    bool incomplete = true;
    const auto dates = Thoughts::libraryModificationDates(temporary.path(), paths, &incomplete, 0);
    QVERIFY(!incomplete);
    QCOMPARE(dates.size(), 2);
}

void CommentsOverviewTest::testProjectManifestRepairAndUniformIcons()
{
    QTemporaryDir temporary;
    Thoughts::LibraryProjectStore store(temporary.path());
    const QString source = QStringLiteral("docs-import-example");
    QVERIFY(writeFile(temporary.filePath(source + QStringLiteral("/markdown/source/a.md")), QByteArrayLiteral("# A\n")));
    const QByteArray bytes = QByteArrayLiteral("{\"schema_version\":1,\"documents\":[{\"format\":\"markdown\",\"source\":\"markdown/source/a.md\"}]}\n");
    const QJsonObject manifest{{QStringLiteral("path"), QString(source + QStringLiteral("/thoughts-project.json"))},
        {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("content"), QString::fromLatin1(bytes.toBase64())}};
    // The catalogue also carries each current Project configuration; missing ones are installed.
    const auto configuration = [&](const QString &id, const QString &name, const QStringList &folders) {
        auto config = Thoughts::ProjectConfiguration::empty(temporary.path(), id, name);
        auto bindings = config.value(QStringLiteral("source_folders")).toArray();
        for (const auto &folder : folders)
            bindings.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), folder}});
        config.insert(QStringLiteral("source_folders"), bindings);
        const QByteArray content = QJsonDocument(config).toJson();
        return QJsonObject{{QStringLiteral("path"), QDir(temporary.path()).relativeFilePath(store.projectManifestPath(id))},
            {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex())},
            {QStringLiteral("content"), QString::fromLatin1(content.toBase64())}};
    };
    const auto mixedConfiguration = configuration(QStringLiteral("new:mixed"), QStringLiteral("Lab"), {source});
    QJsonObject imported{{QStringLiteral("id"), QStringLiteral("new:mixed")}, {QStringLiteral("name"), QStringLiteral("Lab")},
        {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), source}}}},
        {QStringLiteral("source_manifests"), QJsonArray{mixedConfiguration, manifest}}, {QStringLiteral("documents"), QJsonArray{}}};
    const QJsonObject empty{{QStringLiteral("id"), QStringLiteral("new:empty")}, {QStringLiteral("name"), QStringLiteral("Empty project")},
        {QStringLiteral("source_manifests"), QJsonArray{configuration(QStringLiteral("new:empty"), QStringLiteral("Empty project"), {})}}, {QStringLiteral("documents"), QJsonArray{}}};
    const QJsonObject snapshot{{QStringLiteral("projects"), QJsonArray{imported, empty}}};
    QVERIFY(store.receive(snapshot).isEmpty());
    QFile installed(temporary.filePath(source + QStringLiteral("/thoughts-project.json")));
    QVERIFY(installed.open(QIODevice::ReadOnly)); QCOMPARE(installed.readAll(), bytes); installed.close();
    QVERIFY(store.receive(snapshot).isEmpty());
    // An existing edited source manifest is preserved for normal source Sync.
    QVERIFY(writeFile(installed.fileName(), bytes + QByteArrayLiteral("\n")));
    QVERIFY(store.receive(snapshot).isEmpty());
    QVERIFY(installed.open(QIODevice::ReadOnly)); QCOMPARE(installed.readAll(), bytes + QByteArrayLiteral("\n")); installed.close();
    auto invalid = manifest; invalid.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('0')));
    imported.insert(QStringLiteral("source_manifests"), QJsonArray{mixedConfiguration, invalid});
    QVERIFY(!store.receive({{QStringLiteral("projects"), QJsonArray{imported}}}).isEmpty());
    Thoughts::CommentsOverview overview(temporary.path());
    overview.resize(900, 600); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 2);
    QCOMPARE(tree->topLevelItem(0)->icon(0).pixmap(24).toImage(), tree->topLevelItem(1)->icon(0).pixmap(24).toImage());
    for (int i = 0; i < 2; ++i) {
        QCOMPARE(tree->topLevelItem(i)->data(0, Qt::UserRole + 114).toString(), QStringLiteral("Project"));
        QCOMPARE(tree->topLevelItem(i)->child(0)->text(0), QStringLiteral("No documents yet"));
        QVERIFY(QFileInfo::exists(store.projectManifestPath(tree->topLevelItem(i)->data(0, Qt::UserRole + 140).value<QJsonObject>().value(QStringLiteral("id")).toString())));
    }
    const auto screenshot = qEnvironmentVariable("THOUGHTS_UNIFIED_PROJECT_SCREENSHOT");
    if (!screenshot.isEmpty()) { tree->expandToDepth(1); QTest::qWait(200); QVERIFY(tree->grab().save(screenshot)); }
}

void CommentsOverviewTest::testRemovedProjectConfigurationIsRetired()
{
    // 10 November 2026: a removed Project's configuration stayed on the Mac and
    // kept its docs-* folder published, so Sync never matched the Library again.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::LibraryProjectStore store(temporary.path());
    const auto project = [&](const QString &id, const QString &name, const QString &folder) {
        auto config = Thoughts::ProjectConfiguration::empty(temporary.path(), id, name);
        auto bindings = config.value(QStringLiteral("source_folders")).toArray();
        bindings.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), folder}});
        config.insert(QStringLiteral("source_folders"), bindings);
        const QByteArray content = QJsonDocument(config).toJson();
        return QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("revision"), 1}, {QStringLiteral("documents"), QJsonArray{}},
            {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), folder}}}},
            {QStringLiteral("source_manifests"), QJsonArray{QJsonObject{{QStringLiteral("path"), QDir(temporary.path()).relativeFilePath(store.projectManifestPath(id))},
                {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex())},
                {QStringLiteral("content"), QString::fromLatin1(content.toBase64())}}}}};
    };
    for (const auto *folder : {"docs-removed-1a2b3c", "docs-edited-4d5e6f", "docs-busy-7a8b9c", "docs-kept-0d1e2f"})
        QVERIFY(writeFile(temporary.filePath(QLatin1String(folder) + QStringLiteral("/tex/source/main.tex")), QByteArrayLiteral("Text.\n")));
    const QJsonObject removed = project(QStringLiteral("source:docs-removed"), QStringLiteral("Removed"), QStringLiteral("docs-removed-1a2b3c"));
    const QJsonObject edited = project(QStringLiteral("new:edited"), QStringLiteral("Edited"), QStringLiteral("docs-edited-4d5e6f"));
    const QJsonObject busy = project(QStringLiteral("new:busy"), QStringLiteral("Busy"), QStringLiteral("docs-busy-7a8b9c"));
    const QJsonObject kept = project(QStringLiteral("new:kept"), QStringLiteral("Kept"), QStringLiteral("docs-kept-0d1e2f"));
    QCOMPARE(store.receive({{QStringLiteral("projects"), QJsonArray{removed, edited, busy, kept}}}), QString());
    for (const auto &value : {removed, edited, busy, kept})
        QVERIFY(QFileInfo::exists(store.projectManifestPath(value.value(QStringLiteral("id")).toString())));

    // The user edited one configuration, and a change to another is still queued.
    const QString editedPath = store.projectManifestPath(QStringLiteral("new:edited"));
    QFile editedFile(editedPath);
    QVERIFY(editedFile.open(QIODevice::ReadOnly));
    const QByteArray editedBytes = editedFile.readAll() + QByteArrayLiteral("\n");
    editedFile.close();
    QVERIFY(writeFile(editedPath, editedBytes));
    QCOMPARE(store.enqueue(QStringLiteral("rename"), busy, {{QStringLiteral("name"), QStringLiteral("Busy renamed")}}), QString());

    // All but one are removed, on this computer or another.
    QCOMPARE(store.receive({{QStringLiteral("projects"), QJsonArray{kept}}}), QString());
    QVERIFY(!QFileInfo::exists(store.projectManifestPath(QStringLiteral("source:docs-removed"))));
    // The folders and their files stay.
    QVERIFY(QFileInfo::exists(temporary.filePath(QStringLiteral("docs-removed-1a2b3c/tex/source/main.tex"))));
    QFile stillEdited(editedPath);
    QVERIFY(stillEdited.open(QIODevice::ReadOnly));
    QCOMPARE(stillEdited.readAll(), editedBytes);
    QVERIFY(QFileInfo::exists(store.projectManifestPath(QStringLiteral("new:busy"))));
    QVERIFY(QFileInfo::exists(store.projectManifestPath(QStringLiteral("new:kept"))));
    QFile catalogue(store.path());
    QVERIFY(catalogue.open(QIODevice::ReadOnly));
    const auto generated = QJsonDocument::fromJson(catalogue.readAll()).object().value(QStringLiteral("generated_manifests")).toObject();
    QVERIFY(!generated.contains(QDir(temporary.path()).relativeFilePath(store.projectManifestPath(QStringLiteral("source:docs-removed")))));
    QVERIFY(generated.contains(QDir(temporary.path()).relativeFilePath(store.projectManifestPath(QStringLiteral("new:kept")))));
}

void CommentsOverviewTest::testCreatedProjectManifestFollowsFedora()
{
    // 2026-10-12: a Project created here kept its first manifest after Fedora
    // wrote later revisions (a folder moved into it). This computer's own
    // copies follow Fedora; edits and queued changes stay.
    QTemporaryDir temporary;
    Thoughts::LibraryProjectStore store(temporary.path());
    const QString id = QStringLiteral("new:apps");
    const QString file = store.projectManifestPath(id);
    QVERIFY(store.enqueue(QStringLiteral("create"), {{QStringLiteral("id"), id}, {QStringLiteral("revision"), 0}}, {{QStringLiteral("name"), QStringLiteral("Apps")}}).isEmpty());
    const QByteArray created = fileContents(file);
    QVERIFY(!created.isEmpty());
    const QString createOperation = store.pending().first().toObject().value(QStringLiteral("operation_id")).toString();
    // Fedora's revisions in its own layout: the created Project, then with a folder moved in.
    const QJsonObject first = Thoughts::ProjectConfiguration::empty(temporary.path(), id, QStringLiteral("Apps"));
    QJsonObject second = first;
    QJsonArray folders = second.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject {{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {QStringLiteral("path"), QStringLiteral("docs-playlists")}, {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
    second.insert(QStringLiteral("source_folders"), folders);
    const auto fedora = [](const QJsonObject &value) { return QJsonDocument(value).toJson(QJsonDocument::Indented).replace("    ", "  "); };
    const auto snapshot = [&](const QByteArray &bytes, const QString &name = QStringLiteral("Apps")) {
        return QJsonObject {{QStringLiteral("projects"), QJsonArray {QJsonObject {{QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("documents"), QJsonArray {}},
            {QStringLiteral("source_manifests"), QJsonArray {QJsonObject {{QStringLiteral("path"), QDir(temporary.path()).relativeFilePath(file)},
                {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())},
                {QStringLiteral("content"), QString::fromLatin1(bytes.toBase64())}}}}}}}};
    };
    QVERIFY(fedora(first) != created);
    // Not while Fedora has yet to apply the queued creation.
    QVERIFY(store.receive(snapshot(fedora(first))).isEmpty());
    QCOMPARE(fileContents(file), created);
    QVERIFY(store.receive(snapshot(fedora(first)), createOperation).isEmpty());
    QCOMPARE(fileContents(file), fedora(first));
    QVERIFY(store.receive(snapshot(fedora(second))).isEmpty());
    QCOMPARE(fileContents(file), fedora(second));

    // A queued Library change keeps this computer's copy until Fedora applies it.
    QVERIFY(store.enqueue(QStringLiteral("rename"), store.projects(false).first().toObject(), {{QStringLiteral("name"), QStringLiteral("Apps 2")}}).isEmpty());
    const QByteArray renamed = fileContents(file);
    QVERIFY(renamed != fedora(second));
    QVERIFY(store.receive(snapshot(fedora(second))).isEmpty());
    QCOMPARE(fileContents(file), renamed);
    QJsonObject third = second;
    third.insert(QStringLiteral("name"), QStringLiteral("Apps 2"));
    QVERIFY(store.receive(snapshot(fedora(third), QStringLiteral("Apps 2")), store.pending().first().toObject().value(QStringLiteral("operation_id")).toString()).isEmpty());
    QCOMPARE(fileContents(file), fedora(third));

    // An edit made outside Thoughts is never replaced here.
    QVERIFY(writeFile(file, fedora(third) + "\n"));
    QJsonObject fourth = third;
    fourth.insert(QStringLiteral("documents"), QJsonArray {});
    fourth.insert(QStringLiteral("rendering"), QJsonObject {{QStringLiteral("x"), QJsonObject {}}});
    QVERIFY(store.receive(snapshot(fedora(fourth), QStringLiteral("Apps 2"))).isEmpty());
    QCOMPARE(fileContents(file), fedora(third) + "\n");

    // Computers left with the manifest "Add project" wrote, from before
    // Thoughts recorded its own copies, take Fedora's.
    const QString catalogue = temporary.filePath(QStringLiteral(".thoughts/library-projects.json"));
    QJsonObject state = QJsonDocument::fromJson(fileContents(catalogue)).object();
    state.remove(QStringLiteral("generated_manifests"));
    QVERIFY(writeFile(catalogue, QJsonDocument(state).toJson()));
    QVERIFY(writeFile(file, created));
    QVERIFY(store.receive(snapshot(fedora(second))).isEmpty());
    QCOMPARE(fileContents(file), fedora(second));
    // But not one for another name.
    state = QJsonDocument::fromJson(fileContents(catalogue)).object();
    state.remove(QStringLiteral("generated_manifests"));
    QVERIFY(writeFile(catalogue, QJsonDocument(state).toJson()));
    const QByteArray other = QJsonDocument(Thoughts::ProjectConfiguration::empty(temporary.path(), id, QStringLiteral("Other"))).toJson();
    QVERIFY(writeFile(file, other));
    QVERIFY(store.receive(snapshot(fedora(second))).isEmpty());
    QCOMPARE(fileContents(file), other);
}

void CommentsOverviewTest::testLibraryRequiresCurrentProjectManifest()
{
    QTemporaryDir root;
    Thoughts::LibraryProjectStore store(root.path());
    const QJsonObject synthetic{{QStringLiteral("id"), QStringLiteral("pdf:paper.pdf")},
        {QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("documents"), QJsonArray{}},
        {QStringLiteral("sources"), QJsonArray{}}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{synthetic}}}).isEmpty());
    QVERIFY(store.projects().isEmpty());
    QVERIFY(!QFileInfo::exists(root.filePath(QStringLiteral(".thoughts/projects"))));
    const auto legacyPath = root.filePath(QStringLiteral("docs-old/thoughts-project.json"));
    QVERIFY(QDir().mkpath(QFileInfo(legacyPath).absolutePath()));
    QFile legacy(legacyPath); QVERIFY(legacy.open(QIODevice::WriteOnly));
    legacy.write("{\"schema_version\":4,\"documents\":[]}"); legacy.close();
    QString legacyError;
    QVERIFY(Thoughts::DocumentBuildSelection::rawRead(legacyPath, &legacyError).isEmpty());
    QVERIFY(!legacyError.isEmpty());
    const QString id = QStringLiteral("new:explicit");
    const auto configuration = Thoughts::ProjectConfiguration::empty(root.path(), id, QStringLiteral("Explicit"));
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), id), configuration).isEmpty());
    const auto projects = store.projects();
    QCOMPARE(projects.size(), 1);
    QCOMPARE(projects.first().toObject().value(QStringLiteral("id")).toString(), id);
    QCOMPARE(projects.first().toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Explicit"));
    Thoughts::CommentsOverview overview(root.path());
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Explicit"));
    auto obsolete = configuration; obsolete.insert(QStringLiteral("schema_version"), 4);
    const auto path = Thoughts::ProjectConfiguration::path(root.path(), id);
    QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(obsolete).toJson()); file.close();
    QVERIFY(store.projects().isEmpty());
    QVERIFY(!store.error().isEmpty());
}

void CommentsOverviewTest::testProjectSizeIncludesAllSourcesAndUpdatesInPlace()
{
    QTemporaryDir temporary;
    const QString root = temporary.path(), id = QStringLiteral("new:size-test");
    auto config = Thoughts::ProjectConfiguration::empty(root, id, QStringLiteral("Sizes"));
    auto folders = config.value(QStringLiteral("source_folders")).toArray();
    const auto defaultOwner = folders.first().toObject().value(QStringLiteral("path")).toString();
    const QString owner = QStringLiteral("docs-sizes");
    folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), owner}});
    config.insert(QStringLiteral("source_folders"), folders);
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root, id), config).isEmpty());
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/.ThoughtIgnore")), QByteArray("excluded.dat\n")));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/unused.bin")), QByteArray(40, 'x')));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/excluded.dat")), QByteArray(500, 'z')));
    QVERIFY(writeFile(temporary.filePath(defaultOwner + QStringLiteral("/extra.html")), QByteArray(20, 'a')));
    const QString pdf = QStringLiteral("final.pdf");
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("current_pdfs/") + pdf), QByteArray(100, 'p')));
    Thoughts::LibraryProjectStore store(root);
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{QJsonObject{{QStringLiteral("id"), id},
        {QStringLiteral("target"), pdf}, {QStringLiteral("pdf_size_bytes"), 9999}, {QStringLiteral("documents"), QJsonArray{}}}}}}).isEmpty());
    Thoughts::LibrarySourceInventory inventory(root);
    const auto a = inventory.refresh(defaultOwner), b = inventory.refresh(owner);
    inventory.save({defaultOwner, owner});
    const auto expected = 100 + a.value(QStringLiteral("size_bytes")).toInteger() + b.value(QStringLiteral("size_bytes")).toInteger();
    Thoughts::CommentsOverview overview(root);
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *project = tree->topLevelItem(0);
    QCOMPARE(project->data(3, Qt::UserRole + 100).toLongLong(), expected);
    QVERIFY(project->text(3).contains(QStringLiteral("cached")));
    // A source monitoring failure cannot invalidate source/PDF metadata sizes.
    overview.applySourceMonitoringInformation(temporary.filePath(QStringLiteral("current_pdfs/") + pdf),
        {{QStringLiteral("status"), QStringLiteral("unavailable")}});
    project = tree->topLevelItem(0);
    QCOMPARE(project->data(3, Qt::UserRole + 100).toLongLong(), expected);
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/unused.bin")), QByteArray(70, 'x')));
    overview.m_librarySourceInventory.insert(defaultOwner, inventory.refresh(defaultOwner));
    overview.m_librarySourceInventory.insert(owner, inventory.refresh(owner));
    overview.updateLibrarySizes();
    QCOMPARE(tree->topLevelItem(0), project);
    QCOMPARE(project->data(3, Qt::UserRole + 100).toLongLong(), expected + 30);
    QVERIFY(!project->text(3).contains(QStringLiteral("cached")));
    // The final PDF is counted once, independently of component estimates.
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("current_pdfs/") + pdf), QByteArray(120, 'p')));
    overview.updateLibrarySizes();
    QCOMPARE(project->data(3, Qt::UserRole + 100).toLongLong(), expected + 50);
    // Multiple imported PDFs can exist before a final concatenation is published.
    QJsonArray declarations, catalogue;
    for (const auto &name : {QStringLiteral("one.pdf"), QStringLiteral("two.pdf")}) {
        const auto documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QVERIFY(writeFile(temporary.filePath(defaultOwner + QLatin1Char('/') + name), QByteArray(10, 'p')));
        declarations.append(QJsonObject{{QStringLiteral("document_id"), documentId}, {QStringLiteral("format"), QStringLiteral("pdf")},
            {QStringLiteral("included"), true}, {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), config.value(QStringLiteral("default_folder_id"))}, {QStringLiteral("path"), name}}}});
        catalogue.append(QJsonObject{{QStringLiteral("document_id"), documentId}, {QStringLiteral("filename"), name}, {QStringLiteral("size_bytes"), 10}});
    }
    config.insert(QStringLiteral("documents"), declarations);
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root, id), config).isEmpty());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{QJsonObject{{QStringLiteral("id"), id},
        {QStringLiteral("target"), QString()}, {QStringLiteral("documents"), catalogue}}}}}).isEmpty());
    const auto finalDefault = inventory.refresh(defaultOwner);
    overview.m_librarySourceInventory.insert(defaultOwner, finalDefault);
    overview.updateLibraryTree();
    project = tree->topLevelItem(0);
    QCOMPARE(project->data(3, Qt::UserRole + 100).toLongLong(),
        finalDefault.value(QStringLiteral("size_bytes")).toInteger() + inventory.refresh(owner).value(QStringLiteral("size_bytes")).toInteger());
    QVERIFY(project->text(3).contains(QStringLiteral("PDF not built")));
}

void CommentsOverviewTest::testEmptyLibraryProjectPersistence()
{
    QTemporaryDir root;
    QVERIFY(root.isValid());
    Thoughts::LibraryProjectStore store(root.path());
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:test")}, {QStringLiteral("revision"), 0}};
    QVERIFY(store.enqueue(QStringLiteral("create"), project, {{QStringLiteral("name"), QStringLiteral("Reading")}}).isEmpty());
    QFile manifest(store.projectManifestPath(QStringLiteral("new:test")));
    QVERIFY(manifest.open(QIODevice::ReadOnly));
    const auto initial = QJsonDocument::fromJson(manifest.readAll()).object();
    // Windows cannot replace a file that is still open.
    manifest.close();
    QCOMPARE(initial.value(QStringLiteral("name")).toString(), QStringLiteral("Reading"));
    QVERIFY(initial.value(QStringLiteral("documents")).toArray().isEmpty());
    auto projects = Thoughts::LibraryProjectStore(root.path()).projects();
    QCOMPARE(projects.size(), 1);
    QCOMPARE(projects[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Reading"));
    QCOMPARE(projects[0].toObject().value(QStringLiteral("documents")).toArray().size(), 0);
    const QString renameError = store.enqueue(QStringLiteral("rename"), projects[0].toObject(), {{QStringLiteral("name"), QStringLiteral("Papers")}});
    QVERIFY2(renameError.isEmpty(), qPrintable(renameError));
    const auto pending = store.pending();
    QCOMPARE(pending[1].toObject().value(QStringLiteral("base_revision")).toInt(), 1);
    QJsonObject acknowledged = projects[0].toObject();
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {acknowledged}}}, pending[0].toObject().value(QStringLiteral("operation_id")).toString()).isEmpty());
    QCOMPARE(store.pending().size(), 1);
    QCOMPARE(store.projects()[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Papers"));
    {
        Thoughts::CommentsOverview overview(root.path());
        auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
        QVERIFY(tree);
        QTRY_COMPARE(tree->topLevelItemCount(), 1);
        QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole + 114).toString(), QStringLiteral("Project"));
        QCOMPARE(tree->topLevelItem(0)->child(0)->childCount(), 0);
    }
    QFile corrupt(store.path()); QVERIFY(corrupt.open(QIODevice::WriteOnly)); corrupt.write("broken"); corrupt.close();
    QVERIFY(!store.enqueue(QStringLiteral("create"), project).isEmpty());
    QVERIFY(corrupt.open(QIODevice::ReadOnly)); QCOMPARE(corrupt.readAll(), QByteArray("broken"));
}

void CommentsOverviewTest::testWebsiteImportPersistence()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString pdf = temporary.filePath(QStringLiteral("download.pdf"));
    { QPdfWriter writer(pdf); QPainter painter(&writer); painter.drawText(100, 100, QStringLiteral("Paper")); }
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:course")}, {QStringLiteral("name"), QStringLiteral("Course")}, {QStringLiteral("revision"), 1}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path());
    QSignalSpy changed(&overview, &Thoughts::CommentsOverview::projectAttachmentChangedLocally);
    auto refreshed = project;
    refreshed.insert(QStringLiteral("revision"), 3);
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{refreshed}}}).isEmpty());
    const QUrl link(QStringLiteral("https://arxiv.org/abs/2401.12345"));
    QVERIFY(overview.saveWebsiteImport(project, {}, {}, pdf, QStringLiteral("Chosen title"), link).isEmpty());
    QCOMPARE(changed.size(), 1);
    const auto pending = store.pending();
    QCOMPARE(pending.size(), 1);
    const auto operation = pending.first().toObject();
    QCOMPARE(operation.value(QStringLiteral("action")).toString(), QStringLiteral("import"));
    QCOMPARE(operation.value(QStringLiteral("name")).toString(), QStringLiteral("Chosen title"));
    QCOMPARE(operation.value(QStringLiteral("base_revision")).toInt(), 3);
    const QString staged = operation.value(QStringLiteral("local_path")).toString();
    QCOMPARE(readFile(staged), readFile(pdf));
    // The tab menu offers the link for the imported PDF, and only for it.
    QCOMPARE(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), operation.value(QStringLiteral("filename")).toString()), link);
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("other.pdf")).isEmpty());
    QVERIFY(QFile::remove(pdf));
    QVERIFY(QFileInfo::exists(staged));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("docs-course/tex/source/main.tex")), QByteArrayLiteral("Source.\n")));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("docs-course/tex/docs_order_manifest.txt")), QByteArrayLiteral("main.tex\n")));
    QVERIFY(registerSourceProject(temporary.path(), QStringLiteral("source:docs-course"), QStringLiteral("Course sources"), {QStringLiteral("docs-course")}));
    QVERIFY(overview.saveWebsiteImport({}, QStringLiteral("docs-course"), QStringLiteral("course.pdf"), staged, QStringLiteral("Attached title"), link).isEmpty());
    const auto attachments = Thoughts::loadProjectAttachments(temporary.path(), QStringLiteral("docs-course"));
    QCOMPARE(attachments.size(), 1);
    QCOMPARE(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), attachments.first().filename), link);
    QCOMPARE(attachments.first().title, QStringLiteral("Attached title"));
    QCOMPARE(readFile(defaultSourceFolder(temporary.path(), QStringLiteral("source:docs-course")) + QLatin1Char('/') + attachments.first().filename), readFile(staged));
    QCOMPARE(changed.size(), 2);
    QCOMPARE(changed.last().first().toString(), QStringLiteral("course.pdf"));
    const QJsonObject missing {{QStringLiteral("id"), QStringLiteral("new:missing")}};
    QVERIFY(!overview.saveWebsiteImport(missing, {}, {}, staged, QStringLiteral("Missing destination")).isEmpty());
    QCOMPARE(store.pending().size(), 1);
}

void CommentsOverviewTest::testServerImportSourceLink()
{
    // A PDF Fedora downloaded from a URL (import_pdf) has no entry in this
    // computer's document-links.json; its /v1/library entry keeps the URL as
    // metadata.source_url, and the installed generation lists that entry.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QUrl serverLink(QStringLiteral("https://arxiv.org/pdf/1706.03762"));
    const QUrl ownLink(QStringLiteral("https://example.org/own.pdf"));
    const auto publication = [](const QString &filename, const QJsonValue &sourceUrl) {
        QJsonObject metadata {{QStringLiteral("filename"), filename}, {QStringLiteral("kind"), QStringLiteral("unlinked")}, {QStringLiteral("archived"), false}};
        if (!sourceUrl.isUndefined()) metadata.insert(QStringLiteral("source_url"), sourceUrl);
        return QJsonObject {{QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("metadata"), metadata}};
    };
    const QJsonObject generation {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("publications"), QJsonArray {
            publication(QStringLiteral("Agent_paper_0123456789ab_89abcdef.pdf"), serverLink.toString()),
            publication(QStringLiteral("Imported_here.pdf"), QStringLiteral("https://example.org/server.pdf")),
            publication(QStringLiteral("Plain.pdf"), QJsonValue::Undefined),
            publication(QStringLiteral("Local_file.pdf"), QStringLiteral("file:///etc/hosts")),
            publication(QStringLiteral("Script.pdf"), QStringLiteral("javascript:alert(1)")),
            publication(QStringLiteral("Not_text.pdf"), 42),
        }},
    };
    const QString generationPath = temporary.filePath(QStringLiteral("current_pdfs/.thoughts-generation.json"));
    // Neither file yet: no link.
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Agent_paper_0123456789ab_89abcdef.pdf")).isEmpty());
    QVERIFY(writeFile(generationPath, QJsonDocument(generation).toJson()));
    QCOMPARE(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Agent_paper_0123456789ab_89abcdef.pdf")), serverLink);
    // Only http and https links, and only for the PDF listed.
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Plain.pdf")).isEmpty());
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Local_file.pdf")).isEmpty());
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Script.pdf")).isEmpty());
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Not_text.pdf")).isEmpty());
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("other.pdf")).isEmpty());
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QString()).isEmpty());
    // This computer's own link import comes first; for other PDFs, or an
    // entry it cannot use, the Library entry still answers.
    QVERIFY(Thoughts::LibraryProjectStore::recordSourceLink(temporary.path(), QStringLiteral("Imported_here.pdf"), ownLink));
    QVERIFY(Thoughts::LibraryProjectStore::recordSourceLink(temporary.path(), QStringLiteral("Agent_paper_0123456789ab_89abcdef.pdf"), QUrl(QStringLiteral("ftp://example.org/a.pdf"))));
    QCOMPARE(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Imported_here.pdf")), ownLink);
    QCOMPARE(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Agent_paper_0123456789ab_89abcdef.pdf")), serverLink);
    // A damaged generation file gives no server link but keeps the own one.
    QVERIFY(writeFile(generationPath, QByteArrayLiteral("broken")));
    QVERIFY(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Agent_paper_0123456789ab_89abcdef.pdf")).isEmpty());
    QCOMPARE(Thoughts::LibraryProjectStore::sourceLinkForPdf(temporary.path(), QStringLiteral("Imported_here.pdf")), ownLink);
}

void CommentsOverviewTest::testWebsiteImportMenu_data()
{
    QTest::addColumn<int>("kind");
    QTest::newRow("library-project") << 0;
    QTest::newRow("source-catalogue-project") << 1;
    QTest::newRow("local-source-project") << 2;
}

void CommentsOverviewTest::testWebsiteImportMenu()
{
    QFETCH(int, kind);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::CommentsOverview overview(temporary.path());
    QTreeWidget tree;
    auto *project = new QTreeWidgetItem(&tree, {QStringLiteral("Course")});
    if (kind < 2) {
        const QJsonObject data {{QStringLiteral("id"), QStringLiteral("new:course")}, {QStringLiteral("name"), QStringLiteral("Course")},
                               {QStringLiteral("source_project"), kind == 1 ? QStringLiteral("docs-course") : QString()}};
        project->setData(0, Qt::UserRole + 140, data);
    } else {
        project->setToolTip(0, QStringLiteral("docs-course"));
        project->setData(0, Qt::UserRole + 103, QStringLiteral("source_linked"));
    }
    auto *documents = new QTreeWidgetItem(project, {QStringLiteral("Documents")});
    documents->setData(0, Qt::UserRole + 107, QStringLiteral("course/documents"));
    tree.show(); tree.expandAll();
    bool opened = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QTimer::singleShot(3000, menu, &QMenu::close);
        auto *import = menu->findChild<QAction *>(QStringLiteral("libraryImportAction"));
        QVERIFY(import);
        QCOMPARE(import->text(), QStringLiteral("Import…"));
        QCOMPARE(menu->actions().first()->text(), QStringLiteral("Manage documents…"));
        tree.clear();
        QTimer::singleShot(0, &overview, [&] {
            auto *chooser = overview.findChild<QDialog *>(QStringLiteral("documentImportDialog"));
            QVERIFY(chooser);
            // A pasted link goes straight to the link importer, already filled in.
            auto *link = chooser->findChild<QLineEdit *>(QStringLiteral("documentImportLink"));
            auto *importLink = chooser->findChild<QPushButton *>(QStringLiteral("documentImportLinkButton"));
            QVERIFY(link && importLink);
            QVERIFY(chooser->findChild<QPushButton *>(QStringLiteral("documentImportPdfButton")));
            link->setText(QStringLiteral("https://arxiv.org/pdf/2609.37633"));
            QVERIFY(importLink->isEnabled());
            QTimer::singleShot(0, &overview, [&] {
                auto *dialog = overview.findChild<QDialog *>(QStringLiteral("websiteImportDialog"));
                QVERIFY(dialog);
                opened = true;
                QCOMPARE(dialog->findChild<QLineEdit *>(QStringLiteral("websiteImportUrl"))->text(), QStringLiteral("https://arxiv.org/pdf/2609.37633"));
                dialog->reject();
            });
            importLink->click();
        });
        import->trigger();
        menu->close();
    });
    overview.showLibraryItemContextMenu(documents, &tree, tree.visualItemRect(documents).center(), true);
    QVERIFY(opened);
    QVERIFY(Thoughts::LibraryProjectStore(temporary.path()).pending().isEmpty());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral(".thoughts/project-imports"))));
}

void CommentsOverviewTest::testPdfOnlyDocumentActions()
{
    QTemporaryDir temporary;
    const QJsonArray docs{QJsonObject{{QStringLiteral("id"), QStringLiteral("a.pdf")}, {QStringLiteral("filename"), QStringLiteral("a.pdf")}, {QStringLiteral("name"), QStringLiteral("A")}, {QStringLiteral("imported"), true}},
                          QJsonObject{{QStringLiteral("id"), QStringLiteral("b.pdf")}, {QStringLiteral("filename"), QStringLiteral("b.pdf")}, {QStringLiteral("name"), QStringLiteral("B")}, {QStringLiteral("imported"), true}}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:reading")}, {QStringLiteral("name"), QStringLiteral("Reading")}, {QStringLiteral("revision"), 1}, {QStringLiteral("documents"), docs}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    const QString path = Thoughts::ProjectConfiguration::path(temporary.path(), project.value(QStringLiteral("id")).toString());
    const QByteArray initial = readFile(path);
    Thoughts::CommentsOverview overview(temporary.path());
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("thoughtsManageDocumentsDialog")); QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { dialog->reject(); });
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("manageDocumentsList")); QVERIFY(list);
        QCOMPARE(list->count(), 2);
        // PDFs share the Project's document order and inclusion.
        QVERIFY(list->item(0)->flags() & Qt::ItemIsUserCheckable);
        list->insertItem(0, list->takeItem(1));
    });
    overview.manageDocuments(project);
    QCOMPARE(readFile(path), initial);
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("thoughtsManageDocumentsDialog")); QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("manageDocumentsList")); QVERIFY(list);
        list->insertItem(0, list->takeItem(1));
        auto *apply = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
        QVERIFY(apply->isEnabled()); apply->click();
    });
    overview.manageDocuments(project);
    // Apply saves the order in the Project configuration; Sync publishes it.
    QVERIFY(store.pending().isEmpty());
    QStringList order;
    for (const auto &value : Thoughts::ProjectConfiguration::read(path).value(QStringLiteral("documents")).toArray())
        order.append(value.toObject().value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString());
    QCOMPARE(order, QStringList({QStringLiteral("b.pdf"), QStringLiteral("a.pdf")}));
}

void CommentsOverviewTest::testMoveDocumentDialog()
{
    QTemporaryDir temporary;
    const QJsonObject doc{{QStringLiteral("id"), QStringLiteral("a.pdf")}, {QStringLiteral("filename"), QStringLiteral("a.pdf")}, {QStringLiteral("name"), QStringLiteral("A")}, {QStringLiteral("imported"), true}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:reading")}, {QStringLiteral("name"), QStringLiteral("Reading")}, {QStringLiteral("revision"), 1}, {QStringLiteral("documents"), QJsonArray{doc}}};
    const QJsonObject destination{{QStringLiteral("id"), QStringLiteral("new:other")}, {QStringLiteral("name"), QStringLiteral("Other")}, {QStringLiteral("revision"), 2}, {QStringLiteral("documents"), QJsonArray{}}};
    const QJsonObject duplicate{{QStringLiteral("id"), QStringLiteral("new:duplicate")}, {QStringLiteral("name"), QStringLiteral("Duplicate")}, {QStringLiteral("revision"), 1}, {QStringLiteral("documents"), QJsonArray{doc}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project, destination, duplicate}}}).isEmpty());
    for (const auto &configured : {project, destination, duplicate}) QVERIFY(registerCatalogueProject(temporary.path(), configured));
    Thoughts::CommentsOverview overview(temporary.path());
    // Projects are listed by configuration location, so choose destinations by name.
    const auto choose = [](QDialog *dialog, const QString &name) {
        auto *choices = dialog->findChild<QComboBox *>(QStringLiteral("moveDocumentDestination"));
        choices->setCurrentIndex(choices->findText(name));
        return choices->currentText() == name;
    };
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("moveDocumentDialog")); QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { dialog->reject(); });
        auto *choices = dialog->findChild<QComboBox *>(QStringLiteral("moveDocumentDestination")); QVERIFY(choices);
        QCOMPARE(choices->count(), 2);
        QVERIFY(choose(dialog, QStringLiteral("Duplicate")));
        QVERIFY(!dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->isEnabled());
    });
    overview.moveLibraryDocument(project, doc);
    QVERIFY(store.pending().isEmpty());
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("moveDocumentDialog")); QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
        QVERIFY(choose(dialog, QStringLiteral("Other")));
        const auto screenshot = qEnvironmentVariable("THOUGHTS_MOVE_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
    });
    overview.moveLibraryDocument(project, doc);
    QCOMPARE(store.pending().size(), 1);
    const auto move = store.pending().first().toObject();
    QCOMPARE(move.value(QStringLiteral("action")).toString(), QStringLiteral("move_document"));
    QCOMPARE(move.value(QStringLiteral("destination_project_id")).toString(), QStringLiteral("new:other"));
    QCOMPARE(move.value(QStringLiteral("destination_revision")).toInt(), 2);
    const auto projected = store.projects();
    const auto find = [](const QJsonArray &projects, const QJsonObject &project) {
        for (const auto &value : projects)
            if (value.toObject().value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) return value.toObject();
        return QJsonObject();
    };
    QCOMPARE(find(projected, project).value(QStringLiteral("documents")).toArray().size(), 0);
    QCOMPARE(find(projected, destination).value(QStringLiteral("documents")).toArray().size(), 1);
    QCOMPARE(find(projected, destination).value(QStringLiteral("revision")).toInt(), 3);
    // Sync delivers both Project configurations with the declaration moved.
    QVERIFY(registerCatalogueProject(temporary.path(), QJsonObject{{QStringLiteral("id"), project.value(QStringLiteral("id"))}, {QStringLiteral("name"), project.value(QStringLiteral("name"))}}));
    QVERIFY(registerCatalogueProject(temporary.path(), QJsonObject{{QStringLiteral("id"), destination.value(QStringLiteral("id"))},
        {QStringLiteral("name"), destination.value(QStringLiteral("name"))}, {QStringLiteral("documents"), QJsonArray{doc}}}));
    QVERIFY(store.receive({{QStringLiteral("projects"), projected}}, move.value(QStringLiteral("operation_id")).toString()).isEmpty());
    QVERIFY(store.pending().isEmpty());
    QCOMPARE(find(store.projects(), destination).value(QStringLiteral("documents")).toArray().size(), 1);
    QCOMPARE(find(store.projects(), project).value(QStringLiteral("documents")).toArray().size(), 0);
}

void CommentsOverviewTest::testUnlinkedDocumentMenu()
{
    QTemporaryDir temporary;
    Thoughts::CommentsOverview overview(temporary.path());
    QTreeWidget tree;
    auto *project = new QTreeWidgetItem(&tree, {QStringLiteral("Reading")});
    project->setData(0, Qt::UserRole + 140, QJsonObject{{QStringLiteral("id"), QStringLiteral("source:docs-reading")}, {QStringLiteral("source_project"), QStringLiteral("docs-reading")}});
    auto *documents = new QTreeWidgetItem(project, {QStringLiteral("Documents")});
    auto *pdf = new QTreeWidgetItem(documents, {QStringLiteral("Paper")});
    pdf->setData(0, Qt::UserRole + 141, QJsonObject{{QStringLiteral("id"), QStringLiteral("third_party/paper.pdf")}, {QStringLiteral("imported"), true}});
    tree.show(); tree.expandAll();
    bool checked = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
        QStringList labels;
        for (auto *action : menu->actions()) labels.append(action->text());
        QCOMPARE(labels, QStringList({QStringLiteral("Open PDF"), QStringLiteral("Rename…"), QStringLiteral("Remove…"), QStringLiteral("Move PDF to different project")}));
        checked = true; menu->close();
    });
    overview.showLibraryItemContextMenu(pdf, &tree, {}, true);
    QVERIFY(checked);
}

void CommentsOverviewTest::testDocumentPublicationMenu()
{
    // protocol/project-document-publication.md: Unpublish/Publish only when the
    // server lists unpublished_documents; an unpublished document does not open.
    QTemporaryDir temporary;
    Thoughts::CommentsOverview overview(temporary.path());
    QTreeWidget tree;
    auto *project = new QTreeWidgetItem(&tree, {QStringLiteral("Reading")});
    project->setData(0, Qt::UserRole + 140, QJsonObject{{QStringLiteral("id"), QStringLiteral("new:reading")}, {QStringLiteral("unpublished_documents"), QJsonArray{}}});
    auto *documents = new QTreeWidgetItem(project, {QStringLiteral("Documents")});
    auto *shown = new QTreeWidgetItem(documents, {QStringLiteral("Shown")});
    shown->setData(0, Qt::UserRole + 141, QJsonObject{{QStringLiteral("id"), QStringLiteral("shown.pdf")}, {QStringLiteral("imported"), true}});
    shown->setData(0, Qt::UserRole + 109, 1);
    shown->setData(0, Qt::UserRole, temporary.filePath(QStringLiteral("shown.pdf")));
    auto *hidden = new QTreeWidgetItem(documents, {QStringLiteral("Hidden")});
    hidden->setData(0, Qt::UserRole + 141, QJsonObject{{QStringLiteral("id"), QStringLiteral("hidden.pdf")}, {QStringLiteral("imported"), true}, {QStringLiteral("published"), false}});
    hidden->setData(0, Qt::UserRole + 162, true);
    tree.show(); tree.expandAll();
    const auto labelsOf = [&](QTreeWidgetItem *item) {
        QStringList labels;
        bool openEnabled = false;
        QTimer::singleShot(0, &overview, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            for (auto *action : menu->actions()) {
                labels.append(action->text());
                if (action->text() == QLatin1String("Open PDF")) openEnabled = action->isEnabled();
            }
            menu->close();
        });
        overview.showLibraryItemContextMenu(item, &tree, {}, true);
        return std::pair{labels, openEnabled};
    };
    const auto [published, publishedOpens] = labelsOf(shown);
    QVERIFY2(published.contains(QStringLiteral("Unpublish")) && published.contains(QStringLiteral("Remove…")), qPrintable(published.join(QLatin1Char('|'))));
    QVERIFY(publishedOpens);
    const auto [unpublished, unpublishedOpens] = labelsOf(hidden);
    QVERIFY2(unpublished.contains(QStringLiteral("Publish")) && !unpublished.contains(QStringLiteral("Unpublish")), qPrintable(unpublished.join(QLatin1Char('|'))));
    QVERIFY(!unpublishedOpens);
    // An older server: no Publish/Unpublish at all.
    project->setData(0, Qt::UserRole + 140, QJsonObject{{QStringLiteral("id"), QStringLiteral("new:reading")}});
    const auto [older, olderOpens] = labelsOf(shown);
    Q_UNUSED(olderOpens)
    QVERIFY(!older.contains(QStringLiteral("Unpublish")) && !older.contains(QStringLiteral("Publish")));
}

void CommentsOverviewTest::testNavigationUsesPreparedSettings()
{
    QTemporaryDir root;
    Thoughts::CommentsOverview overview(root.path());
    overview.m_catalogPollTimer->stop();
    const auto manifest = Thoughts::ProjectConfiguration::empty(root.path(), QStringLiteral("navigation"), QStringLiteral("Disk name"));
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root.path(), QStringLiteral("navigation")), manifest).isEmpty());
    Thoughts::TrackedCommentDocument document;
    document.stem = QStringLiteral("paper");
    document.displayName = QStringLiteral("Paper");
    document.libraryProject = QStringLiteral("docs-paper");
    document.navigationProjectId = QStringLiteral("navigation");
    document.navigationProjectName = QStringLiteral("Prepared project name");
    const auto first = QStringLiteral("docs-paper/tex/source/first.tex");
    const auto second = QStringLiteral("docs-paper/tex/source/second.tex");
    document.navigationOptions = {{QStringLiteral("selected_documents"), QJsonArray{first}},
        {QStringLiteral("titles"), QJsonObject{{first, QStringLiteral("Prepared document title")}}}};
    document.librarySegments = {QJsonObject{{QStringLiteral("source_id"), first}, {QStringLiteral("start_page"), 1}},
                               QJsonObject{{QStringLiteral("source_id"), second}, {QStringLiteral("start_page"), 2}}};
    overview.m_documents = {document};
    const auto reads = Thoughts::ProjectConfiguration::diskReadCount();
    overview.populateDocumentList({});
    QCOMPARE(Thoughts::ProjectConfiguration::diskReadCount(), reads);
    const auto matches = [&](const QString &text) { return overview.m_documentList->findItems(text, Qt::MatchExactly | Qt::MatchRecursive).size(); };
    QCOMPARE(matches(QStringLiteral("Prepared project name")), 1);
    QCOMPARE(matches(QStringLiteral("Prepared document title")), 1);
    QVERIFY(overview.m_documentList->findItems(QStringLiteral("second"), Qt::MatchContains | Qt::MatchRecursive).isEmpty());
}

void CommentsOverviewTest::testSettingsChangeRejectsPendingReload()
{
    QTemporaryDir root;
    auto manifest = Thoughts::ProjectConfiguration::empty(root.path(), QStringLiteral("stale"), QStringLiteral("Before"));
    const auto path = Thoughts::ProjectConfiguration::path(root.path(), QStringLiteral("stale"));
    QVERIFY(Thoughts::ProjectConfiguration::save(path, manifest).isEmpty());
    Thoughts::CommentsOverview overview(root.path());
    overview.m_catalogPollTimer->stop();
    const auto identity = Thoughts::ProjectConfiguration::snapshotIdentity(root.path());
    Thoughts::TrackedCommentDocument visible, stale;
    visible.stem = QStringLiteral("current"); stale.stem = QStringLiteral("obsolete");
    overview.m_documents = {visible};
    manifest.insert(QStringLiteral("name"), QStringLiteral("After"));
    QVERIFY(Thoughts::ProjectConfiguration::save(path, manifest).isEmpty());
    overview.finishFullReload({stale}, true, overview.m_commentGeneration, {}, identity);
    QCOMPARE(overview.m_documents.first().stem, QStringLiteral("current"));
    QVERIFY(overview.m_fullReloadScheduled);
    overview.m_reloadTimer->stop();
}

void CommentsOverviewTest::testDocumentBuildSelection()
{
    QTemporaryDir temporary;
    const QString owner = QStringLiteral("docs-paper");
    const QDir root(temporary.path());
    QVERIFY(QDir().mkpath(root.filePath(owner + QStringLiteral("/tex/source"))));
    const auto source = [&](const QString &name) {
        QFile file(root.filePath(owner + QStringLiteral("/tex/source/") + name));
        QVERIFY(file.open(QIODevice::WriteOnly)); file.write("Original source");
    };
    source(QStringLiteral("one.tex")); source(QStringLiteral("two.tex"));
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("source:docs-paper")}, {QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("source_project"), owner}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    // The Project configuration declares both sources for building.
    auto declared = project;
    declared.insert(QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("id"), QString(owner + QStringLiteral("/tex/source/one.tex"))}},
        QJsonObject{{QStringLiteral("id"), QString(owner + QStringLiteral("/tex/source/two.tex"))}}});
    QVERIFY(registerCatalogueProject(temporary.path(), declared));
    Thoughts::CommentsOverview overview(temporary.path());
    QSignalSpy changed(&overview, &Thoughts::CommentsOverview::projectAttachmentChangedLocally);
    bool saved = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("manageDocumentsList"));
        QVERIFY(list); QCOMPARE(list->count(), 2);
        QCOMPARE(list->item(0)->checkState(), Qt::Checked);
        list->item(1)->setCheckState(Qt::Unchecked);
        const auto screenshot = qEnvironmentVariable("THOUGHTS_SELECTION_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        saved = true;
    });
    overview.manageDocuments(project);
    QVERIFY(saved); QCOMPARE(changed.size(), 1);
    source(QStringLiteral("new.tex"));
    using Thoughts::DocumentBuildSelection;
    const auto options = DocumentBuildSelection::read(DocumentBuildSelection::optionsPath(temporary.path(), owner));
    const auto entries = DocumentBuildSelection::candidates(temporary.path(), owner);
    QCOMPARE(entries.size(), 3);
    int selected = 0;
    for (const auto &entry : entries) selected += DocumentBuildSelection::selected(options, entry.toObject());
    QCOMPARE(selected, 1);
    const auto documents = store.projects().first().toObject().value(QStringLiteral("documents")).toArray();
    QCOMPARE(documents.size(), 1);
    QVERIFY(documents.first().toObject().value(QStringLiteral("pending_build")).toBool());
    QVERIFY(store.pending().isEmpty());
    QFile original(root.filePath(owner + QStringLiteral("/tex/source/two.tex")));
    QVERIFY(original.open(QIODevice::ReadOnly)); QCOMPARE(original.readAll(), QByteArray("Original source"));
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree); QCOMPARE(tree->topLevelItemCount(), 1);
    QVERIFY(tree->topLevelItem(0)->child(0));
    QCOMPARE(tree->topLevelItem(0)->child(0)->childCount(), 1);
    auto *row = tree->topLevelItem(0)->child(0)->child(0);
    QCOMPARE(row->data(0, Qt::UserRole + 114).toString(), QStringLiteral("Not built yet"));
    QVERIFY(row->data(0, Qt::UserRole).toString().isEmpty());
    bool menuChecked = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        const auto actions = menu->actions();
        for (int i = 0; i < actions.size(); ++i) if (actions[i]->objectName() == QStringLiteral("libraryManageDocumentsAction")) {
            QVERIFY(actions[i]->isEnabled());
            QCOMPARE(actions[i]->text(), QStringLiteral("Manage documents…"));
            for (auto *action : actions) QVERIFY(action->text() != QStringLiteral("Change order…"));
            menuChecked = true;
        }
        menu->close();
    });
    overview.showLibraryItemContextMenu(tree->topLevelItem(0)->child(0), tree, {}, true);
    QVERIFY(menuChecked);
    const QJsonArray selectedIds{QStringLiteral("docs-paper/tex/source/one.tex"), QStringLiteral("docs-paper/tex/source/new.tex")};
    QVERIFY(DocumentBuildSelection::save(temporary.path(), owner, entries, selectedIds).isEmpty());
    // Newly selected sources join the Project configuration with their own identity.
    QString configurationError;
    QCOMPARE(Thoughts::ProjectConfiguration::read(Thoughts::ProjectConfiguration::path(temporary.path(), project.value(QStringLiteral("id")).toString()), &configurationError)
        .value(QStringLiteral("documents")).toArray().size(), 3);
    QVERIFY2(configurationError.isEmpty(), qPrintable(configurationError));
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
        auto *list = dialog->findChild<QListWidget *>(); QVERIFY(list); QCOMPARE(list->count(), 3);
        list->insertItem(0, list->takeItem(2));
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
    });
    overview.editLibraryProject(store.projects().first().toObject(), QStringLiteral("order"), {});
    QVERIFY(store.pending().isEmpty());
    const auto ordered = store.projects().first().toObject().value(QStringLiteral("documents")).toArray();
    QCOMPARE(ordered.first().toObject().value(QStringLiteral("id")).toString(), QStringLiteral("docs-paper/tex/source/new.tex"));

    const QString markdownOwner = QStringLiteral("docs-markdown");
    const auto markdownRoot = root.filePath(markdownOwner + QStringLiteral("/markdown/source"));
    QVERIFY(QDir().mkpath(markdownRoot));
    QVERIFY(writeFile(QDir(markdownRoot).filePath(QStringLiteral("first.md")), QByteArrayLiteral("# First\n")));
    QVERIFY(writeFile(QDir(markdownRoot).filePath(QStringLiteral("new.md")), QByteArrayLiteral("# New\n")));
    const QString manifestPath = root.filePath(markdownOwner + QStringLiteral("/thoughts-project.json"));
    QVERIFY(registerCatalogueProject(temporary.path(), {{QStringLiteral("id"), QStringLiteral("source:docs-markdown")}, {QStringLiteral("name"), QStringLiteral("Markdown")},
        {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("id"), QString(markdownOwner + QStringLiteral("/markdown/source/first.md"))}}}}}));
    const auto markdownEntries = DocumentBuildSelection::candidates(temporary.path(), markdownOwner);
    QCOMPARE(markdownEntries.size(), 2);
    const QJsonArray markdownSelected{QStringLiteral("docs-markdown/markdown/source/new.md")};
    QVERIFY(DocumentBuildSelection::save(temporary.path(), markdownOwner, markdownEntries, markdownSelected).isEmpty());
    QCOMPARE(DocumentBuildSelection::read(manifestPath).value(QStringLiteral("documents")).toArray().size(), 2);
    QCOMPARE(DocumentBuildSelection::read(DocumentBuildSelection::optionsPath(temporary.path(), markdownOwner)).value(QStringLiteral("selected_documents")).toArray(), markdownSelected);
}

void CommentsOverviewTest::testManageDocuments()
{
    QTemporaryDir temporary;
    using Thoughts::ProjectConfiguration;
    const QString owner = QStringLiteral("docs-paper");
    const QString first = owner + QStringLiteral("/tex/source/first.tex");
    const QString second = owner + QStringLiteral("/tex/source/second.tex");
    QVERIFY(writeFile(temporary.filePath(first), "First"));
    QVERIFY(writeFile(temporary.filePath(second), "Second"));
    // The Project declares and names the first source; the second is only discovered.
    const QString firstId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:paper")}, {QStringLiteral("name"), QStringLiteral("Paper")},
        {QStringLiteral("source_project"), owner}, {QStringLiteral("documents"), QJsonArray{
        QJsonObject{{QStringLiteral("id"), first}, {QStringLiteral("document_id"), firstId}}}}};
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    const QString path = ProjectConfiguration::path(temporary.path(), project.value(QStringLiteral("id")).toString());
    auto configuration = ProjectConfiguration::read(path);
    configuration.insert(QStringLiteral("document_names"), QJsonObject{{firstId, QStringLiteral("Renamed first document")}});
    QVERIFY(ProjectConfiguration::save(path, configuration).isEmpty());
    const QByteArray initial = readFile(path);
    const auto order = [&] {
        QStringList result;
        for (const auto &value : ProjectConfiguration::read(path).value(QStringLiteral("documents")).toArray())
            result.append(ProjectConfiguration::documentAlias(ProjectConfiguration::read(path), value.toObject()));
        return result;
    };
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), project);
        dialog.show();
        auto *list = dialog.findChild<QListWidget *>();
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->item(0)->text(), QStringLiteral("Renamed first document"));
        QCOMPARE(list->item(0)->toolTip(), first);
        QCOMPARE(list->item(0)->checkState(), Qt::Checked);
        QCOMPARE(list->item(1)->toolTip(), second);
        QCOMPARE(list->item(1)->checkState(), Qt::Unchecked);
        list->item(1)->setCheckState(Qt::Checked);
        list->setCurrentRow(1);
        dialog.findChild<QPushButton *>(QStringLiteral("documentOrderUp"))->click();
        QCOMPARE(list->item(0)->toolTip(), second);
        QCOMPARE(list->item(0)->checkState(), Qt::Checked);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        QCOMPARE(readFile(path), initial);
    }
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), project);
        dialog.show();
        auto *list = dialog.findChild<QListWidget *>();
        auto *buttons = dialog.findChild<QDialogButtonBox *>();
        QVERIFY(!buttons->button(QDialogButtonBox::Save));
        QCOMPARE(dialog.windowTitle(), QStringLiteral("Manage documents"));
        auto *row = list->item(1);
        QCOMPARE(row->checkState(), Qt::Unchecked);
        // A handle click selects a row without changing its checkbox.
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(12, list->visualItemRect(row).center().y()));
        QCOMPARE(row->checkState(), Qt::Unchecked);
        QCOMPARE(list->currentRow(), 1);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(34, list->visualItemRect(row).center().y()));
        QCOMPARE(row->checkState(), Qt::Checked);
        QTest::mouseClick(list->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(34, list->visualItemRect(row).center().y()));
        QCOMPARE(row->checkState(), Qt::Unchecked);
        dialog.findChild<QPushButton *>(QStringLiteral("documentOrderUp"))->click();
        QCOMPARE(list->item(0), row);
        QCOMPARE(row->checkState(), Qt::Unchecked);
        buttons->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    // Selection and order are saved together in the Project configuration.
    QCOMPARE(order(), (QStringList{second, first}));
    const auto saved = ProjectConfiguration::read(path).value(QStringLiteral("documents")).toArray();
    QVERIFY(!saved.first().toObject().value(QStringLiteral("included")).toBool());
    QVERIFY(!QUuid(saved.first().toObject().value(QStringLiteral("document_id")).toString()).isNull());
    QVERIFY(saved.last().toObject().value(QStringLiteral("included")).toBool());
    QCOMPARE(saved.last().toObject().value(QStringLiteral("document_id")).toString(), firstId);
    QVERIFY(!QFileInfo::exists(Thoughts::DocumentBuildSelection::optionsPath(temporary.path(), owner)));
    QVERIFY(!QFileInfo::exists(temporary.filePath(owner + QStringLiteral("/tex/support/thoughts_document_order.txt"))));
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), project);
        auto *list = dialog.findChild<QListWidget *>();
        QCOMPARE(list->item(0)->toolTip(), second);
        QCOMPARE(list->item(0)->checkState(), Qt::Unchecked);
        list->item(0)->setCheckState(Qt::Checked);
        // A concurrent settings edit must prevent Apply.
        auto concurrent = ProjectConfiguration::read(path);
        concurrent.insert(QStringLiteral("name"), QStringLiteral("Concurrent rename"));
        QVERIFY(ProjectConfiguration::save(path, concurrent).isEmpty());
        const auto concurrentBytes = readFile(path);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(readFile(path), concurrentBytes);
    }
    // Multiple source folders and unchecked candidates share one persisted order.
    const QString otherOwner = QStringLiteral("docs-other");
    const QString other = otherOwner + QStringLiteral("/tex/source/other.tex");
    QVERIFY(writeFile(temporary.filePath(other), "Other"));
    configuration = ProjectConfiguration::read(path);
    auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), otherOwner}});
    configuration.insert(QStringLiteral("source_folders"), folders);
    QVERIFY(ProjectConfiguration::save(path, configuration).isEmpty());
    auto multiple = project;
    multiple.insert(QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), otherOwner}}});
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), multiple);
        auto *list = dialog.findChild<QListWidget *>();
        QCOMPARE(list->count(), 3);
        QCOMPARE(list->item(2)->toolTip(), other);
        list->insertItem(0, list->takeItem(2));
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    QCOMPARE(order(), (QStringList{other, second, first}));
    QCOMPARE(readFile(temporary.filePath(first)), QByteArray("First"));
}

void CommentsOverviewTest::testManageDocumentsReusesRemovedIdentity()
{
    // "Remove document" keeps the file and its permanent identity; Fedora refuses
    // the document's PDF name for any other, so listing it again reuses it.
    QTemporaryDir temporary;
    using Thoughts::ProjectConfiguration;
    const QString owner = QStringLiteral("docs-paper");
    const QString kept = owner + QStringLiteral("/tex/source/kept.tex");
    const QString removed = owner + QStringLiteral("/tex/source/removed.tex");
    QVERIFY(writeFile(temporary.filePath(kept), "Kept"));
    QVERIFY(writeFile(temporary.filePath(removed), "Removed"));
    const QString keptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString removedId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:paper")}, {QStringLiteral("name"), QStringLiteral("Paper")},
        {QStringLiteral("source_project"), owner}, {QStringLiteral("documents"), QJsonArray{
        QJsonObject{{QStringLiteral("id"), kept}, {QStringLiteral("document_id"), keptId}, {QStringLiteral("source_project"), owner}},
        QJsonObject{{QStringLiteral("id"), removed}, {QStringLiteral("document_id"), removedId}, {QStringLiteral("source_project"), owner}}}}};
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral(".thoughts/library-projects.json")),
        QJsonDocument(QJsonObject{{QStringLiteral("projects"), QJsonArray{project}}}).toJson()));
    // Until Fedora confirms the removal, the pending operation keeps the identity.
    QVERIFY(Thoughts::LibraryProjectStore(temporary.path()).enqueue(QStringLiteral("remove_document"), project,
        {{QStringLiteral("document_id"), removed}}).isEmpty());
    const auto pending = Thoughts::LibraryProjectStore(temporary.path()).projects(false);
    QCOMPARE(pending.size(), 1);
    const auto current = pending.first().toObject();
    QCOMPARE(current.value(QStringLiteral("removed_identities")).toObject(), (QJsonObject{{removed, removedId}}));
    const QString path = ProjectConfiguration::path(temporary.path(), project.value(QStringLiteral("id")).toString());
    QCOMPARE(ProjectConfiguration::read(path).value(QStringLiteral("documents")).toArray().size(), 1);
    const auto declared = [&](const QString &alias) {
        const auto configuration = ProjectConfiguration::read(path);
        for (const auto &value : configuration.value(QStringLiteral("documents")).toArray())
            if (ProjectConfiguration::documentAlias(configuration, value.toObject()) == alias) return value.toObject();
        return QJsonObject();
    };
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), current);
        auto *list = dialog.findChild<QListWidget *>();
        QCOMPARE(list->count(), 2);
        QCOMPARE(list->item(1)->toolTip(), removed);
        QCOMPARE(list->item(1)->checkState(), Qt::Unchecked);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    // Left unchecked, it stays removed rather than declared again.
    QVERIFY(declared(removed).isEmpty());
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), current);
        auto *list = dialog.findChild<QListWidget *>();
        QCOMPARE(list->item(1)->toolTip(), removed);
        list->item(1)->setCheckState(Qt::Checked);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    QCOMPARE(declared(removed).value(QStringLiteral("document_id")).toString(), removedId);
    QVERIFY(declared(removed).value(QStringLiteral("included")).toBool());
    // An identity the Project already declares is never handed out twice.
    auto clash = current;
    const QString other = owner + QStringLiteral("/tex/source/other.tex");
    QVERIFY(writeFile(temporary.filePath(other), "Other"));
    clash.insert(QStringLiteral("removed_identities"), QJsonObject{{other, keptId}});
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), clash);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    const QString otherId = declared(other).value(QStringLiteral("document_id")).toString();
    QVERIFY(Thoughts::DocumentIdentityStore::validId(otherId));
    QVERIFY(otherId != keptId);
    QCOMPARE(declared(kept).value(QStringLiteral("document_id")).toString(), keptId);
}

void CommentsOverviewTest::testManageDefaultFolderDocuments()
{
    // Markdown and standalone LaTeX files in a Project's default folder are
    // offered and declared there, with the shared Library PDF name.
    QTemporaryDir temporary;
    using Thoughts::ProjectConfiguration;
    const QString id = QStringLiteral("new:default-folder");
    auto configuration = ProjectConfiguration::empty(temporary.path(), id, QStringLiteral("Default folder"));
    const auto defaultFolderId = configuration.value(QStringLiteral("default_folder_id")).toString();
    const QString imported = QUuid::createUuid().toString(QUuid::WithoutBraces);
    configuration.insert(QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("document_id"), imported},
        {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), defaultFolderId}, {QStringLiteral("path"), QStringLiteral("imported.pdf")}}},
        {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), true}}});
    configuration.insert(QStringLiteral("rendering"), QJsonObject{{imported, QJsonObject{{QStringLiteral("sha256"), QString(64, QLatin1Char('0'))}}}});
    const QString path = ProjectConfiguration::path(temporary.path(), id);
    QVERIFY(ProjectConfiguration::save(path, configuration).isEmpty());
    const QString owner = QFileInfo(path).absolutePath().mid(QDir(temporary.path()).absolutePath().size() + 1);
    QVERIFY(Thoughts::SourceOwner::isDefaultFolder(owner));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/notes.md")), "# Notes\n"));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/chapters/paper.tex")), "\\documentclass{article}\n\\begin{document}Paper\\end{document}\n"));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/imported.pdf")), "%PDF-1.4\n"));
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), QJsonObject{{QStringLiteral("id"), id}});
        auto *list = dialog.findChild<QListWidget *>();
        QCOMPARE(list->count(), 3);
        QStringList tips;
        for (int row = 0; row < list->count(); ++row) tips.append(list->item(row)->toolTip());
        QCOMPARE(tips, (QStringList{QStringLiteral("third_party/imported.pdf"), owner + QStringLiteral("/notes.md"), owner + QStringLiteral("/chapters/paper.tex")}));
        QCOMPARE(list->item(1)->checkState(), Qt::Unchecked);
        list->item(1)->setCheckState(Qt::Checked);
        list->item(2)->setCheckState(Qt::Checked);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    const auto saved = ProjectConfiguration::read(path);
    const auto documents = saved.value(QStringLiteral("documents")).toArray();
    QCOMPARE(documents.size(), 3);
    const auto rendering = saved.value(QStringLiteral("rendering")).toObject();
    for (const auto &[index, source, format] : {std::tuple{1, QStringLiteral("notes.md"), QStringLiteral("markdown")}, std::tuple{2, QStringLiteral("chapters/paper.tex"), QStringLiteral("latex")}}) {
        const auto document = documents.at(index).toObject();
        QCOMPARE(document.value(QStringLiteral("source")).toObject().value(QStringLiteral("folder_id")).toString(), defaultFolderId);
        QCOMPARE(document.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString(), source);
        QCOMPARE(document.value(QStringLiteral("format")).toString(), format);
        QVERIFY(document.value(QStringLiteral("included")).toBool());
        const auto options = rendering.value(document.value(QStringLiteral("document_id")).toString()).toObject();
        QCOMPARE(options.value(QStringLiteral("output")).toString(), Thoughts::SourceOwner::defaultFolderOutput(owner + QLatin1Char('/') + source));
    }
    QCOMPARE(rendering.value(documents.at(2).toObject().value(QStringLiteral("document_id")).toString()).toObject().value(QStringLiteral("working_directory")).toString(), QStringLiteral("chapters"));
    QVERIFY(!rendering.value(documents.at(1).toObject().value(QStringLiteral("document_id")).toString()).toObject().contains(QStringLiteral("working_directory")));
    QCOMPARE(rendering.value(imported).toObject().value(QStringLiteral("sha256")).toString(), QString(64, QLatin1Char('0')));
    // Reopening lists the declared documents once, still checked.
    Thoughts::DocumentManagementDialog dialog(temporary.path(), QJsonObject{{QStringLiteral("id"), id}});
    auto *list = dialog.findChild<QListWidget *>();
    QCOMPARE(list->count(), 3);
    QCOMPARE(list->item(2)->checkState(), Qt::Checked);
}

void CommentsOverviewTest::testSingleProjectDocumentManagement()
{
    QTemporaryDir temporary;
    using Thoughts::ProjectConfiguration;
    const QString id = QStringLiteral("new:central");
    auto configuration = ProjectConfiguration::empty(temporary.path(), id, QStringLiteral("Central"));
    const auto folder = configuration.value(QStringLiteral("default_folder_id"));
    QJsonArray documents;
    for (const auto &name : {QStringLiteral("a.pdf"), QStringLiteral("b.pdf")})
        documents.append(QJsonObject{{QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folder}, {QStringLiteral("path"), name}}},
            {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), true}});
    configuration.insert(QStringLiteral("documents"), documents);
    const auto path = ProjectConfiguration::path(temporary.path(), id);
    QVERIFY(ProjectConfiguration::save(path, configuration).isEmpty());
    const QJsonObject project{{QStringLiteral("id"), id}};
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), project);
        auto *list = dialog.findChild<QListWidget *>(); QCOMPARE(list->count(), 2);
        list->insertItem(0, list->takeItem(1));
        list->item(0)->setCheckState(Qt::Unchecked);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    auto saved = ProjectConfiguration::read(path);
    const auto reordered = saved.value(QStringLiteral("documents")).toArray();
    QCOMPARE(reordered.first().toObject().value(QStringLiteral("document_id")), documents.last().toObject().value(QStringLiteral("document_id")));
    QVERIFY(!reordered.first().toObject().value(QStringLiteral("included")).toBool());
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), project);
        saved.insert(QStringLiteral("name"), QStringLiteral("Concurrent rename"));
        QVERIFY(ProjectConfiguration::save(path, saved).isEmpty());
        const auto concurrent = readFile(path);
        dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply)->click();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(readFile(path), concurrent);
    }
    {
        Thoughts::DocumentManagementDialog dialog(temporary.path(), project);
        auto *list = dialog.findChild<QListWidget *>();
        for (int i = 0; i < list->count(); ++i) list->item(i)->setCheckState(Qt::Unchecked);
        auto *apply = dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Apply);
        QVERIFY(apply->isEnabled()); apply->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }
    for (const auto &d : ProjectConfiguration::read(path).value(QStringLiteral("documents")).toArray())
        QVERIFY(!d.toObject().value(QStringLiteral("included")).toBool());
}

void CommentsOverviewTest::testDocumentSettingsTransaction()
{
    QTemporaryDir temporary;
    const QString first = temporary.filePath(QStringLiteral("a.json"));
    const QString newFile = temporary.filePath(QStringLiteral("b.json"));
    const QString blocked = temporary.filePath(QStringLiteral("z-parent/settings.json"));
    QVERIFY(writeFile(first, "Original bytes\n"));
    // A file in place of the parent directory forces failure after earlier writes.
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("z-parent")), "Block"));
    Thoughts::DocumentSettingsTransaction transaction;
    QVERIFY(transaction.watch(first).isEmpty());
    QVERIFY(transaction.watch(newFile).isEmpty());
    QVERIFY(transaction.watch(blocked).isEmpty());
    const QJsonObject settings{{QStringLiteral("schema_version"), 1}};
    QVERIFY(!transaction.commit({{first, settings}, {newFile, settings}, {blocked, settings}}).isEmpty());
    QCOMPARE(readFile(first), QByteArray("Original bytes\n"));
    QVERIFY(!QFileInfo::exists(newFile));
}

void CommentsOverviewTest::testLibraryDocumentRename()
{
    QTemporaryDir temporary;
    Thoughts::LibraryProjectStore store(temporary.path());
    const QJsonObject doc {{QStringLiteral("id"), QStringLiteral("paper.pdf")}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                          {QStringLiteral("name"), QStringLiteral("Paper")}};
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:reading")}, {QStringLiteral("name"), QStringLiteral("Reading")},
                              {QStringLiteral("documents"), QJsonArray {doc}}, {QStringLiteral("revision"), 1}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *selected = tree->topLevelItem(0)->child(0)->child(0);
    bool renamed = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QTimer::singleShot(3000, menu, &QMenu::close);
        QAction *rename = nullptr;
        for (auto *action : menu->actions()) if (action->text() == QStringLiteral("Rename…")) rename = action;
        QVERIFY(rename);
        QTimer::singleShot(0, &overview, [&] {
            auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
            QVERIFY(dialog);
            QCOMPARE(dialog->textValue(), QStringLiteral("Paper"));
            dialog->setTextValue(QStringLiteral("New title"));
            dialog->accept();
            renamed = true;
        });
        rename->trigger();
        menu->close();
    });
    overview.showLibraryItemContextMenu(selected, tree, {}, true);
    QVERIFY(renamed);
    QCOMPARE(tree->topLevelItem(0)->child(0)->child(0)->text(0), QStringLiteral("New title"));
    const auto pending = store.pending();
    QCOMPARE(pending.size(), 1);
    QCOMPARE(pending.first().toObject().value(QStringLiteral("action")).toString(), QStringLiteral("rename_document"));
    QCOMPARE(pending.first().toObject().value(QStringLiteral("document_id")).toString(), QStringLiteral("paper.pdf"));
}

void CommentsOverviewTest::testLibrarySummarySaysLoadingUntilFirstLoad()
{
    QTemporaryDir temporary;
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("current_pdfs/alpha.pdf")), QByteArrayLiteral("%PDF-1.4\n")));
    Thoughts::CommentsOverview overview(temporary.path());
    auto *summary = overview.findChild<QLabel *>(QStringLiteral("thoughtsOverviewLibrarySummary"));
    QVERIFY(summary);
    QCOMPARE(summary->text(), QStringLiteral("Loading the Library…"));
    overview.show();
    overview.showSummary();
    // A Sync that starts during the first load keeps the window stable: still loading, never zero.
    overview.syncStarted();
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    QCOMPARE(summary->text(), QStringLiteral("Loading the Library…"));
    overview.syncFinished(true, QString());
    QTRY_VERIFY(summary->text() != QStringLiteral("Loading the Library…"));
    QVERIFY(!summary->text().startsWith(QStringLiteral("0 entries")));
}

void CommentsOverviewTest::testProjectMenuCopiesProjectName()
{
    QTemporaryDir temporary;
    Thoughts::LibraryProjectStore store(temporary.path());
    const QJsonObject doc {{QStringLiteral("id"), QStringLiteral("paper.pdf")}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                          {QStringLiteral("name"), QStringLiteral("Paper")}};
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:reading")}, {QStringLiteral("name"), QStringLiteral("Benchmark-Conf")},
                              {QStringLiteral("documents"), QJsonArray {doc}}, {QStringLiteral("revision"), 1}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QGuiApplication::clipboard()->clear();
    QStringList labels;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QTimer::singleShot(3000, menu, &QMenu::close);
        for (auto *action : menu->actions()) labels.append(action->text());
        auto *copy = menu->findChild<QAction *>(QStringLiteral("libraryCopyProjectNameAction"));
        QVERIFY(copy);
        copy->trigger();
        menu->close();
    });
    overview.showLibraryItemContextMenu(tree->topLevelItem(0), tree, {}, true);
    QVERIFY2(labels.indexOf(QStringLiteral("Copy project name")) == labels.indexOf(QStringLiteral("Rename…")) + 1, qPrintable(labels.join(QLatin1Char('|'))));
    QCOMPARE(QGuiApplication::clipboard()->text(), QStringLiteral("Benchmark-Conf"));
}

void CommentsOverviewTest::testLibrarySearchDocumentHitsOpenTheirSearch()
{
    QTemporaryDir root;
    Thoughts::CommentsOverview overview(root.path());
    QSignalSpy opened(&overview, &Thoughts::CommentsOverview::librarySearchPageActivated);

    // Found in its text: opens on the best page, with the search.
    Thoughts::LibrarySearchHit inText;
    inText.kind = QStringLiteral("document");
    inText.pdfPath = QStringLiteral("/library/paper.pdf");
    inText.page = 7;
    inText.matchingPages = 12;
    overview.openLibrarySearchHit(inText, QStringLiteral("BAR"));
    QCOMPARE(opened.count(), 1);
    QCOMPARE(opened.at(0).at(0).toString(), inText.pdfPath);
    QCOMPARE(opened.at(0).at(1).toInt(), 7);
    QCOMPARE(opened.at(0).at(2).toString(), QStringLiteral("BAR"));

    // Found by its name alone: there is nothing to search for inside it.
    Thoughts::LibrarySearchHit byName;
    byName.kind = QStringLiteral("document");
    byName.pdfPath = QStringLiteral("/library/survey.pdf");
    overview.openLibrarySearchHit(byName, QStringLiteral("survey"));
    QCOMPARE(opened.count(), 2);
    QCOMPARE(opened.at(1).at(1).toInt(), 0);
    QVERIFY(opened.at(1).at(2).toString().isEmpty());
}

void CommentsOverviewTest::testLibrarySearchFindsSourceLines()
{
    // The search field's Sources scope indexes the projects' source files on
    // first use and opens a match in the source editor at its line.
    QTemporaryDir temporary;
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/tex/benchmark.tex"), QByteArrayLiteral("\\section{Results}\nThe quokka leaderboard.\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/build/benchmark.tex"), QByteArrayLiteral("quokka build copy\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/.ThoughtIgnore"), QByteArrayLiteral("build/\n")));
    QVERIFY(registerSourceProject(root, QStringLiteral("search-project"), QStringLiteral("Paper"), {QStringLiteral("docs-paper")}));
    KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("LibrarySearch")).deleteEntry("Scope");
    Thoughts::CommentsOverview overview(root);
    overview.resize(1000, 700);
    overview.show();
    QVERIFY(QTest::qWaitForWindowExposed(&overview));
    auto *field = overview.findChild<Thoughts::LibrarySearchField *>(QStringLiteral("librarySearchField"));
    QVERIFY(field);
    QCOMPARE(field->scope(), Thoughts::LibrarySearchScope::Documents);
    QVERIFY(field->placeholderText().contains(QStringLiteral("PDFs")));
    const auto restore = qScopeGuard([field] { field->setScope(Thoughts::LibrarySearchScope::Documents); });
    field->setFocus();
    QTest::keyClick(field, Qt::Key_2, Qt::ControlModifier);
    QCOMPARE(field->scope(), Thoughts::LibrarySearchScope::Sources);
    QVERIFY(field->placeholderText().contains(QStringLiteral("source files")));
    QCOMPARE(KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("LibrarySearch")).readEntry("Scope", QString()), QStringLiteral("sources"));
    QSignalSpy opened(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    QTest::keyClicks(field, QStringLiteral("quokka"));
    QListWidget *results = nullptr;
    const auto rows = [&results] {
        QStringList text;
        for (int row = 0; results && row < results->count(); ++row) text.append(results->item(row)->data(Qt::UserRole + 2).toString());
        return text.join(QLatin1Char('\n'));
    };
    // Results refresh once the first source index is written.
    QTRY_VERIFY_WITH_TIMEOUT((results = overview.findChild<QListWidget *>(QStringLiteral("librarySearchResults"))) && results->isVisible()
                                 && rows().contains(QStringLiteral("Line 2")),
                             15000);
    // The file, its folder and its project; the ignored build copy is not searched.
    QVERIFY(rows().contains(QStringLiteral("benchmark.tex")));
    QVERIFY(rows().contains(QStringLiteral("docs-paper/tex · Paper")));
    QVERIFY(!rows().contains(QStringLiteral("build")));
    QTest::keyClick(field, Qt::Key_Return);
    QCOMPARE(opened.size(), 1);
    QCOMPARE(QFileInfo(opened.first().at(0).toString()).canonicalFilePath(), QFileInfo(root + QStringLiteral("/docs-paper/tex/benchmark.tex")).canonicalFilePath());
    QCOMPARE(opened.first().at(1).toInt(), 2);
    // Documents alone does not show sources.
    QTest::keyClick(field, Qt::Key_1, Qt::ControlModifier);
    QCOMPARE(KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("LibrarySearch")).readEntry("Scope", QString()), QStringLiteral("documents"));
    QVERIFY(!Thoughts::LibrarySearchIndex(overview.m_librarySearchIndexPath).search(QStringLiteral("quokka"), Thoughts::LibrarySearchScope::Documents).size());
}

void CommentsOverviewTest::testLibraryMenuSurvivesRefresh_data()
{
    QTest::addColumn<bool>("removeDocument");
    QTest::newRow("open-pdf") << false;
    QTest::newRow("remove-document") << true;
}

void CommentsOverviewTest::testLibraryDocumentDoubleClickWithoutPage()
{
    // Double-clicking a Project document used to do nothing unless it had a
    // page in the Project reading: a new document opened nothing at all.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString builtPdf = temporary.filePath(QStringLiteral("current_pdfs/document_57e91a9ef12aa950fa73.pdf"));
    QVERIFY(writeFile(builtPdf, QByteArrayLiteral("%PDF-report")));
    const QString source = QStringLiteral(".thoughts/projects/abc/documents/report.tex");
    QVERIFY(writeFile(temporary.filePath(source), QByteArrayLiteral("\\documentclass{article}")));
    Thoughts::CommentsOverview overview(temporary.path());
    auto *tree = overview.m_libraryTree;
    QVERIFY(tree);
    constexpr int pageRole = Qt::UserRole + 109; // LibraryPageRole
    constexpr int documentRole = Qt::UserRole + 141;
    QSignalSpy pdfOpened(&overview, &Thoughts::CommentsOverview::documentActivated);
    QSignalSpy pageOpened(&overview, &Thoughts::CommentsOverview::documentPageActivated);
    QSignalSpy sourceOpened(&overview, &Thoughts::CommentsOverview::openSourceRequested);

    // Built, but not (yet) in the Project reading: open its own PDF.
    auto *built = new QTreeWidgetItem(tree, {QStringLiteral("report")});
    built->setData(0, Qt::UserRole, builtPdf);
    built->setData(0, pageRole, 0);
    built->setData(0, documentRole, QVariant::fromValue(QJsonObject{{QStringLiteral("document_id"), QStringLiteral("bb2c87c2")},
                                                                    {QStringLiteral("source_file"), source}}));
    Q_EMIT tree->itemDoubleClicked(built, 0);
    QCOMPARE(pdfOpened.count(), 1);
    QCOMPARE(pdfOpened.at(0).at(0).toString(), builtPdf);
    QCOMPARE(pageOpened.count(), 0);

    // Checked but not built yet: open its source.
    auto *pending = new QTreeWidgetItem(tree, {QStringLiteral("report")});
    pending->setData(0, Qt::UserRole, QString());
    pending->setData(0, pageRole, 0);
    pending->setData(0, documentRole, QVariant::fromValue(QJsonObject{{QStringLiteral("source_file"), source}, {QStringLiteral("pending_build"), true}}));
    Q_EMIT tree->itemDoubleClicked(pending, 0);
    QCOMPARE(sourceOpened.count(), 1);
    QCOMPARE(sourceOpened.at(0).at(0).toString(), QDir(temporary.path()).filePath(source));

    // A known PDF that has not been downloaded yet also falls back to the source.
    auto *missing = new QTreeWidgetItem(tree, {QStringLiteral("report")});
    missing->setData(0, Qt::UserRole, temporary.filePath(QStringLiteral("current_pdfs/not_downloaded.pdf")));
    missing->setData(0, pageRole, 1);
    missing->setData(0, documentRole, QVariant::fromValue(QJsonObject{{QStringLiteral("source_file"), source}}));
    Q_EMIT tree->itemDoubleClicked(missing, 0);
    QCOMPARE(sourceOpened.count(), 2);
    QCOMPARE(pageOpened.count(), 0);
}

void CommentsOverviewTest::testLibraryMenuSurvivesRefresh()
{
    QFETCH(bool, removeDocument);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString firstPath = temporary.filePath(QStringLiteral("current_pdfs/first.pdf"));
    QVERIFY(writeFile(firstPath, QByteArrayLiteral("%PDF-first")));
    QVERIFY(writeFile(temporary.filePath(QStringLiteral("current_pdfs/second.pdf")), QByteArrayLiteral("%PDF-second")));
    const QJsonObject first {{QStringLiteral("id"), QStringLiteral("first.pdf")}, {QStringLiteral("filename"), QStringLiteral("first.pdf")},
                            {QStringLiteral("name"), QStringLiteral("First")}, {QStringLiteral("start_page"), 1}};
    const QJsonObject second {{QStringLiteral("id"), QStringLiteral("second.pdf")}, {QStringLiteral("filename"), QStringLiteral("second.pdf")},
                             {QStringLiteral("name"), QStringLiteral("Second")}, {QStringLiteral("start_page"), 1}};
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:course")}, {QStringLiteral("name"), QStringLiteral("Course")},
                              {QStringLiteral("revision"), 4}, {QStringLiteral("documents"), QJsonArray {first, second}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *documents = tree->topLevelItem(0)->child(0);
    QCOMPARE(documents->childCount(), 2);
    auto *selected = documents->child(0);
    QCOMPARE(selected->text(0), QStringLiteral("First"));
    tree->expandAll();
    tree->setCurrentItem(selected);
    const QPersistentModelIndex originalIndex(tree->currentIndex());
    QSignalSpy opened(&overview, &Thoughts::CommentsOverview::documentPageActivated);
    QSignalSpy changed(&overview, &Thoughts::CommentsOverview::projectAttachmentChangedLocally);
    bool triggered = false;
    bool confirmed = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QTimer::singleShot(3000, menu, &QMenu::close);
        const QString label = removeDocument ? QStringLiteral("Remove…") : QStringLiteral("Open PDF");
        QAction *chosen = nullptr;
        for (auto *action : menu->actions()) {
            if (action->text() == label) { chosen = action; break; }
        }
        QVERIFY(chosen);
        QVERIFY(chosen->isEnabled());
        overview.updateLibraryTree();
        QVERIFY(!originalIndex.isValid());
        auto *replacementDocuments = tree->topLevelItem(0)->child(0);
        tree->setCurrentItem(replacementDocuments->child(1));
        QCOMPARE(tree->currentItem()->text(0), QStringLiteral("Second"));
        QTimer confirmationTimer;
        if (removeDocument) {
            connect(&confirmationTimer, &QTimer::timeout, &overview, [&] {
                QDialog *dialog = nullptr;
                for (auto *widget : QApplication::topLevelWidgets()) {
                    auto *candidate = qobject_cast<QDialog *>(widget);
                    if (candidate && candidate->isVisible() && candidate->windowTitle() == QStringLiteral("Remove document")) {
                        dialog = candidate;
                        break;
                    }
                }
                if (!dialog) { return; }
                confirmationTimer.stop();
                QTimer::singleShot(3000, dialog, &QDialog::reject);
                for (auto *button : dialog->findChildren<QPushButton *>()) {
                    if (button->text().remove(QLatin1Char('&')) == QStringLiteral("Remove")) {
                        confirmed = true;
                        button->click();
                        return;
                    }
                }
                QFAIL("The removal confirmation button was not found.");
            });
            confirmationTimer.start(10);
        }
        triggered = true;
        chosen->trigger();
        menu->close();
    });
    overview.showLibraryItemContextMenu(selected, tree, tree->visualItemRect(selected).center(), true);
    QVERIFY(triggered);
    if (removeDocument) {
        QVERIFY(confirmed);
        QCOMPARE(opened.size(), 0);
        QCOMPARE(changed.size(), 1);
        const auto pending = store.pending();
        QCOMPARE(pending.size(), 1);
        const auto operation = pending.first().toObject();
        QCOMPARE(operation.value(QStringLiteral("action")).toString(), QStringLiteral("remove_document"));
        QCOMPARE(operation.value(QStringLiteral("project_id")), project.value(QStringLiteral("id")));
        QCOMPARE(operation.value(QStringLiteral("document_id")), first.value(QStringLiteral("id")));
        const auto remaining = store.projects().first().toObject().value(QStringLiteral("documents")).toArray();
        QCOMPARE(remaining.size(), 1);
        QCOMPARE(remaining.first().toObject().value(QStringLiteral("id")), second.value(QStringLiteral("id")));
    } else {
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.first().at(0).toString(), firstPath);
        QCOMPARE(opened.first().at(1).toInt(), 1);
        QCOMPARE(changed.size(), 0);
        QVERIFY(store.pending().isEmpty());
    }
}

void CommentsOverviewTest::testImportedProjectOpensPublishedPdf()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString target = temporary.filePath(QStringLiteral("current_pdfs/project_course.pdf"));
    const QString original = temporary.filePath(QStringLiteral("current_pdfs/lecture.pdf"));
    QVERIFY(writeFile(target, QByteArrayLiteral("%PDF-project")));
    QVERIFY(writeFile(original, QByteArrayLiteral("%PDF-lecture")));
    Thoughts::LibraryProjectStore store(temporary.path());
    const QJsonObject doc {{QStringLiteral("id"), QStringLiteral("lecture.pdf")}, {QStringLiteral("filename"), QStringLiteral("lecture.pdf")},
        {QStringLiteral("name"), QStringLiteral("Lecture")}, {QStringLiteral("start_page"), 1}};
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:course")}, {QStringLiteral("name"), QStringLiteral("Course")},
        {QStringLiteral("target"), QStringLiteral("project_course.pdf")}, {QStringLiteral("documents"), QJsonArray {doc}}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *root = tree->topLevelItem(0);
    QCOMPARE(root->data(0, Qt::UserRole + 114).toString(), QStringLiteral("Project"));
    QCOMPARE(root->child(0)->child(0)->data(0, Qt::UserRole + 114).toString(), QStringLiteral("PDF"));
    QSignalSpy pageOpened(&overview, &Thoughts::CommentsOverview::documentPageActivated);
    // The project and its Documents group open the published PDF titled by the project.
    for (auto *item : {root, root->child(0)}) {
        pageOpened.clear();
        tree->itemDoubleClicked(item, 0);
        QCOMPARE(pageOpened.size(), 1);
        QCOMPARE(pageOpened.first().first().toString(), target);
        QCOMPARE(pageOpened.first().at(1).toInt(), 0);
        QCOMPARE(pageOpened.first().at(3).toString(), QStringLiteral("Course"));
        QCOMPARE(pageOpened.first().at(4).toString(), QStringLiteral("new:course"));
    }
    pageOpened.clear();
    tree->itemDoubleClicked(root->child(0)->child(0), 0);
    QCOMPARE(pageOpened.size(), 1);
    QCOMPARE(pageOpened.first().first().toString(), original);
    QCOMPARE(pageOpened.first().at(1).toInt(), 1);
    QCOMPARE(pageOpened.first().at(3).toString(), QStringLiteral("Lecture"));
}

void CommentsOverviewTest::testMixedProjectDocumentsMenuWithoutTarget()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::LibraryProjectStore store(temporary.path());
    const QJsonObject source{{QStringLiteral("id"), QStringLiteral("docs-notes/tex/source/note.tex")},
        {QStringLiteral("name"), QStringLiteral("Note")}, {QStringLiteral("filename"), QStringLiteral("notes.pdf")},
        {QStringLiteral("source_project"), QStringLiteral("docs-notes")}};
    const QJsonObject imported{{QStringLiteral("id"), QStringLiteral("paper.pdf")},
        {QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
        {QStringLiteral("imported"), true}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:models")},
        {QStringLiteral("name"), QStringLiteral("Models")}, {QStringLiteral("target"), QString()},
        {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), QStringLiteral("docs-notes")}}}},
        {QStringLiteral("documents"), QJsonArray{source, imported}}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QTest::qWait(200);
    auto *root = tree->topLevelItem(0);
    auto *documents = root->child(0);
    QVERIFY(documents);
    QCOMPARE(documents->text(0), QStringLiteral("Documents"));
    root->setData(0, Qt::UserRole + 140, QVariant());
    bool manage = false;
    bool import = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        for (auto *action : menu->actions()) {
            manage |= action->objectName() == QStringLiteral("libraryManageDocumentsAction");
            import |= action->objectName() == QStringLiteral("libraryImportAction");
        }
        menu->close();
    });
    overview.showLibraryItemContextMenu(documents, tree, tree->visualItemRect(documents).center(), true);
    QVERIFY(manage);
    QVERIFY(import);
}

void CommentsOverviewTest::testImportedMarkdownFolderNavigation()
{
    QTemporaryDir temporary;
    Thoughts::LibraryProjectStore store(temporary.path());
    const QString target = temporary.filePath(QStringLiteral("current_pdfs/import-example.pdf"));
    QVERIFY(writeFile(target, QByteArrayLiteral("%PDF-example")));
    const QJsonObject doc{{QStringLiteral("id"), QStringLiteral("docs-import-example/markdown/source/motors/README.md")},
        {QStringLiteral("source_file"), QStringLiteral("docs-import-example/markdown/source/motors/README.md")},
        {QStringLiteral("source_project"), QStringLiteral("docs-import-example")}, {QStringLiteral("folder"), QStringLiteral("Knowledge")},
        {QStringLiteral("relative_path"), QStringLiteral("motors/README.md")}, {QStringLiteral("name"), QStringLiteral("Motors overview")},
        {QStringLiteral("filename"), QStringLiteral("import-example.pdf")}, {QStringLiteral("start_page"), 4}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:lab")}, {QStringLiteral("name"), QStringLiteral("Lab")},
        {QStringLiteral("source_project"), QString()}, {QStringLiteral("target"), QStringLiteral("import-example.pdf")},
        {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), QStringLiteral("docs-import-example")}}}},
        {QStringLiteral("documents"), QJsonArray{doc}}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path()); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *root = tree->topLevelItem(0); QCOMPARE(root->text(0), QStringLiteral("Lab"));
    auto *documents = root->child(0);
    QCOMPARE(documents->childCount(), 1);
    auto *child = documents->child(0); QCOMPARE(child->text(0), QStringLiteral("Motors overview"));
    QCOMPARE(child->data(0, Qt::UserRole + 114).toString(), QStringLiteral("PDF"));
    QSignalSpy opened(&overview, &Thoughts::CommentsOverview::documentPageActivated);
    tree->itemDoubleClicked(child, 0); QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().first().toString(), target); QCOMPARE(opened.first().at(1).toInt(), 4);
    bool hasImport = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
        for (auto *action : menu->actions()) hasImport |= action->text() == QStringLiteral("Import sources…");
        menu->close();
    });
    overview.showLibraryItemContextMenu(root, tree, tree->visualItemRect(root).center(), true);
    QVERIFY(hasImport);
    documents = tree->topLevelItem(0)->child(0);
    hasImport = false; bool hasPdfImport = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
        for (auto *action : menu->actions()) { hasImport |= action->text() == QStringLiteral("Import sources…"); hasPdfImport |= action->objectName() == QStringLiteral("libraryImportAction"); }
        menu->close();
    });
    overview.showLibraryItemContextMenu(documents, tree, tree->visualItemRect(documents).center(), true);
    QVERIFY(!hasImport); QVERIFY(hasPdfImport);

    auto *sources = new QTreeWidgetItem(tree->topLevelItem(0), {QStringLiteral("Sources")});
    sources->setData(0, Qt::UserRole + 107, QStringLiteral("new:lab/sources"));
    bool hasSourceImport = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
        auto *action = menu->findChild<QAction *>(QStringLiteral("libraryImportSourcesAction"));
        QVERIFY(action);
        QCOMPARE(action->text(), QStringLiteral("Import sources…"));
        QVERIFY(action->isEnabled());
        hasSourceImport = true;
        menu->close();
    });
    overview.showLibraryItemContextMenu(sources, tree, {}, true);
    QVERIFY(hasSourceImport);
}

void CommentsOverviewTest::testHtmlSourceOpening()
{
    QTemporaryDir temporary;
    Thoughts::LibraryProjectStore store(temporary.path());
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:html")}, {QStringLiteral("name"), QStringLiteral("HTML")},
        {QStringLiteral("documents"), QJsonArray{}}, {QStringLiteral("sources"), QJsonArray{}}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path()); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *sources = new QTreeWidgetItem(tree->topLevelItem(0), {QStringLiteral("Sources")});
    const QString htmlPath = QStringLiteral("docs-import-example/markdown/source/tools/plot.html");
    QVERIFY(writeFile(temporary.filePath(htmlPath), QByteArrayLiteral("<!doctype html><p>Plot</p>")));
    auto *html = new QTreeWidgetItem(sources, {QStringLiteral("plot.html")});
    html->setData(0, Qt::UserRole + 1, htmlPath);
    QSignalSpy sourceOpened(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    tree->itemDoubleClicked(html, 0);
    QCOMPARE(sourceOpened.size(), 1);
    QCOMPARE(sourceOpened.first().first().toString(), temporary.filePath(htmlPath));
    bool hasBrowser = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu);
        auto *openWith = menu->findChild<QMenu *>(QStringLiteral("libraryOpenWithMenu")); QVERIFY(openWith);
        auto *browser = openWith->findChild<QAction *>(QStringLiteral("libraryOpenHtmlBrowserAction")); QVERIFY(browser);
        QCOMPARE(browser->text(), QStringLiteral("Browser"));
        hasBrowser = browser->isEnabled();
        menu->close();
    });
    overview.showLibraryItemContextMenu(html, tree, {}, true);
    QVERIFY(hasBrowser);
}

void CommentsOverviewTest::testImportedDocumentSizes_data()
{
    QTest::addColumn<int>("componentCount");
    QTest::addColumn<QString>("catalogueState");
    QTest::newRow("whole-pdf") << 1 << QStringLiteral("current");
    QTest::newRow("imported-pdf-components") << 2 << QStringLiteral("current");
    QTest::newRow("whole-pdf-without-local-map") << 1 << QStringLiteral("current-no-map");
    QTest::newRow("components-without-local-map") << 2 << QStringLiteral("current-no-map");
    QTest::newRow("stale-catalogue-hash") << 1 << QStringLiteral("stale");
    QTest::newRow("different-page-range") << 1 << QStringLiteral("different-range");
    QTest::newRow("pending-build") << 1 << QStringLiteral("pending");
    QTest::newRow("provided-size") << 1 << QStringLiteral("provided");
}

void CommentsOverviewTest::testImportedDocumentSizes()
{
    QFETCH(int, componentCount);
    QFETCH(QString, catalogueState);
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QString filename = QStringLiteral("import-example.pdf");
    const QString owner = QStringLiteral("docs-import-example");
    const QString pdfPath = temporary.filePath(QStringLiteral("current_pdfs/") + filename);
    QVERIFY(QDir().mkpath(QFileInfo(pdfPath).path()));
    {
        QPdfWriter writer(pdfPath);
        QPainter painter(&writer);
        painter.drawText(100, 100, QStringLiteral("First imported document"));
        if (componentCount > 1) {
            QVERIFY(writer.newPage());
            painter.drawText(100, 100, QStringLiteral("Second imported document"));
        }
    }
    const QString pdfSha = QString::fromLatin1(QCryptographicHash::hash(readFile(pdfPath), QCryptographicHash::Sha256).toHex());
    QJsonArray segments;
    QJsonArray catalogueDocuments;
    for (int i = 0; i < componentCount; ++i) {
        const QString source = owner + QStringLiteral("/markdown/source/document%1.md").arg(i);
        segments.append(QJsonObject{{QStringLiteral("source_id"), source}, {QStringLiteral("start_page"), i + 1}, {QStringLiteral("page_count"), 1}});
        catalogueDocuments.append(QJsonObject{{QStringLiteral("id"), source}, {QStringLiteral("source_file"), source},
            {QStringLiteral("source_project"), owner}, {QStringLiteral("name"), QStringLiteral("Imported %1").arg(i)},
            {QStringLiteral("filename"), filename}, {QStringLiteral("start_page"), catalogueState == QLatin1String("different-range") ? 2 : i + 1},
            {QStringLiteral("page_count"), 1}, {QStringLiteral("sha256"), catalogueState == QLatin1String("stale") ? QString(64, QLatin1Char('0')) : pdfSha},
            {QStringLiteral("size_bytes"), catalogueState == QLatin1String("provided") ? 1234 : -1},
            {QStringLiteral("pending_build"), catalogueState == QLatin1String("pending")}});
    }
    const QJsonObject artifact{{QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
        {QStringLiteral("filename"), filename}, {QStringLiteral("pdf"), QJsonObject{{QStringLiteral("filename"), filename},
            {QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("page_count"), componentCount}, {QStringLiteral("segments"), segments}}}};
    const QByteArray artifactBytes = QJsonDocument(artifact).toJson();
    const QString artifactSha = QString::fromLatin1(QCryptographicHash::hash(artifactBytes, QCryptographicHash::Sha256).toHex());
    const QString bundle = root + QStringLiteral("/.runtime/target/source-bundle");
    QVERIFY(writeFile(bundle + QStringLiteral("/objects/") + artifactSha, artifactBytes));
    QVERIFY(writeFile(bundle + QStringLiteral("/documents/") + filename + QStringLiteral(".json"),
        QJsonDocument(QJsonObject{{QStringLiteral("artifact_sha256"), artifactSha}, {QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("filename"), filename}}).toJson()));
    if (catalogueState == QLatin1String("current-no-map")) {
        QVERIFY(QFile::remove(bundle + QStringLiteral("/documents/") + filename + QStringLiteral(".json")));
    }
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(QJsonObject{
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray{QJsonObject{{QStringLiteral("filename"), filename},
            {QStringLiteral("project"), owner}, {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), pdfSha}}}}}).toJson()));
    // Imported PDFs have a different navigation target from their parent project.
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("source:docs-parent")}, {QStringLiteral("name"), QStringLiteral("Parent")},
        {QStringLiteral("source_project"), QStringLiteral("docs-parent")}, {QStringLiteral("target"), QStringLiteral("parent.pdf")},
        {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), owner}}}},
        {QStringLiteral("documents"), catalogueDocuments}};
    Thoughts::LibraryProjectStore store(root);
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    auto configuration = Thoughts::ProjectConfiguration::empty(root, project.value(QStringLiteral("id")).toString(), QStringLiteral("Parent"));
    auto folderBindings = configuration.value(QStringLiteral("source_folders")).toArray();
    const QString folderId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    folderBindings.append(QJsonObject{{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), owner},
        {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}});
    configuration.insert(QStringLiteral("source_folders"), folderBindings);
    QJsonArray declarations;
    for (int i = 0; i < componentCount; ++i)
        declarations.append(QJsonObject{{QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("format"), QStringLiteral("markdown")}, {QStringLiteral("included"), true},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), folderId},
                {QStringLiteral("path"), QStringLiteral("markdown/source/document%1.md").arg(i)}}}});
    configuration.insert(QStringLiteral("documents"), declarations);
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root, project.value(QStringLiteral("id")).toString()), configuration).isEmpty());
    Thoughts::CommentsOverview overview(root);
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->child(0)->childCount(), componentCount);
    tree->topLevelItem(0)->setExpanded(true);
    const auto size = [&](int row) { return tree->topLevelItem(0)->child(0)->child(row)->data(3, Qt::UserRole + 100).toLongLong(); };
    if (catalogueState.startsWith(QLatin1String("current"))) {
        for (int i = 0; i < componentCount; ++i) QCOMPARE(size(i), -1);
    } else if (catalogueState == QLatin1String("provided")) {
        QCOMPARE(size(0), 1234);
    } else {
        QCOMPARE(size(0), -1);
        QTRY_COMPARE_WITH_TIMEOUT(tree->topLevelItem(0)->child(0)->child(0)->text(3),
                 catalogueState == QLatin1String("pending") ? QStringLiteral("Not built yet") : QStringLiteral("Unavailable"), 30000);
    }
}

void CommentsOverviewTest::testPendingMarkdownFolderSources()
{
    QTemporaryDir temporary;
    const QByteArray markdown("# Saved note\n\nThis text opens before Sync.\n");
    const QByteArray bundle = QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1},
        {QStringLiteral("documents"), QJsonArray{QStringLiteral("section/note.md")}},
        {QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("section/note.md")},
            {QStringLiteral("content"), QString::fromLatin1(markdown.toBase64())}}}}}).toJson();
    const auto path = temporary.filePath(QStringLiteral(".thoughts/folder-imports/copy.json"));
    QVERIFY(writeFile(path, bundle));
    const auto sha = QString::fromLatin1(QCryptographicHash::hash(bundle, QCryptographicHash::Sha256).toHex());
    Thoughts::LibraryProjectStore store(temporary.path());
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:pending")}, {QStringLiteral("name"), QStringLiteral("Lab")}, {QStringLiteral("revision"), 1}, {QStringLiteral("documents"), QJsonArray{}}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    QVERIFY(store.enqueue(QStringLiteral("import_folder"), project, {{QStringLiteral("name"), QString()}, {QStringLiteral("local_path"), path}, {QStringLiteral("sha256"), sha},
        {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("Saved note")}, {QStringLiteral("relative_path"), QStringLiteral("section/note.md")}}}}}).isEmpty());
    const auto operation = store.pending().first().toObject();
    QCOMPARE(operation.value(QStringLiteral("name")).toString(), QStringLiteral("Imported folder"));
    QCOMPARE(operation.value(QStringLiteral("sha256")).toString(), sha);
    Thoughts::CommentsOverview overview(temporary.path()); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *documents = tree->topLevelItem(0)->child(0);
    QCOMPARE(documents->childCount(), 0);
    auto *sources = tree->topLevelItem(0)->child(1); QCOMPARE(sources->text(0), QStringLiteral("Sources"));
    // The default source folder precedes connected folders.
    QCOMPARE(sources->childCount(), 2);
    auto *folder = sources->child(1); QCOMPARE(folder->text(0), QStringLiteral("Imported folder"));
    const auto findNote = [&]() -> QTreeWidgetItem * {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == QLatin1String("note.md")) return *it;
        return nullptr;
    };
    QTRY_VERIFY(findNote());
    auto *child = findNote();
    QCOMPARE(child->text(0), QStringLiteral("note.md"));
    QTRY_VERIFY(child->text(1) != QString::fromUtf8("—"));
    QVERIFY(child->text(3) != QStringLiteral("0.00 KiB"));
    QCOMPARE(child->data(0, Qt::UserRole).toString(), QString());
    QSignalSpy sourceOpened(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    tree->itemDoubleClicked(child, 0);
    QCOMPARE(sourceOpened.size(), 1);
    const auto sourcePath = sourceOpened.first().first().toString();
    QFile source(sourcePath); QVERIFY(source.open(QIODevice::ReadOnly)); QCOMPARE(source.readAll(), markdown); source.close();
    QVERIFY(source.open(QIODevice::WriteOnly)); source.write("# Edited source\n\nSaved before Sync.\n"); source.close();
    QCOMPARE(store.pending().first().toObject().value(QStringLiteral("operation_id")), operation.value(QStringLiteral("operation_id")));
    const auto materialized = store.projects(); // Refresh must not restore the old snapshot.
    QCOMPARE(materialized.first().toObject().value(QStringLiteral("documents")).toArray().size(), 0);
    QVERIFY(source.open(QIODevice::ReadOnly)); QVERIFY(source.readAll().contains("Saved before Sync."));

}

void CommentsOverviewTest::testCatalogueSourceFolderMenu()
{
    QTemporaryDir temporary;
    // Connected folders are rooted at their owner and keep their catalogue label.
    const QString owner = QStringLiteral("docs-import-example");
    const QString location = owner + QStringLiteral("/tex");
    QVERIFY(writeFile(temporary.filePath(location + QStringLiteral("/section/note.tex")), QByteArrayLiteral("Example")));
    const QJsonObject source{{QStringLiteral("id"), location}, {QStringLiteral("name"), QStringLiteral("Lab-meeting-notes")},
        {QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("id"), QString(location + QStringLiteral("/section/note.tex"))},
            {QStringLiteral("name"), QStringLiteral("note.tex")}, {QStringLiteral("relative_path"), QStringLiteral("section/note.tex")}}}}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:lab")}, {QStringLiteral("name"), QStringLiteral("Lab")},
        {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("project"), owner}, {QStringLiteral("folder"), QStringLiteral("Lab-meeting-notes")}}}},
        {QStringLiteral("sources"), QJsonArray{source}}, {QStringLiteral("documents"), QJsonArray{}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path()); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->child(1)->childCount(), 2);
    auto *folder = childNamed(tree->topLevelItem(0)->child(1), QStringLiteral("Lab-meeting-notes")); QVERIFY(folder);
    QCOMPARE(folder->data(0, Qt::UserRole + 118).toString(), temporary.filePath(owner));
    QCOMPARE(folder->child(0)->data(0, Qt::UserRole + 118).toString(), temporary.filePath(location));
    QSignalSpy sourceOpened(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    // A background Library refresh can rebuild the tree while a menu is open,
    // so never keep item pointers across one: find each item afresh and
    // decide the expected path before the menu runs.
    const auto itemFor = [tree](const QString &path) -> QTreeWidgetItem * {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->data(0, Qt::UserRole + 118).toString() == path) return *it;
        return nullptr;
    };
    const QStringList folderPaths {temporary.filePath(owner), temporary.filePath(location)};
    for (const QString &expected : folderPaths) {
        QTreeWidgetItem *selected = itemFor(expected);
        QVERIFY(selected);
        bool found = false;
        QTimer::singleShot(0, &overview, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (menu) {
                auto *action = menu->findChild<QAction *>(QStringLiteral("libraryOpenInVSCodeAction"));
                found = action && action->isEnabled() && action->text() == QStringLiteral("Open in VS Code");
                if (found) action->trigger();
                menu->close();
            }
        });
        overview.showLibraryItemContextMenu(selected, tree, {}, true);
        QVERIFY(found);
        QCOMPARE(sourceOpened.size(), 1);
        const auto arguments = sourceOpened.takeFirst();
        QCOMPARE(arguments.at(0).toString(), expected);
        QCOMPARE(arguments.at(1).toInt(), 0);
    }
#ifdef Q_OS_MACOS
    for (const QString &expected : folderPaths) {
        QTreeWidgetItem *selected = itemFor(expected);
        QVERIFY(selected);
        bool found = false;
        QTimer::singleShot(0, &overview, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (menu) {
                auto *action = menu->findChild<QAction *>(QStringLiteral("libraryRevealInFinderAction"));
                found = action && action->isEnabled() && action->text() == QStringLiteral("Reveal in Finder");
                menu->close();
            }
        });
        overview.showLibraryItemContextMenu(selected, tree, {}, true);
        QVERIFY(found);
    }
#endif
}

void CommentsOverviewTest::testRemoveSourceFolder()
{
    QTemporaryDir temporary;
    const QString owner = QStringLiteral("docs-import-example");
    const QString sourcePath = owner + QStringLiteral("/markdown/source/note.md");
    QVERIFY(writeFile(temporary.filePath(sourcePath), QByteArrayLiteral("Original note")));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/markdown/source/.git/config")), QByteArrayLiteral("Repository")));
    const QJsonObject folder{{QStringLiteral("project"), owner}, {QStringLiteral("folder"), QStringLiteral("weekly_slides")}};
    const QJsonObject doc{{QStringLiteral("id"), sourcePath}, {QStringLiteral("source_file"), sourcePath},
        {QStringLiteral("source_project"), owner}, {QStringLiteral("name"), QStringLiteral("Weekly slide")}};
    const QJsonObject other{{QStringLiteral("id"), QStringLiteral("other.pdf")}, {QStringLiteral("filename"), QStringLiteral("other.pdf")},
        {QStringLiteral("name"), QStringLiteral("Other document")}, {QStringLiteral("imported"), true}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:slides")}, {QStringLiteral("name"), QStringLiteral("Slides")},
        {QStringLiteral("revision"), 1}, {QStringLiteral("source_folders"), QJsonArray{folder}}, {QStringLiteral("documents"), QJsonArray{doc, other}},
        {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("id"), QString(owner + QStringLiteral("/markdown/source"))},
            {QStringLiteral("name"), QStringLiteral("weekly_slides")}}}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path()); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    bool found = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (menu) {
            auto *action = menu->findChild<QAction *>(QStringLiteral("libraryRemoveSourceAction"));
            found = action && action->isEnabled() && action->text() == QStringLiteral("Remove source…");
            menu->close();
        }
    });
    QCOMPARE(tree->topLevelItem(0)->child(1)->childCount(), 2);
    overview.showLibraryItemContextMenu(tree->topLevelItem(0)->child(1)->child(1), tree, {}, true);
    QVERIFY(found);
    bool inspected = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("removeSourceDialog"));
        if (!dialog) return;
        auto *list = dialog->findChild<QListWidget *>(QStringLiteral("removeSourceDocuments"));
        inspected = list && list->count() == 1 && list->item(0)->text() == QStringLiteral("Weekly slide");
        dialog->reject();
    });
    overview.editLibraryProject(project, QStringLiteral("remove_source"), folder);
    QVERIFY(inspected); QVERIFY(store.pending().isEmpty());
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("removeSourceDialog"));
        if (dialog) dialog->accept();
    });
    overview.editLibraryProject(project, QStringLiteral("remove_source"), folder);
    QCOMPARE(store.pending().size(), 1);
    QCOMPARE(store.pending().first().toObject().value(QStringLiteral("action")).toString(), QStringLiteral("remove_source"));
    const auto updated = store.projects().first().toObject();
    QVERIFY(updated.value(QStringLiteral("source_folders")).toArray().isEmpty());
    // Only the Project's default source folder remains.
    const auto remaining = updated.value(QStringLiteral("sources")).toArray();
    QCOMPARE(remaining.size(), 1);
    QCOMPARE(remaining.first().toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Default source folder"));
    const auto documents = updated.value(QStringLiteral("documents")).toArray();
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.first().toObject().value(QStringLiteral("name")), other.value(QStringLiteral("name")));
    QVERIFY(QFileInfo::exists(temporary.filePath(sourcePath)));
    QVERIFY(QFileInfo::exists(temporary.filePath(owner + QStringLiteral("/markdown/source/.git/config"))));
    QCOMPARE(tree->topLevelItem(0)->child(1)->childCount(), 1);
}

void CommentsOverviewTest::testRenameSourceFolder()
{
    // Rename changes only the name Sources shows; the folder stays where it is.
    QTemporaryDir temporary;
    const QString owner = QStringLiteral("docs-import-example");
    const QString other = QStringLiteral("docs-handmade");
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/markdown/source/note.md")), QByteArrayLiteral("Original note")));
    QVERIFY(writeFile(temporary.filePath(other + QStringLiteral("/markdown/source/other.md")), QByteArrayLiteral("Other note")));
    const QJsonObject folder{{QStringLiteral("project"), owner}, {QStringLiteral("folder"), QStringLiteral("Git repository")}};
    const QJsonObject handmade{{QStringLiteral("project"), other}, {QStringLiteral("folder"), other}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:models")}, {QStringLiteral("name"), QStringLiteral("Models")},
        {QStringLiteral("revision"), 1}, {QStringLiteral("source_folders"), QJsonArray{folder, handmade}}, {QStringLiteral("documents"), QJsonArray{}},
        {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("id"), owner}, {QStringLiteral("name"), QStringLiteral("Git repository")}}}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));
    Thoughts::CommentsOverview overview(temporary.path()); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QTreeWidgetItem *folderItem = nullptr;
    QTRY_VERIFY((folderItem = [&]() -> QTreeWidgetItem * {
        auto *sources = tree->topLevelItem(0)->child(1);
        for (int i = 0; sources && i < sources->childCount(); ++i)
            if (sources->child(i)->text(0) == QStringLiteral("Git repository")) return sources->child(i);
        return nullptr;
    }()));
    bool found = false;
    QTimer::singleShot(0, &overview, [&] {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            auto *action = menu->findChild<QAction *>(QStringLiteral("libraryRenameSourceAction"));
            found = action && action->isEnabled() && action->text() == QStringLiteral("Rename…");
            menu->close();
        }
    });
    overview.showLibraryItemContextMenu(folderItem, tree, {}, true);
    QVERIFY(found);

    // Types the name into the rename dialog and closes any error box that follows.
    QString typed; int errors = 0;
    QTimer driver;
    connect(&driver, &QTimer::timeout, &overview, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (auto *input = qobject_cast<QInputDialog *>(dialog)) { input->setTextValue(typed); input->accept(); }
        else if (dialog) { ++errors; dialog->reject(); }
    });
    driver.start(10);
    // A name another folder already shows is refused before anything is queued.
    typed = other.toUpper();
    overview.editLibraryProject(project, QStringLiteral("rename_source"), folder);
    QCOMPARE(errors, 1);
    QVERIFY(store.pending().isEmpty());

    typed = QStringLiteral("  release-1.5-report ");
    overview.editLibraryProject(project, QStringLiteral("rename_source"), folder);
    driver.stop();
    QCOMPARE(errors, 1);
    QCOMPARE(store.pending().size(), 1);
    const auto operation = store.pending().first().toObject();
    QCOMPARE(operation.value(QStringLiteral("action")).toString(), QStringLiteral("rename_source"));
    QCOMPARE(operation.value(QStringLiteral("source_project")).toString(), owner);
    QCOMPARE(operation.value(QStringLiteral("name")).toString(), QStringLiteral("release-1.5-report"));
    const auto updated = store.projects().first().toObject();
    QStringList labels;
    for (const auto &entry : updated.value(QStringLiteral("source_folders")).toArray())
        if (entry.toObject().value(QStringLiteral("project")).toString() == owner) labels << entry.toObject().value(QStringLiteral("folder")).toString();
    QCOMPARE(labels, QStringList{QStringLiteral("release-1.5-report")});
    QTRY_VERIFY([&] {
        auto *sources = tree->topLevelItem(0)->child(1);
        for (int i = 0; sources && i < sources->childCount(); ++i)
            if (sources->child(i)->text(0) == QStringLiteral("release-1.5-report")) return true;
        return false;
    }());
    QVERIFY(QFileInfo::exists(temporary.filePath(owner + QStringLiteral("/markdown/source/note.md"))));
}

void CommentsOverviewTest::testMoveSourceDocumentDialog()
{
    // A source-linked document moves through "Move to project…": the dialog
    // shows Fedora's plan, a name the destination already uses blocks Move,
    // and the queued operation carries the reviewed plan and this computer's
    // figure transfer.
    QTemporaryDir temporary;
    const QString documentId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QJsonObject doc{{QStringLiteral("id"), QStringLiteral("docs-concepts/tex/source/sdpo.tex")}, {QStringLiteral("document_id"), documentId},
        {QStringLiteral("filename"), QStringLiteral("concepts_sdpo.pdf")}, {QStringLiteral("name"), QStringLiteral("sdpo")},
        {QStringLiteral("source_file"), QStringLiteral("docs-concepts/tex/source/sdpo.tex")}, {QStringLiteral("source_project"), QStringLiteral("docs-concepts")}};
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("source:docs-concepts")}, {QStringLiteral("name"), QStringLiteral("concepts")}, {QStringLiteral("revision"), 4},
        {QStringLiteral("source_project"), QStringLiteral("docs-concepts")}, {QStringLiteral("documents"), QJsonArray{doc}}};
    const QJsonObject destination{{QStringLiteral("id"), QStringLiteral("source:docs-course-notes")}, {QStringLiteral("name"), QStringLiteral("course-notes")},
        {QStringLiteral("revision"), 7}, {QStringLiteral("source_project"), QStringLiteral("docs-course-notes")}, {QStringLiteral("documents"), QJsonArray{}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project, destination}}}).isEmpty());
    for (const auto &configured : {project, destination}) QVERIFY(registerCatalogueProject(temporary.path(), configured));
    const QByteArray figure("%PDF-figure");
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("docs-concepts/tex/assets"))));
    QFile file(temporary.filePath(QStringLiteral("docs-concepts/tex/assets/fig.pdf")));
    QVERIFY(file.open(QIODevice::WriteOnly) && file.write(figure) == figure.size());
    file.close();
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("docs-concepts/tex/generated"))));
    QFile generated(temporary.filePath(QStringLiteral("docs-concepts/tex/generated/t.tex")));
    QVERIFY(generated.open(QIODevice::WriteOnly) && generated.write(figure) == figure.size());
    generated.close();
    QVERIFY(QDir().mkpath(temporary.filePath(QStringLiteral("docs-concepts/scripts"))));
    QFile generator(temporary.filePath(QStringLiteral("docs-concepts/scripts/make_table.py")));
    QVERIFY(generator.open(QIODevice::WriteOnly) && generator.write("OUT = 'tex/generated/t.tex'\n") > 0);
    generator.close();
    const QString figureSha = QString::fromLatin1(QCryptographicHash::hash(figure, QCryptographicHash::Sha256).toHex());

    Thoughts::CommentsOverview overview(temporary.path());
    QList<QJsonObject> requests;
    overview.setSourceMovePlanner([&](const QJsonObject &request, QObject *, std::function<void(const QJsonObject &, const QString &)> reply) {
        requests.append(request);
        const QString path = request.value(QStringLiteral("destination_path")).toString();
        const bool collision = path == QLatin1String("tex/source/taken.tex");
        reply(QJsonObject{{QStringLiteral("document_id"), documentId}, {QStringLiteral("name"), QStringLiteral("sdpo")}, {QStringLiteral("filename"), QStringLiteral("concepts_sdpo.pdf")},
            {QStringLiteral("source"), QJsonObject{{QStringLiteral("project_id"), QStringLiteral("source:docs-concepts")}, {QStringLiteral("project_name"), QStringLiteral("concepts")},
                {QStringLiteral("folder"), QStringLiteral("docs-concepts")}, {QStringLiteral("path"), QStringLiteral("tex/source/sdpo.tex")}, {QStringLiteral("revision"), 4}}},
            {QStringLiteral("destination"), QJsonObject{{QStringLiteral("project_id"), QStringLiteral("source:docs-course-notes")}, {QStringLiteral("project_name"), QStringLiteral("course-notes")},
                {QStringLiteral("folder"), QStringLiteral("docs-course-notes")}, {QStringLiteral("path"), path}, {QStringLiteral("revision"), 7},
                {QStringLiteral("collision"), collision}, {QStringLiteral("suggested_path"), collision ? QStringLiteral("tex/source/taken_2.tex") : QString()}}},
            {QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("source"), QStringLiteral("tex/assets/fig.pdf")}, {QStringLiteral("destination"), QStringLiteral("tex/assets/fig.pdf")},
                {QStringLiteral("action"), QStringLiteral("move")}, {QStringLiteral("transfer"), QStringLiteral("desktop")}, {QStringLiteral("sha256"), figureSha},
                {QStringLiteral("reason"), QStringLiteral("used only by this document")}},
                QJsonObject{{QStringLiteral("source"), QStringLiteral("tex/generated/t.tex")}, {QStringLiteral("destination"), QStringLiteral("tex/generated/t.tex")},
                {QStringLiteral("action"), QStringLiteral("copy")}, {QStringLiteral("transfer"), QStringLiteral("desktop")}, {QStringLiteral("generated"), true},
                {QStringLiteral("sha256"), figureSha}, {QStringLiteral("reason"), QStringLiteral("generated input")}}}},
            {QStringLiteral("preamble"), QJsonObject{{QStringLiteral("applies"), true}, {QStringLiteral("missing"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("\\originOnly")}}}},
                {QStringLiteral("carried"), request.value(QStringLiteral("carry_preamble")).toBool() ? QStringLiteral("% carried") : QString()},
                {QStringLiteral("declined"), !request.value(QStringLiteral("carry_preamble")).toBool()}}},
            {QStringLiteral("markers"), QJsonObject{{QStringLiteral("review_cards"), 5}}},
            {QStringLiteral("plan_digest"), QString(QStringLiteral("digest-") + path)}}, {});
    });
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("moveSourceDocumentDialog")); QVERIFY(dialog);
        const auto close = qScopeGuard([dialog] { if (dialog->isVisible()) dialog->reject(); });
        auto *choices = dialog->findChild<QComboBox *>(QStringLiteral("moveSourceDocumentDestination")); QVERIFY(choices);
        QCOMPARE(choices->count(), 1);
        auto *review = dialog->findChild<QTextBrowser *>(QStringLiteral("moveSourceDocumentReview")); QVERIFY(review);
        auto *ok = dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok);
        QTRY_VERIFY(ok->isEnabled());
        QVERIFY(review->toPlainText().contains(QStringLiteral("5 review card markers")));
        QVERIFY(review->toPlainText().contains(QStringLiteral("regenerating there no longer updates")));
        QVERIFY(review->toPlainText().contains(QStringLiteral("generated by scripts/make_table.py")));
        QVERIFY(review->toPlainText().contains(QStringLiteral("\\originOnly")));
        QVERIFY(dialog->findChild<QCheckBox *>(QStringLiteral("moveSourceDocumentCarryPreamble"))->isVisibleTo(dialog));
        auto *path = dialog->findChild<QLineEdit *>(QStringLiteral("moveSourceDocumentPath"));
        QCOMPARE(path->text(), QStringLiteral("tex/source/sdpo.tex"));
        path->setText(QStringLiteral("tex/source/taken.tex"));
        Q_EMIT path->textEdited(path->text());
        QTRY_VERIFY(review->toPlainText().contains(QStringLiteral("already exists")));
        QVERIFY(!ok->isEnabled());
        path->setText(QStringLiteral("tex/source/sdpo.tex"));
        Q_EMIT path->textEdited(path->text());
        QTRY_VERIFY(ok->isEnabled());
        const auto screenshot = qEnvironmentVariable("THOUGHTS_MOVE_SOURCE_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(dialog->grab().save(screenshot));
        ok->click();
    });
    overview.moveLibrarySourceDocument(project, doc);
    QVERIFY(requests.size() >= 3);
    QCOMPARE(requests.first().value(QStringLiteral("action")).toString(), QStringLiteral("plan_move_source_document"));
    QCOMPARE(requests.first().value(QStringLiteral("document_id")).toString(), documentId);
    QCOMPARE(store.pending().size(), 1);
    const auto move = store.pending().first().toObject();
    QCOMPARE(move.value(QStringLiteral("action")).toString(), QStringLiteral("move_source_document"));
    QCOMPARE(move.value(QStringLiteral("plan_digest")).toString(), QStringLiteral("digest-tex/source/sdpo.tex"));
    QCOMPARE(move.value(QStringLiteral("destination_revision")).toInt(), 7);
    QCOMPARE(move.value(QStringLiteral("base_revision")).toInt(), 4);
    QCOMPARE(move.value(QStringLiteral("desktop_transfers")).toArray().size(), 2);
    QCOMPARE(move.value(QStringLiteral("fedora_moves")).toArray().first().toString(), QStringLiteral("docs-concepts/tex/source/sdpo.tex"));
    // Local-only bookkeeping never reaches Fedora.
    const auto payload = Thoughts::SourceDocumentMove::serverPayload(move);
    QVERIFY(!payload.contains(QStringLiteral("desktop_transfers")) && payload.contains(QStringLiteral("plan_digest")));
    // Shown in the destination until Fedora applies it.
    for (const auto &value : store.projects(false)) {
        const auto entry = value.toObject();
        QStringList ids;
        for (const auto &d : entry.value(QStringLiteral("documents")).toArray()) ids << d.toObject().value(QStringLiteral("document_id")).toString();
        QCOMPARE(ids.contains(documentId), entry.value(QStringLiteral("id")).toString() == QLatin1String("source:docs-course-notes"));
    }
    // Staging copies the figure and a refused move removes only that copy.
    QJsonObject staged = move;
    QVERIFY(Thoughts::SourceDocumentMove::stageTransfers(temporary.path(), staged).isEmpty());
    QCOMPARE(staged.value(QStringLiteral("local_created")).toArray().size(), 2);
    QVERIFY(QFileInfo::exists(temporary.filePath(QStringLiteral("docs-course-notes/tex/generated/t.tex"))));
    QVERIFY(Thoughts::SourceDocumentMove::stageTransfers(temporary.path(), staged).isEmpty()); // idempotent
    Thoughts::SourceDocumentMove::rollbackTransfers(temporary.path(), staged);
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("docs-course-notes/tex/generated/t.tex"))));
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("docs-course-notes/tex/assets/fig.pdf"))));
    QVERIFY(QFileInfo::exists(temporary.filePath(QStringLiteral("docs-concepts/tex/assets/fig.pdf"))));
    // After Fedora accepted: moved originals go, copied ones stay.
    QVERIFY(Thoughts::SourceDocumentMove::stageTransfers(temporary.path(), staged).isEmpty());
    QVERIFY(Thoughts::SourceDocumentMove::finishTransfers(temporary.path(), staged).isEmpty());
    QVERIFY(!QFileInfo::exists(temporary.filePath(QStringLiteral("docs-concepts/tex/assets/fig.pdf"))));
    QVERIFY(QFileInfo::exists(temporary.filePath(QStringLiteral("docs-concepts/tex/generated/t.tex"))));
    QVERIFY(QFileInfo::exists(temporary.filePath(QStringLiteral("docs-course-notes/tex/assets/fig.pdf"))));
}

void CommentsOverviewTest::testMoveSourceDocumentStaysInItsTwoFolders()
{
    using namespace Thoughts::SourceDocumentMove;
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.filePath(QStringLiteral("library"));
    const QString outside = temporary.filePath(QStringLiteral("outside"));
    const QByteArray bytes("%PDF-figure");
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    const auto write = [&](const QString &path, const QByteArray &content = QByteArray("%PDF-figure")) {
        return QDir().mkpath(QFileInfo(path).absolutePath()) && writeFile(path, content);
    };
    QVERIFY(write(root + QStringLiteral("/docs-a/tex/assets/fig.pdf")));
    QVERIFY(write(root + QStringLiteral("/docs-a/tex/source/doc.tex")));
    QVERIFY(write(root + QStringLiteral("/docs-c/tex/assets/private.pdf")));
    QVERIFY(write(root + QStringLiteral("/.thoughts/state.json")));
    QVERIFY(write(outside + QStringLiteral("/secret.pdf")));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/docs-b/tex")));
    QVERIFY(QDir().mkpath(outside + QStringLiteral("/assets")));

    // By its text: inside the named folder, which is one plain name of the Library.
    QCOMPARE(boundPath(root, QStringLiteral("docs-a"), QStringLiteral("docs-a/tex/assets/fig.pdf")), root + QStringLiteral("/docs-a/tex/assets/fig.pdf"));
    QCOMPARE(boundPath(root, QStringLiteral("docs-b"), QStringLiteral("docs-b/tex/assets/new/fig.pdf")), root + QStringLiteral("/docs-b/tex/assets/new/fig.pdf"));
    QCOMPARE(boundPath(root, QStringLiteral("docs-new"), QStringLiteral("docs-new/tex/fig.pdf")), root + QStringLiteral("/docs-new/tex/fig.pdf"));
    for (const auto &[folder, path] : QList<QPair<QString, QString>>{
             {QStringLiteral("docs-a"), QStringLiteral("docs-c/tex/assets/private.pdf")}, {QStringLiteral("docs-a"), QStringLiteral("docs-a/../docs-c/tex/assets/private.pdf")},
             {QStringLiteral("docs-a"), QStringLiteral("../outside/secret.pdf")}, {QStringLiteral("docs-a"), outside + QStringLiteral("/secret.pdf")},
             {QStringLiteral("docs-a"), QStringLiteral("docs-a")}, {QStringLiteral("docs-a"), QStringLiteral("docs-abc/x.pdf")}, {QStringLiteral("docs-a"), QString()},
             {QStringLiteral(".thoughts"), QStringLiteral(".thoughts/state.json")}, {QStringLiteral(".."), QStringLiteral("../outside/secret.pdf")},
             {QStringLiteral("docs-a/tex"), QStringLiteral("docs-a/tex/assets/fig.pdf")}, {QString(), QStringLiteral("docs-a/tex/assets/fig.pdf")},
             {QStringLiteral("."), QStringLiteral("./docs-a/tex/assets/fig.pdf")}}) {
        QVERIFY2(boundPath(root, folder, path).isEmpty(), qPrintable(folder + QStringLiteral(" | ") + path));
    }

    // By where it leads: links out of the folder are refused, wherever they sit.
    QVERIFY(QFile::link(outside + QStringLiteral("/assets"), root + QStringLiteral("/docs-b/tex/assets")));
    QVERIFY(QFile::link(outside + QStringLiteral("/secret.pdf"), root + QStringLiteral("/docs-a/tex/assets/linked.pdf")));
    QVERIFY(QFile::link(outside + QStringLiteral("/nothing-here"), root + QStringLiteral("/docs-a/tex/assets/dangling.pdf")));
    QVERIFY(QFile::link(root + QStringLiteral("/docs-c"), root + QStringLiteral("/docs-link")));
    QVERIFY(QFile::link(outside, root + QStringLiteral("/docs-out")));
    QVERIFY(QFile::link(root + QStringLiteral("/docs-a/tex/assets"), root + QStringLiteral("/docs-a/tex/pictures")));
    QVERIFY(boundPath(root, QStringLiteral("docs-b"), QStringLiteral("docs-b/tex/assets/fig.pdf")).isEmpty());
    QVERIFY(boundPath(root, QStringLiteral("docs-a"), QStringLiteral("docs-a/tex/assets/linked.pdf")).isEmpty());
    QVERIFY(boundPath(root, QStringLiteral("docs-a"), QStringLiteral("docs-a/tex/assets/dangling.pdf")).isEmpty());
    QVERIFY(boundPath(root, QStringLiteral("docs-link"), QStringLiteral("docs-link/tex/assets/private.pdf")).isEmpty());
    QVERIFY(boundPath(root, QStringLiteral("docs-out"), QStringLiteral("docs-out/secret.pdf")).isEmpty());
    // A link that stays inside the folder is the folder's own business.
    QVERIFY(!boundPath(root, QStringLiteral("docs-a"), QStringLiteral("docs-a/tex/pictures/fig.pdf")).isEmpty());

    const auto operation = [&](const QString &from, const QString &to, const QString &action = QStringLiteral("move")) {
        return QJsonObject{{QStringLiteral("source_folder"), QStringLiteral("docs-a")}, {QStringLiteral("destination_folder"), QStringLiteral("docs-b")},
                           {QStringLiteral("fedora_moves"), QJsonArray{QStringLiteral("docs-a/tex/source/doc.tex")}},
                           {QStringLiteral("desktop_transfers"), QJsonArray{QJsonObject{{QStringLiteral("source"), from}, {QStringLiteral("destination"), to},
                                                                                        {QStringLiteral("action"), action}, {QStringLiteral("sha256"), digest}}}}};
    };
    // A plan that reads another project's file, or one outside the Library, copies nothing.
    for (const QString &from : {QStringLiteral("docs-c/tex/assets/private.pdf"), QStringLiteral("docs-a/tex/assets/linked.pdf"), QStringLiteral("../outside/secret.pdf"),
                                QStringLiteral(".thoughts/state.json")}) {
        QJsonObject staged = operation(from, QStringLiteral("docs-b/tex/copied.pdf"));
        QVERIFY2(stageTransfers(root, staged).contains(QStringLiteral("outside its two project folders")), qPrintable(from));
        QVERIFY(!QFileInfo::exists(root + QStringLiteral("/docs-b/tex/copied.pdf")));
        QVERIFY(staged.value(QStringLiteral("local_created")).toArray().isEmpty());
        // Nor is it removed once the server accepted the move.
        QCOMPARE(finishTransfers(root, staged), QStringList());
    }
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/docs-c/tex/assets/private.pdf")));
    QVERIFY(QFileInfo::exists(outside + QStringLiteral("/secret.pdf")));
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/.thoughts/state.json")));
    // A plan that writes into another project, the Library's own state, or through a link out of the folder writes nothing.
    for (const QString &to : {QStringLiteral("docs-c/tex/assets/planted.pdf"), QStringLiteral("docs-b/tex/assets/planted.pdf"), QStringLiteral(".thoughts/planted.json"),
                              QStringLiteral("../outside/planted.pdf"), QStringLiteral("docs-out/planted.pdf"), QStringLiteral("docs-a/tex/assets/planted.pdf")}) {
        QJsonObject staged = operation(QStringLiteral("docs-a/tex/assets/fig.pdf"), to);
        QVERIFY2(stageTransfers(root, staged).contains(QStringLiteral("outside its two project folders")), qPrintable(to));
        QVERIFY(staged.value(QStringLiteral("local_created")).toArray().isEmpty());
    }
    QVERIFY(QDir(outside + QStringLiteral("/assets")).isEmpty());
    QVERIFY(!QFileInfo::exists(outside + QStringLiteral("/planted.pdf")));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/docs-c/tex/assets/planted.pdf")));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/.thoughts/planted.json")));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/docs-a/tex/assets/planted.pdf")));
    // A rollback removes nothing outside the destination folder, whatever was recorded.
    QJsonObject forged = operation(QStringLiteral("docs-a/tex/assets/fig.pdf"), QStringLiteral("docs-c/tex/assets/private.pdf"));
    forged.insert(QStringLiteral("local_created"), QJsonArray{QStringLiteral("docs-c/tex/assets/private.pdf")});
    rollbackTransfers(root, forged);
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/docs-c/tex/assets/private.pdf")));
    // Unpublished edits are looked for in the document's folder only.
    forged.insert(QStringLiteral("fedora_moves"), QJsonArray{QStringLiteral("docs-a/tex/source/doc.tex"), QStringLiteral("docs-c/tex/assets/private.pdf")});
    QCOMPARE(unpublishedEdits(root, root + QStringLiteral("/no-state.json"), forged), QStringList{QStringLiteral("docs-a/tex/source/doc.tex")});

    // A move queued by an earlier version names no folders: they are the ones its paths start with.
    QJsonObject earlier = operation(QStringLiteral("docs-a/tex/assets/fig.pdf"), QStringLiteral("docs-b/tex/figures/fig.pdf"));
    earlier.remove(QStringLiteral("source_folder"));
    earlier.remove(QStringLiteral("destination_folder"));
    QCOMPARE(folders(earlier).source, QStringLiteral("docs-a"));
    QCOMPARE(folders(earlier).destination, QStringLiteral("docs-b"));
    QVERIFY(stageTransfers(root, earlier).isEmpty());
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/docs-b/tex/figures/fig.pdf")));
    QJsonObject mixed = earlier;
    mixed.insert(QStringLiteral("desktop_transfers"), QJsonArray{earlier.value(QStringLiteral("desktop_transfers")).toArray().first(),
                                                                 QJsonObject{{QStringLiteral("source"), QStringLiteral("docs-c/tex/assets/private.pdf")},
                                                                             {QStringLiteral("destination"), QStringLiteral("docs-b/tex/figures/private.pdf")},
                                                                             {QStringLiteral("action"), QStringLiteral("copy")}, {QStringLiteral("sha256"), digest}}});
    QVERIFY(stageTransfers(root, mixed).contains(QStringLiteral("outside its two project folders")));
    QVERIFY(!QFileInfo::exists(root + QStringLiteral("/docs-b/tex/figures/private.pdf")));

    // The reviewed plan: the document's folder, and a folder of the chosen project (or a new one, never another project's).
    const auto plan = [](const QString &origin, const QString &target, const QString &source = QStringLiteral("tex/assets/fig.pdf"),
                         const QString &destination = QStringLiteral("tex/assets/fig.pdf")) {
        return QJsonObject{{QStringLiteral("source"), QJsonObject{{QStringLiteral("folder"), origin}, {QStringLiteral("path"), QStringLiteral("tex/source/doc.tex")}}},
                           {QStringLiteral("destination"), QJsonObject{{QStringLiteral("folder"), target}, {QStringLiteral("path"), QStringLiteral("tex/source/doc.tex")}}},
                           {QStringLiteral("files"), QJsonArray{QJsonObject{{QStringLiteral("source"), source}, {QStringLiteral("destination"), destination}}}}};
    };
    const QStringList chosen{QStringLiteral("docs-b"), QStringLiteral("docs-b-extra")};
    const QStringList foreign{QStringLiteral("docs-a"), QStringLiteral("docs-c")};
    QString error;
    QVERIFY2(planBound(plan(QStringLiteral("docs-a"), QStringLiteral("docs-b")), QStringLiteral("docs-a"), chosen, foreign, &error), qPrintable(error));
    QVERIFY(planBound(plan(QStringLiteral("docs-a"), QStringLiteral("docs-b-extra")), QStringLiteral("docs-a"), chosen, foreign, &error));
    QVERIFY(planBound(plan(QStringLiteral("docs-a"), QStringLiteral("docs-first-folder")), QStringLiteral("docs-a"), {}, foreign, &error));
    QVERIFY(!planBound(plan(QStringLiteral("docs-c"), QStringLiteral("docs-b")), QStringLiteral("docs-a"), chosen, foreign, &error));
    QVERIFY(error.contains(QStringLiteral("another folder than the document's")));
    QVERIFY(!planBound(plan(QStringLiteral("docs-a"), QStringLiteral("docs-c")), QStringLiteral("docs-a"), chosen, foreign, &error));
    QVERIFY(error.contains(QStringLiteral("another folder than the chosen project's")));
    QVERIFY(!planBound(plan(QStringLiteral("docs-a"), QStringLiteral("DOCS-C")), QStringLiteral("docs-a"), chosen, foreign, &error));
    for (const QString &folder : {QStringLiteral(".thoughts"), QStringLiteral(".."), QStringLiteral("docs-b/tex"), QStringLiteral("/tmp"), QString()}) {
        QVERIFY2(!planBound(plan(QStringLiteral("docs-a"), folder), QStringLiteral("docs-a"), {}, foreign, &error), qPrintable(folder));
        QVERIFY2(!planBound(plan(folder, QStringLiteral("docs-b")), QString(), chosen, foreign, &error), qPrintable(folder));
    }
    QVERIFY(!planBound(plan(QStringLiteral("docs-a"), QStringLiteral("docs-b"), QStringLiteral("../docs-c/tex/assets/private.pdf")), QStringLiteral("docs-a"), chosen, foreign, &error));
    QVERIFY(error.contains(QStringLiteral("outside the two project folders")));
    QVERIFY(!planBound(plan(QStringLiteral("docs-a"), QStringLiteral("docs-b"), QStringLiteral("tex/assets/fig.pdf"), QStringLiteral("/etc/planted")), QStringLiteral("docs-a"), chosen, foreign, &error));
    // What the dialog queues carries the two folders, and they never reach the server.
    const QJsonObject values = operationValues(plan(QStringLiteral("docs-a"), QStringLiteral("docs-b")), true);
    QCOMPARE(values.value(QStringLiteral("source_folder")).toString(), QStringLiteral("docs-a"));
    QCOMPARE(values.value(QStringLiteral("destination_folder")).toString(), QStringLiteral("docs-b"));
    QVERIFY(!serverPayload(values).contains(QStringLiteral("source_folder")) && !serverPayload(values).contains(QStringLiteral("destination_folder")));
}

void CommentsOverviewTest::testMoveSourceFolderPreview()
{
    QTemporaryDir temporary;
    const QString owner = QStringLiteral("docs-import-notes");
    const QString sourcePath = owner + QStringLiteral("/markdown/source/note.md");
    QVERIFY(writeFile(temporary.filePath(sourcePath), QByteArrayLiteral("Original note")));
    const QJsonObject folder{{QStringLiteral("project"), owner}, {QStringLiteral("folder"), QStringLiteral("Notes")},
                             {QStringLiteral("format"), QStringLiteral("markdown")}};
    const QJsonObject document{{QStringLiteral("id"), sourcePath}, {QStringLiteral("source_file"), sourcePath},
                               {QStringLiteral("source_project"), owner}, {QStringLiteral("name"), QStringLiteral("Note")}};
    const QJsonObject source{{QStringLiteral("id"), QStringLiteral("new:source")}, {QStringLiteral("name"), QStringLiteral("Source")},
                             {QStringLiteral("revision"), 1}, {QStringLiteral("source_folders"), QJsonArray{folder}},
                             {QStringLiteral("documents"), QJsonArray{document}},
                             {QStringLiteral("sources"), QJsonArray{QJsonObject{{QStringLiteral("id"), QString(owner + QStringLiteral("/markdown"))},
                                                                                 {QStringLiteral("name"), QStringLiteral("Markdown sources")}}}}};
    const QJsonObject target{{QStringLiteral("id"), QStringLiteral("new:target")}, {QStringLiteral("name"), QStringLiteral("Target")},
                             {QStringLiteral("revision"), 1}, {QStringLiteral("documents"), QJsonArray{}}, {QStringLiteral("sources"), QJsonArray{}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{source, target}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), source));
    QVERIFY(registerCatalogueProject(temporary.path(), target));
    Thoughts::CommentsOverview overview(temporary.path()); overview.show(); overview.showSummary();
    QTimer::singleShot(0, &overview, [&] {
        auto *dialog = overview.findChild<QDialog *>(QStringLiteral("moveSourceDialog")); QVERIFY(dialog);
        dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Ok)->click();
        // A refused move keeps the dialog open; close it so the checks below report it.
        if (dialog->isVisible()) dialog->reject();
    });
    overview.moveLibrarySourceFolder(source, owner, QStringLiteral("Notes"));
    QCOMPARE(store.pending().size(), 1);
    const auto operation = store.pending().first().toObject();
    QCOMPARE(operation.value(QStringLiteral("action")).toString(), QStringLiteral("move_source"));
    QCOMPARE(operation.value(QStringLiteral("source_project")).toString(), owner);
    // Projects are listed by configuration location, not catalogue order.
    const auto projected = [&](const QJsonObject &project) {
        for (const auto &value : store.projects())
            if (value.toObject().value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) return value.toObject();
        return QJsonObject();
    };
    QCOMPARE(projected(source).value(QStringLiteral("documents")).toArray().size(), 0);
    QCOMPARE(projected(target).value(QStringLiteral("documents")).toArray().first().toObject().value(QStringLiteral("id")).toString(), sourcePath);
    QCOMPARE(projected(target).value(QStringLiteral("source_folders")).toArray().first().toObject().value(QStringLiteral("project")).toString(), owner);
    QVERIFY(QFileInfo::exists(temporary.filePath(sourcePath)));
}

void CommentsOverviewTest::testConfiguredSourceRootsAndCachedHierarchy()
{
    QTemporaryDir temporary;
    const QString root = temporary.path(), id = QStringLiteral("new:source-roots"), owner = QStringLiteral("docs-mixed");
    auto config = Thoughts::ProjectConfiguration::empty(root, id, QStringLiteral("Applications"));
    auto folders = config.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), owner}});
    config.insert(QStringLiteral("source_folders"), folders);
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root, id), config).isEmpty());
    const auto file = [](const QString &path, const QString &relative) {
        return QJsonObject{{QStringLiteral("id"), path}, {QStringLiteral("name"), QFileInfo(path).fileName()},
            {QStringLiteral("relative_path"), relative}, {QStringLiteral("size_bytes"), 10}};
    };
    const QString markdown = owner + QStringLiteral("/markdown/source/note.md"), tex = owner + QStringLiteral("/tex/source/main.tex");
    const QJsonObject md = file(markdown, QStringLiteral("source/note.md"));
    const QJsonArray categories{
        QJsonObject{{QStringLiteral("id"), QString(owner + QStringLiteral("/markdown"))}, {QStringLiteral("name"), QStringLiteral("Markdown sources")},
                    {QStringLiteral("files"), QJsonArray{md, md}}},
        QJsonObject{{QStringLiteral("id"), QString(owner + QStringLiteral("/tex"))}, {QStringLiteral("name"), QStringLiteral("Project sources")},
                    {QStringLiteral("files"), QJsonArray{file(tex, QStringLiteral("source/main.tex"))}}}};
    Thoughts::LibraryProjectStore store(root);
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{QJsonObject{{QStringLiteral("id"), id},
        {QStringLiteral("documents"), QJsonArray{}}, {QStringLiteral("sources"), categories}}}}}).isEmpty());
    const QHash<QString, QJsonObject> emptyInventory;
    const auto projected = store.projects(true, &emptyInventory).first().toObject().value(QStringLiteral("sources")).toArray();
    QCOMPARE(projected.size(), 2);
    QCOMPARE(projected.first().toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Default source folder"));
    const auto connected = projected.last().toObject();
    QCOMPARE(connected.value(QStringLiteral("id")).toString(), owner);
    const auto files = connected.value(QStringLiteral("files")).toArray();
    QCOMPARE(files.size(), 2);
    QCOMPARE(files[0].toObject().value(QStringLiteral("relative_path")).toString(), QStringLiteral("markdown/source/note.md"));
    QCOMPARE(files[1].toObject().value(QStringLiteral("relative_path")).toString(), QStringLiteral("tex/source/main.tex"));
    // The synchronous path must also discover roots solely from the manifest.
    QCOMPARE(store.projects().first().toObject().value(QStringLiteral("sources")).toArray().size(), 2);
    QVERIFY(writeFile(temporary.filePath(markdown), QByteArrayLiteral("A note.\n")));
    QVERIFY(writeFile(temporary.filePath(tex), QByteArrayLiteral("A paper.\n")));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/.ThoughtIgnore")), QByteArrayLiteral("ignored.md\n")));
    QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/ignored.md")), QByteArrayLiteral("Ignored.\n")));
    Thoughts::LibrarySourceInventory inventory(root);
    const auto defaultOwner = folders.first().toObject().value(QStringLiteral("path")).toString();
    inventory.refresh(defaultOwner); inventory.refresh(owner); inventory.save({defaultOwner, owner});
    Thoughts::CommentsOverview overview(root); overview.resize(1100, 760); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *sources = tree->topLevelItem(0)->child(1);
    QTRY_COMPARE(sources->childCount(), 2);
    auto *connectedRoot = sources->child(1);
    QCOMPARE(connectedRoot->text(0), owner);
    QCOMPARE(connectedRoot->data(0, Qt::UserRole + 118).toString(), temporary.filePath(owner));
    QTRY_COMPARE(connectedRoot->childCount(), 3); // .ThoughtIgnore, markdown, tex.
    QSet<QString> seen;
    for (QTreeWidgetItemIterator it(connectedRoot); *it; ++it) {
        const auto path = (*it)->data(0, Qt::UserRole + 1).toString();
        if (!path.isEmpty()) seen.insert(path);
    }
    QCOMPARE(seen, (QSet<QString>{markdown, tex, owner + QStringLiteral("/.ThoughtIgnore")}));
    QSignalSpy opened(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    for (auto *selected : {connectedRoot, connectedRoot->child(1)}) {
        const auto expected = selected->data(0, Qt::UserRole + 118).toString();
        QVERIFY(!expected.isEmpty());
        QTimer::singleShot(0, &overview, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) return;
            auto *action = menu->findChild<QAction *>(QStringLiteral("libraryOpenInVSCodeAction"));
            if (action && action->isEnabled()) action->trigger();
            menu->close();
        });
        overview.showLibraryItemContextMenu(selected, tree, {}, true);
        QCOMPARE(opened.size(), 1);
        QCOMPARE(opened.takeFirst().first().toString(), expected);
    }
    const auto image = qEnvironmentVariable("THOUGHTS_SOURCE_ROOTS_SCREENSHOT");
    if (!image.isEmpty()) { tree->expandAll(); QTest::qWait(100); QVERIFY(tree->grab().save(image)); }
}

void CommentsOverviewTest::testRealImportedSourceLayout()
{
    const auto input = qEnvironmentVariable("THOUGHTS_SOURCE_IMPORT_FIXTURE");
    if (input.isEmpty()) QSKIP("Optional real-folder layout check");
    const auto copy = Thoughts::MarkdownFolderImport::scan(input); QVERIFY(copy.error.isEmpty());
    const auto raw = copy.bundle(copy.documents); QVERIFY(!raw.isEmpty());
    QTemporaryDir temporary; const auto path = temporary.filePath(QStringLiteral("copy.json")); QVERIFY(writeFile(path, raw));
    QJsonArray entries;
    for (const auto &name : copy.documents) entries.append(QJsonObject{{QStringLiteral("name"), copy.title(name)}, {QStringLiteral("relative_path"), name}});
    Thoughts::LibraryProjectStore store(temporary.path());
    const QJsonObject project{{QStringLiteral("id"), QStringLiteral("new:real")}, {QStringLiteral("name"), QStringLiteral("Lab")}, {QStringLiteral("revision"), 1}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(store.enqueue(QStringLiteral("import_folder"), project, {{QStringLiteral("name"), Thoughts::MarkdownFolderImport::folderName(input)}, {QStringLiteral("documents"), entries},
        {QStringLiteral("local_path"), path}, {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex())}}).isEmpty());
    Thoughts::CommentsOverview overview(temporary.path()); overview.resize(1200, 760); overview.show(); overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree")); QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QCOMPARE(tree->topLevelItem(0)->child(0)->childCount(), 0);
    auto *sources = tree->topLevelItem(0)->child(1); QCOMPARE(sources->childCount(), 1);
    int count = 0; qint64 bytes = 0;
    for (QTreeWidgetItemIterator it(sources); *it; ++it) {
        const auto relative = (*it)->data(0, Qt::UserRole + 1).toString();
        if (relative.isEmpty()) continue;
        const auto original = relative.section(QStringLiteral("/markdown/source/"), 1);
        QFile file(QDir(temporary.path()).filePath(relative)); QVERIFY(file.open(QIODevice::ReadOnly));
        const auto data = file.readAll(); QFile reference(QDir(input).filePath(original)); QVERIFY(reference.open(QIODevice::ReadOnly)); QCOMPARE(data, reference.readAll());
        ++count; bytes += data.size();
    }
    QCOMPARE(count, copy.includedFiles(copy.documents).size()); QCOMPARE(bytes, copy.selectionBytes(copy.documents));
    qInfo() << "Editable Sources:" << count << "files," << bytes << "bytes; Documents: 0 pending PDFs";
    QTRY_VERIFY(tree->topLevelItem(0)->child(1)->text(1) != QString::fromUtf8("—"));
    const auto image = qEnvironmentVariable("THOUGHTS_SOURCE_IMPORT_SCREENSHOT");
    if (!image.isEmpty()) { tree->expandToDepth(3); QTest::qWait(200); QVERIFY(tree->grab().save(image)); }
}

void CommentsOverviewTest::testSourceFileRenameAndDelete()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString folder = temporary.filePath(QStringLiteral("docs-test/markdown/source/assets"));
    const QString original = folder + QStringLiteral("/thoughts-test-figure.png");
    QVERIFY(writeFile(original, QByteArrayLiteral("png")));
    QVERIFY(writeFile(folder + QStringLiteral("/taken.png"), QByteArrayLiteral("other")));
    Thoughts::CommentsOverview overview(temporary.path());

    // Rename stays in the folder and never overwrites another file.
    overview.m_libraryInventoryDirty = false;
    QVERIFY(overview.renameSourceFile(original, QStringLiteral(" thoughts-test-figure-renamed.png ")).isEmpty());
    const QString renamed = folder + QStringLiteral("/thoughts-test-figure-renamed.png");
    QVERIFY(!QFileInfo::exists(original));
    QVERIFY(QFileInfo::exists(renamed));
    QVERIFY(overview.m_libraryInventoryDirty);
    QCOMPARE(overview.renameSourceFile(renamed, QStringLiteral("taken.png")), QStringLiteral("“taken.png” already exists in this folder."));
    QCOMPARE(QFile(folder + QStringLiteral("/taken.png")).size(), 5);
    QVERIFY(!overview.renameSourceFile(renamed, QStringLiteral("../escape.png")).isEmpty());
    QVERIFY(!overview.renameSourceFile(renamed, QStringLiteral("  ")).isEmpty());
    QVERIFY(QFileInfo::exists(renamed));

    // Delete moves the file to the Trash instead of erasing it.
    overview.m_libraryInventoryDirty = false;
    QVERIFY(overview.deleteSourceFile(renamed).isEmpty());
    QVERIFY(!QFileInfo::exists(renamed));
    QVERIFY(overview.m_libraryInventoryDirty);

    // New files get the extension, a title, and the preamble of the folder's other .tex files.
    QVERIFY(writeFile(folder + QStringLiteral("/main.tex"), QByteArrayLiteral("\\input{style}\n\\begin{document}\nBody.\n\\end{document}\n")));
    overview.m_libraryInventoryDirty = false;
    QString created;
    QVERIFY(overview.createSourceFile(folder, QStringLiteral(" my_new-note "), QStringLiteral("md"), &created).isEmpty());
    QCOMPARE(created, folder + QStringLiteral("/my_new-note.md"));
    QFile markdown(created);
    QVERIFY(markdown.open(QIODevice::ReadOnly));
    QCOMPARE(markdown.readAll(), QByteArrayLiteral("# My new note\n\n"));
    QVERIFY(overview.m_libraryInventoryDirty);
    QVERIFY(overview.createSourceFile(folder, QStringLiteral("essay.tex"), QStringLiteral("tex"), &created).isEmpty());
    QFile tex(created);
    QVERIFY(tex.open(QIODevice::ReadOnly));
    QCOMPARE(tex.readAll(), QByteArrayLiteral("\\input{style}\n\\begin{document}\n\n\\end{document}\n"));
    QCOMPARE(overview.createSourceFile(folder, QStringLiteral("essay"), QStringLiteral("tex")), QStringLiteral("“essay.tex” already exists in this folder."));
    QVERIFY(!overview.createSourceFile(folder, QStringLiteral("../escape"), QStringLiteral("md")).isEmpty());
    QVERIFY(!overview.createSourceFile(folder, QStringLiteral(".hidden"), QStringLiteral("md")).isEmpty());
    QVERIFY(!overview.createSourceFile(folder, QStringLiteral(" "), QStringLiteral("md")).isEmpty());
    QVERIFY(!QFileInfo::exists(QFileInfo(folder).dir().filePath(QStringLiteral("escape.md"))));
}

void CommentsOverviewTest::testFailedProjectShowsWarningInLibrary()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString failedOwner = QStringLiteral("docs-import-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const QString healthyOwner = QStringLiteral("docs-import-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    for (const QString &owner : {failedOwner, healthyOwner}) {
        QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/tex/source/main.tex")), QByteArrayLiteral("Source.\n")));
    }
    const QJsonObject failed {{QStringLiteral("id"), QStringLiteral("new:failed")}, {QStringLiteral("name"), QStringLiteral("Failed")},
        {QStringLiteral("source_folders"), QJsonArray {QJsonObject {{QStringLiteral("project"), failedOwner}, {QStringLiteral("folder"), QStringLiteral("Paper")},
            {QStringLiteral("build_error"), QStringLiteral("LaTeX failed")}}}}};
    const QJsonObject healthy {{QStringLiteral("id"), QStringLiteral("new:healthy")}, {QStringLiteral("name"), QStringLiteral("Healthy")},
        {QStringLiteral("source_folders"), QJsonArray {QJsonObject {{QStringLiteral("project"), healthyOwner}, {QStringLiteral("folder"), QStringLiteral("Paper")}}}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {failed, healthy}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), failed));
    QVERIFY(registerCatalogueProject(temporary.path(), healthy));

    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 2);
    const auto project = [tree](const QString &name) -> QTreeWidgetItem * {
        for (int i = 0; i < tree->topLevelItemCount(); ++i)
            if (tree->topLevelItem(i)->text(0) == name) return tree->topLevelItem(i);
        return nullptr;
    };
    QVERIFY(project(QStringLiteral("Failed")) && project(QStringLiteral("Healthy")));
    // Only the failed project is marked; the other keeps syncing and building.
    QCOMPARE(project(QStringLiteral("Failed"))->toolTip(0), QStringLiteral("PDF build failed: LaTeX failed"));
    QVERIFY(project(QStringLiteral("Healthy"))->toolTip(0).isEmpty());

    // A folder note stands only while one of its documents has no built PDF:
    // once every document from the folder is built, an old note is stale.
    const QString owner = QStringLiteral("docs-import-b807");
    const QString note = QStringLiteral("Sources recovered from an interrupted import; selected PDFs still need to be built.");
    const auto document = [&owner](const QString &name) {
        return QJsonObject {{QStringLiteral("filename"), QString(name + QStringLiteral(".pdf"))}, {QStringLiteral("source_file"), QString(owner + QStringLiteral("/tex/source/") + name + QStringLiteral(".tex"))}};
    };
    const auto built = [](QJsonObject record) {
        record.insert(QStringLiteral("published"), true);
        record.insert(QStringLiteral("sha256"), QString(64, QLatin1Char('a')));
        return record;
    };
    const auto models = [&](const QJsonObject &second) {
        return QJsonObject {{QStringLiteral("documents"), QJsonArray {built(document(QStringLiteral("olmo3"))), second,
                                                                      QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("imported"), true}}}}};
    };
    const QJsonObject qwen3 = document(QStringLiteral("qwen3"));
    QCOMPARE(Thoughts::CommentsOverview::currentFolderBuildError(models(qwen3), owner, note), note); // qwen3 not built
    QJsonObject unpublished = built(qwen3);
    unpublished.insert(QStringLiteral("published"), false);
    QCOMPARE(Thoughts::CommentsOverview::currentFolderBuildError(models(unpublished), owner, note), note);
    // The record alone answers, so a project is not marked at startup, before its documents are read.
    QVERIFY(Thoughts::CommentsOverview::currentFolderBuildError(models(built(qwen3)), owner, note).isEmpty());
    // A folder without documents keeps its error: nothing shows it is stale.
    QCOMPARE(Thoughts::CommentsOverview::currentFolderBuildError(models(built(qwen3)), QStringLiteral("docs-other"), note), note);
}

void CommentsOverviewTest::testImportedIgnoreFilesStayWithTheirRepositories()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString first = QStringLiteral("docs-import-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const QString second = QStringLiteral("docs-import-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    for (const QString &owner : {first, second}) {
        QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/tex/source/main.tex")), QByteArrayLiteral("Source.\n")));
        QVERIFY(writeFile(temporary.filePath(owner + QStringLiteral("/.ThoughtIgnore")), QByteArrayLiteral(".git/\n")));
    }
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:models")},
        {QStringLiteral("name"), QStringLiteral("Models")},
        {QStringLiteral("source_folders"), QJsonArray {
            QJsonObject {{QStringLiteral("project"), first}, {QStringLiteral("folder"), QStringLiteral("Git repository")}},
            QJsonObject {{QStringLiteral("project"), second}, {QStringLiteral("folder"), QStringLiteral("Git repository")}}}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));

    Thoughts::CommentsOverview overview(temporary.path());
    overview.resize(1200, 760);
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    auto *sources = tree->topLevelItem(0)->child(1);
    QVERIFY(sources);
    // The default source folder precedes both repositories.
    QCOMPARE(sources->childCount(), 3);
    for (int i = 1; i < sources->childCount(); ++i) QCOMPARE(sources->child(i)->text(0), QStringLiteral("Git repository"));
    // Files appear once the background source inventory completes.
    const auto ignorePaths = [&] {
        QSet<QString> paths;
        auto *current = tree->topLevelItem(0)->child(1);
        for (int i = 0; i < current->childCount(); ++i)
            for (int j = 0; j < current->child(i)->childCount(); ++j)
                if (current->child(i)->child(j)->text(0) == QLatin1String(".ThoughtIgnore"))
                    paths.insert(current->child(i)->child(j)->data(0, Qt::UserRole + 1).toString());
        return paths;
    };
    QTRY_COMPARE(ignorePaths(), QSet<QString>({first + QStringLiteral("/.ThoughtIgnore"), second + QStringLiteral("/.ThoughtIgnore")}));
}

void CommentsOverviewTest::testSourceProjectCatalogueOrder_data()
{
    QTest::addColumn<bool>("hasSegments");
    QTest::addColumn<bool>("markdown");
    QTest::addColumn<bool>("savedProjectId");
    QTest::newRow("verified-components") << true << false << false;
    QTest::newRow("missing-component-map") << false << false << false;
    QTest::newRow("markdown-components") << true << true << false;
    QTest::newRow("saved-project-source-components") << true << false << true;
    QTest::newRow("saved-project-missing-map") << false << false << true;
    QTest::newRow("saved-project-markdown-components") << true << true << true;
}

void CommentsOverviewTest::testSourceProjectCatalogueOrder()
{
    QFETCH(bool, hasSegments);
    QFETCH(bool, markdown);
    QFETCH(bool, savedProjectId);
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString pdfPath = root + QStringLiteral("/current_pdfs/paper.pdf");
    const QByteArray pdf = QByteArrayLiteral("%PDF-order-fixture");
    const QString pdfSha = QString::fromLatin1(QCryptographicHash::hash(pdf, QCryptographicHash::Sha256).toHex());
    QVERIFY(writeFile(pdfPath, pdf));
    const QString first = markdown ? QStringLiteral("docs-paper/markdown/source/z_first.md") : QStringLiteral("docs-paper/tex/source/z_first.tex");
    const QString second = QStringLiteral("docs-paper/tex/source/a_second.tex");
    const QString local = QStringLiteral("docs-paper/tex/source/local.tex");
    const QString imported = QStringLiteral("third_party/2402.pdf");
    const QString anotherImport = QStringLiteral("third_party/517f.pdf");
    for (const QString &source : {first, second, local}) {
        QVERIFY(writeFile(root + QLatin1Char('/') + source, QByteArrayLiteral("Source.\n")));
    }
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/.ThoughtIgnore"), QByteArrayLiteral(".git/\ntmp/\nbuild/\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/README.md"), QByteArrayLiteral("Project readme.\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/build/duplicate.tex"), QByteArrayLiteral("Duplicate.\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/tex/docs_order_manifest.txt"), QByteArrayLiteral("a_second.tex\nz_first.tex\nlocal.tex\n")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(QJsonObject {
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray {QJsonObject {
            {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("project"), QStringLiteral("docs-paper")},
            {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), pdfSha}}}}}).toJson()));
    if (hasSegments) {
        const QJsonArray segments {
            QJsonObject {{QStringLiteral("source_id"), second}, {QStringLiteral("start_page"), 1}, {QStringLiteral("page_count"), 1}},
            QJsonObject {{QStringLiteral("source_id"), first}, {QStringLiteral("start_page"), 2}, {QStringLiteral("page_count"), 1}},
        };
        const QByteArray artifact = QJsonDocument(QJsonObject {
            {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
            {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                {QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("page_count"), 2}, {QStringLiteral("segments"), segments}}}}).toJson();
        const QString digest = QString::fromLatin1(QCryptographicHash::hash(artifact, QCryptographicHash::Sha256).toHex());
        const QString bundle = root + QStringLiteral("/.runtime/target/source-bundle");
        QVERIFY(writeFile(bundle + QStringLiteral("/objects/") + digest, artifact));
        QVERIFY(writeFile(bundle + QStringLiteral("/documents/paper.pdf.json"), QJsonDocument(QJsonObject {
            {QStringLiteral("artifact_sha256"), digest}, {QStringLiteral("pdf_sha256"), pdfSha},
            {QStringLiteral("filename"), QStringLiteral("paper.pdf")}}).toJson()));
    }
    const auto document = [&](const QString &id, bool isImport) {
        return QJsonObject {{QStringLiteral("id"), id}, {QStringLiteral("name"), QFileInfo(id).completeBaseName()},
            {QStringLiteral("filename"), isImport ? QFileInfo(id).fileName() : QStringLiteral("paper.pdf")},
            {QStringLiteral("imported"), isImport}};
    };
    const QString projectId = savedProjectId ? QStringLiteral("new:linked-after-creation") : QStringLiteral("source:docs-paper");
    const QJsonObject project {{QStringLiteral("id"), projectId},
        {QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")},
        {QStringLiteral("target"), QStringLiteral("paper.pdf")}, {QStringLiteral("revision"), 0},
        {QStringLiteral("documents"), QJsonArray {document(first, false), document(second, false), document(imported, true), document(anotherImport, true)}}};
    Thoughts::LibraryProjectStore store(root);
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    // The Project also declares a local source that has not been built yet.
    auto declared = project;
    declared.insert(QStringLiteral("documents"), project.value(QStringLiteral("documents")).toArray() + QJsonObject{{QStringLiteral("id"), local}});
    QVERIFY(registerCatalogueProject(root, declared));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QTRY_COMPARE(tree->topLevelItem(0)->childCount(), 3); // Documents, Sources, Agent conversations
    const auto order = [&] {
        QStringList ids;
        auto *documents = tree->topLevelItem(0)->child(0);
        for (int i = 0; i < documents->childCount(); ++i) {
            const QString id = documents->child(i)->data(0, Qt::UserRole + 112).toString();
            if (!id.isEmpty()) { ids.append(id); }
        }
        return ids;
    };
    QTRY_COMPARE(order().size(), 5);
    QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("Paper"));
    QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole + 114).toString(), QStringLiteral("Project"));
    QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole).toString(), pdfPath);
    QCOMPARE(tree->topLevelItem(0)->data(0, Qt::UserRole + 140).value<QJsonObject>().value(QStringLiteral("id")).toString(), projectId);
    QCOMPARE(tree->topLevelItem(0)->child(1)->text(0), QStringLiteral("Sources"));
    auto *sources = tree->topLevelItem(0)->child(1);
    QCOMPARE(sources->childCount(), 2);
    QVERIFY(childNamed(sources, QStringLiteral("docs-paper")));
    const auto rootNames = [&] {
        QSet<QString> names;
        auto *connected = childNamed(tree->topLevelItem(0)->child(1), QStringLiteral("docs-paper"));
        for (int i = 0; i < connected->childCount(); ++i) names.insert(connected->child(i)->text(0));
        return names;
    };
    QTRY_VERIFY(rootNames().contains(QStringLiteral("README.md")));
    QVERIFY(rootNames().contains(QStringLiteral(".ThoughtIgnore")));
    QVERIFY(rootNames().contains(QStringLiteral("tex")));
    QVERIFY(!rootNames().contains(QStringLiteral("build")));
    const QString screenshot = qEnvironmentVariable("THOUGHTS_CONNECTED_ROOT_SCREENSHOT");
    if (!screenshot.isEmpty() && !markdown && hasSegments && !savedProjectId) {
        overview.resize(1100, 650); tree->expandAll();
        QVERIFY(tree->grab().save(screenshot));
    }
    auto *documentGroup = tree->topLevelItem(0)->child(0);
    for (int i = 0; i < documentGroup->childCount(); ++i) {
        auto *item = documentGroup->child(i);
        const QString id = item->data(0, Qt::UserRole + 112).toString();
        QCOMPARE(item->data(0, Qt::UserRole + 114).toString(), id == local ? QStringLiteral("Not built yet") : QStringLiteral("PDF"));
        if (id == imported) {
            tree->setCurrentItem(item);
            auto *details = overview.findChild<QLabel *>(QStringLiteral("thoughtsLibrarySelectionDetails"));
            QVERIFY(details);
            QVERIFY(details->text().contains(QStringLiteral("2402.pdf")));
        }
    }
    const QStringList expected {first, second, imported, anotherImport, local};
    for (const int column : {0, 3}) {
        for (const Qt::SortOrder direction : {Qt::AscendingOrder, Qt::DescendingOrder}) {
            tree->sortItems(column, direction);
            QCOMPARE(order(), expected);
        }
    }
    // A catalogue reorder must affect existing components as well as imports.
    // A pending order lists every document of the Project.
    QVERIFY(store.enqueue(QStringLiteral("order"), project, {{QStringLiteral("documents"), QJsonArray {anotherImport, second, imported, first, local}}}).isEmpty());
    QCOMPARE(store.pending().last().toObject().value(QStringLiteral("project_id")).toString(), projectId);
    overview.reload();
    QTRY_COMPARE(order(), (QStringList {anotherImport, second, imported, first, local}));
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}, store.pending().last().toObject().value(QStringLiteral("operation_id")).toString()).isEmpty());
    // Keep a desktop reorder of the Project configuration visible before explicit Sync, with local additions.
    const QString configurationPath = Thoughts::ProjectConfiguration::path(root, projectId);
    auto configuration = Thoughts::ProjectConfiguration::read(configurationPath);
    QJsonArray reordered;
    for (const auto &id : {imported, first, second, anotherImport, local})
        for (const auto &value : configuration.value(QStringLiteral("documents")).toArray())
            if (Thoughts::ProjectConfiguration::documentAlias(configuration, value.toObject()) == id) reordered.append(value);
    QCOMPARE(reordered.size(), 5);
    configuration.insert(QStringLiteral("documents"), reordered);
    QVERIFY(Thoughts::ProjectConfiguration::save(configurationPath, configuration).isEmpty());
    overview.reload();
    QTRY_COMPARE(order(), (QStringList {imported, first, second, anotherImport, local}));
    // Sorting by build date keeps the Documents group and the Project order.
    QCOMPARE(tree->headerItem()->text(1), QStringLiteral("Date"));
    for (const Qt::SortOrder direction : {Qt::DescendingOrder, Qt::AscendingOrder}) {
        tree->sortItems(1, direction);
        QCOMPARE(tree->topLevelItem(0)->child(0)->text(0), QStringLiteral("Documents"));
        QCOMPARE(order(), (QStringList {imported, first, second, anotherImport, local}));
    }
}

void CommentsOverviewTest::testProjectAttachmentManifestMutation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString project = QStringLiteral("docs-paper");
    const QString pdfPath = root + QStringLiteral("/current_pdfs/rollouts.pdf");
    const QByteArray pdf = QByteArrayLiteral("%PDF-rollouts");
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(pdf, QCryptographicHash::Sha256).toHex());
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/tex/source/main.tex"), QByteArrayLiteral("source\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/tex/docs_order_manifest.txt"), QByteArrayLiteral("main.tex\n")));
    QVERIFY(writeFile(pdfPath, pdf));
    const QString projectId = QStringLiteral("source:docs-paper");
    QVERIFY(registerSourceProject(root, projectId, QStringLiteral("Paper"), {project}));
    const QString stored = defaultSourceFolder(root, projectId) + QStringLiteral("/rollouts.pdf");
    const QString title = QStringLiteral("GRPO trajectories");

    const Thoughts::ProjectAttachmentMutationResult attached = Thoughts::setProjectAttachment(
        root,
        project,
        pdfPath,
        QStringLiteral("rollouts.pdf"),
        digest,
        title,
        true);
    QVERIFY2(attached.success, qPrintable(attached.error));
    QVERIFY(attached.changed);
    // Attached PDFs are Project inputs stored in the default source folder.
    QCOMPARE(readFile(stored), pdf);
    QString error;
    const QList<Thoughts::ProjectAttachment> attachments = Thoughts::loadProjectAttachments(root, project, &error);
    QVERIFY(error.isEmpty());
    QCOMPARE(attachments.size(), 1);
    QCOMPARE(attachments.constFirst().filename, QStringLiteral("rollouts.pdf"));
    QCOMPARE(attachments.constFirst().sha256, digest);
    QCOMPARE(attachments.constFirst().title, title);
    QCOMPARE(attachments.constFirst().placement, QStringLiteral("append"));

    const Thoughts::ProjectAttachmentMutationResult repeated = Thoughts::setProjectAttachment(
        root,
        project,
        pdfPath,
        QStringLiteral("rollouts.pdf"),
        digest,
        title,
        true);
    QVERIFY(repeated.success);
    QVERIFY(!repeated.changed);

    const Thoughts::ProjectAttachmentMutationResult detached = Thoughts::setProjectAttachment(
        root,
        project,
        pdfPath,
        QStringLiteral("rollouts.pdf"),
        digest,
        title,
        false);
    QVERIFY2(detached.success, qPrintable(detached.error));
    QVERIFY(detached.changed);
    QVERIFY(Thoughts::loadProjectAttachments(root, project, &error).isEmpty());
    QVERIFY(QFileInfo::exists(stored));
}

void CommentsOverviewTest::testTrackedLayoutLoading()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();

    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/beta.json"), QJsonDocument(layout(QStringLiteral("beta"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/invalid.json"), QJsonDocument(QJsonObject {{QStringLiteral("version"), 1}, {QStringLiteral("sources"), QJsonArray()}}).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/beta.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/gamma.pdf"), QByteArrayLiteral("placeholder")));

    QByteArray alpha;
    alpha += jsonLine(comment(QStringLiteral("alpha-1"), QStringLiteral("First tracked comment"), 1, QStringLiteral("2026-07-29T08:00:00Z"), QStringLiteral("anchored")));
    QJsonObject reply = comment(QStringLiteral("alpha-2"), QStringLiteral("Reply text"), 2, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("needs_anchor"));
    reply.insert(QStringLiteral("in_reply_to"), QStringLiteral("alpha-1"));
    alpha += jsonLine(reply);
    QJsonObject deleted = comment(QStringLiteral("alpha-deleted"), QStringLiteral("Deleted"), 3, QStringLiteral("2026-07-30T09:00:00Z"), QStringLiteral("anchored"));
    deleted.insert(QStringLiteral("deleted"), true);
    deleted.insert(QStringLiteral("deleted_at"), QStringLiteral("2026-07-30T09:00:00Z"));
    alpha += jsonLine(deleted);
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), alpha));
    QVERIFY(writeFile(root + QStringLiteral("/comments/gamma_comments.jsonl"), jsonLine(comment(QStringLiteral("gamma-1"), QStringLiteral("PDF-only comment"), 4, QStringLiteral("2026-07-30T09:30:00Z"), QStringLiteral("anchored")))));
    QVERIFY(writeFile(root + QStringLiteral("/comments/unlinked_comments.jsonl"), jsonLine(comment(QStringLiteral("unlinked"), QStringLiteral("Not tracked"), 1, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored")))));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 3);
    QCOMPARE(documents.at(0).stem, QStringLiteral("alpha"));
    QCOMPARE(documents.at(0).state, Thoughts::TrackedCommentDocument::SourceLinked);
    QCOMPARE(documents.at(0).comments.size(), 2);
    QVERIFY(documents.at(0).pdfAvailable);
    QCOMPARE(documents.at(0).comments.at(1).contents, QStringLiteral("Reply text"));
    QVERIFY(documents.at(0).comments.at(1).reply);
    QCOMPARE(documents.at(0).comments.at(1).threadRootId, QStringLiteral("alpha-1"));
    QCOMPARE(documents.at(0).comments.at(1).sourceLine, 12);
    QCOMPARE(documents.at(1).stem, QStringLiteral("beta"));
    QCOMPARE(documents.at(1).state, Thoughts::TrackedCommentDocument::SourceLinked);
    QVERIFY(documents.at(1).comments.isEmpty());
    QCOMPARE(documents.at(2).stem, QStringLiteral("gamma"));
    QCOMPARE(documents.at(2).state, Thoughts::TrackedCommentDocument::PdfOnly);
    QCOMPARE(documents.at(2).comments.size(), 1);
    QCOMPARE(documents.at(2).comments.constFirst().contents, QStringLiteral("PDF-only comment"));

    // The Source doc column: ✓ for a document built from a source, ✗ for a PDF.
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(table);
    QTRY_VERIFY(table->model()->rowCount() >= 2);
    QHash<QString, QString> marks;
    for (int row = 0; row < table->model()->rowCount(); ++row)
        marks.insert(table->model()->index(row, 0).data().toString(), table->model()->index(row, 9).data().toString());
    QCOMPARE(marks.value(QStringLiteral("First tracked comment")), QStringLiteral("✓"));
    QCOMPARE(marks.value(QStringLiteral("PDF-only comment")), QStringLiteral("✗"));
}

void CommentsOverviewTest::testSidecarSyncConflictsRemainVisible()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"),
                      jsonLine(comment(QStringLiteral("canonical"), QStringLiteral("Canonical comment"), 1, QStringLiteral("2026-08-18T08:00:00Z"), QStringLiteral("anchored")))));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.sync-conflict-test.jsonl"),
                      jsonLine(comment(QStringLiteral("conflict"), QStringLiteral("Conflict comment"), 2, QStringLiteral("2026-08-18T09:00:00Z"), QStringLiteral("anchored")))));

    QElapsedTimer loadTimer;
    loadTimer.start();
    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QVERIFY2(loadTimer.elapsed() < 1000,
             qPrintable(QStringLiteral("sidecar conflict scan blocked for %1 ms").arg(loadTimer.elapsed())));
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.constFirst().comments.size(), 2);
    QCOMPARE(documents.constFirst().comments.at(0).id, QStringLiteral("canonical"));
    QCOMPARE(documents.constFirst().comments.at(1).id, QStringLiteral("conflict"));
}

void CommentsOverviewTest::testAgentProposalRowsFiltersAndResolution()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString proposalId = QStringLiteral("agent-remove-aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("Remove this whole sentence.\n")));
    QVERIFY(writeAgentProposalBundle(root, QStringLiteral("alpha.pdf"), proposalId));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.constFirst().comments.size(), 1);
    QVERIFY(documents.constFirst().comments.constFirst().agentProposal);
    QCOMPARE(documents.constFirst().comments.constFirst().bundleId.size(), 64);

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *typeFilter = overview.findChild<QComboBox *>(QStringLiteral("commentsOverviewTypeFilter"));
    auto *acceptButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    auto *rejectButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDisregardProposal"));
    auto *selectedText = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewProposalPreview"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    QVERIFY(table);
    QVERIFY(typeFilter);
    QVERIFY(acceptButton);
    QVERIFY(rejectButton);
    QCOMPARE(rejectButton->text(), QStringLiteral("Reject Proposal"));
    QVERIFY(selectedText);
    QVERIFY(selectedText->isReadOnly());
    QVERIFY(editor);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    typeFilter->setCurrentIndex(typeFilter->findData(QStringLiteral("comment")));
    QTRY_COMPARE(table->model()->rowCount(), 0);
    typeFilter->setCurrentIndex(typeFilter->findData(QStringLiteral("agent_remove")));
    QTRY_COMPARE(table->model()->rowCount(), 1);

    QSignalSpy resolutionSpy(&overview, &Thoughts::CommentsOverview::agentProposalsResolveRequested);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(selectedText->toPlainText(), QStringLiteral("New version: [delete]\n\nOld version: Remove this whole sentence.\n\nMotivation: This sentence is redundant."));
    QVERIFY(!editor->isVisible());
    QTRY_VERIFY(acceptButton->isVisible());
    for (const QString &name : {QStringLiteral("commentsOverviewLocationEnabled"),
                                QStringLiteral("commentsOverviewLocationLabel"), QStringLiteral("commentsOverviewLocationLatitude"),
                                QStringLiteral("commentsOverviewOpenCodex"), QStringLiteral("commentsOverviewChatGptUrl")}) {
        auto *field = overview.findChild<QWidget *>(name);
        QVERIFY2(field, qPrintable(name));
        QVERIFY2(!field->isVisible(), qPrintable(name));
    }
    {
        const QStringList clipped = clippedDetailsControls(overview);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join(QStringLiteral(", "))));
    }
    QTest::mouseClick(acceptButton, Qt::LeftButton);
    QCOMPARE(resolutionSpy.size(), 1);
    QCOMPARE(resolutionSpy.constFirst().at(1).toString(), QStringLiteral("accept"));
    QCOMPARE(resolutionSpy.constFirst().at(2).toStringList(), QStringList {proposalId});

    // Build selection does not remove proposals from a connected source file.
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/support/thoughts_document_options.txt"),
                      QByteArrayLiteral("{\"schema_version\":1,\"excluded_documents\":[\"docs-test/tex/source/main.tex\"]}")));
    QCOMPARE(Thoughts::loadTrackedCommentDocuments(root).constFirst().comments.size(), 1);
    QVERIFY(QFile::remove(root + QStringLiteral("/docs-test/tex/source/main.tex")));
    QCOMPARE(Thoughts::loadTrackedCommentDocuments(root).constFirst().comments.size(), 0);
}

void CommentsOverviewTest::testUnpublishedRegisteredProposalVisibleWithoutPdf()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString source = QStringLiteral("docs-models/tex/source/olmo3.tex");
    const QString proposalId = QStringLiteral("agent-local-aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QByteArray pending = QByteArrayLiteral("\\agentProposedEdit[created=2026-09-24T13:27:50.195Z,conversation_id=0190d000-0000-7000-8000-00000000000d]{agent-local-aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}{Correct the link.\n}{Old text.\n}{New text.\n}\n");
    QVERIFY(writeFile(QDir(root).filePath(source), pending));
    const QJsonObject snapshot {{QStringLiteral("proposal_id"), proposalId}, {QStringLiteral("source_file"), source},
                                {QStringLiteral("created_at"), QStringLiteral("2026-09-24T13:27:50.195Z")},
                                {QStringLiteral("comment"), QStringLiteral("Correct the link.")},
                                {QStringLiteral("before"), QStringLiteral("Old text.\n")},
                                {QStringLiteral("after"), QStringLiteral("New text.\n")}};
    QString error;
    QVERIFY2(Thoughts::ProposalHistory::append(root, QJsonArray {Thoughts::ProposalHistory::event(
                 QStringLiteral("first_seen"), snapshot, QString(), QString())}, &error), qPrintable(error));

    const auto documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.first().comments.size(), 1);
    const auto &proposal = documents.first().comments.first();
    QCOMPARE(proposal.id, proposalId);
    QCOMPARE(proposal.sourceFile, source);
    QCOMPARE(proposal.sourceLine, 1);
    QVERIFY(proposal.liveSourceProposal);
    QVERIFY(!proposal.pdfAvailable);

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    QVERIFY(table);
    QVERIFY(accept);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(accept->isVisible());
    QTest::mouseClick(accept, Qt::LeftButton);
    QTRY_COMPARE(table->model()->rowCount(), 0);
    QFile accepted(QDir(root).filePath(source));
    QVERIFY(accepted.open(QIODevice::ReadOnly));
    QCOMPARE(accepted.readAll(), QByteArrayLiteral("New text.\n\n"));
    const auto resolved = Thoughts::loadTrackedCommentDocuments(root);
    QVERIFY(resolved.isEmpty());
}

void CommentsOverviewTest::testConnectedSourceProposalsUseLibraryProjectBeforeSync()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString first = QStringLiteral("docs-import-one/tex/source/olmo3.tex");
    const QString second = QStringLiteral("docs-import-two/tex/source/qwen3.tex");
    const QString separate = QStringLiteral("docs-models/tex/source/olmo3.tex");
    const QString ignored = QStringLiteral("docs-import-one/tex/source/ignored.tex");
    const auto proposal = [](const QString &id) {
        return QStringLiteral("\\agentProposedEdit[created=2026-09-24T13:27:50.195Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{%1}{Fix the link.\n}{Old text.\n}{New text.\n}\n").arg(id).toUtf8();
    };
    const QString firstId = QStringLiteral("agent-local-aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    const QString secondId = QStringLiteral("agent-local-bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const QString separateId = QStringLiteral("agent-local-cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    QVERIFY(writeFile(QDir(root).filePath(first), proposal(firstId)));
    QVERIFY(writeFile(QDir(root).filePath(second), proposal(secondId)));
    QVERIFY(writeFile(QDir(root).filePath(separate), proposal(separateId)));
    QVERIFY(writeFile(QDir(root).filePath(ignored), proposal(QStringLiteral("agent-local-dddddddd-dddd-4ddd-8ddd-dddddddddddd"))));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("docs-import-one/.ThoughtIgnore")), QByteArrayLiteral(".git/\ntex/source/ignored.tex\n")));
    QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("docs-import-one/tex/support/thoughts_document_options.txt")),
                      QByteArrayLiteral("{\"schema_version\":1,\"selected_documents\":[]}")));
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:models")},
                               {QStringLiteral("name"), QStringLiteral("Models")},
                               {QStringLiteral("source_folders"), QJsonArray {QJsonObject {{QStringLiteral("project"), QStringLiteral("docs-import-one")}},
                                                                                QJsonObject {{QStringLiteral("project"), QStringLiteral("docs-import-two")}}}}};
    Thoughts::LibraryProjectStore store(root);
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerSourceProject(root, project.value(QStringLiteral("id")).toString(), project.value(QStringLiteral("name")).toString(), {QStringLiteral("docs-import-one"), QStringLiteral("docs-import-two")}));
    QString error;
    const QJsonObject snapshot {{QStringLiteral("proposal_id"), separateId}, {QStringLiteral("source_file"), separate}};
    QVERIFY2(Thoughts::ProposalHistory::append(root, QJsonArray {Thoughts::ProposalHistory::event(
                 QStringLiteral("first_seen"), snapshot, QString(), QString())}, &error), qPrintable(error));

    const auto documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 2);
    const auto model = std::find_if(documents.cbegin(), documents.cend(), [](const auto &document) {
        return document.libraryProjectId == QLatin1String("new:models");
    });
    QVERIFY(model != documents.cend());
    QCOMPARE(model->displayName, QStringLiteral("Models"));
    QVERIFY(model->sourceProposalOnly);
    QCOMPARE(model->comments.size(), 2);
    QSet<QString> ids;
    for (const auto &comment : model->comments) ids.insert(comment.id);
    QCOMPARE(ids, QSet<QString>({firstId, secondId}));
    const auto unassigned = std::find_if(documents.cbegin(), documents.cend(), [](const auto &document) {
        return document.displayName == QLatin1String("No project");
    });
    QVERIFY(unassigned != documents.cend());
    QCOMPARE(unassigned->comments.size(), 1);
    QCOMPARE(unassigned->comments.first().id, separateId);

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(tree);
    QTRY_VERIFY([&] {
        for (QTreeWidgetItemIterator it(tree); *it; ++it) {
            if ((*it)->text(0) == QLatin1String("Models") && (*it)->text(2) == QLatin1String("2")) return true;
        }
        return false;
    }());
}

void CommentsOverviewTest::testAgentProposalRowsRejectStalePdfArtifact()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("current PDF")));
    QVERIFY(writeAgentProposalBundle(root,
                                     QStringLiteral("alpha.pdf"),
                                     QStringLiteral("stale-proposal"),
                                     QString(64, QLatin1Char('a'))));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QVERIFY(documents.constFirst().comments.isEmpty());
}

void CommentsOverviewTest::testUnplacedCommentDoesNotBlockPreparation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QDir root(temporary.path());
    QVERIFY(root.mkpath(QStringLiteral("current_pdfs")));
    QVERIFY(root.mkpath(QStringLiteral("comments")));
    QFile pdf(root.filePath(QStringLiteral("current_pdfs/test.pdf")));
    QVERIFY(pdf.open(QIODevice::WriteOnly));
    pdf.write("%PDF-test");
    pdf.close();
    QFile sidecar(root.filePath(QStringLiteral("comments/test_comments.jsonl")));
    QVERIFY(sidecar.open(QIODevice::WriteOnly));
    const QByteArray record = R"({"schema_version":2,"id":"unplaced","revision":1,"updated_at":"2026-09-15T12:00:00Z","origin_id":"test","page":17,"subtype":"Text","rect":[],"fragments":[],"contents":"Keep this comment","anchor_status":"needs_anchor"})";
    sidecar.write(record + '\n');
    sidecar.close();
    Okular::DocsCommentSynchronizer synchronizer({.docsRoot = root.path()});
    const auto result = synchronizer.prepareSidecar(QStringLiteral("test"), [](const QString &, int, QString *) {
        return std::optional<Okular::DocsCommentSynchronizer::PageGeometry>({612.0, 792.0, 0});
    });
    QVERIFY2(result.success, qPrintable(result.failureReason));
    QCOMPARE(result.anchorAttempts, 0);
    QVERIFY(sidecar.open(QIODevice::ReadOnly));
    const QJsonObject retained = QJsonDocument::fromJson(sidecar.readAll().trimmed()).object();
    QCOMPARE(retained.value(QStringLiteral("id")).toString(), QStringLiteral("unplaced"));
    QCOMPARE(retained.value(QStringLiteral("contents")).toString(), QStringLiteral("Keep this comment"));
    QCOMPARE(retained.value(QStringLiteral("anchor_status")).toString(), QStringLiteral("needs_anchor"));
    QVERIFY(!retained.value(QStringLiteral("deleted")).toBool());
}

void CommentsOverviewTest::testCanonicalCommentRecordsOwnCatalogue()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("current PDF")));
    const auto record = [](const QString &id, bool deleted) {
        return QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), id},
            {QStringLiteral("thread_root_id"), id}, {QStringLiteral("revision"), 2},
            {QStringLiteral("contents"), QStringLiteral("Canonical text")}, {QStringLiteral("deleted"), deleted},
            {QStringLiteral("origin"), QJsonObject {{QStringLiteral("collection"), QStringLiteral("alpha")}}}};
    };
    QJsonArray markers;
    for (const QString &id : {QStringLiteral("active"), QStringLiteral("deleted"), QStringLiteral("marker-only")}) {
        markers.append(QJsonObject {{QStringLiteral("id"), id}, {QStringLiteral("contents"), QStringLiteral("Stale source text")},
            {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
            {QStringLiteral("source_sha256"), QString(64, QLatin1Char('b'))}, {QStringLiteral("line"), 42}});
    }
    QVERIFY(writeReviewCommentBundle(root, QStringLiteral("alpha.pdf"), markers));
    const QJsonObject projections {
        {QStringLiteral("logical_comment\ncomments\nactive"), record(QStringLiteral("active"), false)},
        {QStringLiteral("logical_comment\ncomments\ndeleted"), record(QStringLiteral("deleted"), true)},
    };
    const auto documents = Thoughts::loadTrackedCommentDocuments(root, projections);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.first().comments.size(), 1);
    QCOMPARE(documents.first().comments.first().id, QStringLiteral("active"));
    QCOMPARE(documents.first().comments.first().contents, QStringLiteral("Canonical text"));
    QVERIFY(documents.first().comments.first().logicalComment);
}

void CommentsOverviewTest::testPdfOnlyCommentOriginPage()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QByteArray pdf("original PDF");
    const QString path = root + QStringLiteral("/current_pdfs/alpha.pdf");
    QVERIFY(writeFile(path, pdf));
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(pdf, QCryptographicHash::Sha256).toHex());
    QJsonObject origin {{QStringLiteral("collection"), QStringLiteral("alpha")},
                        {QStringLiteral("pdf_sha256"), digest}, {QStringLiteral("page_index"), 206}};
    QJsonObject parent {{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), QStringLiteral("parent")},
                        {QStringLiteral("thread_root_id"), QStringLiteral("parent")}, {QStringLiteral("subtype"), QStringLiteral("StrikeOut")},
                        {QStringLiteral("origin"), origin}};
    QJsonObject reply {{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), QStringLiteral("reply")},
                       {QStringLiteral("thread_root_id"), QStringLiteral("parent")}, {QStringLiteral("parent_id"), QStringLiteral("parent")},
                       {QStringLiteral("subtype"), QStringLiteral("Text")}, {QStringLiteral("contents"), QStringLiteral("Replacement sentence.")}};
    const auto projections = [&] {
        return QJsonObject {{QStringLiteral("logical_comment\ncomments\nparent"), parent},
                            {QStringLiteral("logical_comment\ncomments\nreply"), reply}};
    };
    const auto checkPages = [&](int expected) {
        const auto documents = Thoughts::loadTrackedCommentDocuments(root, projections());
        QCOMPARE(documents.size(), 1);
        QCOMPARE(documents.first().comments.size(), 2);
        for (const auto &comment : documents.first().comments) {
            QCOMPARE(comment.page, expected);
            QVERIFY(comment.pdfLocations.isEmpty());
        }
    };
    checkPages(207); // Includes an unsynced reply without its own origin.
    QVERIFY(writeLogicalCommentBundle(root, QStringLiteral("alpha.pdf"), QJsonArray {parent, reply}));
    {
        Thoughts::CommentsOverview overview(root);
        overview.show();
        auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
        QVERIFY(table);
        QTRY_COMPARE(table->model()->rowCount(), 1);
        QCOMPARE(table->model()->index(0, 2).data().toString(), QStringLiteral("207"));
        // A PDF without a source: ✗ in the Source doc column.
        QCOMPARE(table->model()->index(0, 9).data().toString(), QStringLiteral("✗"));
        const auto parentIndex = table->model()->index(0, 0);
        QCOMPARE(table->model()->rowCount(parentIndex), 1);
        QCOMPARE(table->model()->index(0, 2, parentIndex).data().toString(), QStringLiteral("207"));
        table->selectionModel()->setCurrentIndex(parentIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        auto *metadata = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewMetadata"));
        QVERIFY(metadata);
        QTRY_VERIFY(metadata->text().startsWith(QStringLiteral("Project: ")));
        QVERIFY(metadata->text().contains(QStringLiteral("\nDocument: alpha · Page 207")));
        QVERIFY(metadata->text().endsWith(QStringLiteral("\nLinking status: Unlinked PDF")));
    }
    QVERIFY(writeFile(path, QByteArrayLiteral("changed PDF")));
    checkPages(0);
    QVERIFY(writeFile(path, pdf));
    origin.remove(QStringLiteral("pdf_sha256"));
    parent.insert(QStringLiteral("origin"), origin);
    checkPages(0);
    origin.insert(QStringLiteral("pdf_sha256"), digest);
    origin.insert(QStringLiteral("page_index"), -1);
    parent.insert(QStringLiteral("origin"), origin);
    checkPages(0);
    origin.insert(QStringLiteral("page_index"), 0);
    parent.insert(QStringLiteral("origin"), origin);
    checkPages(1);
}

void CommentsOverviewTest::testLogicalCommentConsolidatesRevisionBoundLocations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString alphaFilename = QStringLiteral("alpha.pdf");
    const QString betaFilename = QStringLiteral("beta.pdf");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/") + alphaFilename, QByteArrayLiteral("alpha revision")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/") + betaFilename, QByteArrayLiteral("beta revision")));
    const QString alphaSha256 = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("alpha revision"), QCryptographicHash::Sha256).toHex());
    const QString betaSha256 = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("beta revision"), QCryptographicHash::Sha256).toHex());
    const QString commentId = QStringLiteral("global-comment-1");
    const QJsonArray locations {
        QJsonObject {{QStringLiteral("status"), QStringLiteral("exact")},
                     {QStringLiteral("page"), 6},
                     {QStringLiteral("point"), QJsonArray {0.25, 0.40}},
                     {QStringLiteral("pdf_filename"), alphaFilename},
                     {QStringLiteral("pdf_sha256"), alphaSha256},
                     {QStringLiteral("source_sha256"), QString(64, QLatin1Char('a'))}},
        QJsonObject {{QStringLiteral("status"), QStringLiteral("exact")},
                     {QStringLiteral("page"), 803},
                     {QStringLiteral("point"), QJsonArray {0.30, 0.45}},
                     {QStringLiteral("pdf_filename"), betaFilename},
                     {QStringLiteral("pdf_sha256"), betaSha256},
                     {QStringLiteral("source_sha256"), QString(64, QLatin1Char('b'))}},
    };
    const QJsonObject logical {{QStringLiteral("schema_version"), 1},
                               {QStringLiteral("comment_id"), commentId},
                               {QStringLiteral("thread_root_id"), commentId},
                               {QStringLiteral("parent_id"), QJsonValue::Null},
                               {QStringLiteral("revision"), 1},
                               {QStringLiteral("created_at"), QStringLiteral("2026-08-27T08:00:00Z")},
                               {QStringLiteral("updated_at"), QStringLiteral("2026-08-27T08:00:00Z")},
                               {QStringLiteral("origin_id"), QStringLiteral("thoughts-android")},
                               {QStringLiteral("deleted"), false},
                               {QStringLiteral("contents"), QStringLiteral("One comment in two generated PDFs")},
                               {QStringLiteral("author"), QStringLiteral("Alex")},
                               {QStringLiteral("subtype"), QStringLiteral("Text")},
                               {QStringLiteral("selected_text"), QStringLiteral("shared source")},
                               {QStringLiteral("color"), QJsonArray()},
                               {QStringLiteral("priority"), QStringLiteral("normal")},
                               {QStringLiteral("location"), QJsonValue::Null},
                               {QStringLiteral("chatgpt_url"), QString()},
                               {QStringLiteral("origin"),
                                QJsonObject {{QStringLiteral("collection"), QStringLiteral("alpha")},
                                             {QStringLiteral("document_id"), QStringLiteral("alpha-document")},
                                             {QStringLiteral("pdf_sha256"), alphaSha256},
                                             {QStringLiteral("page_index"), 5},
                                             {QStringLiteral("rect"), QJsonArray {0.2, 0.3, 0.4, 0.5}},
                                             {QStringLiteral("fragments"), QJsonArray()}}},
                               {QStringLiteral("scope"),
                                QJsonObject {{QStringLiteral("state"), QStringLiteral("source_linked")},
                                             {QStringLiteral("anchor_id"), commentId},
                                             {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
                                             {QStringLiteral("source_line"), 42},
                                             {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}}},
                               {QStringLiteral("locations"), locations}};
    QVERIFY(writeLogicalCommentBundle(root, alphaFilename, QJsonArray {logical}));
    QVERIFY(writeLogicalCommentBundle(root, betaFilename, QJsonArray {logical}));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(std::accumulate(documents.cbegin(), documents.cend(), 0, [](int count, const Thoughts::TrackedCommentDocument &document) {
                 return count + document.comments.size();
             }),
             1);
    const auto containing = std::find_if(documents.cbegin(), documents.cend(), [](const Thoughts::TrackedCommentDocument &document) {
        return !document.comments.isEmpty();
    });
    QVERIFY(containing != documents.cend());
    QVERIFY(containing->comments.constFirst().logicalComment);
    QCOMPARE(containing->comments.constFirst().pdfLocations.size(), 2);
    // A source link survives the absence of a mapping for the current PDF.
    auto unplaced = logical;
    unplaced.insert(QStringLiteral("locations"), QJsonArray());
    QVERIFY(writeLogicalCommentBundle(root, alphaFilename, QJsonArray{unplaced}));
    QVERIFY(writeLogicalCommentBundle(root, betaFilename, QJsonArray()));
    {
        Thoughts::CommentsOverview missingPlacement(root);
        missingPlacement.show();
        auto *table = missingPlacement.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
        QVERIFY(table);
        QTRY_COMPARE(table->model()->rowCount(), 1);
        QCOMPARE(table->model()->index(0, 5).data().toString(), QStringLiteral("Linked but PDF-placement failed"));
    }
    QVERIFY(writeLogicalCommentBundle(root, alphaFilename, QJsonArray{logical}));
    QVERIFY(writeLogicalCommentBundle(root, betaFilename, QJsonArray{logical}));
    QJsonObject deleted = logical;
    deleted.insert(QStringLiteral("deleted"), true);
    deleted.insert(QStringLiteral("revision"), 2);
    const QString projectionKey = QStringLiteral("logical_comment\ncomments\n") + commentId;
    // Keep both cached bundles unchanged: the deletion must beat both old copies.
    for (int refresh = 0; refresh < 2; ++refresh) {
        if (refresh == 1) {
            // A refreshed bundle can omit the logical comment while the old
            // annotation projection still exists in the repository cache.
            QVERIFY(writeLogicalCommentBundle(root, alphaFilename, QJsonArray()));
            QVERIFY(writeLogicalCommentBundle(root, betaFilename, QJsonArray()));
        }
        const QJsonObject legacy {{QStringLiteral("id"), commentId},
                                  {QStringLiteral("subtype"), QStringLiteral("Text")},
                                  {QStringLiteral("contents"), logical.value(QStringLiteral("contents"))},
                                  {QStringLiteral("page"), 6}};
        const QJsonObject projections {{projectionKey, deleted},
                                       {QStringLiteral("annotation\nalpha\n") + commentId, legacy}};
        const auto afterDeletion = Thoughts::loadTrackedCommentDocuments(root, projections);
        for (const auto &document : afterDeletion) {
            QVERIFY(document.comments.isEmpty());
        }
    }

    QVERIFY(writeLogicalCommentBundle(root, alphaFilename, QJsonArray {logical}));
    QVERIFY(writeLogicalCommentBundle(root, betaFilename, QJsonArray {logical}));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    QVERIFY(table);
    QVERIFY(editor);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QVERIFY(table->model()->index(0, 1).data().toString().contains(QStringLiteral("p. 6")));
    QVERIFY(table->model()->index(0, 1).data().toString().contains(QStringLiteral("p. 803")));
    QCOMPARE(table->model()->index(0, 5).data().toString(), QStringLiteral("Linked"));
    // Plain PDFs (no layout names a source): ✗ in the Source doc column.
    QCOMPARE(table->model()->index(0, 9).data().toString(), QStringLiteral("✗"));
    QVERIFY(!table->model()->index(0, 5).data().toString().contains(QStringLiteral("projected"), Qt::CaseInsensitive));

    QSignalSpy locationActivated(&overview, &Thoughts::CommentsOverview::logicalCommentLocationActivated);
    QTimer::singleShot(0, [] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        QCOMPARE(menu->actions().size(), 2);
        menu->setActiveAction(menu->actions().last());
        QTest::keyClick(menu, Qt::Key_Return);
    });
    overview.activateComment(table->model()->index(0, 1));
    QCOMPARE(locationActivated.size(), 1);
    QCOMPARE(locationActivated.first().at(0).toString(), root + QStringLiteral("/current_pdfs/") + betaFilename);
    QCOMPARE(locationActivated.first().at(2).toInt(), 803);
    QCOMPARE(QJsonObject::fromVariantMap(locationActivated.first().at(3).toMap()), locations.last().toObject());

    QSignalSpy mutationSpy(&overview, &Thoughts::CommentsOverview::logicalCommentMutationRequested);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    editor->setPlainText(QStringLiteral("Edited once globally"));
    QVERIFY(QMetaObject::invokeMethod(&overview, "saveSelectedComment"));
    QCOMPARE(mutationSpy.size(), 1);
    const QVariantMap payload = mutationSpy.constFirst().at(0).toMap();
    QCOMPARE(payload.value(QStringLiteral("comment_id")).toString(), commentId);
    QCOMPARE(payload.value(QStringLiteral("revision")).toLongLong(), 2);
    QCOMPARE(payload.value(QStringLiteral("contents")).toString(), QStringLiteral("Edited once globally"));
    QVERIFY(!payload.contains(QStringLiteral("locations")));

    auto *replyButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewReply"));
    QVERIFY(replyButton);
    QVERIFY(replyButton->isEnabled());
    QSignalSpy replyRequested(&overview, &Thoughts::CommentsOverview::commentReplyRequested);
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
                QPlainTextEdit *replyEditor = dialog->findChild<QPlainTextEdit *>();
                QVERIFY(replyEditor);
                replyEditor->setPlainText(QStringLiteral("Logical reply"));
                dialog->accept();
                return;
            }
        }
        QFAIL("The reply editor did not open.");
    });
    QTest::mouseClick(replyButton, Qt::LeftButton);
    QCOMPARE(replyRequested.size(), 1);
    QCOMPARE(replyRequested.constFirst().at(1).toString(), commentId);
    QCOMPARE(replyRequested.constFirst().at(3).toString(), QStringLiteral("Logical reply"));
    const QVariantMap replyPayload = replyRequested.constFirst().at(4).toMap();
    QVERIFY(replyPayload.value(QStringLiteral("comment_id")).toString().startsWith(QStringLiteral("thoughts-")));
    QCOMPARE(replyPayload.value(QStringLiteral("thread_root_id")).toString(), commentId);
    QCOMPARE(replyPayload.value(QStringLiteral("parent_id")).toString(), commentId);
    QCOMPARE(replyPayload.value(QStringLiteral("revision")).toInt(), 1);
    QCOMPARE(replyPayload.value(QStringLiteral("contents")).toString(), QStringLiteral("Logical reply"));
    QCOMPARE(replyPayload.value(QStringLiteral("scope")).toMap().value(QStringLiteral("reason")).toString(), QStringLiteral("inherits_thread_locations"));
    QVERIFY(!replyPayload.contains(QStringLiteral("locations")));
    overview.completeReply(root + QStringLiteral("/current_pdfs/alpha.pdf"), commentId, {{QStringLiteral("success"), false}, {QStringLiteral("error"), QStringLiteral("Expected test failure")}});
}

void CommentsOverviewTest::testAgentEditProposalSchemaThreeLoading()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString proposalId = QStringLiteral("objective-label-20260820");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("old and new\n")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("alpha.pdf"), proposalId));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);

    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.constFirst().comments.size(), 1);
    const Thoughts::TrackedComment &proposal = documents.constFirst().comments.constFirst();
    QCOMPARE(proposal.proposalKind, QStringLiteral("edit"));
    QCOMPARE(proposal.selectedText, QStringLiteral("Standard SDPO → On-policy SDPO"));
    QCOMPARE(proposal.proposalBeforePreview, QStringLiteral("Standard SDPO"));
    QCOMPARE(proposal.proposalAfterPreview, QStringLiteral("On-policy SDPO"));
    QCOMPARE(proposal.contents, QStringLiteral("The objective label was stale."));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *selectedText = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewProposalPreview"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    QVERIFY(table);
    QVERIFY(selectedText);
    QVERIFY(selectedText->isReadOnly());
    QVERIFY(editor);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(selectedText->toPlainText(), QStringLiteral("New version: On-policy SDPO\n\nOld version: Standard SDPO\n\nMotivation: The objective label was stale."));
    QVERIFY(!editor->isVisible());
}

void CommentsOverviewTest::testProofCarryingProposalShowsCompletePlacementEvidence()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString proposalId = QStringLiteral("complete-multiline-edit-20260827");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("new text\n")));
    QVERIFY(writeAgentEditProposalBundle(root,
                                         QStringLiteral("alpha.pdf"),
                                         proposalId,
                                         QStringLiteral("Old conclusion."),
                                         QStringLiteral("New complete conclusion."),
                                         false,
                                         true));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.constFirst().comments.size(), 1);
    const Thoughts::TrackedComment &proposal = documents.constFirst().comments.constFirst();
    QCOMPARE(proposal.correspondence.value(QStringLiteral("state")).toString(), QStringLiteral("exact_text"));
    QCOMPARE(proposal.correspondence.value(QStringLiteral("locations")).toArray().first().toObject().value(QStringLiteral("regions")).toArray().size(), 2);

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *evidence = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewCorrespondenceEvidence"));
    auto *confirm = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewConfirmCorrespondence"));
    QVERIFY(table);
    QVERIFY(evidence);
    QVERIFY(confirm);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0),
                                             QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(evidence->isVisible());
    QVERIFY(evidence->toPlainText().contains(QStringLiteral("exact_text")));
    QVERIFY(evidence->toPlainText().contains(QStringLiteral("2 region(s)")));
    QVERIFY(confirm->isEnabled());
    QSignalSpy decisionSpy(&overview, &Thoughts::CommentsOverview::reviewCorrespondenceReviewRequested);
    QTest::mouseClick(confirm, Qt::LeftButton);
    QTRY_COMPARE(decisionSpy.size(), 1);
    QCOMPARE(decisionSpy.constFirst().constFirst().toMap().value(QStringLiteral("review_key")).toString(),
             QStringLiteral("proposal:") + proposalId);
}

void CommentsOverviewTest::testAgentAdditionProposalDetailUsesEmptyMarker()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString proposalId = QStringLiteral("addition-20260823");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("new text\n")));
    const QString replacement = QStringLiteral("Inserted sentence.\n").repeated(300);
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("alpha.pdf"), proposalId, QString(), replacement));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *selectedText = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewProposalPreview"));
    QVERIFY(table);
    QVERIFY(selectedText);
    QVERIFY(selectedText->isReadOnly());
    QTRY_COMPARE(table->model()->rowCount(), 1);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(selectedText->toPlainText(), QStringLiteral("New version: %1\n\nOld version: [empty]\n\nMotivation: The objective label was stale.").arg(replacement));
    QTRY_VERIFY(selectedText->verticalScrollBar()->maximum() > 0);
    selectedText->verticalScrollBar()->setValue(selectedText->verticalScrollBar()->maximum());
    QVERIFY(selectedText->verticalScrollBar()->value() > 0);
}

void CommentsOverviewTest::testProjectCommentLoadIgnoresBrokenOtherProject()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("alpha PDF")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/beta.pdf"), QByteArrayLiteral("beta PDF")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{broken}{unfinished}")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("alpha.pdf"), QStringLiteral("alpha-proposal")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("beta.pdf"), QStringLiteral("beta-proposal"), QStringLiteral("old"), QStringLiteral("new"), true));
    bool valid = false;
    const auto alpha = Thoughts::loadTrackedCommentDocuments(root, {}, &valid, QStringLiteral("alpha.pdf"));
    QVERIFY(valid);
    QCOMPARE(alpha.size(), 1);
    QCOMPARE(alpha.first().stem, QStringLiteral("alpha"));
    QCOMPARE(alpha.first().comments.size(), 1);
    QCOMPARE(alpha.first().comments.first().id, QStringLiteral("alpha-proposal"));
    // A broken wrapper is listed with its issue code; it no longer fails the load (spec §6).
    const auto all = Thoughts::loadTrackedCommentDocuments(root, {}, &valid);
    QVERIFY(valid);
    QCOMPARE(all.size(), 2);
    const auto healthy = std::find_if(all.begin(), all.end(), [](const auto &doc) { return doc.stem == QLatin1String("alpha"); });
    QVERIFY(healthy != all.end());
    QCOMPARE(healthy->comments, alpha.first().comments);
    const auto broken = std::find_if(all.begin(), all.end(), [](const auto &doc) { return doc.stem == QLatin1String("beta"); });
    QVERIFY(broken != all.end());
    QVERIFY(broken->error.isEmpty());
    QCOMPARE(broken->comments.size(), 1);
    QCOMPARE(broken->comments.first().proposalIssue, QStringLiteral("missing_argument"));
    QCOMPARE(broken->comments.first().sourceLine, 1);
    QVERIFY(broken->comments.first().agentProposal);
}

void CommentsOverviewTest::testProposalPageSkipsLaTeXCopies()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString filename = QStringLiteral("alpha.pdf");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/") + filename, QByteArrayLiteral("alpha PDF")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("Plain text.\n")));
    const QString pdfSha256 = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("alpha PDF"), QCryptographicHash::Sha256).toHex());

    // A renamed heading is also typeset in the contents on page 1; the server
    // marks that copy, in legacy fragments and in correspondence locations.
    const auto fragment = [&filename](int page, QJsonValue duplicate) {
        QJsonObject result {{QStringLiteral("pdf_filename"), filename},
                            {QStringLiteral("page"), page},
                            {QStringLiteral("left"), 72.0},
                            {QStringLiteral("top"), 100.0},
                            {QStringLiteral("right"), 300.0},
                            {QStringLiteral("bottom"), 112.0}};
        if (!duplicate.isUndefined()) result.insert(QStringLiteral("duplicate"), duplicate);
        return result;
    };
    const auto location = [&](int page, QJsonValue duplicate) {
        QJsonObject result {{QStringLiteral("pdf_filename"), filename},
                            {QStringLiteral("pdf_sha256"), pdfSha256},
                            {QStringLiteral("page_index"), page - 1},
                            {QStringLiteral("page"), page},
                            {QStringLiteral("role"), QStringLiteral("after")},
                            {QStringLiteral("placement_kind"), QStringLiteral("exact_text")},
                            {QStringLiteral("regions"), QJsonArray {QJsonArray {0.1, 0.2, 0.7, 0.22}}}};
        if (!duplicate.isUndefined()) result.insert(QStringLiteral("duplicate"), duplicate);
        return result;
    };
    const auto proposal = [&](const QString &id, const QJsonArray &fragments) {
        return QJsonObject {
            {QStringLiteral("id"), id},
            {QStringLiteral("kind"), QStringLiteral("edit")},
            {QStringLiteral("comment"), QStringLiteral("Rename the section.")},
            {QStringLiteral("body_preview"), QStringLiteral("Old heading")},
            {QStringLiteral("before"), QJsonObject {{QStringLiteral("preview"), QStringLiteral("Old heading")}}},
            {QStringLiteral("after"), QJsonObject {{QStringLiteral("preview"), QStringLiteral("New heading")}}},
            {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
            {QStringLiteral("source_sha256"), QString(64, QLatin1Char('b'))},
            {QStringLiteral("line"), 1},
            {QStringLiteral("parent_id"), QJsonValue::Null},
            {QStringLiteral("suppressed"), false},
            {QStringLiteral("pdf_filenames"), QJsonArray {filename}},
            {QStringLiteral("placement"), QJsonObject {{QStringLiteral("status"), QStringLiteral("anchored")}, {QStringLiteral("quad_fragments"), fragments}}},
        };
    };
    const auto correspondence = [](const QString &id, const QJsonArray &locations) {
        return QJsonObject {{QStringLiteral("schema_version"), 2},
                            {QStringLiteral("review_key"), QString(QStringLiteral("proposal:") + id)},
                            {QStringLiteral("kind"), QStringLiteral("agent_edit")},
                            {QStringLiteral("item_id"), id},
                            {QStringLiteral("state"), QStringLiteral("exact_text")},
                            {QStringLiteral("locations"), locations}};
    };
    const QJsonArray proposals {
        proposal(QStringLiteral("legacy-heading"), QJsonArray {fragment(1, true), fragment(3, false)}),
        proposal(QStringLiteral("legacy-unflagged"), QJsonArray {fragment(2, QJsonValue::Undefined), fragment(3, QJsonValue::Undefined)}),
        proposal(QStringLiteral("linked-heading"), {}),
        proposal(QStringLiteral("linked-copies-only"), {}),
    };
    const QJsonArray correspondences {
        correspondence(QStringLiteral("linked-heading"), QJsonArray {location(1, true), location(4, false)}),
        correspondence(QStringLiteral("linked-copies-only"), QJsonArray {location(2, true), location(5, true)}),
    };
    QJsonArray proposalObjects;
    for (const QJsonValue &value : proposals) {
        proposalObjects.append(QJsonObject {
            {QStringLiteral("id"), value.toObject().value(QStringLiteral("id"))},
            {QStringLiteral("object"), QString::fromLatin1(QCryptographicHash::hash(QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256).toHex())}});
    }
    const QJsonObject manifest {{QStringLiteral("schema_version"), 1},
                                {QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), filename},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), filename}, {QStringLiteral("pdf_sha256"), pdfSha256}}},
                                {QStringLiteral("sources"), QJsonArray()},
                                {QStringLiteral("agent_proposals"), proposals},
                                {QStringLiteral("proposal_objects"), proposalObjects},
                                {QStringLiteral("review_correspondence_schema_version"), 2},
                                {QStringLiteral("review_correspondences"), correspondences}};
    const QByteArray contents = QJsonDocument(manifest).toJson(QJsonDocument::Compact) + '\n';
    const QString bundleId = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
    const QString bundleRoot = root + QStringLiteral("/.runtime/target/source-bundle");
    QVERIFY(writeFile(bundleRoot + QStringLiteral("/objects/") + bundleId, contents));
    QVERIFY(writeFile(bundleRoot + QStringLiteral("/documents/") + filename + QStringLiteral(".json"),
                      QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1},
                                                 {QStringLiteral("artifact_sha256"), bundleId},
                                                 {QStringLiteral("filename"), filename},
                                                 {QStringLiteral("pdf_sha256"), pdfSha256}})
                          .toJson()));

    const auto documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QHash<QString, Thoughts::TrackedComment> comments;
    for (const auto &comment : documents.constFirst().comments) {
        comments.insert(comment.id, comment);
    }
    QCOMPARE(comments.size(), 4);
    QCOMPARE(comments.value(QStringLiteral("legacy-heading")).page, 3);
    QCOMPARE(comments.value(QStringLiteral("legacy-unflagged")).page, 2);
    QCOMPARE(comments.value(QStringLiteral("linked-heading")).page, 4);
    QCOMPARE(comments.value(QStringLiteral("linked-copies-only")).page, 2);
    // Every location stays listed, and the copy remains recognisable.
    const QJsonArray linkedLocations = comments.value(QStringLiteral("linked-heading")).pdfLocations;
    QCOMPARE(linkedLocations.size(), 2);
    QVERIFY(linkedLocations.at(0).toObject().value(QStringLiteral("duplicate")).toBool());
    QVERIFY(!linkedLocations.at(1).toObject().contains(QStringLiteral("duplicate")));
}

void CommentsOverviewTest::testLiveTexProposalReplacesArtifactAndResolvesLocally()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString proposalId = QStringLiteral("live-source-20260823");
    const QString sourcePath = root + QStringLiteral("/docs-test/tex/source/main.tex");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("current PDF")));
    QVERIFY(writeFile(sourcePath, QByteArrayLiteral("Before \\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{live-source-20260823}{Use the current wording.}{old version}{replacement version} after.\n")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("alpha.pdf"), proposalId, QStringLiteral("artifact old"), QStringLiteral("artifact new"), true));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.constFirst().comments.size(), 1);
    const Thoughts::TrackedComment &proposal = documents.constFirst().comments.constFirst();
    QVERIFY(proposal.liveSourceProposal);
    QVERIFY(proposal.bundleId.isEmpty());
    QCOMPARE(proposal.proposalBeforePreview, QStringLiteral("old version"));
    QCOMPARE(proposal.proposalAfterPreview, QStringLiteral("replacement version"));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *acceptButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    QVERIFY(table);
    QVERIFY(acceptButton);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QSignalSpy remoteResolutionSpy(&overview, &Thoughts::CommentsOverview::agentProposalsResolveRequested);
    QSignalSpy aggregateSyncSpy(&overview, &Thoughts::CommentsOverview::syncRequested);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(acceptButton->isVisible());
    QTest::mouseClick(acceptButton, Qt::LeftButton);
    QTRY_COMPARE(readFile(sourcePath), QByteArrayLiteral("Before replacement version after.\n"));
    QCOMPARE(remoteResolutionSpy.size(), 0);
    QCOMPARE(aggregateSyncSpy.size(), 0);
    QTRY_COMPARE(table->model()->rowCount(), 0);
}

void CommentsOverviewTest::testLiveProposalAcceptanceIsAsyncAndIncremental()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    QVERIFY(registerSourceProject(root, QStringLiteral("async-project"), QStringLiteral("Async"), {QStringLiteral("docs-test")}));
    const QString path = root + QStringLiteral("/docs-test/tex/source/main.tex");
    QVERIFY(writeFile(path, QByteArrayLiteral(
        "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{async-first}{First.}{Old.}{New.}\n"
        "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{async-second}{Second.}{Before.}{After.}\n")));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    QVERIFY(table);
    QVERIFY(accept);
    QTRY_COMPARE(table->model()->rowCount(), 2);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(accept->isEnabled());
    // Force real disk contention; the GUI must keep dispatching events while
    // source persistence waits, and no rows may disappear before it commits.
    QLockFile lock(QDir(Thoughts::ProposalHistory::directory(root)).filePath(
        Thoughts::ProposalHistory::digest(QFileInfo(path).canonicalFilePath().toUtf8()) + QStringLiteral(".source.lock")));
    QVERIFY(lock.tryLock());
    QElapsedTimer elapsed;
    elapsed.start();
    QTest::mouseClick(accept, Qt::LeftButton);
    QVERIFY2(elapsed.elapsed() < 500, "Acceptance blocked the UI on the source lock");
    QVERIFY(!accept->isEnabled());
    bool heartbeat = false;
    QTimer::singleShot(100, &overview, [&] { heartbeat = true; });
    QTRY_VERIFY(heartbeat);
    QCOMPARE(table->model()->rowCount(), 2);
    const QPersistentModelIndex survivor(table->model()->index(1, 0));
    lock.unlock();
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QVERIFY(survivor.isValid());
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(accept->isEnabled());
    QTest::mouseClick(accept, Qt::LeftButton);
    QTRY_COMPARE(table->model()->rowCount(), 0);
    QCOMPARE(readFile(path), QByteArrayLiteral("New.\nAfter.\n"));
}

void CommentsOverviewTest::testAsyncProposalConflictPreservesSource()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    QVERIFY(registerSourceProject(root, QStringLiteral("conflict-project"), QStringLiteral("Conflict"), {QStringLiteral("docs-test")}));
    const QString path = root + QStringLiteral("/docs-test/tex/source/main.tex");
    QVERIFY(writeFile(path, QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{async-conflict}{Reason.}{Old.}{New.}\n")));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    QVERIFY(table);
    QVERIFY(accept);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    overview.m_catalogPollTimer->stop();
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(writeFile(path, QByteArrayLiteral("Newer user text.\n")));
    QTest::mouseClick(accept, Qt::LeftButton);
    QVERIFY(overview.m_operationPending);
    QTRY_VERIFY(!overview.m_operationPending);
    QCOMPARE(readFile(path), QByteArrayLiteral("Newer user text.\n"));
    QCOMPARE(table->model()->rowCount(), 1);
    QVERIFY(overview.m_summary->text().contains(QStringLiteral("changed")));
}

void CommentsOverviewTest::testLiveTexProposalRefreshesWithoutPdfBuild()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString sourcePath = root + QStringLiteral("/docs-test/tex/source/main.tex");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("unchanged PDF")));
    QVERIFY(writeFile(sourcePath,
                      QByteArrayLiteral("\\providecommand{\\agentProposedEdit}[4]{#4}\n"
                                        "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{live-refresh-20260823}{Initial.}{old}{first replacement}\n")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("alpha.pdf"), QStringLiteral("live-refresh-20260823"), QStringLiteral("artifact old"), QStringLiteral("artifact new"), true));

    QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.constFirst().comments.constFirst().proposalAfterPreview, QStringLiteral("first replacement"));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *selectedText = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewProposalPreview"));
    QVERIFY(table);
    QVERIFY(selectedText);
    QVERIFY(selectedText->isReadOnly());
    QTRY_COMPARE(table->model()->rowCount(), 1);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(selectedText->toPlainText(), QStringLiteral("New version: first replacement\n\nOld version: old\n\nMotivation: Initial."));
    const QByteArray pdfBefore = readFile(root + QStringLiteral("/current_pdfs/alpha.pdf"));
    const auto atomicSave = [&sourcePath](const QByteArray &contents) {
        QSaveFile file(sourcePath);
        return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size() && file.commit();
    };
    QVERIFY(atomicSave(QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{live-refresh-20260823}{Malformed temporary save.}{old}{second replacement\n")));
    // A malformed wrapper is listed once with its issue (spec §6); it cannot be decided.
    QTRY_VERIFY(table->model()->rowCount() == 1
                && table->model()->index(0, 0).data().toString().contains(QStringLiteral("cannot be accepted or rejected")));
    QVERIFY(overview.m_documents.constFirst().error.isEmpty());
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    auto *reject = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDisregardProposal"));
    QVERIFY(accept && reject);
    QTRY_VERIFY(accept->isVisible() && reject->isVisible());
    QVERIFY(!accept->isEnabled());
    QVERIFY(!reject->isEnabled());
    QVERIFY(accept->toolTip().contains(QStringLiteral("never closed")));
    QElapsedTimer refreshTimer;
    refreshTimer.start();
    QVERIFY(atomicSave(QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{live-refresh-20260823}{Revised locally.}{old}{second replacement}\n")));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->index(0, 0).data().toString(), QStringLiteral("Revised locally."), 2000);
    QVERIFY2(refreshTimer.elapsed() < 1500, qPrintable(QStringLiteral("Proposal refresh took %1 ms.").arg(refreshTimer.elapsed())));
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(selectedText->toPlainText(), QStringLiteral("New version: second replacement\n\nOld version: old\n\nMotivation: Revised locally."));
    refreshTimer.restart();
    QVERIFY(atomicSave(QByteArrayLiteral("resolved text\n")));
    QTRY_COMPARE_WITH_TIMEOUT(table->model()->rowCount(), 0, 2000);
    QVERIFY2(refreshTimer.elapsed() < 1500, qPrintable(QStringLiteral("Proposal removal took %1 ms.").arg(refreshTimer.elapsed())));
    QCOMPARE(readFile(root + QStringLiteral("/current_pdfs/alpha.pdf")), pdfBefore);
}

void CommentsOverviewTest::testLiveTexProposalDoubleClickRequestsPdfFocus()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString proposalId = QStringLiteral("arbitrary-proposal-id-20260823");
    const QString sourcePath = root + QStringLiteral("/docs-test/tex/source/main.tex");
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("current PDF")));
    QVERIFY(writeFile(sourcePath, QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{arbitrary-proposal-id-20260823}{Focus the changed line.}{old text}{new text}\n")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("alpha.pdf"), proposalId, QStringLiteral("artifact old"), QStringLiteral("artifact new"), true));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(table);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QSignalSpy proposalActivated(&overview, &Thoughts::CommentsOverview::agentProposalActivated);
    QSignalSpy commentActivated(&overview, &Thoughts::CommentsOverview::commentActivated);
    QSignalSpy sourceOpened(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    const QModelIndex index = table->model()->index(0, 0);
    QVERIFY(QMetaObject::invokeMethod(table, "doubleClicked", Q_ARG(QModelIndex, index)));
    QTRY_COMPARE(proposalActivated.size(), 1);
    QCOMPARE(proposalActivated.constFirst().at(0).toString(), root + QStringLiteral("/current_pdfs/alpha.pdf"));
    QCOMPARE(proposalActivated.constFirst().at(1).toString(), proposalId);
    QCOMPARE(proposalActivated.constFirst().at(2).toString(), QFileInfo(sourcePath).canonicalFilePath());
    QCOMPARE(proposalActivated.constFirst().at(3).toInt(), 1);
    QCOMPARE(commentActivated.size(), 0);
    QCOMPARE(sourceOpened.size(), 0);

    bool sourceActionFound = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) { return; }
        auto *action = menu->findChild<QAction *>(QStringLiteral("commentsOverviewOpenSourceAction"));
        sourceActionFound = action && action->isEnabled() && action->text() == QStringLiteral("Open with VS Code");
        if (sourceActionFound) { action->trigger(); }
        menu->close();
    });
    QVERIFY(QMetaObject::invokeMethod(&overview, "showCommentsContextMenu", Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(index).center())));
    QVERIFY(sourceActionFound);
    QCOMPARE(sourceOpened.size(), 1);
    QCOMPARE(sourceOpened.constFirst().at(0).toString(), QFileInfo(sourcePath).canonicalFilePath());
    QCOMPARE(sourceOpened.constFirst().at(1).toInt(), 1);
}

void CommentsOverviewTest::testAgentProposalParserRejectsNestedMacros()
{
    const QString source = QStringLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{parser-20260823}{Reason}{old {with braces}}{new}\\n");
    const auto parsed = Thoughts::parseLatexAgentProposals(source);
    QVERIFY(parsed.issues.isEmpty());
    QCOMPARE(parsed.proposals.size(), 1);
    QCOMPARE(parsed.proposals.constFirst().original, QStringLiteral("old {with braces}"));
    QString transformed;
    QVERIFY(Thoughts::decideAgentProposals(QStringLiteral("a.tex"), source, {QStringLiteral("parser-20260823")}, QStringLiteral("disregard"), &transformed));
    QCOMPARE(transformed, QStringLiteral("old {with braces}\\n"));
    // Only the outer wrapper is reported, as one issue; a neighbour stays decidable.
    const QString nested = QStringLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{outer}{Reason}{old}{\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{inner}{Reason}{old}{new}}\n"
                                          "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{other}{Reason}{a}{b}");
    const auto nestedParse = Thoughts::parseLatexAgentProposals(nested);
    QCOMPARE(nestedParse.issues.size(), 1);
    QCOMPARE(nestedParse.issues.constFirst().code, QStringLiteral("nested_proposal"));
    QCOMPARE(nestedParse.issues.constFirst().id, QStringLiteral("outer"));
    QCOMPARE(nestedParse.proposals.size(), 1);
    QString code;
    QVERIFY(!Thoughts::decideAgentProposals(QStringLiteral("a.tex"), nested, {QStringLiteral("outer")}, QStringLiteral("accept"), &transformed, &code));
    QCOMPARE(code, QStringLiteral("nested_proposal"));
    QVERIFY(!Thoughts::decideAgentProposals(QStringLiteral("a.tex"), nested, {QStringLiteral("inner")}, QStringLiteral("accept"), &transformed, &code));
    QCOMPARE(code, QStringLiteral("unknown_id"));
    QVERIFY(Thoughts::decideAgentProposals(QStringLiteral("a.tex"), nested, {QStringLiteral("other")}, QStringLiteral("accept"), &transformed, &code));
    QVERIFY(transformed.endsWith(QStringLiteral("}\nb")));
}

void CommentsOverviewTest::testAgentProposalParserIgnoresDefinitions()
{
    const QString source = QStringLiteral("\\providecommand{\\agentProposedEdit}[4]{#4}\n"
                                          "\\renewcommand{\\agentProposedEdit}[4]{#4}\n"
                                          "\\def\\agentProposedEdit#1#2#3#4{#4}\n"
                                          "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{parser-definition-20260828}{Reason}{old}{new}\n");
    const auto parsed = Thoughts::parseLatexAgentProposals(source);
    QVERIFY(parsed.issues.isEmpty());
    QCOMPARE(parsed.proposals.size(), 1);
    QCOMPARE(parsed.proposals.constFirst().id, QStringLiteral("parser-definition-20260828"));

    QString transformed, code;
    QVERIFY2(Thoughts::decideAgentProposals(QStringLiteral("a.tex"), source,
                                            {QStringLiteral("parser-definition-20260828")},
                                            QStringLiteral("accept"),
                                            &transformed,
                                            &code),
             qPrintable(code));
    QCOMPARE(transformed,
             QStringLiteral("\\providecommand{\\agentProposedEdit}[4]{#4}\n"
                            "\\renewcommand{\\agentProposedEdit}[4]{#4}\n"
                            "\\def\\agentProposedEdit#1#2#3#4{#4}\n"
                            "new\n"));
}

void CommentsOverviewTest::testIgnoredHistoricalCopiesDoNotCreateProposalEvents()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QByteArray source = QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{copied-proposal}{Reason.}{Old.}{New.}\n");
    const QString live = root + QStringLiteral("/docs-paper/tex/source/main.tex");
    const QString copy = root + QStringLiteral("/docs-paper/tmp/copy.tex");
    QVERIFY(writeFile(live, source));
    QVERIFY(writeFile(copy, source));
    // Simulate a copy recorded before the user added an exclusion.
    QVERIFY(Thoughts::observeAgentProposalSource(root, copy, source));
    QString duplicateError;
    QVERIFY(!Thoughts::observeAgentProposalSource(root, live, source, &duplicateError));
    QVERIFY(duplicateError.contains(QStringLiteral("ambiguous")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/.ThoughtIgnore"), QByteArrayLiteral(".git/\ntmp/\nbuild/\n")));
    Thoughts::LibraryProjectStore store(root);
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("source:docs-paper")},
        {QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerSourceProject(root, project.value(QStringLiteral("id")).toString(), project.value(QStringLiteral("name")).toString(), {QStringLiteral("docs-paper")}));
    QHash<QString, QJsonObject> before;
    QVERIFY(Thoughts::ProposalHistory::latest(root, &before));
    for (int pass = 0; pass < 2; ++pass) {
        const auto documents = Thoughts::loadTrackedCommentDocuments(root);
        int count = 0;
        for (const auto &document : documents) for (const auto &comment : document.comments) {
            if (comment.id != QLatin1String("copied-proposal")) continue;
            ++count;
            QCOMPARE(comment.sourceFile, QStringLiteral("docs-paper/tex/source/main.tex"));
        }
        QCOMPARE(count, 1);
    }
    QHash<QString, QJsonObject> after;
    QVERIFY(Thoughts::ProposalHistory::latest(root, &after));
    QCOMPARE(after, before);
}

void CommentsOverviewTest::testDuplicateProposalDoesNotManufactureMoves()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QByteArray source = QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{duplicate-id}{Reason.}{Old.}{New.}\n");
    const QString first = root + QStringLiteral("/docs-paper/tex/source/first.tex");
    const QString second = root + QStringLiteral("/docs-paper/submission/second.tex");
    QVERIFY(writeFile(first, source));
    QVERIFY(writeFile(second, source));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/.ThoughtIgnore"), QByteArrayLiteral(".git/\ntmp/\nbuild/\n")));
    QString error;
    QVERIFY(Thoughts::observeAgentProposalSource(root, first, source, &error));
    for (int attempt = 0; attempt < 3; ++attempt) {
        QVERIFY(!Thoughts::observeAgentProposalSource(root, second, source, &error));
        QVERIFY(error.contains(QStringLiteral("ambiguous")));
    }
    QHash<QString, QJsonObject> events;
    QVERIFY(Thoughts::ProposalHistory::latest(root, &events));
    QCOMPARE(events.size(), 1);
    QCOMPARE(events.value(QStringLiteral("duplicate-id")).value(QStringLiteral("kind")).toString(), QStringLiteral("first_seen"));
    Thoughts::LibraryProjectStore store(root);
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("source:docs-paper")},
        {QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    QVERIFY(registerSourceProject(root, project.value(QStringLiteral("id")).toString(), project.value(QStringLiteral("name")).toString(), {QStringLiteral("docs-paper")}));
    for (int attempt = 0; attempt < 2; ++attempt) {
        const auto documents = Thoughts::loadTrackedCommentDocuments(root);
        QVERIFY(std::any_of(documents.cbegin(), documents.cend(), [](const auto &document) {
            return document.error.contains(QStringLiteral("ambiguous"));
        }));
    }
    QHash<QString, QJsonObject> after;
    QVERIFY(Thoughts::ProposalHistory::latest(root, &after));
    QCOMPARE(after, events);
}

void CommentsOverviewTest::testProposalHistoryAppendPreservesLegacyJournal()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QString journal = Thoughts::ProposalHistory::filename(root);
    const QJsonObject snapshot {{QStringLiteral("proposal_id"), QStringLiteral("legacy-id")}};
    const QJsonObject legacy = Thoughts::ProposalHistory::event(QStringLiteral("first_seen"), snapshot, {}, {});
    const QByteArray original = QJsonDocument(legacy).toJson(QJsonDocument::Compact) + '\n';
    QVERIFY(writeFile(journal, original));
    const QJsonObject added = Thoughts::ProposalHistory::event(QStringLiteral("changed"), snapshot, {}, {});
    QString error;
    QVERIFY2(Thoughts::ProposalHistory::append(root, QJsonArray{added}, &error), qPrintable(error));
    QCOMPARE(readFile(journal), original);
    QHash<QString, QJsonObject> latest;
    QVERIFY2(Thoughts::ProposalHistory::latest(root, &latest, &error), qPrintable(error));
    QCOMPARE(latest.value(QStringLiteral("legacy-id")).value(QStringLiteral("event_id")), added.value(QStringLiteral("event_id")));
}

void CommentsOverviewTest::testProposalCurrentIgnoresUnsavedHistory()
{
    QTemporaryDir temporary;
    const QJsonObject snapshot {{QStringLiteral("proposal_id"), QStringLiteral("saved")},
        {QStringLiteral("source_file"), QStringLiteral("docs-paper/tex/source/main.tex")}, {QStringLiteral("after"), QStringLiteral("Saved.")}};
    auto saved = Thoughts::ProposalHistory::event(QStringLiteral("first_seen"), snapshot, {}, {});
    auto unsaved = Thoughts::ProposalHistory::event(QStringLiteral("changed"), snapshot, {}, {});
    auto buffer = snapshot;
    buffer.insert(QStringLiteral("after"), QStringLiteral("Unsaved."));
    unsaved.insert(QStringLiteral("snapshot"), buffer);
    unsaved.insert(QStringLiteral("source_state"), QStringLiteral("editor_buffer"));
    QString error;
    QVERIFY2(Thoughts::ProposalHistory::append(temporary.path(), {saved, unsaved}, &error), qPrintable(error));
    QHash<QString, QJsonObject> current;
    QVERIFY(Thoughts::ProposalHistory::latest(temporary.path(), &current, &error));
    QCOMPARE(current.value(QStringLiteral("saved")).value(QStringLiteral("snapshot")).toObject(), snapshot);
}

void CommentsOverviewTest::testCurrentAnnotationIndexJsonMatchesQt()
{
    // The index digest is computed over spliced JSON; it must equal Qt's own bytes.
    const QJsonObject projections {{QStringLiteral("logical_comment\ncomments\nnote-1"), QJsonObject{{QStringLiteral("text"), QStringLiteral("\"projections\":{} and café")}}},
                                   {QStringLiteral("review_card\ncards\nc1"), QJsonObject{{QStringLiteral("n"), 3}}}};
    const QList<QJsonObject> indexes {
        {{QStringLiteral("schema_version"), 2}, {QStringLiteral("repository_identity"), QStringLiteral("/a\"projections\":{}/b")},
         {QStringLiteral("bootstrapped"), true}, {QStringLiteral("resolved_agent_proposals"), QJsonObject{{QStringLiteral("p"), 1}}}},
        {{QStringLiteral("schema_version"), 2}, {QStringLiteral("repository_identity"), QStringLiteral("x")},
         {QStringLiteral("resolved_agent_proposals"), QJsonObject()}},
    };
    for (const auto &index : indexes) {
        for (const auto &records : {projections, QJsonObject()}) {
            QJsonObject complete = index;
            complete.insert(QStringLiteral("projections"), records);
            const QByteArray expected = QJsonDocument(complete).toJson(QJsonDocument::Compact);
            QCOMPARE(Thoughts::CurrentAnnotationIndex::indexJson(index, records), expected);
            QCOMPARE(Thoughts::CurrentAnnotationIndex::indexJson(index, records), expected); // cached projections
        }
    }
}

void CommentsOverviewTest::testCurrentAnnotationIndexInvalidatesOnRepositoryChange()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QString repository = root + QStringLiteral("/repository.json");
    const QJsonObject comment {{QStringLiteral("comment_id"), QStringLiteral("note-1")}};
    const QJsonObject event {{QStringLiteral("event_id"), QStringLiteral("event-1")}};
    const QJsonObject state {{QStringLiteral("bootstrapped"), true},
        {QStringLiteral("resolved_agent_proposals"), QJsonObject{{QStringLiteral("proposal-1"), QJsonObject{{QStringLiteral("action"), QStringLiteral("accept")}}}}},
        {QStringLiteral("projections"), QJsonObject{{QStringLiteral("logical_comment\ncomments\nnote-1"), comment},
                                                     {QStringLiteral("proposal_event\nproposals\nevent-1"), event}}}};
    QVERIFY(writeFile(repository, QJsonDocument(state).toJson(QJsonDocument::Compact)));
    QVERIFY(Thoughts::CurrentAnnotationIndex::write(root, state));
    QJsonObject current;
    QVERIFY(Thoughts::CurrentAnnotationIndex::read(root, &current));
    QCOMPARE(current.value(QStringLiteral("projections")).toObject().size(), 1);
    QCOMPARE(current.value(QStringLiteral("projections")).toObject().value(QStringLiteral("logical_comment\ncomments\nnote-1")).toObject(), comment);
    QVERIFY(current.value(QStringLiteral("resolved_agent_proposals")).toObject().contains(QStringLiteral("proposal-1")));
    QJsonObject damaged = current;
    damaged.insert(QStringLiteral("projections"), QJsonObject());
    QVERIFY(writeFile(Thoughts::CurrentAnnotationIndex::filename(root), QJsonDocument(damaged).toJson(QJsonDocument::Compact)));
    QVERIFY(!Thoughts::CurrentAnnotationIndex::read(root, &current));
    QVERIFY(Thoughts::CurrentAnnotationIndex::write(root, state));
    QJsonObject metadataOnly = state;
    metadataOnly.insert(QStringLiteral("pending_builds"), QJsonArray{QStringLiteral("paper.pdf")});
    QVERIFY(writeFile(repository, QJsonDocument(metadataOnly).toJson(QJsonDocument::Compact)));
    QVERIFY(!Thoughts::CurrentAnnotationIndex::read(root, &current));
    QVERIFY(Thoughts::CurrentAnnotationIndex::write(root, metadataOnly, false));
    QVERIFY(Thoughts::CurrentAnnotationIndex::read(root, &current));
    QCOMPARE(current.value(QStringLiteral("projections")).toObject().value(QStringLiteral("logical_comment\ncomments\nnote-1")).toObject(), comment);
    // Verify persisted records too, rather than relying only on the new memory cache.
    Thoughts::CurrentAnnotationIndex::cache().repositoryIdentity.clear();
    QVERIFY(Thoughts::CurrentAnnotationIndex::read(root, &current));
    QCOMPARE(current.value(QStringLiteral("projections")).toObject().size(), 1);
    QVERIFY(writeFile(repository, QByteArrayLiteral("{\"changed\":true}")));
    QVERIFY(!Thoughts::CurrentAnnotationIndex::read(root, &current));
}

void CommentsOverviewTest::testUnbuiltProposalRefreshesOnlyOnDiscovery()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QString path = root + QStringLiteral("/docs-paper/tex/source/draft.tex");
    QVERIFY(writeFile(path, QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{draft-id}{Reason.}{Old.}{First.}\n")));
    Thoughts::LibraryProjectStore store(root);
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("source:docs-paper")},
        {QStringLiteral("name"), QStringLiteral("Paper")}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")}};
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray{project}}}).isEmpty());
    auto configuration = Thoughts::ProjectConfiguration::empty(root, QStringLiteral("source:docs-paper"), QStringLiteral("Paper"));
    auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}, {QStringLiteral("path"), QStringLiteral("docs-paper")}});
    configuration.insert(QStringLiteral("source_folders"), folders);
    QVERIFY(Thoughts::ProjectConfiguration::save(Thoughts::ProjectConfiguration::path(root, QStringLiteral("source:docs-paper")), configuration).isEmpty());
    const auto proposedText = [](const QList<Thoughts::TrackedCommentDocument> &documents) {
        for (const auto &document : documents)
            for (const auto &comment : document.comments)
                if (comment.id == QLatin1String("draft-id")) return comment.proposalAfterPreview;
        return QString();
    };
    QCOMPARE(proposedText(Thoughts::loadTrackedCommentDocuments(root)), QStringLiteral("First."));
    QVERIFY(writeFile(path, QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{draft-id}{Reason.}{Old.}{Second.}\n")));
    QCOMPARE(proposedText(Thoughts::loadTrackedCommentDocuments(root, {}, nullptr, {}, false)), QStringLiteral("First."));
    const auto refreshed = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(proposedText(refreshed), QStringLiteral("Second."));
    bool assigned = false;
    for (const auto &document : refreshed)
        for (const auto &comment : document.comments)
            if (comment.id == QLatin1String("draft-id")) assigned = document.libraryProjectId == QLatin1String("source:docs-paper");
    QVERIFY(assigned);
    // A corrupt archive cannot break ordinary discovery or display of current source.
    QVERIFY(writeFile(Thoughts::ProposalHistory::filename(root), QByteArrayLiteral("not valid history")));
    QCOMPARE(proposedText(Thoughts::loadTrackedCommentDocuments(root)), QStringLiteral("Second."));
}

void CommentsOverviewTest::testDurableProposalHistoryAndRecovery()
{
    // Timestamps are UTC with Z only (spec §3.2); an offset or a missing zone is invalid_metadata.
    const QString dated = QStringLiteral("\\agentProposedEdit[created=2026-08-27T08:03:05Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{dated-id}{Reason.}{Old.}{New.}");
    QString code, transformed;
    const auto parsed = Thoughts::parseLatexAgentProposals(dated);
    QCOMPARE(parsed.proposals.size(), 1);
    QCOMPARE(parsed.proposals.first().createdAt, QStringLiteral("2026-08-27T08:03:05Z"));
    QVERIFY(Thoughts::decideAgentProposals(QStringLiteral("a.tex"), dated, {QStringLiteral("dated-id")}, QStringLiteral("accept"), &transformed, &code));
    QCOMPARE(transformed, QStringLiteral("New."));
    for (const QString &invalid : {QStringLiteral("+02:00"), QString()}) {
        const auto broken = Thoughts::parseLatexAgentProposals(QString(dated).replace(QStringLiteral("05Z,"), QStringLiteral("05") + invalid + QLatin1Char(',')));
        QVERIFY(broken.proposals.isEmpty());
        QCOMPARE(broken.issues.size(), 1);
        QCOMPARE(broken.issues.first().code, QStringLiteral("invalid_metadata"));
    }

    QTemporaryDir temporary;
    const auto root = temporary.path();
    const QString path = root + QStringLiteral("/docs-test/tex/source/test.tex");
    const QByteArray original = QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{history-test}{Reason.}{Old.}{New.}\n");
    QVERIFY(writeFile(path, original));
    QString error;
    QVERIFY2(Thoughts::observeAgentProposalSource(root, path, original, &error), qPrintable(error));
    QVERIFY(Thoughts::observeAgentProposalSource(root, path, original, &error));
    QHash<QString, QJsonObject> current;
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QCOMPARE(current.size(), 1);
    QCOMPARE(current.value(QStringLiteral("history-test")).value(QStringLiteral("kind")).toString(), QStringLiteral("first_seen"));
    const auto firstSeen = current.value(QStringLiteral("history-test")).value(QStringLiteral("first_seen_at"));
    QVERIFY(!firstSeen.toString().isEmpty());
    const auto changed = QByteArray(original).replace("{New.}", "{Better.}");
    QVERIFY(writeFile(path, changed));
    QVERIFY(Thoughts::observeAgentProposalSource(root, path, changed, &error));
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QCOMPARE(current.value(QStringLiteral("history-test")).value(QStringLiteral("first_seen_at")), firstSeen);
    QVERIFY(!current.value(QStringLiteral("history-test")).value(QStringLiteral("modified_at")).toString().isEmpty());
    QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(root, path, Thoughts::ProposalHistory::digest(changed),
        {QStringLiteral("history-test")}, QStringLiteral("accept"), &error), qPrintable(error));
    QCOMPARE(readFile(path), QByteArrayLiteral("Better.\n"));
    // An accepted proposal is resolved: nothing about it is kept.
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QVERIFY(!current.contains(QStringLiteral("history-test")));
    QVERIFY(!QFileInfo::exists(Thoughts::ProposalHistory::filename(root)));
    QVERIFY(Thoughts::restoreLiveAgentProposalSource(root, path, Thoughts::ProposalHistory::digest(readFile(path)), changed, &error));
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QCOMPARE(current.value(QStringLiteral("history-test")).value(QStringLiteral("snapshot")).toObject().value(QStringLiteral("after")).toString(), QStringLiteral("Better."));

    // A prepared source edit is not complete until its source matches; only then
    // is the current state updated, including after a process restart.
    const auto snapshot = current.value(QStringLiteral("history-test")).value(QStringLiteral("snapshot")).toObject();
    const auto recovered = Thoughts::ProposalHistory::event(QStringLiteral("rejected"), snapshot,
        Thoughts::ProposalHistory::digest(changed), Thoughts::ProposalHistory::digest(QByteArrayLiteral("Old.\n")));
    const QJsonObject pending {{QStringLiteral("source_path"), path}, {QStringLiteral("after_sha256"), recovered.value(QStringLiteral("source_after_sha256"))},
                              {QStringLiteral("events"), QJsonArray{recovered}}};
    const QString journal = Thoughts::ProposalHistory::directory(root) + QStringLiteral("/proposal_pending_test.json");
    QVERIFY(writeFile(journal, QJsonDocument(pending).toJson()));
    QVERIFY(Thoughts::ProposalHistory::recover(root, &error));
    QVERIFY(QFileInfo::exists(journal));
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QVERIFY(current.contains(QStringLiteral("history-test")));
    QVERIFY(writeFile(path, QByteArrayLiteral("Old.\n")));
    QVERIFY(Thoughts::ProposalHistory::recover(root, &error));
    QVERIFY(!QFileInfo::exists(journal));
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QVERIFY(!current.contains(QStringLiteral("history-test")));
}

void CommentsOverviewTest::testProposalResolvedOutsideThoughtsIsClosed()
{
    QTemporaryDir temporary;
    const auto root = temporary.path();
    const QString path = root + QStringLiteral("/docs-test/tex/source/test.tex");
    const QString other = root + QStringLiteral("/docs-test/tex/source/other.tex");
    const QByteArray original = QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{first}{Reason.}{Old.}{New.}\n\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{second}{Reason.}{Two.}{Deux.}\n");
    const QByteArray otherSource = QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{elsewhere}{Reason.}{A.}{B.}\n");
    QVERIFY(writeFile(path, original));
    QVERIFY(writeFile(other, otherSource));
    QString error;
    QVERIFY2(Thoughts::observeAgentProposalSource(root, path, original, &error), qPrintable(error));
    QVERIFY2(Thoughts::observeAgentProposalSource(root, other, otherSource, &error), qPrintable(error));
    QHash<QString, QJsonObject> current;
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QCOMPARE(current.size(), 3);

    // An editor such as VS Code accepts one proposal: only that one closes.
    const QByteArray oneAccepted = QByteArrayLiteral("New.\n\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{second}{Reason.}{Two.}{Deux.}\n");
    QVERIFY(writeFile(path, oneAccepted));
    QVERIFY2(Thoughts::observeAgentProposalSource(root, path, oneAccepted, &error), qPrintable(error));
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QVERIFY(!current.contains(QStringLiteral("first")));
    QVERIFY(current.contains(QStringLiteral("second")));
    QVERIFY(current.contains(QStringLiteral("elsewhere")));

    // Accepting the last proposal of a source closes it too.
    const QByteArray allAccepted = QByteArrayLiteral("New.\nDeux.\n");
    QVERIFY(writeFile(path, allAccepted));
    QVERIFY2(Thoughts::observeAgentProposalSource(root, path, allAccepted, &error), qPrintable(error));
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QCOMPARE(current.keys(), QList<QString>{QStringLiteral("elsewhere")});

    // An undo that brings the marker back reopens the proposal.
    QVERIFY(writeFile(path, original));
    QVERIFY2(Thoughts::observeAgentProposalSource(root, path, original, &error), qPrintable(error));
    QVERIFY(Thoughts::ProposalHistory::latest(root, &current, &error));
    QCOMPARE(current.size(), 3);
    QCOMPARE(current.value(QStringLiteral("first")).value(QStringLiteral("kind")).toString(), QStringLiteral("first_seen"));
}

void CommentsOverviewTest::testNotesBrowserShowsOnlyNoteColumns()
{
    QTemporaryDir temporary;
    Thoughts::CommentsOverview overview(temporary.path());
    QList<QTreeView *> tables;
    for (auto *view : overview.findChildren<QTreeView *>())
        if (view->model() && view->model()->columnCount() == 10
            && QStringList{QStringLiteral("Comment"), QStringLiteral("To-dos")}.contains(view->model()->headerData(0, Qt::Horizontal).toString())) tables.append(view);
    QCOMPARE(tables.size(), 2);
    int notesTables = 0;
    for (auto *table : tables) {
        const bool notes = table->model()->headerData(0, Qt::Horizontal).toString() == QStringLiteral("To-dos");
        notesTables += notes;
        if (!notes) QCOMPARE(table->model()->headerData(0, Qt::Horizontal).toString(), QStringLiteral("Comment"));
        // Notes are not assigned to a project: no document, page or source columns.
        for (const int column : {1, 2, 3, 4, 9}) QCOMPARE(table->isColumnHidden(column), notes);
        if (!notes) QCOMPARE(table->model()->headerData(9, Qt::Horizontal).toString(), QStringLiteral("Source doc"));
        if (!notes) QCOMPARE(table->model()->headerData(3, Qt::Horizontal).toString(), QStringLiteral("Snapshot"));
    }
    QCOMPARE(notesTables, 1);
}

void CommentsOverviewTest::testLogicalDatesUseSharedPolicy()
{
    QTemporaryDir temporary;
    const auto root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QJsonObject payload {{QStringLiteral("comment_id"), QStringLiteral("logical-date")},
        {QStringLiteral("thread_root_id"), QStringLiteral("logical-date")}, {QStringLiteral("contents"), QStringLiteral("Dated comment")},
        {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QStringLiteral("alpha")}}},
        {QStringLiteral("created_at"), QStringLiteral("2026-08-01T10:00:00+02:00")},
        {QStringLiteral("updated_at"), QStringLiteral("2026-09-10T10:00:00Z")},
        {QStringLiteral("content_updated_at"), QStringLiteral("2026-08-02T10:00:00Z")},
        {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")}}}};
    QJsonObject projections {{QStringLiteral("logical_comment\ncomments\nlogical-date"), payload}};
    const auto documents = Thoughts::loadTrackedCommentDocuments(root, projections);
    QVERIFY(!documents.isEmpty());
    const Thoughts::TrackedComment *dated = nullptr;
    for (const auto &document : documents) for (const auto &comment : document.comments) if (comment.id == QStringLiteral("logical-date")) dated = &comment;
    QVERIFY(dated);
    QCOMPARE(dated->displayDate.toUTC(), QDateTime::fromString(QStringLiteral("2026-08-01T08:00:00Z"), Qt::ISODate));
    QCOMPARE(dated->rootSortDate.toUTC(), QDateTime::fromString(QStringLiteral("2026-08-02T10:00:00Z"), Qt::ISODate));
    payload.insert(QStringLiteral("subtype"), QStringLiteral("Note"));
    payload.insert(QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("standalone")}});
    payload.insert(QStringLiteral("origin"), QJsonValue::Null);
    projections.insert(QStringLiteral("logical_comment\ncomments\nlogical-date"), payload);
    const auto notes = Thoughts::loadTrackedCommentDocuments(root, projections);
    for (const auto &document : notes) for (const auto &note : document.comments) {
        if (note.id == QStringLiteral("logical-date")) {
            QCOMPARE(note.displayDate, dated->displayDate);
            QCOMPARE(note.rootSortDate, dated->rootSortDate);
        }
    }
}

void CommentsOverviewTest::testGuardedAgentProposalResolver()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString bundleRoot = temporary.path() + QStringLiteral("/thoughts-server/source-bundle");
    QVERIFY(QDir().mkpath(bundleRoot));
    QVERIFY(writeFile(temporary.path() + QStringLiteral("/thoughts-server/repository.json"),
                      QJsonDocument(QJsonObject {
                                        {QStringLiteral("resolved_agent_proposals"),
                                         QJsonObject {{QStringLiteral("resolved-remotely-20260828"),
                                                       QJsonObject {{QStringLiteral("operation_id"), QStringLiteral("resolution-20260828")}}}}},
                                    })
                          .toJson(QJsonDocument::Compact)));
    QCOMPARE(Thoughts::locallyResolvedAgentProposalIds(bundleRoot), QSet<QString> {QStringLiteral("resolved-remotely-20260828")});

    const QString sourcePath = temporary.path() + QStringLiteral("/docs-test/tex/source/main.tex");
    const QString proposalId = QStringLiteral("guarded-resolver-20260824");
    const QByteArray proposal = QByteArrayLiteral("Before \\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{guarded-resolver-20260824}{Use the clearer wording.}{old}{new} after.\n");
    QVERIFY(writeFile(sourcePath, proposal));
    const QString sourceSha256 = QString::fromLatin1(QCryptographicHash::hash(proposal, QCryptographicHash::Sha256).toHex());
    QString error;
    QByteArray previousContents;
    QString updatedSha256;
    QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(
                 temporary.path(), sourcePath, sourceSha256, {proposalId}, QStringLiteral("accept"), &error, &previousContents, &updatedSha256),
             qPrintable(error));
    QCOMPARE(readFile(sourcePath), QByteArrayLiteral("Before new after.\n"));
    QCOMPARE(previousContents, proposal);
    QVERIFY2(Thoughts::restoreLiveAgentProposalSource(temporary.path(), sourcePath, updatedSha256, previousContents, &error), qPrintable(error));
    QCOMPARE(readFile(sourcePath), proposal);
    QVERIFY(!Thoughts::restoreLiveAgentProposalSource(temporary.path(), sourcePath, updatedSha256, previousContents, &error));
    QCOMPARE(error, QStringLiteral("Undo is no longer safe because the TeX source changed."));

    QVERIFY(!Thoughts::resolveLiveAgentProposedEdits(temporary.path(), sourcePath, QString(64, QLatin1Char('a')), {proposalId}, QStringLiteral("accept"), &error));
    QCOMPARE(error, QStringLiteral("The TeX source changed before this proposal could be applied."));
    QCOMPARE(readFile(sourcePath), proposal);

    QVERIFY(writeFile(sourcePath, proposal));
    QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(temporary.path(), sourcePath, sourceSha256, {proposalId}, QStringLiteral("disregard"), &error), qPrintable(error));
    QCOMPARE(readFile(sourcePath), QByteArrayLiteral("Before old after.\n"));

    const QByteArray removal = QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{guarded-resolver-20260824}{Remove this.}{old}{}\n");
    QVERIFY(writeFile(sourcePath, removal));
    const QString removalSha256 = QString::fromLatin1(QCryptographicHash::hash(removal, QCryptographicHash::Sha256).toHex());
    QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(temporary.path(), sourcePath, removalSha256, {proposalId}, QStringLiteral("accept"), &error), qPrintable(error));
    QCOMPARE(readFile(sourcePath), QByteArrayLiteral("\n"));

    QTemporaryFile outsideSource;
    QVERIFY(outsideSource.open());
    QCOMPARE(outsideSource.write(proposal), proposal.size());
    outsideSource.flush();
    QVERIFY(!Thoughts::resolveLiveAgentProposedEdits(temporary.path(), outsideSource.fileName(), sourceSha256, {proposalId}, QStringLiteral("accept"), &error));
    QCOMPARE(error, QStringLiteral("The local TeX source is missing or outside the documentation folder."));
    outsideSource.seek(0);
    QCOMPARE(outsideSource.readAll(), proposal);
}

void CommentsOverviewTest::testReplyThreadsMatchMjPresentation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    QJsonObject threadRoot = comment(QStringLiteral("mjpdf-e2215efb-5ee5-4b88-88c9-6910f2c7eeb8"),
                                     QStringLiteral("Following bugs, the deletion on .Tex doesn't update Thoughts immediately."),
                                     1,
                                     QStringLiteral("D:20260810174133+02'00'"),
                                     QStringLiteral("anchored"));
    QJsonObject firstReply = comment(QStringLiteral("mjpdf-de97f3ef-5b09-49a4-b19f-65357585b0ae"),
                                     QStringLiteral("Also the .Tex change on Android doesn't work"),
                                     1,
                                     QStringLiteral("D:20260810174310+02'00'"),
                                     QStringLiteral("anchored"));
    firstReply.insert(QStringLiteral("in_reply_to"), QStringLiteral("mjpdf-e2215efb-5ee5-4b88-88c9-6910f2c7eeb8"));
    QJsonObject nestedReply = comment(QStringLiteral("reply-a2"), QStringLiteral("Nested needle reply"), 1, QStringLiteral("D:20260810174500+02'00'"), QStringLiteral("anchored"));
    nestedReply.insert(QStringLiteral("in_reply_to"), QStringLiteral("mjpdf-de97f3ef-5b09-49a4-b19f-65357585b0ae"));
    QJsonObject newerRoot = comment(QStringLiteral("root-b"), QStringLiteral("Root B"), 2, QStringLiteral("2026-08-10T11:00:00Z"), QStringLiteral("anchored"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(threadRoot) + jsonLine(firstReply) + jsonLine(nestedReply) + jsonLine(newerRoot)));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 2);
    auto *tree = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *search = overview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewSearch"));
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(tree);
    QVERIFY(search);
    QVERIFY(documents);

    QCOMPARE(tree->model()->rowCount(), 2);
    QCOMPARE(tree->model()->index(1, 0).data().toString(), QStringLiteral("Root B"));
    const QModelIndex rootA = tree->model()->index(0, 0);
    QCOMPARE(rootA.data().toString(), QStringLiteral("Following bugs, the deletion on .Tex doesn't update Thoughts immediately."));
    QVERIFY(!rootA.data(Qt::FontRole).value<QFont>().bold());
    QCOMPARE(tree->model()->rowCount(rootA), 2);
    QCOMPARE(tree->model()->index(0, 0, rootA).data().toString(), QStringLiteral("Also the .Tex change on Android doesn't work"));
    QVERIFY(!tree->model()->index(0, 0, rootA).data(Qt::FontRole).value<QFont>().bold());
    QCOMPARE(tree->model()->index(1, 0, rootA).data().toString(), QStringLiteral("Nested needle reply"));
    QVERIFY(documentItem(documents, QStringLiteral("alpha")));
    QCOMPARE(documentItem(documents, QStringLiteral("alpha"))->text(0), QStringLiteral("alpha"));

    search->setText(QStringLiteral("needle"));
    QTRY_COMPARE(tree->model()->rowCount(), 1);
    const QModelIndex filteredRoot = tree->model()->index(0, 0);
    QCOMPARE(filteredRoot.data().toString(), QStringLiteral("Following bugs, the deletion on .Tex doesn't update Thoughts immediately."));
    QCOMPARE(tree->model()->rowCount(filteredRoot), 1);
    QCOMPARE(tree->model()->index(0, 0, filteredRoot).data().toString(), QStringLiteral("Nested needle reply"));
}

void CommentsOverviewTest::testReplyThreadOrphansRemainReachable()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    QJsonObject ordinary = comment(QStringLiteral("root"), QStringLiteral("Root"), 1, QStringLiteral("2026-08-10T08:00:00Z"), QStringLiteral("anchored"));
    QJsonObject orphan = comment(QStringLiteral("orphan"), QStringLiteral("Orphan"), 1, QStringLiteral("2026-08-10T09:00:00Z"), QStringLiteral("anchored"));
    orphan.insert(QStringLiteral("in_reply_to"), QStringLiteral("missing"));
    QJsonObject cycleA = comment(QStringLiteral("cycle-a"), QStringLiteral("Cycle A"), 1, QStringLiteral("2026-08-10T10:00:00Z"), QStringLiteral("anchored"));
    cycleA.insert(QStringLiteral("in_reply_to"), QStringLiteral("cycle-b"));
    QJsonObject cycleB = comment(QStringLiteral("cycle-b"), QStringLiteral("Cycle B"), 1, QStringLiteral("2026-08-10T11:00:00Z"), QStringLiteral("anchored"));
    cycleB.insert(QStringLiteral("in_reply_to"), QStringLiteral("cycle-a"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(ordinary) + jsonLine(orphan) + jsonLine(cycleA) + jsonLine(cycleB)));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    for (const Thoughts::TrackedComment &record : documents.constFirst().comments) {
        QCOMPARE(record.threadRootId, record.id);
        QVERIFY(!record.reply);
    }

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 4);
    auto *tree = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(tree);
    QCOMPARE(tree->model()->rowCount(), 4);
}

void CommentsOverviewTest::testRevealCommentClearsFiltersAndSelectsExactRecord()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString pdfPath = root + QStringLiteral("/current_pdfs/alpha.pdf");
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("placeholder")));

    QJsonObject first = comment(QStringLiteral("first"), QStringLiteral("First comment"), 1, QStringLiteral("2026-08-11T08:00:00Z"), QStringLiteral("anchored"));
    first.insert(QStringLiteral("priority"), QStringLiteral("high"));
    QJsonObject target = comment(QStringLiteral("target"), QStringLiteral("Exact target"), 2, QStringLiteral("2026-08-11T09:00:00Z"), QStringLiteral("anchored"));
    target.insert(QStringLiteral("priority"), QStringLiteral("normal"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(first) + jsonLine(target)));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 2);
    auto *search = overview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewSearch"));
    auto *tree = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(search);
    QVERIFY(tree);

    search->setText(QStringLiteral("First"));
    QTRY_COMPARE(tree->model()->rowCount(), 1);
    QVERIFY(overview.revealComment(pdfPath, QStringLiteral("target")));
    QCOMPARE(search->text(), QString());
    QVERIFY(tree->currentIndex().isValid());
    QCOMPARE(tree->currentIndex().siblingAtColumn(0).data().toString(), QStringLiteral("Exact target"));
}

void CommentsOverviewTest::testDocumentSectionsAndActivation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString deltaLayout = root + QStringLiteral("/comments/.layout/delta.json");
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(deltaLayout, QJsonDocument(layout(QStringLiteral("delta"))).toJson()));
    for (const QString &stem : {QStringLiteral("alpha"), QStringLiteral("beta"), QStringLiteral("delta"), QStringLiteral("gamma")}) {
        QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/%1.pdf").arg(stem), QByteArrayLiteral("placeholder")));
    }
    QJsonArray indexed{QJsonObject{{QStringLiteral("filename"), QStringLiteral("beta.pdf")},
                                   {QStringLiteral("kind"), QStringLiteral("unlinked")},
                                   {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('b'))},
                                   {QStringLiteral("display_name"), QStringLiteral("Beta Readable")},
                                   {QStringLiteral("archived"), true}}};
    // Library PDFs are placed in their Project by filename.
    for (const QString &stem : {QStringLiteral("alpha"), QStringLiteral("delta"), QStringLiteral("gamma")})
        indexed.append(QJsonObject{{QStringLiteral("filename"), QString(stem + QStringLiteral(".pdf"))}, {QStringLiteral("kind"), QStringLiteral("unlinked")},
                                   {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))}});
    const QJsonObject libraryIndex{{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), indexed}};
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(libraryIndex).toJson()));
    // Library projects come only from Project configurations; give each PDF its own.
    for (const auto &[stem, name] : {std::pair{QStringLiteral("alpha"), QStringLiteral("alpha")}, std::pair{QStringLiteral("beta"), QStringLiteral("Beta Readable")},
             std::pair{QStringLiteral("delta"), QStringLiteral("delta")}, std::pair{QStringLiteral("gamma"), QStringLiteral("gamma")}})
        QVERIFY(registerCatalogueProject(root, {{QStringLiteral("id"), QString(QStringLiteral("new:") + stem)}, {QStringLiteral("name"), name},
            {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QString(stem + QStringLiteral(".pdf"))}}}}}));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.documentCount(), 4);
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(documents);
    // All items, No project, the Projects-and-PDFs group, and the Projects
    // heading with four project containers. A Library entry still carrying
    // the retired archived flag is listed like any other document, and there
    // is no separate PDF-only section any more.
    QCOMPARE(documents->topLevelItemCount(), 8);
    QCOMPARE(documents->topLevelItem(0)->text(0), QStringLiteral("All items"));
    QCOMPARE(documents->topLevelItem(3)->text(0), QStringLiteral("Projects"));
    QCOMPARE(documents->topLevelItem(3)->flags(), Qt::NoItemFlags);
    QVERIFY(documents->topLevelItem(3)->font(0).bold());
    for (int row = 0; row < documents->topLevelItemCount(); ++row) {
        QVERIFY2(documents->topLevelItem(row)->text(0) != QStringLiteral("PDF-only documents"),
                 "source availability must not create a separate section");
        QVERIFY2(documents->topLevelItem(row)->text(0) != QStringLiteral("Archived"), "documents are no longer archived");
    }

    // Each document sits under the container the Library gives it, and the
    // container is the only thing naming that placement.
    const auto projectFor = [documents](const QString &name) {
        for (int row = 0; row < documents->topLevelItemCount(); ++row) {
            QTreeWidgetItem *candidate = documents->topLevelItem(row);
            if (navigationItemKind(candidate) == NavigationProjectEntry && candidate->text(0) == name) {
                return candidate;
            }
        }
        return static_cast<QTreeWidgetItem *>(nullptr);
    };
    for (const QString &stem : {QStringLiteral("alpha"), QStringLiteral("delta"), QStringLiteral("gamma")}) {
        QTreeWidgetItem *project = projectFor(stem);
        QVERIFY2(project, qPrintable(QStringLiteral("missing project container for ") + stem));
        QCOMPARE(project->childCount(), 1);
        QCOMPARE(project->child(0)->text(0), stem);
        QCOMPARE(project->child(0)->data(0, Qt::UserRole).toString(), stem);
        QCOMPARE(navigationItemKind(project->child(0)), NavigationDocumentEntry);
    }
    // The formerly archived document keeps its Library display name and is an
    // ordinary document entry inside its project container.
    QTreeWidgetItem *archivedProject = projectFor(QStringLiteral("Beta Readable"));
    QVERIFY(archivedProject);
    QCOMPARE(archivedProject->childCount(), 1);
    QTreeWidgetItem *archivedDocument = archivedProject->child(0);
    QCOMPARE(archivedDocument->text(0), QStringLiteral("Beta Readable"));
    QCOMPARE(archivedDocument->data(0, Qt::UserRole).toString(), QStringLiteral("beta"));

    QSignalSpy activated(&overview, &Thoughts::CommentsOverview::documentActivated);
    Q_EMIT documents->itemDoubleClicked(archivedDocument, 0);
    QTRY_COMPARE(activated.size(), 1);
    QCOMPARE(activated.constFirst().constFirst().toString(), root + QStringLiteral("/current_pdfs/beta.pdf"));

    Q_EMIT documents->itemDoubleClicked(documents->topLevelItem(3), 0);
    QCOMPARE(activated.size(), 1);
    Q_EMIT documents->itemDoubleClicked(documents->topLevelItem(0), 0);
    QCOMPARE(activated.size(), 1);

    documents->setCurrentItem(projectFor(QStringLiteral("delta"))->child(0));
    QCOMPARE(overview.selectedDocument(), QStringLiteral("delta"));
    QVERIFY(QFile::remove(deltaLayout));
    overview.reload();
    QCOMPARE(overview.selectedDocument(), QStringLiteral("delta"));
    QCOMPARE(documents->topLevelItem(3)->text(0), QStringLiteral("Projects"));
    for (int row = 0; row < documents->topLevelItemCount(); ++row) {
        QVERIFY2(documents->topLevelItem(row)->text(0) != QStringLiteral("PDF-only documents"),
                 "a reload must not reintroduce the PDF-only split");
    }
    for (const QString &stem : {QStringLiteral("alpha"), QStringLiteral("delta"), QStringLiteral("gamma"), QStringLiteral("Beta Readable")}) {
        QVERIFY2(projectFor(stem), qPrintable(QStringLiteral("missing project container after reload for ") + stem));
    }
}

// The reported defect: the navigation tree used to split tracked items into
// "Projects" and "PDF-only documents", so imported PDFs were detached from the
// projects the Library puts them in. A document's source availability must not
// decide its place in the tree.
void CommentsOverviewTest::testLibraryProjectHierarchyDrivesNavigation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(QDir().mkpath(root + QStringLiteral("/current_pdfs")));

    // "Course lectures" is a catalogue project whose members were
    // imported as PDFs; the lecture PDFs carry no editable source at all.
    const QStringList lecturePdfs {
        QStringLiteral("01_-_Introduction_and_Motivation_db3d665398be.pdf"),
        QStringLiteral("02_-_Supercomputers_and_PyTorch_Review_9b704020ffd0.pdf"),
        QStringLiteral("13_-_Release_24cd262eaa6d.pdf"),
    };
    // "2605.06663" is a standalone Library project holding a single PDF.
    const QString standalonePdf = QStringLiteral("2605.06663.pdf");
    // A source-backed document inside its own project, for contrast.
    const QString linkedPdf = QStringLiteral("concepts.pdf");

    for (const QString &name : lecturePdfs + QStringList {standalonePdf, linkedPdf}) {
        QVERIFY(writeFile(QDir(root).filePath(QStringLiteral("current_pdfs/") + name), QByteArrayLiteral("placeholder")));
    }

    const QString sha = QString(64, QLatin1Char('a'));
    QJsonArray pdfs;
    for (const QString &name : lecturePdfs) {
        pdfs.append(QJsonObject{{QStringLiteral("filename"), name}, {QStringLiteral("kind"), QStringLiteral("unlinked")},
                                {QStringLiteral("pdf_sha256"), sha}});
    }
    pdfs.append(QJsonObject{{QStringLiteral("filename"), standalonePdf}, {QStringLiteral("kind"), QStringLiteral("unlinked")}, {QStringLiteral("pdf_sha256"), sha}});
    pdfs.append(QJsonObject{{QStringLiteral("filename"), linkedPdf}, {QStringLiteral("project"), QStringLiteral("docs-concepts")},
                            {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), sha}});
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"),
                      QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), pdfs}}).toJson()));

    // The catalogue is the same structure the Library tree consumes.
    QJsonArray lectureDocuments;
    for (const QString &name : lecturePdfs) {
        lectureDocuments.append(QJsonObject{{QStringLiteral("filename"), name}, {QStringLiteral("name"), name}, {QStringLiteral("imported"), true}});
    }
    const QJsonObject catalogue{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("downloaded"), true},
        {QStringLiteral("pending"), QJsonArray{}},
        {QStringLiteral("projects"),
         QJsonArray{
             QJsonObject{{QStringLiteral("id"), QStringLiteral("new:lsaie")},
                         {QStringLiteral("name"), QStringLiteral("Course lectures")},
                         {QStringLiteral("target"), QStringLiteral("project_0efb7318544ca609e11cc1a0.pdf")},
                         {QStringLiteral("source_project"), QString()},
                         {QStringLiteral("documents"), lectureDocuments}},
             QJsonObject{{QStringLiteral("id"), QString(QStringLiteral("pdf:") + standalonePdf)},
                         {QStringLiteral("name"), QStringLiteral("2605.06663")},
                         {QStringLiteral("target"), standalonePdf},
                         {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("filename"), standalonePdf}, {QStringLiteral("name"), QStringLiteral("2605.06663")}}}}},
             QJsonObject{{QStringLiteral("id"), QStringLiteral("source:docs-concepts")},
                         {QStringLiteral("name"), QStringLiteral("concepts")},
                         {QStringLiteral("target"), linkedPdf},
                         {QStringLiteral("source_project"), QStringLiteral("docs-concepts")},
                         {QStringLiteral("documents"), QJsonArray{}}},
         }},
    };
    QVERIFY(writeFile(root + QStringLiteral("/.thoughts/library-projects.json"), QJsonDocument(catalogue).toJson()));
    for (const auto &project : catalogue.value(QStringLiteral("projects")).toArray()) QVERIFY(registerCatalogueProject(root, project.toObject()));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.documentCount(), 5);
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(documents);
    for (int row = 0; row < documents->topLevelItemCount(); ++row) {
        QVERIFY2(documents->topLevelItem(row)->text(0) != QStringLiteral("PDF-only documents"),
                 "PDF-only documents must not be a separate section");
    }

    const auto projectFor = [documents](const QString &name) {
        for (int row = 0; row < documents->topLevelItemCount(); ++row) {
            QTreeWidgetItem *candidate = documents->topLevelItem(row);
            if (navigationItemKind(candidate) == NavigationProjectEntry && candidate->text(0) == name) {
                return candidate;
            }
        }
        return static_cast<QTreeWidgetItem *>(nullptr);
    };
    const auto childStems = [](QTreeWidgetItem *project) {
        QStringList stems;
        for (int i = 0; i < project->childCount(); ++i) {
            stems.append(project->child(i)->data(0, Qt::UserRole).toString());
        }
        return stems;
    };

    // Every PDF imported into the project appears inside that project.
    QTreeWidgetItem *lsaie = projectFor(QStringLiteral("Course lectures"));
    QVERIFY2(lsaie, "Course lectures must be a project container");
    QCOMPARE(navigationItemKind(lsaie), NavigationProjectEntry);
    QCOMPARE(lsaie->childCount(), lecturePdfs.size());
    for (const QString &name : lecturePdfs) {
        QTreeWidgetItem *child = documentItem(documents, QFileInfo(name).completeBaseName());
        QVERIFY2(child, qPrintable(QStringLiteral("missing document ") + name));
        QCOMPARE(child->parent(), lsaie);
    }

    // A standalone Library project keeps its own single document nested under it.
    QTreeWidgetItem *standalone = projectFor(QStringLiteral("2605.06663"));
    QVERIFY2(standalone, "2605.06663 must follow its Library placement");
    QCOMPARE(standalone->childCount(), 1);
    QCOMPARE(standalone->child(0)->data(0, Qt::UserRole).toString(), QStringLiteral("2605.06663"));
    QCOMPARE(documentItem(documents, QStringLiteral("2605.06663")), standalone->child(0));

    // A source-backed document is nested the same way, so source availability
    // makes no structural difference.
    QTreeWidgetItem *linked = projectFor(QStringLiteral("concepts"));
    QVERIFY(linked);
    QCOMPARE(linked->childCount(), 1);
    QCOMPARE(linked->child(0)->data(0, Qt::UserRole).toString(), QStringLiteral("concepts"));

    // Each imported lecture PDF is independently reachable and activatable.
    QSignalSpy activated(&overview, &Thoughts::CommentsOverview::documentActivated);
    QTreeWidgetItem *releaseLecture = documentItem(documents, QStringLiteral("13_-_Release_24cd262eaa6d"));
    QVERIFY(releaseLecture);
    Q_EMIT documents->itemDoubleClicked(releaseLecture, 0);
    QTRY_COMPARE(activated.size(), 1);
    QCOMPARE(activated.constFirst().constFirst().toString(), QDir(root).filePath(QStringLiteral("current_pdfs/13_-_Release_24cd262eaa6d.pdf")));

    // Selecting the project keeps every one of its documents in scope.
    documents->setCurrentItem(lsaie);
    QCOMPARE(documents->selectedItems().size(), 1);
    QCOMPARE(childStems(lsaie).size(), lecturePdfs.size());
}

void CommentsOverviewTest::testProjectFilteringAndSelection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QJsonArray pdfs;
    QJsonArray members;
    for (const QString &stem : {QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("archived"), QStringLiteral("other")}) {
        const QString filename = stem + QStringLiteral(".pdf");
        QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/") + filename, QByteArrayLiteral("placeholder")));
        pdfs.append(QJsonObject{{QStringLiteral("filename"), filename}, {QStringLiteral("kind"), QStringLiteral("unlinked")},
                               {QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('a'))},
                               {QStringLiteral("archived"), stem == QLatin1String("archived")}});
        if (stem != QLatin1String("other")) { members.append(QJsonObject{{QStringLiteral("filename"), filename}}); }
        QVERIFY(writeFile(root + QStringLiteral("/comments/") + stem + QStringLiteral("_comments.jsonl"),
                          jsonLine(comment(stem, stem, 1, QStringLiteral("2026-09-15T10:00:00Z"), QStringLiteral("anchored")))));
    }
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"),
                      QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), pdfs}}).toJson()));
    const QJsonObject shared{{QStringLiteral("id"), QStringLiteral("new:shared")},
        {QStringLiteral("name"), QStringLiteral("Shared")}, {QStringLiteral("target"), QStringLiteral("first.pdf")},
        {QStringLiteral("documents"), members}};
    QVERIFY(writeFile(root + QStringLiteral("/.thoughts/library-projects.json"), QJsonDocument(QJsonObject{
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("downloaded"), true}, {QStringLiteral("pending"), QJsonArray{}},
        {QStringLiteral("projects"), QJsonArray{shared}}}).toJson()));
    QVERIFY(registerCatalogueProject(root, shared));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 4);
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(documents);
    QVERIFY(table);
    const auto project = [&](const QString &stem) { return documentItem(documents, stem)->parent(); };
    // A Library entry still carrying the retired archived flag stays in its
    // project like any other member.
    auto *sharedItem = project(QStringLiteral("first"));
    QCOMPARE(project(QStringLiteral("archived")), sharedItem);
    sharedItem->setExpanded(true);
    documents->setCurrentItem(sharedItem);
    QCOMPARE(table->model()->rowCount(), 3);
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const QString text = table->model()->index(row, 0).data().toString();
        QVERIFY(text == QLatin1String("first") || text == QLatin1String("second") || text == QLatin1String("archived"));
    }
    overview.reload();
    QCOMPARE(documents->currentItem(), project(QStringLiteral("first")));
    QVERIFY(project(QStringLiteral("first"))->isExpanded());
    QCOMPARE(table->model()->rowCount(), 3);
    documents->setCurrentItem(documentItem(documents, QStringLiteral("other")));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("other"));
    documents->setCurrentItem(documents->topLevelItem(0));
    QCOMPARE(table->model()->rowCount(), 4);
}

void CommentsOverviewTest::testProjectDocumentCommentNavigation()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString pdfPath = root + QStringLiteral("/current_pdfs/paper.pdf");
    QVERIFY(QDir().mkpath(QFileInfo(pdfPath).path()));
    {
        QPdfWriter writer(pdfPath);
        QPainter painter(&writer);
        painter.drawText(100, 100, QStringLiteral("First"));
        QVERIFY(writer.newPage());
        painter.drawText(100, 100, QStringLiteral("Second"));
        QVERIFY(writer.newPage());
        painter.drawText(100, 100, QStringLiteral("Empty"));
    }
    QVERIFY(writeAgentProposalBundle(root, QStringLiteral("paper.pdf"), QStringLiteral("navigation-proposal")));
    const QString bundle = root + QStringLiteral("/.runtime/target/source-bundle");
    const QString pointerPath = bundle + QStringLiteral("/documents/paper.pdf.json");
    QJsonObject pointer = QJsonDocument::fromJson(readFile(pointerPath)).object();
    QJsonObject artifact = QJsonDocument::fromJson(readFile(bundle + QStringLiteral("/objects/") + pointer.value(QStringLiteral("artifact_sha256")).toString())).object();
    QJsonObject pdf = artifact.value(QStringLiteral("pdf")).toObject();
    const QString firstSource = QStringLiteral("docs-paper/tex/source/first.tex");
    pdf.insert(QStringLiteral("page_count"), 3);
    pdf.insert(QStringLiteral("segments"), QJsonArray {
        QJsonObject {{QStringLiteral("source_id"), firstSource}, {QStringLiteral("inputs"), QJsonArray {QJsonObject {{QStringLiteral("source_file"), QStringLiteral("docs-paper/shared.tex")}}, QJsonObject {{QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")}}}}, {QStringLiteral("title"), QStringLiteral("First document")}, {QStringLiteral("start_page"), 1}, {QStringLiteral("page_count"), 1}},
        QJsonObject {{QStringLiteral("source_id"), QStringLiteral("docs-paper/tex/source/second.tex")}, {QStringLiteral("title"), QStringLiteral("Second document")}, {QStringLiteral("start_page"), 2}, {QStringLiteral("page_count"), 1}},
        QJsonObject {{QStringLiteral("source_id"), QStringLiteral("docs-paper/tex/source/empty.tex")}, {QStringLiteral("title"), QStringLiteral("Empty document")}, {QStringLiteral("start_page"), 3}, {QStringLiteral("page_count"), 1}},
    });
    artifact.insert(QStringLiteral("pdf"), pdf);
    const QByteArray bytes = QJsonDocument(artifact).toJson();
    const QString digest = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    pointer.insert(QStringLiteral("artifact_sha256"), digest);
    QVERIFY(writeFile(bundle + QStringLiteral("/objects/") + digest, bytes));
    QVERIFY(writeFile(pointerPath, QJsonDocument(pointer).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(QJsonObject {
        {QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray {QJsonObject {
            {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("project"), QStringLiteral("docs-paper")},
            {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), pdf.value(QStringLiteral("pdf_sha256"))}}}}}).toJson()));
    const QString date = QStringLiteral("2026-09-09T10:00:00Z");
    QJsonObject first = comment(QStringLiteral("first"), QStringLiteral("First thread"), 1, date, QStringLiteral("anchored"));
    QJsonObject second = comment(QStringLiteral("second"), QStringLiteral("Second thread"), 2, date, QStringLiteral("anchored"));
    QJsonObject reply = comment(QStringLiteral("reply"), QStringLiteral("Reply with a different stored page"), 1, date, QStringLiteral("anchored"));
    reply.insert(QStringLiteral("in_reply_to"), QStringLiteral("second"));
    first.insert(QStringLiteral("tex_file"), firstSource);
    second.insert(QStringLiteral("tex_file"), QStringLiteral("docs-paper/tex/source/second.tex"));
    reply.insert(QStringLiteral("tex_file"), QStringLiteral("docs-paper/tex/source/second.tex"));
    QJsonObject sourceOnly = comment(QStringLiteral("source"), QStringLiteral("Source match despite stale page"), 2, date, QStringLiteral("anchored"));
    sourceOnly.insert(QStringLiteral("tex_file"), QStringLiteral("docs-paper/shared.tex"));
    QJsonObject unknown = comment(QStringLiteral("unknown"), QStringLiteral("Unassigned thread"), 0, date, QStringLiteral("needs_anchor"));
    unknown.insert(QStringLiteral("tex_file"), QStringLiteral("docs-paper/unknown.tex"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/paper_comments.jsonl"), jsonLine(first) + jsonLine(second) + jsonLine(reply) + jsonLine(sourceOnly) + jsonLine(unknown)));
    // Proposals are listed only while their source exists.
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("Remove this whole sentence.\n")));
    // The Library project names the navigation container and declares the published sources.
    QJsonArray published;
    for (const auto &[stem, name] : {std::pair{QStringLiteral("first"), QStringLiteral("First document")}, std::pair{QStringLiteral("second"), QStringLiteral("Second document")},
             std::pair{QStringLiteral("empty"), QStringLiteral("Empty document")}}) {
        const QString source = QStringLiteral("docs-paper/tex/source/") + stem + QStringLiteral(".tex");
        QVERIFY(writeFile(root + QLatin1Char('/') + source, QByteArrayLiteral("Source.\n")));
        published.append(QJsonObject{{QStringLiteral("id"), source}, {QStringLiteral("source_file"), source}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")},
            {QStringLiteral("name"), name}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("start_page"), published.size() + 1}});
    }
    const QJsonObject libraryProject{{QStringLiteral("id"), QStringLiteral("source:docs-paper")}, {QStringLiteral("name"), QStringLiteral("paper")},
        {QStringLiteral("target"), QStringLiteral("paper.pdf")}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")}, {QStringLiteral("documents"), published}};
    QVERIFY(Thoughts::LibraryProjectStore(root).receive({{QStringLiteral("projects"), QJsonArray{libraryProject}}}).isEmpty());
    QVERIFY(registerCatalogueProject(root, libraryProject));

    Thoughts::CommentsOverview overview(root);
    overview.resize(1100, 780);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 5);
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(documents);
    QVERIFY(table);
    QCOMPARE(documents->headerItem()->text(2), QStringLiteral("Agent proposals"));
    auto *documentNode = documentItem(documents, QStringLiteral("paper"));
    QVERIFY(documentNode);
    // The project container holds the tracked document, and the document holds
    // the verified components, exactly as the Library nests them.
    auto *project = documentNode->parent();
    QVERIFY(project);
    QCOMPARE(navigationItemKind(project), NavigationProjectEntry);
    QCOMPARE(project->text(0), QStringLiteral("paper"));
    QCOMPARE(project->childCount(), 1);
    QCOMPARE(documentNode->text(0), QStringLiteral("paper"));
    QCOMPARE(documentNode->text(1), QStringLiteral("4"));
    QCOMPARE(documentNode->text(2), QStringLiteral("1"));
    QCOMPARE(documentNode->childCount(), 4);
    QCOMPARE(documentNode->child(0)->text(0), QStringLiteral("First document"));
    QCOMPARE(documentNode->child(0)->text(1), QStringLiteral("2"));
    QCOMPARE(documentNode->child(0)->text(2), QStringLiteral("1"));
    QCOMPARE(documentNode->child(1)->text(1), QStringLiteral("1"));
    QCOMPARE(documentNode->child(2)->text(1), QStringLiteral("0"));
    QCOMPARE(documentNode->child(3)->text(0), QStringLiteral("Unassigned to a document"));
    QCOMPARE(documentNode->child(3)->text(1), QStringLiteral("1"));
    project->setExpanded(true);
    documents->setCurrentItem(documentNode->child(1));
    QCOMPARE(overview.selectedDocument(), QStringLiteral("paper"));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Second thread"));
    QCOMPARE(table->model()->rowCount(table->model()->index(0, 0)), 1);
    QSignalSpy openedPage(&overview, &Thoughts::CommentsOverview::documentPageActivated);
    Q_EMIT documents->itemDoubleClicked(documentNode->child(1), 0);
    QCOMPARE(openedPage.size(), 1);
    QCOMPARE(openedPage.first().at(0).toString(), pdfPath);
    QCOMPARE(openedPage.first().at(1).toInt(), 2);
    overview.reload();
    documentNode = documentItem(documents, QStringLiteral("paper"));
    QVERIFY(documentNode);
    QVERIFY(documentNode->parent());
    QVERIFY(documentNode->parent()->isExpanded());
    QCOMPARE(documents->currentItem()->text(0), QStringLiteral("Second document"));
    QCOMPARE(table->model()->rowCount(), 1);
    documents->setCurrentItem(documentNode->child(2));
    QCOMPARE(table->model()->rowCount(), 0);
    documents->setCurrentItem(documentNode->child(3));
    QCOMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Unassigned thread"));
    // A standalone deletion preserves the selected component and updates its
    // filter and counts without rebuilding the navigation tree.
    QTreeWidgetItem *unassigned = documentNode->child(3);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    overview.completeOperation(Thoughts::CommentsOverview::DeleteOperation, pdfPath, QStringLiteral("unknown"),
                               {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    QCOMPARE(documents->currentItem(), unassigned);
    QCOMPARE(unassigned->text(1), QStringLiteral("0"));
    QCOMPARE(documentNode->text(1), QStringLiteral("3"));
    QCOMPARE(documentNode->text(2), QStringLiteral("1"));
    QCOMPARE(table->model()->rowCount(), 0);

    // Deleting a thread root must leave its surviving replies reachable.
    documents->setCurrentItem(documentNode->child(1));
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    overview.completeOperation(Thoughts::CommentsOverview::DeleteOperation, pdfPath, QStringLiteral("second"),
                               {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    documents->setCurrentItem(documents->topLevelItem(0));
    bool foundReply = false;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        foundReply |= table->model()->index(row, 0).data().toString() == QStringLiteral("Reply with a different stored page");
    }
    QVERIFY(foundReply);
    overview.reload();
    documentNode = documentItem(documents, QStringLiteral("paper"));
    QVERIFY(documentNode);

    documents->setCurrentItem(documentNode);
    QCOMPARE(table->model()->rowCount(), 5);
    documentNode->setExpanded(false);
    overview.setSyncSnapshot({}, {}, {}, {});
    documentNode = documentItem(documents, QStringLiteral("paper"));
    QVERIFY(!documentNode->isExpanded());
    QVERIFY(!documentNode->text(0).contains(QStringLiteral("Synced")));
    documents->setCurrentItem(documents->topLevelItem(0));
    QCOMPARE(table->model()->rowCount(), 5);

    const QString screenshot = qEnvironmentVariable("THOUGHTS_COMMENTS_TEST_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        documentNode->setExpanded(true);
        documents->setCurrentItem(documentNode->child(1));
        QCoreApplication::processEvents();
        QVERIFY(documents->grab().save(screenshot));
    }

    // A component in the comments tree offers the actions of its Library document.
    auto *library = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(library);
    QSignalSpy opened(&overview, &Thoughts::CommentsOverview::documentPageActivated);
    QSignalSpy renamedProject(&overview, &Thoughts::CommentsOverview::libraryDisplayNameRequested);
    QSignalSpy changed(&overview, &Thoughts::CommentsOverview::projectAttachmentChangedLocally);
    const auto componentMenu = [&](bool inComments, const QString &trigger) {
        if (inComments) { overview.findChild<QTabWidget *>(QStringLiteral("thoughtsOverviewTabs"))->setCurrentWidget(overview.findChild<QWidget *>(QStringLiteral("thoughtsOverviewCommentsPage"))); } else { overview.showSummary(); }
        QTreeWidget *view = inComments ? documents : library;
        QTreeWidgetItem *selected = inComments ? documentItem(documents, QStringLiteral("paper"))->child(1)
                                              : library->topLevelItem(0)->child(0)->child(1);
        view->expandAll(); view->scrollToItem(selected);
        QStringList actions;
        QTimer::singleShot(0, &overview, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) { return; }
            for (auto *action : menu->actions()) { if (!action->isSeparator()) { actions.append(action->text()); } }
            for (auto *action : menu->actions()) {
                if (action->text() != trigger) { continue; }
                if (trigger == QStringLiteral("Rename…")) {
                    QTimer::singleShot(0, &overview, [&] {
                        auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget());
                        if (dialog) { dialog->setTextValue(QStringLiteral("Renamed second")); dialog->accept(); }
                    });
                }
                action->trigger(); break;
            }
            menu->close();
        });
        QMetaObject::invokeMethod(&overview, inComments ? "showDocumentsContextMenu" : "showLibraryContextMenu", Qt::DirectConnection, Q_ARG(QPoint, view->visualItemRect(selected).center()));
        return actions;
    };
    // A source-linked document can move to another Project with its source.
    const QStringList expected {QStringLiteral("Open PDF"), QStringLiteral("Rename…"), QStringLiteral("Remove…"), QStringLiteral("Move to project…")};
    QCOMPARE(componentMenu(true, QStringLiteral("Open PDF")), expected);
    QCOMPARE(opened.size(), 1); QCOMPARE(opened.first().at(0).toString(), pdfPath); QCOMPARE(opened.first().at(1).toInt(), 2);
    QCOMPARE(componentMenu(false, {}), expected);
    auto *sourceModel = qobject_cast<QSortFilterProxyModel *>(table->model())->sourceModel();
    const QPersistentModelIndex survivingRow(sourceModel->index(0, 0));
    componentMenu(true, QStringLiteral("Rename…"));
    QVERIFY(survivingRow.isValid());
    QCOMPARE(renamedProject.size(), 0);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(Thoughts::LibraryProjectStore(root).pending().last().toObject().value(QStringLiteral("action")).toString(), QStringLiteral("rename_document"));
    QCOMPARE(library->topLevelItem(0)->child(0)->child(1)->text(0), QStringLiteral("Renamed second"));
    // Deleting a document is offered on its verified component in the PDF outline.
    const QString configurationPath = Thoughts::ProjectConfiguration::path(root, libraryProject.value(QStringLiteral("id")).toString());
    const QByteArray beforeDelete = readFile(configurationPath);
    const QByteArray sourceText = QByteArrayLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{delete-with-document}{Redundant.}{Old text.}{}\n");
    const QString secondPath = root + QStringLiteral("/docs-paper/tex/source/second.tex");
    QVERIFY(QDir().mkpath(QFileInfo(secondPath).path()));
    QVERIFY(writeFile(secondPath, sourceText));
    const QString orderPath = root + QStringLiteral("/docs-paper/tex/docs_order_manifest.txt");
    const QByteArray originalOrder = QByteArrayLiteral("# Document order\n\nfirst.tex\nsecond.tex\nempty.tex\n");
    QVERIFY(writeFile(orderPath, originalOrder));
    const auto outlineDelete = [&](bool confirm) {
        QMenu menu;
        if (!overview.addOutlineDocumentActions(&menu, pdfPath, 2, QStringLiteral("Second document"))) return false;
        auto *remove = menu.findChild<QAction *>(QStringLiteral("libraryDeleteDocumentAction"));
        if (!remove) return false;
        auto *confirmationTimer = new QTimer(&overview);
        connect(confirmationTimer, &QTimer::timeout, &overview, [&, confirmationTimer] {
            QDialog *dialog = nullptr;
            for (auto *widget : QApplication::allWidgets()) {
                auto *candidate = qobject_cast<QDialog *>(widget);
                if (candidate && candidate->isVisible() && candidate->windowTitle().contains(QStringLiteral("Delete Document"))) { dialog = candidate; break; }
            }
            if (!dialog) { return; }
            confirmationTimer->stop();
            confirmationTimer->deleteLater();
            if (!confirm) { dialog->reject(); return; }
            for (auto *button : dialog->findChildren<QPushButton *>()) {
                if (button->text().remove(QLatin1Char('&')) == QStringLiteral("Delete")) { button->click(); return; }
            }
            dialog->reject();
        });
        confirmationTimer->start(10);
        remove->trigger();
        return true;
    };
    QVERIFY(outlineDelete(false));
    QCOMPARE(readFile(configurationPath), beforeDelete);
    QCOMPARE(readFile(secondPath), sourceText);
    QList<Thoughts::TrackedCommentTarget> requestedDeletions;
    connect(&overview, &Thoughts::CommentsOverview::commentsDeleteRequested, &overview, [&](const QList<Thoughts::TrackedCommentTarget> &targets) { requestedDeletions = targets; });
    QVERIFY(outlineDelete(true));
    QCOMPARE(requestedDeletions.size(), 2);
    QSet<QString> requestedIds;
    for (const auto &target : requestedDeletions) { requestedIds.insert(target.annotationId); }
    QCOMPARE(requestedIds, (QSet<QString> {QStringLiteral("second"), QStringLiteral("reply")}));
    // A failed queue operation must keep the document included.
    overview.completeBatchDelete({}, {{QStringLiteral("success"), false}, {QStringLiteral("changed"), false}});
    QCOMPARE(readFile(configurationPath), beforeDelete);
    QCOMPARE(changed.size(), 1);
    QCOMPARE(readFile(secondPath), sourceText);
    QCOMPARE(readFile(orderPath), originalOrder);
    // A completed deletion moves the source with QFile::moveToTrash, which uses
    // the real user Trash rather than the test home, so it is not exercised here.
    QVERIFY(outlineDelete(false));
    QCOMPARE(readFile(secondPath), sourceText);

    // Replacing a PDF without a matching source map must not reuse old page ranges.
    // Local sources would still be listed as unbuilt documents, so remove them.
    for (const QString &stem : {QStringLiteral("first"), QStringLiteral("second"), QStringLiteral("empty")})
        QVERIFY(QFile::remove(root + QStringLiteral("/docs-paper/tex/source/") + stem + QStringLiteral(".tex")));
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("a different publication")));
    overview.reload();
    documentNode = documentItem(documents, QStringLiteral("paper"));
    QVERIFY(documentNode);
    QCOMPARE(documentNode->childCount(), 0);
    QVERIFY(documentNode->toolTip(0).contains(QStringLiteral("verified document map is unavailable")));
}

void CommentsOverviewTest::testLibraryDocumentComponentsAndLocations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString pdfPath = root + QStringLiteral("/current_pdfs/paper.pdf");
    QVERIFY(QDir().mkpath(QFileInfo(pdfPath).path()));
    {
        QPdfWriter writer(pdfPath);
        QPainter painter(&writer);
        painter.drawText(100, 100, QStringLiteral("First document"));
        QVERIFY(writer.newPage());
        painter.drawText(100, 100, QStringLiteral("Second document"));
    }
    QFile pdf(pdfPath);
    QVERIFY(pdf.open(QIODevice::ReadOnly));
    const QString pdfSha = QString::fromLatin1(QCryptographicHash::hash(pdf.readAll(), QCryptographicHash::Sha256).toHex());
    const QString first = QStringLiteral("docs-paper/tex/source/z_first.tex");
    const QString second = QStringLiteral("docs-paper/tex/source/a_second.tex");
    const QJsonArray segments {
        QJsonObject {{QStringLiteral("source_id"), first}, {QStringLiteral("start_page"), 1}, {QStringLiteral("page_count"), 1}},
        QJsonObject {{QStringLiteral("source_id"), second}, {QStringLiteral("start_page"), 2}, {QStringLiteral("page_count"), 1}},
    };
    const QJsonObject artifact {{QStringLiteral("kind"), QStringLiteral("thoughts-document-source-artifact")},
                                {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                {QStringLiteral("pdf"), QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("page_count"), 2}, {QStringLiteral("segments"), segments}}}};
    const QByteArray artifactBytes = QJsonDocument(artifact).toJson();
    const QString artifactSha = QString::fromLatin1(QCryptographicHash::hash(artifactBytes, QCryptographicHash::Sha256).toHex());
    const QString bundle = root + QStringLiteral("/.runtime/target/source-bundle");
    QVERIFY(writeFile(bundle + QStringLiteral("/objects/") + artifactSha, artifactBytes));
    QVERIFY(writeFile(bundle + QStringLiteral("/documents/paper.pdf.json"), QJsonDocument(QJsonObject {{QStringLiteral("artifact_sha256"), artifactSha}, {QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")}}).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("pdfs"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("project"), QStringLiteral("docs-paper")}, {QStringLiteral("kind"), QStringLiteral("source_linked")}, {QStringLiteral("pdf_sha256"), pdfSha}}}}}).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/docs-paper/.thoughts-overleaf.json"), QByteArrayLiteral("{\"overleaf_root\":\"overleaf\",\"display_name\":\"Shared paper\"}")));
    QJsonArray published;
    for (const auto &[source, name] : {std::pair{first, QStringLiteral("Z first")}, std::pair{second, QStringLiteral("A second")}}) {
        QVERIFY(writeFile(root + QLatin1Char('/') + source, QByteArrayLiteral("Source.\n")));
        published.append(QJsonObject {{QStringLiteral("id"), source}, {QStringLiteral("source_file"), source}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")},
            {QStringLiteral("name"), name}, {QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("start_page"), published.size() + 1}});
    }
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("source:docs-paper")}, {QStringLiteral("name"), QStringLiteral("Paper")},
        {QStringLiteral("target"), QStringLiteral("paper.pdf")}, {QStringLiteral("source_project"), QStringLiteral("docs-paper")}, {QStringLiteral("documents"), published}};
    QVERIFY(Thoughts::LibraryProjectStore(root).receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(root, project));
    Thoughts::CommentsOverview overview(root);
    QMenu initialOutline;
    QVERIFY(overview.addOutlineDocumentActions(&initialOutline, pdfPath, 2, QStringLiteral("a_second")));
    overview.resize(1050, 780);
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QTRY_COMPARE(tree->topLevelItem(0)->childCount(), 3); // Documents, Sources, Agent conversations
    QVERIFY(!tree->isColumnHidden(1));
    QVERIFY(tree->isColumnHidden(2));
    QCOMPARE(tree->headerItem()->text(0), QStringLiteral("Project"));
    tree->topLevelItem(0)->setExpanded(true);
    auto *documents = tree->topLevelItem(0)->child(0);
    QCOMPARE(documents->text(0), QStringLiteral("Documents"));
    QCOMPARE(documents->child(0)->text(0), QStringLiteral("Z first"));
    QCOMPARE(documents->child(1)->text(0), QStringLiteral("A second"));
    tree->sortItems(3, Qt::DescendingOrder);
    QCOMPARE(tree->topLevelItem(0)->child(0)->text(0), QStringLiteral("Documents"));
    QCOMPARE(documents->child(0)->text(0), QStringLiteral("Z first"));
    QSignalSpy activated(&overview, &Thoughts::CommentsOverview::documentPageActivated);
    QVERIFY(QMetaObject::invokeMethod(tree, "itemDoubleClicked", Qt::DirectConnection, Q_ARG(QTreeWidgetItem *, documents->child(1)), Q_ARG(int, 0)));
    QCOMPARE(activated.size(), 1);
    QCOMPARE(activated.first().at(1).toInt(), 2);
    const QString screenshot = qEnvironmentVariable("THOUGHTS_LIBRARY_TEST_SCREENSHOT");
    if (!screenshot.isEmpty()) {
        tree->expandAll();
        QTest::qWait(50);
        QVERIFY(overview.grab().save(screenshot));
    }

    // The outline resolves the clicked component, not the open page or a similarly named section.
    QMenu outlineMenu;
    QVERIFY(overview.addOutlineDocumentActions(&outlineMenu, pdfPath, 2, QStringLiteral("a_second")));
    auto *rename = outlineMenu.findChild<QAction *>(QStringLiteral("libraryRenameAction"));
    QVERIFY(rename);
    QVERIFY(outlineMenu.findChild<QAction *>(QStringLiteral("libraryDeleteDocumentAction")));
    auto *info = outlineMenu.findChild<QAction *>(QStringLiteral("libraryDocumentInformationAction"));
    QVERIFY(info);
    QMenu wrongHeading;
    QVERIFY(!overview.addOutlineDocumentActions(&wrongHeading, pdfPath, 2, QStringLiteral("Introduction")));
    QVERIFY(!overview.addOutlineDocumentActions(&wrongHeading, pdfPath, 1, QStringLiteral("a_second")));
    QVERIFY(wrongHeading.actions().isEmpty());
    bool inspectedInfo = false;
    QTimer informationTimer;
    connect(&informationTimer, &QTimer::timeout, &overview, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            auto *dialog = qobject_cast<QDialog *>(widget);
            if (!dialog || !dialog->isVisible()) continue;
            QString text;
            for (auto *label : dialog->findChildren<QLabel *>()) text += label->text();
            inspectedInfo = text.contains(second) && text.contains(QStringLiteral("First page: 2"));
            informationTimer.stop();
            dialog->accept();
        }
    });
    informationTimer.start(20);
    info->trigger();
    QVERIFY(inspectedInfo);
    QSignalSpy renamed(&overview, &Thoughts::CommentsOverview::projectAttachmentChangedLocally);
    bool inspectedRename = false;
    QTimer::singleShot(0, [&] {
        for (auto *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
                inspectedRename = dialog->textValue() == QStringLiteral("A second");
                dialog->setTextValue(QStringLiteral("Renamed second document"));
                dialog->accept();
            }
        }
    });
    rename->trigger();
    QVERIFY(inspectedRename);
    QCOMPARE(renamed.size(), 1);
    // The title is saved as the document's name in the Project configuration.
    const auto titles = Thoughts::DocumentBuildSelection::read(Thoughts::DocumentBuildSelection::optionsPath(root, QStringLiteral("docs-paper"))).value(QStringLiteral("titles")).toObject();
    QCOMPARE(titles.value(second).toString(), QStringLiteral("Renamed second document"));
    QVERIFY(!titles.contains(first));
    QMenu renamedMenu;
    QVERIFY(overview.addOutlineDocumentActions(&renamedMenu, pdfPath, 2, QStringLiteral("Renamed second document")));
    QMenu oldOutline;
    QVERIFY(overview.addOutlineDocumentActions(&oldOutline, pdfPath, 2, QStringLiteral("a_second"))); // The PDF has not been rebuilt yet.

    // A later PDF must never inherit the old artifact's page map or sizes.
    pdf.close();
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("different PDF")));
    const auto reloaded = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(reloaded.size(), 1);
    QVERIFY(reloaded.first().librarySegments.isEmpty());
    QMenu staleMenu;
    QVERIFY(!overview.addOutlineDocumentActions(&staleMenu, pdfPath, 2, QStringLiteral("a_second")));
    QVERIFY(staleMenu.actions().isEmpty());
}

void CommentsOverviewTest::testDocumentInformationMetricsAndDialog()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString linkedPath = root + QStringLiteral("/current_pdfs/linked.pdf");
    const QString unlinkedPath = root + QStringLiteral("/current_pdfs/unlinked.pdf");
    const QString localPath = root + QStringLiteral("/current_pdfs/local.pdf");
    const QByteArray linkedPdf(1536, 'l');
    const QByteArray unlinkedPdf(2048, 'u');
    const QByteArray sharedStyle = QByteArrayLiteral("\\ProvidesPackage{manual_style}\n");
    QVERIFY(writeFile(linkedPath, linkedPdf));
    QVERIFY(writeFile(unlinkedPath, unlinkedPdf));
    QVERIFY(writeFile(localPath, QByteArray(3072, 'o')));
    // The manual style is project-local; there is no workspace-wide shared style.
    QVERIFY(writeFile(root + QStringLiteral("/docs-linked/tex/support/manual_style.sty"), sharedStyle));
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/linked.json"), QJsonDocument(layout(QStringLiteral("linked"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/unlinked.json"), QJsonDocument(layout(QStringLiteral("unlinked"))).toJson()));
    const QString linkedSha = QString::fromLatin1(QCryptographicHash::hash(linkedPdf, QCryptographicHash::Sha256).toHex());
    const QString unlinkedSha = QString::fromLatin1(QCryptographicHash::hash(unlinkedPdf, QCryptographicHash::Sha256).toHex());
    const QString revision(64, QLatin1Char('a'));
    const QString sourceIndexSha(64, QLatin1Char('b'));
    const QString sharedStyleSha = QString::fromLatin1(QCryptographicHash::hash(sharedStyle, QCryptographicHash::Sha256).toHex());
    const QJsonObject libraryIndex {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("library_revision"), revision},
        {QStringLiteral("source_index_sha256"), sourceIndexSha},
        {QStringLiteral("pdfs"), QJsonArray {
                                         QJsonObject {{QStringLiteral("filename"), QStringLiteral("linked.pdf")},
                                                      {QStringLiteral("kind"), QStringLiteral("source_linked")},
                                                      {QStringLiteral("pdf_sha256"), linkedSha},
                                                      {QStringLiteral("project"), QStringLiteral("docs-linked")},
                                                      {QStringLiteral("source_resources"),
                                                       QJsonArray {QJsonObject {{QStringLiteral("kind"), QStringLiteral("project_style")},
                                                                                {QStringLiteral("path"), QStringLiteral("docs-linked/tex/support/manual_style.sty")},
                                                                                {QStringLiteral("sha256"), sharedStyleSha},
                                                                                {QStringLiteral("object_sha256"), sharedStyleSha},
                                                                                {QStringLiteral("size_bytes"), sharedStyle.size()}}}},
                                                      {QStringLiteral("display_name"), QStringLiteral("Linked document")}},
                                         QJsonObject {{QStringLiteral("filename"), QStringLiteral("unlinked.pdf")},
                                                      {QStringLiteral("kind"), QStringLiteral("unlinked")},
                                                      {QStringLiteral("pdf_sha256"), unlinkedSha},
                                                      {QStringLiteral("display_name"), QStringLiteral("Imported document")}},
                                     }},
    };
    const QJsonObject closureMetadata {
        {QStringLiteral("project"), QStringLiteral("docs-linked")},
        {QStringLiteral("build_input_discovery_policy"), QStringLiteral("tex-closure-v1")},
        {QStringLiteral("verified_closure_sha256"), QString(64, QLatin1Char('c'))},
        {QStringLiteral("verified_closure_object_sha256"), QString(64, QLatin1Char('d'))},
        {QStringLiteral("verified_closure_input_count"), 68},
        {QStringLiteral("verified_closure_input_bytes"), 855579},
    };
    const QJsonObject generation {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("library_revision"), revision},
        {QStringLiteral("source_index_sha256"), sourceIndexSha},
        {QStringLiteral("publications"), QJsonArray {
                                                QJsonObject {{QStringLiteral("filename"), QStringLiteral("linked.pdf")},
                                                             {QStringLiteral("sha256"), linkedSha},
                                                             {QStringLiteral("project"), QStringLiteral("docs-linked")},
                                                             {QStringLiteral("metadata"), closureMetadata}},
                                                QJsonObject {{QStringLiteral("filename"), QStringLiteral("unlinked.pdf")},
                                                             {QStringLiteral("sha256"), unlinkedSha}},
                                            }},
    };
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(libraryIndex).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-generation.json"), QJsonDocument(generation).toJson()));

    const QList<Thoughts::TrackedCommentDocument> loaded = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(loaded.size(), 3);
    const auto linked = std::find_if(loaded.cbegin(), loaded.cend(), [](const Thoughts::TrackedCommentDocument &document) { return document.pdfPath.endsWith(QLatin1String("/linked.pdf")); });
    const auto unlinked = std::find_if(loaded.cbegin(), loaded.cend(), [](const Thoughts::TrackedCommentDocument &document) { return document.pdfPath.endsWith(QLatin1String("/unlinked.pdf")); });
    const auto local = std::find_if(loaded.cbegin(), loaded.cend(), [](const Thoughts::TrackedCommentDocument &document) { return document.pdfPath.endsWith(QLatin1String("/local.pdf")); });
    QVERIFY(linked != loaded.cend());
    QVERIFY(unlinked != loaded.cend());
    QVERIFY(local != loaded.cend());
    QCOMPARE(linked->libraryProject, QStringLiteral("docs-linked"));
    QCOMPARE(linked->pdfSizeBytes, 1536);
    QVERIFY(linked->verifiedBuildInputsAvailable);
    QCOMPARE(linked->verifiedBuildInputCount, 68);
    QCOMPARE(linked->verifiedBuildInputBytes, 855579);
    QCOMPARE(unlinked->pdfSizeBytes, 2048);
    QVERIFY(!unlinked->verifiedBuildInputsAvailable);
    QCOMPARE(local->pdfSizeBytes, 3072);
    QVERIFY(!local->verifiedBuildInputsAvailable);

    Thoughts::CommentsOverview overview(root);
    const QVariantMap monitoringInformation {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("target"), QStringLiteral("linked.pdf")},
        {QStringLiteral("status"), QStringLiteral("current")},
        {QStringLiteral("authoritative"), true},
        {QStringLiteral("input_count"), 68},
        {QStringLiteral("pending_count"), 2},
        {QStringLiteral("folders"),
         QVariantList {
             QVariantMap {
                 {QStringLiteral("path"), QStringLiteral("docs-linked/tex/source")},
                 {QStringLiteral("mode"), QStringLiteral("discovery")},
                 {QStringLiteral("shared"), false},
                 {QStringLiteral("exists"), true},
                 {QStringLiteral("tracked_bytes"), 104857},
                 {QStringLiteral("tracked_bytes_available"), true},
                 {QStringLiteral("inputs"),
                  QVariantList {
                      QVariantMap {
                          {QStringLiteral("path"), QStringLiteral("docs-linked/tex/source/main.tex")},
                          {QStringLiteral("role"), QStringLiteral("source")},
                          {QStringLiteral("source_kind"), QStringLiteral("source")},
                          {QStringLiteral("pending"), true},
                          {QStringLiteral("action"), QStringLiteral("upsert")},
                          {QStringLiteral("exists"), true},
                          {QStringLiteral("size_bytes"), 104857},
                          {QStringLiteral("size_available"), true},
                      },
                      QVariantMap {
                          {QStringLiteral("path"), QStringLiteral("docs-linked/tex/source/missing.tex")},
                          {QStringLiteral("role"), QStringLiteral("source")},
                          {QStringLiteral("source_kind"), QStringLiteral("source")},
                          {QStringLiteral("pending"), true},
                          {QStringLiteral("action"), QStringLiteral("delete")},
                          {QStringLiteral("exists"), false},
                          {QStringLiteral("size_bytes"), 0},
                          {QStringLiteral("size_available"), true},
                      },
                  }},
             },
             QVariantMap {
                 {QStringLiteral("path"), QStringLiteral("tools/latex")},
                 {QStringLiteral("mode"), QStringLiteral("discovery")},
                 {QStringLiteral("shared"), true},
                 {QStringLiteral("exists"), true},
                 {QStringLiteral("tracked_bytes"), 104858},
                 {QStringLiteral("tracked_bytes_available"), true},
                 {QStringLiteral("inputs"),
                  QVariantList {
                      QVariantMap {
                          {QStringLiteral("path"), QStringLiteral("docs-linked/tex/support/manual_style.sty")},
                          {QStringLiteral("role"), QStringLiteral("shared_style")},
                          {QStringLiteral("source_kind"), QStringLiteral("source")},
                          {QStringLiteral("pending"), false},
                          {QStringLiteral("exists"), true},
                          {QStringLiteral("size_bytes"), 104858},
                          {QStringLiteral("size_available"), true},
                      },
                  }},
             },
         }},
    };
    connect(&overview,
            &Thoughts::CommentsOverview::sourceMonitoringInformationRequested,
            &overview,
            [&overview, monitoringInformation](const QString &path) {
                overview.applySourceMonitoringInformation(path, monitoringInformation);
            });
    connect(&overview, &Thoughts::CommentsOverview::sourceMonitoringBatchRequested, &overview, [&](quint64 id, const QStringList &targets) {
        QVariantMap results;
        for (const QString &target : targets) { results.insert(target, monitoringInformation); }
        overview.applySourceMonitoringBatch(id, results);
    });
    overview.show();
    overview.showSummary();
    QTRY_COMPARE(overview.documentCount(), 3);
    auto *libraryTree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(libraryTree);
    QCOMPARE(libraryTree->headerItem()->text(3), QStringLiteral("Size"));
    QVERIFY(libraryTree->isSortingEnabled());
    QVERIFY(libraryTree->header()->sectionsClickable());
    // Library rows come only from Project configurations; per-PDF rows with
    // monitored folders were removed, while Document Information remains.
    QVERIFY(QMetaObject::invokeMethod(&overview, "showDocumentInformation", Qt::DirectConnection, Q_ARG(QString, linkedPath)));
    auto *dialog = overview.findChild<QDialog *>(QStringLiteral("documentInformationDialog"));
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    auto *project = dialog->findChild<QLabel *>(QStringLiteral("documentInformationProject"));
    auto *pdfSize = dialog->findChild<QLabel *>(QStringLiteral("documentInformationPdfSize"));
    auto *buildInputs = dialog->findChild<QLabel *>(QStringLiteral("documentInformationBuildInputs"));
    auto *viewMonitoring = dialog->findChild<QPushButton *>(QStringLiteral("documentInformationViewMonitoring"));
    auto *sharedStyleStatus = dialog->findChild<QLabel *>(QStringLiteral("documentInformationSharedStyleStatus"));
    auto *projectOverrideStatus = dialog->findChild<QLabel *>(QStringLiteral("documentInformationProjectOverrideStatus"));
    auto *openSharedStyle = dialog->findChild<QPushButton *>(QStringLiteral("documentInformationOpenSharedStyle"));
    auto *createProjectOverride = dialog->findChild<QPushButton *>(QStringLiteral("documentInformationCreateProjectOverride"));
    auto *buttons = dialog->findChild<QDialogButtonBox *>(QStringLiteral("documentInformationButtons"));
    QVERIFY(project);
    QVERIFY(pdfSize);
    QVERIFY(buildInputs);
    QVERIFY(viewMonitoring);
    QVERIFY(sharedStyleStatus);
    QVERIFY(projectOverrideStatus);
    QVERIFY(openSharedStyle);
    QVERIFY(createProjectOverride);
    QVERIFY(buttons);
    QCOMPARE(project->text(), QStringLiteral("docs-linked"));
    QString compactPdfSize = pdfSize->text();
    compactPdfSize.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")));
    QVERIFY(compactPdfSize.contains(QStringLiteral("150KiB")));
    QVERIFY(!pdfSize->text().contains(QLatin1Char('(')));
    QString compactBuildInputs = buildInputs->text();
    compactBuildInputs.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")));
    QVERIFY(compactBuildInputs.contains(QStringLiteral("Serverbuildinputs68files")));
    QVERIFY(compactBuildInputs.contains(QStringLiteral("082MiB")));
    QVERIFY(!buildInputs->text().contains(QStringLiteral("bytes")));
    QVERIFY(compactBuildInputs.contains(QStringLiteral("Localmonitoring68trackedfilesacross2monitoredfolders")));
    QVERIFY(compactBuildInputs.contains(QStringLiteral("2pendinglocalfiles")));
    QVERIFY(sharedStyleStatus->text().contains(QStringLiteral("Matches published PDF")));
    QVERIFY(sharedStyleStatus->text().contains(sharedStyleSha.left(12)));
    QCOMPARE(projectOverrideStatus->text(), QStringLiteral("No project override"));
    QVERIFY(createProjectOverride->toolTip().contains(QStringLiteral("must not be redefined")));
    QSignalSpy openSourceRequested(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    QTest::mouseClick(openSharedStyle, Qt::LeftButton);
    QCOMPARE(openSourceRequested.size(), 1);
    QCOMPARE(openSourceRequested.first().at(0).toString(), root + QStringLiteral("/docs-linked/tex/support/manual_style.sty"));
    QCOMPARE(openSourceRequested.first().at(1).toInt(), 1);
    QTest::mouseClick(viewMonitoring, Qt::LeftButton);
    auto *monitoringDialog = dialog->findChild<QDialog *>(QStringLiteral("sourceMonitoringDetailsDialog"));
    QVERIFY(monitoringDialog);
    QTRY_VERIFY(monitoringDialog->isVisible());
    auto *monitoringTree = monitoringDialog->findChild<QTreeWidget *>(QStringLiteral("sourceMonitoringTree"));
    QVERIFY(monitoringTree);
    QCOMPARE(monitoringTree->topLevelItemCount(), 2);
    QCOMPARE(monitoringTree->topLevelItem(0)->text(0), QStringLiteral("docs-linked/tex/source"));
    QCOMPARE(monitoringTree->topLevelItem(0)->text(2), QStringLiteral("Watching"));
    QCOMPARE(monitoringTree->topLevelItem(0)->childCount(), 2);
    QTreeWidgetItem *monitoredMain = nullptr;
    QTreeWidgetItem *monitoredMissing = nullptr;
    for (int child = 0; child < monitoringTree->topLevelItem(0)->childCount(); ++child) {
        QTreeWidgetItem *item = monitoringTree->topLevelItem(0)->child(child);
        if (item->text(0) == QLatin1String("main.tex")) {
            monitoredMain = item;
        } else if (item->text(0) == QLatin1String("missing.tex")) {
            monitoredMissing = item;
        }
    }
    QVERIFY(monitoredMain);
    QVERIFY(monitoredMissing);
    QCOMPARE(monitoredMain->text(2), QStringLiteral("Pending local change"));
    QCOMPARE(monitoredMissing->text(2), QStringLiteral("Pending deletion"));
    monitoringTree->setCurrentItem(monitoredMain);
    auto *openMonitoredSource = monitoringDialog->findChild<QPushButton *>(QStringLiteral("sourceMonitoringOpenSource"));
    QVERIFY(openMonitoredSource);
    QVERIFY(openMonitoredSource->isEnabled());
    QTest::mouseClick(openMonitoredSource, Qt::LeftButton);
    QCOMPARE(openSourceRequested.size(), 2);
    QCOMPARE(openSourceRequested.last().at(0).toString(), root + QStringLiteral("/docs-linked/tex/source/main.tex"));
    monitoringDialog->close();
    QCOMPARE(buttons->standardButtons(), QDialogButtonBox::Close);
    QCOMPARE(buttons->buttons().size(), 1);
    dialog->close();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);

    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(documents);
    QTreeWidgetItem *localItem = documentItem(documents, QStringLiteral("local"));
    QVERIFY(localItem);
    // Documents without a Library project share the No project container.
    localItem->parent()->setExpanded(true);
    documents->scrollToItem(localItem);
    bool informationFound = false;
    bool informationAndOpenOnly = false;
    QTimer::singleShot(0, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) {
            return;
        }
        const QList<QAction *> actions = menu->actions();
        const auto information = std::find_if(actions.cbegin(), actions.cend(), [](const QAction *action) { return action->objectName() == QLatin1String("documentInformationAction"); });
        informationFound = information != actions.cend();
        informationAndOpenOnly = actions.size() == 2 && actions.first()->text() == QStringLiteral("Open PDF");
        if (information != actions.cend()) {
            (*information)->trigger();
        }
        menu->close();
    });
    const QPoint localPosition = documents->visualItemRect(localItem).center();
    QVERIFY(QMetaObject::invokeMethod(&overview, "showDocumentsContextMenu", Qt::DirectConnection, Q_ARG(QPoint, localPosition)));
    QVERIFY(informationFound);
    QVERIFY(informationAndOpenOnly);
    dialog = overview.findChild<QDialog *>(QStringLiteral("documentInformationDialog"));
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("documentInformationProject"))->text(), QStringLiteral("No build project"));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("documentInformationBuildInputs"))->text(), QStringLiteral("Not applicable — no docs-* build project."));
    dialog->close();

    QVERIFY(!QApplication::activePopupWidget());
    const QPoint allCommentsPosition = documents->visualItemRect(documents->topLevelItem(0)).center();
    QVERIFY(QMetaObject::invokeMethod(&overview, "showDocumentsContextMenu", Qt::DirectConnection, Q_ARG(QPoint, allCommentsPosition)));
    QVERIFY(!QApplication::activePopupWidget());
    const QPoint headingPosition = documents->visualItemRect(documents->topLevelItem(1)).center();
    QVERIFY(QMetaObject::invokeMethod(&overview, "showDocumentsContextMenu", Qt::DirectConnection, Q_ARG(QPoint, headingPosition)));
    QVERIFY(!QApplication::activePopupWidget());
}

void CommentsOverviewTest::testDocumentInformationRejectsInvalidMetrics()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QByteArray pdf(512, 'p');
    const QString pdfPath = root + QStringLiteral("/current_pdfs/paper.pdf");
    QVERIFY(writeFile(pdfPath, pdf));
    const QString pdfSha = QString::fromLatin1(QCryptographicHash::hash(pdf, QCryptographicHash::Sha256).toHex());
    const QString revision(64, QLatin1Char('1'));
    const QString sourceIndexSha(64, QLatin1Char('2'));
    const QJsonObject index {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("library_revision"), revision},
        {QStringLiteral("source_index_sha256"), sourceIndexSha},
        {QStringLiteral("pdfs"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                                          {QStringLiteral("kind"), QStringLiteral("source_linked")},
                                                          {QStringLiteral("pdf_sha256"), pdfSha},
                                                          {QStringLiteral("project"), QStringLiteral("docs-paper")}}}},
    };
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(index).toJson()));
    const auto writeGeneration = [&](const QJsonObject &metadata, const QString &publicationSha = QString()) {
        const QJsonObject generation {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("library_revision"), revision},
            {QStringLiteral("source_index_sha256"), sourceIndexSha},
            {QStringLiteral("publications"), QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                                                      {QStringLiteral("sha256"), publicationSha.isEmpty() ? pdfSha : publicationSha},
                                                                      {QStringLiteral("metadata"), metadata}}}},
        };
        return writeFile(root + QStringLiteral("/current_pdfs/.thoughts-generation.json"), QJsonDocument(generation).toJson());
    };
    const auto loadedDocument = [&]() {
        return Thoughts::loadTrackedCommentDocuments(root).constFirst();
    };

    QJsonObject metadata {
        {QStringLiteral("build_input_discovery_policy"), QStringLiteral("tex-closure-v1")},
        {QStringLiteral("verified_closure_sha256"), QString(64, QLatin1Char('3'))},
        {QStringLiteral("verified_closure_object_sha256"), QString(64, QLatin1Char('4'))},
        {QStringLiteral("verified_closure_input_count"), 4.5},
        {QStringLiteral("verified_closure_input_bytes"), 1024},
    };
    QVERIFY(writeGeneration(metadata));
    QVERIFY(!loadedDocument().verifiedBuildInputsAvailable);
    metadata.insert(QStringLiteral("verified_closure_input_count"), 4);
    metadata.insert(QStringLiteral("verified_closure_input_bytes"), -1);
    QVERIFY(writeGeneration(metadata));
    QVERIFY(!loadedDocument().verifiedBuildInputsAvailable);
    metadata.insert(QStringLiteral("verified_closure_input_bytes"), 1024);
    QVERIFY(writeGeneration(metadata));
    QVERIFY(writeFile(pdfPath, QByteArray(512, 'x')));
    QVERIFY(!loadedDocument().verifiedBuildInputsAvailable);
    QVERIFY(writeFile(pdfPath, pdf));
    QVERIFY(writeGeneration(metadata, QString(64, QLatin1Char('f'))));
    const Thoughts::TrackedCommentDocument mismatched = loadedDocument();
    QVERIFY(!mismatched.libraryGenerationCurrent);
    QVERIFY(!mismatched.verifiedBuildInputsAvailable);
    QVERIFY(writeGeneration(QJsonObject {}));
    QVERIFY(!loadedDocument().verifiedBuildInputsAvailable);

    Thoughts::CommentsOverview overview(root);
    connect(&overview,
            &Thoughts::CommentsOverview::sourceMonitoringInformationRequested,
            &overview,
            [&overview](const QString &path) {
                overview.applySourceMonitoringInformation(
                    path,
                    QVariantMap {
                        {QStringLiteral("schema_version"), 1},
                        {QStringLiteral("status"), QStringLiteral("unavailable")},
                        {QStringLiteral("error"), QStringLiteral("this publication has no verified build-closure record")},
                    });
            });
    overview.show();
    QTRY_COMPARE(overview.documentCount(), 1);
    QVERIFY(QMetaObject::invokeMethod(&overview, "showDocumentInformation", Qt::DirectConnection, Q_ARG(QString, pdfPath)));
    auto *dialog = overview.findChild<QDialog *>(QStringLiteral("documentInformationDialog"));
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->isVisible());
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("documentInformationBuildInputs"))->text(), QStringLiteral("Unavailable — this publication has no verified build-closure record"));
    dialog->close();
}

void CommentsOverviewTest::testUncheckedDocumentIsNoBuildTargetAndHidden()
{
    // A document unchecked in its Project keeps its PDF, but Fedora neither
    // builds nor lists it. It read as "Sync needed" forever (no render status)
    // and stayed in the Library; checking it again brings it back.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString revision(64, QLatin1Char('d'));
    const QString sourceIndexSha256(64, QLatin1Char('e'));
    const QString keptId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString uncheckedId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonArray pdfs, publications;
    for (const auto &[stem, id, digit] : {std::tuple{QStringLiteral("kept"), keptId, QLatin1Char('a')},
                                          std::tuple{QStringLiteral("document_29cb63b7b7b95fd73f56"), uncheckedId, QLatin1Char('b')}}) {
        QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/%1.pdf").arg(stem), QByteArrayLiteral("placeholder")));
        QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/%1.json").arg(stem), QJsonDocument(layout(stem)).toJson()));
        const QString sha256(64, digit);
        const QString filename = stem + QStringLiteral(".pdf");
        pdfs.append(QJsonObject {{QStringLiteral("filename"), filename}, {QStringLiteral("kind"), QStringLiteral("source_linked")},
                                 {QStringLiteral("pdf_sha256"), sha256}, {QStringLiteral("document_id"), id}, {QStringLiteral("project"), QStringLiteral("docs-eth")},
                                 {QStringLiteral("published_at"), QStringLiteral("2026-10-08T10:00:00.123456Z")}, {QStringLiteral("pdf_stale"), false}});
        publications.append(QJsonObject {{QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), sha256}});
    }
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"),
        QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("library_revision"), revision},
                                   {QStringLiteral("source_index_sha256"), sourceIndexSha256}, {QStringLiteral("pdfs"), pdfs}}).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-generation.json"),
        QJsonDocument(QJsonObject {{QStringLiteral("schema_version"), 1}, {QStringLiteral("library_revision"), revision},
                                   {QStringLiteral("source_index_sha256"), sourceIndexSha256}, {QStringLiteral("publications"), publications}}).toJson()));

    const auto manifestPath = Thoughts::ProjectConfiguration::path(root, QStringLiteral("eth"));
    auto configuration = Thoughts::ProjectConfiguration::empty(root, QStringLiteral("eth"), QStringLiteral("ETH"));
    const QString folderId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject {{QStringLiteral("folder_id"), folderId}, {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
                                {QStringLiteral("path"), QStringLiteral("docs-eth")}});
    configuration.insert(QStringLiteral("source_folders"), folders);
    const auto declare = [&](bool uncheckedIncluded) {
        configuration.insert(QStringLiteral("documents"), QJsonArray {
            QJsonObject {{QStringLiteral("document_id"), keptId}, {QStringLiteral("format"), QStringLiteral("markdown")}, {QStringLiteral("included"), true},
                         {QStringLiteral("source"), QJsonObject {{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), QStringLiteral("markdown/source/kept.md")}}}},
            QJsonObject {{QStringLiteral("document_id"), uncheckedId}, {QStringLiteral("format"), QStringLiteral("markdown")}, {QStringLiteral("included"), uncheckedIncluded},
                         {QStringLiteral("source"), QJsonObject {{QStringLiteral("folder_id"), folderId}, {QStringLiteral("path"), QStringLiteral("markdown/source/weekly.md")}}}}});
        return Thoughts::ProjectConfiguration::save(manifestPath, configuration);
    };
    QVERIFY(declare(false).isEmpty());
    QCOMPARE(Thoughts::ProjectConfiguration::unselectedSourceDocumentIds({configuration}), QSet<QString> {uncheckedId});
    // Selected in any Project wins.
    auto other = configuration;
    other.insert(QStringLiteral("documents"), QJsonArray {QJsonObject {{QStringLiteral("document_id"), uncheckedId}, {QStringLiteral("format"), QStringLiteral("markdown")}, {QStringLiteral("included"), true}}});
    QVERIFY(Thoughts::ProjectConfiguration::unselectedSourceDocumentIds({configuration, other}).isEmpty());

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(documents);
    const auto descriptorFor = [&overview](const QString &filename) {
        for (const auto &entry : overview.documentSyncDescriptors())
            if (entry.toMap().value(QStringLiteral("filename")).toString() == filename) return entry.toMap();
        return QVariantMap {};
    };
    QTRY_COMPARE(overview.documentCount(), 2);
    QTRY_VERIFY(!descriptorFor(QStringLiteral("document_29cb63b7b7b95fd73f56.pdf")).isEmpty());
    const auto unchecked = descriptorFor(QStringLiteral("document_29cb63b7b7b95fd73f56.pdf"));
    QVERIFY(unchecked.value(QStringLiteral("source_linked")).toBool());
    QVERIFY(!unchecked.value(QStringLiteral("build_target"), true).toBool());
    QVERIFY(descriptorFor(QStringLiteral("kept.pdf")).value(QStringLiteral("build_target")).toBool());
    QVERIFY(documentItem(documents, QStringLiteral("kept")));
    QVERIFY(!documentItem(documents, QStringLiteral("document_29cb63b7b7b95fd73f56")));

    // Without a render status (render-state leaves it out) it is current, not "Sync needed".
    Thoughts::DocumentSyncState state;
    state.sourceLinked = true;
    state.deferLocalFreshness = true;
    state.buildTarget = unchecked.value(QStringLiteral("build_target"), true).toBool();
    const auto presentation = Thoughts::documentSyncPresentation(state);
    QCOMPARE(presentation.state, Thoughts::DocumentSyncStatus::Current);
    QVERIFY(!presentation.requiresSync());
    QVERIFY(!presentation.requiresBuild());
    QCOMPARE(Thoughts::aggregateSyncPresentation({presentation}).pendingDocuments, 0);
    // Its kept comments can still wait for a Sync.
    state.pendingDocumentSync = {{QStringLiteral("comments"), 1}, {QStringLiteral("total"), 1}};
    QCOMPARE(Thoughts::documentSyncPresentation(state).state, Thoughts::DocumentSyncStatus::SyncNeeded);
    // Before the fix: a build target without a status waits for Sync forever.
    state.pendingDocumentSync = {};
    state.buildTarget = true;
    QCOMPARE(Thoughts::documentSyncPresentation(state).state, Thoughts::DocumentSyncStatus::SyncNeeded);

    // Checking it again lists and builds it again.
    QVERIFY(declare(true).isEmpty());
    overview.reload();
    QTRY_VERIFY(descriptorFor(QStringLiteral("document_29cb63b7b7b95fd73f56.pdf")).value(QStringLiteral("build_target")).toBool());
    QTRY_VERIFY(documentItem(documents, QStringLiteral("document_29cb63b7b7b95fd73f56")));
}

void CommentsOverviewTest::testLibraryBuildWarnings()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString revision(64, QLatin1Char('d'));
    const QString sourceIndexSha256(64, QLatin1Char('e'));
    const QString healthySha256(64, QLatin1Char('a'));
    const QString healthyUnlinkedSha256(64, QLatin1Char('f'));
    const QString directFailureSha256(64, QLatin1Char('b'));
    const QString fallbackFailureSha256(64, QLatin1Char('c'));
    for (const QString &stem : {QStringLiteral("healthy"), QStringLiteral("direct-failure"), QStringLiteral("fallback-failure")}) {
        QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/%1.pdf").arg(stem), QByteArrayLiteral("placeholder")));
        QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/%1.json").arg(stem), QJsonDocument(layout(stem)).toJson()));
    }
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/healthy-unlinked.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/healthy-unlinked.json"), QJsonDocument(layout(QStringLiteral("healthy-unlinked"))).toJson()));

    const auto libraryIndex = [&](bool buildsStale) {
        QJsonObject directBuild;
        QJsonObject fallbackBuild;
        if (buildsStale) {
            directBuild = QJsonObject {{QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("error"), QStringLiteral("renderer exited with 1")}};
            fallbackBuild = QJsonObject {{QStringLiteral("status"), QStringLiteral("attention")},
                                         {QStringLiteral("failed_documents"),
                                          QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("fallback-failure.pdf")},
                                                                  {QStringLiteral("message"), QStringLiteral("missing Source label")}}}}};
        }
        return QJsonObject {
            {QStringLiteral("schema_version"), 1},
            {QStringLiteral("library_revision"), revision},
            {QStringLiteral("source_index_sha256"), sourceIndexSha256},
            {QStringLiteral("pdfs"),
             QJsonArray {
                 QJsonObject {{QStringLiteral("filename"), QStringLiteral("healthy.pdf")},
                              {QStringLiteral("kind"), QStringLiteral("source_linked")},
                              {QStringLiteral("pdf_sha256"), healthySha256},
                              {QStringLiteral("display_name"), QStringLiteral("Healthy")},
                              {QStringLiteral("published_at"), QStringLiteral("2026-08-16T08:00:00Z")},
                              {QStringLiteral("pdf_stale"), false}},
                 QJsonObject {{QStringLiteral("filename"), QStringLiteral("healthy-unlinked.pdf")},
                              {QStringLiteral("kind"), QStringLiteral("unlinked")},
                              {QStringLiteral("pdf_sha256"), healthyUnlinkedSha256},
                              {QStringLiteral("display_name"), QStringLiteral("Healthy unlinked")},
                              {QStringLiteral("published_at"), QStringLiteral("2026-08-16T08:00:00Z")},
                              {QStringLiteral("pdf_stale"), false}},
                 QJsonObject {{QStringLiteral("filename"), QStringLiteral("direct-failure.pdf")},
                              {QStringLiteral("kind"), QStringLiteral("source_linked")},
                              {QStringLiteral("pdf_sha256"), directFailureSha256},
                              {QStringLiteral("display_name"), QStringLiteral("Direct failure")},
                              {QStringLiteral("published_at"), QStringLiteral("2026-08-16T08:00:00Z")},
                              {QStringLiteral("pdf_stale"), buildsStale},
                              {QStringLiteral("latest_build"), directBuild}},
                 QJsonObject {{QStringLiteral("filename"), QStringLiteral("fallback-failure.pdf")},
                              {QStringLiteral("kind"), QStringLiteral("source_linked")},
                              {QStringLiteral("pdf_sha256"), fallbackFailureSha256},
                              {QStringLiteral("display_name"), QStringLiteral("Fallback failure")},
                              {QStringLiteral("published_at"), QStringLiteral("2026-08-16T08:00:00Z")},
                              {QStringLiteral("pdf_stale"), buildsStale},
                              {QStringLiteral("latest_build"), fallbackBuild}},
             }},
        };
    };
    const QJsonObject generation {
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("library_revision"), revision},
        {QStringLiteral("source_index_sha256"), sourceIndexSha256},
        {QStringLiteral("publications"),
         QJsonArray {
             QJsonObject {{QStringLiteral("filename"), QStringLiteral("healthy.pdf")}, {QStringLiteral("sha256"), healthySha256}},
             QJsonObject {{QStringLiteral("filename"), QStringLiteral("healthy-unlinked.pdf")}, {QStringLiteral("sha256"), healthyUnlinkedSha256}},
             QJsonObject {{QStringLiteral("filename"), QStringLiteral("direct-failure.pdf")}, {QStringLiteral("sha256"), directFailureSha256}},
             QJsonObject {{QStringLiteral("filename"), QStringLiteral("fallback-failure.pdf")}, {QStringLiteral("sha256"), fallbackFailureSha256}},
         }},
    };
    const QString indexPath = root + QStringLiteral("/current_pdfs/.thoughts-library-index.json");
    QVERIFY(writeFile(indexPath, QJsonDocument(libraryIndex(true)).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-generation.json"), QJsonDocument(generation).toJson()));

    const QList<Thoughts::TrackedCommentDocument> loaded = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(loaded.size(), 4);
    const auto directDocument = std::find_if(loaded.cbegin(), loaded.cend(), [](const Thoughts::TrackedCommentDocument &document) { return document.stem == QLatin1String("direct-failure"); });
    const auto fallbackDocument = std::find_if(loaded.cbegin(), loaded.cend(), [](const Thoughts::TrackedCommentDocument &document) { return document.stem == QLatin1String("fallback-failure"); });
    QVERIFY(directDocument != loaded.cend());
    QVERIFY(fallbackDocument != loaded.cend());
    QVERIFY(directDocument->libraryPdfStale);
    QCOMPARE(directDocument->libraryLatestBuildMessage, QStringLiteral("renderer exited with 1"));
    QVERIFY(fallbackDocument->libraryPdfStale);
    QCOMPARE(fallbackDocument->libraryLatestBuildMessage, QStringLiteral("missing Source label"));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.documentCount(), 4);
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(documents);
    const auto itemForStem = [documents](const QString &stem) { return documentItem(documents, stem); };
    const QString warningIconName = QIcon::fromTheme(QStringLiteral("dialog-warning")).name();
    QCOMPARE(documents->topLevelItem(0)->icon(0).name(), warningIconName);
    QVERIFY(documents->topLevelItem(0)->toolTip(0).contains(QStringLiteral("2 documents")));
    QVERIFY(documents->topLevelItem(0)->toolTip(0).contains(QStringLiteral("Direct failure")));
    QVERIFY(documents->topLevelItem(0)->toolTip(0).contains(QStringLiteral("Fallback failure")));
    QCOMPARE(itemForStem(QStringLiteral("direct-failure"))->icon(0).name(), warningIconName);
    QVERIFY(itemForStem(QStringLiteral("direct-failure"))->toolTip(0).contains(QStringLiteral("last-good publication")));
    QVERIFY(itemForStem(QStringLiteral("direct-failure"))->toolTip(0).contains(QStringLiteral("renderer exited with 1")));
    QCOMPARE(itemForStem(QStringLiteral("fallback-failure"))->icon(0).name(), warningIconName);
    QVERIFY(itemForStem(QStringLiteral("fallback-failure"))->toolTip(0).contains(QStringLiteral("missing Source label")));
    QVERIFY((itemForStem(QStringLiteral("healthy"))->icon(0).isNull() || itemForStem(QStringLiteral("healthy"))->icon(0).name() != warningIconName));
    QVERIFY((itemForStem(QStringLiteral("healthy-unlinked"))->icon(0).isNull() || itemForStem(QStringLiteral("healthy-unlinked"))->icon(0).name() != warningIconName));
    QVERIFY(itemForStem(QStringLiteral("healthy"))->toolTip(0).contains(QStringLiteral("Library: current")));
    QVERIFY(!itemForStem(QStringLiteral("healthy"))->toolTip(0).contains(QStringLiteral("failed")));
    const QVariantList descriptors = overview.documentSyncDescriptors();
    const auto unlinkedDescriptor = std::find_if(descriptors.cbegin(), descriptors.cend(), [](const QVariant &value) {
        return value.toMap().value(QStringLiteral("filename")).toString() == QLatin1String("healthy-unlinked.pdf");
    });
    QVERIFY(unlinkedDescriptor != descriptors.cend());
    QVERIFY(!unlinkedDescriptor->toMap().value(QStringLiteral("source_linked")).toBool());

    QVERIFY(writeFile(indexPath, QJsonDocument(libraryIndex(false)).toJson()));
    overview.reload();
    QTRY_VERIFY(documents->topLevelItem(0)->icon(0).isNull());
    QVERIFY(documents->topLevelItem(0)->toolTip(0).isEmpty());
    QVERIFY((itemForStem(QStringLiteral("direct-failure"))->icon(0).isNull() || itemForStem(QStringLiteral("direct-failure"))->icon(0).name() != warningIconName));
    QVERIFY((itemForStem(QStringLiteral("fallback-failure"))->icon(0).isNull() || itemForStem(QStringLiteral("fallback-failure"))->icon(0).name() != warningIconName));
    QVERIFY(!itemForStem(QStringLiteral("direct-failure"))->toolTip(0).contains(QStringLiteral("failed")));
    QVERIFY(!itemForStem(QStringLiteral("fallback-failure"))->toolTip(0).contains(QStringLiteral("failed")));
    // A retained failed publication stops being a build target when its source
    // folder is absent from the current configuration; connected failures remain.
    const auto manifestPath = Thoughts::ProjectConfiguration::path(root, QStringLiteral("current"));
    auto configuration = Thoughts::ProjectConfiguration::empty(root, QStringLiteral("current"), QStringLiteral("Current"));
    auto folders = configuration.value(QStringLiteral("source_folders")).toArray();
    folders.append(QJsonObject{{QStringLiteral("folder_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("path"), QStringLiteral("docs-direct-failure")}});
    configuration.insert(QStringLiteral("source_folders"), folders);
    QVERIFY(Thoughts::ProjectConfiguration::save(manifestPath, configuration).isEmpty());
    auto retainedIndex = libraryIndex(true);
    auto retainedPdfs = retainedIndex.value(QStringLiteral("pdfs")).toArray();
    for (int i = 0; i < retainedPdfs.size(); ++i) {
        auto entry = retainedPdfs[i].toObject();
        entry.insert(QStringLiteral("project"), QString(QStringLiteral("docs-") + QFileInfo(entry.value(QStringLiteral("filename")).toString()).completeBaseName()));
        retainedPdfs[i] = entry;
    }
    retainedIndex.insert(QStringLiteral("pdfs"), retainedPdfs);
    QVERIFY(writeFile(indexPath, QJsonDocument(retainedIndex).toJson()));
    overview.reload();
    const auto descriptorFor = [&overview](const QString &filename) {
        for (const auto &entry : overview.documentSyncDescriptors())
            if (entry.toMap().value(QStringLiteral("filename")).toString() == filename) return entry.toMap();
        return QVariantMap{};
    };
    QTRY_VERIFY(descriptorFor(QStringLiteral("direct-failure.pdf")).value(QStringLiteral("source_linked")).toBool());
    QTRY_VERIFY(!descriptorFor(QStringLiteral("fallback-failure.pdf")).value(QStringLiteral("source_linked")).toBool());
    QCOMPARE(overview.documentSyncDescriptors().size(), 4);
    const auto detached = descriptorFor(QStringLiteral("fallback-failure.pdf"));
    QVERIFY(detached.value(QStringLiteral("library_pdf_stale")).toBool());
    QVERIFY(!detached.value(QStringLiteral("latest_build_message")).toString().isEmpty());
    Thoughts::DocumentSyncState state;
    state.sourceLinked = detached.value(QStringLiteral("source_linked")).toBool();
    state.libraryPdfStale = true;
    state.latestBuildMessage = detached.value(QStringLiteral("latest_build_message")).toString();
    state.pendingDocumentSync = {{QStringLiteral("comments"), 1}, {QStringLiteral("total"), 1}};
    const auto presentation = Thoughts::documentSyncPresentation(state);
    QVERIFY(!presentation.lastBuildFailed);
    QCOMPARE(presentation.action, Thoughts::DocumentSyncAction::Sync);
    QVERIFY(QFileInfo::exists(root + QStringLiteral("/current_pdfs/fallback-failure.pdf")));
    // Unreadable settings are unknown ownership, not proof of disconnection.
    QVERIFY(writeFile(manifestPath, QByteArrayLiteral("invalid json")));
    overview.reload();
    QTRY_VERIFY(descriptorFor(QStringLiteral("fallback-failure.pdf")).value(QStringLiteral("source_linked")).toBool());

}

void CommentsOverviewTest::testSyncFailureKinds()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    QVERIFY(!overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewLocalChanges")));
    auto *status = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewPdfSyncStatus"));
    auto *timestamp = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewPdfSyncTimestamp"));
    auto *logs = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewSyncLogsLink"));
    auto *sync = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewSync"));
    auto *review = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewReviewCards"));
    QVERIFY(status && timestamp && logs && sync && review);
    QTRY_VERIFY(sync->geometry().left() < review->geometry().left());
    const QJsonObject clean {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}};
    overview.setSyncSnapshot({}, {}, clean, {});
    QCOMPARE(status->text(), QStringLiteral("No pending changes"));
    QVERIFY(logs->isHidden());
    overview.syncStarted();
    overview.syncFinished(false, QStringLiteral("Server unreachable"), QStringLiteral("Host not found"), QStringLiteral("sync_failed"), QStringLiteral("failed-sync"));
    QCOMPARE(status->text(), QStringLiteral("Sync failed"));
    QCOMPARE(timestamp->text(), QStringLiteral("Last attempt: just now"));
    QVERIFY(timestamp->toolTip().contains(QStringLiteral("Host not found")));
    QVERIFY(!logs->isHidden());
    QSignalSpy requested(&overview, &Thoughts::CommentsOverview::syncOperationLogsRequested);
    QCoreApplication::processEvents();
    QTest::mouseClick(logs, Qt::LeftButton, Qt::NoModifier,
                     QPoint(logs->width() - logs->fontMetrics().horizontalAdvance(QStringLiteral("Show logs")) / 2, logs->height() / 2));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.takeFirst().at(0).toString(), QStringLiteral("failed-sync"));
    for (const QString &code : {QStringLiteral("pdf_build_failed"), QStringLiteral("build_failed")}) {
        overview.syncStarted();
        QVERIFY(logs->isHidden());
        overview.syncFinished(false, QStringLiteral("PDF compilation failed"), QStringLiteral("Undefined control sequence"), code, code);
        QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
        QCOMPARE(timestamp->text(), QStringLiteral("Last attempt: just now"));
        QVERIFY(timestamp->toolTip().contains(QStringLiteral("Undefined control sequence")));
        QVERIFY(!logs->isHidden());
    }
    overview.syncStarted();
    overview.syncFinished(true, {});
    QCOMPARE(status->text(), QStringLiteral("Up to date"));
    QVERIFY(logs->isHidden());
    const QDateTime lastSync = QDateTime::currentDateTimeUtc().addSecs(-120);
    overview.setSyncSnapshot({}, {}, clean, lastSync);
    QCOMPARE(status->text(), QStringLiteral("Up to date"));
    QCOMPARE(timestamp->text(), QStringLiteral("Last sync: 2m ago"));
    const QJsonObject failedBuild {{QStringLiteral("state"), QStringLiteral("error")},
                                   {QStringLiteral("requires_build"), true}};
    const QJsonObject buildAggregate {{QStringLiteral("state"), QStringLiteral("error")},
                                      {QStringLiteral("requires_sync"), true},
                                      {QStringLiteral("pending_documents"), 1}};
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), failedBuild}}, buildAggregate, clean, lastSync);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Your changes are synced · 1 PDF failed to build.")));
    QVERIFY(!logs->isHidden());
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), failedBuild}, {QStringLiteral("other.pdf"), failedBuild}}, buildAggregate, clean, lastSync);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Your changes are synced · 2 PDFs failed to build.")));
    QVERIFY(!logs->isHidden());
    const QJsonObject successfulSync {{QStringLiteral("id"), QStringLiteral("synced-with-build-warning")},
                                     {QStringLiteral("kind"), QStringLiteral("sync")},
                                     {QStringLiteral("status"), QStringLiteral("succeeded")},
                                     {QStringLiteral("updated_at_ms"), lastSync.toMSecsSinceEpoch()}};
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), failedBuild}}, buildAggregate, clean, lastSync, successfulSync);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Your changes are synced · 1 PDF failed to build.")));
    QVERIFY(!logs->isHidden());
    QCoreApplication::processEvents();
    QTest::mouseClick(logs, Qt::LeftButton, Qt::NoModifier,
                     QPoint(logs->width() - logs->fontMetrics().horizontalAdvance(QStringLiteral("Show logs")) / 2, logs->height() / 2));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.takeFirst().at(0).toString(), QStringLiteral("synced-with-build-warning"));
    overview.syncStarted();
    QVERIFY(logs->isHidden());
    overview.syncFinished(true, {});
    overview.setSyncSnapshot({}, {}, clean, lastSync, successfulSync);
    QCOMPARE(status->text(), QStringLiteral("Up to date"));
    QVERIFY(logs->isHidden());
    QJsonObject unsynced = clean;
    unsynced.insert(QStringLiteral("total"), 1);
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), failedBuild}}, buildAggregate, unsynced, lastSync);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Changes pending · 1 PDF failed to build.")));
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), failedBuild}}, buildAggregate, unsynced, lastSync, successfulSync);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Changes pending · 1 PDF failed to build.")));
    QJsonObject unknown = clean;
    unknown.insert(QStringLiteral("source_state"), QStringLiteral("unknown"));
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), failedBuild}}, buildAggregate, unknown, lastSync, successfulSync);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Your changes are synced · 1 PDF failed to build.")));
    QVERIFY(!logs->isHidden());
    QVERIFY(status->toolTip().contains(QStringLiteral("Local source tracking is incomplete")));
    // Exercise the real document projection: a local edit changes the next action
    // to Sync but must preserve the failed build and access to its logs.
    Thoughts::DocumentSyncState editedFailure;
    editedFailure.localFreshnessAvailable = true;
    editedFailure.localSourcesCurrent = false;
    editedFailure.renderStatus = {{QStringLiteral("status"), QStringLiteral("error")}};
    const auto editedPresentation = Thoughts::documentSyncPresentation(editedFailure);
    QVERIFY(editedPresentation.requiresSync());
    QVERIFY(editedPresentation.lastBuildFailed);
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), editedPresentation.toJson()}},
                             Thoughts::aggregateSyncPresentation({editedPresentation}).toJson(), unsynced, lastSync, successfulSync);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Changes pending · 1 PDF failed to build.")));
    QVERIFY(!logs->isHidden());
    QTest::mouseClick(logs, Qt::LeftButton, Qt::NoModifier,
                     QPoint(logs->width() - logs->fontMetrics().horizontalAdvance(QStringLiteral("Show logs")) / 2, logs->height() / 2));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.takeFirst().at(0).toString(), QStringLiteral("synced-with-build-warning"));

    // A completed Sync with a PDF failure also sets the manual error state.
    // The persisted operation must retain its error code in the overview query.
    editedFailure.manualSyncState = 3;
    const auto completedFailure = Thoughts::documentSyncPresentation(editedFailure);
    QJsonObject buildFailedOperation = successfulSync;
    buildFailedOperation.insert(QStringLiteral("status"), QStringLiteral("failed"));
    buildFailedOperation.insert(QStringLiteral("last_error_code"), QStringLiteral("pdf_build_failed"));
    buildFailedOperation.insert(QStringLiteral("sync_outcome"), QStringLiteral("changes_synced"));
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), completedFailure.toJson()}},
                             Thoughts::aggregateSyncPresentation({completedFailure}).toJson(), clean, lastSync, buildFailedOperation);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Your changes are synced · 1 PDF failed to build.")));
    QVERIFY(!logs->isHidden());
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), completedFailure.toJson()}},
                             Thoughts::aggregateSyncPresentation({completedFailure}).toJson(), unsynced, lastSync, buildFailedOperation);
    QCOMPARE(status->text(), QStringLiteral("PDF build failed"));
    QVERIFY(status->toolTip().startsWith(QStringLiteral("Changes pending · 1 PDF failed to build.")));
    QVERIFY(!logs->isHidden());
    editedFailure.manualSyncState = 0;

    editedFailure.buildActivity = {{QStringLiteral("active"), true}};
    const auto retryPresentation = Thoughts::documentSyncPresentation(editedFailure);
    QVERIFY(!retryPresentation.lastBuildFailed);
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), retryPresentation.toJson()}},
                             Thoughts::aggregateSyncPresentation({retryPresentation}).toJson(), unsynced, lastSync, successfulSync);
    QVERIFY(status->text() != QStringLiteral("PDF build failed"));
    QVERIFY(!status->toolTip().contains(QStringLiteral("failed to build")));
    QVERIFY(logs->isHidden());

    const QJsonObject installationError {{QStringLiteral("state"), QStringLiteral("error")},
                                        {QStringLiteral("requires_build"), false}};
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), installationError}}, buildAggregate, clean, lastSync, successfulSync);
    QCOMPARE(status->text(), QStringLiteral("Sync failed"));
    QJsonObject connectionFailure {{QStringLiteral("status"), QStringLiteral("failed")},
                                    {QStringLiteral("last_error_code"), QStringLiteral("sync_failed")}};
    overview.setSyncSnapshot({{QStringLiteral("paper.pdf"), failedBuild}}, buildAggregate, clean, lastSync, connectionFailure);
    QCOMPARE(status->text(), QStringLiteral("Sync failed"));
    QVERIFY(!status->toolTip().startsWith(QStringLiteral("Your changes are synced")));
    QJsonObject pending = clean;
    pending.insert(QStringLiteral("total"), 1);
    overview.setSyncSnapshot({}, {}, pending, lastSync);
    QCOMPARE(status->text(), QStringLiteral("Changes pending"));
    QCOMPARE(timestamp->text(), QStringLiteral("Last sync: 2m ago"));
    QVERIFY(logs->isHidden());
    pending.insert(QStringLiteral("total"), 0);
    pending.insert(QStringLiteral("source_state"), QStringLiteral("unknown"));
    overview.setSyncSnapshot({}, {}, pending, lastSync);
    QCOMPARE(status->text(), QStringLiteral("Checking local changes…"));
    const QJsonObject failure {{QStringLiteral("id"), QStringLiteral("restored-failure")},
                              {QStringLiteral("status"), QStringLiteral("failed")},
                              {QStringLiteral("last_error_code"), QStringLiteral("pdf_build_failed")},
                              {QStringLiteral("updated_at_ms"), QDateTime::currentMSecsSinceEpoch()}};
    overview.setSyncSnapshot({}, {}, clean, lastSync, failure);
    QVERIFY(!logs->isHidden());
    QCoreApplication::processEvents();
    QTest::mouseClick(logs, Qt::LeftButton, Qt::NoModifier,
                     QPoint(logs->width() - logs->fontMetrics().horizontalAdvance(QStringLiteral("Show logs")) / 2, logs->height() / 2));
    QCOMPARE(requested.count(), 1);
    QCOMPARE(requested.takeFirst().at(0).toString(), QStringLiteral("restored-failure"));
}

void CommentsOverviewTest::testSyncControlsAndFedoraProjectionOverlay()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(registerCatalogueProject(root, {{QStringLiteral("id"), QStringLiteral("new:alpha")}, {QStringLiteral("name"), QStringLiteral("Alpha")},
        {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QStringLiteral("alpha.pdf")}}}}}));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *reviewCardsButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewReviewCards"));
    auto *localChangesButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewLocalChanges"));
    auto *syncButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewSync"));
    auto *heading = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewHeading"));
    auto *headingIcon = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewHeadingIcon"));
    auto *pdfSyncStatus = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewPdfSyncStatus"));
    auto *pdfSyncTimestamp = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewPdfSyncTimestamp"));
    auto *fedoraStorage = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewFedoraStorage"));
    auto *fedoraStorageProgress = overview.findChild<QProgressBar *>(QStringLiteral("thoughtsOverviewFedoraStorageProgress"));
    auto *fedoraDetailsButton = overview.findChild<QPushButton *>(QStringLiteral("thoughtsOverviewFedoraDetailsButton"));
    auto *tabs = overview.findChild<QTabWidget *>(QStringLiteral("thoughtsOverviewTabs"));
    auto *librarySummary = overview.findChild<QLabel *>(QStringLiteral("thoughtsOverviewLibrarySummary"));
    auto *libraryTree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    auto *libraryGroup = overview.findChild<QGroupBox *>(QStringLiteral("thoughtsOverviewLibrarySummaryGroup"));
    auto *syncBuildGroup = overview.findChild<QGroupBox *>(QStringLiteral("thoughtsOverviewSyncBuildSummaryGroup"));
    auto *fedoraStorageGroup = overview.findChild<QGroupBox *>(QStringLiteral("thoughtsOverviewFedoraStorageGroup"));
    auto *syncBuildSummary = overview.findChild<QLabel *>(QStringLiteral("thoughtsOverviewSyncBuildSummary"));
    auto *search = overview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewSearch"));
    QVERIFY(reviewCardsButton);
    QVERIFY(!localChangesButton);
    QVERIFY(syncButton);
    QVERIFY(heading);
    QVERIFY(headingIcon);
    QCOMPARE(heading->text(), QStringLiteral("Thoughts"));
    QVERIFY(headingIcon->width() >= 24);
    QCOMPARE(headingIcon->size(), headingIcon->minimumSize());
    QVERIFY(!headingIcon->pixmap().isNull());
    QVERIFY(!overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewBuildAll")));
    QVERIFY(pdfSyncStatus);
    QVERIFY(pdfSyncTimestamp);
    QVERIFY(fedoraStorage);
    QVERIFY(fedoraStorageProgress);
    QVERIFY(fedoraStorageProgress->isHidden());
    QVERIFY(fedoraDetailsButton);
    QVERIFY(!fedoraDetailsButton->isEnabled());
    QVERIFY(tabs);
    QVERIFY(librarySummary);
    QVERIFY(libraryTree);
    QVERIFY(libraryGroup);
    QVERIFY(syncBuildGroup);
    QVERIFY(fedoraStorageGroup);
    QVERIFY(!overview.findChild<QLabel *>(QStringLiteral("thoughtsOverviewCommentsReviewSummary")));
    QVERIFY(syncBuildSummary);
    QCOMPARE(overview.windowTitle(), QStringLiteral("Thoughts"));
    QCOMPARE(tabs->count(), 5);
    QCOMPARE(tabs->tabText(0), QStringLiteral("Projects"));
    QCOMPARE(tabs->tabText(1), QStringLiteral("To-dos"));
    QCOMPARE(tabs->tabText(2), QStringLiteral("Diary"));
    QCOMPARE(tabs->tabText(3), QStringLiteral("Annotations"));
    QCOMPARE(tabs->tabText(4), QStringLiteral("Review"));
    // Review holds Browse cards and Mental Model, as on the phone.
    auto *reviewPage = overview.findChild<QTabWidget *>(QStringLiteral("thoughtsOverviewReviewPage"));
    QVERIFY(reviewPage);
    QCOMPARE(reviewPage->count(), 2);
    QCOMPARE(reviewPage->tabText(0), QStringLiteral("Browse cards"));
    QCOMPARE(reviewPage->tabText(1), QStringLiteral("Mental Model"));
    // A cited card in the Mental Model opens that card.
    auto *mentalText = reviewPage->widget(1)->findChild<QTextBrowser *>(QStringLiteral("mentalModelText"));
    QVERIFY(mentalText);
    QSignalSpy cardRequested(reviewPage->widget(1), SIGNAL(cardRequested(QString)));
    QVERIFY(QMetaObject::invokeMethod(mentalText, "anchorClicked", Qt::DirectConnection,
                                      Q_ARG(QUrl, QUrl(QStringLiteral("thoughts-card:271b3401-1832-4f8b-826e-ced75bab44b5")))));
    QCOMPARE(cardRequested.size(), 1);
    QCOMPARE(cardRequested.at(0).at(0).toString(), QStringLiteral("271b3401-1832-4f8b-826e-ced75bab44b5"));
    // ... in Browse cards.
    QCOMPARE(tabs->currentWidget(), reviewPage);
    QCOMPARE(reviewPage->currentIndex(), 0);
    tabs->setCurrentIndex(3);
    QCOMPARE(tabs->currentIndex(), 3);
    tabs->setCurrentIndex(0);
    QTRY_VERIFY(libraryTree->isVisibleTo(&overview));
    QVERIFY(syncBuildGroup->isHidden());
    QVERIFY(fedoraStorageGroup->isHidden());
    QTRY_COMPARE(libraryTree->topLevelItemCount(), 1);
    QCOMPARE(libraryTree->topLevelItem(0)->text(0), QStringLiteral("Alpha"));
    tabs->setCurrentIndex(3);
    QVERIFY(search);
    QTRY_VERIFY(syncButton->isVisibleTo(&overview));
    QVERIFY(search->isVisibleTo(&overview));
    // The author and location filters were removed from both browsers.
    for (const QString &name : {QStringLiteral("commentsOverviewAuthorFilter"), QStringLiteral("commentsOverviewLocationFilter"),
                                QStringLiteral("notesOverviewAuthorFilter"), QStringLiteral("notesOverviewLocationFilter")}) {
        QVERIFY2(!overview.findChild<QWidget *>(name), qPrintable(name));
    }
    QVERIFY(syncButton->geometry().left() < reviewCardsButton->geometry().left());
    QCOMPARE(reviewCardsButton->text(), QStringLiteral("Review cards"));
    QVERIFY(syncButton->toolTip().contains(QStringLiteral("only PDFs whose build inputs changed")));

    // Cmd+S does what the button does, and nothing while the button is disabled.
    auto *syncShortcut = overview.findChild<QShortcut *>(QStringLiteral("commentsOverviewSyncShortcut"));
    QVERIFY(syncShortcut);
    QCOMPARE(syncShortcut->key(), QKeySequence(Qt::CTRL | Qt::Key_S));
    {
        const bool wasEnabled = syncButton->isEnabled();
        QSignalSpy viaShortcut(&overview, &Thoughts::CommentsOverview::syncRequested);
        syncButton->setEnabled(true);
        Q_EMIT syncShortcut->activated();
        QCOMPARE(viaShortcut.count(), 1);
        syncButton->setEnabled(false);
        Q_EMIT syncShortcut->activated();
        QCOMPARE(viaShortcut.count(), 1);
        syncButton->setEnabled(wasEnabled);
    }

    QSignalSpy requested(&overview, &Thoughts::CommentsOverview::syncRequested);
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Checking sync status…"));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last sync: Checking…"));
    const QJsonObject currentAggregate {{QStringLiteral("state"), QStringLiteral("current")},
                                        {QStringLiteral("status"), QStringLiteral("Synced")},
                                        {QStringLiteral("reason"), QStringLiteral("0 Synced")},
                                        {QStringLiteral("enabled"), true},
                                        {QStringLiteral("pending_documents"), 0}};
    overview.setSyncSnapshot(QJsonObject(),
                             currentAggregate,
                             QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}},
                             QDateTime());
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("No pending changes"));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last sync: Never"));
    QVERIFY(syncBuildSummary->text().contains(QStringLiteral("No pending changes")));

    const qint64 failedAtMs = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject failedOperation {{QStringLiteral("kind"), QStringLiteral("sync")},
                                       {QStringLiteral("status"), QStringLiteral("failed")},
                                       {QStringLiteral("phase"), QStringLiteral("failed")},
                                       {QStringLiteral("created_at_ms"), failedAtMs},
                                       {QStringLiteral("updated_at_ms"), failedAtMs}};
    QJsonObject failedAggregate = currentAggregate;
    failedAggregate.insert(QStringLiteral("state"), QStringLiteral("error"));
    failedAggregate.insert(QStringLiteral("pending_documents"), 35);
    overview.setSyncSnapshot(QJsonObject(),
                             failedAggregate,
                             QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}},
                             QDateTime(),
                             failedOperation);
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Sync failed"));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last attempt: just now"));
    overview.setSyncSnapshot(QJsonObject(),
                             currentAggregate,
                             QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}},
                             QDateTime());

    overview.setSyncSnapshot(QJsonObject(),
                             currentAggregate,
                             QJsonObject {{QStringLiteral("available"), true},
                                          {QStringLiteral("source_state"), QStringLiteral("unknown")},
                                          {QStringLiteral("comments"), 1},
                                          {QStringLiteral("total"), 1},
                                          {QStringLiteral("documents"),
                                           QJsonObject {{QStringLiteral("alpha.pdf"),
                                                         QJsonObject {{QStringLiteral("comments"), 1},
                                                                      {QStringLiteral("total"), 1}}}}}},
                             QDateTime());
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Changes pending"));
    overview.setSyncSnapshot(QJsonObject(),
                             currentAggregate,
                             QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}},
                             QDateTime());
    tabs->setCurrentIndex(3);
    QSignalSpy reviewOverviewRequested(&overview, &Thoughts::CommentsOverview::reviewOverviewRequested);
    QTest::mouseClick(reviewCardsButton, Qt::LeftButton);
    QCOMPARE(reviewOverviewRequested.size(), 1);
    QCOMPARE(tabs->currentIndex(), 3);
    auto *reviewCardsPage = overview.findChild<QWidget *>(QStringLiteral("thoughtsOverviewReviewCardsPage"));
    QVERIFY(reviewCardsPage);
    QVERIFY(QMetaObject::invokeMethod(reviewCardsPage, "countsChanged", Qt::DirectConnection, Q_ARG(int, 5), Q_ARG(int, 3)));
    QCOMPARE(reviewCardsButton->text(), QStringLiteral("Review cards (3)"));
    QVERIFY(reviewCardsButton->toolTip().contains(QStringLiteral("3 cards are due")));
    QVERIFY(QMetaObject::invokeMethod(reviewCardsPage, "countsChanged", Qt::DirectConnection, Q_ARG(int, 5), Q_ARG(int, 0)));
    QCOMPARE(reviewCardsButton->text(), QStringLiteral("Review cards"));
    QTest::mouseClick(syncButton, Qt::LeftButton);
    QCOMPARE(requested.size(), 1);
    QVERIFY(syncButton->isEnabled());

    tabs->setCurrentIndex(0);
    const QPersistentModelIndex libraryIndex(libraryTree->model()->index(0, 0));
    QSignalSpy libraryRowsRemoved(libraryTree->model(), &QAbstractItemModel::rowsRemoved);
    QSignalSpy libraryRowsInserted(libraryTree->model(), &QAbstractItemModel::rowsInserted);
    overview.syncStarted(QDateTime::currentDateTimeUtc().addMSecs(-3100));
    QVERIFY(!syncButton->isEnabled());
    QCOMPARE(syncButton->text(), QStringLiteral("Syncing…"));
    QVERIFY(!syncButton->toolTip().contains(QStringLiteral("Terminal")));
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Syncing…"));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("3 seconds elapsed"));
    QVERIFY(libraryIndex.isValid());
    QCOMPARE(libraryRowsRemoved.size(), 0);
    QCOMPARE(libraryRowsInserted.size(), 0);
    QTest::qWait(1100);
    QVERIFY(libraryIndex.isValid());
    QCOMPARE(libraryRowsRemoved.size(), 0);
    QCOMPARE(libraryRowsInserted.size(), 0);
    QVERIFY(syncBuildSummary->text().contains(QStringLiteral("seconds elapsed")));
    overview.syncStarted();
    QVERIFY(pdfSyncTimestamp->text().contains(QStringLiteral("seconds elapsed")));
    QVERIFY(syncButton->toolTip().contains(QStringLiteral("Logs")));
    QVERIFY(search->isEnabled());
    overview.syncFinished(true, QString());
    QVERIFY(syncButton->isEnabled());
    QCOMPARE(syncButton->text(), QStringLiteral("Sync"));
    overview.syncFinished(false, QStringLiteral("offline"), QStringLiteral("Host not found"));
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Sync failed"));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last attempt: just now"));
    QCOMPARE(pdfSyncTimestamp->toolTip(), QStringLiteral("offline\nHost not found"));
    QCOMPARE(pdfSyncTimestamp->accessibleDescription(), QStringLiteral("offline\nHost not found"));
    overview.syncFinished(false, QStringLiteral("PDF compilation failed"), QStringLiteral("Undefined control sequence"), QStringLiteral("pdf_build_failed"));
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("PDF build failed"));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last attempt: just now"));
    QVERIFY(pdfSyncTimestamp->toolTip().contains(QStringLiteral("Undefined control sequence")));
    overview.syncStarted();
    QVERIFY(pdfSyncStatus->toolTip().isEmpty());
    QVERIFY(pdfSyncStatus->accessibleDescription().isEmpty());
    overview.syncFinished(true, QString());

    overview.setPdfSyncState(QJsonObject {{QStringLiteral("status"), QStringLiteral("changed")},
                                          {QStringLiteral("refreshed"), true},
                                          {QStringLiteral("checked_at"), QStringLiteral("2026-08-18T08:00:00Z")},
                                          {QStringLiteral("last_successful_build_at"), QStringLiteral("2026-08-17T08:00:00Z")},
                                          {QStringLiteral("storage"),
                                           QJsonObject {{QStringLiteral("total_bytes"), 75484020736.0},
                                                        {QStringLiteral("used_bytes"), 49907572736.0},
                                                        {QStringLiteral("available_bytes"), 18296586240.0}}}});
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Up to date"));
    QVERIFY(!fedoraStorageProgress->isHidden());
    QVERIFY(fedoraStorage->text().contains(QLocale().formattedDataSize(49907572736LL, 1, QLocale::DataSizeIecFormat)));
    QVERIFY(fedoraStorage->text().contains(QLocale().formattedDataSize(18296586240LL, 1, QLocale::DataSizeIecFormat)));
    QVERIFY(fedoraStorage->toolTip().contains(QLocale().toString(75484020736LL)));
    QCOMPARE(fedoraStorageProgress->value(), int((49907572736LL * 1000) / 75484020736LL));
    QVERIFY(fedoraDetailsButton->isEnabled());
    QVERIFY(pdfSyncStatus->toolTip().contains(QStringLiteral("2026-08-18T08:00:00.000Z")));
    QVERIFY(pdfSyncStatus->toolTip().contains(QStringLiteral("2026-08-17T08:00:00.000Z")));
    const QString recentBuild = QDateTime::currentDateTimeUtc().addSecs(-2 * 60 * 60).toString(Qt::ISODateWithMs);
    overview.setPdfSyncState(QJsonObject {{QStringLiteral("status"), QStringLiteral("changed")},
                                          {QStringLiteral("last_successful_build_at"), recentBuild}});
    overview.setPdfSyncState(QJsonObject {{QStringLiteral("status"), QStringLiteral("changed")},
                                          {QStringLiteral("last_successful_build_at"), QStringLiteral("2026-08-17T08:00:00Z")}});
    overview.setPdfSyncState(QJsonObject {{QStringLiteral("status"), QStringLiteral("changed")},
                                          {QStringLiteral("refreshed"), false},
                                          {QStringLiteral("refresh_error"), QStringLiteral("offline")}});
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Up to date"));
    QVERIFY(pdfSyncStatus->toolTip().contains(QStringLiteral("offline")));

    QTRY_COMPARE(overview.documentCount(), 1);
    QTRY_VERIFY(librarySummary->text().contains(QStringLiteral("1 project")));
    QTRY_VERIFY(!overview.m_syncRefreshPending && !overview.m_syncLibraryTree);
    QCOMPARE(librarySummary->text(), QStringLiteral("1 project"));
    const QJsonObject buildPresentations {{QStringLiteral("alpha.pdf"),
                                           QJsonObject {{QStringLiteral("state"), QStringLiteral("build_needed")},
                                                        {QStringLiteral("status"), QStringLiteral("Build needed")},
                                                        {QStringLiteral("reason"), QStringLiteral("local source edit")},
                                                        {QStringLiteral("requires_build"), true}}}};
    const QJsonObject buildAggregate {{QStringLiteral("state"), QStringLiteral("build_needed")},
                                      {QStringLiteral("status"), QStringLiteral("Build needed")},
                                      {QStringLiteral("reason"), QStringLiteral("1 Build needed")},
                                      {QStringLiteral("action_label"), QStringLiteral("Build")},
                                      {QStringLiteral("enabled"), true},
                                      {QStringLiteral("pending_documents"), 1}};
    overview.setSyncSnapshot(buildPresentations,
                             buildAggregate,
                             QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}},
                             QDateTime());
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Changes pending"));
    QVERIFY(pdfSyncStatus->toolTip().contains(QStringLiteral("1 Build needed")));
    QCOMPARE(syncButton->text(), QStringLiteral("Sync"));
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(documents);
    QVERIFY(documentItem(documents, QStringLiteral("alpha"))->toolTip(0).contains(QStringLiteral("Build needed")));
    const QDateTime lastSync = QDateTime::fromString(QStringLiteral("2026-08-28T20:20:58Z"), Qt::ISODate);
    QJsonObject pendingAggregate = currentAggregate;
    pendingAggregate.insert(QStringLiteral("pending_documents"), 1);
    overview.setSyncSnapshot(QJsonObject(),
                             pendingAggregate,
                             QJsonObject {{QStringLiteral("available"), true},
                                          {QStringLiteral("comments"), 1},
                                          {QStringLiteral("review_events"), 1},
                                          {QStringLiteral("other"), 1},
                                          {QStringLiteral("library_changes"), 1},
                                          {QStringLiteral("files"), 2},
                                          {QStringLiteral("total"), 5},
                                          {QStringLiteral("documents"),
                                           QJsonObject {{QStringLiteral("alpha.pdf"),
                                                         QJsonObject {{QStringLiteral("comments"), 1},
                                                                      {QStringLiteral("review_events"), 1},
                                                                      {QStringLiteral("other"), 1},
                                                                      {QStringLiteral("library_changes"), 1},
                                                                      {QStringLiteral("library_operations"),
                                                                       QJsonArray {QJsonObject {{QStringLiteral("operation_id"), QStringLiteral("rename-alpha")},
                                                                                                {QStringLiteral("generation"), 7},
                                                                                                {QStringLiteral("type"), QStringLiteral("rename")},
                                                                                                {QStringLiteral("filename"), QStringLiteral("alpha.pdf")},
                                                                                                {QStringLiteral("state"), QStringLiteral("pending")},
                                                                                                {QStringLiteral("mutation"),
                                                                                                 QJsonObject {{QStringLiteral("display_name"), QStringLiteral("Alpha paper")}}}}}},
                                                                      {QStringLiteral("total"), 3}}}}},
                                          {QStringLiteral("file_groups"),
                                           QJsonObject {{QStringLiteral("alpha.pdf"),
                                                         QJsonArray {QJsonObject {{QStringLiteral("path"), QStringLiteral("docs-alpha/plot.svg")},
                                                                                  {QStringLiteral("role"), QStringLiteral("asset")},
                                                                                  {QStringLiteral("action"), QStringLiteral("upsert")}}}},
                                                        {QStringLiteral("Shared sources"),
                                                         QJsonArray {QJsonObject {{QStringLiteral("path"), QStringLiteral("docs-shared/new-style.sty")},
                                                                                  {QStringLiteral("action"), QStringLiteral("upsert")}}}}}}},
                             lastSync);
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Changes pending"));
    QVERIFY(pdfSyncStatus->toolTip().contains(QStringLiteral("1 pending comment change")));
    QVERIFY(pdfSyncStatus->toolTip().contains(QStringLiteral("1 pending review event")));
    QVERIFY(pdfSyncStatus->toolTip().contains(QStringLiteral("1 pending Library change")));
    QVERIFY(!pdfSyncStatus->toolTip().contains(QStringLiteral("other pending change")));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last sync: ") + Thoughts::compactSyncActivityAge(lastSync.secsTo(QDateTime::currentDateTimeUtc())));
    overview.setSyncSnapshot(QJsonObject(),
                             currentAggregate,
                             QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}},
                             lastSync);
    overview.setPdfSyncState(QJsonObject {{QStringLiteral("status"), QStringLiteral("current")}});
    QCOMPARE(pdfSyncStatus->text(), QStringLiteral("Up to date"));
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last sync: ") + Thoughts::compactSyncActivityAge(lastSync.secsTo(QDateTime::currentDateTimeUtc())));
    overview.syncStarted();
    overview.syncFinished(true, QString());
    QCOMPARE(pdfSyncTimestamp->text(), QStringLiteral("Last sync: just now"));

    QJsonObject remote = comment(QStringLiteral("remote-comment"), QStringLiteral("Remote todo"), 1, QStringLiteral("2026-08-16T08:00:00Z"), QStringLiteral("anchored"));
    remote.insert(QStringLiteral("annotation_id"), QStringLiteral("remote-comment"));
    remote.insert(QStringLiteral("discussion"), true);
    const QString key = QStringLiteral("annotation\nalpha\nremote-comment");
    overview.setFedoraProjections(QJsonObject {{key, remote}});
    overview.reload();
    QTRY_COMPARE(overview.commentCount(), 1);

    remote.insert(QStringLiteral("deleted"), true);
    overview.setFedoraProjections(QJsonObject {{key, remote}});
    overview.reload();
    QTRY_COMPARE(overview.commentCount(), 0);
}

void CommentsOverviewTest::testPendingLibraryRemovalProjectsImmediately()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QByteArray pdfContents("%PDF-1.7\nlocal removal projection\n");
    const QString sha256 = QString::fromLatin1(QCryptographicHash::hash(pdfContents, QCryptographicHash::Sha256).toHex());
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/paper.pdf"), pdfContents));
    const QJsonObject index {{QStringLiteral("schema_version"), 1},
                             {QStringLiteral("pdfs"),
                              QJsonArray {QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                                       {QStringLiteral("pdf_sha256"), sha256},
                                                       {QStringLiteral("kind"), QStringLiteral("unlinked")},
                                                       {QStringLiteral("archived"), false}}}}};
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), QJsonDocument(index).toJson()));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *librarySummary = overview.findChild<QLabel *>(QStringLiteral("thoughtsOverviewLibrarySummary"));
    auto *libraryTree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    auto *localChangesButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewLocalChanges"));
    QVERIFY(librarySummary);
    QVERIFY(libraryTree);
    QVERIFY(documents);
    QVERIFY(!localChangesButton);
    QTRY_COMPARE(overview.documentCount(), 1);
    QCOMPARE(overview.documentSyncDescriptors().size(), 1);

    const QJsonObject aggregate {{QStringLiteral("state"), QStringLiteral("current")},
                                 {QStringLiteral("enabled"), true},
                                 {QStringLiteral("pending_documents"), 0}};
    const auto removalSnapshot = [&sha256](const QString &state) {
        const QJsonObject mutation {{QStringLiteral("type"), QStringLiteral("remove")},
                                    {QStringLiteral("request_id"), QStringLiteral("remove-paper")},
                                    {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                    {QStringLiteral("sha256"), sha256}};
        const QJsonObject operation {{QStringLiteral("operation_id"), QStringLiteral("remove-paper")},
                                     {QStringLiteral("generation"), 1},
                                     {QStringLiteral("type"), QStringLiteral("remove")},
                                     {QStringLiteral("filename"), QStringLiteral("paper.pdf")},
                                     {QStringLiteral("state"), state},
                                     {QStringLiteral("mutation"), mutation}};
        return QJsonObject {{QStringLiteral("available"), true},
                            {QStringLiteral("other"), 1},
                            {QStringLiteral("library_changes"), 1},
                            {QStringLiteral("total"), 1},
                            {QStringLiteral("library_operations"), QJsonArray {operation}},
                            {QStringLiteral("documents"),
                             QJsonObject {{QStringLiteral("paper.pdf"),
                                           QJsonObject {{QStringLiteral("other"), 1},
                                                        {QStringLiteral("library_changes"), 1},
                                                        {QStringLiteral("total"), 1},
                                                        {QStringLiteral("library_operations"), QJsonArray {operation}}}}}}};
    };

    for (const QString &state : {QStringLiteral("pending"), QStringLiteral("in_flight"), QStringLiteral("pending_activation")}) {
        overview.setSyncSnapshot({}, aggregate, removalSnapshot(state), {});
        QCOMPARE(overview.documentCount(), 1);
        QCOMPARE(documentRow(documents, QStringLiteral("paper")) >= 0, true);
        QVERIFY(overview.documentSyncDescriptors().isEmpty());
    }

    overview.setSyncSnapshot({},
                             aggregate,
                             QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}},
                             {});
    QCOMPARE(overview.documentSyncDescriptors().size(), 1);
}

void CommentsOverviewTest::testFedoraDetailsDialog()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());

    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    auto *detailsButton = overview.findChild<QPushButton *>(QStringLiteral("thoughtsOverviewFedoraDetailsButton"));
    QVERIFY(detailsButton);
    QVERIFY(!detailsButton->isEnabled());

    overview.setPdfSyncState(
        QJsonObject {{QStringLiteral("storage"),
                      QJsonObject {{QStringLiteral("total_bytes"), 75484020736.0},
                                   {QStringLiteral("used_bytes"), 49907572736.0},
                                   {QStringLiteral("available_bytes"), 18296586240.0}}},
                     {QStringLiteral("server_summary"),
                      QJsonObject {{QStringLiteral("status"), QStringLiteral("attention")},
                                   {QStringLiteral("operating_system"), QStringLiteral("Fedora Linux 42")},
                                   {QStringLiteral("architecture"), QStringLiteral("aarch64")},
                                   {QStringLiteral("uptime_seconds"), 950400},
                                   {QStringLiteral("cpu_count"), 10},
                                   {QStringLiteral("memory_total_bytes"), 17179869184.0},
                                   {QStringLiteral("memory_available_bytes"), 6442450944.0},
                                   {QStringLiteral("services_available"), true},
                                   {QStringLiteral("services_running"), 5},
                                   {QStringLiteral("services_total"), 5},
                                   {QStringLiteral("thoughts_memory_current_bytes"), 2253045760.0},
                                   {QStringLiteral("worker_peak_memory_bytes"), 4917133312.0},
                                   {QStringLiteral("builds_queued"), 0},
                                   {QStringLiteral("builds_running"), 0},
                                   {QStringLiteral("latest_build_status"), QStringLiteral("succeeded")},
                                   {QStringLiteral("latest_build_at"), QStringLiteral("2026-09-04T10:00:00Z")},
                                   {QStringLiteral("backup"),
                                    QJsonObject {{QStringLiteral("status"), QStringLiteral("failed")},
                                                 {QStringLiteral("last_run_at"), QStringLiteral("2026-09-04T00:07:09Z")}}},
                                   {QStringLiteral("integrity_check"),
                                    QJsonObject {{QStringLiteral("status"), QStringLiteral("successful")},
                                                 {QStringLiteral("last_run_at"), QStringLiteral("2026-08-31T01:32:09Z")}}}}}});

    QVERIFY(detailsButton->isEnabled());
    QTest::mouseClick(detailsButton, Qt::LeftButton);
    auto *dialog = overview.findChild<QDialog *>(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"));
    QVERIFY(dialog);
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewFedoraDetailsStatus"))->text(), QStringLiteral("The server needs attention."));
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewFedoraDetailsSystem"))->text().contains(QStringLiteral("Fedora Linux 42")));
    QCOMPARE(dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewFedoraDetailsServices"))->text(), QStringLiteral("5 of 5 running"));
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewFedoraDetailsThoughtsMemory"))->text().contains(QLocale().formattedDataSize(4917133312LL, 1, QLocale::DataSizeIecFormat)));
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewFedoraDetailsBuilds"))->text().contains(QStringLiteral("last build successful")));
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewFedoraDetailsBackup"))->text().startsWith(QStringLiteral("Failed")));
    QVERIFY(dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewFedoraDetailsIntegrity"))->text().startsWith(QStringLiteral("Successful")));
    dialog->close();
}

void CommentsOverviewTest::testServerDialogSavesAddress()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString store = Thoughts::ServerAddress::storePath();
    QFile::remove(store);
    const QByteArray previousServer = qgetenv("THOUGHTS_SERVER_URL");
    const auto restore = qScopeGuard([previousServer, store] {
        qputenv("THOUGHTS_SERVER_URL", previousServer);
        QFile::remove(store);
    });
    // An installation that synchronized with an earlier, fixed address: its
    // first run saved that address, which the dialog shows until the owner switches.
    const QString previous = QStringLiteral("https://server.example.ts.net");
    QVERIFY(Thoughts::ServerAddress::save(previous));
    qunsetenv("THOUGHTS_SERVER_URL");

    Thoughts::CommentsOverview overview(temporary.path());
    overview.showServerDetails();
    auto *dialog = overview.findChild<QDialog *>(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"));
    QVERIFY(dialog);
    auto *address = dialog->findChild<QLineEdit *>(QStringLiteral("thoughtsOverviewServerAddress"));
    auto *save = dialog->findChild<QPushButton *>(QStringLiteral("thoughtsOverviewServerAddressSave"));
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewServerAddressStatus"));
    QVERIFY(address && save && status);
    QCOMPARE(address->text(), previous);
    QVERIFY(!address->isReadOnly());
    QVERIFY(!save->isEnabled());

    address->clear();
    QTest::keyClicks(address, QStringLiteral("http://thoughts.example.org"));
    QVERIFY(save->isEnabled());
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(status->text(), QStringLiteral("The server address must start with https://."));
    QCOMPARE(Thoughts::ServerAddress::saved(), previous);

    address->clear();
    QTest::keyClicks(address, QStringLiteral("https://Thoughts.example.org/"));
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(Thoughts::ServerAddress::saved(), QStringLiteral("https://thoughts.example.org"));
    QCOMPARE(address->text(), QStringLiteral("https://thoughts.example.org"));
    QVERIFY(!save->isEnabled());
    QCOMPARE(status->text(), QStringLiteral("Saved. Quit and reopen Thoughts to connect to https://thoughts.example.org."));
    dialog->close();
    delete dialog;

    // THOUGHTS_SERVER_URL wins and the saved address cannot be edited.
    qputenv("THOUGHTS_SERVER_URL", "https://server.example.ts.net:8443");
    overview.showServerDetails();
    dialog = overview.findChild<QDialog *>(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"));
    QVERIFY(dialog);
    address = dialog->findChild<QLineEdit *>(QStringLiteral("thoughtsOverviewServerAddress"));
    QCOMPARE(address->text(), QStringLiteral("https://server.example.ts.net:8443"));
    QVERIFY(address->isReadOnly());
    QVERIFY(!dialog->findChild<QPushButton *>(QStringLiteral("thoughtsOverviewServerAddressSave"))->isEnabled());
    dialog->close();
    delete dialog;
}

void CommentsOverviewTest::testServerDialogAsksForTheTokenOfAnotherServer()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString store = Thoughts::ServerAddress::storePath();
    QFile::remove(store);
    const bool hadServer = qEnvironmentVariableIsSet("THOUGHTS_SERVER_URL");
    const QByteArray previousServer = qgetenv("THOUGHTS_SERVER_URL");
    const auto restore = qScopeGuard([hadServer, previousServer, store] {
        if (hadServer) {
            qputenv("THOUGHTS_SERVER_URL", previousServer);
        } else {
            qunsetenv("THOUGHTS_SERVER_URL");
        }
        QFile::remove(store);
    });
    qunsetenv("THOUGHTS_SERVER_URL");
    const QString first = QStringLiteral("https://first.example.org");
    const QString second = QStringLiteral("https://second.example.org");
    const QString account = Thoughts::Platform::desktopDeviceAccount();
    // This computer's token was issued by the first server.
    QVERIFY(Thoughts::ServerAddress::save(first));
    QVERIFY(Thoughts::ServerAddress::bindToken(account, "token-one", first));

    Thoughts::CommentsOverview overview(temporary.path());
    QString storedAccount;
    QByteArray storedToken;
    bool storeSucceeds = false;
    overview.setServerTokenWriter([&](const QString &tokenAccount, const QByteArray &token, QString *error) {
        if (!storeSucceeds) {
            *error = QStringLiteral("the secret store is locked");
            return false;
        }
        storedAccount = tokenAccount;
        storedToken = token;
        return true;
    });
    overview.showServerDetails();
    auto *dialog = overview.findChild<QDialog *>(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"));
    QVERIFY(dialog);
    auto *address = dialog->findChild<QLineEdit *>(QStringLiteral("thoughtsOverviewServerAddress"));
    auto *save = dialog->findChild<QPushButton *>(QStringLiteral("thoughtsOverviewServerAddressSave"));
    auto *status = dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewServerAddressStatus"));
    auto *tokenStatus = dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewServerTokenStatus"));
    auto *tokenRow = dialog->findChild<QWidget *>(QStringLiteral("thoughtsOverviewServerTokenRow"));
    auto *token = dialog->findChild<QLineEdit *>(QStringLiteral("thoughtsOverviewServerToken"));
    auto *storeToken = dialog->findChild<QPushButton *>(QStringLiteral("thoughtsOverviewServerTokenStore"));
    QVERIFY(address && save && status && tokenStatus && tokenRow && token && storeToken);
    // The token belongs to the server in use: nothing is asked.
    QVERIFY(tokenStatus->isHidden());
    QVERIFY(tokenRow->isHidden());
    QCOMPARE(token->echoMode(), QLineEdit::Password);

    // Another path on the same server keeps the token.
    address->clear();
    QTest::keyClicks(address, first + QStringLiteral("/prefix"));
    QTest::mouseClick(save, Qt::LeftButton);
    QVERIFY(tokenStatus->isHidden());
    QVERIFY(tokenRow->isHidden());

    // Another server: its token is asked for, and the old one is not sent there.
    address->clear();
    QTest::keyClicks(address, second);
    QTest::mouseClick(save, Qt::LeftButton);
    QCOMPARE(status->text(), QStringLiteral("Saved. Quit and reopen Thoughts to connect to https://second.example.org."));
    QVERIFY(!tokenStatus->isHidden());
    QVERIFY(!tokenRow->isHidden());
    QCOMPARE(tokenStatus->text(), QStringLiteral("This computer's device token was issued for https://first.example.org and is not sent to https://second.example.org. "
                                                 "Enter the device token of https://second.example.org:"));
    QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-one", second));
    QVERIFY(!storeToken->isEnabled());

    QTest::keyClicks(token, QStringLiteral(" token-two "));
    QVERIFY(storeToken->isEnabled());
    QTest::mouseClick(storeToken, Qt::LeftButton);
    QCOMPARE(tokenStatus->text(), QStringLiteral("The token could not be stored: the secret store is locked"));
    QVERIFY(!tokenRow->isHidden());
    // A token that was not stored is not recorded either.
    QCOMPARE(Thoughts::ServerAddress::tokenOrigin(account), first);

    storeSucceeds = true;
    QTest::mouseClick(storeToken, Qt::LeftButton);
    QCOMPARE(storedAccount, account);
    QCOMPARE(storedToken, QByteArray("token-two"));
    QVERIFY(tokenRow->isHidden());
    QCOMPARE(tokenStatus->text(), QStringLiteral("Stored the device token of https://second.example.org. Quit and reopen Thoughts to connect."));
    QVERIFY(token->text().isEmpty());
    QVERIFY(Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-two", second));
    QVERIFY(!Thoughts::ServerAddress::tokenMayBeSentTo(account, "token-two", first));
    dialog->close();
    delete dialog;

    // THOUGHTS_SERVER_URL naming another server: the stored token stays here and no token is stored for it.
    qputenv("THOUGHTS_SERVER_URL", "https://third.example.org");
    overview.showServerDetails();
    dialog = overview.findChild<QDialog *>(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"));
    QVERIFY(dialog);
    tokenStatus = dialog->findChild<QLabel *>(QStringLiteral("thoughtsOverviewServerTokenStatus"));
    QVERIFY(!tokenStatus->isHidden());
    QCOMPARE(tokenStatus->text(), QStringLiteral("This computer's device token was issued for https://second.example.org and is not sent to https://third.example.org. "
                                                 "Give the token for this session in THOUGHTS_TOKEN."));
    QVERIFY(dialog->findChild<QWidget *>(QStringLiteral("thoughtsOverviewServerTokenRow"))->isHidden());
    dialog->close();
    delete dialog;
}

void CommentsOverviewTest::testDocumentSyncPresentationAggregation()
{
    Thoughts::DocumentSyncState current;
    current.localFreshnessAvailable = true;
    current.localSourcesCurrent = true;
    current.renderStatus = {{QStringLiteral("status"), QStringLiteral("synced")}, {QStringLiteral("local_state"), QStringLiteral("current")}};
    const Thoughts::DocumentSyncPresentation synced = Thoughts::documentSyncPresentation(current);
    QCOMPARE(synced.status, QStringLiteral("Synced"));
    QCOMPARE(synced.state, Thoughts::DocumentSyncStatus::Current);
    QCOMPARE(synced.toJson().value(QStringLiteral("state")).toString(), QStringLiteral("current"));
    QVERIFY(synced.action == Thoughts::DocumentSyncAction::None);

    Thoughts::DocumentSyncState untracked = current;
    untracked.sourceLinked = true;
    untracked.deferLocalFreshness = true;
    untracked.pendingDocumentSync = {{QStringLiteral("source_coverage_available"), false}};
    const auto untrackedPresentation = Thoughts::documentSyncPresentation(untracked);
    QCOMPARE(untrackedPresentation.state, Thoughts::DocumentSyncStatus::SyncNeeded);
    QVERIFY(untrackedPresentation.requiresSync());

    Thoughts::DocumentSyncState pending = current;
    pending.pendingDocumentSync = {{QStringLiteral("comments"), 1}, {QStringLiteral("total"), 1}};
    const Thoughts::DocumentSyncPresentation syncNeeded = Thoughts::documentSyncPresentation(pending);
    QCOMPARE(syncNeeded.status, QStringLiteral("Sync needed"));
    QVERIFY(syncNeeded.reason.startsWith(QStringLiteral("Waiting to sync:")));
    QVERIFY(!syncNeeded.reason.contains(QStringLiteral("Queued"), Qt::CaseInsensitive));
    QCOMPARE(syncNeeded.actionLabel, QStringLiteral("Sync"));
    QVERIFY(syncNeeded.requiresSync());
    QVERIFY(!syncNeeded.requiresBuild());

    Thoughts::DocumentSyncState changed = current;
    changed.localSourcesCurrent = false;
    changed.localFreshnessMessage = QStringLiteral("local source edit");
    const Thoughts::DocumentSyncPresentation sourceSync = Thoughts::documentSyncPresentation(changed);
    QCOMPARE(sourceSync.status, QStringLiteral("Sync needed"));
    QCOMPARE(sourceSync.actionLabel, QStringLiteral("Sync"));
    QVERIFY(sourceSync.requiresSync());
    QVERIFY(!sourceSync.requiresBuild());

    Thoughts::DocumentSyncState stale = current;
    stale.renderStatus = {{QStringLiteral("status"), QStringLiteral("out_of_sync")}};
    const Thoughts::DocumentSyncPresentation buildAndSync = Thoughts::documentSyncPresentation(stale);
    QCOMPARE(buildAndSync.status, QStringLiteral("Build needed"));
    QCOMPARE(buildAndSync.actionLabel, QStringLiteral("Build"));
    QVERIFY(!buildAndSync.requiresSync());
    QVERIFY(buildAndSync.requiresBuild());

    Thoughts::DocumentSyncState staleLibrary = current;
    staleLibrary.renderStatus.clear();
    staleLibrary.libraryPdfStale = true;
    const Thoughts::DocumentSyncPresentation staleLibraryBuild = Thoughts::documentSyncPresentation(staleLibrary);
    QCOMPARE(staleLibraryBuild.status, QStringLiteral("Build needed"));
    QCOMPARE(staleLibraryBuild.actionLabel, QStringLiteral("Build"));

    staleLibrary.latestBuildMessage = QStringLiteral("LaTeX failed");
    const Thoughts::DocumentSyncPresentation staleLibraryFailure = Thoughts::documentSyncPresentation(staleLibrary);
    QCOMPARE(staleLibraryFailure.status, QStringLiteral("Error"));
    QCOMPARE(staleLibraryFailure.actionLabel, QStringLiteral("Build"));

    Thoughts::DocumentSyncState unlinked = pending;
    unlinked.sourceLinked = false;
    unlinked.localFreshnessAvailable = false;
    const Thoughts::DocumentSyncPresentation unlinkedPending = Thoughts::documentSyncPresentation(unlinked);
    QCOMPARE(unlinkedPending.status, QStringLiteral("Sync needed"));
    QVERIFY(unlinkedPending.reason.startsWith(QStringLiteral("Waiting to sync:")));
    QVERIFY(!unlinkedPending.requiresBuild());

    const Thoughts::AggregateSyncPresentation aggregate = Thoughts::aggregateSyncPresentation({synced, syncNeeded, buildAndSync, unlinkedPending});
    QCOMPARE(aggregate.status, QStringLiteral("Sync needed"));
    QCOMPARE(aggregate.actionLabel, QStringLiteral("Sync"));
    QVERIFY(aggregate.requiresSync);
    QVERIFY(!aggregate.requiresBuild);
    QCOMPARE(aggregate.pendingDocuments, 3);
    QCOMPARE(aggregate.toJson().value(QStringLiteral("state")).toString(), QStringLiteral("sync_needed"));
    QCOMPARE(aggregate.reason, QStringLiteral("1 Synced · 2 Sync needed · 1 Build needed"));

    QList<Thoughts::DocumentSyncPresentation> allCurrent;
    for (int index = 0; index < 19; ++index) {
        allCurrent.append(synced);
    }
    const Thoughts::AggregateSyncPresentation currentAggregate = Thoughts::aggregateSyncPresentation(allCurrent);
    QCOMPARE(currentAggregate.reason, QStringLiteral("19 Synced"));

    const Thoughts::AggregateSyncPresentation buildAggregate = Thoughts::aggregateSyncPresentation({buildAndSync, buildAndSync});
    QCOMPARE(buildAggregate.reason, QStringLiteral("2 Build needed"));
    QCOMPARE(buildAggregate.actionLabel, QStringLiteral("Build 2 PDFs"));
    QCOMPARE(buildAggregate.pendingDocuments, 2);

    Thoughts::DocumentSyncState deferred = current;
    deferred.localFreshnessAvailable = false;
    deferred.localSourcesCurrent = false;
    deferred.deferLocalFreshness = true;
    deferred.renderStatus = {{QStringLiteral("status"), QStringLiteral("out_of_sync")}};
    const Thoughts::DocumentSyncPresentation deferredBuild = Thoughts::documentSyncPresentation(deferred);
    QCOMPARE(deferredBuild.actionLabel, QStringLiteral("Build"));
    QVERIFY(deferredBuild.requiresBuild());

    Thoughts::DocumentSyncState waiting = current;
    waiting.renderStatus = {{QStringLiteral("status"), QStringLiteral("publishing")},
                            {QStringLiteral("queued"), true},
                            {QStringLiteral("reason"), QStringLiteral("Waiting for the active source publication.")}};
    const Thoughts::DocumentSyncPresentation waitingBuild = Thoughts::documentSyncPresentation(waiting);
    QCOMPARE(waitingBuild.status, QStringLiteral("Waiting to publish sources…"));
    QCOMPARE(waitingBuild.actionLabel, QStringLiteral("Waiting to publish sources…"));
    QVERIFY(!waitingBuild.enabled);
    QVERIFY(!waitingBuild.requiresBuild());

    const Thoughts::AggregateSyncPresentation waitingAggregate = Thoughts::aggregateSyncPresentation({waitingBuild});
    QCOMPARE(waitingAggregate.status, QStringLiteral("Waiting to publish sources…"));
    QCOMPARE(waitingAggregate.reason, QStringLiteral("1 Waiting to publish sources…"));

    Thoughts::DocumentSyncState cachedBuild = current;
    cachedBuild.renderStatus = {{QStringLiteral("status"), QStringLiteral("building")},
                                {QStringLiteral("local_state"), QStringLiteral("current")},
                                {QStringLiteral("restored_from_cache"), true}};
    const Thoughts::DocumentSyncPresentation cachedBuildPresentation = Thoughts::documentSyncPresentation(cachedBuild);
    QCOMPARE(cachedBuildPresentation.status, QStringLiteral("Synced"));
    QVERIFY(cachedBuildPresentation.reason.contains(QStringLiteral("last synchronized publication")));
    QVERIFY(!cachedBuildPresentation.requiresSync());

    cachedBuild.renderStatus.remove(QStringLiteral("restored_from_cache"));
    const Thoughts::DocumentSyncPresentation liveBuildPresentation = Thoughts::documentSyncPresentation(cachedBuild);
    // Since 14 November 2026 Sync ends without waiting for the server's build: a PDF the
    // server is building, with no Sync or build of this computer waiting for it, says so.
    QCOMPARE(liveBuildPresentation.status, QStringLiteral("PDF updating on the server"));
    QVERIFY(!liveBuildPresentation.requiresSync());

    const Thoughts::AggregateSyncPresentation empty = Thoughts::aggregateSyncPresentation({});
    QCOMPARE(empty.actionLabel, QStringLiteral("Sync"));
    QVERIFY(empty.enabled);
}

void CommentsOverviewTest::testCommentsOnPassagesAreListed()
{
    // Like, Dislike, Question and Comments in the Library, read-only, with the snapshot each was made on.
    QTemporaryDir temporary;
    const QString root = temporary.path();
    const QString pdfPath = root + QStringLiteral("/current_pdfs/alpha.pdf");
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"),
                      jsonLine(comment(QStringLiteral("plain"), QStringLiteral("A plain comment"), 1, QStringLiteral("2026-09-21T08:00:00Z"), QString()))));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *filter = overview.findChild<QComboBox *>(QStringLiteral("commentsOverviewTypeFilter"));
    auto *earlier = overview.findChild<QCheckBox *>(QStringLiteral("commentsOverviewShowEarlier"));
    auto *resolve = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewResolve"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    QVERIFY(table && filter && earlier && resolve && editor);
    QTRY_COMPARE(overview.commentCount(), 1);
    QVERIFY(!overview.findChild<QComboBox *>(QStringLiteral("commentsOverviewPriorityFilter")));
    QStringList types;
    for (int index = 0; index < filter->count(); ++index) types.append(filter->itemText(index));
    QCOMPARE(types, (QStringList{QStringLiteral("All types"), QStringLiteral("Like"), QStringLiteral("Dislike"), QStringLiteral("Question"), QStringLiteral("Comments"), QStringLiteral("Drawings"), QStringLiteral("Text boxes"), QStringLiteral("Agent proposals")}));

    const QString document = QStringLiteral("c1e62ecc-2dfb-4259-97cc-e319b1186848");
    const QString current = QString(64, QLatin1Char('a'));
    const QString older = QString(64, QLatin1Char('e'));
    const auto record = [&document](const QString &polarity, const QString &text, const QString &sha, const QString &kind = QString()) {
        const QJsonArray regions{QJsonObject{{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QJsonArray{0.1, 0.1, 0.9, 0.2}}}};
        return Thoughts::DocumentFeedback::withKind(Thoughts::DocumentFeedback::create(polarity, text, Thoughts::ReviewCardFeedback::anchor(QStringLiteral("the passage"), QString(), QString(), document, sha, regions), QStringLiteral("mac")), kind);
    };
    const QJsonObject liked = record(QStringLiteral("positive"), QString(), current);
    const QJsonObject stale = record(QStringLiteral("negative"), QStringLiteral("Too long"), older);
    const QJsonObject asked = record(QStringLiteral("positive"), QStringLiteral("Why this bound?"), older, Thoughts::DocumentFeedback::question());
    const QJsonObject other = Thoughts::DocumentFeedback::resolve(record(QStringLiteral("positive"), QStringLiteral("Fix it."), current, Thoughts::DocumentFeedback::other()), true, QStringLiteral("mac"));
    const QJsonObject drawn = Thoughts::DocumentFeedback::createDrawing(3, {{{QPointF(0.2, 0.2), QPointF(0.4, 0.3)}}}, document, current, QStringLiteral("mac"));
    const QJsonObject typed = Thoughts::DocumentFeedback::createTextBox(3, QRectF(0.2, 0.4, 0.3, 0.03), 0.015, QStringLiteral("Why not the median?"), document, current, QStringLiteral("mac"));
    QJsonObject projections;
    for (const QJsonObject &value : {liked, stale, asked, other, drawn, typed}) {
        QCOMPARE(Thoughts::DocumentFeedback::validationError(value), QString());
        projections.insert(Thoughts::DocumentFeedback::projectionKey(value.value(QStringLiteral("feedback_id")).toString()), value);
    }
    overview.m_documents[0].libraryDocumentId = document;
    overview.m_documents[0].librarySha256 = current;
    overview.m_documents[0].state = Thoughts::TrackedCommentDocument::SourceLinked;
    overview.m_fedoraProjections = projections;
    overview.populateDocumentList(QString());
    overview.populateComments();
    overview.applyFilters();

    auto *summary = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewSummary"));
    QVERIFY(summary);
    const auto rowOf = [table](const QString &text) {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            if (table->model()->index(row, 0).data().toString() == text) return table->model()->index(row, 0);
        }
        return QModelIndex();
    };
    const auto cell = [&rowOf](const QString &text, int column) { return rowOf(text).siblingAtColumn(column).data().toString(); };
    const auto chooseType = [filter](const QString &type) { filter->setCurrentIndex(filter->findData(type)); };

    // All types: the plain comment, the like, the drawing and the text box; the rest needs Earlier
    // versions, also the open question asked on another version (questions belong to their version).
    QTRY_COMPARE(table->model()->rowCount(), 4);
    // A text box is its text.
    QCOMPARE(cell(QStringLiteral("Why not the median?"), 5), QStringLiteral("Text box"));
    QCOMPARE(cell(QStringLiteral("Why not the median?"), 3), QStringLiteral("Current"));
    QCOMPARE(cell(QStringLiteral("Why not the median?"), 2), QStringLiteral("3"));
    QVERIFY(!rowOf(QStringLiteral("Why not the median?")).data(Qt::DecorationRole).value<QIcon>().isNull());
    QCOMPARE(cell(QStringLiteral("Drawing"), 5), QStringLiteral("Drawing"));
    QCOMPARE(cell(QStringLiteral("Drawing"), 3), QStringLiteral("Current"));
    QCOMPARE(cell(QStringLiteral("Drawing"), 2), QStringLiteral("3"));
    QCOMPARE(cell(QStringLiteral("Drawing"), 4), QStringLiteral("—"));
    QVERIFY(!rowOf(QStringLiteral("Drawing")).data(Qt::DecorationRole).value<QIcon>().isNull());
    QCOMPARE(cell(QStringLiteral("the passage"), 5), QStringLiteral("Like"));
    QCOMPARE(cell(QStringLiteral("the passage"), 3), QStringLiteral("Current"));
    QCOMPARE(cell(QStringLiteral("the passage"), 2), QStringLiteral("2"));
    QVERIFY(!rowOf(QStringLiteral("Why this bound?")).isValid());
    // Made on the PDF, not on a line of the source; the type shows on the row as in the sidebar.
    QCOMPARE(cell(QStringLiteral("the passage"), 4), QStringLiteral("—"));
    QVERIFY(!rowOf(QStringLiteral("the passage")).data(Qt::DecorationRole).value<QIcon>().isNull());
    QVERIFY(!rowOf(QStringLiteral("A plain comment")).data(Qt::DecorationRole).isValid());
    QCOMPARE(overview.commentCount(), 4);
    QTRY_VERIFY2(summary->text().startsWith(QStringLiteral("4 items")), qPrintable(summary->text()));

    // The tree counts them, and choosing a document keeps them listed.
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(documents);
    QCOMPARE(documents->topLevelItem(0)->text(1), QStringLiteral("4"));
    QTreeWidgetItem *alpha = nullptr;
    for (QTreeWidgetItemIterator it(documents); *it && !alpha; ++it) {
        if ((*it)->data(0, Qt::UserRole).toString() == QStringLiteral("alpha")) alpha = *it;
    }
    QVERIFY(alpha);
    documents->setCurrentItem(alpha);
    QTRY_COMPARE(table->model()->rowCount(), 4);
    QVERIFY(rowOf(QStringLiteral("the passage")).isValid());
    earlier->setChecked(true);
    QTRY_COMPARE(table->model()->rowCount(), 7);
    QCOMPARE(documents->topLevelItem(0)->text(1), QStringLiteral("7"));
    QTRY_VERIFY2(summary->text().startsWith(QStringLiteral("7 items")), qPrintable(summary->text()));
    documents->setCurrentItem(documents->topLevelItem(0));
    QTRY_COMPARE(table->model()->rowCount(), 7);
    QCOMPARE(cell(QStringLiteral("Too long"), 5), QStringLiteral("Dislike · Earlier version"));
    QCOMPARE(cell(QStringLiteral("Why this bound?"), 5), QStringLiteral("Question · Earlier version"));
    QCOMPARE(cell(QStringLiteral("Why this bound?"), 3), QStringLiteral("Earlier · eeeeeeee"));
    QCOMPARE(cell(QStringLiteral("Fix it."), 5), QStringLiteral("Comment · Resolved"));

    // Each type on its own; Comments is the plain comment and the type called Other.
    chooseType(QStringLiteral("like"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    chooseType(QStringLiteral("dislike"));
    QTRY_VERIFY(rowOf(QStringLiteral("Too long")).isValid());
    QCOMPARE(table->model()->rowCount(), 1);
    chooseType(QStringLiteral("question"));
    QTRY_VERIFY(rowOf(QStringLiteral("Why this bound?")).isValid());
    QCOMPARE(table->model()->rowCount(), 1);
    chooseType(QStringLiteral("comment"));
    QTRY_COMPARE(table->model()->rowCount(), 2);
    QVERIFY(rowOf(QStringLiteral("Fix it.")).isValid());
    QVERIFY(rowOf(QStringLiteral("A plain comment")).isValid());
    chooseType(QStringLiteral("drawing"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QVERIFY(rowOf(QStringLiteral("Drawing")).isValid());

    // A drawing is deleted, not resolved.
    table->selectionModel()->setCurrentIndex(rowOf(QStringLiteral("Drawing")), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(editor->isReadOnly());
    QVERIFY(!resolve->isVisible());
    // So is a text box.
    chooseType(QStringLiteral("text"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QVERIFY(rowOf(QStringLiteral("Why not the median?")).isValid());
    table->selectionModel()->setCurrentIndex(rowOf(QStringLiteral("Why not the median?")), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(editor->isReadOnly());
    QVERIFY(!resolve->isVisible());

    // Selected, it can be resolved but not typed into; opening it goes to the document.
    chooseType(QStringLiteral("question"));
    QTRY_VERIFY(rowOf(QStringLiteral("Why this bound?")).isValid());
    table->selectionModel()->setCurrentIndex(rowOf(QStringLiteral("Why this bound?")), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(editor->isReadOnly());
    QVERIFY(resolve->isVisible());
    QCOMPARE(resolve->text(), QStringLiteral("Resolve"));
    QSignalSpy writes(&overview, &Thoughts::CommentsOverview::documentFeedbackWriteRequested);
    resolve->click();
    QCOMPARE(writes.size(), 1);
    const QJsonObject resolved = QJsonObject::fromVariantMap(writes.constFirst().constFirst().toMap());
    QVERIFY(resolved.contains(QStringLiteral("resolved_at")));
    QCOMPARE(resolved.value(QStringLiteral("revision")).toInt(), asked.value(QStringLiteral("revision")).toInt() + 1);
    QCOMPARE(Thoughts::DocumentFeedback::validationError(resolved), QString());
    QSignalSpy opened(&overview, &Thoughts::CommentsOverview::documentFeedbackActivated);
    Q_EMIT table->doubleClicked(rowOf(QStringLiteral("Why this bound?")));
    QTRY_COMPARE(opened.size(), 1);
    QCOMPARE(opened.constFirst().at(1).toString(), asked.value(QStringLiteral("feedback_id")).toString());

    // A plain comment stays editable, and a document without versions has no snapshot.
    chooseType(QStringLiteral("comment"));
    QTRY_VERIFY(rowOf(QStringLiteral("A plain comment")).isValid());
    table->selectionModel()->setCurrentIndex(rowOf(QStringLiteral("A plain comment")), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(!editor->isReadOnly());
    QVERIFY(!resolve->isVisible());
    overview.m_documents[0].state = Thoughts::TrackedCommentDocument::PdfOnly;
    overview.populateComments();
    QTRY_COMPARE(cell(QStringLiteral("Fix it."), 3), QStringLiteral("—"));
}

void CommentsOverviewTest::testCommentIssueFilter()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    auto bad = comment(QStringLiteral("bad"), QStringLiteral("Unresolved comment"), 1, QStringLiteral("2026-09-21T08:00:00Z"), QStringLiteral("needs_attention"));
    auto good = comment(QStringLiteral("good"), QStringLiteral("Healthy comment"), 1, QStringLiteral("2026-09-21T08:00:00Z"), QStringLiteral("anchored"));
    auto deleted = bad;
    deleted.insert(QStringLiteral("id"), QStringLiteral("deleted"));
    deleted.insert(QStringLiteral("deleted"), true);
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(bad) + jsonLine(good) + jsonLine(deleted)));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(table);
    QTRY_COMPARE(overview.commentIssueCount(), 1);
    overview.showCommentIssues();
    QTRY_COMPARE(table->model()->rowCount(), 1);
    auto *filter = overview.findChild<QComboBox *>(QStringLiteral("commentsOverviewTypeFilter"));
    QCOMPARE(filter->currentData().toString(), QStringLiteral("issues"));
    filter->setCurrentIndex(0);
    QTRY_COMPARE(table->model()->rowCount(), 2);
    // It is not one of the types: only the issues link brings it.
    QTRY_COMPARE(filter->findData(QStringLiteral("issues")), -1);
}

void CommentsOverviewTest::testAnchorRemovedCommentsAreIssuesOnly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("alpha revision")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/beta.pdf"), QByteArrayLiteral("beta revision")));
    const QString alphaSha256 = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("alpha revision"), QCryptographicHash::Sha256).toHex());
    auto bad = comment(QStringLiteral("bad"), QStringLiteral("Unresolved comment"), 1, QStringLiteral("2026-09-21T08:00:00Z"), QStringLiteral("needs_attention"));
    auto good = comment(QStringLiteral("good"), QStringLiteral("Healthy comment"), 1, QStringLiteral("2026-09-21T08:00:00Z"), QStringLiteral("anchored"));
    auto orphaned = comment(QStringLiteral("orphaned-placement"), QStringLiteral("Orphaned placement"), 1, QStringLiteral("2026-09-21T08:00:00Z"), QStringLiteral("orphaned"));
    orphaned.insert(QStringLiteral("orphan_reason"), QStringLiteral("anchor_removed"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/beta_comments.jsonl"), jsonLine(bad) + jsonLine(good) + jsonLine(orphaned)));
    const QJsonObject removed{{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), QStringLiteral("removed-1")},
        {QStringLiteral("thread_root_id"), QStringLiteral("removed-1")}, {QStringLiteral("parent_id"), QJsonValue::Null}, {QStringLiteral("revision"), 4},
        {QStringLiteral("created_at"), QStringLiteral("2026-10-07T08:00:00Z")}, {QStringLiteral("updated_at"), QStringLiteral("2026-10-07T09:00:00Z")},
        {QStringLiteral("deleted"), false}, {QStringLiteral("contents"), QStringLiteral("Comment whose marker vanished")}, {QStringLiteral("author"), QStringLiteral("Alex")},
        {QStringLiteral("subtype"), QStringLiteral("Text")}, {QStringLiteral("source_authority"), QStringLiteral("client-v1")},
        {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QStringLiteral("alpha")}, {QStringLiteral("document_id"), QStringLiteral("alpha-document")},
            {QStringLiteral("pdf_sha256"), alphaSha256}, {QStringLiteral("page_index"), 0}, {QStringLiteral("rect"), QJsonArray{0.2, 0.3, 0.4, 0.5}}}},
        {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("needs_attention")}, {QStringLiteral("reason"), QStringLiteral("anchor_removed")},
            {QStringLiteral("anchor_id"), QStringLiteral("removed-1")}, {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
            {QStringLiteral("source_line"), 42}, {QStringLiteral("anchor_kind"), QStringLiteral("CommentAnchor")},
            {QStringLiteral("anchor_mirror"), QStringLiteral("Comment whose marker vanished")}, {QStringLiteral("anchor_removed_at"), QStringLiteral("2026-10-07T09:00:00Z")}}},
        {QStringLiteral("locations"), QJsonArray()}};
    QJsonObject linked = removed;
    linked.insert(QStringLiteral("comment_id"), QStringLiteral("linked-1"));
    linked.insert(QStringLiteral("thread_root_id"), QStringLiteral("linked-1"));
    linked.insert(QStringLiteral("contents"), QStringLiteral("Linked comment"));
    linked.insert(QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")}, {QStringLiteral("anchor_id"), QStringLiteral("linked-1")},
        {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")}, {QStringLiteral("source_line"), 12}, {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}});
    QVERIFY(writeLogicalCommentBundle(root, QStringLiteral("alpha.pdf"), QJsonArray{removed, linked}));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *filter = overview.findChild<QComboBox *>(QStringLiteral("commentsOverviewTypeFilter"));
    auto *restore = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewRestoreAnchor"));
    auto *summary = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewSummary"));
    QVERIFY(table && filter && restore && summary);
    // Anchor-removed comments are Issues: counted there and absent from the main lists.
    QTRY_COMPARE(overview.commentIssueCount(), 3);
    QTRY_COMPARE(table->model()->rowCount(), 3);
    filter->setCurrentIndex(filter->findData(QStringLiteral("comment")));
    QTRY_COMPARE(table->model()->rowCount(), 3);
    for (int row = 0; row < table->model()->rowCount(); ++row)
        QVERIFY(table->model()->index(row, 5).data().toString() != QStringLiteral("Anchor removed"));
    overview.showCommentIssues();
    QTRY_COMPARE(table->model()->rowCount(), 3);
    QModelIndex logicalRow;
    QModelIndex placementRow;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        const QModelIndex status = table->model()->index(row, 5);
        if (table->model()->index(row, 0).data().toString() == QStringLiteral("Comment whose marker vanished")) logicalRow = status;
        if (table->model()->index(row, 0).data().toString() == QStringLiteral("Orphaned placement")) placementRow = status;
    }
    QVERIFY(logicalRow.isValid() && placementRow.isValid());
    QCOMPARE(logicalRow.data().toString(), QStringLiteral("Anchor removed"));
    QCOMPARE(placementRow.data().toString(), QStringLiteral("Anchor removed"));
    QVERIFY(!logicalRow.data(Qt::DecorationRole).value<QIcon>().isNull());
    QVERIFY(logicalRow.data(Qt::ToolTipRole).toString().contains(QStringLiteral("docs-test/tex/source/main.tex:42")));
    QVERIFY(logicalRow.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Restore re-inserts it")));

    // Restore is offered only when every selected row is an anchor-removed comment.
    table->selectionModel()->setCurrentIndex(logicalRow.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(restore->isVisible());
    QVERIFY(restore->isEnabled());
    QCOMPARE(restore->text(), QStringLiteral("Restore Anchor"));
    QSignalSpy requested(&overview, &Thoughts::CommentsOverview::commentsRestoreAnchorRequested);
    restore->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first().first().toStringList(), QStringList{QStringLiteral("removed-1")});
    QVERIFY(!restore->isEnabled());
    overview.completeAnchorRestore({{QStringLiteral("success"), true}, {QStringLiteral("message"), QStringLiteral("Anchor restored at docs-test/tex/source/main.tex:42.")}});
    QVERIFY(restore->isEnabled());
    QCOMPARE(summary->text(), QStringLiteral("Anchor restored at docs-test/tex/source/main.tex:42."));
    const QModelIndex badRow = table->model()->index(logicalRow.row() == 0 ? 1 : 0, 0);
    table->selectionModel()->select(badRow, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTRY_VERIFY(!restore->isVisible());
    table->selectionModel()->setCurrentIndex(placementRow.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(!restore->isVisible());
    // Delete remains available for anchor-removed comments.
    table->selectionModel()->setCurrentIndex(logicalRow.siblingAtColumn(0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDelete"))->isEnabled());
}

// A proposal removed outside Thoughts (protocol/agent-proposals.md §10) is a
// server row listed with the agent proposals and under Issues, with Restore
// Proposal, Copy Proposal Text and Dismiss; it can no longer be accepted or rejected.
void CommentsOverviewTest::testRemovedProposalsAreIssuesOnly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QJsonObject snapshot{{QStringLiteral("proposal_id"), QStringLiteral("agent-1")}, {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
                               {QStringLiteral("comment"), QStringLiteral("Clarify the claim.\n")}, {QStringLiteral("before"), QStringLiteral("Old line.\n")},
                               {QStringLiteral("after"), QStringLiteral("New line.\n")}, {QStringLiteral("created_at"), QStringLiteral("2026-10-10T12:00:00.000Z")},
                               {QStringLiteral("conversation_id"), QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566")}};
    const QJsonObject removed{{QStringLiteral("schema_version"), 1}, {QStringLiteral("proposal_id"), QStringLiteral("agent-1")},
                              {QStringLiteral("status"), QStringLiteral("removed_outside")}, {QStringLiteral("kind"), QStringLiteral("removed_outside")},
                              {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
                              {QStringLiteral("wrapper_sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("revision"), 5}, {QStringLiteral("deleted"), false},
                              {QStringLiteral("removal"), QJsonObject{{QStringLiteral("source_line"), 42}, {QStringLiteral("looks_like"), QStringLiteral("accept")},
                                                                      {QStringLiteral("removed_at"), QStringLiteral("2026-10-11T09:00:00.000Z")}}}};
    QJsonObject pending = removed;
    pending.insert(QStringLiteral("proposal_id"), QStringLiteral("agent-2"));
    pending.insert(QStringLiteral("status"), QStringLiteral("pending"));
    pending.remove(QStringLiteral("removal"));
    QJsonObject dismissed = removed;
    dismissed.insert(QStringLiteral("proposal_id"), QStringLiteral("agent-3"));
    dismissed.insert(QStringLiteral("status"), QStringLiteral("dismissed"));
    dismissed.insert(QStringLiteral("deleted"), true);

    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *summary = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewSummary"));
    auto *restore = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewRestoreProposal"));
    auto *dismiss = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDismissProposal"));
    auto *copy = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewCopyProposal"));
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    auto *reject = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDisregardProposal"));
    QVERIFY(table && summary && restore && dismiss && copy && accept && reject);
    // Several removed proposals selected together keep the panel and Dismiss them all.
    {
        QJsonObject second = removed;
        second.insert(QStringLiteral("proposal_id"), QStringLiteral("agent-4"));
        QJsonObject secondSnapshot = snapshot;
        secondSnapshot.insert(QStringLiteral("proposal_id"), QStringLiteral("agent-4"));
        second.insert(QStringLiteral("snapshot"), secondSnapshot);
        overview.setFedoraProjections({{QStringLiteral("proposal\nproposals\nagent-1"), removed},
                                       {QStringLiteral("proposal\nproposals\nagent-4"), second}});
        overview.m_typeFilter->setCurrentIndex(overview.m_typeFilter->findData(QStringLiteral("agent_remove")));
        QTRY_COMPARE(table->model()->rowCount(), 2);
        table->selectAll();
        QTRY_VERIFY(dismiss->isVisible());
        QCOMPARE(dismiss->text(), QStringLiteral("Dismiss 2 Proposals"));
        QSignalSpy dismissBoth(&overview, &Thoughts::CommentsOverview::proposalsDismissRequested);
        dismiss->click();
        QCOMPARE(dismissBoth.size(), 1);
        QCOMPARE(dismissBoth.first().first().toStringList().size(), 2);
        overview.completeRemovedProposalDismiss({{QStringLiteral("success"), true}, {QStringLiteral("message"), QString()}});
        table->clearSelection();
        overview.m_typeFilter->setCurrentIndex(0);
    }
    overview.setFedoraProjections({{QStringLiteral("proposal\nproposals\nagent-1"), removed},
                                   {QStringLiteral("proposal\nproposals\nagent-2"), pending},
                                   {QStringLiteral("proposal\nproposals\nagent-3"), dismissed}});
    // Only the removed proposal is listed: it is counted as an agent proposal, so All types,
    // Agent proposals and Removed proposals list it, as well as Issues.
    QTRY_COMPARE(overview.commentIssueCount(), 1);
    QTRY_COMPARE(table->model()->rowCount(), 1);
    for (const auto *type : {"agent_remove"}) {
        overview.m_typeFilter->setCurrentIndex(overview.m_typeFilter->findData(QString::fromLatin1(type)));
        QTRY_COMPARE(table->model()->rowCount(), 1);
    }
    overview.showCommentIssues();
    QTRY_COMPARE(table->model()->rowCount(), 1);
    const QModelIndex status = table->model()->index(0, 5);
    QCOMPARE(status.data().toString(), QStringLiteral("Removed outside Thoughts"));
    QVERIFY(!status.data(Qt::DecorationRole).value<QIcon>().isNull());
    QVERIFY(status.data(Qt::ToolTipRole).toString().contains(QStringLiteral("docs-test/tex/source/main.tex:42")));
    QVERIFY(status.data(Qt::ToolTipRole).toString().contains(QStringLiteral("as if it was accepted")));
    QVERIFY(status.data(Qt::ToolTipRole).toString().contains(QStringLiteral("Restore Proposal puts it back")));
    QCOMPARE(table->model()->index(0, 4).data().toString(), QStringLiteral("docs-test/tex/source/main.tex:42"));
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Clarify the claim."));

    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(restore->isVisible());
    QVERIFY(restore->isEnabled() && dismiss->isVisible() && dismiss->isEnabled() && copy->isVisible());
    QCOMPARE(restore->text(), QStringLiteral("Restore Proposal"));
    // Not decidable: it is no longer in the source.
    QVERIFY(!accept->isVisible() && !reject->isVisible());
    QVERIFY(!overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDelete"))->isVisible());

    QSignalSpy restoreRequested(&overview, &Thoughts::CommentsOverview::proposalsRestoreRequested);
    restore->click();
    QCOMPARE(restoreRequested.size(), 1);
    QCOMPARE(restoreRequested.first().first().toStringList(), QStringList{QStringLiteral("agent-1")});
    QVERIFY(!restore->isEnabled());
    // A refusal names the reason and the location and offers Copy and Dismiss.
    overview.completeRemovedProposalRestore({{QStringLiteral("success"), false},
        {QStringLiteral("message"), QStringLiteral("The proposed and the original text were both edited since the markup was removed; Thoughts cannot tell where the proposal belongs. (docs-test/tex/source/main.tex:42)")},
        {QStringLiteral("results"), QVariantList{QVariantMap{{QStringLiteral("code"), QStringLiteral("text_changed")}}}}});
    QVERIFY(restore->isEnabled());
    QVERIFY(summary->text().contains(QStringLiteral("docs-test/tex/source/main.tex:42")));
    QVERIFY(summary->text().contains(QStringLiteral("Copy Proposal Text")));
    QVERIFY(summary->text().contains(QStringLiteral("Dismiss")));

    QGuiApplication::clipboard()->clear();
    copy->click();
    const QString copied = QGuiApplication::clipboard()->text();
    QVERIFY(copied.startsWith(QStringLiteral("Proposal agent-1 in docs-test/tex/source/main.tex:42")));
    QVERIFY(copied.contains(QStringLiteral("Old line.")) && copied.contains(QStringLiteral("New line.")) && copied.contains(QStringLiteral("Clarify the claim.")));

    QSignalSpy dismissRequested(&overview, &Thoughts::CommentsOverview::proposalsDismissRequested);
    dismiss->click();
    QCOMPARE(dismissRequested.size(), 1);
    QCOMPARE(dismissRequested.first().first().toStringList(), QStringList{QStringLiteral("agent-1")});
    overview.completeRemovedProposalDismiss({{QStringLiteral("success"), true}, {QStringLiteral("message"), QStringLiteral("Proposal dismissed; the next Sync dismisses it on your other devices.")}});
    QCOMPARE(summary->text(), QStringLiteral("Proposal dismissed; the next Sync dismisses it on your other devices."));
    // The queued Dismiss retires the row from the effective projections: it is gone.
    overview.setFedoraProjections({{QStringLiteral("proposal\nproposals\nagent-2"), pending}});
    QTRY_COMPARE(overview.commentIssueCount(), 0);
    QTRY_COMPARE(table->model()->rowCount(), 0);
}

// Selecting every row of a mixed list (comments, live proposals of two
// Projects, a broken proposal, a removed one) offers Accept and Reject for the
// proposals that can be decided and Restore, Copy and Dismiss for the removed
// one. Accept decides exactly those proposals, in both sources, and Undo puts
// both sources back; the details panel's Reject works on the same selection.
void CommentsOverviewTest::testMixedSelectionDecidesItsProposals()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(registerSourceProject(root, QStringLiteral("mixed-first"), QStringLiteral("First"), {QStringLiteral("docs-first")}));
    QVERIFY(registerSourceProject(root, QStringLiteral("mixed-second"), QStringLiteral("Second"), {QStringLiteral("docs-second")}));
    const QString wrapper = QStringLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]");
    const QString firstPath = root + QStringLiteral("/docs-first/tex/source/main.tex");
    const QString secondPath = root + QStringLiteral("/docs-second/tex/source/notes.tex");
    const QString brokenPath = root + QStringLiteral("/docs-second/tex/source/broken.tex");
    const QByteArray first = (wrapper + QStringLiteral("{mixed-first-1}{First.}{Old one.}{New one.}\n")
                              + wrapper + QStringLiteral("{mixed-first-2}{Second.}{Old two.}{New two.}\n")).toUtf8();
    const QByteArray second = (QStringLiteral("Intro.\n") + wrapper + QStringLiteral("{mixed-second-1}{Third.}{Old three.}{New three.}\n")).toUtf8();
    const QByteArray broken = (wrapper + QStringLiteral("{mixed-broken}{unfinished}")).toUtf8();
    QVERIFY(writeFile(firstPath, first));
    QVERIFY(writeFile(secondPath, second));
    QVERIFY(writeFile(brokenPath, broken));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"),
                      jsonLine(comment(QStringLiteral("plain"), QStringLiteral("A plain comment"), 1, QStringLiteral("2026-10-10T12:00:00Z"), QStringLiteral("anchored")))));
    const QJsonObject snapshot{{QStringLiteral("proposal_id"), QStringLiteral("mixed-removed")}, {QStringLiteral("source_file"), QStringLiteral("docs-first/tex/source/main.tex")},
                               {QStringLiteral("comment"), QStringLiteral("Removed outside.\n")}, {QStringLiteral("before"), QStringLiteral("Gone.\n")},
                               {QStringLiteral("after"), QStringLiteral("Went.\n")}, {QStringLiteral("created_at"), QStringLiteral("2026-10-10T12:00:00.000Z")},
                               {QStringLiteral("conversation_id"), QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566")}};
    const QJsonObject removed{{QStringLiteral("schema_version"), 1}, {QStringLiteral("proposal_id"), QStringLiteral("mixed-removed")},
                              {QStringLiteral("status"), QStringLiteral("removed_outside")}, {QStringLiteral("kind"), QStringLiteral("removed_outside")},
                              {QStringLiteral("snapshot"), snapshot}, {QStringLiteral("source_file"), QStringLiteral("docs-first/tex/source/main.tex")},
                              {QStringLiteral("wrapper_sha256"), QString(64, QLatin1Char('a'))}, {QStringLiteral("revision"), 5}, {QStringLiteral("deleted"), false},
                              {QStringLiteral("removal"), QJsonObject{{QStringLiteral("source_line"), 3}, {QStringLiteral("looks_like"), QStringLiteral("accept")},
                                                                      {QStringLiteral("removed_at"), QStringLiteral("2026-10-11T09:00:00.000Z")}}}};

    Thoughts::CommentsOverview overview(root);
    overview.setFedoraProjections({{QStringLiteral("proposal\nproposals\nmixed-removed"), removed}});
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    auto *reject = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDisregardProposal"));
    auto *restore = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewRestoreProposal"));
    QVERIFY(table && accept && reject && restore);
    // The comment, three live proposals, the broken one and the removed one.
    QTRY_COMPARE(table->model()->rowCount(), 6);
    QSignalSpy bundleResolution(&overview, &Thoughts::CommentsOverview::agentProposalsResolveRequested);
    QSignalSpy restoreRequested(&overview, &Thoughts::CommentsOverview::proposalsRestoreRequested);

    table->selectAll();
    QTRY_VERIFY(accept->isVisible() && accept->isEnabled());
    QCOMPARE(accept->text(), QStringLiteral("Accept 3 Proposals"));
    QCOMPARE(reject->text(), QStringLiteral("Reject 3 Proposals"));
    QVERIFY(reject->isEnabled());
    QVERIFY(restore->isVisible());
    QCOMPARE(restore->text(), QStringLiteral("Restore Proposal"));
    QCOMPARE(overview.selectedAgentProposalIds().size(), 3);
    QVERIFY(overview.selectedAgentProposalsAreLive());

    QStringList menuItems;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (QAction *action : menu->actions()) menuItems.append(action->isSeparator() ? QStringLiteral("|") : action->text().remove(QLatin1Char('&')));
        if (auto *action = menu->findChild<QAction *>(QStringLiteral("commentsOverviewAcceptProposalAction")); action && action->isEnabled()) action->trigger();
        menu->close();
    });
    QModelIndex proposalRow;
    for (int row = 0; row < table->model()->rowCount() && !proposalRow.isValid(); ++row) {
        if (table->model()->index(row, 0).data().toString() == QStringLiteral("First.")) proposalRow = table->model()->index(row, 0);
    }
    QVERIFY(proposalRow.isValid());
    QVERIFY(QMetaObject::invokeMethod(&overview, "showCommentsContextMenu", Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(proposalRow).center())));
    QCOMPARE(menuItems, (QStringList{QStringLiteral("Open with VS Code"), QStringLiteral("|"), QStringLiteral("Accept 3 Proposals"), QStringLiteral("Reject 3 Proposals"),
                                     QStringLiteral("|"), QStringLiteral("Restore Proposal"), QStringLiteral("Copy Proposal Text"), QStringLiteral("Dismiss")}));

    // Both sources are decided in one batch; the broken source, the comment
    // and the removed proposal are left as they were.
    QTRY_COMPARE(readFile(firstPath), QByteArrayLiteral("New one.\nNew two.\n"));
    QTRY_COMPARE(readFile(secondPath), QByteArrayLiteral("Intro.\nNew three.\n"));
    QTRY_VERIFY(!overview.m_operationPending);
    QCOMPARE(readFile(brokenPath), broken);
    QCOMPARE(bundleResolution.size(), 0);
    QCOMPARE(restoreRequested.size(), 0);
    QTRY_COMPARE(table->model()->rowCount(), 3);
    QStringList remaining;
    for (int row = 0; row < table->model()->rowCount(); ++row) remaining.append(table->model()->index(row, 0).data().toString());
    remaining.sort();
    QCOMPARE(remaining.size(), 3);
    QCOMPARE(remaining.at(0), QStringLiteral("A plain comment"));
    QCOMPARE(remaining.at(1), QStringLiteral("Removed outside."));
    QVERIFY(remaining.at(2).startsWith(QStringLiteral("This proposal cannot be accepted or rejected")));
    // The reload that follows the decision redraws the list's counts, then shows the outcome again.
    QTRY_VERIFY2(overview.m_summary->text().contains(QStringLiteral("3 proposals accepted")), qPrintable(overview.m_summary->text()));

    // Undo puts both sources back, and their proposals are listed again.
    overview.undoOverviewChange();
    QTRY_COMPARE(readFile(firstPath), first);
    QTRY_COMPARE(readFile(secondPath), second);
    QTRY_VERIFY(!overview.m_operationPending);
    QVERIFY2(overview.m_summary->text().contains(QStringLiteral("Acceptance undone — 3 proposals restored.")), qPrintable(overview.m_summary->text()));
    QTRY_COMPARE(table->model()->rowCount(), 6);
    QVERIFY(overview.m_undoHistory.isEmpty());

    // The details panel's Reject decides the same proposals of the same mixed selection.
    table->selectAll();
    QTRY_VERIFY(reject->isVisible() && reject->isEnabled());
    QCOMPARE(reject->text(), QStringLiteral("Reject 3 Proposals"));
    QTest::mouseClick(reject, Qt::LeftButton);
    QTRY_COMPARE(readFile(firstPath), QByteArrayLiteral("Old one.\nOld two.\n"));
    QTRY_COMPARE(readFile(secondPath), QByteArrayLiteral("Intro.\nOld three.\n"));
    QCOMPARE(readFile(brokenPath), broken);
    QTRY_COMPARE(table->model()->rowCount(), 3);
    QCOMPARE(bundleResolution.size(), 0);
}

// Artifact-only proposals are decided through the Sync queue one bundle at a time: a
// selection across two bundles decides the current row's.
void CommentsOverviewTest::testMixedArtifactSelectionDecidesOneBundle()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), QByteArrayLiteral("Standard SDPO.\n")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("alpha PDF")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/beta.pdf"), QByteArrayLiteral("beta PDF")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("alpha.pdf"), QStringLiteral("alpha-proposal")));
    QVERIFY(writeAgentEditProposalBundle(root, QStringLiteral("beta.pdf"), QStringLiteral("beta-proposal")));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    QVERIFY(table && accept);
    QTRY_COMPARE(table->model()->rowCount(), 2);
    QModelIndex beta;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 1).data().toString().contains(QStringLiteral("beta"))) beta = table->model()->index(row, 0);
    }
    QVERIFY(beta.isValid());
    table->selectAll();
    table->selectionModel()->setCurrentIndex(beta, QItemSelectionModel::NoUpdate);
    QTRY_VERIFY(accept->isVisible() && accept->isEnabled());
    QString bundleId;
    QCOMPARE(overview.selectedAgentProposalIds(&bundleId), QStringList{QStringLiteral("beta-proposal")});
    QCOMPARE(bundleId.size(), 64);
    QVERIFY(!overview.selectedAgentProposalsAreLive());
    QSignalSpy resolution(&overview, &Thoughts::CommentsOverview::agentProposalsResolveRequested);
    QTest::mouseClick(accept, Qt::LeftButton);
    QCOMPARE(resolution.size(), 1);
    QCOMPARE(resolution.constFirst().at(0).toString(), bundleId);
    QCOMPARE(resolution.constFirst().at(1).toString(), QStringLiteral("accept"));
    QCOMPARE(resolution.constFirst().at(2).toStringList(), QStringList{QStringLiteral("beta-proposal")});
}

// A batch that holds one half of a move (§12) across two files and another
// proposal in the file of the other half decides all of them, whichever file
// comes first, and Undo puts both files back.
void CommentsOverviewTest::testBatchDecidesMoveAcrossSources()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(registerSourceProject(root, QStringLiteral("move-first"), QStringLiteral("First"), {QStringLiteral("docs-first")}));
    QVERIFY(registerSourceProject(root, QStringLiteral("move-second"), QStringLiteral("Second"), {QStringLiteral("docs-second")}));
    const QString wrapper = QStringLiteral("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]");
    const QString cut = QStringLiteral("agent-11111111-2222-4333-8444-555555555555.cut");
    const QString paste = QStringLiteral("agent-11111111-2222-4333-8444-555555555555.paste");
    const QString fromPath = root + QStringLiteral("/docs-first/tex/source/main.tex");
    const QString toPath = root + QStringLiteral("/docs-second/tex/source/notes.tex");
    const QByteArray from = (QStringLiteral("Keep.\n") + wrapper + QStringLiteral("{%1}{Move it away.}{Moved passage.\n}{}").arg(cut) + QStringLiteral("End.\n")).toUtf8();
    const QByteArray to = (wrapper + QStringLiteral("{%1}{Move it here.}{}{Moved passage.\n}").arg(paste)
                           + wrapper + QStringLiteral("{move-other}{Other.}{Old.}{New.}\n")).toUtf8();
    QVERIFY(writeFile(fromPath, from));
    QVERIFY(writeFile(toPath, to));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *accept = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewAcceptProposal"));
    QVERIFY(table && accept);
    QTRY_COMPARE(table->model()->rowCount(), 3);
    const QByteArray decidedFrom = QByteArrayLiteral("Keep.\nEnd.\n");
    const QByteArray decidedTo = QByteArrayLiteral("Moved passage.\nNew.\n");

    // The removal half and the other proposal: the insertion is decided with
    // its half. Selected in both orders, so either file is decided first.
    for (const bool reversed : {false, true}) {
        QTRY_VERIFY(!overview.m_operationPending);
        QItemSelection selection;
        for (int step = 0; step < table->model()->rowCount(); ++step) {
            const QModelIndex index = table->model()->index(reversed ? table->model()->rowCount() - 1 - step : step, 0);
            if (index.data().toString() != QStringLiteral("Move it here.")) selection.select(index, index);
        }
        table->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QTRY_VERIFY(accept->isVisible() && accept->isEnabled());
        QCOMPARE(accept->text(), QStringLiteral("Accept 2 Proposals"));
        QTest::mouseClick(accept, Qt::LeftButton);
        QTRY_COMPARE(readFile(fromPath), decidedFrom);
        QTRY_COMPARE(readFile(toPath), decidedTo);
        QTRY_VERIFY(!overview.m_operationPending);
        QVERIFY2(!overview.m_summary->text().contains(QStringLiteral("undecided")), qPrintable(overview.m_summary->text()));
        QTRY_COMPARE(table->model()->rowCount(), 0);
        overview.undoOverviewChange();
        QTRY_COMPARE(readFile(fromPath), from);
        QTRY_COMPARE(readFile(toPath), to);
        QTRY_COMPARE(table->model()->rowCount(), 3);
    }

    // Both halves and the other proposal.
    QTRY_VERIFY(!overview.m_operationPending);
    table->selectAll();
    QTRY_VERIFY(accept->isVisible() && accept->isEnabled());
    QCOMPARE(accept->text(), QStringLiteral("Accept 3 Proposals"));
    QTest::mouseClick(accept, Qt::LeftButton);
    QTRY_COMPARE(readFile(fromPath), decidedFrom);
    QTRY_COMPARE(readFile(toPath), decidedTo);
    QTRY_VERIFY(!overview.m_operationPending);
    QVERIFY2(!overview.m_summary->text().contains(QStringLiteral("undecided")), qPrintable(overview.m_summary->text()));
    QTRY_COMPARE(table->model()->rowCount(), 0);
    overview.undoOverviewChange();
    QTRY_COMPARE(readFile(fromPath), from);
    QTRY_COMPARE(readFile(toPath), to);
}

void CommentsOverviewTest::testCommentMissingMarkersAreIssuesOnly()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("alpha revision")));
    const QString alphaSha256 = QString::fromLatin1(QCryptographicHash::hash(QByteArrayLiteral("alpha revision"), QCryptographicHash::Sha256).toHex());
    const QJsonObject live{{QStringLiteral("schema_version"), 1}, {QStringLiteral("comment_id"), QStringLiteral("live-1")},
        {QStringLiteral("thread_root_id"), QStringLiteral("live-1")}, {QStringLiteral("parent_id"), QJsonValue::Null}, {QStringLiteral("revision"), 2},
        {QStringLiteral("created_at"), QStringLiteral("2026-10-12T08:00:00Z")}, {QStringLiteral("updated_at"), QStringLiteral("2026-10-12T08:00:00Z")},
        {QStringLiteral("deleted"), false}, {QStringLiteral("contents"), QStringLiteral("Live comment")}, {QStringLiteral("author"), QStringLiteral("Alex")},
        {QStringLiteral("subtype"), QStringLiteral("Text")}, {QStringLiteral("source_authority"), QStringLiteral("client-v1")},
        {QStringLiteral("origin"), QJsonObject{{QStringLiteral("collection"), QStringLiteral("alpha")}, {QStringLiteral("document_id"), QStringLiteral("alpha-document")},
            {QStringLiteral("pdf_sha256"), alphaSha256}, {QStringLiteral("page_index"), 0}, {QStringLiteral("rect"), QJsonArray{0.2, 0.3, 0.4, 0.5}}}},
        {QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")}, {QStringLiteral("anchor_id"), QStringLiteral("live-1")},
            {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")}, {QStringLiteral("source_line"), 3}, {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}}},
        {QStringLiteral("locations"), QJsonArray()}};
    QVERIFY(writeLogicalCommentBundle(root, QStringLiteral("alpha.pdf"), QJsonArray{live}));
    QJsonObject liveProjection = live;
    liveProjection.remove(QStringLiteral("locations"));
    const QJsonObject deletedMarker{{QStringLiteral("anchor_id"), QStringLiteral("dead-1")}, {QStringLiteral("source_file"), QStringLiteral("docs-test/tex/source/main.tex")},
        {QStringLiteral("line"), 12}, {QStringLiteral("contents"), QStringLiteral("Comment of a deleted marker")}, {QStringLiteral("kind"), QStringLiteral("highlight")},
        {QStringLiteral("selected_text"), QStringLiteral("highlighted words")}, {QStringLiteral("state"), QStringLiteral("deleted")}, {QStringLiteral("comment_id"), QStringLiteral("dead-1")},
        {QStringLiteral("targets"), QJsonArray{QStringLiteral("alpha.pdf")}}, {QStringLiteral("project"), QStringLiteral("docs-test")}};
    const QJsonObject unknownMarker{{QStringLiteral("anchor_id"), QStringLiteral("thoughts-unknown")}, {QStringLiteral("source_file"), QStringLiteral("docs-other/notes.tex")},
        {QStringLiteral("line"), 4}, {QStringLiteral("contents"), QStringLiteral("Comment nobody knows")}, {QStringLiteral("kind"), QStringLiteral("anchor")},
        {QStringLiteral("state"), QStringLiteral("unknown")}, {QStringLiteral("targets"), QJsonArray{QStringLiteral("other.pdf")}}, {QStringLiteral("project"), QStringLiteral("docs-other")}};
    QJsonObject projections{{QStringLiteral("logical_comment\ncomments\nlive-1"), liveProjection},
        {Thoughts::commentMissingProjectionKey(), QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("markers"), QJsonArray{deletedMarker, unknownMarker}}}}};

    Thoughts::CommentsOverview overview(root, nullptr, {}, {}, [&] { return projections; });
    overview.setFedoraProjections(projections);
    overview.show();
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *filter = overview.findChild<QComboBox *>(QStringLiteral("commentsOverviewTypeFilter"));
    auto *remove = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewRemoveMissingMarker"));
    auto *restore = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewRestoreMissingComment"));
    auto *deleteButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDelete"));
    auto *summary = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewSummary"));
    QVERIFY(table && filter && remove && restore && deleteButton && summary);
    // Both markers are counted as Issues and stay out of the main lists.
    QTRY_COMPARE(overview.commentIssueCount(), 2);
    filter->setCurrentIndex(filter->findData(QStringLiteral("comment")));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Live comment"));
    overview.showCommentIssues();
    QTRY_COMPARE(table->model()->rowCount(), 2);
    QModelIndex deletedRow;
    QModelIndex unknownRow;
    for (int row = 0; row < table->model()->rowCount(); ++row) {
        if (table->model()->index(row, 0).data().toString() == QStringLiteral("Comment of a deleted marker")) deletedRow = table->model()->index(row, 0);
        if (table->model()->index(row, 0).data().toString() == QStringLiteral("Comment nobody knows")) unknownRow = table->model()->index(row, 0);
    }
    QVERIFY(deletedRow.isValid() && unknownRow.isValid());
    QCOMPARE(deletedRow.siblingAtColumn(5).data().toString(), QStringLiteral("Comment missing"));
    QCOMPARE(unknownRow.siblingAtColumn(5).data().toString(), QStringLiteral("Comment missing"));
    QCOMPARE(deletedRow.siblingAtColumn(4).data().toString(), QStringLiteral("docs-test/tex/source/main.tex:12"));
    QCOMPARE(unknownRow.siblingAtColumn(4).data().toString(), QStringLiteral("docs-other/notes.tex:4"));
    QVERIFY(!deletedRow.siblingAtColumn(5).data(Qt::DecorationRole).value<QIcon>().isNull());
    QVERIFY(deletedRow.siblingAtColumn(5).data(Qt::ToolTipRole).toString().contains(QStringLiteral("was deleted")));
    QVERIFY(unknownRow.siblingAtColumn(5).data(Qt::ToolTipRole).toString().contains(QStringLiteral("no known comment")));

    // Remove Marker and Restore Comment are the actions; Delete and editing are not.
    table->selectionModel()->setCurrentIndex(deletedRow, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(remove->isVisible() && restore->isVisible());
    QVERIFY(remove->isEnabled() && restore->isEnabled());
    QCOMPARE(remove->text(), QStringLiteral("Remove Marker"));
    QCOMPARE(restore->text(), QStringLiteral("Restore Comment"));
    QVERIFY(!deleteButton->isEnabled());
    QVERIFY(!overview.findChild<QWidget *>(QStringLiteral("commentsOverviewEditor"))->isEnabled());
    QSignalSpy requested(&overview, &Thoughts::CommentsOverview::missingCommentMarkersResolveRequested);
    restore->click();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first().at(0).toString(), QStringLiteral("restore"));
    const QVariantList sent = requested.first().at(1).toList();
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first().toMap().value(QStringLiteral("anchor_id")).toString(), QStringLiteral("dead-1"));
    QCOMPARE(sent.first().toMap().value(QStringLiteral("source_file")).toString(), QStringLiteral("docs-test/tex/source/main.tex"));
    QVERIFY(!restore->isEnabled() && !remove->isEnabled());
    overview.completeMissingCommentResolution({{QStringLiteral("success"), false}, {QStringLiteral("message"), QStringLiteral("The marker is no longer in docs-test/tex/source/main.tex.")}});
    QVERIFY(restore->isEnabled());
    QCOMPARE(summary->text(), QStringLiteral("The marker is no longer in docs-test/tex/source/main.tex."));
    // Both rows selected: one Remove request carries both markers.
    table->selectionModel()->select(unknownRow, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTRY_COMPARE(remove->text(), QStringLiteral("Remove 2 Markers"));
    remove->click();
    QCOMPARE(requested.size(), 2);
    QCOMPARE(requested.last().at(0).toString(), QStringLiteral("remove"));
    QCOMPARE(requested.last().at(1).toList().size(), 2);
    overview.completeMissingCommentResolution({{QStringLiteral("success"), true}, {QStringLiteral("message"), QStringLiteral("Marker removed.")}});

    // Once the overlay no longer lists them, the rows are gone.
    projections.remove(Thoughts::commentMissingProjectionKey());
    overview.setFedoraProjections(projections);
    QTRY_COMPARE(overview.commentIssueCount(), 0);
}

void CommentsOverviewTest::testSourceConflictReportsMergeNeeded()
{
    // A conflict Fedora preserved outranks Sync failed and PDF build failed
    // everywhere the operation is named.
    const QJsonObject conflict {{QStringLiteral("kind"), QStringLiteral("sync")}, {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("phase"), QStringLiteral("failed")}, {QStringLiteral("last_error_code"), QStringLiteral("invalid_source_publication")},
        {QStringLiteral("last_error_message"), QStringLiteral("Source publication failed: source conflict was preserved on the server for resolution: docs-paper/tex/intro.tex")}};
    QCOMPARE(Thoughts::syncOperationOutcome(conflict).title, QStringLiteral("Merge needed"));
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(conflict), QStringLiteral("Merge needed"));
    QCOMPARE(Thoughts::syncOperationDisplayStatus(conflict), QStringLiteral("Merge needed"));
    QJsonObject inEvents {{QStringLiteral("kind"), QStringLiteral("sync")}, {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("phase"), QStringLiteral("failed")}, {QStringLiteral("last_error_code"), QStringLiteral("sync_failed")},
        {QStringLiteral("events"), QJsonArray {QJsonObject {{QStringLiteral("error_code"), QStringLiteral("invalid_source_publication")},
            {QStringLiteral("message"), QStringLiteral("Source publication failed: source conflict was preserved on the server for resolution: a.tex")}}}}};
    QCOMPARE(Thoughts::syncOperationOutcome(inEvents).title, QStringLiteral("Merge needed"));

    // Fedora refusing a stale base is not a merge: nothing was preserved and
    // there is nothing to resolve.
    QJsonObject staleBase = conflict;
    staleBase.insert(QStringLiteral("last_error_message"),
        QStringLiteral("Source publication failed: frozen Desktop source has no matching authoring or server base: docs-paper/tex/intro.tex"));
    QCOMPARE(Thoughts::syncOperationOutcome(staleBase).title, QStringLiteral("Sync failed"));
    QCOMPARE(Thoughts::syncOperationDisplayStatus(staleBase), QStringLiteral("Sync failed"));
    QVERIFY(Thoughts::syncOperationReportsSourceConflict(staleBase));
    QVERIFY(!Thoughts::syncOperationReportsPreservedConflict(staleBase));
    QJsonObject other = conflict;
    other.insert(QStringLiteral("last_error_code"), QStringLiteral("network"));
    QCOMPARE(Thoughts::syncOperationOutcome(other).title, QStringLiteral("Sync failed"));

    // The count is every recorded conflict, or one reported by the latest Sync.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString state = temporary.filePath(QStringLiteral("source-publication.json"));
    QCOMPARE(Thoughts::sourceFilesNeedingMerge(other, state), 0);
    QCOMPARE(Thoughts::sourceFilesNeedingMerge(staleBase, state), 0);
    QCOMPARE(Thoughts::sourceFilesNeedingMerge(conflict, state), 1);
    QVERIFY(writeFile(state, QJsonDocument(QJsonObject {{QStringLiteral("conflicts"), QJsonObject {
        {QStringLiteral("docs-paper/tex/intro.tex"), QJsonObject {{QStringLiteral("conflict_id"), QStringLiteral("a")}}},
        {QStringLiteral("docs-paper/tex/method.tex"), QJsonObject {{QStringLiteral("conflict_id"), QStringLiteral("b")}}}}}}).toJson()));
    QCOMPARE(Thoughts::recordedSourceConflictPaths(state), (QStringList {QStringLiteral("docs-paper/tex/intro.tex"), QStringLiteral("docs-paper/tex/method.tex")}));
    QCOMPARE(Thoughts::sourceFilesNeedingMerge(other, state), 2);
}

void CommentsOverviewTest::testMergeNeededOffersOnlyShowMerge()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    // initTestCase enables test mode, so this is a test-only location.
    const QString state = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("thoughts-server/source-publication.json"));
    QVERIFY(writeFile(state, QJsonDocument(QJsonObject {{QStringLiteral("conflicts"), QJsonObject {
        {QStringLiteral("docs-paper/tex/intro.tex"), QJsonObject {{QStringLiteral("conflict_id"), QStringLiteral("a")}}}}}}).toJson()));
    const auto cleanup = qScopeGuard([&state] { QFile::remove(state); });

    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    overview.m_syncSnapshotLoaded = true;
    overview.m_latestSyncOperation = {{QStringLiteral("id"), QStringLiteral("op")}, {QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("phase"), QStringLiteral("failed")}};
    overview.updateIdleSyncPresentation();
    QCOMPARE(overview.m_pdfSyncStatus->text(), QStringLiteral("Merge needed"));
    QVERIFY(overview.m_syncLogsLink->text().contains(QStringLiteral("Show merge")));
    QVERIFY(!overview.m_syncLogsLink->text().contains(QStringLiteral("Show logs")));
    QSignalSpy merge(&overview, &Thoughts::CommentsOverview::sourceMergeRequested);
    QSignalSpy logs(&overview, &Thoughts::CommentsOverview::syncOperationLogsRequested);
    Q_EMIT overview.m_syncLogsLink->linkActivated(QStringLiteral("merge"));
    QCOMPARE(merge.count(), 1);
    QCOMPARE(logs.count(), 0);

    // Once merged, an ordinary failure offers its logs again.
    QVERIFY(QFile::remove(state));
    overview.updateIdleSyncPresentation();
    QCOMPARE(overview.m_pdfSyncStatus->text(), QStringLiteral("Sync failed"));
    QVERIFY(overview.m_syncLogsLink->text().contains(QStringLiteral("Show logs")));
    Q_EMIT overview.m_syncLogsLink->linkActivated(QStringLiteral("logs"));
    QCOMPARE(logs.count(), 1);
}

void CommentsOverviewTest::testSyncOutcomeSummary()
{
    QJsonObject operation {{QStringLiteral("kind"), QStringLiteral("sync")}, {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("phase"), QStringLiteral("failed")},
        {QStringLiteral("last_error_message"), QStringLiteral("The complete source dependency closure could not be frozen safely. Traceback (most recent call last): ...")}};
    const QJsonObject provider {{QStringLiteral("metadata"), QJsonObject {{QStringLiteral("errors"), QJsonArray {
        QJsonObject {{QStringLiteral("config"), QStringLiteral("/docs/docs-course-notes/.thoughts-overleaf-error-detection.json")},
                     {QStringLiteral("error"), QStringLiteral("project attachment is unavailable: reference.pdf")}},
        QJsonObject {{QStringLiteral("config"), QStringLiteral("/docs/docs-data-preprocessing/.thoughts-overleaf.json")},
                     {QStringLiteral("error"), QStringLiteral("Fedora standalone compilation failed: latex Missing $ inserted")}}
    }}}}};
    operation.insert(QStringLiteral("events"), QJsonArray {provider});
    auto summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("Sync failed"));
    QVERIFY(summary.message.contains(QStringLiteral("before upload and download could complete")));
    QVERIFY(summary.message.contains(QStringLiteral("course-notes\nMissing attachment: reference.pdf")));
    QVERIFY(summary.message.contains(QStringLiteral("data-preprocessing\nA formatting error")));
    QVERIFY(!summary.message.contains(QStringLiteral("Traceback")));
    QVERIFY(!summary.message.contains(QStringLiteral(".thoughts-overleaf")));

    operation.insert(QStringLiteral("last_error_message"), QStringLiteral("Available updates were synchronized; some documents need attention. ..."));
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("Sync failed"));
    QVERIFY(summary.message.contains(QStringLiteral("Some project changes still need attention")));
    // Updates were received but local changes were not all sent, even with
    // no project listed as failing: the Sync failed, it is not up to date.
    QJsonObject availableOnly = operation;
    availableOnly.insert(QStringLiteral("events"), QJsonArray());
    availableOnly.insert(QStringLiteral("sync_outcome"), QStringLiteral("available_updates_synced"));
    summary = Thoughts::syncOperationOutcome(availableOnly);
    QCOMPARE(summary.title, QStringLiteral("Sync failed"));
    QVERIFY(summary.message.contains(QStringLiteral("Some project changes still need attention")));

    // Current runs use an explicit persisted boundary, independent of wording.
    operation.insert(QStringLiteral("last_error_message"), QStringLiteral("Translated failure message"));
    operation.insert(QStringLiteral("sync_outcome"), QStringLiteral("changes_synced"));
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("Sync failed"));
    QVERIFY(summary.message.contains(QStringLiteral("Projects needing attention")));
    QVERIFY(!summary.message.contains(QStringLiteral("Your changes are synced with the server.")));

    operation.remove(QStringLiteral("sync_outcome"));
    operation.insert(QStringLiteral("last_error_code"), QStringLiteral("pdf_build_failed"));
    const QJsonArray failedDocuments {
        QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("message"), QStringLiteral("latex_error: failed")}},
        QJsonObject {{QStringLiteral("filename"), QStringLiteral("paper.pdf")}, {QStringLiteral("message"), QStringLiteral("latex_error: failed again")}}
    };
    const QJsonObject buildResult {{QStringLiteral("result"), QJsonObject {{QStringLiteral("failed_documents"), failedDocuments}}}};
    const QJsonObject buildEvent {{QStringLiteral("metadata"), QJsonObject {{QStringLiteral("build"), buildResult}}}};
    operation.insert(QStringLiteral("events"), QJsonArray {buildEvent});
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
    QVERIFY(summary.message.startsWith(QStringLiteral("Your changes are synced · 1 project failed to build.\n\n")));
    QCOMPARE(summary.message.count(QStringLiteral("paper\n")), 1);
    // Fedora renderer failures use pdf_filename/error, including global builds.
    const QJsonObject rendererFailure {
        {QStringLiteral("pdf_filename"), QStringLiteral("data-preprocessing.pdf")},
        {QStringLiteral("error"), QStringLiteral("renderer exited with 1: strict review correspondence gate failed: source instrumentation produced no rendered proposal segments")}
    };
    const QJsonObject rendererBuild {
        {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("target"), QStringLiteral("all")},
        {QStringLiteral("result"), QJsonObject {{QStringLiteral("failed_documents"), QJsonArray {rendererFailure}}}}
    };
    operation.insert(QStringLiteral("events"), QJsonArray {QJsonObject {
        {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("build"), rendererBuild}}}
    }});
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
    QCOMPARE(summary.message, QStringLiteral("Your changes are synced · 1 project failed to build.\n\ndata-preprocessing\nA proposed edit could not be matched to its location in the updated PDF."));
    operation.insert(QStringLiteral("events"), QJsonArray());
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
    QVERIFY(summary.message.startsWith(QStringLiteral("Your changes are synced.\n\n")));
    QVERIFY(summary.message.contains(QStringLiteral("affected project was not recorded")));

    operation.insert(QStringLiteral("kind"), QStringLiteral("build"));
    operation.insert(QStringLiteral("status"), QStringLiteral("succeeded"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("succeeded"));
    QCOMPARE(Thoughts::syncOperationOutcome(operation).title, QStringLiteral("Build complete"));
    QVERIFY(!Thoughts::syncOperationOutcome(operation).message.contains(QStringLiteral("Your changes are synced")));
    operation.insert(QStringLiteral("kind"), QStringLiteral("sync"));
    operation.remove(QStringLiteral("last_error_code"));
    operation.remove(QStringLiteral("last_error_message"));
    QCOMPARE(Thoughts::syncOperationOutcome(operation).title, QStringLiteral("Up to date"));
    operation.insert(QStringLiteral("events"), QJsonArray {QJsonObject {{QStringLiteral("metadata"), QJsonObject {
        {QStringLiteral("failed_documents"), QJsonArray {QJsonObject {
            {QStringLiteral("filename"), QStringLiteral("Benchmark-followup.pdf")},
            {QStringLiteral("reason"), QStringLiteral("strict review correspondence gate failed: source instrumentation")}}}}
    }}}});
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
    QCOMPARE(summary.message, QStringLiteral("Your changes are synced · 1 project failed to build.\n\nBenchmark-followup\nA proposed edit could not be matched to its location in the updated PDF."));
    operation.insert(QStringLiteral("events"), QJsonArray());
    operation.insert(QStringLiteral("status"), QStringLiteral("running"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("building"));
    QCOMPARE(Thoughts::syncOperationOutcome(operation).title, QStringLiteral("Syncing…"));
    operation.insert(QStringLiteral("status"), QStringLiteral("failed"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("failed"));
    operation.insert(QStringLiteral("downloaded_completed_bytes"), 1000);
    operation.insert(QStringLiteral("downloaded_total_bytes"), 1000);
    QCOMPARE(Thoughts::syncOperationOutcome(operation).title, QStringLiteral("Sync failed"));
    // Interrupted and cancelled runs are failed Syncs; the message says which.
    operation.insert(QStringLiteral("status"), QStringLiteral("interrupted"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("interrupted"));
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("Sync failed"));
    QVERIFY(summary.message.contains(QStringLiteral("interrupted")));
    operation.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("cancelled"));
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("Sync failed"));
    QVERIFY(summary.message.contains(QStringLiteral("cancelled")));
}

void CommentsOverviewTest::testSyncOutcomeUsesFinalBuildSnapshot()
{
    const QString aggregateId = QStringLiteral("old-aggregate-build");
    const QStringList filenames {QStringLiteral("applications.pdf"), QStringLiteral("concepts.pdf"),
        QStringLiteral("ideas.pdf"), QStringLiteral("course-notes.pdf"),
        QStringLiteral("thoughts.pdf"), QStringLiteral("Benchmark-followup.pdf")};
    QJsonArray historicalFailures;
    QJsonArray currentFailures;
    for (const QString &filename : filenames) {
        historicalFailures.append(QJsonObject {{QStringLiteral("pdf_filename"), filename},
            {QStringLiteral("message"), filename == QLatin1String("Benchmark-followup.pdf")
                ? QStringLiteral("latex_error: compilation failed") : QStringLiteral("project_build_failed: old error")}});
        if (filename != QLatin1String("applications.pdf")) {
            currentFailures.append(QJsonObject {{QStringLiteral("filename"), filename},
                {QStringLiteral("build_id"), aggregateId}, {QStringLiteral("reason"), QStringLiteral("Needs attention")}});
        }
    }
    const QJsonObject oldEvent {{QStringLiteral("state"), QStringLiteral("running")},
        {QStringLiteral("error_code"), QStringLiteral("pdf_build_pending")},
        {QStringLiteral("message"), QStringLiteral("applications.pdf: latex_error: old failure")},
        {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("build"), QJsonObject {
            {QStringLiteral("id"), aggregateId}, {QStringLiteral("target"), QStringLiteral("all")},
            {QStringLiteral("status"), QStringLiteral("attention")},
            {QStringLiteral("result"), QJsonObject {{QStringLiteral("failed_documents"), historicalFailures}}}}}}}};
    const auto checkpoint = [](const QJsonArray &failures) {
        return QJsonObject {{QStringLiteral("origin"), QStringLiteral("desktop")},
            {QStringLiteral("phase"), QStringLiteral("downloading_updates")},
            {QStringLiteral("metadata"), QJsonObject {
                {QStringLiteral("sync_outcome"), QStringLiteral("changes_synced")},
                {QStringLiteral("failed_documents"), failures}}}};
    };
    QJsonObject operation {{QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("status"), QStringLiteral("failed")}, {QStringLiteral("phase"), QStringLiteral("failed")},
        {QStringLiteral("last_error_code"), QStringLiteral("pdf_build_failed")},
        {QStringLiteral("last_error_message"), QStringLiteral("applications.pdf: old failure")},
        {QStringLiteral("events"), QJsonArray {oldEvent, checkpoint(currentFailures)}}};
    auto summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
    QVERIFY(summary.message.startsWith(QStringLiteral("Your changes are synced · 5 projects failed to build.\n\n")));
    QVERIFY(!summary.message.contains(QStringLiteral("applications")));
    QVERIFY(summary.message.contains(QStringLiteral("concepts\n")));
    QVERIFY(summary.message.contains(QStringLiteral("Benchmark-followup\nA formatting error")));
    QCOMPARE(summary.message.count(QStringLiteral("This project could not be updated.")), 4);

    // A failure from a different build must not lend its old diagnosis to a
    // still-failing project; use the final checkpoint's current reason instead.
    QJsonArray newerFailure {QJsonObject {{QStringLiteral("filename"), QStringLiteral("Benchmark-followup.pdf")},
        {QStringLiteral("build_id"), QStringLiteral("new-build")},
        {QStringLiteral("reason"), QStringLiteral("project attachment is unavailable: new-reference.pdf")}}};
    operation.insert(QStringLiteral("events"), QJsonArray {oldEvent, checkpoint(currentFailures), checkpoint(newerFailure)});
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
    QCOMPARE(summary.message, QStringLiteral("Your changes are synced · 1 project failed to build.\n\nBenchmark-followup\nMissing attachment: new-reference.pdf"));

    // An explicit empty final list clears old named failures after recovery.
    operation.insert(QStringLiteral("status"), QStringLiteral("succeeded"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("succeeded"));
    operation.remove(QStringLiteral("last_error_code"));
    operation.remove(QStringLiteral("last_error_message"));
    operation.insert(QStringLiteral("events"), QJsonArray {oldEvent, checkpoint(QJsonArray())});
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("Up to date"));
    QVERIFY(summary.message.isEmpty());

    // Completing PDF reconciliation does not clear source/provider errors.
    const QJsonObject providerEvent {{QStringLiteral("metadata"), QJsonObject {{QStringLiteral("errors"), QJsonArray {
        QJsonObject {{QStringLiteral("project"), QStringLiteral("docs-applications")},
            {QStringLiteral("error"), QStringLiteral("Conflicting source changes")}}}}}}};
    operation.insert(QStringLiteral("events"), QJsonArray {oldEvent, providerEvent, checkpoint(QJsonArray())});
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("Sync failed"));
    QVERIFY(summary.message.contains(QStringLiteral("applications\nConflicting source changes need to be resolved.")));

    // Legacy journals and server/intermediate events are not final local
    // checkpoints. Absence of a failure list must never stand in for recovery.
    operation.insert(QStringLiteral("status"), QStringLiteral("failed"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("failed"));
    operation.insert(QStringLiteral("last_error_code"), QStringLiteral("pdf_build_failed"));
    operation.insert(QStringLiteral("events"), QJsonArray {oldEvent});
    QVERIFY(Thoughts::syncOperationOutcome(operation).message.contains(QStringLiteral("applications\n")));
    QJsonObject serverCheckpoint = checkpoint(QJsonArray());
    serverCheckpoint.insert(QStringLiteral("origin"), QStringLiteral("server"));
    operation.insert(QStringLiteral("events"), QJsonArray {oldEvent, serverCheckpoint});
    QVERIFY(Thoughts::syncOperationOutcome(operation).message.contains(QStringLiteral("applications\n")));
    QJsonObject intermediate = checkpoint(QJsonArray());
    intermediate.insert(QStringLiteral("phase"), QStringLiteral("building"));
    operation.insert(QStringLiteral("events"), QJsonArray {oldEvent, intermediate});
    QVERIFY(Thoughts::syncOperationOutcome(operation).message.contains(QStringLiteral("applications\n")));
    operation.insert(QStringLiteral("status"), QStringLiteral("running"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("downloading_updates"));
    operation.insert(QStringLiteral("events"), QJsonArray {oldEvent, checkpoint(QJsonArray())});
    QVERIFY(Thoughts::syncOperationOutcome(operation).message.contains(QStringLiteral("applications\n")));

    // A failed run with no identified project remains a visible generic error.
    operation.insert(QStringLiteral("status"), QStringLiteral("failed"));
    operation.insert(QStringLiteral("phase"), QStringLiteral("failed"));
    operation.insert(QStringLiteral("events"), QJsonArray {checkpoint(QJsonArray())});
    summary = Thoughts::syncOperationOutcome(operation);
    QCOMPARE(summary.title, QStringLiteral("PDF build failed"));
    QVERIFY(summary.message.startsWith(QStringLiteral("Your changes are synced.\n\n")));
    QVERIFY(summary.message.contains(QStringLiteral("affected project was not recorded")));
}

void CommentsOverviewTest::testDocumentSyncActivityPresentation()
{
    const qint64 now = 100000;
    const QJsonObject otherDocument{{QStringLiteral("kind"), QStringLiteral("sync")},
                                    {QStringLiteral("target"), QStringLiteral("other.pdf")},
                                    {QStringLiteral("status"), QStringLiteral("Synced")},
                                    {QStringLiteral("created_at_ms"), 99000},
                                    {QStringLiteral("updated_at_ms"), 100000}};
    const QJsonObject build{{QStringLiteral("kind"), QStringLiteral("build")},
                            {QStringLiteral("target"), QStringLiteral("paper.pdf")},
                            {QStringLiteral("status"), QStringLiteral("succeeded")},
                            {QStringLiteral("phase"), QStringLiteral("succeeded")},
                            {QStringLiteral("created_at_ms"), 99500},
                            {QStringLiteral("updated_at_ms"), 100000}};
    QJsonObject running{{QStringLiteral("kind"), QStringLiteral("sync")},
                        {QStringLiteral("target"), QStringLiteral("paper.pdf")},
                        {QStringLiteral("status"), QStringLiteral("running")},
                        {QStringLiteral("phase"), QStringLiteral("downloading")},
                        {QStringLiteral("created_at_ms"), 97000},
                        {QStringLiteral("updated_at_ms"), 97000}};

    QCOMPARE(Thoughts::latestDocumentSyncOperation(QJsonArray{otherDocument, build, running}, QStringLiteral("paper.pdf")), running);
    QCOMPARE(Thoughts::latestDocumentSyncOperation(QJsonArray{running}, QStringLiteral("other.pdf")), QJsonObject());
    QCOMPARE(Thoughts::syncOperationDisplayStatus(build), QStringLiteral("Built"));
    QCOMPARE(Thoughts::syncOperationDisplayStatus(running), QStringLiteral("Syncing…"));
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(build), QStringLiteral("Build complete"));
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(running), QStringLiteral("Syncing"));
    QCOMPARE(Thoughts::syncOperationElapsedLabel(running, now), QStringLiteral("3 s"));
    QJsonObject queued = running;
    queued.insert(QStringLiteral("phase"), QStringLiteral("queued"));
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(queued), QStringLiteral("Waiting to sync"));
    const QJsonObject legacyQueued {{QStringLiteral("kind"), QStringLiteral("sync")},
                                    {QStringLiteral("status"), QStringLiteral("Queued")}};
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(legacyQueued), QStringLiteral("Waiting to sync"));
    QJsonObject failedBuild = build;
    failedBuild.insert(QStringLiteral("status"), QStringLiteral("failed"));
    failedBuild.insert(QStringLiteral("phase"), QStringLiteral("failed"));
    QCOMPARE(Thoughts::syncOperationDisplayStatus(failedBuild), QStringLiteral("Sync failed"));
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(failedBuild), QStringLiteral("Sync failed"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(QJsonObject(), now), QStringLiteral("Never synced"));
    QCOMPARE(Thoughts::syncElapsedSecondsLabel(1), QStringLiteral("1 second elapsed"));
    QCOMPARE(Thoughts::syncElapsedSecondsLabel(553), QStringLiteral("553 seconds elapsed"));
    const QJsonObject noPendingChanges {{QStringLiteral("available"), true},
                                        {QStringLiteral("source_state"), QStringLiteral("clean")},
                                        {QStringLiteral("total"), 0}};
    QCOMPARE(Thoughts::pendingSyncActivityLabel(noPendingChanges), QStringLiteral("No pending changes"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now), QStringLiteral("Syncing…"));
    QCOMPARE(Thoughts::documentSyncActivityDetailLabel(running, now), QStringLiteral("3 seconds elapsed"));
    QCOMPARE(Thoughts::syncActivityPresentation(running, noPendingChanges, now).status, QStringLiteral("Syncing…"));
    QCOMPARE(Thoughts::syncActivityPresentation(running, noPendingChanges, now).detail, QStringLiteral("3 seconds elapsed"));

    running.insert(QStringLiteral("phase"), QStringLiteral("waiting_source_publication"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now), QStringLiteral("Waiting to publish sources…"));
    QCOMPARE(Thoughts::documentSyncActivityDetailLabel(running, now), QStringLiteral("3 seconds elapsed"));
    QCOMPARE(Thoughts::syncOperationDurationMs(running), -1);

    running.insert(QStringLiteral("status"), QStringLiteral("succeeded"));
    running.insert(QStringLiteral("phase"), QStringLiteral("succeeded"));
    running.insert(QStringLiteral("updated_at_ms"), now);
    QCOMPARE(Thoughts::syncOperationDisplayStatus(running), QStringLiteral("Synced"));
    QCOMPARE(Thoughts::syncOperationDurationMs(running), 3000);
    QCOMPARE(Thoughts::syncOperationDurationLabel(running), QStringLiteral("3.0 s"));
    QCOMPARE(Thoughts::syncOperationElapsedLabel(running, now + 5000), QStringLiteral("3 s"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now), QStringLiteral("Synced just now"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 12000), QStringLiteral("Synced just now"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 59000), QStringLiteral("Synced just now"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 60000), QStringLiteral("Synced 1m ago"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 61000), QStringLiteral("Synced 1m ago"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 4 * 60 * 1000), QStringLiteral("Synced 4m ago"));

    QJsonObject interruptedSync = running;
    interruptedSync.insert(QStringLiteral("status"), QStringLiteral("running"));
    interruptedSync.insert(QStringLiteral("phase"), QStringLiteral("downloading"));
    interruptedSync.remove(QStringLiteral("completed_at_ms"));
    interruptedSync.remove(QStringLiteral("duration_ms"));
    QJsonObject interruptedBuild = interruptedSync;
    interruptedBuild.insert(QStringLiteral("kind"), QStringLiteral("build"));
    interruptedBuild.insert(QStringLiteral("phase"), QStringLiteral("building"));
    interruptedBuild.insert(QStringLiteral("build_id"), QStringLiteral("accepted-build"));
    const QJsonArray reconciled = Thoughts::reconcileSyncOperationsAfterRestart(QJsonArray {interruptedSync, interruptedBuild, build}, now + 5000);
    QCOMPARE(reconciled.at(0).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("interrupted"));
    QCOMPARE(reconciled.at(1).toObject().value(QStringLiteral("status")).toString(), QStringLiteral("interrupted"));
    QCOMPARE(reconciled.at(0).toObject().value(QStringLiteral("phase")).toString(), QStringLiteral("interrupted"));
    QCOMPARE(reconciled.at(0).toObject().value(QStringLiteral("completed_at_ms")).toVariant().toLongLong(), now + 5000);
    const QJsonArray interruptedEvents = reconciled.at(0).toObject().value(QStringLiteral("events")).toArray();
    QCOMPARE(interruptedEvents.at(interruptedEvents.size() - 1).toObject().value(QStringLiteral("message")).toString(), QStringLiteral("Interrupted when Thoughts restarted"));
    QCOMPARE(Thoughts::syncOperationFailureMessage(reconciled.at(0).toObject()), QStringLiteral("Interrupted when Thoughts restarted"));
    QCOMPARE(reconciled.at(2).toObject(), build);

    running.insert(QStringLiteral("status"), QStringLiteral("failed"));
    running.insert(QStringLiteral("phase"), QStringLiteral("failed"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 59000), QStringLiteral("Sync failed · just now"));
    QCOMPARE(Thoughts::syncOperationHistoryLabel(running, now + 59000), QStringLiteral("Last attempt: just now"));
    const Thoughts::SyncActivityPresentation failedActivity = Thoughts::syncActivityPresentation(running, noPendingChanges, now + 59000);
    QCOMPARE(failedActivity.status, QStringLiteral("Sync failed"));
    QCOMPARE(failedActivity.detail, QStringLiteral("Last attempt: just now"));
    QJsonObject compilerFailure = running;
    compilerFailure.insert(QStringLiteral("last_error_code"), QStringLiteral("pdf_build_failed"));
    QCOMPARE(Thoughts::syncOperationDisplayStatus(compilerFailure), QStringLiteral("PDF build failed"));
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(compilerFailure), QStringLiteral("PDF build failed"));
    QCOMPARE(Thoughts::syncActivityPresentation(compilerFailure, noPendingChanges, now).status, QStringLiteral("PDF build failed"));
    compilerFailure.insert(QStringLiteral("last_error_code"), QStringLiteral("build_failed"));
    QCOMPARE(Thoughts::syncOperationDisplayStatus(compilerFailure), QStringLiteral("PDF build failed"));
    QCOMPARE(Thoughts::syncOperationHistoryLabel(compilerFailure, now), QStringLiteral("Last attempt: just now"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(compilerFailure, now), QStringLiteral("PDF build failed · just now"));
    QCOMPARE(Thoughts::syncOperationLifecycleTitle(compilerFailure), QStringLiteral("PDF build failed"));
    // A build request can fail to contact the server; its kind is not proof of a build error.
    compilerFailure.insert(QStringLiteral("kind"), QStringLiteral("build"));
    compilerFailure.insert(QStringLiteral("last_error_code"), QStringLiteral("sync_failed"));
    QCOMPARE(Thoughts::syncOperationDisplayStatus(compilerFailure), QStringLiteral("Sync failed"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 60000), QStringLiteral("Sync failed · 1m ago"));
    QCOMPARE(Thoughts::documentSyncActivityLabel(running, now + 2 * 60 * 60 * 1000), QStringLiteral("Sync failed · 2h ago"));

    QJsonObject measured{{QStringLiteral("status"), QStringLiteral("succeeded")},
                         {QStringLiteral("phase"), QStringLiteral("succeeded")},
                         {QStringLiteral("duration_ms"), 221}};
    QCOMPARE(Thoughts::syncOperationDurationLabel(measured), QStringLiteral("221 ms"));
    QCOMPARE(Thoughts::syncOperationElapsedLabel(measured, now), QStringLiteral("0 s"));
    measured.insert(QStringLiteral("duration_ms"), 21655);
    QCOMPARE(Thoughts::syncOperationDurationLabel(measured), QStringLiteral("21.7 s"));
    QCOMPARE(Thoughts::syncOperationElapsedLabel(measured, now), QStringLiteral("21 s"));
    measured.insert(QStringLiteral("duration_ms"), 72500);
    QCOMPARE(Thoughts::syncOperationDurationLabel(measured), QStringLiteral("1 min 12 s"));
    QCOMPARE(Thoughts::syncOperationElapsedLabel(measured, now), QStringLiteral("1 min 12 s"));
    measured.insert(QStringLiteral("duration_ms"), 3780000);
    QCOMPARE(Thoughts::syncOperationDurationLabel(measured), QStringLiteral("1 h 3 min"));

    measured.insert(QStringLiteral("status"), QStringLiteral("running"));
    measured.insert(QStringLiteral("phase"), QStringLiteral("building"));
    QCOMPARE(Thoughts::syncOperationDurationMs(measured), -1);
    QCOMPARE(Thoughts::syncOperationDurationLabel(measured), QString());

    measured.insert(QStringLiteral("phase"), QStringLiteral("waiting_source_publication"));
    QCOMPARE(Thoughts::syncOperationDurationMs(measured), -1);
    QCOMPARE(Thoughts::syncOperationDurationLabel(measured), QString());

    measured.insert(QStringLiteral("status"), QStringLiteral("succeeded"));
    measured.insert(QStringLiteral("phase"), QStringLiteral("succeeded"));
    measured.insert(
        QStringLiteral("events"),
        QJsonArray{
            QJsonObject{{QStringLiteral("at_ms"), 1000}, {QStringLiteral("message"), QStringLiteral("Sync started")}},
            QJsonObject{{QStringLiteral("at_ms"), 1100}, {QStringLiteral("message"), QStringLiteral("Syncing…")}},
            QJsonObject{{QStringLiteral("at_ms"), 1200}, {QStringLiteral("message"), QStringLiteral("Syncing…")}},
            QJsonObject{{QStringLiteral("at_ms"), 22655},
                        {QStringLiteral("message"), QStringLiteral("Completed")},
                        {QStringLiteral("duration_ms"), 21655}},
        });
    const QStringList logLines = Thoughts::syncOperationLogLines(measured);
    QCOMPARE(logLines.size(), 3);
    QVERIFY(logLines.at(0).endsWith(QStringLiteral("Sync started · 100 ms")));
    QVERIFY(logLines.at(1).endsWith(QStringLiteral("Syncing…")));
    QVERIFY(logLines.at(2).endsWith(QStringLiteral("Completed · 21.7 s")));
    const QRegularExpression timestampedLine(QStringLiteral("^\\d{2}\\.\\d{2}\\.\\d{4} \\d{2}:\\d{2}:\\d{2} \\S+ · "));
    for (const QString &line : logLines) {
        QVERIFY2(timestampedLine.match(line).hasMatch(), qPrintable(line));
    }
    QVERIFY(logLines.at(0).contains(QStringLiteral(" · Starting · ")));

    const QJsonObject readableSourceLog {
        {QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("started_at_ms"), 1000},
        {QStringLiteral("events"),
         QJsonArray {
             QJsonObject {{QStringLiteral("at_ms"), 1000},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("queued")},
                          {QStringLiteral("message"), QStringLiteral("Sync started")}},
             QJsonObject {{QStringLiteral("at_ms"), 1200},
                          {QStringLiteral("phase"), QStringLiteral("preparing")},
                          {QStringLiteral("message"), QStringLiteral("The managed source manifest contains 183 path(s).")}},
             QJsonObject {{QStringLiteral("at_ms"), 1500},
                          {QStringLiteral("phase"), QStringLiteral("checking_server")},
                          {QStringLiteral("message"), QStringLiteral("Scheduling verified frozen source deletion: docs-paper/tex/fragments/old.tex")}},
             QJsonObject {{QStringLiteral("at_ms"), 1600},
                          {QStringLiteral("phase"), QStringLiteral("uploading")},
                          {QStringLiteral("message"), QStringLiteral("Uploading source change docs-paper/tex/source/main.tex (14.9 kB).")},
                          {QStringLiteral("total_bytes"), 15260},
                          {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("path"), QStringLiteral("docs-paper/tex/source/main.tex")}}}},
             QJsonObject {{QStringLiteral("at_ms"), 1800},
                          {QStringLiteral("phase"), QStringLiteral("uploading")},
                          {QStringLiteral("message"), QStringLiteral("Transfer progress: 15.9 kB uploaded; 0 B downloaded.")},
                          {QStringLiteral("metadata"), QJsonObject {{QStringLiteral("uploaded_bytes"), 16260}}}},
             QJsonObject {{QStringLiteral("at_ms"), 1801},
                          {QStringLiteral("phase"), QStringLiteral("uploading")},
                          {QStringLiteral("message"), QStringLiteral("Scheduling frozen source update: docs-paper/tex/source/main.tex")}},
             QJsonObject {{QStringLiteral("at_ms"), 2500},
                          {QStringLiteral("phase"), QStringLiteral("uploading")},
                          {QStringLiteral("message"), QStringLiteral("Publication complete at source frontier 123: 1 updated, 1 deleted, 0 unchanged.")}},
             QJsonObject {{QStringLiteral("at_ms"), 2600},
                          {QStringLiteral("origin"), QStringLiteral("server")},
                          {QStringLiteral("component"), QStringLiteral("api")},
                          {QStringLiteral("phase"), QStringLiteral("build_queued")},
                          {QStringLiteral("message"), QStringLiteral("Fedora accepted build build-1.")}},
             QJsonObject {{QStringLiteral("at_ms"), 2601},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("build_queued")},
                          {QStringLiteral("message"), QStringLiteral("Fedora accepted build build-1.")}},
         }},
    };
    const QList<Thoughts::SyncLocalChange> readableChanges = Thoughts::syncOperationLocalChanges(readableSourceLog);
    QCOMPARE(readableChanges.size(), 2);
    QCOMPARE(readableChanges.at(0).kind, Thoughts::SyncLocalChangeKind::Deleted);
    QCOMPARE(readableChanges.at(0).path, QStringLiteral("docs-paper/tex/fragments/old.tex"));
    QCOMPARE(Thoughts::syncLocalChangeDisplayPath(readableChanges, readableChanges.at(0)), QStringLiteral("tex/fragments/old.tex"));
    QCOMPARE(readableChanges.at(1).kind, Thoughts::SyncLocalChangeKind::Modified);
    QCOMPARE(readableChanges.at(1).sizeBytes, 15260);
    QCOMPARE(Thoughts::syncLocalChangeDisplayPath(readableChanges, readableChanges.at(1)), QStringLiteral("tex/source/main.tex"));
    const QString readableActivity = Thoughts::syncOperationActivityLines(readableSourceLog).join(QLatin1Char('\n'));
    QVERIFY(readableActivity.contains(QStringLiteral("+0 sec · Starting · Sync started")));
    QVERIFY(readableActivity.contains(QStringLiteral("Uploaded 2 local changes")));
    QVERIFY(readableActivity.contains(QStringLiteral("15.9 KiB")));
    QCOMPARE(readableActivity.count(QStringLiteral("Fedora accepted build build-1.")), 1);
    QVERIFY(!readableActivity.contains(QStringLiteral("managed source manifest")));
    QVERIFY(!readableActivity.contains(QStringLiteral("Uploading source change")));
    QVERIFY(!readableActivity.contains(QStringLiteral("Transfer progress")));
    QVERIFY(!readableActivity.contains(timestampedLine));
    const QString copiedReadableLog = Thoughts::syncOperationReadableLog(readableSourceLog);
    QVERIFY(copiedReadableLog.contains(QStringLiteral("LOCAL CHANGES · 2 FILES")));
    QVERIFY(copiedReadableLog.contains(QStringLiteral("D  tex/fragments/old.tex")));
    QVERIFY(copiedReadableLog.contains(QStringLiteral("M  tex/source/main.tex")));
    QVERIFY(copiedReadableLog.contains(QStringLiteral("SYNC ACTIVITY")));

    // A second local check starts shortly after the first but owns a new,
    // expensive interval. It must survive de-duplication across other phases.
    const QJsonObject repeatedLocalCheck {
        {QStringLiteral("started_at_ms"), 1000},
        {QStringLiteral("status"), QStringLiteral("succeeded")},
        {QStringLiteral("completed_at_ms"), 41000},
        {QStringLiteral("events"), QJsonArray {
            QJsonObject {{QStringLiteral("at_ms"), 9000}, {QStringLiteral("phase"), QStringLiteral("checking_local_changes")},
                         {QStringLiteral("message"), QStringLiteral("Checking local changes")}},
            QJsonObject {{QStringLiteral("at_ms"), 9400}, {QStringLiteral("phase"), QStringLiteral("installing_sources")},
                         {QStringLiteral("message"), QStringLiteral("Installing sources")}},
            QJsonObject {{QStringLiteral("at_ms"), 10000}, {QStringLiteral("phase"), QStringLiteral("checking_local_changes")},
                         {QStringLiteral("message"), QStringLiteral("Checking local changes")}},
            QJsonObject {{QStringLiteral("at_ms"), 40700}, {QStringLiteral("phase"), QStringLiteral("checking")},
                         {QStringLiteral("message"), QStringLiteral("Checking PDFs")}},
        }},
    };
    const QString repeatedActivity = Thoughts::syncOperationActivityLines(repeatedLocalCheck).join(QLatin1Char('\n'));
    QCOMPARE(repeatedActivity.count(QStringLiteral("Checking local changes")), 2);
    QVERIFY(repeatedActivity.contains(QStringLiteral("Checking local changes · 30.7 s")));

    const QJsonObject measuredPhaseVocabulary {
        {QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("status"), QStringLiteral("running")},
        {QStringLiteral("phase"), QStringLiteral("building")},
        {QStringLiteral("events"),
         QJsonArray {
             QJsonObject {{QStringLiteral("at_ms"), 1000},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("checking_local_changes")},
                          {QStringLiteral("message"), QStringLiteral("Checking local changes")},
                          {QStringLiteral("duration_ms"), QJsonValue::Null}},
             QJsonObject {{QStringLiteral("at_ms"), 121000},
                          {QStringLiteral("origin"), QStringLiteral("server")},
                          {QStringLiteral("component"), QStringLiteral("renderer")},
                          {QStringLiteral("phase"), QStringLiteral("checking")},
                          {QStringLiteral("message"), QStringLiteral("Validating build inputs")},
                          {QStringLiteral("duration_ms"), QJsonValue::Null}},
             QJsonObject {{QStringLiteral("at_ms"), 121500},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("checking")},
                          {QStringLiteral("message"), QStringLiteral("Checking whether the server needs to update any PDFs")},
                          {QStringLiteral("duration_ms"), 0}},
             QJsonObject {{QStringLiteral("at_ms"), 136000},
                          {QStringLiteral("origin"), QStringLiteral("server")},
                          {QStringLiteral("component"), QStringLiteral("renderer")},
                          {QStringLiteral("phase"), QStringLiteral("preparing_comments")},
                          {QStringLiteral("message"), QStringLiteral("Preparing comment sidecars")}},
             QJsonObject {{QStringLiteral("at_ms"), 136500},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("building")},
                          {QStringLiteral("message"), QStringLiteral("Waiting for the PDF update on the server")}},
             QJsonObject {{QStringLiteral("at_ms"), 151000},
                          {QStringLiteral("origin"), QStringLiteral("server")},
                          {QStringLiteral("component"), QStringLiteral("renderer")},
                          {QStringLiteral("phase"), QStringLiteral("compiling")},
                          {QStringLiteral("message"), QStringLiteral("Compiling documentation projects")},
                          {QStringLiteral("duration_ms"), 7250}},
             QJsonObject {{QStringLiteral("at_ms"), 152000},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("checking_server")},
                          {QStringLiteral("message"), QStringLiteral("Confirming the Library state")},
                          {QStringLiteral("duration_ms"), QJsonValue::Null}},
         }},
    };
    const QStringList measuredPhaseLines = Thoughts::syncOperationLogLines(measuredPhaseVocabulary);
    QCOMPARE(measuredPhaseLines.size(), 7);
    QVERIFY(measuredPhaseLines.at(0).endsWith(QStringLiteral("Checking local changes · 2 min")));
    QVERIFY(measuredPhaseLines.at(1).endsWith(QStringLiteral("Validating build inputs · 15.0 s")));
    QVERIFY(measuredPhaseLines.at(2).endsWith(QStringLiteral("Checking whether the server needs to update any PDFs · 15.0 s")));
    QVERIFY(measuredPhaseLines.at(3).endsWith(QStringLiteral("Preparing comment sidecars · 15.0 s")));
    QVERIFY(measuredPhaseLines.at(4).endsWith(QStringLiteral("Waiting for the PDF update on the server · 15.5 s")));
    QVERIFY(measuredPhaseLines.at(5).endsWith(QStringLiteral("Compiling documentation projects · 7.3 s")));
    QVERIFY(measuredPhaseLines.at(6).endsWith(QStringLiteral("Confirming the Library state")));
    for (const QString &line : measuredPhaseLines) {
        QVERIFY2(!line.endsWith(QStringLiteral("0 ms")), qPrintable(line));
    }

    QJsonObject duplicateNullDuration = measuredPhaseVocabulary;
    duplicateNullDuration.insert(
        QStringLiteral("events"),
        QJsonArray {
            QJsonObject {{QStringLiteral("at_ms"), 1000},
                         {QStringLiteral("phase"), QStringLiteral("checking_server")},
                         {QStringLiteral("message"), QStringLiteral("Checking server")},
                         {QStringLiteral("duration_ms"), QJsonValue::Null}},
            QJsonObject {{QStringLiteral("at_ms"), 1001},
                         {QStringLiteral("phase"), QStringLiteral("checking_server")},
                         {QStringLiteral("message"), QStringLiteral("Checking server")},
                         {QStringLiteral("duration_ms"), QJsonValue::Null}},
        });
    const QStringList duplicateNullDurationLines = Thoughts::syncOperationLogLines(duplicateNullDuration);
    QCOMPARE(duplicateNullDurationLines.size(), 1);
    QVERIFY(!duplicateNullDurationLines.constFirst().contains(QStringLiteral(" ms")));

    const QJsonObject componentVocabulary {
        {QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("status"), QStringLiteral("running")},
        {QStringLiteral("events"),
         QJsonArray {
             QJsonObject {{QStringLiteral("at_ms"), 1000},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("preparing")},
                          {QStringLiteral("message"), QStringLiteral("Discovering local changes")}},
             QJsonObject {{QStringLiteral("at_ms"), 1100},
                          {QStringLiteral("origin"), QStringLiteral("server")},
                          {QStringLiteral("component"), QStringLiteral("api")},
                          {QStringLiteral("phase"), QStringLiteral("building")},
                          {QStringLiteral("message"), QStringLiteral("Fedora accepted the build")}},
         }},
    };
    const QStringList componentLines = Thoughts::syncOperationLogLines(componentVocabulary);
    QCOMPARE(componentLines.size(), 2);
    QVERIFY(componentLines.at(0).contains(QStringLiteral(" · Preparing local changes · Discovering local changes")));
    QVERIFY(!componentLines.at(0).contains(QStringLiteral("Desktop")));
    QVERIFY(componentLines.at(1).contains(QStringLiteral(" · Api · Building affected PDFs · Fedora accepted the build")));

    const QJsonObject legacyVocabulary {
        {QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("status"), QStringLiteral("succeeded")},
        {QStringLiteral("phase"), QStringLiteral("succeeded")},
        {QStringLiteral("events"),
         QJsonArray {
             QJsonObject {{QStringLiteral("at_ms"), 1000},
                          {QStringLiteral("phase"), QStringLiteral("Queued")},
                          {QStringLiteral("message"), QStringLiteral("Queued")}},
             QJsonObject {{QStringLiteral("at_ms"), 1100},
                          {QStringLiteral("phase"), QStringLiteral("Preflight")},
                          {QStringLiteral("message"), QStringLiteral("Preflight started")}},
             QJsonObject {{QStringLiteral("at_ms"), 1200},
                          {QStringLiteral("phase"), QStringLiteral("downloading")},
                          {QStringLiteral("message"), QStringLiteral("Offline copy")}},
         }},
    };
    const QString legacyVisibleLog = Thoughts::syncOperationLogLines(legacyVocabulary).join(QLatin1Char('\n'));
    QVERIFY(legacyVisibleLog.contains(QStringLiteral("Starting")));
    QVERIFY(legacyVisibleLog.contains(QStringLiteral("Checking server")));
    QVERIFY(legacyVisibleLog.contains(QStringLiteral("Saved log")));
    QVERIFY(!legacyVisibleLog.contains(QStringLiteral("Queued"), Qt::CaseInsensitive));
    QVERIFY(!legacyVisibleLog.contains(QStringLiteral("Preflight"), Qt::CaseInsensitive));
    QVERIFY(!legacyVisibleLog.contains(QStringLiteral("Offline copy"), Qt::CaseInsensitive));

    QJsonObject legacyTransfer = running;
    legacyTransfer.insert(QStringLiteral("bytes_transferred"), 0);
    legacyTransfer.insert(QStringLiteral("total_bytes"), 0);
    QCOMPARE(Thoughts::syncOperationUploadedLabel(legacyTransfer), QStringLiteral("Not recorded"));
    QCOMPARE(Thoughts::syncOperationDownloadedLabel(legacyTransfer), QStringLiteral("Not recorded"));
    QCOMPARE(Thoughts::syncOperationServerStreamLabel(legacyTransfer), QStringLiteral("Saved log"));

    QJsonObject measuredTransfer = running;
    measuredTransfer.insert(QStringLiteral("status"), QStringLiteral("running"));
    measuredTransfer.insert(QStringLiteral("phase"), QStringLiteral("downloading"));
    measuredTransfer.insert(QStringLiteral("transfer_metrics_recorded"), true);
    measuredTransfer.insert(QStringLiteral("uploaded_completed_bytes"), 0);
    measuredTransfer.insert(QStringLiteral("uploaded_total_bytes"), 0);
    measuredTransfer.insert(QStringLiteral("downloaded_completed_bytes"), 512);
    measuredTransfer.insert(QStringLiteral("downloaded_total_bytes"), 1024);
    QCOMPARE(Thoughts::syncOperationUploadedLabel(measuredTransfer), QStringLiteral("0 B"));
    QVERIFY(Thoughts::syncOperationDownloadedLabel(measuredTransfer).contains(QStringLiteral(" of ")));
    measuredTransfer.insert(QStringLiteral("status"), QStringLiteral("succeeded"));
    measuredTransfer.insert(QStringLiteral("phase"), QStringLiteral("succeeded"));
    QVERIFY(!Thoughts::syncOperationDownloadedLabel(measuredTransfer).contains(QStringLiteral(" of ")));
    measuredTransfer.insert(QStringLiteral("stream_state"), QStringLiteral("live"));
    QCOMPARE(Thoughts::syncOperationServerStreamLabel(measuredTransfer), QStringLiteral("Live"));
    measuredTransfer.insert(QStringLiteral("stream_state"), QStringLiteral("reconnecting"));
    QCOMPARE(Thoughts::syncOperationServerStreamLabel(measuredTransfer), QStringLiteral("Reconnecting"));

    const QJsonObject detailedFailure {
        {QStringLiteral("status"), QStringLiteral("Error")},
        {QStringLiteral("phase"), QStringLiteral("failed")},
        {QStringLiteral("events"),
         QJsonArray {
             QJsonObject {{QStringLiteral("at_ms"), 1000},
                          {QStringLiteral("phase"), QStringLiteral("uploading")},
                          {QStringLiteral("state"), QStringLiteral("failed")},
                          {QStringLiteral("message"),
                           QStringLiteral("Uploading build input docs-Benchmark-Conf/tex/generated/data/classical_baseline_results.json failed: The write operation timed out. The server build was not started.")},
                          {QStringLiteral("error_code"), QStringLiteral("build_input_upload_timeout")}},
             QJsonObject {{QStringLiteral("at_ms"), 1100},
                          {QStringLiteral("phase"), QStringLiteral("uploading")},
                          {QStringLiteral("state"), QStringLiteral("failed")},
                          {QStringLiteral("message"),
                           QStringLiteral("Uploading build input docs-Benchmark-Conf/tex/generated/data/classical_baseline_results.json failed: The write operation timed out. The server build was not started.")},
                          {QStringLiteral("error_code"), QStringLiteral("build_input_upload_timeout")}},
         }},
    };
    QCOMPARE(Thoughts::syncOperationFailureMessage(detailedFailure),
             QStringLiteral("Uploading build input docs-Benchmark-Conf/tex/generated/data/classical_baseline_results.json failed: The write operation timed out. The server build was not started."));
    QCOMPARE(Thoughts::syncOperationErrorCode(detailedFailure), QStringLiteral("build_input_upload_timeout"));
    const QStringList failureLines = Thoughts::syncOperationLogLines(detailedFailure);
    QCOMPARE(failureLines.size(), 1);
    QVERIFY(timestampedLine.match(failureLines.constFirst()).hasMatch());
    QVERIFY(failureLines.constFirst().contains(QStringLiteral(" · Uploading local changes · ")));
    QVERIFY(failureLines.constFirst().contains(QStringLiteral("The server build was not started.")));
    QVERIFY(failureLines.constFirst().endsWith(QStringLiteral("[build_input_upload_timeout]")));

    const QString unusedInputPath = QStringLiteral(
        "docs-course-notes/tex/generated/figures/distillation_outcomes/matched_token_control_by_position_correct_control.pdf");
    const QString unusedInputLog = QStringLiteral("e556d21b48e4e96f03969078b3f942a1c541a5773850001fc2271e5bacac02c4");
    const QString unusedInputRaw = QStringLiteral("renderer exited with 1: unused_tex_input: %1; log object %2")
                                       .arg(unusedInputPath, unusedInputLog);
    const QJsonObject unusedInputFailure {
        {QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("phase"), QStringLiteral("failed")},
        {QStringLiteral("started_at_ms"), 1000},
        {QStringLiteral("last_error_code"), QStringLiteral("sync_failed")},
        {QStringLiteral("last_error_message"), unusedInputRaw},
        {QStringLiteral("events"),
         QJsonArray {
             QJsonObject {{QStringLiteral("at_ms"), 1621},
                          {QStringLiteral("origin"), QStringLiteral("desktop")},
                          {QStringLiteral("component"), QStringLiteral("desktop")},
                          {QStringLiteral("phase"), QStringLiteral("failed")},
                          {QStringLiteral("state"), QStringLiteral("failed")},
                          {QStringLiteral("message"), unusedInputRaw},
                          {QStringLiteral("error_code"), QStringLiteral("sync_failed")},
                          {QStringLiteral("duration_ms"), 621}},
             QJsonObject {{QStringLiteral("at_ms"), 1622},
                          {QStringLiteral("origin"), QStringLiteral("server")},
                          {QStringLiteral("component"), QStringLiteral("api")},
                          {QStringLiteral("phase"), QStringLiteral("build_reused")},
                          {QStringLiteral("state"), QStringLiteral("failed")},
                          {QStringLiteral("message"), QStringLiteral("Reusing Fedora build c539ab8e-caa3-4f18-932a-ceb8832e0249.")}},
         }},
    };
    const QString unusedInputMessage = Thoughts::syncOperationFailureMessage(unusedInputFailure);
    QVERIFY(unusedInputMessage.contains(QStringLiteral("PDF update stopped because no TeX source uses the generated file")));
    QVERIFY(unusedInputMessage.contains(unusedInputPath));
    QVERIFY(unusedInputMessage.contains(QStringLiteral("rejecting an Agent Edit")));
    QVERIFY(unusedInputMessage.contains(QStringLiteral("TeX change was saved")));
    QVERIFY(unusedInputMessage.contains(unusedInputLog));
    QVERIFY(!unusedInputMessage.contains(QStringLiteral("renderer exited with")));
    QCOMPARE(Thoughts::syncOperationErrorCode(unusedInputFailure), QStringLiteral("unused_tex_input"));
    const QString unusedInputActivity = Thoughts::syncOperationActivityLines(unusedInputFailure).join(QLatin1Char('\n'));
    QVERIFY(unusedInputActivity.contains(QStringLiteral("PDF update stopped because no TeX source uses the generated file")));
    QVERIFY(unusedInputActivity.contains(QStringLiteral("[unused_tex_input]")));
    QVERIFY(!unusedInputActivity.contains(QStringLiteral("[sync_failed]")));
    QVERIFY(unusedInputActivity.contains(
        QStringLiteral("The previous failed server build c539ab8e-caa3-4f18-932a-ceb8832e0249 was reused; no new build was run.")));
    // Older servers say "Reusing Fedora build" (above) or "Reusing server build".
    QJsonObject newerServerReuse = unusedInputFailure;
    QJsonArray newerServerEvents = newerServerReuse.value(QStringLiteral("events")).toArray();
    QJsonObject reuseEvent = newerServerEvents.last().toObject();
    reuseEvent.insert(QStringLiteral("message"), QStringLiteral("Reusing server build c539ab8e-caa3-4f18-932a-ceb8832e0249."));
    newerServerEvents.replace(newerServerEvents.size() - 1, reuseEvent);
    newerServerReuse.insert(QStringLiteral("events"), newerServerEvents);
    const QString newerServerActivity = Thoughts::syncOperationActivityLines(newerServerReuse).join(QLatin1Char('\n'));
    QVERIFY(newerServerActivity.contains(
        QStringLiteral("The previous failed server build c539ab8e-caa3-4f18-932a-ceb8832e0249 was reused; no new build was run.")));
    QVERIFY(!newerServerActivity.contains(QStringLiteral("Reusing server build")));
    // The server now names itself so in the trace; journals saved before keep the form above.
    QJsonObject serverWordedFailure = unusedInputFailure;
    QJsonArray serverWordedEvents = serverWordedFailure.value(QStringLiteral("events")).toArray();
    QJsonObject serverWordedReuse = serverWordedEvents.at(1).toObject();
    serverWordedReuse.insert(QStringLiteral("message"), QStringLiteral("Reusing the server's build 7d2f0c1e-0b4e-4a3c-9f21-5e8d6a7b9c10."));
    serverWordedEvents.replace(1, serverWordedReuse);
    serverWordedFailure.insert(QStringLiteral("events"), serverWordedEvents);
    const QString serverWordedActivity = Thoughts::syncOperationActivityLines(serverWordedFailure).join(QLatin1Char('\n'));
    QVERIFY2(serverWordedActivity.contains(
                 QStringLiteral("The previous failed server build 7d2f0c1e-0b4e-4a3c-9f21-5e8d6a7b9c10 was reused; no new build was run.")),
             qPrintable(serverWordedActivity));

    const QString emptyDocumentLog = QStringLiteral("ae75cce44b058b28f3d66bf9d9591a77550db3ce53dc16f1ee42540433a7f0f4");
    const QString emptyDocumentRaw = QStringLiteral(
        "renderer exited with 1: concepts.pdf — empty_document: "
        "Source docs-concepts/tex/source/release.tex produced no pages; "
        "it remains listed in docs-concepts/tex/docs_order_manifest.txt; "
        "add renderable content or remove it from the manifest; latexmk exit 12; log object %1")
                                         .arg(emptyDocumentLog);
    const QJsonObject emptyDocumentFailure {
        {QStringLiteral("kind"), QStringLiteral("sync")},
        {QStringLiteral("status"), QStringLiteral("failed")},
        {QStringLiteral("phase"), QStringLiteral("failed")},
        {QStringLiteral("started_at_ms"), 1000},
        {QStringLiteral("last_error_code"), QStringLiteral("sync_failed")},
        {QStringLiteral("last_error_message"), emptyDocumentRaw},
        {QStringLiteral("events"),
         QJsonArray {QJsonObject {{QStringLiteral("at_ms"), 21000},
                                 {QStringLiteral("origin"), QStringLiteral("server")},
                                 {QStringLiteral("component"), QStringLiteral("build_worker")},
                                 {QStringLiteral("phase"), QStringLiteral("failed")},
                                 {QStringLiteral("state"), QStringLiteral("failed")},
                                 {QStringLiteral("message"), emptyDocumentRaw},
                                 {QStringLiteral("error_code"), QStringLiteral("compiler_failure")}}}},
    };
    const QString emptyDocumentMessage = Thoughts::syncOperationFailureMessage(emptyDocumentFailure);
    QVERIFY(emptyDocumentMessage.startsWith(QStringLiteral("concepts.pdf failed.")));
    QVERIFY(emptyDocumentMessage.contains(QStringLiteral("Source: “docs-concepts/tex/source/release.tex”")));
    QVERIFY(emptyDocumentMessage.contains(QStringLiteral("Cause: Empty document — LaTeX produced no pages.")));
    QVERIFY(emptyDocumentMessage.contains(QStringLiteral("docs-concepts/tex/docs_order_manifest.txt")));
    QVERIFY(emptyDocumentMessage.contains(QStringLiteral("Possible fixes: Add renderable content or remove it from the manifest.")));
    QVERIFY(emptyDocumentMessage.contains(QStringLiteral("Technical details: latexmk exit 12.")));
    QVERIFY(emptyDocumentMessage.contains(emptyDocumentLog));
    QVERIFY(!emptyDocumentMessage.contains(QStringLiteral("renderer exited with")));
    QCOMPARE(Thoughts::syncOperationErrorCode(emptyDocumentFailure), QStringLiteral("empty_document"));
    const QString emptyDocumentActivity = Thoughts::syncOperationActivityLines(emptyDocumentFailure).join(QLatin1Char('\n'));
    QVERIFY(emptyDocumentActivity.startsWith(QStringLiteral("+20 sec · Build worker · Failed · concepts.pdf failed.")));
    QVERIFY(emptyDocumentActivity.endsWith(QStringLiteral("[empty_document]")));

    const QString localSourceSummary = QStringLiteral("The complete source dependency closure could not be frozen safely.");
    const QString localSourceDetail = QStringLiteral(
        "Local source data stopped responding while reading current_pdfs/.thoughts-source-index.json. "
        "Make sure the Library folder is available locally, then choose Retry.");
    QCOMPARE(Thoughts::syncOperationFailureJournalMessage(localSourceSummary, localSourceDetail),
             localSourceSummary + QLatin1Char(' ') + localSourceDetail);
    QCOMPARE(Thoughts::syncOperationFailureJournalMessage(QString(), localSourceDetail), localSourceDetail);
    QCOMPARE(Thoughts::syncOperationFailureJournalMessage(localSourceSummary, localSourceSummary), localSourceSummary);

    const QJsonObject buildProgress{
        {QStringLiteral("status"), QStringLiteral("building")},
        {QStringLiteral("build"),
         QJsonObject{{QStringLiteral("progress"),
                      QJsonObject{{QStringLiteral("phase"), QStringLiteral("compiling")},
                                  {QStringLiteral("completed"), 3},
                                  {QStringLiteral("total"), 17},
                                  {QStringLiteral("unit"), QStringLiteral("projects")},
                                  {QStringLiteral("detail"), QStringLiteral("  Compiled\n docs-sdpo  ")}}}}},
    };
    QCOMPARE(Thoughts::fedoraBuildProgressEvent(buildProgress), QStringLiteral("Compiled docs-sdpo · 3/17"));
    QCOMPARE(Thoughts::fedoraBuildProgressEvent(QJsonObject{}), QString());
}

void CommentsOverviewTest::testPlainTextMarkupIsNotAComment()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    QByteArray records;
    const QStringList subtypes {QStringLiteral("Highlight"), QStringLiteral("Underline"), QStringLiteral("Squiggly"), QStringLiteral("StrikeOut")};
    for (qsizetype index = 0; index < subtypes.size(); ++index) {
        QJsonObject markup = comment(QStringLiteral("markup-%1").arg(index), index == 0 ? QString() : QStringLiteral(" \n\t "), static_cast<int>(index + 1), QStringLiteral("2026-08-08T08:00:00Z"), QStringLiteral("anchored"));
        markup.insert(QStringLiteral("subtype"), subtypes.at(index));
        records += jsonLine(markup);
        QVERIFY(!Okular::DocsCommentSynchronizer::isTrackedCommentRecord(markup));
    }
    QJsonObject reviewedHighlight = comment(QStringLiteral("reviewed-highlight"), QStringLiteral("Actual review text"), 5, QStringLiteral("2026-08-08T09:00:00Z"), QStringLiteral("anchored"));
    reviewedHighlight.insert(QStringLiteral("subtype"), QStringLiteral("Highlight"));
    records += jsonLine(reviewedHighlight);
    QVERIFY(Okular::DocsCommentSynchronizer::isTrackedCommentRecord(reviewedHighlight));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), records));

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.constFirst().comments.size(), 1);
    QCOMPARE(documents.constFirst().comments.constFirst().id, QStringLiteral("reviewed-highlight"));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 1);
}

void CommentsOverviewTest::testPriorityLocationLoadingEditingAndFiltering()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    QJsonObject paris = comment(QStringLiteral("paris"), QStringLiteral("High in Paris"), 1, QStringLiteral("2026-08-05T08:00:00Z"), QStringLiteral("anchored"));
    paris.insert(QStringLiteral("priority"), QStringLiteral("high"));
    paris.insert(QStringLiteral("location"),
                 QJsonObject {
                     {QStringLiteral("label"), QStringLiteral("Paris")},
                     {QStringLiteral("latitude"), 48.8566},
                     {QStringLiteral("longitude"), 2.3522},
                     {QStringLiteral("google_place_id"), QStringLiteral("paris-place")},
                     {QStringLiteral("source"), QStringLiteral("google_maps")},
                 });
    QJsonObject london = comment(QStringLiteral("london"), QStringLiteral("Normal in London"), 2, QStringLiteral("2026-08-05T09:00:00Z"), QStringLiteral("anchored"));
    london.insert(QStringLiteral("priority"), QStringLiteral("normal"));
    london.insert(QStringLiteral("location"), QJsonObject {{QStringLiteral("label"), QStringLiteral("London")}, {QStringLiteral("latitude"), 51.5074}, {QStringLiteral("longitude"), -0.1278}});
    QJsonObject unknown = comment(QStringLiteral("unknown"), QStringLiteral("Unknown metadata"), 3, QStringLiteral("2026-08-05T10:00:00Z"), QStringLiteral("anchored"));
    unknown.insert(QStringLiteral("priority"), QStringLiteral("urgent"));
    unknown.insert(QStringLiteral("location"), QJsonObject {{QStringLiteral("label"), QStringLiteral("Broken")}});
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(paris) + jsonLine(london) + jsonLine(unknown)));

    KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("CommentsOverview"));
    config.deleteGroup();
    config.writeEntry("LocationFilterActive", true);
    config.writeEntry("LocationFilterLabel", QStringLiteral("Paris"));
    config.writeEntry("LocationFilterLatitude", 48.8566);
    config.writeEntry("LocationFilterLongitude", 2.3522);
    config.writeEntry("LocationFilterRadiusKm", 10);
    config.sync();

    const QList<Thoughts::TrackedCommentDocument> documents = Thoughts::loadTrackedCommentDocuments(root);
    QCOMPARE(documents.size(), 1);
    QCOMPARE(documents.constFirst().comments.size(), 3);
    QCOMPARE(documents.constFirst().comments.at(0).priorityRaw, QStringLiteral("high"));
    QVERIFY(documents.constFirst().comments.at(0).locationValid);
    QCOMPARE(documents.constFirst().comments.at(0).location.value(QStringLiteral("google_place_id")).toString(), QStringLiteral("paris-place"));
    QVERIFY(documents.constFirst().comments.at(2).locationPresent);
    QVERIFY(!documents.constFirst().comments.at(2).locationValid);

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 3);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *locationEnabled = overview.findChild<QCheckBox *>(QStringLiteral("commentsOverviewLocationEnabled"));
    auto *locationLabel = overview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewLocationLabel"));
    auto *saveButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewSave"));
    QVERIFY(table);
    QVERIFY(locationEnabled);
    QVERIFY(locationLabel);
    QVERIFY(saveButton);
    QVERIFY(saveButton->isHidden());
    QCOMPARE(table->model()->columnCount(), 10);
    // A layout saved before the Source doc column shows it right after Comment, once.
    QCOMPARE(table->header()->visualIndex(9), table->header()->visualIndex(0) + 1);
    // Settings of the removed location filter no longer hide comments and are dropped on save.
    QCOMPARE(table->model()->rowCount(), 3);
    QCOMPARE(table->model()->headerData(4, Qt::Horizontal).toString(), QStringLiteral("Source"));

    overview.close();
    QVERIFY(!config.hasKey("LocationFilterActive"));
    QVERIFY(!config.hasKey("LocationFilterRadiusKm"));
    config.deleteGroup();
    config.sync();

    Thoughts::CommentsOverview editableOverview(root);
    editableOverview.show();
    QTRY_COMPARE(editableOverview.commentCount(), 3);
    table = editableOverview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    locationEnabled = editableOverview.findChild<QCheckBox *>(QStringLiteral("commentsOverviewLocationEnabled"));
    locationLabel = editableOverview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewLocationLabel"));
    saveButton = editableOverview.findChild<QPushButton *>(QStringLiteral("commentsOverviewSave"));
    auto *search = editableOverview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewSearch"));
    QVERIFY(search);
    search->setText(QStringLiteral("High in Paris"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(locationEnabled->isChecked());
    QCOMPARE(locationLabel->text(), QStringLiteral("Paris"));

    QSignalSpy editRequested(&editableOverview, &Thoughts::CommentsOverview::commentEditRequested);
    locationLabel->setText(QStringLiteral("Paris centre"));
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 1, 1000);
    const QVariantMap patch = editRequested.constFirst().at(4).toMap();
    const QVariantMap editedLocation = patch.value(QStringLiteral("location")).toMap();
    QCOMPARE(editedLocation.value(QStringLiteral("label")).toString(), QStringLiteral("Paris centre"));
    QCOMPARE(editedLocation.value(QStringLiteral("google_place_id")).toString(), QStringLiteral("paris-place"));
    editableOverview.completeOperation(Thoughts::CommentsOverview::EditOperation,
                                       root + QStringLiteral("/current_pdfs/alpha.pdf"),
                                       QStringLiteral("paris"),
                                       QVariantMap {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    QTRY_VERIFY(locationEnabled->isChecked());
    locationEnabled->setChecked(false);
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 2, 1000);
    const QVariantMap clearPatch = editRequested.at(1).at(4).toMap();
    QVERIFY(clearPatch.contains(QStringLiteral("location")));
    QVERIFY(clearPatch.value(QStringLiteral("location")).isNull());
    editableOverview.completeOperation(Thoughts::CommentsOverview::EditOperation,
                                       root + QStringLiteral("/current_pdfs/alpha.pdf"),
                                       QStringLiteral("paris"),
                                       QVariantMap {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    search->clear();
    editableOverview.close();
    config.deleteGroup();
    config.sync();
}

void CommentsOverviewTest::testAtomicMetadataUpdatePreservesUnknownFields()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QJsonObject record = comment(QStringLiteral("metadata"), QStringLiteral("Keep me"), 1, QStringLiteral("2026-08-05T08:00:00Z"), QStringLiteral("anchored"));
    record.insert(QStringLiteral("priority"), QStringLiteral("urgent"));
    record.insert(QStringLiteral("location"), QJsonObject {{QStringLiteral("label"), QStringLiteral("Legacy")}});
    record.insert(QStringLiteral("future_field"), QJsonObject {{QStringLiteral("nested"), 7}});
    record.insert(QStringLiteral("anchor_id"), QStringLiteral("metadata"));
    QByteArray sourceText;
    for (int line = 1; line < 11; ++line) {
        sourceText += "Source line\n";
    }
    sourceText += "\\CommentAnchor{metadata}{Keep me}%\n";
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/tex/source/main.tex"), sourceText));
    QVERIFY(QDir().mkpath(root + QStringLiteral("/comments/.state")));
    const QString sidecar = root + QStringLiteral("/comments/alpha_comments.jsonl");
    QVERIFY(writeFile(sidecar, jsonLine(record)));

    const QJsonObject newLocation {
        {QStringLiteral("label"), QStringLiteral("Paris")},
        {QStringLiteral("latitude"), 48.8566},
        {QStringLiteral("longitude"), 2.3522},
        {QStringLiteral("source"), QStringLiteral("manual")},
    };
    const auto changed = Okular::DocsCommentSynchronizer::updateSidecarComment(root,
                                                                                QStringLiteral("alpha"),
                                                                                QStringLiteral("metadata"),
                                                                                QStringLiteral("Edited instantly"),
                                                                                QJsonObject {{QStringLiteral("priority"), QStringLiteral("high")}, {QStringLiteral("location"), newLocation}});
    QVERIFY2(changed.success, qPrintable(changed.failureReason));
    QVERIFY(changed.changed);
    QVERIFY(changed.sourceMirrorUpdated);
    QCOMPARE(changed.previousContents, QStringLiteral("Keep me"));
    QCOMPARE(changed.currentContents, QStringLiteral("Edited instantly"));
    QCOMPARE(changed.previousMetadata.value(QStringLiteral("priority")).toString(), QStringLiteral("urgent"));

    QFile file(sidecar);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject stored = QJsonDocument::fromJson(file.readLine()).object();
    QCOMPARE(stored.value(QStringLiteral("contents")).toString(), QStringLiteral("Edited instantly"));
    QCOMPARE(stored.value(QStringLiteral("priority")).toString(), QStringLiteral("high"));
    QCOMPARE(stored.value(QStringLiteral("location")).toObject(), newLocation);
    QCOMPARE(stored.value(QStringLiteral("future_field")).toObject().value(QStringLiteral("nested")).toInt(), 7);
    QCOMPARE(stored.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(stored.value(QStringLiteral("origin_id")).toString(), QStringLiteral("mac-overview"));
    file.close();
    QFile source(root + QStringLiteral("/docs-test/tex/source/main.tex"));
    QVERIFY(source.open(QIODevice::ReadOnly));
    QVERIFY(source.readAll().contains("\\CommentAnchor{metadata}{Edited instantly}"));
    source.close();

    const auto restored = Okular::DocsCommentSynchronizer::updateSidecarComment(root,
                                                                                 QStringLiteral("alpha"),
                                                                                 QStringLiteral("metadata"),
                                                                                 changed.previousContents,
                                                                                 changed.previousMetadata);
    QVERIFY2(restored.success, qPrintable(restored.failureReason));
    QVERIFY(file.open(QIODevice::ReadOnly));
    stored = QJsonDocument::fromJson(file.readLine()).object();
    QCOMPARE(stored.value(QStringLiteral("contents")).toString(), QStringLiteral("Keep me"));
    QCOMPARE(stored.value(QStringLiteral("priority")).toString(), QStringLiteral("urgent"));
    QCOMPARE(stored.value(QStringLiteral("location")).toObject().value(QStringLiteral("label")).toString(), QStringLiteral("Legacy"));
    QCOMPARE(stored.value(QStringLiteral("future_field")).toObject().value(QStringLiteral("nested")).toInt(), 7);
}

void CommentsOverviewTest::testChatGptUrlValidationAndDatePreservingUpdate()
{
    QString error;
    QCOMPARE(Okular::DocsCommentSynchronizer::normalizedChatGptUrl(QStringLiteral(" https://chatgpt.com/share/example "), &error),
             QStringLiteral("https://chatgpt.com/share/example"));
    QVERIFY(error.isEmpty());
    QCOMPARE(Okular::DocsCommentSynchronizer::normalizedChatGptUrl(QStringLiteral("https://chatgpt.com/c/example"), &error),
             QStringLiteral("https://chatgpt.com/c/example"));
    QVERIFY(error.isEmpty());
    QVERIFY(Okular::DocsCommentSynchronizer::normalizedChatGptUrl(QStringLiteral("http://chatgpt.com/share/example"), &error).isEmpty());
    QVERIFY(!error.isEmpty());
    QVERIFY(Okular::DocsCommentSynchronizer::normalizedChatGptUrl(QStringLiteral("https://example.com/chatgpt"), &error).isEmpty());
    QVERIFY(!error.isEmpty());

    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString sidecar = root + QStringLiteral("/comments/alpha_comments.jsonl");
    QJsonObject record = comment(QStringLiteral("chatgpt"), QStringLiteral("Keep the comment date"), 1, QStringLiteral("2026-08-05T08:00:00.000Z"), QStringLiteral("anchored"));
    record.insert(QStringLiteral("created"), QStringLiteral("D:20260801070000Z"));
    record.insert(QStringLiteral("modified"), QStringLiteral("D:20260802070000Z"));
    record.insert(QStringLiteral("origin_id"), QStringLiteral("phone-origin"));
    record.insert(QStringLiteral("future_field"), QJsonObject {{QStringLiteral("nested"), 9}});
    QVERIFY(writeFile(sidecar, jsonLine(record)));

    const QString chatGptUrl = QStringLiteral("https://chatgpt.com/share/01234567-89ab-cdef-0123-456789abcdef");
    const auto stored = Okular::DocsCommentSynchronizer::updateSidecarCommentChatGptUrl(root, QStringLiteral("alpha"), QStringLiteral("chatgpt"), chatGptUrl);
    QVERIFY2(stored.success, qPrintable(stored.failureReason));
    QVERIFY(stored.changed);
    QCOMPARE(stored.previousMetadata.value(QStringLiteral("chatgpt_url")), QJsonValue(QJsonValue::Null));
    QCOMPARE(stored.currentMetadata.value(QStringLiteral("chatgpt_url")).toString(), chatGptUrl);

    QFile file(sidecar);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QJsonObject updated = QJsonDocument::fromJson(file.readLine()).object();
    file.close();
    QCOMPARE(updated.value(QStringLiteral("chatgpt_url")).toString(), chatGptUrl);
    QCOMPARE(updated.value(QStringLiteral("created")).toString(), QStringLiteral("D:20260801070000Z"));
    QCOMPARE(updated.value(QStringLiteral("modified")).toString(), QStringLiteral("D:20260802070000Z"));
    QCOMPARE(updated.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-08-05T08:00:00.000Z"));
    QCOMPARE(updated.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(updated.value(QStringLiteral("origin_id")).toString(), QStringLiteral("mac-overview"));
    QCOMPARE(updated.value(QStringLiteral("future_field")).toObject().value(QStringLiteral("nested")).toInt(), 9);

    const auto cleared = Okular::DocsCommentSynchronizer::updateSidecarCommentChatGptUrl(root, QStringLiteral("alpha"), QStringLiteral("chatgpt"), QString());
    QVERIFY2(cleared.success, qPrintable(cleared.failureReason));
    QVERIFY(cleared.changed);
    QVERIFY(file.open(QIODevice::ReadOnly));
    updated = QJsonDocument::fromJson(file.readLine()).object();
    file.close();
    QVERIFY(!updated.contains(QStringLiteral("chatgpt_url")));
    QCOMPARE(updated.value(QStringLiteral("created")).toString(), QStringLiteral("D:20260801070000Z"));
    QCOMPARE(updated.value(QStringLiteral("modified")).toString(), QStringLiteral("D:20260802070000Z"));
    QCOMPARE(updated.value(QStringLiteral("updated_at")).toString(), QStringLiteral("2026-08-05T08:00:00.000Z"));
    QCOMPARE(updated.value(QStringLiteral("revision")).toInt(), 3);

    const auto invalid = Okular::DocsCommentSynchronizer::updateSidecarCommentChatGptUrl(root,
                                                                                          QStringLiteral("alpha"),
                                                                                          QStringLiteral("chatgpt"),
                                                                                          QStringLiteral("https://example.com/not-chatgpt"));
    QVERIFY(!invalid.success);
    QVERIFY(!invalid.failureReason.isEmpty());
}

void CommentsOverviewTest::testOrphanArchive()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString commentsRoot = root + QStringLiteral("/comments");

    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/active.pdf"), QByteArrayLiteral("placeholder")));
    const QByteArray active = jsonLine(comment(QStringLiteral("active"), QStringLiteral("Keep active"), 1, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("anchored")));
    QVERIFY(writeFile(commentsRoot + QStringLiteral("/active_comments.jsonl"), active));
    QVERIFY(writeFile(commentsRoot + QStringLiteral("/empty_comments.jsonl"), QByteArray()));

    QJsonObject tombstone = comment(QStringLiteral("removed"), QStringLiteral("Old deleted comment"), 2, QStringLiteral("2026-07-30T09:00:00Z"), QStringLiteral("anchored"));
    tombstone.insert(QStringLiteral("deleted"), true);
    tombstone.insert(QStringLiteral("deleted_at"), QStringLiteral("2026-07-30T09:30:00Z"));
    const QByteArray tombstones = jsonLine(tombstone);
    QVERIFY(writeFile(commentsRoot + QStringLiteral("/tombstones_comments.jsonl"), tombstones));
    QVERIFY(writeFile(commentsRoot + QStringLiteral("/live_comments.jsonl"), jsonLine(comment(QStringLiteral("live"), QStringLiteral("Keep orphan live"), 3, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored")))));
    QVERIFY(writeFile(commentsRoot + QStringLiteral("/invalid_comments.jsonl"), QByteArrayLiteral("not-json\n")));

    const QByteArray existingArchive = QByteArrayLiteral("existing archive\n");
    QVERIFY(writeFile(commentsRoot + QStringLiteral("/.archive/previous/history.jsonl"), existingArchive));

    const Thoughts::OrphanArchiveResult result = Thoughts::archiveOrphanCommentSidecars(root);
    QCOMPARE(result.archived, 4);
    QVERIFY(result.errors.isEmpty());

    QVERIFY(QFileInfo::exists(commentsRoot + QStringLiteral("/active_comments.jsonl")));
    QVERIFY(!QFileInfo::exists(commentsRoot + QStringLiteral("/empty_comments.jsonl")));
    QVERIFY(!QFileInfo::exists(commentsRoot + QStringLiteral("/tombstones_comments.jsonl")));
    QVERIFY(!QFileInfo::exists(commentsRoot + QStringLiteral("/live_comments.jsonl")));
    QVERIFY(!QFileInfo::exists(commentsRoot + QStringLiteral("/invalid_comments.jsonl")));

    QCOMPARE(readFile(commentsRoot + QStringLiteral("/.archive/previous/history.jsonl")), existingArchive);

    const auto verifyArchive = [&commentsRoot](const QString &stem, const QByteArray &expected, bool parseValid, int records, int liveRecords) {
        const QDir stemArchive(commentsRoot + QStringLiteral("/.archive/") + stem);
        const QStringList snapshots = stemArchive.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QCOMPARE(snapshots.size(), 1);
        const QString snapshotRoot = stemArchive.filePath(snapshots.constFirst());
        QCOMPARE(readFile(QDir(snapshotRoot).filePath(stem + QStringLiteral("_comments.jsonl"))), expected);
        const QJsonObject manifest = QJsonDocument::fromJson(readFile(QDir(snapshotRoot).filePath(QStringLiteral("manifest.json")))).object();
        QCOMPARE(manifest.value(QStringLiteral("schema_version")).toInt(), 1);
        QCOMPARE(manifest.value(QStringLiteral("stem")).toString(), stem);
        QCOMPARE(manifest.value(QStringLiteral("reason")).toString(), QStringLiteral("orphaned_comment_history"));
        QCOMPARE(manifest.value(QStringLiteral("readable")).toBool(), true);
        QCOMPARE(manifest.value(QStringLiteral("parse_valid")).toBool(), parseValid);
        QCOMPARE(manifest.value(QStringLiteral("records")).toInt(), records);
        QCOMPARE(manifest.value(QStringLiteral("live_records")).toInt(), liveRecords);
        QCOMPARE(manifest.value(QStringLiteral("sha256")).toString(), QString::fromLatin1(QCryptographicHash::hash(expected, QCryptographicHash::Sha256).toHex()));
    };
    verifyArchive(QStringLiteral("empty"), QByteArray(), true, 0, 0);
    verifyArchive(QStringLiteral("tombstones"), tombstones, true, 1, 0);
    verifyArchive(QStringLiteral("live"), jsonLine(comment(QStringLiteral("live"), QStringLiteral("Keep orphan live"), 3, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored"))), true, 1, 1);
    verifyArchive(QStringLiteral("invalid"), QByteArrayLiteral("not-json\n"), false, 0, 0);
}

void CommentsOverviewTest::testWatcherDoesNotSelfTrigger()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/active.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/active_comments.jsonl"), jsonLine(comment(QStringLiteral("active"), QStringLiteral("Stable comment"), 1, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("anchored")))));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 1);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(table);

    QTest::qWait(600);
    QSignalSpy rowsInserted(table->model(), &QAbstractItemModel::rowsInserted);
    QSignalSpy rowsRemoved(table->model(), &QAbstractItemModel::rowsRemoved);
    QTest::qWait(800);
    QCOMPARE(rowsInserted.size(), 0);
    QCOMPARE(rowsRemoved.size(), 0);
}

void CommentsOverviewTest::testFilteringActivationAndRefresh()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/beta.json"), QJsonDocument(layout(QStringLiteral("beta"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/beta.pdf"), QByteArrayLiteral("placeholder")));

    const QString sidecar = root + QStringLiteral("/comments/alpha_comments.jsonl");
    QByteArray comments;
    comments += jsonLine(comment(QStringLiteral("older"), QStringLiteral("Older comment"), 1, QStringLiteral("2026-07-29T08:00:00Z"), QStringLiteral("anchored")));
    comments += jsonLine(comment(QStringLiteral("newer"), QStringLiteral("Needle comment"), 2, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("needs_anchor")));
    QVERIFY(writeFile(sidecar, comments));

    Thoughts::CommentsOverview dashboard(root);
    dashboard.show();
    QTRY_COMPARE(dashboard.documentCount(), 2);
    QTRY_COMPARE(dashboard.commentCount(), 2);

    auto *search = dashboard.findChild<QLineEdit *>(QStringLiteral("commentsOverviewSearch"));
    auto *documents = dashboard.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    auto *table = dashboard.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(search);
    QVERIFY(documents);
    QVERIFY(table);
    QSignalSpy modelReset(table->model(), &QAbstractItemModel::modelReset);
    // All items, No project, Projects and PDFs, the Projects heading, and one
    // container for both PDFs, which belong to no Library project.
    QCOMPARE(documents->topLevelItemCount(), 5);
    QCOMPARE(table->model()->rowCount(), 2);
    QCOMPARE(table->header()->logicalIndex(0), 7);
    QCOMPARE(table->model()->headerData(7, Qt::Horizontal).toString(), QStringLiteral("Created"));
    QCOMPARE(table->model()->index(0, 7).data().toString(),
             QDateTime::fromString(QStringLiteral("2026-07-30T08:00:00Z"), Qt::ISODate).toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Needle comment"));

    search->setText(QStringLiteral("needle"));
    QTRY_COMPARE(table->model()->rowCount(), 1);
    search->clear();
    documents->setCurrentItem(documentItem(documents, QStringLiteral("beta")));
    QTRY_COMPARE(dashboard.selectedDocument(), QStringLiteral("beta"));
    QTRY_COMPARE(table->model()->rowCount(), 0);
    documents->setCurrentItem(documentItem(documents, QStringLiteral("alpha")));
    QTRY_COMPARE(table->model()->rowCount(), 2);

    QSignalSpy activated(&dashboard, &Thoughts::CommentsOverview::commentActivated);
    const QModelIndex newerIndex = table->model()->index(0, 0);
    QVERIFY(QMetaObject::invokeMethod(table, "doubleClicked", Q_ARG(QModelIndex, newerIndex)));
    QTRY_COMPARE(activated.size(), 1);
    QCOMPARE(activated.constFirst().at(1).toString(), QStringLiteral("newer"));
    QCOMPARE(activated.constFirst().at(2).toInt(), 2);

    comments += jsonLine(comment(QStringLiteral("latest"), QStringLiteral("Latest comment"), 3, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored")));
    QVERIFY(writeFile(sidecar, comments));
    QTRY_COMPARE_WITH_TIMEOUT(dashboard.commentCount(), 3, 3000);
    QTRY_COMPARE(table->model()->rowCount(), 3);
    QCOMPARE(table->model()->index(0, 0).data().toString(), QStringLiteral("Latest comment"));
    QCOMPARE(modelReset.size(), 0);
}

void CommentsOverviewTest::testDoubleClickNavigationWaitsForRelease()
{
    QTemporaryDir temporary;
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QByteArray comments;
    for (int i = 0; i < 8; ++i) {
        comments += jsonLine(comment(QString::number(i), QStringLiteral("Comment %1").arg(i), 1, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("anchored")));
    }
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), comments));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 8);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    const auto matches = documents->findItems(QStringLiteral("alpha"), Qt::MatchExactly | Qt::MatchRecursive, 0);
    QVERIFY(!matches.isEmpty());
    documents->setCurrentItem(matches.first());
    const QPersistentModelIndex first(table->model()->index(0, 0));
    const QPoint point = table->visualRect(first).center();
    QSignalSpy activated(&overview, &Thoughts::CommentsOverview::commentActivated);
    connect(&overview, &Thoughts::CommentsOverview::commentActivated, &overview, [&] { overview.hide(); });
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTest::mouseDClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTest::qWait(30);
    QCOMPARE(activated.count(), 0); // Loading a PDF must not steal the pending release.
    QTest::mouseRelease(table->viewport(), Qt::LeftButton, Qt::NoModifier, point);
    QTRY_COMPARE(activated.count(), 1);
    overview.show();
    QTest::mouseMove(table->viewport(), table->visualRect(table->model()->index(6, 0)).center());
    QTest::qWait(30);
    QCOMPARE(table->selectionModel()->selectedRows().size(), 1);
    QCOMPARE(table->currentIndex().row(), first.row());
    QCOMPARE(documents->currentItem(), matches.first());
    QVERIFY(overview.findChild<QWidget *>(QStringLiteral("commentsOverviewDetails"))->isVisible());
}

void CommentsOverviewTest::testShiftSelectsCommentRange()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    QByteArray comments;
    comments += jsonLine(comment(QStringLiteral("first"), QStringLiteral("First comment"), 1, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("anchored")));
    comments += jsonLine(comment(QStringLiteral("second"), QStringLiteral("Second comment"), 2, QStringLiteral("2026-07-30T09:00:00Z"), QStringLiteral("anchored")));
    comments += jsonLine(comment(QStringLiteral("third"), QStringLiteral("Third comment"), 3, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), comments));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 3);

    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(table);
    QCOMPARE(table->selectionMode(), QAbstractItemView::ExtendedSelection);

    const QModelIndex first = table->model()->index(0, 0);
    const QModelIndex third = table->model()->index(2, 0);
    QVERIFY(first.isValid());
    QVERIFY(third.isValid());
    table->scrollTo(first);
    // The first click opens the details panel, which narrows the table; aim at
    // the visible part of each row.
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(first).intersected(table->viewport()->rect()).center());
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::ShiftModifier, table->visualRect(third).intersected(table->viewport()->rect()).center());

    QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 3);

    auto *details = overview.findChild<QWidget *>(QStringLiteral("commentsOverviewDetails"));
    QVERIFY(details);
    QTRY_VERIFY(!details->isVisible());

    auto *contentSplitter = overview.findChild<QSplitter *>(QStringLiteral("commentsOverviewContentSplitter"));
    QVERIFY(contentSplitter);
    QCOMPARE(contentSplitter->orientation(), Qt::Horizontal);

    overview.reload();
    QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 3);
    QVERIFY(!details->isVisible());

    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(first).intersected(table->viewport()->rect()).center());
    QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 1);
    QTRY_VERIFY(details->isVisible());
}

void CommentsOverviewTest::testStartupCombinesLibraryLoads()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::DeveloperLog::Options options;
    options.directory = temporary.filePath(QStringLiteral("logs"));
    options.captureStacks = false;
    Thoughts::DeveloperLog log(options);
    Thoughts::CommentsOverview overview(temporary.path());
    overview.m_catalogPollTimer->setInterval(25);
    const QJsonObject note {{QStringLiteral("schema_version"), 1},
                            {QStringLiteral("comment_id"), QStringLiteral("startup-note")},
                            {QStringLiteral("thread_root_id"), QStringLiteral("startup-note")},
                            {QStringLiteral("contents"), QStringLiteral("Visible at startup")},
                            {QStringLiteral("subtype"), QStringLiteral("Note")},
                            {QStringLiteral("scope"), QJsonObject {{QStringLiteral("state"), QStringLiteral("standalone")}}}};
    overview.setFedoraProjections({{QStringLiteral("logical_comment\ncomments\nstartup-note"), note}});
    const auto queuedLoads = [] {
        int count = 0;
        for (const QJsonValue &value : Thoughts::DeveloperLog::performanceEvents()) {
            if (value.toObject().value(QStringLiteral("event")) == QLatin1String("library.reload.queued")) ++count;
        }
        return count;
    };
    QCOMPARE(queuedLoads(), 0);
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning && !overview.m_catalogSignature.isEmpty());
    QCOMPARE(overview.commentCount(), 1);
    QTest::qWait(200);
    QCOMPARE(queuedLoads(), 1);

    QVERIFY(writeFile(temporary.filePath(QStringLiteral("current_pdfs/.thoughts-generation.json")), QByteArrayLiteral("{}")));
    QTRY_VERIFY([&] {
        for (const QJsonValue &value : Thoughts::DeveloperLog::performanceEvents()) {
            if (value.toObject().value(QStringLiteral("event")) == QLatin1String("library.catalog_changed")) return true;
        }
        return false;
    }());
}

// After Sync the Library reloads with discovery, and the catalogue check that runs meanwhile sees
// the files Sync wrote (changed since its last baseline): it asked for a second full reload of the
// same catalogue, 3 to 12 s more each Sync (13 Nov). The reload that started from that catalogue
// already reads it, so none follows; a catalogue changed after it started still gets one.
void CommentsOverviewTest::testCatalogueChangeDuringAFullReloadOfIt()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    Thoughts::DeveloperLog::Options options;
    options.directory = temporary.filePath(QStringLiteral("logs"));
    options.captureStacks = false;
    Thoughts::DeveloperLog log(options);
    const auto eventCount = [](const QString &name) {
        int count = 0;
        for (const QJsonValue &value : Thoughts::DeveloperLog::performanceEvents()) {
            if (value.toObject().value(QStringLiteral("event")).toString() == name) ++count;
        }
        return count;
    };
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/paper.pdf"), QByteArrayLiteral("%PDF-paper")));
    QVERIFY(writeReviewCommentBundle(root, QStringLiteral("paper.pdf"), QJsonArray()));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    const auto settled = [&overview] {
        return !overview.m_fullReloadRunning && !overview.m_reloadTimer->isActive() && !overview.m_catalogSignatureCheckRunning
            && !overview.m_catalogPollTimer->isActive() && !overview.m_catalogSignature.isEmpty();
    };
    QTRY_VERIFY(settled());
    const QByteArray signature = overview.m_catalogSignature;
    const int loads = eventCount(QStringLiteral("library.reload.queued"));

    // Sync's end: a full reload with discovery; the check, against an older baseline, then reports
    // the catalogue that reload started from.
    overview.m_syncRefreshPending = true;
    overview.requestReload();
    QVERIFY(overview.m_fullReloadRunning);
    overview.m_catalogSignature = signature;
    overview.startFullReload(false, QStringLiteral("catalog_changed"));
    QTRY_VERIFY(settled());
    QTest::qWait(200);
    QCOMPARE(eventCount(QStringLiteral("library.reload.queued")), loads + 1);
    QCOMPARE(eventCount(QStringLiteral("library.reload.covered")), 1);
    QVERIFY(!overview.m_syncRefreshPending);

    // A catalogue changed after the reload started: read again.
    overview.m_syncRefreshPending = true;
    overview.requestReload();
    QVERIFY(overview.m_fullReloadRunning);
    overview.m_catalogSignature = QByteArrayLiteral("changed after the reload started");
    overview.startFullReload(false, QStringLiteral("catalog_changed"));
    QTRY_COMPARE(eventCount(QStringLiteral("library.reload.queued")), loads + 3);
    QCOMPARE(eventCount(QStringLiteral("library.reload.covered")), 1);
    QTRY_VERIFY(!overview.m_fullReloadRunning);
}

void CommentsOverviewTest::testSourceEditReloadsOnlyItsDocuments()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    Thoughts::DeveloperLog::Options options;
    options.directory = temporary.filePath(QStringLiteral("logs"));
    options.captureStacks = false;
    Thoughts::DeveloperLog log(options);
    const auto eventCount = [](const QString &name) {
        int count = 0;
        for (const QJsonValue &value : Thoughts::DeveloperLog::performanceEvents()) {
            if (value.toObject().value(QStringLiteral("event")).toString() == name) ++count;
        }
        return count;
    };
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/paper.pdf"), QByteArrayLiteral("%PDF-paper")));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/other.pdf"), QByteArrayLiteral("%PDF-other")));
    const QString source = QDir::cleanPath(root + QStringLiteral("/docs-test/tex/source/main.tex"));
    QVERIFY(writeFile(source, QByteArrayLiteral("\\section{Before}\n")));
    QVERIFY(writeReviewCommentBundle(root, QStringLiteral("paper.pdf"), QJsonArray()));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning && !overview.m_catalogSignature.isEmpty());
    // The show-time catalogue check learns which documents each source builds.
    QTRY_VERIFY_WITH_TIMEOUT(overview.m_documentsBySourceKnown, 5000);
    QCOMPARE(overview.m_documentsBySource.value(source), QStringList {QStringLiteral("paper.pdf")});
    QTRY_VERIFY(!overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive());
    const QByteArray signature = overview.m_catalogSignature;
    const int fullReloads = eventCount(QStringLiteral("library.reload.queued"));

    // Editing the source keeps the catalogue identity and re-reads only the
    // document published from it.
    QVERIFY(writeFile(source, QByteArrayLiteral("\\section{After}\n")));
    overview.handleDocsPathsChanged({source});
    QCOMPARE(overview.m_scheduledDocumentReloads, QSet<QString> {QStringLiteral("paper")});
    QTRY_VERIFY(overview.m_scheduledDocumentReloads.isEmpty() && overview.m_runningDocumentReloads.isEmpty());
    QTRY_VERIFY(!overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive());
    QCOMPARE(overview.m_catalogSignature, signature);
    QCOMPARE(eventCount(QStringLiteral("library.reload.queued")), fullReloads);
    QCOMPARE(eventCount(QStringLiteral("library.catalog_changed")), 0);

    // A source no published document uses reloads nothing.
    const QString unused = QDir::cleanPath(root + QStringLiteral("/docs-test/tex/source/unused.tex"));
    QVERIFY(writeFile(unused, QByteArrayLiteral("unused\n")));
    overview.handleDocsPathsChanged({unused});
    QVERIFY(overview.m_scheduledDocumentReloads.isEmpty());

    // Before the mapping is known, a source edit falls back to a full reload.
    overview.m_documentsBySourceKnown = false;
    overview.handleDocsPathsChanged({source});
    QTRY_COMPARE(eventCount(QStringLiteral("library.reload.queued")), fullReloads + 1);
}

static qint64 mainThreadCpuMicroseconds()
{
#ifdef Q_OS_WIN
    // Windows has no clock_gettime: GetThreadTimes reports kernel and user time in 100 ns units.
    FILETIME creation {}, exit {}, kernel {}, user {};
    if (!GetThreadTimes(GetCurrentThread(), &creation, &exit, &kernel, &user)) {
        return 0;
    }
    const auto ticks = [](const FILETIME &t) { return (qint64(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
    return (ticks(kernel) + ticks(user)) / 10;
#else
    timespec time {};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &time);
    return qint64(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
#endif
}

// Activity durations recorded at or after `since`, by event name.
static QHash<QString, qint64> libraryActivityMilliseconds(const QDateTime &since)
{
    QHash<QString, qint64> totals;
    for (const QJsonValue &value : Thoughts::DeveloperLog::performanceEvents()) {
        const QJsonObject entry = value.toObject();
        const QString event = entry.value(QStringLiteral("event")).toString();
        if (!event.endsWith(QLatin1String(".finished"))
            || QDateTime::fromString(entry.value(QStringLiteral("time")).toString(), Qt::ISODateWithMs) < since) continue;
        totals[event.chopped(9)] += entry.value(QStringLiteral("data")).toObject().value(QStringLiteral("duration_ms")).toInteger();
    }
    return totals;
}

struct LargeLibraryFixture {
    QList<Thoughts::TrackedCommentDocument> publications;
    QStringList sources; // per publication, absolute
    int sourceFolders = 0;
    int sourceFiles = 0;
};

// Shaped like the real Library on 2026-10-13: 25 projects, 230 documents,
// 45 source folders, ~906 source files. Projects, manifests and source files
// are on disk; the publications are returned for CommentsOverview::m_documents.
static LargeLibraryFixture writeLargeLibraryFixture(const QString &root)
{
    LargeLibraryFixture fixture;
    const auto uuid = [](const QString &name) { return QUuid::createUuidV5(QUuid(), name).toString(QUuid::WithoutBraces); };
    QJsonArray cached;
    const QDateTime builtAt = QDateTime::currentDateTimeUtc().addDays(-3);
    int documentNumber = 0;
    for (int p = 0; p < 25; ++p) {
        const QString projectId = QStringLiteral("bench-project-%1").arg(p, 2, 10, QLatin1Char('0'));
        const QString manifestPath = Thoughts::ProjectConfiguration::path(root, projectId);
        const QString defaultFolderId = uuid(projectId + QStringLiteral("/default"));
        QJsonArray folders {QJsonObject {{QStringLiteral("folder_id"), defaultFolderId},
                                         {QStringLiteral("path"), QDir(root).relativeFilePath(QFileInfo(manifestPath).path())},
                                         {QStringLiteral("document_id"), uuid(projectId + QStringLiteral("/default-document"))}}};
        QStringList owners, folderIds;
        for (int f = 0; f < (p < 20 ? 2 : 1); ++f) {
            const QString owner = QStringLiteral("docs-bench-%1-%2").arg(p, 2, 10, QLatin1Char('0')).arg(f);
            owners.append(owner);
            folderIds.append(uuid(owner));
            folders.append(QJsonObject {{QStringLiteral("folder_id"), folderIds.last()}, {QStringLiteral("path"), owner},
                                        {QStringLiteral("document_id"), uuid(owner + QStringLiteral("/document"))}});
            ++fixture.sourceFolders;
            // Section text, figures and a bibliography, about 20 files per folder.
            for (int i = 0; i < 20 + (f == 0 && p < 6 ? 1 : 0); ++i) {
                const QString relative = i < 12 ? QStringLiteral("sections/part-%1.md").arg(i)
                    : i < 19 ? QStringLiteral("figures/figure-%1.png").arg(i) : QStringLiteral("references-%1.bib").arg(i);
                if (!writeFile(root + QLatin1Char('/') + owner + QLatin1Char('/') + relative, QByteArray(256 + i * 37, 'x'))) return {};
                ++fixture.sourceFiles;
            }
        }
        QJsonArray declarations, documents;
        const int count = p < 5 ? 10 : 9;
        for (int d = 0; d < count; ++d, ++documentNumber) {
            const QString documentId = uuid(projectId + QStringLiteral("/document-%1").arg(d));
            const int folder = d % owners.size();
            const QString file = QStringLiteral("chapter-%1.md").arg(d);
            const QString alias = owners.at(folder) + QLatin1Char('/') + file;
            const QString source = root + QLatin1Char('/') + alias;
            if (!writeFile(source, QByteArrayLiteral("# Chapter\n\nText.\n"))) return {};
            ++fixture.sourceFiles;
            declarations.append(QJsonObject {{QStringLiteral("document_id"), documentId}, {QStringLiteral("format"), QStringLiteral("markdown")},
                                             {QStringLiteral("included"), true},
                                             {QStringLiteral("source"), QJsonObject {{QStringLiteral("folder_id"), folderIds.at(folder)}, {QStringLiteral("path"), file}}}});
            const QString filename = QStringLiteral("bench_%1_chapter_%2.pdf").arg(p).arg(d);
            documents.append(QJsonObject {{QStringLiteral("document_id"), documentId}, {QStringLiteral("id"), alias},
                                          {QStringLiteral("name"), QStringLiteral("Chapter %1.%2").arg(p).arg(d)},
                                          {QStringLiteral("filename"), filename}, {QStringLiteral("source_file"), alias},
                                          {QStringLiteral("size_bytes"), 100000 + documentNumber},
                                          {QStringLiteral("sha256"), QString(64, QLatin1Char('a'))}});
            Thoughts::TrackedCommentDocument publication;
            publication.stem = QFileInfo(filename).completeBaseName();
            publication.displayName = documents.last().toObject().value(QStringLiteral("name")).toString();
            publication.pdfPath = root + QStringLiteral("/current_pdfs/") + filename;
            publication.libraryFilename = filename;
            publication.libraryKind = QStringLiteral("source_linked");
            publication.libraryProject = owners.at(folder);
            publication.libraryProjectId = projectId;
            publication.librarySha256 = QString(64, QLatin1Char('a'));
            publication.libraryBuiltAt = builtAt.addSecs(documentNumber * 60);
            publication.libraryPublishedAt = publication.libraryBuiltAt;
            publication.libraryPageCount = 4 + d;
            publication.pdfSizeBytes = 100000 + documentNumber;
            publication.state = Thoughts::TrackedCommentDocument::SourceLinked;
            fixture.publications.append(publication);
            fixture.sources.append(source);
        }
        const QJsonObject manifest {{QStringLiteral("schema_version"), 5}, {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
                                    {QStringLiteral("id"), projectId}, {QStringLiteral("name"), QStringLiteral("Project %1").arg(p, 2, 10, QLatin1Char('0'))},
                                    {QStringLiteral("default_folder_id"), defaultFolderId}, {QStringLiteral("source_folders"), folders},
                                    {QStringLiteral("documents"), declarations}};
        if (!Thoughts::ProjectConfiguration::save(manifestPath, manifest).isEmpty()) return {};
        cached.append(QJsonObject {{QStringLiteral("id"), projectId}, {QStringLiteral("revision"), 1}, {QStringLiteral("target"), QString()},
                                   {QStringLiteral("documents"), documents}, {QStringLiteral("sources"), QJsonArray {}}});
    }
    if (!writeFile(root + QStringLiteral("/.thoughts/library-projects.json"),
                   QJsonDocument(QJsonObject {{QStringLiteral("projects"), cached}}).toJson())) return {};
    return fixture;
}

void CommentsOverviewTest::testSourceEditRebuildsLibraryTreeOnce()
{
    // The real pipeline: a source edit (or an accepted proposal, which writes
    // the source) reaches the overview as a file change, a pending-change
    // snapshot and a background read of the one document built from it.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    Thoughts::DeveloperLog::Options options;
    options.directory = temporary.filePath(QStringLiteral("logs"));
    options.captureStacks = false;
    Thoughts::DeveloperLog log(options);
    QVERIFY(registerSourceProject(root, QStringLiteral("source-edit"), QStringLiteral("Source edit"), {QStringLiteral("docs-test")}));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/paper.pdf"), QByteArrayLiteral("%PDF-paper")));
    const QString source = QDir::cleanPath(root + QStringLiteral("/docs-test/tex/source/main.tex"));
    QVERIFY(writeFile(source, QByteArrayLiteral("\\section{Before}\n")));
    QVERIFY(writeReviewCommentBundle(root, QStringLiteral("paper.pdf"), QJsonArray()));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    overview.m_tabs->setCurrentWidget(overview.m_summaryPage);
    QTRY_VERIFY(!overview.m_fullReloadRunning && !overview.m_catalogSignature.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(overview.m_documentsBySourceKnown, 5000);
    QTRY_VERIFY(!overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive());
    QTest::qWait(1500);
    QVERIFY(overview.m_libraryTree->topLevelItemCount() > 0);
    QSignalSpy rebuilds(overview.m_libraryTree->model(), &QAbstractItemModel::modelReset);
    const auto settled = [&] {
        return overview.m_scheduledDocumentReloads.isEmpty() && overview.m_runningDocumentReloads.isEmpty()
            && !overview.m_reloadTimer->isActive() && !overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive();
    };

    int pending = 1;
    const auto edit = [&](const QByteArray &text) {
        QVERIFY(writeFile(source, text));
        overview.handleDocsPathsChanged({source});
        overview.setSyncSnapshot({}, {}, QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), ++pending}}, {}, {});
    };
    edit(QByteArrayLiteral("\\section{After}\n"));
    QCOMPARE(overview.m_scheduledDocumentReloads, QSet<QString> {QStringLiteral("paper")});
    QTRY_VERIFY(settled());
    QTest::qWait(2500);
    QCOMPARE(rebuilds.size(), 1);

    // While the Projects tab is not shown, nothing rebuilds; showing it rebuilds once.
    rebuilds.clear();
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    edit(QByteArrayLiteral("\\section{Hidden tab}\n"));
    QTRY_VERIFY(settled());
    QTest::qWait(2500);
    QCOMPARE(rebuilds.size(), 0);
    overview.m_tabs->setCurrentWidget(overview.m_summaryPage);
    QTRY_COMPARE(rebuilds.size(), 1);
    QTest::qWait(1500);
    QCOMPARE(rebuilds.size(), 1);

    // The same while the Library window is hidden.
    rebuilds.clear();
    overview.hide();
    edit(QByteArrayLiteral("\\section{Hidden window}\n"));
    QTRY_VERIFY(settled());
    QTest::qWait(2500);
    QCOMPARE(rebuilds.size(), 0);
    overview.show();
    QTRY_COMPARE(rebuilds.size(), 1);
    QTRY_VERIFY(!overview.m_fullReloadRunning && settled());
    QTest::qWait(2500);
    QCOMPARE(rebuilds.size(), 1);
}

void CommentsOverviewTest::testLibraryMarksFailedDocuments()
{
    // The Library of 2026-10-14: the project "concepts" showed a warning, but
    // none of its document rows said which PDF had failed.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString owner = QStringLiteral("docs-concepts");
    const QStringList stems {QStringLiteral("grpo"), QStringLiteral("sdpo"), QStringLiteral("diffusion"), QStringLiteral("moe")};
    QJsonArray projectDocuments;
    for (const QString &stem : stems) {
        const QString source = owner + QStringLiteral("/tex/source/") + stem + QStringLiteral(".tex");
        QVERIFY(writeFile(root + QLatin1Char('/') + source, QByteArrayLiteral("\\section{Body}\n")));
        projectDocuments.append(QJsonObject {{QStringLiteral("id"), source}, {QStringLiteral("filename"), QStringLiteral("concepts_%1.pdf").arg(stem)},
                                             {QStringLiteral("name"), stem}, {QStringLiteral("source_file"), source}});
    }
    const QString failingSource = owner + QStringLiteral("/tex/fragments/moe-routing.tex");
    QVERIFY(writeFile(root + QLatin1Char('/') + failingSource, QByteArrayLiteral("\\Cref{fig:routing}\n")));
    // An imported PDF is not built; no build failure applies to it.
    projectDocuments.append(QJsonObject {{QStringLiteral("id"), QStringLiteral("paper.pdf")}, {QStringLiteral("filename"), QStringLiteral("concepts_paper.pdf")},
                                         {QStringLiteral("name"), QStringLiteral("Imported paper")}, {QStringLiteral("imported"), true}, {QStringLiteral("start_page"), 1}});
    const QJsonObject project {{QStringLiteral("id"), QStringLiteral("new:concepts")}, {QStringLiteral("name"), QStringLiteral("concepts")},
                               {QStringLiteral("source_folders"), QJsonArray {QJsonObject {{QStringLiteral("project"), owner}, {QStringLiteral("folder"), QStringLiteral("concepts")}}}},
                               {QStringLiteral("documents"), projectDocuments}};
    Thoughts::LibraryProjectStore store(root);
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(root, project));

    Thoughts::CommentsOverview overview(root);
    overview.resize(1100, 640);
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    QTRY_VERIFY_WITH_TIMEOUT(!overview.m_fullReloadRunning && !overview.m_catalogSignature.isEmpty() && !overview.m_libraryInventoryDirty
                                 && !overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive()
                                 && overview.m_librarySourceInventory.contains(owner), 10000);

    const QString failure = QStringLiteral("latex_error: Source docs-concepts/tex/fragments/moe-routing.tex:115 failed: Undefined control sequence \\Cref");
    QList<Thoughts::TrackedCommentDocument> tracked;
    for (const QString &stem : stems + QStringList {QStringLiteral("paper")}) {
        Thoughts::TrackedCommentDocument document;
        document.stem = QStringLiteral("concepts_") + stem;
        document.displayName = stem;
        document.libraryFilename = document.stem + QStringLiteral(".pdf");
        document.pdfPath = root + QStringLiteral("/current_pdfs/") + document.libraryFilename;
        document.libraryKind = stem == QLatin1String("paper") ? QStringLiteral("unlinked") : QStringLiteral("source_linked");
        document.librarySha256 = QString(64, QLatin1Char('a'));
        document.libraryPageCount = 6;
        document.state = stem == QLatin1String("paper") ? Thoughts::TrackedCommentDocument::PdfOnly : Thoughts::TrackedCommentDocument::SourceLinked;
        if (stem == QLatin1String("moe") || stem == QLatin1String("paper")) {
            // The Library index keeps the last-good PDF and names the failure.
            document.libraryPdfStale = true;
            document.libraryLatestBuildMessage = stem == QLatin1String("moe") ? failure : QStringLiteral("an imported PDF has no build");
            document.libraryLatestBuildSource = stem == QLatin1String("moe") ? failingSource : QString();
        }
        tracked.append(document);
    }
    overview.m_documents = tracked;
    overview.updateLibraryTree();
    tree->expandAll();

    const auto row = [tree](const QString &text, const QString &identityPart = QString()) -> QTreeWidgetItem * {
        for (QTreeWidgetItemIterator it(tree); *it; ++it)
            if ((*it)->text(0) == text && (identityPart.isEmpty() || (*it)->data(0, Qt::UserRole + 107).toString().contains(identityPart))) return *it;
        return nullptr;
    };
    const auto failureOf = [](QTreeWidgetItem *item) { return item ? item->data(0, Qt::UserRole + 121).toString() : QStringLiteral("<missing row>"); };
    const QString warningIconName = QIcon::fromTheme(QStringLiteral("dialog-warning")).name();
    auto *moe = row(QStringLiteral("moe"));
    QVERIFY(moe);
    QCOMPARE(failureOf(moe), failure);
    QCOMPARE(moe->icon(0).name(), warningIconName);
    QVERIFY2(moe->toolTip(0).startsWith(QStringLiteral("PDF build failed: ") + failure), qPrintable(moe->toolTip(0)));
    QVERIFY(moe->toolTip(0).contains(QStringLiteral("PDF with sources")));
    QVERIFY(moe->data(0, Qt::AccessibleDescriptionRole).toString().startsWith(QStringLiteral("PDF build failed")));
    for (const QString &healthy : {QStringLiteral("grpo"), QStringLiteral("sdpo"), QStringLiteral("diffusion"), QStringLiteral("Imported paper")}) {
        QVERIFY2(row(healthy), qPrintable(healthy));
        QVERIFY2(failureOf(row(healthy)).isEmpty(), qPrintable(healthy));
        QVERIFY2(!row(healthy)->toolTip(0).contains(QStringLiteral("failed")), qPrintable(healthy));
    }
    // The failing source file is marked, the document's own main file is not.
    QTRY_VERIFY(row(QStringLiteral("moe-routing.tex")));
    QCOMPARE(failureOf(row(QStringLiteral("moe-routing.tex"))), failure);
    QVERIFY(failureOf(row(QStringLiteral("moe.tex"))).isEmpty());
    // The project keeps its warning.
    QCOMPARE(failureOf(tree->topLevelItem(0)), failure);
    QCOMPARE(tree->topLevelItem(0)->toolTip(0), QStringLiteral("PDF build failed: ") + failure);

    // Selecting the row shows the whole message below the tree.
    tree->setCurrentItem(moe);
    auto *selectionDetails = overview.findChild<QLabel *>(QStringLiteral("thoughtsLibrarySelectionDetails"));
    QVERIFY(selectionDetails);
    QCOMPARE(selectionDetails->text(), QStringLiteral("PDF · concepts_moe.pdf\nPDF build failed: ") + failure);
    if (const QString screenshot = qEnvironmentVariable("THOUGHTS_LIBRARY_WARNING_SCREENSHOT"); !screenshot.isEmpty()) {
        overview.m_tabs->setCurrentWidget(overview.m_summaryPage);
        tree->scrollToItem(tree->topLevelItem(0));
        settleLayouts();
        QVERIFY(overview.grab().save(screenshot));
    }

    // Fedora's render status alone (the Library index has not caught up) marks
    // a document after a Sync, and a later Sync that fixes it unmarks it.
    QSignalSpy rebuilds(tree->model(), &QAbstractItemModel::modelReset);
    const QJsonObject clean {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}};
    const QString attention = QStringLiteral("The latest Fedora build needs attention for this PDF.");
    const QJsonObject failing {{QStringLiteral("concepts_sdpo.pdf"),
                                QJsonObject {{QStringLiteral("state"), QStringLiteral("error")}, {QStringLiteral("last_build_failed"), true}, {QStringLiteral("reason"), attention}}}};
    overview.setSyncSnapshot(failing, {}, clean, {}, {});
    QTRY_COMPARE(failureOf(row(QStringLiteral("sdpo"))), attention);
    QCOMPARE(failureOf(row(QStringLiteral("moe"))), failure);
    QCOMPARE(rebuilds.size(), 1);
    // Presentation changes that mark no other row leave the tree alone.
    QJsonObject unrelated = failing;
    unrelated.insert(QStringLiteral("concepts_grpo.pdf"), QJsonObject {{QStringLiteral("state"), QStringLiteral("current")}, {QStringLiteral("reason"), QStringLiteral("Current")}});
    overview.setSyncSnapshot(unrelated, {}, clean, {}, {});
    QTest::qWait(2500);
    QCOMPARE(rebuilds.size(), 1);
    overview.setSyncSnapshot({}, {}, clean, {}, {});
    QTRY_VERIFY(failureOf(row(QStringLiteral("sdpo"))).isEmpty());
    QCOMPARE(rebuilds.size(), 2);

    // Details: the row's menu offers the error, whose box leads to the Sync log.
    tree->expandAll();
    moe = row(QStringLiteral("moe"));
    QVERIFY(moe);
    bool offered = false;
    QTimer::singleShot(0, &overview, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        QVERIFY(menu);
        offered = !menu->actions().isEmpty() && menu->actions().first()->objectName() == QLatin1String("libraryShowBuildErrorAction");
        menu->close();
    });
    overview.showLibraryItemContextMenu(moe, tree, tree->visualItemRect(moe).center(), true);
    QVERIFY(offered);
    overview.m_latestSyncOperation = {{QStringLiteral("id"), QStringLiteral("sync-with-failure")}, {QStringLiteral("last_error_code"), QStringLiteral("pdf_build_failed")}};
    QSignalSpy logs(&overview, &Thoughts::CommentsOverview::syncOperationLogsRequested);
    overview.showLibraryBuildFailure(QStringLiteral("moe"), failure);
    auto *box = overview.findChild<QMessageBox *>(QStringLiteral("libraryBuildErrorBox"));
    QVERIFY(box);
    QTRY_VERIFY(box->isVisible());
    QCOMPARE(box->informativeText(), failure);
    auto *showLogs = box->findChild<QAbstractButton *>(QStringLiteral("libraryBuildErrorShowLogs"));
    QVERIFY(showLogs);
    showLogs->click();
    QCOMPARE(logs.size(), 1);
    QCOMPARE(logs.first().at(0).toString(), QStringLiteral("sync-with-failure"));
    // A Sync that succeeded and reports the failed build as its warning leads to its log too.
    QPointer<QMessageBox> closed(box);
    QTRY_VERIFY(!closed);
    overview.m_latestSyncOperation = {{QStringLiteral("id"), QStringLiteral("sync-with-warning")}, {QStringLiteral("state"), QStringLiteral("succeeded")},
                                      {QStringLiteral("warning_code"), QStringLiteral("pdf_build_failed")}};
    overview.showLibraryBuildFailure(QStringLiteral("moe"), failure);
    box = overview.findChild<QMessageBox *>(QStringLiteral("libraryBuildErrorBox"));
    QVERIFY(box);
    QTRY_VERIFY(box->isVisible());
    showLogs = box->findChild<QAbstractButton *>(QStringLiteral("libraryBuildErrorShowLogs"));
    QVERIFY(showLogs);
    showLogs->click();
    QCOMPARE(logs.size(), 2);
    QCOMPARE(logs.last().at(0).toString(), QStringLiteral("sync-with-warning"));
}

void CommentsOverviewTest::testLibraryBuildWarningsKeepRebuildCost()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const LargeLibraryFixture fixture = writeLargeLibraryFixture(root);
    QCOMPARE(fixture.publications.size(), 230);
    Thoughts::CommentsOverview overview(root);
    overview.resize(1200, 800);
    overview.show();
    overview.m_tabs->setCurrentWidget(overview.m_summaryPage);
    QTRY_VERIFY_WITH_TIMEOUT(!overview.m_fullReloadRunning && !overview.m_catalogSignature.isEmpty() && !overview.m_libraryInventoryDirty
                                 && !overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(overview.m_librarySourceInventory.size() >= 45, 20000);
    const auto median = [&overview] {
        QList<qint64> runs;
        for (int i = 0; i < 5; ++i) {
            QElapsedTimer timer;
            timer.start();
            overview.updateLibraryTree();
            runs.append(timer.nsecsElapsed() / 1000);
        }
        std::sort(runs.begin(), runs.end());
        return runs.at(2);
    };
    overview.m_documents = fixture.publications;
    const qint64 healthy = median();
    // Ten failing PDFs in different projects, each naming its failing source.
    QList<Thoughts::TrackedCommentDocument> failing = fixture.publications;
    for (int i = 0; i < failing.size(); i += 23) {
        failing[i].libraryPdfStale = true;
        failing[i].libraryLatestBuildMessage = QStringLiteral("latex_error: Source %1:3 failed: Undefined control sequence").arg(QDir(root).relativeFilePath(fixture.sources.at(i)));
        failing[i].libraryLatestBuildSource = QDir(root).relativeFilePath(fixture.sources.at(i));
    }
    overview.m_documents = failing;
    const qint64 marked = median();
    int failedRows = 0;
    for (QTreeWidgetItemIterator it(overview.m_libraryTree); *it; ++it)
        if (!(*it)->data(0, Qt::UserRole + 121).toString().isEmpty()) ++failedRows;
    qInfo().noquote() << QStringLiteral("Library rebuild (median of 5): %1 ms healthy, %2 ms with 10 failed PDFs; %3 marked rows")
                             .arg(healthy / 1000.0, 0, 'f', 1).arg(marked / 1000.0, 0, 'f', 1).arg(failedRows);
    // Each failing PDF marks its document and its source file; its project once.
    QCOMPARE(failedRows, 10 + 10 + 10);
}

void CommentsOverviewTest::testLibraryChangeBurstRebuildsTreeOnce()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    Thoughts::DeveloperLog::Options options;
    options.directory = temporary.filePath(QStringLiteral("logs"));
    options.captureStacks = false;
    Thoughts::DeveloperLog log(options);
    const LargeLibraryFixture fixture = writeLargeLibraryFixture(root);
    QCOMPARE(fixture.publications.size(), 230);
    QCOMPARE(fixture.sourceFolders, 45);
    QVERIFY(fixture.sourceFiles > 900);

    Thoughts::CommentsOverview overview(root);
    overview.resize(1200, 800);
    // Local source monitoring answers like the reader part does: later, and
    // with changed information when a source changed in between.
    int monitoringRound = 0;
    connect(&overview, &Thoughts::CommentsOverview::sourceMonitoringBatchRequested, &overview,
            [&overview, &monitoringRound](quint64 id, const QStringList &targets) {
                QVariantMap results;
                for (const QString &target : targets)
                    results.insert(target, QVariantMap {{QStringLiteral("status"), QStringLiteral("current")},
                                                        {QStringLiteral("pending_local_changes"), monitoringRound}});
                QTimer::singleShot(300, &overview, [&overview, id, results] { overview.applySourceMonitoringBatch(id, results); });
            });
    overview.show();
    overview.m_tabs->setCurrentWidget(overview.m_summaryPage);
    QTRY_VERIFY_WITH_TIMEOUT(!overview.m_fullReloadRunning && !overview.m_catalogSignature.isEmpty() && !overview.m_libraryInventoryDirty
                                 && !overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive(), 20000);
    QTRY_VERIFY_WITH_TIMEOUT(overview.m_librarySourceInventory.size() >= 45, 20000); // and each default folder
    // The disk has no PDFs: publish the fixture's publications as a load would.
    overview.m_documents = fixture.publications;
    overview.updateLibraryTree();
    QTest::qWait(1500);
    auto *tree = overview.m_libraryTree;
    QCOMPARE(tree->topLevelItemCount(), 25);
    int items = 0;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) ++items;
    QVERIFY2(items > 1200, qPrintable(QString::number(items)));

    // The user's view: some projects open, a document selected, scrolled down.
    const auto identity = [](QTreeWidgetItem *item) { return item ? item->data(0, Qt::UserRole + 107).toString() : QString(); };
    for (int i = 0; i < tree->topLevelItemCount(); i += 3) tree->topLevelItem(i)->setExpanded(true);
    QTreeWidgetItem *selectedDocument = nullptr;
    for (QTreeWidgetItemIterator it(tree); *it && !selectedDocument; ++it)
        if ((*it)->text(0) == QStringLiteral("Chapter 12.4")) selectedDocument = *it;
    QVERIFY(selectedDocument);
    tree->setCurrentItem(selectedDocument);
    tree->scrollToItem(selectedDocument);
    QTest::qWait(50);
    const QString selected = identity(tree->currentItem());
    QVERIFY(!selected.isEmpty());
    QSet<QString> expanded;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->isExpanded()) expanded.insert(identity(*it));
    const int scroll = tree->verticalScrollBar()->value();
    QVERIFY(scroll > 0);

    // One source change as the log of 2026-10-13 13:39 shows it: the document
    // read runs in the background, Sync's pending-change snapshot arrives,
    // the document read finishes, and local monitoring answers afterwards.
    int pending = 3;
    int burst = 0;
    const auto change = [&](int index) {
        ++monitoringRound;
        ++burst;
        Thoughts::TrackedCommentDocument changed = fixture.publications.at(index);
        changed.libraryPageCount = 40 + burst; // visible in the tree
        const QString stem = changed.stem;
        overview.m_runningDocumentReloads.insert(stem);
        QVERIFY(writeFile(fixture.sources.at(index), QByteArray("# Chapter\n\nEdited ") + QByteArray::number(burst) + '\n'));
        overview.requestCatalogCheck(true);
        QTest::qWait(50);
        overview.setSyncSnapshot({}, {}, QJsonObject {{QStringLiteral("available"), true}, {QStringLiteral("total"), ++pending}}, {}, {});
        QTest::qWait(600);
        overview.finishDocumentReload(stem, changed);
        QTest::qWait(3000);
    };
    const auto measure = [&](const char *label, int index, int *rebuildCount) {
        QSignalSpy rebuilds(tree->model(), &QAbstractItemModel::modelReset);
        const QDateTime since = QDateTime::currentDateTimeUtc();
        const qint64 cpu = mainThreadCpuMicroseconds();
        change(index);
        const qint64 cpuMs = (mainThreadCpuMicroseconds() - cpu) / 1000;
        const auto activities = libraryActivityMilliseconds(since);
        qInfo().noquote() << QStringLiteral("%1: %2 tree rebuilds; UI-thread CPU %3 ms over 3.65 s (compare the idle line); library.tree %4 ms, "
                                            "size_rows %5 ms, dates.apply %6 ms, navigation %7 ms")
                                 .arg(QLatin1String(label)).arg(rebuilds.size()).arg(cpuMs)
                                 .arg(activities.value(QStringLiteral("library.tree"))).arg(activities.value(QStringLiteral("library.size_rows")))
                                 .arg(activities.value(QStringLiteral("library.dates.apply"))).arg(activities.value(QStringLiteral("library.navigation")));
        *rebuildCount = rebuilds.size();
    };
    // One rebuild of the laid-out view, with rows expanded and one selected.
    {
        QList<qint64> direct;
        for (int i = 0; i < 3; ++i) {
            const qint64 cpu = mainThreadCpuMicroseconds();
            overview.updateLibraryTree();
            direct.append((mainThreadCpuMicroseconds() - cpu) / 1000);
        }
        std::sort(direct.begin(), direct.end());
        QJsonObject stages;
        for (const QJsonValue &value : Thoughts::DeveloperLog::performanceEvents())
            if (value.toObject().value(QStringLiteral("event")).toString() == QLatin1String("library.tree.stages")) stages = value.toObject().value(QStringLiteral("data")).toObject();
        qInfo().noquote() << QStringLiteral("one rebuild of %1 rows (median of 3): UI-thread CPU %2 ms; stages %3")
                                 .arg(items).arg(direct.at(1)).arg(QString::fromUtf8(QJsonDocument(stages).toJson(QJsonDocument::Compact)));
        QCOMPARE(identity(tree->currentItem()), selected);
        QCOMPARE(tree->verticalScrollBar()->value(), scroll);
    }
    // Idle cost of the same wall time, to separate the burst's own work.
    {
        const qint64 cpu = mainThreadCpuMicroseconds();
        QTest::qWait(3650);
        qInfo().noquote() << QStringLiteral("idle 3.65 s: UI-thread CPU %1 ms").arg((mainThreadCpuMicroseconds() - cpu) / 1000);
    }

    const int changedIndex = 12 * 9 + 5 + 4; // Chapter 12.4: the selected row
    int visibleRebuilds = -1;
    measure("visible Projects tab", changedIndex, &visibleRebuilds);
    QVERIFY2(visibleRebuilds <= 1, qPrintable(QString::number(visibleRebuilds)));
    QCOMPARE(identity(tree->currentItem()), selected);
    QCOMPARE(tree->currentItem()->text(5), QStringLiteral("41"));
    QSet<QString> expandedAfter;
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->isExpanded()) expandedAfter.insert(identity(*it));
    QCOMPARE(expandedAfter, expanded);
    QCOMPARE(tree->verticalScrollBar()->value(), scroll);
    QCOMPARE(tree->topLevelItemCount(), 25);

    // A read that changes only the document's comments or proposals leaves the tree alone.
    {
        QSignalSpy rebuilds(tree->model(), &QAbstractItemModel::modelReset);
        const QString stem = fixture.publications.at(changedIndex).stem;
        auto same = *std::find_if(overview.m_documents.cbegin(), overview.m_documents.cend(),
                                  [&stem](const Thoughts::TrackedCommentDocument &document) { return document.stem == stem; });
        ++same.orphanedCommentCount;
        overview.m_runningDocumentReloads.insert(stem);
        overview.finishDocumentReload(stem, same);
        QTest::qWait(1000);
        QCOMPARE(rebuilds.size(), 0);
        QCOMPARE(tree->currentItem()->text(5), QStringLiteral("41"));
    }

    // Another tab in front: the tree waits until the Projects tab is shown.
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    QTest::qWait(1500);
    int hiddenRebuilds = -1;
    measure("Comments tab in front", changedIndex, &hiddenRebuilds);
    QCOMPARE(hiddenRebuilds, 0);
    QSignalSpy shown(tree->model(), &QAbstractItemModel::modelReset);
    overview.m_tabs->setCurrentWidget(overview.m_summaryPage);
    QTRY_COMPARE(shown.size(), 1);
    QTest::qWait(1500);
    QCOMPARE(shown.size(), 1);
    QCOMPARE(identity(tree->currentItem()), selected);
    QCOMPARE(tree->currentItem()->text(5), QStringLiteral("42"));
    expandedAfter.clear();
    for (QTreeWidgetItemIterator it(tree); *it; ++it) if ((*it)->isExpanded()) expandedAfter.insert(identity(*it));
    QCOMPARE(expandedAfter, expanded);
    QCOMPARE(tree->verticalScrollBar()->value(), scroll);
}

void CommentsOverviewTest::testThoughtIgnoreDecidesSourceRescans()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/.ThoughtIgnore"), QByteArrayLiteral("build/\n*.log\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/notes.md"), QByteArrayLiteral("notes\n")));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning && !overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive());
    QVERIFY(!overview.m_libraryInventoryDirty);

    // Paths the folder's .ThoughtIgnore excludes never rescan the Sources tree.
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/build/out.md"), QByteArrayLiteral("out\n")));
    QVERIFY(writeFile(root + QStringLiteral("/docs-test/texput.log"), QByteArrayLiteral("log\n")));
    overview.handleDocsPathsChanged({root + QStringLiteral("/docs-test/build/out.md"), root + QStringLiteral("/docs-test/texput.log")});
    QVERIFY(!overview.m_libraryInventoryDirty);

    // Any other file does, including a top-level Markdown file or script that
    // the publication heuristics would not treat as a TeX source.
    for (const QString &name : {QStringLiteral("meeting_notes.md"), QStringLiteral("build.sh"), QStringLiteral(".ThoughtIgnore")}) {
        QVERIFY(writeFile(root + QStringLiteral("/docs-test/") + name, QByteArrayLiteral("x\n")));
        overview.handleDocsPathsChanged({root + QStringLiteral("/docs-test/") + name});
        QVERIFY2(overview.m_libraryInventoryDirty, qPrintable(name));
        QTRY_VERIFY(!overview.m_libraryInventoryDirty && !overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive());
    }

    // A project's default source folder lives under .thoughts; a file added
    // there appears at once too, while Finder metadata does not count.
    const QString defaultFolder = root + QStringLiteral("/.thoughts/projects/") + QString(64, QLatin1Char('a')) + QStringLiteral("/documents");
    QVERIFY(writeFile(defaultFolder + QStringLiteral("/.DS_Store"), QByteArrayLiteral("finder")));
    overview.handleDocsPathsChanged({defaultFolder + QStringLiteral("/.DS_Store")});
    QVERIFY(!overview.m_libraryInventoryDirty);
    QTRY_VERIFY(!overview.m_catalogSignatureCheckRunning && !overview.m_catalogPollTimer->isActive());
    QVERIFY(writeFile(defaultFolder + QStringLiteral("/course.md"), QByteArrayLiteral("# Course\n")));
    overview.handleDocsPathsChanged({defaultFolder + QStringLiteral("/course.md")});
    QVERIFY(overview.m_libraryInventoryDirty);
}

void CommentsOverviewTest::testReloadBatchesLibraryRefresh()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    const QString sidecar = root + QStringLiteral("/comments/alpha_comments.jsonl");
    QByteArray records;
    for (int index = 0; index < 100; ++index) {
        records += jsonLine(comment(QStringLiteral("comment-%1").arg(index), QStringLiteral("Comment %1").arg(index), 1,
                                    QStringLiteral("2026-09-08T17:00:00Z"), QStringLiteral("anchored")));
    }
    QVERIFY(writeFile(sidecar, records));
    Thoughts::CommentsOverview overview(root);
    overview.show();
    overview.showSummary(); // the Library tree is rebuilt while it is on screen
    QTRY_COMPARE(overview.commentCount(), 100);
    auto *library = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(library);
    QVERIFY(table);
    QSignalSpy resets(library->model(), &QAbstractItemModel::modelReset);
    records += jsonLine(comment(QStringLiteral("new-comment"), QStringLiteral("New comment"), 1,
                                QStringLiteral("2026-09-08T17:01:00Z"), QStringLiteral("anchored")));
    QVERIFY(writeFile(sidecar, records));
    overview.reload();
    QCOMPARE(overview.commentCount(), 101);
    QCOMPARE(table->model()->rowCount(), 101);
    // A catalog refresh must not rebuild the entire Library for every comment.
    QVERIFY2(resets.size() < 10, qPrintable(QStringLiteral("Library rebuilt %1 times for one reload").arg(resets.size())));
    QVERIFY(resets.size() > 0);

    resets.clear();
    records += jsonLine(comment(QStringLiteral("synced-comment"), QStringLiteral("Synced comment"), 1,
                                QStringLiteral("2026-09-08T17:02:00Z"), QStringLiteral("anchored")));
    QVERIFY(writeFile(sidecar, records));
    overview.requestReload();
    QTRY_COMPARE(table->model()->rowCount(), 102);
    QCOMPARE(overview.commentCount(), 102);
    QTRY_VERIFY(resets.size() > 0);
    QVERIFY2(resets.size() < 10, qPrintable(QStringLiteral("Library rebuilt %1 times for one background reload").arg(resets.size())));
}

void CommentsOverviewTest::testSyncProgressPreservesNavigationModels()
{
    QTemporaryDir temporary;
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    overview.m_catalogPollTimer->stop();
    const QJsonObject pending {{QStringLiteral("available"), true}, {QStringLiteral("total"), 0}};
    const QJsonObject aggregate {{QStringLiteral("enabled"), true}};
    overview.setSyncSnapshot({}, aggregate, pending, {});
    overview.syncStarted();
    QSignalSpy libraryResets(overview.m_libraryTree->model(), &QAbstractItemModel::modelReset);
    QSignalSpy navigationResets(overview.m_documentList->model(), &QAbstractItemModel::modelReset);
    for (int i = 0; i < 100; ++i) {
        overview.setSyncSnapshot({}, aggregate, pending, {},
            QJsonObject {{QStringLiteral("status"), QStringLiteral("running")}, {QStringLiteral("bytes_transferred"), i * 1024}});
    }
    QCOMPARE(libraryResets.size(), 0);
    QCOMPARE(navigationResets.size(), 0);
    // Status changes are retained without rebuilding navigation during Sync.
    overview.setSyncSnapshot(QJsonObject {{QStringLiteral("paper.pdf"), QJsonObject {{QStringLiteral("status"), QStringLiteral("Current")}}}}, aggregate, pending, {});
    QCOMPARE(libraryResets.size(), 0);
    QCOMPARE(navigationResets.size(), 0);
    libraryResets.clear();
    navigationResets.clear();
    // Restoring idle labels must also leave both models alone.
    overview.m_syncOperationRunning = false;
    overview.restoreIdlePdfSyncPresentation();
    QCOMPARE(libraryResets.size(), 0);
    QCOMPARE(navigationResets.size(), 0);
}

void CommentsOverviewTest::testSyncKeepsContentStable_data()
{
    QTest::addColumn<bool>("success");
    QTest::newRow("success") << true;
    QTest::newRow("partial-failure") << false;
}

void CommentsOverviewTest::testSyncKeepsContentStable()
{
    QFETCH(bool, success);
    QTemporaryDir root;
    // The Library row comes from the Project configuration.
    QVERIFY(registerCatalogueProject(root.path(), {{QStringLiteral("id"), QStringLiteral("new:stable")}, {QStringLiteral("name"), QStringLiteral("Stable project")}}));
    Thoughts::CommentsOverview overview(root.path());
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    overview.m_catalogPollTimer->stop();
    Thoughts::TrackedCommentDocument document;
    document.stem = QStringLiteral("stable");
    document.displayName = QStringLiteral("Stable project");
    document.pdfPath = root.filePath(QStringLiteral("stable.pdf"));
    overview.m_documents = {document};
    overview.updateDashboardSummary();
    auto *row = overview.m_libraryTree->topLevelItem(0);
    QVERIFY(row);
    overview.m_libraryTree->setCurrentItem(row);
    row->setExpanded(true);
    const QString summary = overview.m_librarySummary->text();
    QSignalSpy resets(overview.m_libraryTree->model(), &QAbstractItemModel::modelReset);
    overview.syncStarted();
    overview.updateDashboardSummary();
    overview.reload();
    overview.requestReload();
    overview.finishFullReload({}, true); // A read started before Sync completed late.
    overview.finishDocumentReload(document.stem, std::nullopt);
    overview.applyLibraryModificationDates();
    QCoreApplication::processEvents();
    QCOMPARE(resets.size(), 0);
    QCOMPARE(overview.m_libraryTree->currentItem(), row);
    QCOMPARE(overview.m_librarySummary->text(), summary);
    QCOMPARE(overview.m_documents.size(), 1);
    QVERIFY(overview.m_syncButton->text().contains(QStringLiteral("Syncing")));

    overview.syncFinished(success, success ? QString() : QStringLiteral("Transfer failed"));
    // The old rows stay visible while the terminal snapshot is read.
    QCOMPARE(overview.m_libraryTree->currentItem(), row);
    QTRY_VERIFY(!overview.m_syncRefreshPending && !overview.m_syncLibraryTree && !overview.m_fullReloadRunning);
    QCOMPARE(overview.m_documents.size(), 0);
    QCOMPARE(overview.m_libraryTree->topLevelItemCount(), 1);
    QCOMPARE(resets.size(), 1);
    QVERIFY(!overview.m_syncContentDeferred);
}

void CommentsOverviewTest::testSummaryRefreshIsDeferredAndBatched()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    // Library rows come from Project configurations, one per published PDF.
    for (int i = 0; i < 12; ++i) {
        const QString stem = QStringLiteral("project-%1").arg(i);
        QVERIFY(registerCatalogueProject(temporary.path(), {{QStringLiteral("id"), QString(QStringLiteral("new:") + stem)}, {QStringLiteral("name"), stem},
            {QStringLiteral("documents"), QJsonArray{QJsonObject{{QStringLiteral("filename"), QString(stem + QStringLiteral(".pdf"))}}}}}));
    }
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    // The first-show catalogue check refreshes the source inventory and then
    // rebuilds the Library tree. Under load it is still running, or deferred
    // by a second, when the reload ends; stopping the timer cancels neither.
    QTRY_VERIFY_WITH_TIMEOUT(!overview.m_fullReloadRunning && !overview.m_libraryInventoryDirty && !overview.m_catalogSignatureCheckRunning
                                 && !overview.m_catalogPollTimer->isActive(), 10000);
    overview.m_catalogPollTimer->stop();
    overview.m_documents.clear();
    QVariantMap results;
    for (int i = 0; i < 12; ++i) {
        Thoughts::TrackedCommentDocument document;
        document.stem = QStringLiteral("project-%1").arg(i);
        document.displayName = document.stem;
        document.libraryFilename = document.stem + QStringLiteral(".pdf");
        document.pdfPath = temporary.filePath(document.libraryFilename);
        document.libraryKind = QStringLiteral("source_linked");
        document.libraryProject = QStringLiteral("docs-") + document.stem;
        document.librarySha256 = QString(64, QLatin1Char('a'));
        overview.m_documents.append(document);
        results.insert(document.libraryFilename, QVariantMap {{QStringLiteral("status"), QStringLiteral("current")},
                                                             {QStringLiteral("tracked_file_count"), i + 1}});
    }
    overview.updateLibraryTree();
    auto *tree = overview.m_libraryTree;
    tree->topLevelItem(0)->setExpanded(true);
    tree->setCurrentItem(tree->topLevelItem(0));
    const QString selected = tree->currentItem()->text(0);
    QSignalSpy resets(tree->model(), &QAbstractItemModel::modelReset);
    // Project selection, search and tab filtering must not rebuild hidden Library rows.
    overview.populateDocumentList(QString());
    for (bool notes : {false, true}) {
        overview.m_tabs->setCurrentWidget(notes ? overview.m_notesPage : overview.m_commentsPage);
        overview.applyFilters();
        overview.m_search->setText(QStringLiteral("project"));
        overview.m_documentList->setCurrentItem(overview.m_documentList->topLevelItem(1));
        overview.m_search->clear();
        overview.m_documentList->setCurrentItem(overview.m_documentList->topLevelItem(0));
    }
    QCOMPARE(resets.size(), 0);
    QCOMPARE(tree->currentItem()->text(0), selected);
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    QSignalSpy batches(&overview, &Thoughts::CommentsOverview::sourceMonitoringBatchRequested);
    QSignalSpy individual(&overview, &Thoughts::CommentsOverview::sourceMonitoringInformationRequested);
    overview.showSummary();
    QCOMPARE(batches.size(), 0); // No database/file work during the tab change.
    QCOMPARE(resets.size(), 0);
    QTRY_COMPARE(batches.size(), 1);
    QCOMPARE(batches.last().at(1).toStringList().size(), 12);
    QCOMPARE(individual.size(), 0);
    overview.applySourceMonitoringBatch(batches.last().at(0).toULongLong(), results);
    QCOMPARE(resets.size(), 0); // The tree shows no monitoring information: results never rebuild it.
    QCOMPARE(tree->currentItem()->text(0), selected);
    QVERIFY(tree->currentItem()->isExpanded());

    resets.clear();
    overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
    overview.showSummary();
    QTRY_COMPARE(batches.size(), 2);
    overview.applySourceMonitoringBatch(batches.last().at(0).toULongLong(), results);
    QCOMPARE(resets.size(), 0); // Unchanged local information preserves the actual tree.

    // Rapid tab switches retain only one in-flight read and one follow-up.
    overview.showSummary();
    QTRY_COMPARE(batches.size(), 3);
    const quint64 obsolete = batches.last().at(0).toULongLong();
    for (int i = 0; i < 5; ++i) {
        overview.m_tabs->setCurrentWidget(overview.m_commentsPage);
        overview.showSummary();
    }
    QCOMPARE(batches.size(), 3);
    QVariantMap changed = results;
    const QString target = overview.m_documents.first().libraryFilename;
    changed[target] = QVariantMap {{QStringLiteral("status"), QStringLiteral("current")}, {QStringLiteral("tracked_file_count"), 99}};
    overview.applySourceMonitoringBatch(obsolete, changed);
    QCOMPARE(resets.size(), 0); // A superseded read must not flash old state.
    QTRY_COMPARE(batches.size(), 4);
    overview.applySourceMonitoringBatch(obsolete, changed);
    QCOMPARE(resets.size(), 0);
    overview.applySourceMonitoringBatch(batches.last().at(0).toULongLong(), changed);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(overview.m_sourceMonitoringInformation.value(overview.m_documents.first().pdfPath).value(QStringLiteral("tracked_file_count")).toInt(), 99);

    // A publication replaced while the read is running rejects its old result.
    resets.clear();
    overview.showSummary();
    QTRY_COMPARE(batches.size(), 5);
    overview.m_documents.first().librarySha256 = QString(64, QLatin1Char('b'));
    overview.applySourceMonitoringBatch(batches.last().at(0).toULongLong(), results);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(overview.m_sourceMonitoringInformation.value(overview.m_documents.first().pdfPath).value(QStringLiteral("tracked_file_count")).toInt(), 99);
    QTRY_COMPARE(batches.size(), 6);
    overview.applySourceMonitoringBatch(batches.last().at(0).toULongLong(), results);
    QCOMPARE(resets.size(), 0);
    QCOMPARE(overview.m_sourceMonitoringInformation.value(overview.m_documents.first().pdfPath).value(QStringLiteral("tracked_file_count")).toInt(), 1);

    // A background change cannot delete items retained by an open context menu.
    resets.clear();
    QMenu menu(&overview);
    menu.addAction(QStringLiteral("Existing action"));
    menu.popup(overview.mapToGlobal(QPoint(20, 20)));
    QTRY_COMPARE(QApplication::activePopupWidget(), &menu);
    overview.scheduleLibraryTreeUpdate(QStringLiteral("documents"));
    QTest::qWait(400);
    QCOMPARE(resets.size(), 0);
    menu.close();
    QTRY_COMPARE(resets.size(), 1);
    QCOMPARE(tree->currentItem()->text(0), selected);
}

void CommentsOverviewTest::testLargePlacementRefresh()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    Thoughts::CommentsOverview overview(temporary.path());
    overview.show();
    QTRY_VERIFY(!overview.m_fullReloadRunning);
    overview.m_catalogPollTimer->stop();

    // Match the actual workload: a modest number of rows, but tens of thousands
    // of nested placement/proof records, including one very large proposal.
    const QString hash(64, QLatin1Char('a'));
    const QJsonObject proof {{QStringLiteral("resolver_version"), 3},
                             {QStringLiteral("source_sha256"), hash},
                             {QStringLiteral("source_bundle_sha256"), hash},
                             {QStringLiteral("build_sha256"), hash},
                             {QStringLiteral("pdf_sha256"), hash},
                             {QStringLiteral("extracted_text_sha256"), hash},
                             {QStringLiteral("method"), QStringLiteral("compiler_named_span")},
                             {QStringLiteral("candidate_count"), 1},
                             {QStringLiteral("verified"), true}};
    const auto locations = [&](int count) {
        QJsonArray result;
        for (int index = 0; index < count; ++index) {
            QJsonObject location {{QStringLiteral("pdf_filename"), QStringLiteral("alpha.pdf")},
                                   {QStringLiteral("pdf_sha256"), hash},
                                   {QStringLiteral("build_sha256"), hash},
                                   {QStringLiteral("source_bundle_sha256"), hash},
                                   {QStringLiteral("page"), index / 750 + 1},
                                   {QStringLiteral("placement_kind"), QStringLiteral("exact_text")},
                                   {QStringLiteral("regions"), QJsonArray {QJsonArray {0.1, 0.2, 0.3, 0.21}}},
                                   {QStringLiteral("proof"), proof},
                                   {QStringLiteral("occurrence"), index}};
            result.append(location);
        }
        return result;
    };
    const QJsonArray largeLocations = locations(10323);
    const QJsonArray smallLocations = locations(1000);
    Thoughts::TrackedCommentDocument document;
    document.stem = QStringLiteral("alpha");
    document.displayName = QStringLiteral("Alpha");
    document.pdfPath = temporary.filePath(QStringLiteral("current_pdfs/alpha.pdf"));
    document.state = Thoughts::TrackedCommentDocument::PdfOnly;
    for (int index = 0; index < 240; ++index) {
        Thoughts::TrackedComment comment;
        comment.documentStem = document.stem;
        comment.pdfPath = document.pdfPath;
        comment.id = comment.threadRootId = QStringLiteral("comment-%1").arg(index);
        comment.contents = QStringLiteral("Comment %1").arg(index);
        comment.page = 1;
        if (index >= 219) {
            comment.pdfLocations = index == 239 ? largeLocations : smallLocations;
            comment.correspondence = {{QStringLiteral("review_key"), comment.id},
                                       {QStringLiteral("state"), QStringLiteral("exact_text")},
                                       {QStringLiteral("locations"), comment.pdfLocations}};
        }
        document.comments.append(comment);
    }
    QList<Thoughts::TrackedCommentDocument> documents {document};
    QList<qint64> timings;
    for (int iteration = 0; iteration < 3; ++iteration) {
        documents[0].comments[0].contents = QStringLiteral("Updated %1").arg(iteration);
        QElapsedTimer timer;
        timer.start();
        overview.finishFullReload(documents);
        timings.append(timer.nsecsElapsed());
        QCOMPARE(overview.commentCount(), 240);
        QCOMPARE(overview.m_model->rowCount(), 240);
    }
    std::sort(timings.begin(), timings.end());
    const double elapsedMs = timings.at(1) / 1000000.0;
    qInfo() << "Full UI refresh median (240 rows, 30323 placement records), ms:" << elapsedMs;
    QVERIFY2(elapsedMs < 250, qPrintable(QStringLiteral("Large placement refresh blocked the UI for %1 ms").arg(elapsedMs)));

    // Every column must retain the complete record for selection/actions; no
    // geometry or evidence may be discarded to achieve the speedup.
    for (int column = 0; column < overview.m_model->columnCount(); ++column) {
        bool found = false;
        const auto data = overview.m_model->itemData(overview.m_model->index(239, column));
        for (const QVariant &value : data) {
            const QJsonObject object = value.value<QJsonObject>();
            if (object.value(QStringLiteral("review_key")).toString() == QStringLiteral("comment-239")) {
                QCOMPARE(object, documents.first().comments.last().correspondence);
                QCOMPARE(object.value(QStringLiteral("locations")).toArray().last().toObject().value(QStringLiteral("occurrence")).toInt(), 10322);
                found = true;
            }
        }
        QVERIFY(found);
    }
    QPersistentModelIndex survivor(overview.m_model->index(239, 0));
    QSignalSpy inserted(overview.m_model, &QAbstractItemModel::rowsInserted);
    overview.finishFullReload(documents);
    QVERIFY(survivor.isValid());
    QCOMPARE(inserted.size(), 0);
}

void CommentsOverviewTest::testRowRevealPreservesHorizontalPosition()
{
    RowBrowserTree<QTreeView> comments;
    QStandardItemModel model;
    model.setColumnCount(2);
    auto *parent = new QStandardItem(QStringLiteral("Project"));
    model.appendRow(parent);
    parent->appendRow(new QStandardItem(QStringLiteral("Comment")));
    comments.setModel(&model);

    RowBrowserTree<QTreeWidget> review;
    review.setColumnCount(2);
    auto *card = new QTreeWidgetItem(&review, {QStringLiteral("Question")});
    const QList<QPair<QTreeView *, QModelIndex>> views = {
        {&comments, model.index(0, 0, parent->index())},
        {&review, review.indexFromItem(card)},
    };
    for (const auto &[view, target] : views) {
        view->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
        view->resize(240, 180);
        view->header()->setStretchLastSection(false);
        view->setColumnWidth(0, 600);
        view->setColumnWidth(1, 200);
        view->expandAll();
        view->show();
        QTRY_VERIFY(view->horizontalScrollBar()->maximum() > 100);
        view->setCurrentIndex(target);
        view->scrollTo(target);
        QCoreApplication::processEvents();
        QCOMPARE(view->horizontalScrollBar()->value(), 0);
        view->horizontalScrollBar()->setValue(100);
        view->scrollTo(target, QAbstractItemView::PositionAtTop);
        QCoreApplication::processEvents();
        QCOMPARE(view->horizontalScrollBar()->value(), 100);
    }
}

void CommentsOverviewTest::testReloadPreservesScrollPosition()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    const QString sidecar = root + QStringLiteral("/comments/alpha_comments.jsonl");
    QList<QJsonObject> records;
    const QDateTime firstActivity(QDate(2026, 7, 29), QTime(8, 0), QTimeZone::UTC);
    for (int index = 0; index < 60; ++index) {
        records.append(comment(QStringLiteral("comment-%1").arg(index), QStringLiteral("Comment %1").arg(index), index + 1, firstActivity.addSecs(index * 60).toString(Qt::ISODate), QStringLiteral("anchored")));
    }
    const auto serializedRecords = [](const QList<QJsonObject> &items) {
        QByteArray result;
        for (const QJsonObject &item : items) {
            result += jsonLine(item);
        }
        return result;
    };
    QVERIFY(writeFile(sidecar, serializedRecords(records)));

    Thoughts::CommentsOverview overview(root);
    overview.resize(900, 480);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 60);

    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    QVERIFY(table);
    QCOMPARE(table->verticalScrollMode(), QAbstractItemView::ScrollPerItem);
    QCOMPARE(table->horizontalScrollMode(), QAbstractItemView::ScrollPerPixel);
    QCOMPARE(table->verticalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
    QCOMPARE(table->horizontalScrollBarPolicy(), Qt::ScrollBarAlwaysOn);
    QCOMPARE(table->header()->cascadingSectionResizes(), false);
    // Every column, including Comment, must stay user-resizable so the chosen
    // width survives both interactive resizing and a window-state restore.
    for (int column = 0; column < table->model()->columnCount(); ++column) {
        QCOMPARE(table->header()->sectionResizeMode(column), QHeaderView::Interactive);
    }
    const int previousCommentWidth = table->columnWidth(0);
    table->setColumnWidth(0, previousCommentWidth + 90);
    QCOMPARE(table->columnWidth(0), previousCommentWidth + 90);
    table->setColumnWidth(0, previousCommentWidth);
    QCOMPARE(table->columnWidth(0), previousCommentWidth);
    const int previousDocumentWidth = table->columnWidth(1);
    table->setColumnWidth(1, previousDocumentWidth + 37);
    QCOMPARE(table->columnWidth(1), previousDocumentWidth + 37);
    QTRY_VERIFY(table->horizontalScrollBar()->maximum() > 0);
    QTRY_VERIFY(table->verticalScrollBar()->maximum() > 0);

    const auto sendWheel = [table](int angleDelta, Qt::ScrollPhase phase) {
        const QPoint localPosition = table->viewport()->rect().center();
        QWheelEvent event(localPosition, table->viewport()->mapToGlobal(localPosition), QPoint(), QPoint(0, angleDelta), Qt::NoButton, Qt::NoModifier, phase, false);
        QCoreApplication::sendEvent(table->viewport(), &event);
    };

    const auto verifyBottomBoundary = [table] {
        QCOMPARE(table->verticalScrollBar()->value(), table->verticalScrollBar()->maximum());
        const QModelIndex last = table->model()->index(table->model()->rowCount() - 1, 0);
        QTRY_VERIFY(table->visualRect(last).top() >= table->viewport()->rect().top());
        QTRY_VERIFY(table->visualRect(last).bottom() <= table->viewport()->rect().bottom());
        QVERIFY(table->viewport()->rect().bottom() - table->visualRect(last).bottom() <= table->sizeHintForRow(last.row()));
    };

    table->verticalScrollBar()->setValue(table->verticalScrollBar()->minimum());
    sendWheel(0, Qt::ScrollBegin);
    for (int step = 0; step < 4; ++step) {
        sendWheel(-120, Qt::ScrollUpdate);
    }
    sendWheel(0, Qt::ScrollEnd);
    QTRY_VERIFY(table->verticalScrollBar()->value() > table->verticalScrollBar()->minimum());

    const int baseHeight = qMax(480, overview.minimumSizeHint().height());
    int maximumByHeight[2] = {-1, -1};
    for (int pass = 0; pass < 6; ++pass) {
        const int heightIndex = pass % 2;
        overview.resize(900, baseHeight + heightIndex);
        QTRY_COMPARE(overview.height(), baseHeight + heightIndex);
        QTRY_VERIFY(table->verticalScrollBar()->maximum() > 0);
        if (maximumByHeight[heightIndex] < 0) {
            maximumByHeight[heightIndex] = table->verticalScrollBar()->maximum();
        } else {
            QCOMPARE(table->verticalScrollBar()->maximum(), maximumByHeight[heightIndex]);
        }

        table->verticalScrollBar()->setValue(table->verticalScrollBar()->maximum());
        sendWheel(0, Qt::ScrollBegin);
        for (int step = 0; step < 4; ++step) {
            sendWheel(-120, Qt::ScrollUpdate);
        }
        sendWheel(0, Qt::ScrollEnd);
        QTRY_COMPARE(table->verticalScrollBar()->value(), table->verticalScrollBar()->maximum());
        verifyBottomBoundary();
    }

    table->verticalScrollBar()->setValue(table->verticalScrollBar()->minimum());
    sendWheel(0, Qt::ScrollBegin);
    for (int step = 0; step < 4; ++step) {
        sendWheel(120, Qt::ScrollUpdate);
    }
    sendWheel(0, Qt::ScrollEnd);
    QTRY_COMPARE(table->verticalScrollBar()->value(), table->verticalScrollBar()->minimum());

    const QModelIndex selected = table->model()->index(0, 0);
    table->selectionModel()->setCurrentIndex(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    const QString selectedComment = selected.data().toString();
    table->scrollTo(table->model()->index(30, 0), QAbstractItemView::PositionAtTop);
    QTRY_VERIFY(table->verticalScrollBar()->value() > 0);
    // The date column comes first, so read the comment text of the top row.
    const QString topVisibleComment = table->indexAt(QPoint(4, 4)).siblingAtColumn(0).data().toString();
    QVERIFY(!topVisibleComment.isEmpty());
    QVERIFY(topVisibleComment != selectedComment);

    records.append(comment(QStringLiteral("latest"), QStringLiteral("Latest comment"), 61, firstActivity.addDays(1).toString(Qt::ISODate), QStringLiteral("anchored")));
    QVERIFY(writeFile(sidecar, serializedRecords(records)));
    QTRY_COMPARE_WITH_TIMEOUT(overview.commentCount(), 61, 3000);
    QTRY_COMPARE(table->indexAt(QPoint(4, 4)).siblingAtColumn(0).data().toString(), topVisibleComment);
    QCOMPARE(table->currentIndex().data().toString(), selectedComment);

    const int fallbackScrollValue = table->verticalScrollBar()->value();
    records.erase(std::remove_if(records.begin(), records.end(), [&topVisibleComment](const QJsonObject &record) { return record.value(QStringLiteral("contents")).toString() == topVisibleComment; }), records.end());
    QVERIFY(writeFile(sidecar, serializedRecords(records)));
    QTRY_COMPARE_WITH_TIMEOUT(overview.commentCount(), 60, 3000);
    QTRY_COMPARE(table->verticalScrollBar()->value(), qMin(fallbackScrollValue, table->verticalScrollBar()->maximum()));
    QVERIFY(table->verticalScrollBar()->value() > 0);
}

void CommentsOverviewTest::testAutosavePreservesEditorState()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    QString contents;
    for (int line = 0; line < 80; ++line) {
        contents += QStringLiteral("Line %1 contains enough comment text for cursor testing.\n").arg(line);
    }
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"),
                      jsonLine(comment(QStringLiteral("editable"), contents, 2, QStringLiteral("2026-08-09T14:00:00Z"), QStringLiteral("anchored")))));

    Thoughts::CommentsOverview overview(root);
    overview.resize(900, 480);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 1);

    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    QVERIFY(table);
    QVERIFY(editor);

    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(editor->toPlainText(), contents);
    QTRY_VERIFY(editor->verticalScrollBar()->maximum() > 0);
    // The window grows to fit the details panel; let it settle before the
    // scroll positions below are measured.
    settleLayouts();

    QSignalSpy editRequested(&overview, &Thoughts::CommentsOverview::commentEditRequested);
    const QVariantMap success {
        {QStringLiteral("success"), true},
        {QStringLiteral("partial"), false},
        {QStringLiteral("canUndo"), true},
        {QStringLiteral("canRedo"), false},
    };
    const QString pdfPath = root + QStringLiteral("/current_pdfs/alpha.pdf");

    QTextCursor cursor = editor->textCursor();
    cursor.setPosition(contents.indexOf(QStringLiteral("Line 40")) + 4);
    editor->setTextCursor(cursor);
    overview.raise();
    overview.activateWindow();
    QApplication::setActiveWindow(&overview);
    editor->setFocus();
    QTRY_VERIFY(overview.focusWidget() == editor);
    QTest::keyClick(editor, Qt::Key_X);
    const int middlePosition = editor->textCursor().position();
    const int middleAnchor = editor->textCursor().anchor();
    QCoreApplication::processEvents();
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum() / 2);
    const int middleScroll = editor->verticalScrollBar()->value();
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 1, 1000);
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation, pdfPath, QStringLiteral("editable"), success);
    QTRY_VERIFY(overview.focusWidget() == editor);
    QCOMPARE(editor->textCursor().position(), middlePosition);
    QCOMPARE(editor->textCursor().anchor(), middleAnchor);
    QCOMPARE(editor->verticalScrollBar()->value(), middleScroll);

    cursor = editor->textCursor();
    cursor.movePosition(QTextCursor::End);
    editor->setTextCursor(cursor);
    QTest::keyClick(editor, Qt::Key_Exclam);
    cursor = editor->textCursor();
    cursor.setPosition(12);
    cursor.setPosition(36, QTextCursor::KeepAnchor);
    editor->setTextCursor(cursor);
    QCoreApplication::processEvents();
    editor->verticalScrollBar()->setValue(editor->verticalScrollBar()->maximum() / 3);
    const int selectionPosition = editor->textCursor().position();
    const int selectionAnchor = editor->textCursor().anchor();
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 2, 1000);
    const int selectionScroll = editor->verticalScrollBar()->value();
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation, pdfPath, QStringLiteral("editable"), success);
    QCOMPARE(editor->textCursor().position(), selectionPosition);
    QCOMPARE(editor->textCursor().anchor(), selectionAnchor);
    QCOMPARE(editor->verticalScrollBar()->value(), selectionScroll);

    cursor = editor->textCursor();
    cursor.clearSelection();
    cursor.setPosition(8);
    editor->setTextCursor(cursor);
    QTest::keyClick(editor, Qt::Key_Y);
    const int failedPosition = editor->textCursor().position();
    const int failedAnchor = editor->textCursor().anchor();
    const int failedScroll = editor->verticalScrollBar()->value();
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 3, 1000);
    const QVariantMap failed {
        {QStringLiteral("success"), false},
        {QStringLiteral("partial"), false},
        {QStringLiteral("error"), QStringLiteral("Test failure")},
    };
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation, pdfPath, QStringLiteral("editable"), failed);
    QCOMPARE(editor->textCursor().position(), failedPosition);
    QCOMPARE(editor->textCursor().anchor(), failedAnchor);
    QCOMPARE(editor->verticalScrollBar()->value(), failedScroll);
    QVERIFY(editor->isEnabled());
}

void CommentsOverviewTest::testAutosaveNeverBlocksUi()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString pdfPath = root + QStringLiteral("/current_pdfs/alpha.pdf");
    const QString sidecarPath = root + QStringLiteral("/comments/alpha_comments.jsonl");
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(sidecarPath, jsonLine(comment(QStringLiteral("editable"), QStringLiteral("Original"), 1, QStringLiteral("2026-08-12T12:00:00Z"), QStringLiteral("anchored")))));

    const QByteArray lockIdentity = QFileInfo(root).absoluteFilePath().toUtf8() + '\0' + QByteArrayLiteral("alpha");
    const QString lockDirectory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).filePath(QStringLiteral("thoughts-server/comment-sync-locks"));
    const QString lockPath = QDir(lockDirectory).filePath(QStringLiteral("comment-sync-%1.lock")
                                                              .arg(QString::fromLatin1(QCryptographicHash::hash(lockIdentity, QCryptographicHash::Sha256).toHex().left(16))));
    QVERIFY(QDir().mkpath(QFileInfo(lockPath).absolutePath()));
    QLockFile competingWriter(lockPath);
    competingWriter.setStaleLockTime(30000);
    QVERIFY(competingWriter.tryLock());

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 1);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    QVERIFY(table);
    QVERIFY(editor);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(editor->toPlainText(), QStringLiteral("Original"));

    QSignalSpy editRequested(&overview, &Thoughts::CommentsOverview::commentEditRequested);
    QSignalSpy modelReset(table->model(), &QAbstractItemModel::modelReset);
    editor->setFocus();
    editor->setPlainText(QStringLiteral("Responsive"));
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 1, 1000);
    QVERIFY(editor->isEnabled());
    QVERIFY(overview.cursor().shape() != Qt::WaitCursor);

    QElapsedTimer keyTimer;
    keyTimer.start();
    editor->moveCursor(QTextCursor::End);
    editor->insertPlainText(QStringLiteral("!"));
    QVERIFY2(keyTimer.elapsed() < 16, qPrintable(QStringLiteral("editor update took %1 ms").arg(keyTimer.elapsed())));
    QCOMPARE(editor->toPlainText(), QStringLiteral("Responsive!"));
    QVERIFY(editor->isEnabled());

    competingWriter.unlock();
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 2, 1000);
    const auto storedContents = [&sidecarPath] {
        const QList<QByteArray> lines = readFile(sidecarPath).split('\n');
        return lines.isEmpty() ? QString() : QJsonDocument::fromJson(lines.constFirst()).object().value(QStringLiteral("contents")).toString();
    };
    // Overview is now an optimistic UI only; the connected Part owns the
    // fsynced journal and canonical sidecar rewrite.
    QCOMPARE(storedContents(), QStringLiteral("Original"));
    QCOMPARE(modelReset.size(), 0);
    QVERIFY(editor->isEnabled());
}

void CommentsOverviewTest::testCommentDetailsAndActions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));

    QJsonObject trackedComment = comment(QStringLiteral("editable"), QStringLiteral("Original comment"), 2, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("anchored"));
    trackedComment.insert(QStringLiteral("selected_text"), QStringLiteral("Quoted source text"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(trackedComment)));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QCOMPARE(overview.windowTitle(), QStringLiteral("Thoughts"));
    QTRY_COMPARE(overview.commentCount(), 1);

    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *metadata = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewMetadata"));
    auto *selectedText = overview.findChild<QLabel *>(QStringLiteral("commentsOverviewSelectedText"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    auto *saveButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewSave"));
    auto *deleteButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDelete"));
    auto *replyButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewReply"));
    auto *undoButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewUndo"));
    auto *redoButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewRedo"));
    QVERIFY(table);
    QVERIFY(metadata);
    QVERIFY(selectedText);
    QVERIFY(editor);
    QVERIFY(saveButton);
    QVERIFY(deleteButton);
    QVERIFY(replyButton);
    QVERIFY(undoButton);
    QVERIFY(redoButton);

    const QModelIndex commentIndex = table->model()->index(0, 0);
    table->selectionModel()->setCurrentIndex(commentIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_COMPARE(editor->toPlainText(), QStringLiteral("Original comment"));
    QVERIFY(metadata->text().contains(QStringLiteral("alpha")));
    QVERIFY(metadata->text().contains(QStringLiteral("Document: alpha · Page 2")));
    QVERIFY(metadata->text().startsWith(QStringLiteral("Project: ")));
    QVERIFY(metadata->text().contains(QStringLiteral("\nLinking status: ")));
    QVERIFY(selectedText->text().contains(QStringLiteral("Quoted source text")));
    QVERIFY(editor->isEnabled());
    QVERIFY(deleteButton->isEnabled());
    QVERIFY(replyButton->isEnabled());
    QVERIFY(!saveButton->isEnabled());
    QVERIFY(saveButton->isHidden());

    QSignalSpy editRequested(&overview, &Thoughts::CommentsOverview::commentEditRequested);
    QSignalSpy activated(&overview, &Thoughts::CommentsOverview::commentActivated);
    editor->setPlainText(QStringLiteral("Edited from Overview"));
    overview.raise();
    overview.activateWindow();
    QApplication::setActiveWindow(&overview);
    editor->setFocus();
    QTRY_VERIFY(overview.focusWidget() == editor);
    QTest::keyClick(editor, Qt::Key_Return);
    QVERIFY(editor->toPlainText().contains(QLatin1Char('\n')));
    QCOMPARE(activated.size(), 0);
    editor->setPlainText(QStringLiteral("Edited from Overview"));
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 1, 1000);
    QCOMPARE(editRequested.constFirst().at(1).toString(), QStringLiteral("editable"));
    QCOMPARE(editRequested.constFirst().at(2).toInt(), 2);
    QCOMPARE(editRequested.constFirst().at(3).toString(), QStringLiteral("Edited from Overview"));

    const QString pdfPath = root + QStringLiteral("/current_pdfs/alpha.pdf");
    const QVariantMap success {
        {QStringLiteral("success"), true},
        {QStringLiteral("partial"), false},
        {QStringLiteral("canUndo"), true},
        {QStringLiteral("canRedo"), false},
    };
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation, pdfPath, QStringLiteral("editable"), success);
    QTRY_VERIFY(overview.focusWidget() == editor);
    QVERIFY(undoButton->isEnabled());
    QVERIFY(!redoButton->isEnabled());

    const int editsBeforeUndo = editRequested.size();
    QTest::mouseClick(undoButton, Qt::LeftButton);
    QCOMPARE(editRequested.size(), editsBeforeUndo + 1);
    QVERIFY(!undoButton->isEnabled());
    QVERIFY(redoButton->isEnabled());

    QTest::mouseClick(redoButton, Qt::LeftButton);
    QCOMPARE(editRequested.size(), editsBeforeUndo + 2);
    QVERIFY(undoButton->isEnabled());
    QVERIFY(!redoButton->isEnabled());

    editor->setPlainText(QStringLiteral("Original comment"));
    editor->moveCursor(QTextCursor::End);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_Exclam);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Original comment!"));
    const int overviewEditCount = editRequested.size();
    QTest::keySequence(editor, QKeySequence::Undo);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Original comment"));
    QCOMPARE(editRequested.size(), overviewEditCount);

    // The date column comes first; click the visible part of the current row.
    QTest::mouseClick(table->viewport(), Qt::LeftButton, Qt::NoModifier, table->visualRect(table->currentIndex()).intersected(table->viewport()->rect()).center());
    QTRY_VERIFY(overview.focusWidget() == table);
    QTest::keySequence(table, QKeySequence::Undo);
    QCOMPARE(editRequested.size(), overviewEditCount + 2);
    QTest::keySequence(table, QKeySequence::Redo);
    QCOMPARE(editRequested.size(), overviewEditCount + 3);

    QSignalSpy deleteRequested(&overview, &Thoughts::CommentsOverview::commentsDeleteRequested);
    editor->setPlainText(QStringLiteral("Original comment!"));
    editor->moveCursor(QTextCursor::End);
    editor->setFocus();
    QTest::keyClick(editor, Qt::Key_Backspace);
    QCOMPARE(editor->toPlainText(), QStringLiteral("Original comment"));
    QCOMPARE(deleteRequested.size(), 0);

    table->setFocus();
    QTest::keyClick(table, Qt::Key_Backspace);
    QCOMPARE(deleteRequested.size(), 1);
    const auto requestedTargets = deleteRequested.constFirst().at(0).value<QList<Thoughts::TrackedCommentTarget>>();
    QCOMPARE(requestedTargets.size(), 1);
    QCOMPARE(requestedTargets.first().annotationId, QStringLiteral("editable"));
    QCOMPARE(requestedTargets.first().page, 2);
    const QVariantMap failed {
        {QStringLiteral("success"), false},
        {QStringLiteral("partial"), false},
        {QStringLiteral("error"), QStringLiteral("Test failure")},
    };
    overview.completeOperation(Thoughts::CommentsOverview::DeleteOperation, pdfPath, QStringLiteral("editable"), failed);

    table->setFocus();
    QTest::keyClick(table, Qt::Key_Delete);
    QCOMPARE(deleteRequested.size(), 2);
    overview.completeOperation(Thoughts::CommentsOverview::DeleteOperation, pdfPath, QStringLiteral("editable"), failed);
    QTRY_VERIFY(deleteButton->isEnabled());

    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QCOMPARE(deleteRequested.size(), 3);
    const auto buttonTargets = deleteRequested.at(2).at(0).value<QList<Thoughts::TrackedCommentTarget>>();
    QCOMPARE(buttonTargets.size(), 1);
    QCOMPARE(buttonTargets.first().annotationId, QStringLiteral("editable"));
    QCOMPARE(buttonTargets.first().page, 2);
    overview.completeOperation(Thoughts::CommentsOverview::DeleteOperation, pdfPath, QStringLiteral("editable"), success);
}

void CommentsOverviewTest::testReplyActions()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"),
                      jsonLine(comment(QStringLiteral("reply-parent"), QStringLiteral("Comment with a reply action"), 2, QStringLiteral("2026-07-30T08:00:00Z"), QStringLiteral("anchored")))));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 1);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *replyButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewReply"));
    QVERIFY(table);
    QVERIFY(replyButton);
    table->selectionModel()->setCurrentIndex(table->model()->index(0, 0), QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(replyButton->isEnabled());

    QSignalSpy replyRequested(&overview, &Thoughts::CommentsOverview::commentReplyRequested);
    QTimer::singleShot(0, [] {
        for (QWidget *widget : QApplication::topLevelWidgets()) {
            if (auto *dialog = qobject_cast<QInputDialog *>(widget)) {
                QPlainTextEdit *replyEditor = dialog->findChild<QPlainTextEdit *>();
                QVERIFY(replyEditor);
                replyEditor->setPlainText(QStringLiteral("Overview reply"));
                dialog->accept();
                return;
            }
        }
        QFAIL("The reply editor did not open.");
    });
    QTest::mouseClick(replyButton, Qt::LeftButton);
    QCOMPARE(replyRequested.size(), 1);
    QCOMPARE(replyRequested.constFirst().at(0).toString(), root + QStringLiteral("/current_pdfs/alpha.pdf"));
    QCOMPARE(replyRequested.constFirst().at(1).toString(), QStringLiteral("reply-parent"));
    QCOMPARE(replyRequested.constFirst().at(2).toInt(), 2);
    QCOMPARE(replyRequested.constFirst().at(3).toString(), QStringLiteral("Overview reply"));
    QVERIFY(replyRequested.constFirst().at(4).toMap().isEmpty());
    overview.completeReply(root + QStringLiteral("/current_pdfs/alpha.pdf"), QStringLiteral("reply-parent"), {{QStringLiteral("success"), false}, {QStringLiteral("error"), QStringLiteral("Expected test failure")}});
    QVERIFY(replyButton->isEnabled());
}

void CommentsOverviewTest::testOpenCodexAction()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QJsonObject firstRecord = comment(QStringLiteral("first"), QStringLiteral("First comment"), 1, QStringLiteral("2026-08-09T08:00:00Z"), QStringLiteral("anchored"));
    QJsonObject secondRecord = comment(QStringLiteral("second"), QStringLiteral("Second comment"), 2, QStringLiteral("2026-08-09T09:00:00Z"), QStringLiteral("anchored"));
    secondRecord.insert(QStringLiteral("chatgpt_url"), QStringLiteral("https://chatgpt.com/share/existing"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(firstRecord) + jsonLine(secondRecord)));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 2);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *button = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewOpenCodex"));
    auto *chatGptEditor = overview.findChild<QLineEdit *>(QStringLiteral("commentsOverviewChatGptUrl"));
    auto *openChatGptButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewOpenChatGpt"));
    auto *longitudeEditor = overview.findChild<QDoubleSpinBox *>(QStringLiteral("commentsOverviewLocationLongitude"));
    QVERIFY(table);
    QVERIFY(button);
    QVERIFY(chatGptEditor);
    QVERIFY(openChatGptButton);
    QVERIFY(longitudeEditor);
    QCOMPARE(button->text(), QStringLiteral("Open Codex"));
    QCOMPARE(button->parentWidget(), chatGptEditor->parentWidget());
    QCOMPARE(openChatGptButton->parentWidget(), chatGptEditor->parentWidget());

    table->selectionModel()->clear();
    QTRY_VERIFY(!button->isEnabled());
    const QModelIndex first = table->model()->index(0, 0);
    const QModelIndex second = table->model()->index(1, 0);
    table->selectionModel()->setCurrentIndex(first, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(button->isEnabled());
    {
        const QStringList clipped = clippedDetailsControls(overview);
        QVERIFY2(clipped.isEmpty(), qPrintable(clipped.join(QStringLiteral(", "))));
    }
    QCOMPARE(chatGptEditor->text(), QStringLiteral("https://chatgpt.com/share/existing"));
    QTRY_VERIFY(openChatGptButton->isEnabled());

    QSignalSpy openRequested(&overview, &Thoughts::CommentsOverview::openCodexRequested);
    QTest::mouseClick(button, Qt::LeftButton);
    QCOMPARE(openRequested.size(), 1);
    QCOMPARE(openRequested.constFirst().at(0).toString(), root + QStringLiteral("/current_pdfs/alpha.pdf"));
    QCOMPARE(openRequested.constFirst().at(1).toString(), QStringLiteral("second"));

    QSignalSpy chatGptUpdateRequested(&overview, &Thoughts::CommentsOverview::commentChatGptUrlUpdateRequested);
    chatGptEditor->setText(QStringLiteral("https://chatgpt.com/c/replacement"));
    QTest::keyClick(chatGptEditor, Qt::Key_Return);
    QCOMPARE(chatGptUpdateRequested.size(), 1);
    QCOMPARE(chatGptUpdateRequested.constFirst().at(0).toString(), root + QStringLiteral("/current_pdfs/alpha.pdf"));
    QCOMPARE(chatGptUpdateRequested.constFirst().at(1).toString(), QStringLiteral("second"));
    QCOMPARE(chatGptUpdateRequested.constFirst().at(2).toString(), QStringLiteral("https://chatgpt.com/c/replacement"));
    overview.completeChatGptUrlUpdate(root + QStringLiteral("/current_pdfs/alpha.pdf"),
                                      QStringLiteral("second"),
                                      QVariantMap {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}});
    QTRY_VERIFY(openChatGptButton->isEnabled());

    table->selectionModel()->select(second, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 2);
    QTRY_VERIFY(!button->isEnabled());
}

void CommentsOverviewTest::testOpenSourceAction()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString sourcePath = root + QStringLiteral("/docs-test/tex/source/main.tex");
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/alpha.pdf"), QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(sourcePath, QByteArrayLiteral("first\nsecond\nthird\n")));

    QJsonObject anchored = comment(QStringLiteral("anchored"), QStringLiteral("Anchored comment"), 2, QStringLiteral("2026-08-09T08:00:00Z"), QStringLiteral("anchored"));
    QJsonObject needsAnchor = comment(QStringLiteral("needs-anchor"), QStringLiteral("Needs anchor comment"), 3, QStringLiteral("2026-08-09T09:00:00Z"), QStringLiteral("needs_anchor"));
    QJsonObject missingLine = comment(QStringLiteral("missing-line"), QStringLiteral("Missing line comment"), 4, QStringLiteral("2026-08-09T10:00:00Z"), QStringLiteral("anchored"));
    missingLine.insert(QStringLiteral("tex_line"), 0);
    QJsonObject missingFile = comment(QStringLiteral("missing-file"), QStringLiteral("Missing file comment"), 5, QStringLiteral("2026-08-09T11:00:00Z"), QStringLiteral("anchored"));
    missingFile.insert(QStringLiteral("tex_file"), QStringLiteral("docs-test/tex/source/missing.tex"));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(anchored) + jsonLine(needsAnchor) + jsonLine(missingLine) + jsonLine(missingFile)));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 4);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *openCodexButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewOpenCodex"));
    auto *sourceButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewOpenSource"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    QVERIFY(table);
    QVERIFY(openCodexButton);
    QVERIFY(sourceButton);
    QVERIFY(editor);
    QCOMPARE(sourceButton->text(), QStringLiteral("Open Source in VS Code"));
    QFormLayout *actionsLayout = nullptr;
    int sourceRow = -1;
    QFormLayout::ItemRole sourceRole = QFormLayout::SpanningRole;
    for (QFormLayout *layout : overview.findChildren<QFormLayout *>()) {
        layout->getWidgetPosition(sourceButton, &sourceRow, &sourceRole);
        if (sourceRow >= 0) {
            actionsLayout = layout;
            break;
        }
    }
    QVERIFY(actionsLayout);
    int assistantRow = -1;
    QFormLayout::ItemRole assistantRole = QFormLayout::SpanningRole;
    actionsLayout->getWidgetPosition(openCodexButton->parentWidget(), &assistantRow, &assistantRole);
    QCOMPARE(sourceRow, assistantRow + 1);

    const auto indexForComment = [table](const QString &contents) {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const QModelIndex index = table->model()->index(row, 0);
            if (index.data().toString() == contents) {
                return index;
            }
        }
        return QModelIndex();
    };
    const QModelIndex anchoredIndex = indexForComment(QStringLiteral("Anchored comment"));
    const QModelIndex needsAnchorIndex = indexForComment(QStringLiteral("Needs anchor comment"));
    const QModelIndex missingLineIndex = indexForComment(QStringLiteral("Missing line comment"));
    const QModelIndex missingFileIndex = indexForComment(QStringLiteral("Missing file comment"));
    QVERIFY(anchoredIndex.isValid());
    QVERIFY(needsAnchorIndex.isValid());
    QVERIFY(missingLineIndex.isValid());
    QVERIFY(missingFileIndex.isValid());

    QSignalSpy openRequested(&overview, &Thoughts::CommentsOverview::openSourceRequested);
    table->selectionModel()->setCurrentIndex(anchoredIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(sourceButton->isEnabled());
    QVERIFY(sourceButton->toolTip().contains(QStringLiteral(":12")));
    QTest::mouseClick(sourceButton, Qt::LeftButton);
    QCOMPARE(openRequested.size(), 1);
    QCOMPARE(openRequested.constFirst().at(0).toString(), QFileInfo(sourcePath).canonicalFilePath());
    QCOMPARE(openRequested.constFirst().at(1).toInt(), 12);

    table->selectionModel()->setCurrentIndex(needsAnchorIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(!sourceButton->isEnabled());
    QVERIFY(sourceButton->toolTip().contains(QStringLiteral("trustworthy")));
    table->selectionModel()->setCurrentIndex(missingLineIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(!sourceButton->isEnabled());
    table->selectionModel()->setCurrentIndex(missingFileIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(!sourceButton->isEnabled());

    table->selectionModel()->setCurrentIndex(anchoredIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    table->selectionModel()->select(needsAnchorIndex, QItemSelectionModel::Select | QItemSelectionModel::Rows);
    QTRY_COMPARE(table->selectionModel()->selectedRows().size(), 2);
    QTRY_VERIFY(!sourceButton->isEnabled());

    const auto sourceMenuEnabled = [&](const QModelIndex &clicked, bool trigger) {
        bool found = false;
        bool enabled = false;
        QTimer::singleShot(0, &overview, [&] {
            auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
            if (!menu) { return; }
            auto *action = menu->findChild<QAction *>(QStringLiteral("commentsOverviewOpenSourceAction"));
            found = action != nullptr;
            enabled = action && action->isEnabled();
            if (enabled && trigger) { action->trigger(); }
            menu->close();
        });
        QMetaObject::invokeMethod(&overview, "showCommentsContextMenu", Qt::DirectConnection, Q_ARG(QPoint, table->visualRect(clicked).center()));
        return found && enabled;
    };
    QVERIFY(!sourceMenuEnabled(anchoredIndex, false));
    table->selectionModel()->setCurrentIndex(missingFileIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QVERIFY(!sourceMenuEnabled(missingFileIndex, false));
    // Right-clicking an unselected row must open that row's source.
    QVERIFY(sourceMenuEnabled(anchoredIndex, true));
    QCOMPARE(openRequested.size(), 2);
    QCOMPARE(openRequested.constLast().at(0).toString(), QFileInfo(sourcePath).canonicalFilePath());
    QCOMPARE(openRequested.constLast().at(1).toInt(), 12);

    table->selectionModel()->setCurrentIndex(anchoredIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    QTRY_VERIFY(sourceButton->isEnabled());
    QSignalSpy editRequested(&overview, &Thoughts::CommentsOverview::commentEditRequested);
    editor->setPlainText(QStringLiteral("Changed comment"));
    QTRY_COMPARE_WITH_TIMEOUT(editRequested.size(), 1, 1000);
    QTRY_VERIFY(sourceButton->isEnabled());
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation,
                               root + QStringLiteral("/current_pdfs/alpha.pdf"),
                               QStringLiteral("anchored"),
                               QVariantMap {{QStringLiteral("success"), false}, {QStringLiteral("partial"), false}, {QStringLiteral("error"), QStringLiteral("Test failure")}});
    QTRY_VERIFY(sourceButton->isEnabled());
}

void CommentsOverviewTest::testCommentCodexLauncher()
{
#ifdef Q_OS_MACOS
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString docsRoot = temporary.path() + QStringLiteral("/docs");
    const QString codexHome = temporary.path() + QStringLiteral("/codex-home");
    const QString pdfPath = docsRoot + QStringLiteral("/current_pdfs/alpha.pdf");
    const QString codexExecutable = temporary.path() + QStringLiteral("/bin/codex");
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(codexExecutable, QByteArrayLiteral("#!/bin/zsh\nexit 0\n")));
    QVERIFY(QFile::setPermissions(codexExecutable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    const QString canonicalDocsRoot = QFileInfo(docsRoot).canonicalFilePath();

    int launches = 0;
    QString launchedProgram;
    QStringList launchedArguments;
    QString focusedTty;
    bool focusSucceeds = false;
    Thoughts::CommentCodexLauncher::Options options;
    options.codexExecutable = codexExecutable;
    options.codexHome = codexHome;
    options.startDetached = [&](const QString &program, const QStringList &arguments) {
        ++launches;
        launchedProgram = program;
        launchedArguments = arguments;
        return true;
    };
    options.focusTerminal = [&](const QString &tty) {
        focusedTty = tty;
        return focusSucceeds;
    };
    Thoughts::CommentCodexLauncher launcher(docsRoot, options);

    const QString annotationId = QStringLiteral("comment-a");
    const QString key = Thoughts::CommentCodexLauncher::associationKey(pdfPath, annotationId);
    QCOMPARE(key.size(), 64);
    QCOMPARE(key, Thoughts::CommentCodexLauncher::associationKey(pdfPath, annotationId));
    QVERIFY(key != Thoughts::CommentCodexLauncher::associationKey(pdfPath, QStringLiteral("comment-b")));
    const QString runtime = canonicalDocsRoot + QStringLiteral("/.runtime/target/okular-codex-comments/") + key;
    QString error;
    QVERIFY2(launcher.openComment(pdfPath, annotationId, &error), qPrintable(error));
    QCOMPARE(launches, 1);
    QCOMPARE(launchedProgram, QStringLiteral("/usr/bin/open"));
    QCOMPARE(launchedArguments.mid(0, 2), QStringList({QStringLiteral("-a"), QStringLiteral("Terminal")}));
    const QString scriptPath = launchedArguments.constLast();
    QFile firstScriptFile(scriptPath);
    QVERIFY(firstScriptFile.open(QIODevice::ReadOnly));
    const QString firstScriptContents = QString::fromUtf8(firstScriptFile.readAll());
    // Codex keeps its approval prompts and its sandbox unless the owner turns them off.
    QVERIFY(firstScriptContents.contains(QStringLiteral("exec '%1' -C '%2'").arg(codexExecutable, canonicalDocsRoot)));
    QVERIFY(!firstScriptContents.contains(QStringLiteral("--yolo")));
    QVERIFY(!firstScriptContents.contains(QStringLiteral(" resume -C ")));
    QVERIFY(QFileInfo::exists(runtime + QStringLiteral("/launching")));

    QVERIFY(launcher.openComment(pdfPath, annotationId, &error));
    QCOMPARE(launches, 1);
    QVERIFY(QFile::remove(runtime + QStringLiteral("/launching")));
    QVERIFY(writeFile(runtime + QStringLiteral("/active.tty"), QByteArrayLiteral("/dev/ttys099\n")));
    focusSucceeds = true;
    QVERIFY(launcher.openComment(pdfPath, annotationId, &error));
    QVERIFY(focusedTty.isEmpty());
    QCOMPARE(launches, 2);
    QVERIFY(!QFileInfo::exists(runtime + QStringLiteral("/active.tty")));

    QVERIFY(QFile::remove(runtime + QStringLiteral("/launching")));
    QVERIFY(writeFile(runtime + QStringLiteral("/active.tty"), QByteArrayLiteral("/dev/ttys099\n")));
    QVERIFY(writeFile(runtime + QStringLiteral("/active.pid"), QByteArray::number(QCoreApplication::applicationPid()) + '\n'));
    QVERIFY(launcher.openComment(pdfPath, annotationId, &error));
    QCOMPARE(focusedTty, QStringLiteral("/dev/ttys099"));
    QCOMPARE(launches, 2);

    focusSucceeds = false;
    QVERIFY(!launcher.openComment(pdfPath, annotationId, &error));
    QVERIFY(error.contains(QStringLiteral("could not be focused")));
    QCOMPARE(launches, 2);
    QVERIFY(QFile::remove(runtime + QStringLiteral("/active.pid")));
    QVERIFY(QFile::remove(runtime + QStringLiteral("/active.tty")));
    const QString sessionId = QStringLiteral("019fe7aa-1111-7222-8333-123456789abc");
    const QString rolloutPath = codexHome + QStringLiteral("/sessions/2026/08/09/rollout-test-") + sessionId + QStringLiteral(".jsonl");
    const QJsonObject sessionMeta {
        {QStringLiteral("type"), QStringLiteral("session_meta")},
        {QStringLiteral("payload"), QJsonObject {{QStringLiteral("id"), sessionId}, {QStringLiteral("cwd"), docsRoot}}},
    };
    QVERIFY(writeFile(rolloutPath, jsonLine(sessionMeta)));
    QVERIFY(writeFile(runtime + QStringLiteral("/session.path"), rolloutPath.toUtf8() + '\n'));
    QVERIFY(launcher.openComment(pdfPath, annotationId, &error));
    QCOMPARE(launches, 3);
    QFile resumeScriptFile(launchedArguments.constLast());
    QVERIFY(resumeScriptFile.open(QIODevice::ReadOnly));
    const QString resumeScript = QString::fromUtf8(resumeScriptFile.readAll());
    QVERIFY(resumeScript.contains(QStringLiteral("exec '%1' resume -C '%2' '%3'").arg(codexExecutable, canonicalDocsRoot, sessionId)));
    QVERIFY(!resumeScript.contains(QStringLiteral("--yolo")));
    QVERIFY(QFileInfo::exists(runtime + QStringLiteral("/association.json")));

    QVERIFY(QFile::remove(runtime + QStringLiteral("/launching")));
    const QString archivedPath = codexHome + QStringLiteral("/archived_sessions/") + QFileInfo(rolloutPath).fileName();
    QVERIFY(QDir().mkpath(QFileInfo(archivedPath).absolutePath()));
    QVERIFY(QFile::rename(rolloutPath, archivedPath));
    QVERIFY(launcher.openComment(pdfPath, annotationId, &error));
    QCOMPARE(launches, 4);
    QFile archivedScriptFile(launchedArguments.constLast());
    QVERIFY(archivedScriptFile.open(QIODevice::ReadOnly));
    const QString archivedScript = QString::fromUtf8(archivedScriptFile.readAll());
    QVERIFY(archivedScript.contains(QStringLiteral("'%1' unarchive '%2'").arg(codexExecutable, sessionId)));
    QVERIFY(archivedScript.contains(QStringLiteral("exec '%1' resume -C '%2' '%3'").arg(codexExecutable, canonicalDocsRoot, sessionId)));
    QVERIFY(!archivedScript.contains(QStringLiteral("--yolo")));

    QVERIFY(QFile::remove(runtime + QStringLiteral("/launching")));
    QVERIFY(QFile::remove(archivedPath));
    QVERIFY(launcher.openComment(pdfPath, annotationId, &error));
    QCOMPARE(launches, 5);
    QFile replacementScriptFile(launchedArguments.constLast());
    QVERIFY(replacementScriptFile.open(QIODevice::ReadOnly));
    const QString replacementScript = QString::fromUtf8(replacementScriptFile.readAll());
    QVERIFY(replacementScript.contains(QStringLiteral("exec '%1' -C '%2'").arg(codexExecutable, canonicalDocsRoot)));
    QVERIFY(!replacementScript.contains(QStringLiteral("--yolo")));
    QVERIFY(!replacementScript.contains(QStringLiteral(" resume -C ")));
    QVERIFY(!QFileInfo::exists(runtime + QStringLiteral("/association.json")));
#endif
}

void CommentsOverviewTest::testCommentCodexBypassIsOffUnlessTurnedOn()
{
    const QString docsRoot = QStringLiteral("/tmp/docs");
    const QString runtime = QStringLiteral("/tmp/runtime");
    const QString codexHome = QStringLiteral("/tmp/codex-home");
    const QString codex = QStringLiteral("/tmp/bin/codex");
    const QString sessionId = QStringLiteral("019fe7aa-1111-7222-8333-123456789abc");
    const auto script = [&](const QString &session, bool bypass) {
        return Thoughts::CommentCodexLauncher::terminalScript(runtime, docsRoot, codexHome, codex, session, false, bypass);
    };
    // The command line with and without the option, for a new and a resumed conversation.
    QVERIFY(!Thoughts::CommentCodexLauncher::terminalScript(runtime, docsRoot, codexHome, codex, QString(), false).contains(QStringLiteral("--yolo")));
    QVERIFY(!script(QString(), false).contains(QStringLiteral("--yolo")));
    QVERIFY(!script(sessionId, false).contains(QStringLiteral("--yolo")));
#ifndef Q_OS_WIN
    QVERIFY(script(QString(), false).contains(QStringLiteral("exec '/tmp/bin/codex' -C '/tmp/docs'")));
    QVERIFY(script(sessionId, false).contains(QStringLiteral("exec '/tmp/bin/codex' resume -C '/tmp/docs' '%1'").arg(sessionId)));
    QVERIFY(script(QString(), true).contains(QStringLiteral("exec '/tmp/bin/codex' --yolo -C '/tmp/docs'")));
    QVERIFY(script(sessionId, true).contains(QStringLiteral("exec '/tmp/bin/codex' --yolo resume -C '/tmp/docs' '%1'").arg(sessionId)));
#else
    QVERIFY(script(QString(), true).contains(QStringLiteral("& '/tmp/bin/codex' --yolo -C $DocsRoot")));
    QVERIFY(script(sessionId, true).contains(QStringLiteral("& '/tmp/bin/codex' --yolo resume -C $DocsRoot '%1'").arg(sessionId)));
#endif

    // Off by default; the configuration key turns it on, and the environment has the last word.
    const bool hadEnvironment = qEnvironmentVariableIsSet("THOUGHTS_CODEX_YOLO");
    const QByteArray previousEnvironment = qgetenv("THOUGHTS_CODEX_YOLO");
    KConfigGroup group(KSharedConfig::openConfig(), QStringLiteral("Codex"));
    const auto restore = qScopeGuard([&] {
        group.deleteEntry("BypassApprovalsAndSandbox");
        if (hadEnvironment) {
            qputenv("THOUGHTS_CODEX_YOLO", previousEnvironment);
        } else {
            qunsetenv("THOUGHTS_CODEX_YOLO");
        }
    });
    qunsetenv("THOUGHTS_CODEX_YOLO");
    group.deleteEntry("BypassApprovalsAndSandbox");
    QVERIFY(!Thoughts::CommentCodexLauncher::codexBypassesApprovalsAndSandbox());
    group.writeEntry("BypassApprovalsAndSandbox", true);
    QVERIFY(Thoughts::CommentCodexLauncher::codexBypassesApprovalsAndSandbox());
    qputenv("THOUGHTS_CODEX_YOLO", "0");
    QVERIFY(!Thoughts::CommentCodexLauncher::codexBypassesApprovalsAndSandbox());
    group.deleteEntry("BypassApprovalsAndSandbox");
    qputenv("THOUGHTS_CODEX_YOLO", "1");
    QVERIFY(Thoughts::CommentCodexLauncher::codexBypassesApprovalsAndSandbox());
    qputenv("THOUGHTS_CODEX_YOLO", "");
    QVERIFY(!Thoughts::CommentCodexLauncher::codexBypassesApprovalsAndSandbox());

#ifdef Q_OS_MACOS
    // The launcher writes what the option says.
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString pdfPath = temporary.path() + QStringLiteral("/docs/current_pdfs/alpha.pdf");
    const QString executable = temporary.path() + QStringLiteral("/bin/codex");
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(executable, QByteArrayLiteral("#!/bin/zsh\nexit 0\n")));
    QVERIFY(QFile::setPermissions(executable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QStringList launchedArguments;
    Thoughts::CommentCodexLauncher::Options options;
    options.codexExecutable = executable;
    options.codexHome = temporary.path() + QStringLiteral("/codex-home");
    options.startDetached = [&](const QString &, const QStringList &arguments) {
        launchedArguments = arguments;
        return true;
    };
    options.focusTerminal = [](const QString &) { return false; };
    options.bypassApprovalsAndSandbox = true;
    Thoughts::CommentCodexLauncher launcher(temporary.path() + QStringLiteral("/docs"), options);
    QString error;
    qputenv("THOUGHTS_CODEX_YOLO", "0");
    QVERIFY2(launcher.openComment(pdfPath, QStringLiteral("comment-a"), &error), qPrintable(error));
    QVERIFY(readFile(launchedArguments.constLast()).contains(" --yolo -C "));
#endif
}

void CommentsOverviewTest::testCommentCodexTerminalScriptCapturesSession()
{
#ifdef Q_OS_MACOS
    // The script finds its process group with the setuid /bin/ps, which a
    // sandbox-exec profile refuses to run.
    QProcess ps;
    ps.start(QStringLiteral("/bin/ps"), {QStringLiteral("-o"), QStringLiteral("pgid="), QStringLiteral("-p"), QString::number(QCoreApplication::applicationPid())});
    if (!ps.waitForFinished(5000) || ps.exitStatus() != QProcess::NormalExit || ps.exitCode() != 0)
        QSKIP("/bin/ps cannot run here (setuid binaries are refused inside sandbox-exec)");
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString runtime = temporary.path() + QStringLiteral("/runtime");
    const QString docsRoot = temporary.path() + QStringLiteral("/docs");
    const QString codexHome = temporary.path() + QStringLiteral("/codex-home");
    const QString sessionId = QStringLiteral("019fe7bb-1111-7222-8333-123456789abc");
    const QString rolloutPath = codexHome + QStringLiteral("/sessions/2026/08/09/rollout-test-") + sessionId + QStringLiteral(".jsonl");
    const QString fakeCodex = temporary.path() + QStringLiteral("/bin/codex");
    const QString fakeNativeCodex = temporary.path() + QStringLiteral("/bin/codex-native");
    QVERIFY(writeFile(rolloutPath,
                      jsonLine(QJsonObject {
                          {QStringLiteral("type"), QStringLiteral("session_meta")},
                          {QStringLiteral("payload"), QJsonObject {{QStringLiteral("id"), sessionId}, {QStringLiteral("cwd"), docsRoot}}},
                      })));
    QString quotedRollout = rolloutPath;
    quotedRollout.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    QString quotedNative = fakeNativeCodex;
    quotedNative.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    const QByteArray fakeNativeScript = QStringLiteral("#!/bin/zsh\nexec 9<'%1'\n/bin/sleep 1\n").arg(quotedRollout).toUtf8();
    const QByteArray fakeWrapperScript = QStringLiteral("#!/bin/zsh\n'%1' &\nCHILD_PID=$!\nwait \"${CHILD_PID}\"\n").arg(quotedNative).toUtf8();
    QVERIFY(writeFile(fakeNativeCodex, fakeNativeScript));
    QVERIFY(QFile::setPermissions(fakeNativeCodex, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(writeFile(fakeCodex, fakeWrapperScript));
    QVERIFY(QFile::setPermissions(fakeCodex, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
    QVERIFY(QDir().mkpath(runtime));
    const QString launcherPath = runtime + QStringLiteral("/launch.command");
    QVERIFY(writeFile(launcherPath,
                      Thoughts::CommentCodexLauncher::terminalScript(runtime, docsRoot, QFileInfo(codexHome).canonicalFilePath(), fakeCodex, QString(), false).toUtf8()));
    QVERIFY(QFile::setPermissions(launcherPath, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));

    QProcess process;
    process.start(QStringLiteral("/bin/zsh"), {launcherPath});
    QVERIFY(process.waitForStarted(1000));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(runtime + QStringLiteral("/session.path")), 3000);
    QCOMPARE(readFile(runtime + QStringLiteral("/session.path")).trimmed(), QFileInfo(rolloutPath).canonicalFilePath().toUtf8());
    QVERIFY(process.waitForFinished(5000));
    QTRY_VERIFY_WITH_TIMEOUT(!QFileInfo::exists(runtime + QStringLiteral("/active.tty")), 3000);
    QVERIFY(!QFileInfo::exists(runtime + QStringLiteral("/active.pid")));
#endif
}

void CommentsOverviewTest::testDeleteKeepsUnrelatedRows()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    for (int document = 0; document < 20; ++document) {
        const QString stem = QStringLiteral("doc%1").arg(document);
        QVERIFY(writeFile(root + QStringLiteral("/current_pdfs/") + stem + QStringLiteral(".pdf"), QByteArrayLiteral("placeholder")));
        QByteArray records;
        for (int row = 0; row < 25; ++row) {
            records += jsonLine(comment(QStringLiteral("c%1").arg(row), QStringLiteral("Comment %1 %2").arg(document).arg(row),
                                        1, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored")));
        }
        QVERIFY(writeFile(root + QStringLiteral("/comments/") + stem + QStringLiteral("_comments.jsonl"), records));
    }
    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 500);
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    QVERIFY(table);
    QVERIFY(documents);
    documents->setCurrentItem(documents->topLevelItem(0));
    QTRY_COMPARE(table->model()->rowCount(), 500);
    const QVariantMap success {{QStringLiteral("success"), true}, {QStringLiteral("changed"), true}};
    QList<qint64> timings;
    for (int iteration = 0; iteration < 10; ++iteration) {
        const QString text = QStringLiteral("Comment 0 %1").arg(iteration);
        QModelIndex selected;
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const QModelIndex index = table->model()->index(row, 0);
            if (index.data().toString() == text) { selected = index; break; }
        }
        QVERIFY(selected.isValid());
        table->selectionModel()->setCurrentIndex(selected, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        QPersistentModelIndex survivor(table->model()->index(selected.row() == 0 ? 1 : 0, 0));
        const QString survivorText = survivor.data().toString();
        QSignalSpy inserted(table->model(), &QAbstractItemModel::rowsInserted);
        QSignalSpy reset(table->model(), &QAbstractItemModel::modelReset);
        QTreeWidgetItem *allProjects = documents->topLevelItem(0);
        QElapsedTimer timer;
        timer.start();
        overview.completeOperation(Thoughts::CommentsOverview::DeleteOperation, root + QStringLiteral("/current_pdfs/doc0.pdf"), QStringLiteral("c%1").arg(iteration), success);
        timings.append(timer.nsecsElapsed());
        QVERIFY(survivor.isValid());
        QCOMPARE(survivor.data().toString(), survivorText);
        QCOMPARE(inserted.size(), 0);
        QCOMPARE(reset.size(), 0);
        QCOMPARE(documents->topLevelItem(0), allProjects);
        QCOMPARE(overview.commentCount(), 499 - iteration);
        QCOMPARE(table->model()->rowCount(), 499 - iteration);
        QCOMPARE(documents->topLevelItem(0)->text(1).toInt(), 499 - iteration);
    }
    std::sort(timings.begin(), timings.end());
    qInfo() << "Deletion completion median (500 comments, 20 documents), ms:" << timings.at(timings.size() / 2) / 1000000.0;
    overview.completeBatchDelete({{root + QStringLiteral("/current_pdfs/doc1.pdf"), QStringLiteral("c0"), 1, 0},
                                 {root + QStringLiteral("/current_pdfs/doc1.pdf"), QStringLiteral("c1"), 1, 0},
                                 {root + QStringLiteral("/current_pdfs/doc2.pdf"), QStringLiteral("c0"), 1, 0}}, success);
    QCOMPARE(overview.commentCount(), 487);
    QCOMPARE(table->model()->rowCount(), 487);
    QCOMPARE(documents->topLevelItem(0)->text(1).toInt(), 487);
    QCOMPARE(documentItem(documents, QStringLiteral("doc1"))->text(1).toInt(), 23);
    QCOMPARE(documentItem(documents, QStringLiteral("doc2"))->text(1).toInt(), 24);
}

void CommentsOverviewTest::testDeleteUndoRedoRestoresSelection()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString pdfPath = root + QStringLiteral("/current_pdfs/alpha.pdf");
    const QString commentsPath = root + QStringLiteral("/comments/alpha_comments.jsonl");
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(pdfPath, QByteArrayLiteral("placeholder")));

    const QJsonObject deletedComment = comment(QStringLiteral("delete-me"), QStringLiteral("Delete me"), 1, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored"));
    const QJsonObject neighborComment = comment(QStringLiteral("neighbor"), QStringLiteral("Neighbor"), 2, QStringLiteral("2026-07-30T09:00:00Z"), QStringLiteral("anchored"));
    QVERIFY(writeFile(commentsPath, jsonLine(deletedComment) + jsonLine(neighborComment)));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 2);

    auto *documents = overview.findChild<QTreeWidget *>(QStringLiteral("commentsOverviewDocuments"));
    auto *table = overview.findChild<QTreeView *>(QStringLiteral("commentsOverviewTable"));
    auto *editor = overview.findChild<QPlainTextEdit *>(QStringLiteral("commentsOverviewEditor"));
    auto *deleteButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewDelete"));
    auto *undoButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewUndo"));
    auto *redoButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewRedo"));
    QVERIFY(documents);
    QVERIFY(table);
    QVERIFY(editor);
    QVERIFY(deleteButton);
    QVERIFY(undoButton);
    QVERIFY(redoButton);

    documents->setCurrentItem(documentItem(documents, QStringLiteral("alpha")));
    QTRY_COMPARE(table->model()->rowCount(), 2);
    const auto selectContents = [table](const QString &contents) {
        for (int row = 0; row < table->model()->rowCount(); ++row) {
            const QModelIndex index = table->model()->index(row, 0);
            if (index.data().toString() == contents) {
                table->selectionModel()->setCurrentIndex(index, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
                return true;
            }
        }
        return false;
    };
    QVERIFY(selectContents(QStringLiteral("Delete me")));
    QTRY_COMPARE(editor->toPlainText(), QStringLiteral("Delete me"));

    const QVariantMap changeSuccess {
        {QStringLiteral("success"), true},
        {QStringLiteral("partial"), false},
        {QStringLiteral("changed"), true},
        {QStringLiteral("canUndo"), true},
        {QStringLiteral("canRedo"), false},
    };
    QTest::mouseClick(deleteButton, Qt::LeftButton);
    QVERIFY(writeFile(commentsPath, jsonLine(neighborComment)));
    overview.completeOperation(Thoughts::CommentsOverview::DeleteOperation, pdfPath, QStringLiteral("delete-me"), changeSuccess);
    QTRY_COMPARE(editor->toPlainText(), QStringLiteral("Neighbor"));

    QTest::mouseClick(undoButton, Qt::LeftButton);
    QVERIFY(writeFile(commentsPath, jsonLine(deletedComment) + jsonLine(neighborComment)));
    const QVariantMap undoSuccess {
        {QStringLiteral("success"), true},
        {QStringLiteral("partial"), false},
        {QStringLiteral("changed"), true},
        {QStringLiteral("canUndo"), false},
        {QStringLiteral("canRedo"), true},
    };
    overview.completeOperation(Thoughts::CommentsOverview::UndoOperation, pdfPath, QString(), undoSuccess);
    QTRY_COMPARE(editor->toPlainText(), QStringLiteral("Delete me"));

    QTest::mouseClick(redoButton, Qt::LeftButton);
    QVERIFY(writeFile(commentsPath, jsonLine(neighborComment)));
    overview.completeOperation(Thoughts::CommentsOverview::RedoOperation, pdfPath, QString(), changeSuccess);
    QTRY_COMPARE(editor->toPlainText(), QStringLiteral("Neighbor"));
}

void CommentsOverviewTest::testHistoryInvalidationIsPerDocument()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString root = temporary.path();
    const QString alphaPath = root + QStringLiteral("/current_pdfs/alpha.pdf");
    const QString betaPath = root + QStringLiteral("/current_pdfs/beta.pdf");
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/alpha.json"), QJsonDocument(layout(QStringLiteral("alpha"))).toJson()));
    QVERIFY(writeFile(root + QStringLiteral("/comments/.layout/beta.json"), QJsonDocument(layout(QStringLiteral("beta"))).toJson()));
    QVERIFY(writeFile(alphaPath, QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(betaPath, QByteArrayLiteral("placeholder")));
    QVERIFY(writeFile(root + QStringLiteral("/comments/alpha_comments.jsonl"), jsonLine(comment(QStringLiteral("alpha-comment"), QStringLiteral("Alpha"), 1, QStringLiteral("2026-07-30T10:00:00Z"), QStringLiteral("anchored")))));
    QVERIFY(writeFile(root + QStringLiteral("/comments/beta_comments.jsonl"), jsonLine(comment(QStringLiteral("beta-comment"), QStringLiteral("Beta"), 1, QStringLiteral("2026-07-30T09:00:00Z"), QStringLiteral("anchored")))));

    Thoughts::CommentsOverview overview(root);
    overview.show();
    QTRY_COMPARE(overview.commentCount(), 2);
    auto *undoButton = overview.findChild<QPushButton *>(QStringLiteral("commentsOverviewUndo"));
    QVERIFY(undoButton);

    const QVariantMap success {
        {QStringLiteral("success"), true},
        {QStringLiteral("partial"), false},
        {QStringLiteral("changed"), true},
        {QStringLiteral("canUndo"), true},
        {QStringLiteral("canRedo"), false},
    };
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation, alphaPath, QStringLiteral("alpha-comment"), success);
    overview.completeOperation(Thoughts::CommentsOverview::EditOperation, betaPath, QStringLiteral("beta-comment"), success);

    QSignalSpy undoRequested(&overview, &Thoughts::CommentsOverview::commentUndoRequested);
    QTest::mouseClick(undoButton, Qt::LeftButton);
    QCOMPARE(undoRequested.size(), 1);
    QCOMPARE(undoRequested.constLast().at(0).toString(), betaPath);
    const QVariantMap reset {
        {QStringLiteral("success"), false},
        {QStringLiteral("partial"), false},
        {QStringLiteral("historyReset"), true},
        {QStringLiteral("canUndo"), false},
        {QStringLiteral("canRedo"), false},
        {QStringLiteral("error"), QStringLiteral("The PDF tab is closed.")},
    };
    overview.completeOperation(Thoughts::CommentsOverview::UndoOperation, betaPath, QString(), reset);
    QVERIFY(undoButton->isEnabled());

    QTest::mouseClick(undoButton, Qt::LeftButton);
    QCOMPARE(undoRequested.size(), 2);
    QCOMPARE(undoRequested.constLast().at(0).toString(), alphaPath);
}

void CommentsOverviewTest::testLibraryAgentConversations()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const QString projectId = QStringLiteral("new:course");
    const QJsonObject project {{QStringLiteral("id"), projectId}, {QStringLiteral("name"), QStringLiteral("Course")},
                              {QStringLiteral("revision"), 1}, {QStringLiteral("documents"), QJsonArray {}}};
    Thoughts::LibraryProjectStore store(temporary.path());
    QVERIFY(store.receive({{QStringLiteral("projects"), QJsonArray {project}}}).isEmpty());
    QVERIFY(registerCatalogueProject(temporary.path(), project));

    const auto record = [&](const QString &agent, const QString &conversationId, const QString &title, const QString &url, const QString &at) {
        Thoughts::AgentConversations::Touch touch;
        touch.projectId = projectId;
        touch.agent = agent;
        touch.conversationId = conversationId;
        touch.conversationUrl = url;
        touch.title = title;
        touch.titleSource = QStringLiteral("app");
        touch.sessionName = agent == QLatin1String("claude") ? QStringLiteral("alex-94") : QString();
        touch.linkedBy = QStringLiteral("tool");
        touch.tool = QStringLiteral("create_proposal");
        return Thoughts::AgentConversations::touch({}, touch, QStringLiteral("desktop-test"), QDateTime::fromString(at, Qt::ISODateWithMs), QStringLiteral("mac")).record;
    };
    const QString codexId = QStringLiteral("01900000-0000-7000-8000-000000000002");
    const QJsonObject claude = record(QStringLiteral("claude"), QStringLiteral("5ef4cd0f-6a8a-5b7a-ac7b-1e9f51cd3139"), QStringLiteral("Search on mobile"),
                                      QStringLiteral("https://claude.ai/code/session_01ExampleSessionBcdeFghi"), QStringLiteral("2026-10-26T11:00:00.000Z"));
    const QJsonObject codex = record(QStringLiteral("codex"), codexId, QStringLiteral("GRPO notes"), QStringLiteral("codex://threads/") + codexId,
                                     QStringLiteral("2026-10-26T12:00:00.000Z"));
    const QJsonObject unlinked = record(QStringLiteral("claude"), QStringLiteral("11111111-2222-4333-8444-555555555555"), QStringLiteral("Plain CLI"), QString(),
                                        QStringLiteral("2026-10-26T10:00:00.000Z"));
    const QJsonObject removed = Thoughts::AgentConversations::remove(
        record(QStringLiteral("claude"), QStringLiteral("22222222-2222-4333-8444-555555555555"), QStringLiteral("Removed"), QString(), QStringLiteral("2026-10-26T13:00:00.000Z")),
        QStringLiteral("phone"));
    QJsonObject projections;
    for (const QJsonObject &value : {claude, codex, unlinked, removed})
        projections.insert(Thoughts::AgentConversations::projectionKey(value.value(QStringLiteral("link_id")).toString()), value);

    Thoughts::CommentsOverview overview(temporary.path());
    overview.setFedoraProjections(projections);
    overview.show();
    overview.showSummary();
    auto *tree = overview.findChild<QTreeWidget *>(QStringLiteral("thoughtsOverviewLibraryTree"));
    QVERIFY(tree);
    QTRY_COMPARE(tree->topLevelItemCount(), 1);
    constexpr int identityRole = Qt::UserRole + 107;
    constexpr int orderRole = Qt::UserRole + 108;
    constexpr int conversationRole = Qt::UserRole + 161;
    const auto group = [&]() -> QTreeWidgetItem * {
        auto *root = tree->topLevelItem(0);
        for (int i = 0; i < root->childCount(); ++i)
            if (root->child(i)->data(0, identityRole).toString() == projectId + QStringLiteral("/agent-conversations")) return root->child(i);
        return nullptr;
    };
    QVERIFY(group());
    // The projections arrived before the first build: the rows follow without a full reload.
    QTRY_COMPARE(group()->text(0), QStringLiteral("Agent conversations (3)"));
    // Documents and Sources come first; the group starts collapsed.
    QCOMPARE(group()->data(0, orderRole).toInt(), 2);
    QVERIFY(!group()->isExpanded());
    QCOMPARE(group()->childCount(), 3);
    QList<QTreeWidgetItem *> rows;
    for (int i = 0; i < group()->childCount(); ++i) rows.append(group()->child(i));
    std::sort(rows.begin(), rows.end(), [&](QTreeWidgetItem *a, QTreeWidgetItem *b) { return a->data(0, orderRole).toInt() < b->data(0, orderRole).toInt(); });
    // Latest activity first; the removed record is hidden.
    QCOMPARE(rows.at(0)->text(0), QStringLiteral("GRPO notes"));
    QCOMPARE(rows.at(1)->text(0), QStringLiteral("Search on mobile"));
    QCOMPARE(rows.at(2)->text(0), QStringLiteral("Plain CLI"));
    QCOMPARE(rows.at(0)->text(2), QStringLiteral("Codex"));
    QCOMPARE(rows.at(1)->text(2), QStringLiteral("Claude"));
    QVERIFY(rows.at(1)->toolTip(0).contains(QStringLiteral("alex-94")));
    QVERIFY(rows.at(1)->toolTip(0).contains(QStringLiteral("Linked by create_proposal")));
    QVERIFY(rows.at(2)->toolTip(0).contains(QStringLiteral("not started with Remote Control")));
    QVERIFY(rows.at(0)->text(3).isEmpty());
    QVERIFY(rows.at(0)->data(0, conversationRole).isValid());

    // Double-click opens the conversation link; a row without one does nothing.
    m_openedUrls.clear();
    QDesktopServices::setUrlHandler(QStringLiteral("https"), this, "conversationUrlHandler");
    QDesktopServices::setUrlHandler(QStringLiteral("codex"), this, "conversationUrlHandler");
    Q_EMIT tree->itemDoubleClicked(rows.at(1), 0);
    Q_EMIT tree->itemDoubleClicked(rows.at(0), 0);
    Q_EMIT tree->itemDoubleClicked(rows.at(2), 0);
    QDesktopServices::unsetUrlHandler(QStringLiteral("https"));
    QDesktopServices::unsetUrlHandler(QStringLiteral("codex"));
    QCOMPARE(m_openedUrls, (QList<QUrl>{QUrl(QStringLiteral("https://claude.ai/code/session_01ExampleSessionBcdeFghi")), QUrl(QStringLiteral("codex://threads/") + codexId)}));

    // Remove from Project queues the removed next revision.
    QSignalSpy removals(&overview, &Thoughts::CommentsOverview::agentConversationRemoveRequested);
    overview.removeAgentConversation(rows.at(1)->data(0, conversationRole).value<QJsonObject>());
    QCOMPARE(removals.size(), 1);
    const QJsonObject queued = QJsonObject::fromVariantMap(removals.at(0).at(0).toMap());
    QCOMPARE(queued.value(QStringLiteral("link_id")), claude.value(QStringLiteral("link_id")));
    QCOMPARE(queued.value(QStringLiteral("removed")).toBool(), true);
    QCOMPARE(queued.value(QStringLiteral("revision")).toInt(), 2);
    QVERIFY2(Thoughts::AgentConversations::validationError(queued).isEmpty(), qPrintable(Thoughts::AgentConversations::validationError(queued)));

    // Once the queued removal is shown, the row disappears without a full reload.
    projections.insert(Thoughts::AgentConversations::projectionKey(queued.value(QStringLiteral("link_id")).toString()), queued);
    overview.setFedoraProjections(projections);
    QTRY_COMPARE(group()->text(0), QStringLiteral("Agent conversations (2)"));
    QCOMPARE(group()->childCount(), 2);

    // Without conversations the group says so.
    overview.setFedoraProjections({});
    overview.updateLibraryTree();
    QTRY_COMPARE(group()->text(0), QStringLiteral("Agent conversations (0)"));
    QCOMPARE(group()->childCount(), 1);
    QCOMPARE(group()->child(0)->text(0), QStringLiteral("No agent conversations yet"));
    QVERIFY(!(group()->child(0)->flags() & Qt::ItemIsSelectable));
}

QTEST_MAIN(CommentsOverviewTest)

#include "commentsoverviewtest.moc"
