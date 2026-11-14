#include "pdfversions.h"
#include "projectreadingcontext.h"
#include "thoughtsplatform.h"
/*
    SPDX-FileCopyrightText: 2005 Enrico Ros <eros.kde@email.it>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "side_reviews.h"
#include "docschangemonitor.h"

// qt/kde includes
#include <QAction>
#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QGridLayout>
#include <QHeaderView>
#include <QHash>
#include <QIcon>
#include <QItemSelectionModel>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPersistentModelIndex>
#include <QPixmap>
#include <QPromise>
#include <QPushButton>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QSet>
#include <QStringList>
#include <QStyle>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QTimer>
#include <QToolBar>
#include <QTreeView>

#include <KLocalizedString>
#include <KConfigGroup>
#include <KSharedConfig>
#include <KViewStateSerializer>

// local includes
#include "annotationmodel.h"
#include "documentfeedback.h"
#include "reviewcardfeedback.h"
#include "../shell/agentproposalparser.h"
#include "annotationpopup.h"
#include "annotationproxymodels.h"
#include "commentfragmentgroup.h"
#include "core/annotations.h"
#include "core/document.h"
#include "core/htmlattachment.h"
#include "core/page.h"
#include "ktreeviewsearchline.h"
#include "settings.h"
#include "reviewplacement.h"
#include "reviewcardregions.h"

#include <algorithm>
#include <tuple>
#include <utility>

namespace
{
// The Annotations type filters (Comments, Agent proposals, Review cards): one
// segmented row when the sidebar is wide enough, otherwise two columns, so a
// narrow sidebar is not forced wider by the filters. Each filter switches on
// and off by itself.
class AnnotationTypeFilters : public QWidget
{
public:
    explicit AnnotationTypeFilters(QWidget *parent)
        : QWidget(parent)
        , m_layout(new QGridLayout(this))
    {
        m_layout->setContentsMargins(2, 2, 2, 2);
        m_layout->setSpacing(0);
        setAttribute(Qt::WA_StyledBackground);
        setStyleSheet(QStringLiteral("#annotationTypeFilters { background: rgba(128, 128, 128, 40); border-radius: 7px; }"
                                     "#annotationTypeFilters QPushButton { padding: 3px 4px; border: 0; border-radius: 5px; background: transparent; }"
                                     "#annotationTypeFilters QPushButton:checked { background: palette(base); }"
                                     "#annotationTypeFilters QPushButton:focus { border: 1px solid palette(highlight); padding: 2px 3px; }"));
    }

    void addButton(QPushButton *button)
    {
        m_buttons.append(button);
        arrange(true);
    }

protected:
    void resizeEvent(QResizeEvent *event) override
    {
        QWidget::resizeEvent(event);
        arrange(false);
    }

private:
    int columnsFor(int width) const
    {
        int oneRow = 4;
        for (const QPushButton *button : m_buttons) oneRow += button->sizeHint().width();
        return width >= oneRow ? int(m_buttons.size()) : 2;
    }

    void arrange(bool force)
    {
        const int columns = std::max(1, columnsFor(width()));
        if (!force && columns == m_columns) return;
        m_columns = columns;
        for (QPushButton *button : std::as_const(m_buttons)) m_layout->removeWidget(button);
        for (int column = 0; column < m_layout->columnCount(); ++column) m_layout->setColumnStretch(column, 0);
        for (int index = 0; index < m_buttons.size(); ++index) m_layout->addWidget(m_buttons.at(index), index / columns, index % columns);
        for (int column = 0; column < columns; ++column) m_layout->setColumnStretch(column, 1);
        // Never let the one-row layout's minimum keep the sidebar wide.
        int twoColumns = m_layout->spacing();
        for (int index = 0; index < std::min<qsizetype>(2, m_buttons.size()); ++index) {
            int widest = 0;
            for (int row = index; row < m_buttons.size(); row += 2) widest = std::max(widest, m_buttons.at(row)->minimumSizeHint().width());
            twoColumns += widest;
        }
        setMinimumWidth(twoColumns);
    }

    QGridLayout *m_layout;
    QList<QPushButton *> m_buttons;
    int m_columns = 0;
};
}

static QIcon docsSourceLinkIcon(QColor color, bool disconnected = false)
{
    QIcon icon;
    for (const qreal devicePixelRatio : {1.0, 2.0, 3.0}) {
        const int pixelSize = qRound(16 * devicePixelRatio);
        QPixmap pixmap(pixelSize, pixelSize);
        pixmap.setDevicePixelRatio(devicePixelRatio);
        pixmap.fill(Qt::transparent);

        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(color, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));

        QPainterPath leftLink;
        leftLink.moveTo(7, 4);
        leftLink.lineTo(5, 4);
        leftLink.cubicTo(2.8, 4, 1, 5.8, 1, 8);
        leftLink.cubicTo(1, 10.2, 2.8, 12, 5, 12);
        leftLink.lineTo(7, 12);
        painter.drawPath(leftLink);

        QPainterPath rightLink;
        rightLink.moveTo(9, 4);
        rightLink.lineTo(11, 4);
        rightLink.cubicTo(13.2, 4, 15, 5.8, 15, 8);
        rightLink.cubicTo(15, 10.2, 13.2, 12, 11, 12);
        rightLink.lineTo(9, 12);
        painter.drawPath(rightLink);
        if (disconnected) {
            painter.drawLine(QPointF(6, 14), QPointF(10, 2));
        } else {
            painter.drawLine(QPointF(5.5, 8), QPointF(10.5, 8));
        }

        icon.addPixmap(pixmap);
    }
    return icon;
}

// A wrapper cut short (unclosed group, missing argument) may be an editor's save in progress.
static bool agentProposalIssueMayBeTransient(const Thoughts::AgentProposalIssue &issue)
{
    return issue.code == QLatin1String(Thoughts::AgentProposalIssueCode::UnclosedGroup)
        || issue.code == QLatin1String(Thoughts::AgentProposalIssueCode::MissingArgument);
}

static QString canonicalAgentProposalSourcePath(const QString &docsRoot, const QString &storedPath)
{
    if (docsRoot.isEmpty() || storedPath.isEmpty()) {
        return {};
    }
    const QFileInfo rootInfo(docsRoot);
    const QString rootPath = rootInfo.canonicalFilePath().isEmpty() ? rootInfo.absoluteFilePath() : rootInfo.canonicalFilePath();
    const QFileInfo sourceInfo(QFileInfo(storedPath).isAbsolute() ? storedPath : QDir(rootPath).absoluteFilePath(storedPath));
    const QString sourcePath = sourceInfo.canonicalFilePath();
    if (!sourceInfo.isFile() || !Thoughts::isAgentProposalSourcePath(sourcePath)
        || (sourcePath != rootPath && !sourcePath.startsWith(rootPath + QLatin1Char('/')))) {
        return {};
    }
    return sourcePath;
}

static bool agentProposalSourcePathIsSafe(const QString &docsRoot, const QString &storedPath)
{
    if (docsRoot.isEmpty() || storedPath.isEmpty()) {
        return false;
    }
    const QFileInfo rootInfo(docsRoot);
    const QString rootPath = QDir::cleanPath(rootInfo.canonicalFilePath().isEmpty() ? rootInfo.absoluteFilePath() : rootInfo.canonicalFilePath());
    const QFileInfo requestedInfo(QFileInfo(storedPath).isAbsolute() ? storedPath : QDir(rootPath).absoluteFilePath(storedPath));
    const QString requestedPath = QDir::cleanPath(requestedInfo.absoluteFilePath());
    if (!Thoughts::isAgentProposalSourcePath(requestedPath)
        || (requestedPath != rootPath && !requestedPath.startsWith(rootPath + QLatin1Char('/')))) {
        return false;
    }
    const QString canonicalPath = requestedInfo.canonicalFilePath();
    return canonicalPath.isEmpty() || canonicalPath == rootPath || canonicalPath.startsWith(rootPath + QLatin1Char('/'));
}

static QVariant loadArtifactAgentProposalRows(const QString &docsRoot, const QUrl &documentUrl, const Thoughts::ReadingDocumentContext *child = nullptr)
{
    QVariantMap result;
    QVariantList rows;
    QVariantList sourceComments;
    QStringList sourcePaths;
    bool liveSourcesValid = true;
    if (docsRoot.isEmpty() || !documentUrl.isLocalFile()) {
        result.insert(QStringLiteral("rows"), rows);
        result.insert(QStringLiteral("source_paths"), sourcePaths);
        return result;
    }

    if (!child) {
        if (const auto reading = Thoughts::ProjectReadingContext::load(documentUrl.toLocalFile())) {
            QSet<QString> proposalIds, commentIds, decidedIds;
            for (const auto &document : reading->documents) {
                const auto part = loadArtifactAgentProposalRows(docsRoot, documentUrl, &document).toMap();
                for (const QString &id : part.value(QStringLiteral("decided_ids")).toStringList()) decidedIds.insert(id);
                for (const auto &row : part.value(QStringLiteral("rows")).toList()) {
                    const auto id = row.toMap().value(QStringLiteral("id")).toString();
                    if (!proposalIds.contains(id)) { proposalIds.insert(id); rows.append(row); }
                }
                for (const auto &row : part.value(QStringLiteral("source_comments")).toList()) {
                    const auto id = row.toMap().value(QStringLiteral("id")).toString();
                    if (!commentIds.contains(id)) { commentIds.insert(id); sourceComments.append(row); }
                }
                sourcePaths.append(part.value(QStringLiteral("source_paths")).toStringList());
                liveSourcesValid = liveSourcesValid && part.value(QStringLiteral("live_sources_valid"), true).toBool();
            }
            sourcePaths.removeDuplicates();
            for (const QString &id : std::as_const(proposalIds)) decidedIds.remove(id);
            return QVariantMap{{QStringLiteral("rows"), rows}, {QStringLiteral("source_comments"), sourceComments},
                {QStringLiteral("source_paths"), sourcePaths}, {QStringLiteral("live_sources_valid"), liveSourcesValid},
                {QStringLiteral("decided_ids"), QStringList(decidedIds.cbegin(), decidedIds.cend())}};
        }
    }
    const QFileInfo pdfInfo(child ? child->path : documentUrl.toLocalFile());
    QDir bundleRoot(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/source-bundle")));
    if (!QFileInfo::exists(bundleRoot.filePath(QStringLiteral("documents")))) {
        bundleRoot = QDir(QDir(docsRoot).filePath(QStringLiteral(".runtime/target/source-bundle")));
    }
    QString artifactSha256;
    QJsonObject manifest, pointer;
    if (child) {
        artifactSha256 = child->document.packageSha256;
        manifest = child->package;
    } else {
    QFile pointerFile(bundleRoot.filePath(QStringLiteral("documents/%1.json").arg(pdfInfo.fileName())));
    if (!pointerFile.open(QIODevice::ReadOnly)) {
        result.insert(QStringLiteral("rows"), rows);
        result.insert(QStringLiteral("source_paths"), sourcePaths);
        return result;
    }
    pointer = QJsonDocument::fromJson(pointerFile.readAll()).object();
    artifactSha256 = pointer.value(QStringLiteral("artifact_sha256")).toString();
    if (pointer.value(QStringLiteral("schema_version")).toInt() != 1 || artifactSha256.size() != 64) {
        result.insert(QStringLiteral("rows"), rows);
        result.insert(QStringLiteral("source_paths"), sourcePaths);
        return result;
    }
    QFile manifestFile(bundleRoot.filePath(QStringLiteral("objects/%1").arg(artifactSha256)));
    if (!manifestFile.open(QIODevice::ReadOnly)) {
        result.insert(QStringLiteral("rows"), rows);
        result.insert(QStringLiteral("source_paths"), sourcePaths);
        return result;
    }
    const QByteArray manifestContents = manifestFile.readAll();
    if (QCryptographicHash::hash(manifestContents, QCryptographicHash::Sha256).toHex() != artifactSha256.toLatin1()) {
        result.insert(QStringLiteral("rows"), rows);
        result.insert(QStringLiteral("source_paths"), sourcePaths);
        return result;
    }
    manifest = QJsonDocument::fromJson(manifestContents).object();
    const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
    if (manifest.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact")
        || pdf.value(QStringLiteral("filename")).toString().compare(pdfInfo.fileName(), Qt::CaseInsensitive) != 0) {
        result.insert(QStringLiteral("rows"), rows);
        result.insert(QStringLiteral("source_paths"), sourcePaths);
        return result;
    }

    }
    const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
    bool pdfMatchesArtifact = false;
    QFile openedPdf(pdfInfo.absoluteFilePath());
    if (openedPdf.open(QIODevice::ReadOnly)) {
        QCryptographicHash pdfHash(QCryptographicHash::Sha256);
        if (pdfHash.addData(&openedPdf)) {
            const QString openedPdfSha256 = QString::fromLatin1(pdfHash.result().toHex());
            pdfMatchesArtifact = openedPdfSha256 == pdf.value(QStringLiteral("pdf_sha256")).toString()
                && (pointer.value(QStringLiteral("pdf_sha256")).toString().isEmpty() || openedPdfSha256 == pointer.value(QStringLiteral("pdf_sha256")).toString());
        }
    }

    QHash<QString, QString> sourceHashes;
    for (const QJsonValue &sourceValue : manifest.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = sourceValue.toObject();
        const QString path = source.value(QStringLiteral("path")).toString();
        const QString sha256 = source.value(QStringLiteral("sha256")).toString();
        if (!path.isEmpty() && sha256.size() == 64) {
            sourceHashes.insert(path, sha256);
        }
    }
    if (manifest.value(QStringLiteral("review_comments_schema_version")).toInt() == 1) {
        QSet<QString> seenReviewIds;
        for (const QJsonValue &reviewValue : manifest.value(QStringLiteral("review_comments")).toArray()) {
            const QJsonObject review = reviewValue.toObject();
            const QString id = review.value(QStringLiteral("id")).toString().trimmed();
            const QString sourceFile = review.value(QStringLiteral("source_file")).toString();
            const int sourceLine = review.value(QStringLiteral("line")).toInt();
            if (id.isEmpty() || seenReviewIds.contains(id) || sourceLine < 1 || review.value(QStringLiteral("source_sha256")).toString() != sourceHashes.value(sourceFile)) {
                continue;
            }
            seenReviewIds.insert(id);
            sourceComments.append(QVariantMap {{QStringLiteral("id"), id},
                                                {QStringLiteral("contents"), review.value(QStringLiteral("contents")).toString()},
                                                {QStringLiteral("source_path"), sourceFile},
                                                {QStringLiteral("source_line"), sourceLine}});
        }
    }

    QHash<QString, QVariantMap> proposalRows;
    QStringList proposalOrder;
    const QSet<QString> locallyResolved = Thoughts::locallyResolvedAgentProposalIds(bundleRoot.absolutePath());
    if (pdfMatchesArtifact) {
        for (const QJsonValue &proposalValue : manifest.value(QStringLiteral("agent_proposals")).toArray()) {
            const QJsonObject proposal = proposalValue.toObject();
            const QString id = proposal.value(QStringLiteral("id")).toString().trimmed();
            if (id.isEmpty() || locallyResolved.contains(id) || proposal.value(QStringLiteral("suppressed")).toBool() || proposalRows.contains(id)) {
                continue;
            }
            const QString storedSource = proposal.value(QStringLiteral("source_file")).toString();
            QVariantMap row {{QStringLiteral("id"), id},
                             {QStringLiteral("motivation"), proposal.value(QStringLiteral("comment")).toString()},
                             {QStringLiteral("conversation_id"), proposal.value(QStringLiteral("conversation_id")).toString()},
                             {QStringLiteral("conversation_url"), proposal.value(QStringLiteral("conversation_url")).toString()},
                             {QStringLiteral("before"), proposal.value(QStringLiteral("before")).toObject().value(QStringLiteral("preview")).toString()},
                             {QStringLiteral("after"), proposal.value(QStringLiteral("after")).toObject().value(QStringLiteral("preview")).toString()},
                             {QStringLiteral("kind"), proposal.value(QStringLiteral("kind")).toString()},
                             {QStringLiteral("source_path"), canonicalAgentProposalSourcePath(docsRoot, storedSource)},
                             {QStringLiteral("stored_source"), storedSource},
                             {QStringLiteral("source_line"), proposal.value(QStringLiteral("line")).toInt()},
                             {QStringLiteral("source_sha256"), proposal.value(QStringLiteral("source_sha256")).toString()},
                             {QStringLiteral("bundle_id"), artifactSha256},
                             {QStringLiteral("live_source"), false},
                             {QStringLiteral("local_source_present"), false},
                             {QStringLiteral("resolution_error"), QString()}};
            if (!storedSource.isEmpty() && !agentProposalSourcePathIsSafe(docsRoot, storedSource)) {
                row.insert(QStringLiteral("resolution_error"), QStringLiteral("The TeX source is outside the documentation folder."));
            }
            proposalRows.insert(id, row);
            proposalOrder.append(id);
        }
    }

    QStringList storedSources;
    for (const QJsonValue &sourceValue : manifest.value(QStringLiteral("sources")).toArray()) {
        storedSources.append(sourceValue.toObject().value(QStringLiteral("path")).toString());
    }
    for (const QJsonValue &proposalValue : manifest.value(QStringLiteral("agent_proposals")).toArray()) {
        const QString proposalSource = proposalValue.toObject().value(QStringLiteral("source_file")).toString();
        if (!proposalSource.isEmpty() && !storedSources.contains(proposalSource)) {
            storedSources.append(proposalSource);
        }
    }

    QSet<QString> seenSources;
    QSet<QString> decidedIds;
    for (const QString &storedSource : std::as_const(storedSources)) {
        const QString sourcePath = canonicalAgentProposalSourcePath(docsRoot, storedSource);
        if (sourcePath.isEmpty() || seenSources.contains(sourcePath)) {
            continue;
        }
        seenSources.insert(sourcePath);
        sourcePaths.append(sourcePath);
        QFile sourceFile(sourcePath);
        if (!sourceFile.open(QIODevice::ReadOnly)) {
            liveSourcesValid = false;
            continue;
        }
        const QByteArray sourceBytes = sourceFile.readAll();
        const QString currentSourceSha256 = QString::fromLatin1(QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256).toHex());
        for (auto row = proposalRows.begin(); row != proposalRows.end(); ++row) {
            if (row->value(QStringLiteral("stored_source")).toString() == storedSource) {
                row->insert(QStringLiteral("source_path"), sourcePath);
                row->insert(QStringLiteral("source_sha256"), currentSourceSha256);
                row->insert(QStringLiteral("local_source_present"), true);
                row->insert(QStringLiteral("resolution_error"), QStringLiteral("The source changed before this proposal could be applied."));
            }
        }
        const Thoughts::AgentProposalParse parsed = Thoughts::parseAgentProposals(sourcePath, QString::fromUtf8(sourceBytes));
        QSet<QString> liveProposalIds;
        // A broken wrapper stays listed with its issue; it cannot be decided until fixed.
        for (const Thoughts::AgentProposalIssue &issue : parsed.issues) {
            // An incomplete wrapper is often a save in progress: retry before listing it.
            if (agentProposalIssueMayBeTransient(issue)) liveSourcesValid = false;
            if (!issue.hasId || issue.id.isEmpty() || liveProposalIds.contains(issue.id)) continue;
            liveProposalIds.insert(issue.id);
            if (!proposalRows.contains(issue.id)) {
                proposalRows.insert(issue.id, QVariantMap {{QStringLiteral("id"), issue.id}, {QStringLiteral("kind"), QStringLiteral("edit")}});
                proposalOrder.append(issue.id);
            }
            QVariantMap row = proposalRows.value(issue.id);
            row.insert(QStringLiteral("source_path"), sourcePath);
            row.insert(QStringLiteral("source_line"), issue.line);
            row.insert(QStringLiteral("source_sha256"), currentSourceSha256);
            row.insert(QStringLiteral("live_source"), true);
            row.insert(QStringLiteral("local_source_present"), true);
            row.insert(QStringLiteral("issue_code"), issue.code);
            row.insert(QStringLiteral("resolution_error"), i18n("This proposal cannot be decided until its source is fixed: %1", Thoughts::agentProposalIssueMessage(issue)));
            proposalRows.insert(issue.id, row);
        }
        for (const Thoughts::AgentProposedEdit &proposal : std::as_const(parsed.proposals)) {
            liveProposalIds.insert(proposal.id);
            if (!proposalRows.contains(proposal.id)) {
                proposalRows.insert(proposal.id, QVariantMap {{QStringLiteral("id"), proposal.id}});
                proposalOrder.append(proposal.id);
            }
            QVariantMap row = proposalRows.value(proposal.id);
            row.insert(QStringLiteral("id"), proposal.id);
            row.insert(QStringLiteral("motivation"), proposal.explanation);
            row.insert(QStringLiteral("conversation_id"), proposal.conversationId);
            row.insert(QStringLiteral("conversation_url"), proposal.conversationUrl);
            row.insert(QStringLiteral("before"), proposal.original);
            row.insert(QStringLiteral("after"), proposal.replacement);
            row.insert(QStringLiteral("kind"), proposal.replacement.trimmed().isEmpty() ? QStringLiteral("removal") : QStringLiteral("edit"));
            row.insert(QStringLiteral("source_path"), sourcePath);
            row.insert(QStringLiteral("source_line"), proposal.line);
            row.insert(QStringLiteral("source_sha256"), currentSourceSha256);
            row.insert(QStringLiteral("live_source"), true);
            row.insert(QStringLiteral("local_source_present"), true);
            row.insert(QStringLiteral("resolution_error"), QString());
            proposalRows.insert(proposal.id, row);
        }
        QStringList removedIds;
        for (auto row = proposalRows.cbegin(); row != proposalRows.cend(); ++row) {
            if (row->value(QStringLiteral("stored_source")).toString() == storedSource
                && !liveProposalIds.contains(row.key())) {
                removedIds.append(row.key());
            }
        }
        for (const QString &removedId : std::as_const(removedIds)) {
            proposalRows.remove(removedId);
            proposalOrder.removeAll(removedId);
            decidedIds.insert(removedId);
        }
    }
    for (const QString &id : std::as_const(proposalOrder)) {
        QVariantMap row = proposalRows.value(id);
        row.remove(QStringLiteral("stored_source"));
        rows.append(row);
        decidedIds.remove(id);
    }
    // In the PDF but no longer in its local source: decided (or removed) since
    // the build. The page hides their marks until the next build confirms it.
    result.insert(QStringLiteral("decided_ids"), QStringList(decidedIds.cbegin(), decidedIds.cend()));
    result.insert(QStringLiteral("rows"), rows);
    result.insert(QStringLiteral("source_comments"), sourceComments);
    result.insert(QStringLiteral("source_paths"), sourcePaths);
    result.insert(QStringLiteral("live_sources_valid"), liveSourcesValid);
    return result;
}

// Explicit overview source locations remain usable when PDF metadata is missing or stale.
static QVariant loadAgentProposalRows(const QString &docsRoot, const QUrl &documentUrl, const QStringList &additionalSources)
{
    QVariantMap result = loadArtifactAgentProposalRows(docsRoot, documentUrl).toMap();
    QVariantList rows = result.value(QStringLiteral("rows")).toList();
    QStringList paths = result.value(QStringLiteral("source_paths")).toStringList();
    for (const QString &path : additionalSources) {
        const QString sourcePath = canonicalAgentProposalSourcePath(docsRoot, path);
        if (sourcePath.isEmpty() || paths.contains(sourcePath)) {
            continue;
        }
        paths.append(sourcePath);
        QFile source(sourcePath);
        if (!source.open(QIODevice::ReadOnly)) {
            result.insert(QStringLiteral("live_sources_valid"), false);
            continue;
        }
        const QByteArray contents = source.readAll();
        const QString sourceSha256 = QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
        const Thoughts::AgentProposalParse parsed = Thoughts::parseAgentProposals(sourcePath, QString::fromUtf8(contents));
        for (const auto &proposal : parsed.proposals) {
            rows.append(QVariantMap {{QStringLiteral("id"), proposal.id},
                                     {QStringLiteral("motivation"), proposal.explanation},
                                     {QStringLiteral("conversation_id"), proposal.conversationId},
                                     {QStringLiteral("conversation_url"), proposal.conversationUrl},
                                     {QStringLiteral("before"), proposal.original},
                                     {QStringLiteral("after"), proposal.replacement},
                                     {QStringLiteral("kind"), proposal.replacement.trimmed().isEmpty() ? QStringLiteral("removal") : QStringLiteral("edit")},
                                     {QStringLiteral("source_path"), sourcePath},
                                     {QStringLiteral("source_line"), proposal.line},
                                     {QStringLiteral("source_sha256"), sourceSha256},
                                     {QStringLiteral("live_source"), true},
                                     {QStringLiteral("local_source_present"), true}});
        }
        QSet<QString> issueIds;
        for (const auto &issue : parsed.issues) {
            if (agentProposalIssueMayBeTransient(issue)) result.insert(QStringLiteral("live_sources_valid"), false);
            if (!issue.hasId || issue.id.isEmpty() || issueIds.contains(issue.id)) continue;
            issueIds.insert(issue.id);
            rows.append(QVariantMap {{QStringLiteral("id"), issue.id},
                                     {QStringLiteral("kind"), QStringLiteral("edit")},
                                     {QStringLiteral("source_path"), sourcePath},
                                     {QStringLiteral("source_line"), issue.line},
                                     {QStringLiteral("source_sha256"), sourceSha256},
                                     {QStringLiteral("live_source"), true},
                                     {QStringLiteral("local_source_present"), true},
                                     {QStringLiteral("issue_code"), issue.code},
                                     {QStringLiteral("resolution_error"),
                                      i18n("This proposal cannot be decided until its source is fixed: %1", Thoughts::agentProposalIssueMessage(issue))}});
        }
    }
    QStringList decidedIds = result.value(QStringLiteral("decided_ids")).toStringList();
    for (const QVariant &row : std::as_const(rows)) {
        decidedIds.removeAll(row.toMap().value(QStringLiteral("id")).toString());
    }
    result.insert(QStringLiteral("decided_ids"), decidedIds);
    result.insert(QStringLiteral("rows"), rows);
    result.insert(QStringLiteral("source_paths"), paths);
    return result;
}

class TreeView : public QTreeView
{
    Q_OBJECT

public:
    explicit TreeView(QWidget *parent = Q_NULLPTR)
        : QTreeView(parent)
    {
    }

Q_SIGNALS:
    void deleteRequested(const QModelIndex &index);
    void reviewMoveRequested(bool forward);
    void reviewRequested(const QModelIndex &index);

public:
    bool reviewNavigationActive() const
    {
        return m_reviewNavigationActive;
    }

    void setReviewNavigationActive(bool active)
    {
        m_reviewNavigationActive = active;
    }

protected:
    void keyPressEvent(QKeyEvent *event) override
    {
        const Qt::KeyboardModifiers modifiers = event->modifiers() & ~Qt::KeypadModifier;
        if (modifiers == Qt::NoModifier) {
            switch (event->key()) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
                if (currentIndex().isValid()) {
                    Q_EMIT reviewRequested(currentIndex());
                    event->accept();
                    return;
                }
                break;
            case Qt::Key_Up:
            case Qt::Key_Down:
                if (m_reviewNavigationActive) {
                    Q_EMIT reviewMoveRequested(event->key() == Qt::Key_Down);
                    event->accept();
                    return;
                }
                break;
            case Qt::Key_Backspace:
            case Qt::Key_Delete:
                if (currentIndex().isValid()) {
                    Q_EMIT deleteRequested(currentIndex());
                    event->accept();
                    return;
                }
                break;
            default:
                break;
            }
        }

        QTreeView::keyPressEvent(event);
    }

private:
    bool m_reviewNavigationActive = false;
};

class ReviewItemDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    // Review cards and feedback: the page in a column of its own, then wrapped text.
    static bool cardLike(const QModelIndex &index)
    {
        const int type = index.data(AnnotationModel::ItemTypeRole).toInt();
        return type == AnnotationModel::ReviewCard || type == AnnotationModel::Feedback;
    }

    // Width of the column left of a review card's question that names its pages ("p. 3").
    // Every card shares it, so the questions line up; a third of the row at most.
    static int pagesColumnWidth(const QStyleOptionViewItem &option, const QModelIndex &index, int rowWidth)
    {
        int width = 0;
        const QStringList labels = index.data(AnnotationModel::ReviewCardPagesLabelsRole).toStringList();
        for (const QString &label : labels) width = qMax(width, option.fontMetrics.horizontalAdvance(label));
        return width > 0 ? qMin(width, rowWidth / 3) + 12 : 0;
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        if (index.data(AnnotationModel::IsReplyRole).toBool()) {
            const auto *view = qobject_cast<const QTreeView *>(parent());
            int indentation = view ? view->indentation() : 0;
            for (auto ancestor = index.parent(); view && ancestor.isValid(); ancestor = ancestor.parent()) indentation += view->indentation();
            const int width = qMax(40, (view ? view->viewport()->width() : 300) - indentation - 40);
            return QSize(0, option.fontMetrics.boundingRect(QRect(0, 0, width, 100000), Qt::TextWordWrap, index.data().toString()).height() + option.fontMetrics.height() + 18);
        }
        if (index.data(AnnotationModel::ReplyCountRole).toInt() > 0) {
            const auto base = QStyledItemDelegate::sizeHint(option, index);
            return QSize(base.width(), qMax(base.height(), option.fontMetrics.height() + 16));
        }
        if (!cardLike(index)) return QStyledItemDelegate::sizeHint(option, index);
        const auto *view = qobject_cast<const QTreeView *>(parent());
        int indentation = view && view->rootIsDecorated() ? view->indentation() : 0;
        for (QModelIndex parent = index.parent(); view && parent.isValid(); parent = parent.parent()) indentation += view->indentation();
        const int width = view ? view->viewport()->width() - indentation : 300;
        const int textWidth = qMax(1, width - 53 - pagesColumnWidth(option, index, width - 53));
        int height = option.fontMetrics.boundingRect(QRect(0, 0, textWidth, 100000), Qt::TextWordWrap, index.data().toString()).height() + 16;
        if (!index.data(AnnotationModel::ReviewCardScheduleRole).toString().isEmpty()) height += option.fontMetrics.height() + 4;
        if (!index.data(AnnotationModel::ReviewCardStatusRole).toString().isEmpty()) height += option.fontMetrics.height() + 4;
        return QSize(0, height);
    }

    // A review card's schedule line in @p width: its parts in order (when it is due first, its last
    // tags last) as far as they fit whole; the tooltip has them all.
    static QString scheduleLine(const QModelIndex &index, const QFontMetrics &metrics, int width)
    {
        const QStringList parts = index.data(AnnotationModel::ReviewCardSchedulePartsRole).toStringList();
        if (parts.isEmpty()) return {};
        QString line = parts.first();
        for (qsizetype part = 1; part < parts.size(); ++part) {
            const QString longer = line + QStringLiteral(" · ") + parts.at(part);
            if (metrics.horizontalAdvance(longer) > width) break;
            line = longer;
        }
        return metrics.elidedText(line, Qt::ElideRight, width);
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        const int replies = index.data(AnnotationModel::ReplyCountRole).toInt();
        const bool reply = index.data(AnnotationModel::IsReplyRole).toBool();
        if (reply || replies > 0) {
            QStyleOptionViewItem background(option);
            initStyleOption(&background, index);
            background.text.clear();
            background.icon = {};
            const QStyle *style = option.widget ? option.widget->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
            painter->save();
            painter->setFont(option.font);
            const bool selected = option.state & QStyle::State_Selected;
            const QColor foreground = option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
            const QColor secondary = option.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText);
            const int markerWidth = index.data(AnnotationModel::UnplacedRole).toBool() ? 22 : 0;
            QRect textRect = option.rect.adjusted(reply ? 12 : 28, 7, -8 - markerWidth, -7);
            if (reply) {
                painter->setPen(option.palette.color(QPalette::Mid));
                painter->drawLine(option.rect.left() + 2, option.rect.top() + 4, option.rect.left() + 2, option.rect.bottom() - 4);
                painter->setPen(secondary);
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignTop, i18n("Reply"));
                textRect.setTop(textRect.top() + option.fontMetrics.height() + 4);
                painter->setPen(foreground);
                painter->drawText(textRect, Qt::TextWordWrap | Qt::AlignTop, index.data().toString());
            } else {
                const QString count = i18np("1 reply", "%1 replies", replies);
                const int countWidth = option.fontMetrics.horizontalAdvance(count);
                painter->setPen(secondary);
                painter->drawText(textRect, Qt::AlignRight | Qt::AlignVCenter, count);
                textRect.adjust(0, 0, -countWidth - 12, 0);
                painter->setPen(foreground);
                painter->drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, option.fontMetrics.elidedText(index.data().toString(), Qt::ElideRight, qMax(0, textRect.width())));
                qvariant_cast<QIcon>(index.data(Qt::DecorationRole)).paint(painter, QRect(option.rect.left() + 6, option.rect.center().y() - 8, 16, 16));
            }
            if (markerWidth) docsSourceLinkIcon(foreground, true).paint(painter, QRect(option.rect.right() - 18, option.rect.center().y() - 7, 14, 14));
            painter->restore();
            return;
        }
        if (cardLike(index)) {
            QStyleOptionViewItem background(option);
            initStyleOption(&background, index);
            background.text.clear();
            background.icon = {};
            const QStyle *style = option.widget ? option.widget->style() : QApplication::style();
            style->drawControl(QStyle::CE_ItemViewItem, &background, painter, option.widget);
            painter->save();
            painter->setFont(option.font);
            const bool selected = option.state & QStyle::State_Selected;
            const int pagesWidth = pagesColumnWidth(option, index, option.rect.width() - 53);
            const QString pages = index.data(AnnotationModel::ReviewCardPagesLabelRole).toString();
            if (pagesWidth > 0 && !pages.isEmpty()) {
                painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::PlaceholderText));
                // Integer advances can round below the painted width, so the rect takes half the gap as slack.
                const QRect pagesRect(option.rect.left() + 28, option.rect.top() + 8, pagesWidth - 6, option.fontMetrics.height());
                painter->drawText(pagesRect, Qt::AlignLeft | Qt::AlignTop, option.fontMetrics.elidedText(pages, Qt::ElideRight, pagesRect.width()));
            }
            painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
            const QRect textRect = option.rect.adjusted(28 + pagesWidth, 8, -25, -8);
            QRect questionBounds;
            painter->drawText(textRect, Qt::TextWordWrap | Qt::AlignTop, index.data().toString(), &questionBounds);
            const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
            icon.paint(painter, QRect(option.rect.left() + 6, option.rect.top() + 9, 16, 16));
            int lineTop = questionBounds.bottom() + 4;
            if (!index.data(AnnotationModel::ReviewCardScheduleRole).toString().isEmpty()) {
                // In the pages' quieter colour; a due card's in the question's, a little bolder.
                const bool due = index.data(AnnotationModel::ReviewCardDueRole).toBool();
                QFont font = option.font;
                if (due) font.setWeight(QFont::DemiBold);
                const QFontMetrics metrics(font);
                painter->setFont(font);
                painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : due ? QPalette::Text : QPalette::PlaceholderText));
                const QRect scheduleRect(textRect.left(), lineTop, textRect.width(), metrics.height());
                painter->drawText(scheduleRect, Qt::AlignLeft | Qt::AlignTop, scheduleLine(index, metrics, scheduleRect.width()));
                painter->setFont(option.font);
                painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
                lineTop += metrics.height() + 4;
            }
            const QString status = index.data(AnnotationModel::ReviewCardStatusRole).toString();
            if (!status.isEmpty()) {
                const QRect statusRect(textRect.left(), lineTop, textRect.width(), option.fontMetrics.height());
                painter->drawText(statusRect, Qt::AlignLeft | Qt::AlignTop, option.fontMetrics.elidedText(status, Qt::ElideRight, statusRect.width()));
            }
            painter->restore();
            return;
        }
        if (!index.data(AnnotationModel::UnplacedRole).toBool()) {
            QStyledItemDelegate::paint(painter, option, index);
            return;
        }
        QStyleOptionViewItem shortened(option);
        shortened.rect.adjust(0, 0, -22, 0);
        QStyledItemDelegate::paint(painter, shortened, index);
        painter->save();
        const QRect marker(option.rect.right() - 21, option.rect.top(), 22, option.rect.height());
        if (option.state & QStyle::State_Selected) painter->fillRect(marker, option.palette.highlight());
        else if (option.features & QStyleOptionViewItem::Alternate) painter->fillRect(marker, option.palette.alternateBase());
        else painter->fillRect(marker, option.palette.base());
        const QColor color = option.state & QStyle::State_Selected ? option.palette.highlightedText().color() : option.palette.text().color();
        const QIcon link = docsSourceLinkIcon(color, true);
        link.paint(painter, QRect(marker.center().x() - 7, marker.center().y() - 7, 14, 14));
        painter->restore();
    }
};

Reviews::Reviews(QWidget *parent, Okular::Document *document, const QString &docsRoot)
    : QWidget(parent)
    , m_docsRoot(docsRoot)
    , m_document(document)
{
    // create widgets and layout them vertically
    QVBoxLayout *vLayout = new QVBoxLayout(this);
    vLayout->setSpacing(6);

    m_sourceLinkIndicator = new QLabel(this);
    m_sourceLinkIndicator->setObjectName(QStringLiteral("annotationsSourceLink"));
    m_sourceLinkIndicator->hide();

    m_view = new TreeView(this);
    m_view->setAlternatingRowColors(true);
    m_view->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_view->header()->hide();
    m_view->setItemDelegate(new ReviewItemDelegate(m_view));

    QToolBar *toolBar = new QToolBar(this);
    toolBar->setObjectName(QStringLiteral("reviewOptsBar"));
    QSizePolicy sp = toolBar->sizePolicy();
    sp.setVerticalPolicy(QSizePolicy::Minimum);
    toolBar->setSizePolicy(sp);

    m_model = new AnnotationModel(m_document, m_view);
    m_model->setAgentProposalsVisible(true);

    m_filterProxy = new PageFilterProxyModel(m_view);
    m_groupProxy = new PageGroupProxyModel(m_view);
    m_authorProxy = new AuthorGroupProxyModel(m_view);

    m_filterProxy->setSourceModel(m_model);
    m_groupProxy->setSourceModel(m_filterProxy);
    m_authorProxy->setSourceModel(m_groupProxy);

    m_view->setModel(m_authorProxy);

    connect(m_authorProxy, &QAbstractItemModel::modelAboutToBeReset, this, &Reviews::saveTreeState);
    // Must be after setModel() so that we restore after QTreeView::reset cleared expanded items, not the other way around
    connect(m_authorProxy, &QAbstractItemModel::modelReset, this, &Reviews::restoreTreeState);

    m_searchLine = new KTreeViewSearchLine(this, m_view);
    m_searchLine->setPlaceholderText(i18n("Search…"));
    m_searchLine->setCaseSensitivity(Okular::Settings::self()->reviewsSearchCaseSensitive() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    m_searchLine->setRegularExpression(Okular::Settings::self()->reviewsSearchRegularExpression());
    connect(m_searchLine, &KTreeViewSearchLine::searchOptionsChanged, this, &Reviews::saveSearchOptions);
    auto *typeFilters = new AnnotationTypeFilters(this);
    typeFilters->setObjectName(QStringLiteral("annotationTypeFilters"));
    const KConfigGroup filterConfig(KSharedConfig::openConfig(), QStringLiteral("Annotation Sidebar"));
    const auto addTypeFilter = [this, typeFilters](const QString &text, const QString &toolTip, const QString &name, bool checked, int itemType) {
        auto *button = new QPushButton(text, typeFilters);
        button->setObjectName(name);
        button->setCheckable(true);
        button->setAutoDefault(false);
        button->setChecked(checked);
        button->setToolTip(toolTip);
        button->setFocusPolicy(Qt::TabFocus);
        button->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        typeFilters->addButton(button);
        connect(button, &QPushButton::toggled, this, &Reviews::updateTypeFilter);
        // Only a user click shows or hides markup; restoring or programmatic changes do not.
        connect(button, &QPushButton::clicked, this, [this, itemType](bool on) {
            Q_EMIT typeFilterClicked(itemType, on);
        });
        return button;
    };
    m_commentsFilter = addTypeFilter(i18n("Comments"), i18n("Comments: they stay on every version of the PDF"), QStringLiteral("reviewsCommentsFilter"), filterConfig.readEntry("ShowComments", true), AnnotationModel::Comment);
    m_feedbackFilter = addTypeFilter(i18n("Feedback"), i18n("Feedback: likes, dislikes, questions, drawings and text boxes, which belong to the PDF version they were given on"),
                                     QStringLiteral("reviewsFeedbackFilter"), filterConfig.readEntry("ShowFeedback", true), AnnotationModel::Feedback);
    m_proposalsFilter = addTypeFilter(i18n("Proposals"), i18n("Agent proposals"), QStringLiteral("reviewsProposalsFilter"), filterConfig.readEntry("ShowProposals", true), AnnotationModel::AgentProposal);
    m_reviewCardsFilter = addTypeFilter(i18n("Cards"), i18n("Review cards"), QStringLiteral("reviewsCardsFilter"), filterConfig.readEntry("ShowReviewCards", true), AnnotationModel::ReviewCard);
    vLayout->addWidget(typeFilters);
    auto *searchRow = new QHBoxLayout;
    searchRow->setSpacing(6);
    searchRow->addWidget(m_searchLine, 1);
    searchRow->addWidget(m_sourceLinkIndicator);
    vLayout->addLayout(searchRow);
    vLayout->addWidget(m_view);
    // What the version rule hides (document-feedback.md section 9): one line, never lost.
    m_earlierFeedback = new QPushButton(this);
    m_earlierFeedback->setObjectName(QStringLiteral("reviewsEarlierFeedback"));
    m_earlierFeedback->setFlat(true);
    m_earlierFeedback->setCheckable(true);
    m_earlierFeedback->hide();
    connect(m_earlierFeedback, &QPushButton::clicked, this, &Reviews::earlierFeedbackToggled);
    vLayout->addWidget(m_earlierFeedback);
    // The Retired filter: retired cards stay on the PDF version they were retired on, so this
    // version lists them only when asked, unplaced; that version (from History) draws them.
    m_retiredCardsFilter = new QPushButton(this);
    m_retiredCardsFilter->setObjectName(QStringLiteral("reviewsRetiredCardsFilter"));
    m_retiredCardsFilter->setFlat(true);
    m_retiredCardsFilter->setCheckable(true);
    m_retiredCardsFilter->setChecked(filterConfig.readEntry("ShowRetiredCards", false));
    m_retiredCardsFilter->setToolTip(i18n("Retired cards are not reviewed. Each stays on the PDF version it was on when it was retired: open that version "
                                          "from the card’s History to see it there."));
    m_retiredCardsFilter->hide();
    connect(m_retiredCardsFilter, &QPushButton::toggled, this, [this](bool on) {
        KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("Annotation Sidebar"));
        config.writeEntry("ShowRetiredCards", on);
        config.sync();
        rebuildReviewCards();
    });
    vLayout->addWidget(m_retiredCardsFilter);
    updateTypeFilter();
    vLayout->addWidget(toolBar);

    toolBar->setIconSize(QSize(16, 16));
    toolBar->setMovable(false);
    // - add Page button
    QAction *groupByPageAction = toolBar->addAction(QIcon::fromTheme(QStringLiteral("text-x-generic")), i18n("Group by Page"));
    groupByPageAction->setCheckable(true);
    connect(groupByPageAction, &QAction::toggled, this, &Reviews::slotPageEnabled);
    groupByPageAction->setChecked(Okular::Settings::groupByPage());
    // - add Author button
    QAction *groupByAuthorAction = toolBar->addAction(QIcon::fromTheme(QStringLiteral("user-identity")), i18n("Group by Author"));
    groupByAuthorAction->setCheckable(true);
    connect(groupByAuthorAction, &QAction::toggled, this, &Reviews::slotAuthorEnabled);
    groupByAuthorAction->setChecked(Okular::Settings::groupByAuthor());

    // - add separator
    toolBar->addSeparator();
    // - add Current Page Only button
    QAction *curPageOnlyAction = toolBar->addAction(QIcon::fromTheme(QStringLiteral("arrow-down")), i18n("Show annotations for current page only"));
    curPageOnlyAction->setCheckable(true);
    connect(curPageOnlyAction, &QAction::toggled, this, &Reviews::slotCurrentPageOnly);
    curPageOnlyAction->setChecked(Okular::Settings::currentPageOnly());
    // - add Due first button: review cards in the review queue's order, then the next to come due
    QAction *dueFirstAction = toolBar->addAction(QIcon::fromTheme(QStringLiteral("view-calendar-upcoming-days"), QIcon::fromTheme(QStringLiteral("view-sort-ascending"))),
                                                 i18n("Review cards due first"));
    dueFirstAction->setObjectName(QStringLiteral("reviewCardsDueFirst"));
    dueFirstAction->setCheckable(true);
    dueFirstAction->setToolTip(i18n("Review cards due first: overdue and due now, then the next to come due; the others by page"));
    connect(dueFirstAction, &QAction::toggled, this, [this](bool on) {
        m_filterProxy->setReviewCardsDueFirst(on);
        KConfigGroup(KSharedConfig::openConfig(), QStringLiteral("Annotation Sidebar")).writeEntry("ReviewCardsDueFirst", on);
    });
    dueFirstAction->setChecked(filterConfig.readEntry("ReviewCardsDueFirst", false));

    // Adds space between left actions, so that the next two buttons are aligned to the right
    QWidget *spacer = new QWidget();
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    toolBar->addWidget(spacer);

    QAction *expandAll = toolBar->addAction(QIcon::fromTheme(QStringLiteral("expand-all")), i18n("Expand all elements"));
    connect(expandAll, &QAction::triggered, this, &Reviews::slotExpandAll);
    QAction *collapseAll = toolBar->addAction(QIcon::fromTheme(QStringLiteral("collapse-all")), i18n("Collapse all elements"));
    connect(collapseAll, &QAction::triggered, this, &Reviews::slotCollapseAll);

    connect(m_view, &TreeView::doubleClicked, this, &Reviews::openReview);
    m_view->setExpandsOnDoubleClick(false);
    connect(m_view, &TreeView::clicked, this, [this](const QModelIndex &index) {
        if (index.data(AnnotationModel::ReplyCountRole).toInt() > 0) m_view->setExpanded(index, !m_view->isExpanded(index));
        if (activateReviewCard(index)) return;
        const QModelIndex source = annotationModelIndex(index);
        if (source.data(AnnotationModel::DocumentFeedbackRole).toBool()) {
            // Opens on one click, as a comment's row does.
            Q_EMIT feedbackRequested(source.data(AnnotationModel::ItemIdRole).toString(), true);
            return;
        }
        if (m_model->isAgentProposal(source)) {
            activateAgentProposal(index);
        }
    });
    connect(m_view, &TreeView::deleteRequested, this, &Reviews::deleteAnnotation);
    connect(m_view, &TreeView::reviewMoveRequested, this, &Reviews::moveReview);
    connect(m_view, &TreeView::reviewRequested, this, &Reviews::openReview);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &Reviews::currentChanged);

    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_view, &TreeView::customContextMenuRequested, this, &Reviews::contextMenuRequested);

    m_agentProposalWatcher = new QFutureWatcher<QVariant>(this);
    connect(m_agentProposalWatcher, &QFutureWatcher<QVariant>::finished, this, &Reviews::finishAgentProposalRefresh);
    m_agentProposalRefreshTimer = new QTimer(this);
    m_agentProposalRefreshTimer->setSingleShot(true);
    m_agentProposalRefreshTimer->setInterval(150);
    connect(m_agentProposalRefreshTimer, &QTimer::timeout, this, &Reviews::refreshAgentProposals);
    // Proposal markers are written into sources by agents and editors; follow
    // the shared change stream for exactly those sources.
    if (!m_docsRoot.isEmpty()) {
        Thoughts::DocsChangeMonitor *monitor = Thoughts::DocsChangeMonitor::forRoot(m_docsRoot);
        connect(monitor, &Thoughts::DocsChangeMonitor::pathsChanged, this, [this](const QStringList &paths) {
            if (std::any_of(paths.cbegin(), paths.cend(), [this](const QString &path) { return m_agentProposalSourcePaths.contains(path); })) {
                m_agentProposalRefreshTimer->start();
            }
        });
        connect(monitor, &Thoughts::DocsChangeMonitor::rescanRequired, this, [this] {
            if (!m_agentProposalSourcePaths.isEmpty()) {
                m_agentProposalRefreshTimer->start();
            }
        });
    }

    setDocsSourceLinkStatus(DocsSourceLinkState::NoDocument);
}

Reviews::~Reviews()
{
    m_document->removeObserver(this);
}

// BEGIN DocumentObserver Notifies
void Reviews::notifySetup(const QList<Okular::Page *> &pages, int setupFlags)
{
    Q_UNUSED(pages)
    if (setupFlags & (Okular::DocumentObserver::DocumentChanged | Okular::DocumentObserver::UrlChanged)) {
        clearActiveReviewCard();
        m_reviewCardRecords = {};
        m_reviewCards.clear();
        Q_EMIT reviewCardHighlightsChanged({});
        m_reviewPdfSha256.clear();
        m_model->setReviewCards({});
        m_suppressedAgentProposalIds.clear();
        m_suppressedAgentProposals.clear();
        setCommentSidebarState({});
        refreshAgentProposals();
    }
}

void Reviews::notifyCurrentPageChanged(int previousPage, int currentPage)
{
    Q_UNUSED(previousPage)

    m_filterProxy->setCurrentPage(currentPage);
}
// END DocumentObserver Notifies

void Reviews::setCommentSidebarState(const QHash<QString, AnnotationSourceOrder> &state)
{
    m_filterProxy->setAnnotationSourceOrder(state);
}

void Reviews::refreshCommentThreads()
{
    m_authorProxy->refreshCommentThreads();
}

void Reviews::refreshAgentProposals()
{
    if (m_docsRoot.isEmpty()) {
        m_agentProposalRefreshTimer->stop();
        m_agentProposalRequestedUrl = {};
        m_additionalAgentProposalSources.clear();
        m_agentProposalRefreshPending = false;
        m_model->setAgentProposals({});
        m_model->setSourceComments({});
        return;
    }
    if (m_agentProposalRequestedUrl != m_document->currentDocument()) {
        m_additionalAgentProposalSources.clear();
    }
    m_agentProposalRequestedUrl = m_document->currentDocument();
    if (m_agentProposalRefreshInFlight) {
        m_agentProposalRefreshPending = true;
        return;
    }
    startAgentProposalRefresh();
}

void Reviews::startAgentProposalRefresh()
{
    m_agentProposalRefreshTimer->stop();
    m_agentProposalRefreshPending = false;
    if (m_docsRoot.isEmpty() || m_agentProposalRequestedUrl.isEmpty()) {
        m_model->setAgentProposals({});
        m_model->setSourceComments({});
        return;
    }
    const QString docsRoot = m_docsRoot;
    const QUrl documentUrl = m_agentProposalRequestedUrl;
    m_agentProposalRunningUrl = documentUrl;
    m_agentProposalRefreshInFlight = true;
    QPromise<QVariant> promise;
    const QFuture<QVariant> future = promise.future();
    promise.start();
    m_agentProposalWatcher->setFuture(future);
    const QStringList additionalSources = m_additionalAgentProposalSources;
    QThreadPool::globalInstance()->start([promise = std::move(promise), docsRoot, documentUrl, additionalSources]() mutable {
        promise.addResult(loadAgentProposalRows(docsRoot, documentUrl, additionalSources));
        promise.finish();
    });
}

void Reviews::finishAgentProposalRefresh()
{
    const bool superseded = m_agentProposalRefreshPending || m_agentProposalRunningUrl != m_agentProposalRequestedUrl || m_document->currentDocument() != m_agentProposalRequestedUrl;
    if (!superseded) {
        const QVariantMap payload = m_agentProposalWatcher->result().toMap();
        if (!payload.value(QStringLiteral("live_sources_valid"), true).toBool()) {
            if (m_agentProposalTransientRetries < 10) {
                ++m_agentProposalTransientRetries;
                m_agentProposalRefreshTimer->start(100);
                m_agentProposalRefreshInFlight = false;
                return;
            }
        }
        m_agentProposalTransientRetries = 0;
        QList<AgentProposalSidebarEntry> entries;
        QList<SourceCommentSidebarEntry> sourceComments;
        const QVariantList rows = payload.value(QStringLiteral("rows")).toList();
        entries.reserve(rows.size());
        for (const QVariant &value : rows) {
            const QVariantMap row = value.toMap();
            AgentProposalSidebarEntry entry;
            entry.id = row.value(QStringLiteral("id")).toString();
            entry.motivation = row.value(QStringLiteral("motivation")).toString();
            entry.conversationId = row.value(QStringLiteral("conversation_id")).toString();
            entry.conversationUrl = row.value(QStringLiteral("conversation_url")).toString();
            entry.before = row.value(QStringLiteral("before")).toString();
            entry.after = row.value(QStringLiteral("after")).toString();
            entry.kind = row.value(QStringLiteral("kind")).toString();
            entry.sourcePath = row.value(QStringLiteral("source_path")).toString();
            entry.sourceLine = row.value(QStringLiteral("source_line")).toInt();
            entry.sourceSha256 = row.value(QStringLiteral("source_sha256")).toString();
            entry.bundleId = row.value(QStringLiteral("bundle_id")).toString();
            entry.liveSource = row.value(QStringLiteral("live_source")).toBool();
            entry.localSourcePresent = row.value(QStringLiteral("local_source_present")).toBool();
            entry.resolutionError = row.value(QStringLiteral("resolution_error")).toString();
            if (!entry.id.isEmpty() && !m_suppressedAgentProposalIds.contains(entry.id)) {
                entries.append(entry);
            }
        }
        m_model->setAgentProposals(entries);
        for (const QVariant &value : payload.value(QStringLiteral("source_comments")).toList()) {
            const QVariantMap row = value.toMap();
            SourceCommentSidebarEntry entry;
            entry.id = row.value(QStringLiteral("id")).toString();
            entry.contents = row.value(QStringLiteral("contents")).toString();
            entry.sourcePath = row.value(QStringLiteral("source_path")).toString();
            entry.sourceLine = row.value(QStringLiteral("source_line")).toInt();
            if (!entry.id.isEmpty() && !entry.sourcePath.isEmpty() && entry.sourceLine > 0) {
                sourceComments.append(entry);
            }
        }
        m_model->setSourceComments(sourceComments);
        const QStringList decidedIds = payload.value(QStringLiteral("decided_ids")).toStringList();
        m_decidedAgentProposalIds = QSet<QString>(decidedIds.cbegin(), decidedIds.cend());
        Q_EMIT agentProposalsChanged();
        m_agentProposalSourcePaths.clear();
        for (const QString &path : payload.value(QStringLiteral("source_paths")).toStringList()) {
            const QFileInfo info(path);
            m_agentProposalSourcePaths.insert(QDir::cleanPath(info.absoluteFilePath()));
            const QString canonical = info.canonicalFilePath();
            if (!canonical.isEmpty()) {
                m_agentProposalSourcePaths.insert(canonical);
            }
        }
    }
    m_agentProposalRefreshInFlight = false;
    if (m_agentProposalRefreshPending || m_agentProposalRunningUrl != m_agentProposalRequestedUrl) {
        startAgentProposalRefresh();
    }
}

void Reviews::setDocsSourceLinkStatus(DocsSourceLinkState state, int sourceFileCount, const QStringList &projects)
{
    sourceFileCount = std::max(0, sourceFileCount);
    QStringList normalizedProjects = projects;
    normalizedProjects.removeDuplicates();
    std::sort(normalizedProjects.begin(), normalizedProjects.end());
    if (m_docsSourceLinkState == state && m_docsSourceLinkFileCount == sourceFileCount && m_docsSourceLinkProjects == normalizedProjects) {
        return;
    }

    m_docsSourceLinkState = state;
    m_docsSourceLinkFileCount = sourceFileCount;
    m_docsSourceLinkProjects = normalizedProjects;

    QIcon icon;
    QString description;
    if (state == DocsSourceLinkState::Linked) {
        icon = docsSourceLinkIcon(palette().color(QPalette::WindowText));

        if (normalizedProjects.size() == 1) {
            description = i18np("Source linked to one TeX file in %2. Anchor changes are monitored.", "Source linked to %1 TeX files in %2. Anchor changes are monitored.", sourceFileCount, normalizedProjects.constFirst());
        } else {
            description = i18np("Source linked to one TeX file. Anchor changes are monitored.", "Source linked to %1 TeX files. Anchor changes are monitored.", sourceFileCount);
        }
    } else if (state == DocsSourceLinkState::NotLinked) {
        QColor dimmedColor = palette().color(QPalette::WindowText);
        dimmedColor.setAlphaF(dimmedColor.alphaF() * 0.35);
        icon = docsSourceLinkIcon(dimmedColor);
        description = i18n("Not source linked. Only canonical PDFs in current_pdfs are monitored.");
    }

    m_sourceLinkIndicator->setPixmap(icon.isNull() ? QPixmap() : icon.pixmap(QSize(16, 16), devicePixelRatioF()));
    m_sourceLinkIndicator->setVisible(!icon.isNull());
    m_sourceLinkIndicator->setToolTip(description);
    m_sourceLinkIndicator->setAccessibleDescription(description);
}

void Reviews::reparseConfig()
{
    m_searchLine->setCaseSensitivity(Okular::Settings::reviewsSearchCaseSensitive() ? Qt::CaseSensitive : Qt::CaseInsensitive);
    m_searchLine->setRegularExpression(Okular::Settings::reviewsSearchRegularExpression());
    m_view->update();
}

// BEGIN GUI Slots -> requestListViewUpdate
void Reviews::slotPageEnabled(bool on)
{
    // store toggle state in Settings and update the listview
    Okular::Settings::setGroupByPage(on);
    m_groupProxy->groupByPage(on);

    m_view->expandAll();
}

void Reviews::slotAuthorEnabled(bool on)
{
    // store toggle state in Settings and update the listview
    Okular::Settings::setGroupByAuthor(on);
    m_authorProxy->groupByAuthor(on);

    m_view->expandAll();
}

void Reviews::slotCurrentPageOnly(bool on)
{
    // store toggle state in Settings and update the listview
    Okular::Settings::setCurrentPageOnly(on);
    m_filterProxy->groupByCurrentPage(on);

    m_view->expandAll();
}

void Reviews::slotExpandAll()
{
    m_view->expandAll();
}

void Reviews::slotCollapseAll()
{
    m_view->collapseAll();
}

void Reviews::selectAnnotation(Okular::Annotation *annotation, int pageNumber)
{
    // Following a PDF marker must reveal its row even after filtering comments out.
    m_commentsFilter->setChecked(true);
    m_filterProxy->setCurrentPage(pageNumber);

    const QModelIndex sourceIndex = m_model->indexForAnnotation(annotation, pageNumber);
    if (!sourceIndex.isValid()) {
        return;
    }

    const QModelIndex filterIndex = m_filterProxy->mapFromSource(sourceIndex);
    const QModelIndex groupIndex = m_groupProxy->mapFromSource(filterIndex);
    const QModelIndex viewIndex = m_authorProxy->mapFromSource(groupIndex);
    if (!viewIndex.isValid()) {
        return;
    }

    revealRow(viewIndex);
    m_view->setFocus(Qt::MouseFocusReason);
}

void Reviews::revealRow(const QModelIndex &viewIndex)
{
    if (m_view->isRowHidden(viewIndex.row(), viewIndex.parent())) {
        m_searchLine->clear();
        m_searchLine->updateSearch(QString());
    }

    QList<QModelIndex> parents;
    for (QModelIndex parent = viewIndex.parent(); parent.isValid(); parent = parent.parent()) {
        parents.prepend(parent);
    }
    for (const QModelIndex &parent : std::as_const(parents)) {
        m_view->expand(parent);
    }

    m_view->selectionModel()->setCurrentIndex(viewIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_view->scrollTo(viewIndex, QAbstractItemView::PositionAtCenter);
}

void Reviews::selectFeedback(const QString &feedbackId)
{
    // A new row shows even after filtering its type out: a comment with the comments, the rest
    // with the feedback.
    const QModelIndex source = m_model->indexForFeedback(feedbackId);
    if (!source.isValid()) return;
    (source.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::Comment ? m_commentsFilter : m_feedbackFilter)->setChecked(true);
    if (source.data(AnnotationModel::PageRole).toInt() >= 0) m_filterProxy->setCurrentPage(source.data(AnnotationModel::PageRole).toInt());
    const QModelIndex viewIndex = m_authorProxy->mapFromSource(m_groupProxy->mapFromSource(m_filterProxy->mapFromSource(source)));
    if (viewIndex.isValid()) revealRow(viewIndex);
}

bool Reviews::selectAndOpenAnnotation(Okular::Annotation *annotation, int pageNumber)
{
    if (!annotation) {
        return false;
    }
    selectAnnotation(annotation, pageNumber);
    const QModelIndex viewIndex = m_view->currentIndex();
    const QModelIndex annotationIndex = annotationModelIndex(viewIndex);
    if (!annotationIndex.isValid() || m_model->annotationForIndex(annotationIndex) != annotation) {
        return false;
    }
    openCommentWindow(viewIndex);
    return true;
}
// END GUI Slots

void Reviews::openCommentWindow(const QModelIndex &index)
{
    const QModelIndex annotIndex = annotationModelIndex(index);
    Okular::Annotation *annotation = m_model->annotationForIndex(annotIndex);
    if (!annotation) {
        return;
    }

    int pageNumber = m_model->data(annotIndex, AnnotationModel::PageRole).toInt();
    const Thoughts::CommentFragment logicalRoot = Thoughts::commentFragmentGroup(m_document, annotation, pageNumber).root();
    if (logicalRoot.annotation) {
        annotation = logicalRoot.annotation;
        pageNumber = logicalRoot.page;
    }
    Okular::Annotation *selectedAnnotation = annotation;
    const int selectedPage = pageNumber;
    const Thoughts::CommentFragment threadRoot = Thoughts::commentThreadRoot(m_document, annotation, pageNumber);
    if (threadRoot.annotation) {
        annotation = threadRoot.annotation;
        pageNumber = threadRoot.page;
    }
    const Okular::Page *page = m_document->page(pageNumber);
    if (!page) {
        return;
    }

    // calculating the right coordinates to center the view on the annotation
    QRect rect = Okular::AnnotationUtils::annotationGeometry(annotation, page->width(), page->height());
    Okular::NormalizedRect nr(rect, (int)page->width(), (int)page->height());
    // set the viewport parameters
    Okular::DocumentViewport vp;
    vp.pageNumber = pageNumber;
    vp.rePos.enabled = true;
    vp.rePos.pos = Okular::DocumentViewport::Center;
    vp.rePos.normalizedX = (nr.right + nr.left) / 2.0;
    vp.rePos.normalizedY = (nr.bottom + nr.top) / 2.0;
    // setting the viewport
    m_document->setViewport(vp, nullptr, true);

    m_view->setReviewNavigationActive(true);
    Q_EMIT openReviewAnnotationWindow(selectedAnnotation, selectedPage);
    // Keep keyboard focus on the list so Backspace/Delete keep acting on the
    // selected comment instead of the freshly shown editor.
    m_view->setFocus(Qt::OtherFocusReason);
}

void Reviews::currentChanged(const QModelIndex &index)
{
    if (activateReviewCard(index)) {
        Q_EMIT reviewSelectionChanged(nullptr, -1);
        return;
    }
    clearActiveReviewCard();
    const QModelIndex annotIndex = annotationModelIndex(index);
    Okular::Annotation *annotation = annotIndex.data(AnnotationModel::UnplacedRole).toBool() ? nullptr : m_model->annotationForIndex(annotIndex);
    const int pageNumber = annotation ? m_model->data(annotIndex, AnnotationModel::PageRole).toInt() : -1;
    Q_EMIT reviewSelectionChanged(annotation, pageNumber);
}

void Reviews::openReview(const QModelIndex &index)
{
    if (activateReviewCard(index, true)) return;
    const QModelIndex annotIndex = annotationModelIndex(index);
    if (m_model->isAgentProposal(annotIndex)) {
        activateAgentProposal(index);
        return;
    }

    const QString id = annotIndex.data(AnnotationModel::ItemIdRole).toString();
    const int type = annotIndex.data(AnnotationModel::ItemTypeRole).toInt();
    if (annotIndex.data(AnnotationModel::DocumentFeedbackRole).toBool()) {
        Q_EMIT feedbackRequested(id, true);
        return;
    }
    if (id.isEmpty() || type != AnnotationModel::Comment) {
        return;
    }

    // Comments open through the single comment-opening path. Whether the PDF
    // location resolves is carried as data so the gesture and focus policy
    // stay identical for placed and unplaced comments.
    m_view->setReviewNavigationActive(true);
    Q_EMIT openCommentRequested(id, annotIndex.data(AnnotationModel::PageRole).toInt(), annotIndex.data(AnnotationModel::UnplacedRole).toBool());
    m_view->setFocus(Qt::OtherFocusReason);
}

void Reviews::activateAgentProposal(const QModelIndex &index)
{
    const AgentProposalSidebarEntry proposal = m_model->agentProposalForIndex(annotationModelIndex(index));
    if (!proposal.id.isEmpty()) {
        Q_EMIT agentProposalReviewRequested(proposal.id);
    }
}

void Reviews::updateTypeFilter()
{
    const QModelIndex selectedSource = annotationModelIndex(m_view->currentIndex());
    const int types = (m_commentsFilter->isChecked() ? AnnotationModel::Comment : 0) | (m_feedbackFilter->isChecked() ? AnnotationModel::Feedback : 0)
        | (m_proposalsFilter->isChecked() ? AnnotationModel::AgentProposal : 0)
        | (m_reviewCardsFilter->isChecked() ? AnnotationModel::ReviewCard : 0);
    m_filterProxy->setVisibleItemTypes(types);
    if (selectedSource.isValid()) {
        const QModelIndex viewIndex = m_authorProxy->mapFromSource(m_groupProxy->mapFromSource(m_filterProxy->mapFromSource(selectedSource)));
        if (viewIndex.isValid()) m_view->setCurrentIndex(viewIndex);
    }
    m_searchLine->updateSearch(m_searchLine->text());
    KConfigGroup config(KSharedConfig::openConfig(), QStringLiteral("Annotation Sidebar"));
    config.deleteEntry("ShowNotes"); // Notes left the Annotations list.
    config.writeEntry("ShowComments", m_commentsFilter->isChecked());
    config.writeEntry("ShowFeedback", m_feedbackFilter->isChecked());
    config.writeEntry("ShowProposals", m_proposalsFilter->isChecked());
    config.writeEntry("ShowReviewCards", m_reviewCardsFilter->isChecked());
    config.sync();
}

AgentProposalSidebarEntry Reviews::agentProposal(const QString &proposalId) const
{
    return m_suppressedAgentProposalIds.contains(proposalId) ? AgentProposalSidebarEntry {} : m_model->agentProposal(proposalId);
}

QString Reviews::loadAgentProposalSource(const QString &proposalId, const QString &path)
{
    const QString sourcePath = canonicalAgentProposalSourcePath(m_docsRoot, path);
    if (sourcePath.isEmpty()) {
        return i18n("The proposal's source is unavailable or outside the documentation folder. Its resolution status could not be checked.");
    }
    QFile source(sourcePath);
    if (!source.open(QIODevice::ReadOnly)) {
        return i18n("The proposal's source could not be read. Its resolution status could not be checked.");
    }
    const Thoughts::AgentProposalParse parsed = Thoughts::parseAgentProposals(sourcePath, QString::fromUtf8(source.readAll()));
    if (!parsed.proposal(proposalId)) {
        const QString code = parsed.refusalCode(proposalId);
        if (code != QLatin1String(Thoughts::AgentProposalIssueCode::UnknownId)) {
            return i18n("This proposal cannot be decided until its source is fixed: %1", Thoughts::agentProposalIssueMessage(code));
        }
        return i18n("This proposal ID is absent from the current source. It may have been resolved or edited; its absence alone does not identify an accept or reject decision.");
    }
    if (!m_additionalAgentProposalSources.contains(sourcePath)) {
        if (m_agentProposalRequestedUrl != m_document->currentDocument()) {
            m_additionalAgentProposalSources.clear();
            m_agentProposalRequestedUrl = m_document->currentDocument();
        }
        m_additionalAgentProposalSources.append(sourcePath);
        refreshAgentProposals();
    }
    return {};
}

QSet<QString> Reviews::decidedAgentProposalIds() const
{
    return m_decidedAgentProposalIds;
}

bool Reviews::agentProposalRefreshPending() const
{
    return m_agentProposalRefreshInFlight || m_agentProposalRefreshPending || m_agentProposalRefreshTimer->isActive();
}

void Reviews::suppressAgentProposal(const QString &proposalId)
{
    if (proposalId.isEmpty()) {
        return;
    }
    const AgentProposalSidebarEntry proposal = m_model->agentProposal(proposalId);
    if (!proposal.id.isEmpty()) {
        m_suppressedAgentProposals.insert(proposalId, proposal);
    }
    m_suppressedAgentProposalIds.insert(proposalId);
    m_model->removeAgentProposal(proposalId);
}

void Reviews::restoreAgentProposal(const QString &proposalId, bool reload)
{
    if (proposalId.isEmpty()) {
        return;
    }
    m_suppressedAgentProposalIds.remove(proposalId);
    const AgentProposalSidebarEntry proposal = m_suppressedAgentProposals.take(proposalId);
    if (!proposal.id.isEmpty() && m_model->agentProposal(proposalId).id.isEmpty()) {
        QList<AgentProposalSidebarEntry> proposals = m_model->agentProposals();
        proposals.append(proposal);
        m_model->setAgentProposals(proposals);
    }
    if (reload) {
        refreshAgentProposals();
    }
}

void Reviews::moveReview(bool forward)
{
    QModelIndex adjacentIndex = forward ? m_view->indexBelow(m_view->currentIndex()) : m_view->indexAbove(m_view->currentIndex());
    // Skip expanded headings, but keep collapsed groups reachable for Right to expand.
    while (adjacentIndex.isValid() && annotationModelIndex(adjacentIndex).data(AnnotationModel::ItemIdRole).toString().isEmpty() && m_view->isExpanded(adjacentIndex)) {
        adjacentIndex = forward ? m_view->indexBelow(adjacentIndex) : m_view->indexAbove(adjacentIndex);
    }
    if (!adjacentIndex.isValid()) {
        return;
    }

    Q_EMIT closeReviewAnnotationWindow();
    m_view->selectionModel()->setCurrentIndex(adjacentIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    m_view->scrollTo(adjacentIndex);
    m_view->setFocus(Qt::OtherFocusReason);
}

void Reviews::deleteAnnotation(const QModelIndex &index)
{
    const QModelIndex annotIndex = annotationModelIndex(index);
    const QString id = m_model->data(annotIndex, AnnotationModel::ItemIdRole).toString();
    const int type = m_model->data(annotIndex, AnnotationModel::ItemTypeRole).toInt();
    if (m_model->data(annotIndex, AnnotationModel::DocumentFeedbackRole).toBool()) {
        Q_EMIT feedbackDeleteRequested(id);
        return;
    }
    if (id.isEmpty() || type != AnnotationModel::Comment) return;

    QModelIndex adjacentIndex = adjacentAnnotationIndex(index, true);
    if (!adjacentIndex.isValid()) {
        adjacentIndex = adjacentAnnotationIndex(index, false);
    }
    const QPersistentModelIndex adjacentModelIndex = annotationModelIndex(adjacentIndex);
    const bool reviewNavigationActive = m_view->reviewNavigationActive();

    if (reviewNavigationActive) {
        Q_EMIT closeReviewAnnotationWindow();
    }

    Q_EMIT commentsDeleteRequested({id});

    QModelIndex nextViewIndex;
    if (adjacentModelIndex.isValid()) {
        const QModelIndex filterIndex = m_filterProxy->mapFromSource(adjacentModelIndex);
        const QModelIndex groupIndex = m_groupProxy->mapFromSource(filterIndex);
        nextViewIndex = m_authorProxy->mapFromSource(groupIndex);
        m_view->selectionModel()->setCurrentIndex(nextViewIndex, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        m_view->scrollTo(nextViewIndex);
    } else {
        m_view->selectionModel()->clearSelection();
        m_view->selectionModel()->clearCurrentIndex();
    }
    m_view->setFocus(Qt::OtherFocusReason);

    if (reviewNavigationActive && !nextViewIndex.isValid()) {
        m_view->setReviewNavigationActive(false);
    }
}

QModelIndex Reviews::annotationModelIndex(const QModelIndex &index) const
{
    if (!index.isValid() || index.model() != m_authorProxy) {
        return QModelIndex();
    }

    const QModelIndex authorIndex = m_authorProxy->mapToSource(index);
    const QModelIndex filterIndex = m_groupProxy->mapToSource(authorIndex);
    return m_filterProxy->mapToSource(filterIndex);
}

QModelIndex Reviews::adjacentAnnotationIndex(const QModelIndex &index, bool forward) const
{
    QModelIndex candidate = forward ? m_view->indexBelow(index) : m_view->indexAbove(index);
    while (candidate.isValid()) {
        if (!annotationModelIndex(candidate).data(AnnotationModel::ItemIdRole).toString().isEmpty()) {
            return candidate;
        }
        candidate = forward ? m_view->indexBelow(candidate) : m_view->indexAbove(candidate);
    }
    return QModelIndex();
}

QModelIndexList Reviews::retrieveAnnotations(const QModelIndex &idx) const
{
    QModelIndexList ret;
    if (idx.isValid()) {
        const QAbstractItemModel *model = idx.model();
        if (idx.data(AnnotationModel::ItemTypeRole).toInt() != 0) {
            ret += idx;
        } else if (model->hasChildren(idx)) {
            int rowCount = model->rowCount(idx);
            for (int i = 0; i < rowCount; i++) {
                ret += retrieveAnnotations(model->index(i, idx.column(), idx));
            }
        } else {
            ret += idx;
        }
    }

    return ret;
}

void Reviews::contextMenuRequested(const QPoint pos)
{
    const QModelIndex currentIndex = m_view->indexAt(pos);
    if (annotationModelIndex(currentIndex).data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::ReviewCard) {
        const QString id = annotationModelIndex(currentIndex).data(AnnotationModel::ItemIdRole).toString();
        // A right-click inside a selection of several cards keeps it, for Merge cards….
        QItemSelectionModel *selection = m_view->selectionModel();
        if (selection->isSelected(currentIndex)) selection->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
        else m_view->setCurrentIndex(currentIndex);
        // The selected cards in list order, whatever group they are under.
        QStringList selectedCards;
        const std::function<void(const QModelIndex &)> collect = [&](const QModelIndex &parent) {
            for (int index = 0; index < m_authorProxy->rowCount(parent); ++index) {
                const QModelIndex row = m_authorProxy->index(index, 0, parent);
                const QModelIndex source = annotationModelIndex(row);
                const QString cardId = source.data(AnnotationModel::ItemIdRole).toString();
                if (selection->isSelected(row) && source.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::ReviewCard
                    && !cardId.isEmpty() && !selectedCards.contains(cardId))
                    selectedCards.append(cardId);
                collect(row);
            }
        };
        collect({});
        QMenu menu(this);
        // Review now asks this one card in the Review window, whether or not it is due.
        auto *reviewNow = menu.addAction(i18n("Review now"));
        reviewNow->setObjectName(QStringLiteral("reviewSidebarReviewCard"));
        // Open card shows the card with its schedule and history in the Library's card browser.
        auto *open = menu.addAction(i18n("Open card"));
        open->setObjectName(QStringLiteral("openSidebarReviewCard"));
        auto *edit = menu.addAction(i18n("Edit…"));
        edit->setObjectName(QStringLiteral("editSidebarReviewCard"));
        // Two or more selected cards become one (Shell::mergeSidebarReviewCards).
        auto *merge = menu.addAction(i18n("Merge cards…"));
        merge->setObjectName(QStringLiteral("mergeSidebarReviewCards"));
        merge->setEnabled(selectedCards.size() >= 2);
        QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
        if (chosen == reviewNow) Q_EMIT reviewCardReviewRequested(id);
        else if (chosen == open) Q_EMIT reviewCardOpenRequested(id);
        else if (chosen == edit) Q_EMIT reviewCardEditRequested(id);
        else if (chosen == merge) Q_EMIT reviewCardsMergeRequested(selectedCards);
        return;
    }
    if (annotationModelIndex(currentIndex).data(AnnotationModel::DocumentFeedbackRole).toBool()) {
        const QString id = annotationModelIndex(currentIndex).data(AnnotationModel::ItemIdRole).toString();
        m_view->setCurrentIndex(currentIndex);
        QMenu menu(this);
        auto *edit = menu.addAction(i18n("Edit…"));
        edit->setObjectName(QStringLiteral("editSidebarFeedback"));
        const QJsonObject record = m_feedbackRecords.value(id);
        const bool resolved = Thoughts::DocumentFeedback::isResolved(record);
        QAction *resolve = nullptr;
        if (Thoughts::DocumentFeedback::canResolve(record)) {
            resolve = menu.addAction(resolved ? i18n("Reopen") : i18n("Resolve"));
            resolve->setObjectName(QStringLiteral("resolveSidebarFeedback"));
        }
        auto *remove = menu.addAction(i18n("Delete"));
        remove->setObjectName(QStringLiteral("deleteSidebarFeedback"));
        QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
        if (chosen == edit) Q_EMIT feedbackRequested(id, true);
        else if (chosen == remove) Q_EMIT feedbackDeleteRequested(id);
        else if (resolve && chosen == resolve) Q_EMIT feedbackResolveRequested(id, !resolved);
        return;
    }
    if (m_model->isAgentProposal(annotationModelIndex(currentIndex))) {
        agentProposalContextMenu(currentIndex, pos);
        return;
    }
    const auto detachedIndex = annotationModelIndex(currentIndex);
    if (!m_model->annotationForIndex(detachedIndex)) {
        const QString id = detachedIndex.data(AnnotationModel::ItemIdRole).toString();
        const int type = detachedIndex.data(AnnotationModel::ItemTypeRole).toInt();
        if (!id.isEmpty() && type == AnnotationModel::Comment) {
            QMenu menu(this);
            auto *remove = menu.addAction(i18n("Delete"));
            if (menu.exec(m_view->viewport()->mapToGlobal(pos)) == remove) Q_EMIT commentsDeleteRequested({id});
        }
        return;
    }
    AnnotationPopup popup(m_document, AnnotationPopup::SingleAnnotationMode, this);
    connect(&popup, &AnnotationPopup::openAnnotationWindow, this, &Reviews::openAnnotationWindow);
    connect(&popup, &AnnotationPopup::replyToAnnotation, this, &Reviews::replyToAnnotation);

    const QModelIndexList indexes = m_view->selectionModel()->selectedIndexes();
    for (const QModelIndex &index : indexes) {
        const QModelIndexList annotations = retrieveAnnotations(index);
        for (const QModelIndex &idx : annotations) {
            const QModelIndex authorIndex = m_authorProxy->mapToSource(idx);
            const QModelIndex filterIndex = m_groupProxy->mapToSource(authorIndex);
            const QModelIndex annotIndex = m_filterProxy->mapToSource(filterIndex);
            Okular::Annotation *annotation = m_model->annotationForIndex(annotIndex);
            if (annotation) {
                const int pageNumber = m_model->data(annotIndex, AnnotationModel::PageRole).toInt();
                popup.addAnnotation(annotation, pageNumber);
            }
        }
    }

    popup.exec(m_view->viewport()->mapToGlobal(pos));
    m_view->setFocus(Qt::OtherFocusReason);
}

void Reviews::agentProposalContextMenu(const QModelIndex &row, const QPoint pos)
{
    QItemSelectionModel *selection = m_view->selectionModel();
    if (!selection->isSelected(row)) selection->setCurrentIndex(row, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
    // Every proposal listed now, whatever group it is under and with the search applied.
    QModelIndexList rows;
    const std::function<void(const QModelIndex &)> collect = [this, &rows, &collect](const QModelIndex &parent) {
        for (int index = 0; index < m_authorProxy->rowCount(parent); ++index) {
            const QModelIndex child = m_authorProxy->index(index, 0, parent);
            if (m_model->isAgentProposal(annotationModelIndex(child))) rows.append(child);
            collect(child);
        }
    };
    collect({});
    QStringList listed;
    QStringList selected;
    for (const QModelIndex &proposal : std::as_const(rows)) {
        const QString id = m_model->agentProposalForIndex(annotationModelIndex(proposal)).id;
        listed.append(id);
        if (selection->isSelected(proposal)) selected.append(id);
    }
    const bool several = selected.size() > 1;
    QMenu menu(this);
    QAction *accept = menu.addAction(several ? i18n("Accept %1 Selected", selected.size()) : i18n("Accept"));
    accept->setObjectName(QStringLiteral("acceptSidebarProposals"));
    QAction *reject = menu.addAction(several ? i18n("Reject %1 Selected", selected.size()) : i18n("Reject"));
    reject->setObjectName(QStringLiteral("rejectSidebarProposals"));
    QAction *selectAll = nullptr;
    QAction *acceptAll = nullptr;
    QAction *rejectAll = nullptr;
    if (selected.size() < listed.size()) {
        menu.addSeparator();
        selectAll = menu.addAction(i18n("Select All"));
        selectAll->setObjectName(QStringLiteral("selectAllSidebarProposals"));
        acceptAll = menu.addAction(i18n("Accept All (%1)", listed.size()));
        acceptAll->setObjectName(QStringLiteral("acceptAllSidebarProposals"));
        rejectAll = menu.addAction(i18n("Reject All (%1)", listed.size()));
        rejectAll->setObjectName(QStringLiteral("rejectAllSidebarProposals"));
    }
    QAction *chosen = menu.exec(m_view->viewport()->mapToGlobal(pos));
    if (!chosen) return;
    if (chosen == selectAll) {
        QItemSelection all;
        for (const QModelIndex &proposal : std::as_const(rows)) all.select(proposal, proposal);
        selection->select(all, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
        return;
    }
    const QLatin1String action(chosen == accept || chosen == acceptAll ? "accept" : "disregard");
    Q_EMIT agentProposalsResolutionRequested(chosen == accept || chosen == reject ? selected : listed, action);
}

void Reviews::saveSearchOptions()
{
    Okular::Settings::setReviewsSearchRegularExpression(m_searchLine->regularExpression());
    Okular::Settings::setReviewsSearchCaseSensitive(m_searchLine->caseSensitivity() == Qt::CaseSensitive ? true : false);
    Okular::Settings::self()->save();
}

class AnnotationStateSaver : public KViewStateSerializer
{
    Q_OBJECT
public:
    explicit AnnotationStateSaver(QTreeView *view)
        : KViewStateSerializer()
    {
        setView(view);
    }
    QString indexToConfigString(const QModelIndex &index) const override
    {
        const QString id = index.data(AnnotationModel::ItemIdRole).toString();
        if (!id.isEmpty()) return QStringLiteral("id:") + id;
        // Store the display text, with parent if any, e.g. "Page 4 / David Faure"
        QString key = index.data().toString();
        if (auto parent = index.parent(); parent.isValid()) {
            key.prepend(parent.data().toString() + QLatin1String(" / "));
        }
        return key;
    }
    QModelIndex indexFromConfigString(const QAbstractItemModel *model, const QString &key) const override
    {
        if (key.startsWith(QLatin1String("id:"))) {
            return model->match(model->index(0, 0), AnnotationModel::ItemIdRole, key.mid(3), 1, Qt::MatchExactly | Qt::MatchRecursive).value(0);
        }
        const auto matchFirst = [model](const QModelIndex &startIndex, const QString &text) {
            const QModelIndexList list = model->match(startIndex, Qt::DisplayRole, text, 1, Qt::MatchRecursive);
            if (list.isEmpty()) {
                return QModelIndex();
            }
            return list.first();
        };

        const QStringList subKeys = key.split(QStringLiteral(" / "));
        QModelIndex index = model->index(0, 0);
        for (const QString &subKey : subKeys) {
            index = matchFirst(index, subKey);
            if (!index.isValid()) {
                break;
            }
        }
        return index;
    }
};

void Reviews::saveTreeState()
{
    AnnotationStateSaver saver(m_view);
    m_expansionKeys = saver.expansionKeys();
}

void Reviews::restoreTreeState()
{
    AnnotationStateSaver saver(m_view);
    saver.restoreExpanded(m_expansionKeys);
    if (m_searchLine && !m_searchLine->text().isEmpty()) m_searchLine->updateSearch(m_searchLine->text());
}

QAbstractItemModel *Reviews::annotationsModel() const
{
    return m_model;
}

#include "side_reviews.moc"

void Reviews::setUnplacedComments(const QMap<QString, QString> &comments)
{
    m_model->setUnplacedComments(comments);
}

void Reviews::setReviewCards(const QJsonArray &cards)
{
    if (!m_document->isOpened() || !m_document->currentDocument().isLocalFile()) return;
    const QFileInfo pdf(m_document->currentDocument().toLocalFile());
    if (m_reviewPdfSha256.isEmpty()) {
        QFile file(pdf.absoluteFilePath());
        if (file.open(QIODevice::ReadOnly)) {
            QCryptographicHash hash(QCryptographicHash::Sha256);
            if (hash.addData(&file)) m_reviewPdfSha256 = QString::fromLatin1(hash.result().toHex());
        }
    }
    if (cards == m_reviewCardRecords) return;
    m_reviewCardRecords = cards;
    rebuildReviewCards();
}

void Reviews::rebuildReviewCards()
{
    if (!m_document->isOpened() || !m_document->currentDocument().isLocalFile()) return;
    const QFileInfo pdf(m_document->currentDocument().toLocalFile());
    const auto reading = Thoughts::ProjectReadingContext::load(pdf.absoluteFilePath());
    // An earlier version opened from History lists the cards retired on it, and draws them.
    const bool earlierVersion = Thoughts::PdfVersions::isCachedVersion(pdf.absoluteFilePath());
    clearActiveReviewCard();
    m_reviewCards.clear();
    QList<ReviewCardSidebarEntry> entries;
    QHash<QString, std::pair<double, double>> positions;
    QVariantList highlights;
    int retiredElsewhere = 0;
    for (const QJsonValue &value : std::as_const(m_reviewCardRecords)) {
        const QJsonObject card = value.toObject();
        const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
        const QString id = card.value(QStringLiteral("card_id")).toString();
        const QString status = card.value(QStringLiteral("status")).toString();
        const QString question = primary.value(QStringLiteral("question")).toString().trimmed();
        const QString cardPdfSha256 = primary.value(QStringLiteral("pdf_sha256")).toString();
        const bool retired = card.value(QStringLiteral("sidebar_schedule")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("retired");
        const auto *child = reading ? reading->forStem(primary.value(QStringLiteral("pdf_stem")).toString()) : nullptr;
        if (id.isEmpty() || question.isEmpty() || m_reviewCards.contains(id) || card.value(QStringLiteral("sidebar_deleted")).toBool() || card.value(QStringLiteral("deleted")).toBool()) continue;
        if (earlierVersion) {
            if (!retired || m_reviewPdfSha256.isEmpty() || cardPdfSha256 != m_reviewPdfSha256) continue;
        } else if ((reading ? !child : primary.value(QStringLiteral("pdf_stem")).toString() != pdf.completeBaseName())
                   || (status != QLatin1String("approved") && status != QLatin1String("needs_reanchor"))) {
            continue;
        }
        // On the Library's PDF a retired card is neither drawn nor listed, unless the Retired filter asks.
        if (retired && !earlierVersion) {
            ++retiredElsewhere;
            if (!m_retiredCardsFilter->isChecked()) continue;
        }
        QJsonArray regions;
        for (const auto &value : Thoughts::effectiveReviewCardRegions(primary, card.value(QStringLiteral("schema_version")).toInt())) {
            regions.append(Thoughts::offsetCommentPages(value.toObject(), child ? child->document.firstPage : 0));
        }
        const QString state = card.value(QStringLiteral("reanchor")).toObject().value(QStringLiteral("state")).toString();
        const bool placedHere = !regions.isEmpty() && !m_reviewPdfSha256.isEmpty() && cardPdfSha256 == (child ? child->document.pdfSha256 : m_reviewPdfSha256);
        // A retired card keeps the placement it had when it was retired: valid on its own version only.
        bool valid = retired ? earlierVersion && placedHere
                             : placedHere && status == QLatin1String("approved") && (state.isEmpty() || state == QLatin1String("succeeded"));
        QList<int> pages;
        for (const QJsonValue &regionValue : regions) {
            const QJsonObject region = regionValue.toObject();
            const int page = region.value(QStringLiteral("page")).toInt() - 1;
            valid = valid && page >= 0 && page < int(m_document->pages()) && Thoughts::validNormalizedReviewCoordinates(region.value(QStringLiteral("rect")).toArray(), 4);
            if (!pages.contains(page)) pages.append(page);
        }
        std::sort(pages.begin(), pages.end());
        if (valid) {
            // Order cards on a page as the answers appear: top edge, then left edge.
            std::pair<double, double> position{2.0, 2.0};
            for (const QJsonValue &regionValue : regions) {
                const QJsonObject region = regionValue.toObject();
                const QJsonArray rect = region.value(QStringLiteral("rect")).toArray();
                if (region.value(QStringLiteral("page")).toInt() - 1 == pages.first()) position = std::min(position, {rect.at(1).toDouble(), rect.at(0).toDouble()});
            }
            positions.insert(id, position);
        }
        QJsonObject entry = card;
        entry.insert(QStringLiteral("sidebar_regions"), valid ? regions : QJsonArray());
        m_reviewCards.insert(id, entry);
        if (valid) highlights.append(QVariant(regions.toVariantList()));
        // The review queue leaves out a card made on another PDF than the Library's current one
        // (ReviewStore::activeCards); the shell left that to this tab, which shows that PDF.
        QJsonObject schedule = card.value(QStringLiteral("sidebar_schedule")).toObject();
        const QString shownSha256 = child ? child->document.pdfSha256 : m_reviewPdfSha256;
        const QString scheduleState = schedule.value(QStringLiteral("state")).toString();
        const bool placementPending = !valid || status == QLatin1String("needs_reanchor")
            || (!state.isEmpty() && state != QLatin1String("succeeded"));
        if (!retired && !shownSha256.isEmpty() && !Thoughts::isHtmlAttachmentCard(card) && cardPdfSha256 != shownSha256
            && (scheduleState == QLatin1String("new") || scheduleState == QLatin1String("learning") || scheduleState == QLatin1String("review"))) {
            schedule.insert(QStringLiteral("state"), QStringLiteral("unavailable"));
            schedule.remove(QStringLiteral("due_from"));
        }
        // Why the queue does not offer it (AnnotationModel::reviewCardSchedule words it).
        if (schedule.value(QStringLiteral("state")).toString() == QLatin1String("unavailable") && placementPending && !Thoughts::isHtmlAttachmentCard(card))
            schedule.insert(QStringLiteral("not_offered"), QStringLiteral("placement"));
        ReviewCardSidebarEntry sidebarEntry{id, question, valid ? pages.first() : -1, valid ? pages : QList<int>(),
                                            QCryptographicHash::hash(QJsonDocument(entry).toJson(QJsonDocument::Compact), QCryptographicHash::Sha256),
                                            status == QLatin1String("candidate"), primary.value(QStringLiteral("scope")).toString() == QLatin1String("document"), schedule};
        sidebarEntry.retired = retired;
        sidebarEntry.pdfSha256 = cardPdfSha256;
        entries.append(sidebarEntry);
    }
    std::sort(entries.begin(), entries.end(), [&positions](const auto &a, const auto &b) {
        return std::tuple(a.page, positions.value(a.id), a.id) < std::tuple(b.page, positions.value(b.id), b.id);
    });
    const bool showingRetired = m_retiredCardsFilter->isChecked();
    m_retiredCardsFilter->setText(showingRetired ? i18np("Retired: 1 card shown, unplaced: it stays on the PDF version it was retired on",
                                                         "Retired: %1 cards shown, unplaced: each stays on the PDF version it was retired on", retiredElsewhere)
                                                 : i18np("Retired: 1 card, on the PDF version it was retired on", "Retired: %1 cards, on the PDF versions they were retired on",
                                                         retiredElsewhere));
    m_retiredCardsFilter->setVisible(retiredElsewhere > 0);
    m_model->setReviewCards(entries);
    Q_EMIT reviewCardHighlightsChanged(highlights);
    m_searchLine->updateSearch(m_searchLine->text());
}

void Reviews::setDocumentFeedback(const QList<QJsonObject> &records)
{
    // Long passages are cut: the row is there to find it, the page shows the rest.
    const auto shortened = [](const QString &text) {
        const QString line = text.simplified();
        return line.size() > 160 ? line.left(160).trimmed() + QStringLiteral("…") : line;
    };
    QList<FeedbackSidebarEntry> entries;
    QHash<QString, std::pair<qreal, qreal>> positions;
    m_feedbackRecords.clear();
    for (const QJsonObject &record : records) {
        if (record.value(QStringLiteral("deleted")).toBool()) continue;
        const QString id = record.value(QStringLiteral("feedback_id")).toString();
        m_feedbackRecords.insert(id, record);
        const QString comment = shortened(record.value(QStringLiteral("comment")).toString());
        const QString passage = Thoughts::DocumentFeedback::isDrawing(record) ? i18n("Drawing")
            : Thoughts::DocumentFeedback::isTextBox(record) ? i18n("Text box")
            : shortened(record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString());
        const auto regions = Thoughts::ReviewCardFeedback::regionsOf(record);
        // A drawing or a text box in its place on the page, not at the top of its whole-page region.
        const QRectF place = Thoughts::DocumentFeedback::isPageMark(record) ? Thoughts::DocumentFeedback::pageMarkBounds(record) : regions.isEmpty() ? QRectF() : regions.first().second;
        if (!regions.isEmpty()) positions.insert(id, {place.top(), place.left()});
        const QString attachment = !Thoughts::DocumentFeedback::isAttachmentFeedback(record) ? QString()
            : QFileInfo(record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("path")).toString())
                  .completeBaseName().replace(QLatin1Char('_'), QLatin1Char(' ')).replace(QLatin1Char('-'), QLatin1Char(' '));
        // Under the text: the passage under a comment, and what the row is not any more.
        QStringList notes;
        if (!comment.isEmpty()) notes.append(passage);
        if (Thoughts::DocumentFeedback::isResolved(record)) notes.append(i18n("Resolved"));
        if (m_earlierFeedbackIds.contains(id)) notes.append(i18n("Earlier version"));
        entries.append({id, record.value(QStringLiteral("polarity")).toString() != Thoughts::ReviewCardFeedback::negative(), comment.isEmpty() ? passage : comment,
                        notes.join(QStringLiteral(" · ")), attachment, regions.isEmpty() ? -1 : regions.first().first - 1, Thoughts::DocumentFeedback::kindOf(record)});
    }
    std::sort(entries.begin(), entries.end(), [&positions](const auto &a, const auto &b) {
        return std::tuple(a.page < 0, a.page, positions.value(a.id), a.id) < std::tuple(b.page < 0, b.page, positions.value(b.id), b.id);
    });
    m_model->setFeedback(entries);
    m_searchLine->updateSearch(m_searchLine->text());
}

void Reviews::setEarlierFeedback(const QSet<QString> &earlier, int earlierCount, bool showing)
{
    m_earlierFeedbackIds = earlier;
    m_earlierFeedback->setChecked(showing);
    m_earlierFeedback->setText(showing ? i18n("Hide earlier versions") : i18np("%1 from an earlier version", "%1 from earlier versions", earlierCount));
    m_earlierFeedback->setVisible(showing || earlierCount > 0);
}

void Reviews::clearActiveReviewCard()
{
    if (m_activeReviewCardId.isEmpty()) return;
    const QString previous = std::exchange(m_activeReviewCardId, {});
    m_model->setReviewCardStatus(previous, {});
    Q_EMIT clearReviewCardPresentationRequested();
}

void Reviews::editActiveReviewCard()
{
    if (!m_activeReviewCardId.isEmpty()) Q_EMIT reviewCardEditRequested(m_activeReviewCardId);
}

bool Reviews::selectReviewCard(const QString &id)
{
    const auto find = [&](auto &&self, const QModelIndex &parent) -> QModelIndex {
        for (int row = 0; row < m_view->model()->rowCount(parent); ++row) {
            const auto index = m_view->model()->index(row, 0, parent);
            if (index.data(AnnotationModel::ItemTypeRole).toInt() == AnnotationModel::ReviewCard && index.data(AnnotationModel::ItemIdRole).toString() == id) return index;
            const auto child = self(self, index);
            if (child.isValid()) return child;
        }
        return {};
    };
    const auto index = find(find, {});
    if (!index.isValid()) return false;
    m_view->setCurrentIndex(index);
    m_view->scrollTo(index);
    return activateReviewCard(index, true);
}

bool Reviews::activateReviewCard(const QModelIndex &index, bool navigate)
{
    const QModelIndex source = annotationModelIndex(index);
    if (source.data(AnnotationModel::ItemTypeRole).toInt() != AnnotationModel::ReviewCard) return false;
    const QString id = source.data(AnnotationModel::ItemIdRole).toString();
    const QJsonArray regions = m_reviewCards.value(id).value(QStringLiteral("sidebar_regions")).toArray();
    if (regions.isEmpty()) {
        clearActiveReviewCard();
        return true;
    }
    const bool changed = id != m_activeReviewCardId;
    if (changed) {
        clearActiveReviewCard();
        m_activeReviewCardId = id;
    }
    if (!changed && !navigate) return true;
    Q_EMIT reviewCardPresentationRequested(regions.toVariantList(), changed || navigate);
    m_model->setReviewCardStatus(id, i18n("Answer highlighted in PDF"));
    m_view->doItemsLayout();
    return true;
}
