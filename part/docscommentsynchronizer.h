/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_DOCSCOMMENTSYNCHRONIZER_H
#define OKULAR_DOCSCOMMENTSYNCHRONIZER_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QJsonDocument>
#include <QJsonValue>
#include <QList>
#include <QProcessEnvironment>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimeZone>
#include <QUrl>

#include <functional>
#include <memory>
#include <optional>

namespace Okular
{

struct DocsAnchorOccurrence {
    QString path;
    int line = 0;
};

using DocsAnchorOccurrences = QHash<QString, QList<DocsAnchorOccurrence>>;

struct DocsAnchorIndex {
    DocsAnchorOccurrences occurrences;
    bool complete = false;
};

/**
 * Synchronously persists one saved docs-workspace PDF's annotations and
 * performs source anchoring only for annotations whose source position is new
 * or changed.
 *
 * This starts no Python process. The Part owns lightweight in-process
 * watchers used to consume companion-side sidecar updates and reconcile
 * source-anchor edits.
 */
class DocsCommentSynchronizer
{
public:
    struct PageGeometry {
        double width = 0.0;
        double height = 0.0;
        int rotation = 0;
    };

    using PageGeometryProvider = std::function<std::optional<PageGeometry>(const QString &pdfPath, int pageNumber, QString *error)>;

    struct Options {
        QString docsRoot;
        QString sourceBundleRoot;
        QString synctexExecutable;
        QProcessEnvironment processEnvironment;
        int synctexTimeoutMs = 1000;
        int lockTimeoutMs = 2000;
        bool allowLegacySourceResolution = true;
        // Opt in only for a short-lived, read-only localization operation.
        bool cacheSourceResolution = false;
        bool cacheSourceProjection = false;
        // Explicit dependency closure for background reconciliation; an empty set
        // must remain empty rather than falling back to a workspace scan.
        std::optional<QSet<QString>> commentSourceFiles;
        bool commentSourceFilesComplete = false;
        // Sources the desktop tracks (TeX and Markdown) in addition to the build
        // layouts' managed TeX inputs, which never list Markdown documents.
        QSet<QString> additionalCommentSourceFiles;
    };

    struct Result {
        bool success = false;
        QString failureReason;
        QStringList logLines;
        int visibleComments = 0;
        int plainMarkups = 0;
        int deletions = 0;
        int anchorCandidates = 0;
        int anchorAttempts = 0;
        int anchorsInserted = 0;
        int anchorMirrorsUpdated = 0;
        int anchorsRemoved = 0;
        // Legacy counter: new clients never delete a comment because its marker vanished.
        int sourceAnchorDeletions = 0;
        int sourceAnchorsOrphaned = 0;
        int sourceCommentEditsImported = 0;
        int sourceCommentsMoved = 0;
        int unchangedComments = 0;
        QStringList watchedSourceFiles;
        QByteArray visibleSidecarFingerprint;
        bool watchedSourceFilesComplete = false;
        qint64 loadMs = 0;
        qint64 anchorMs = 0;
        qint64 writeMs = 0;
        qint64 totalMs = 0;
    };

    struct DocumentResult {
        QString pdfStem;
        Result result;
    };

    struct BatchResult {
        bool success = false;
        QString failureReason;
        QList<DocumentResult> documents;
        int visibleComments = 0;
        int plainMarkups = 0;
        int anchorsInserted = 0;
        int anchorMirrorsUpdated = 0;
        int sourceAnchorDeletions = 0;
        int sourceAnchorsOrphaned = 0;
        int sourceCommentEditsImported = 0;
        int sourceCommentsMoved = 0;
        qint64 totalMs = 0;
    };

    struct CommentMetadataUpdateResult {
        bool success = false;
        bool changed = false;
        QString failureReason;
        QString previousContents;
        QString currentContents;
        QJsonObject previousMetadata;
        QJsonObject currentMetadata;
        bool sourceMirrorUpdated = false;
        qint64 totalMs = 0;
    };

    struct CommentDeletionResult {
        bool success = false;
        bool changed = false;
        QString failureReason;
        QStringList deletedIds;
        QList<QJsonObject> previousRecords;
        qint64 totalMs = 0;
    };

    struct CommentMutationBatchResult {
        bool success = false;
        QString failureReason;
        QList<QJsonObject> changedRecords;
        QByteArray visibleFingerprint;
    };

    struct SourceResolutionRequest {
        QString pdfStem;
        int pageNumber = 0;
        // Top-left page coordinates in PDF points, never normalized fractions.
        double x = 0.0;
        double y = 0.0;
        QString selectedText = {};
        QString pdfPath = {};
        bool requireExactPassage = false;
    };

    struct SourceResolutionResult {
        enum class Problem { Unavailable, AmbiguousLocation, GeneratedContent };

        bool success = false;
        QString failureReason;
        QString sourceId;
        QString sourcePath;
        QString buildId;
        int line = 0;
        int column = -1;
        Problem problem = Problem::Unavailable;
        // A verified, editable file that can be opened without claiming a line match.
        QString fallbackSourcePath;
    };

    struct AnchorRestoreResult {
        enum class Placement { Exact, NearestLine, EndOfFile, FallbackFile, AlreadyPresent };
        bool success = false;
        QString failureReason;
        QString sourceFile;
        int requestedLine = 0;
        int line = 0;
        Placement placement = Placement::Exact;
        bool pointAnchorFallback = false;
        QString message;
    };

    explicit DocsCommentSynchronizer(Options options);
    ~DocsCommentSynchronizer();

    DocsCommentSynchronizer(const DocsCommentSynchronizer &) = delete;
    DocsCommentSynchronizer &operator=(const DocsCommentSynchronizer &) = delete;

    Result synchronize(const QUrl &documentUrl, const QJsonObject &annotationSnapshot);
    Result reconcileSourceAnchorChanges(const QUrl &documentUrl);
    Result prepareSidecar(const QString &pdfStem, const PageGeometryProvider &pageGeometry);
    Result prepareAnnotation(const QString &pdfStem,
                             const QString &annotationId,
                             const PageGeometryProvider &pageGeometry,
                             const QString &exactPdfPath = {});
    BatchResult prepareAllSidecars(const PageGeometryProvider &pageGeometry, const QString &onlyPdfStem = {}, const QSet<QString> &excludedPdfStems = {});
    SourceResolutionResult resolveSource(const SourceResolutionRequest &request);
    // A read projects existing markers; a mutation writes through to source before publishing its record.
    static QJsonObject sourceCommentFileIndex(const QString &path, const QStringList &identifiers);
    // Remove the single marker of a comment that no longer exists ("Comment missing"):
    // a highlight keeps its words, a marker alone on its line takes the line with it,
    // text before it keeps a trailing '%'. The edit is verified (this marker gone,
    // every other marker kept) and the original bytes are restored otherwise.
    static bool removeMissingCommentMarker(const QString &sourcePath, const QString &anchorId, QString *error = nullptr);
    bool sourceAuthoritativeComment(QJsonObject *comment, const QJsonObject &previous,
                                    const PageGeometryProvider &geometry, bool mutate, QString *error = nullptr);

    // Re-insert the marker of an anchor-removed comment at its last known line, or
    // the nearest safe line. A logical root payload or a sidecar record is accepted.
    // This is a pure source edit; the caller publishes the source normally.
    AnchorRestoreResult restoreRemovedAnchor(const QJsonObject &rootPayload);
    // Remove the unique marker of a comment from one source file (TeX or Markdown).
    static bool removeCommentAnchor(const QString &sourcePath, const QString &anchorId, QString *error = nullptr);

    static QStringList sourceFilesForDocument(const QString &docsRoot, const QString &pdfStem, QString *error = nullptr);
    static QList<QJsonObject> visibleSidecarComments(const QString &docsRoot, const QString &pdfStem, QByteArray *fingerprint, QString *error, QSet<QString> *suppressedIds = nullptr);
    static CommentMetadataUpdateResult updateSidecarCommentMetadata(const QString &docsRoot,
                                                                    const QString &pdfStem,
                                                                    const QString &annotationId,
                                                                    const QJsonObject &patch,
                                                                    int lockTimeoutMs = 2000);
    static CommentMetadataUpdateResult updateSidecarComment(const QString &docsRoot,
                                                            const QString &pdfStem,
                                                            const QString &annotationId,
                                                            const QString &contents,
                                                            const QJsonObject &metadataPatch = {},
                                                            int lockTimeoutMs = 2000);
    static CommentMetadataUpdateResult updateSidecarCommentChatGptUrl(const QString &docsRoot,
                                                                      const QString &pdfStem,
                                                                      const QString &annotationId,
                                                                      const QString &url,
                                                                      int lockTimeoutMs = 2000);
    static CommentDeletionResult deleteSidecarComments(const QString &docsRoot,
                                                        const QString &pdfStem,
                                                        const QStringList &annotationIds,
                                                        int lockTimeoutMs = 2000);
    static CommentDeletionResult restoreSidecarComments(const QString &docsRoot,
                                                         const QString &pdfStem,
                                                         const QList<QJsonObject> &previousRecords,
                                                         int lockTimeoutMs = 2000);
    static CommentMutationBatchResult applySidecarMutations(const QString &docsRoot,
                                                            const QString &pdfStem,
                                                            const QList<QJsonObject> &mutations,
                                                            int lockTimeoutMs = 2000);
    static QString normalizedChatGptUrl(const QString &url, QString *error = nullptr);
    static QDateTime parseCommentDate(const QJsonValue &value, const QTimeZone &localTimeZone = QTimeZone::systemTimeZone());
    static bool isTrackedCommentRecord(const QJsonObject &record);

private:
    struct SynchronizationCache;
    QSet<QString> commentSourceSet() const;
    QHash<QString, QByteArray> m_resolutionObjects;
    QHash<QString, QJsonDocument> m_resolutionJson;
    QHash<QString, QList<QList<QByteArray>>> m_resolutionPages;

    Result synchronize(const QUrl &documentUrl,
                       const QJsonObject &annotationSnapshot,
                       bool prepareUnanchored,
                       DocsAnchorIndex *sharedAnchorIndex = nullptr,
                       const QString &onlyAnnotationId = {},
                       const QString &canonicalPdfStem = {});
    Result prepareSidecar(const QString &pdfStem,
                          const PageGeometryProvider &pageGeometry,
                          DocsAnchorIndex *sharedAnchorIndex,
                          const QString &onlyAnnotationId = {},
                          const QString &exactPdfPath = {});

    Options m_options;
    std::unique_ptr<SynchronizationCache> m_cache;
};

}

#endif
