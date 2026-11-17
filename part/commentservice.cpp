// SPDX-License-Identifier: GPL-2.0-or-later
// Desktop storage/render adapter. Selection, cascade and lifecycle history live in the shared SDK.
#include "part.h"
#include "thoughtsplatform.h"
#include "fedorarepository.h"
#include "docscommentsynchronizer.h"
#include "commentfragmentids.h"
#include "core/annotations.h"
#include "core/page.h"
#include <QDateTime>
#include <QFileInfo>
#include <QScopedValueRollback>
#include <QScopeGuard>

namespace Okular {
namespace {
QVariantMap resultMap(const Thoughts::CommentResult &result) {
    return {{QStringLiteral("success"), result.success}, {QStringLiteral("changed"), result.success && !result.ids.isEmpty()},
        {QStringLiteral("error"), result.error}, {QStringLiteral("deletedIds"), result.ids}, {QStringLiteral("serviceToken"), result.historyToken}};
}
}
Thoughts::CommentService &Part::commentService()
{
    if (!m_commentService) m_commentService = std::make_unique<Thoughts::CommentService>(
        [this] { return commentRecords(); }, [this](const auto &changes, QString *error) { return commitCommentChanges(changes, error); },
        [] { return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs); }, Thoughts::Platform::desktopOriginId());
    return *m_commentService;
}

QList<Thoughts::CommentRecord> Part::commentRecords()
{
    QHash<QString, Thoughts::CommentRecord> records = m_legacyCommentStates;
    // Keep original sidecar identity and metadata, even if no PDF annotation exists.
    for (auto payload : DocsCommentSynchronizer::visibleSidecarComments(m_docsRenderRoot, QFileInfo(localFilePath()).completeBaseName(), nullptr, nullptr)) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        if (id.isEmpty() || m_legacyCommentStates.contains(id)) continue;
        payload.insert(QStringLiteral("_legacy"), true);
        records.insert(id, {id, payload.value(QStringLiteral("in_reply_to")).toString(), payload});
    }
    for (uint page = 0; page < m_document->pages(); ++page) {
        for (auto *annotation : m_document->page(page)->annotations()) {
            const QString id = Thoughts::logicalCommentId(annotation->uniqueName());
            if (id.isEmpty() || records.contains(id)) continue;
            auto payload = currentDocsCommentRecord(id);
            payload.insert(QStringLiteral("_legacy"), true);
            payload.insert(QStringLiteral("page"), int(page) + 1);
            payload.insert(QStringLiteral("deleted"), false);
            const auto *parent = annotation->previousAnnotation();
            records.insert(id, {id, parent ? Thoughts::logicalCommentId(parent->uniqueName()) : QString(), payload});
        }
    }
    const auto projections = effectiveFedoraProjections();
    for (auto it = projections.begin(); it != projections.end(); ++it) {
        if (!it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))) continue;
        const auto payload = it.value().toObject();
        const QString id = payload.value(QStringLiteral("comment_id")).toString();
        if (!id.isEmpty()) records.insert(id, {id, payload.value(QStringLiteral("parent_id")).toString(), payload});
    }
    auto keys = records.keys(); keys.sort();
    QList<Thoughts::CommentRecord> result;
    for (const auto &key : keys) result.append(records.value(key));
    return result;
}

bool Part::commitCommentChanges(const QList<Thoughts::CommentChange> &changes, QString *error)
{
    QScopedValueRollback<bool> applying(m_applyingCommentStorage, true);
    const auto viewport = m_document->viewport();
    const auto restoreViewport = qScopeGuard([&] {
        if (m_document->isOpened() && viewport.pageNumber >= 0 && uint(viewport.pageNumber) < m_document->pages())
            m_document->setViewport(viewport, nullptr, false, false);
    });
    QList<QJsonObject> logical;
    QVariantList legacy;
    bool restore = false;
    bool redo = false;
    for (const auto &change : changes) {
        if (change.before.payload.value(QStringLiteral("_legacy")).toBool()) {
            restore = !change.after.payload.value(QStringLiteral("deleted")).toBool();
            // Redo the earlier deletion only when the history is redone: a new deletion of a
            // restored comment would otherwise redo whichever deletion was undone last.
            redo = m_commentHistoryStep && change.before.payload.contains(QStringLiteral("_legacyHistory"));
            legacy.append(QVariantMap{{QStringLiteral("annotationId"), change.before.id}, {QStringLiteral("page"), change.before.payload.value(QStringLiteral("page")).toInt()}});
        } else logical.append(change.after.payload);
    }
    // A legacy adapter owns PDF persistence and its existing native undo representation.
    QVariantMap legacyResult;
    if (!legacy.isEmpty()) {
        legacyResult = restore ? undoReviewChange() : redo ? redoReviewChange() : deleteLegacyReviewAnnotations(legacy);
        if (!legacyResult.value(QStringLiteral("success")).toBool()) {
            if (error) *error = legacyResult.value(QStringLiteral("error")).toString();
            return false;
        }
    }
    if (!logical.isEmpty() && (!m_fedoraRepository || !m_fedoraRepository->enqueueLogicalComments(logical))) {
        if (!legacy.isEmpty()) { if (restore) redoReviewChange(); else undoReviewChange(); }
        if (error) *error = QStringLiteral("The comments could not be saved to the local sync queue.");
        return false;
    }
    for (const auto &change : changes) {
        if (change.before.payload.value(QStringLiteral("_legacy")).toBool()) {
            auto record = change.after;
            record.payload.insert(QStringLiteral("_legacyHistory"), true);
            m_legacyCommentStates.insert(record.id, record);
        }
    }
    return true;
}

QVariantMap Part::deleteComments(const QStringList &ids) {
    const auto result = commentService().deleteComments(ids);
    if (result.success && result.historyToken) {
        m_commentUndoTokens.append(result.historyToken); m_commentRedoTokens.clear();
        m_commentHistoryDocumentIndex[result.historyToken] = m_document->undoHistoryIndex();
    }
    updateUndoAction();
    auto mapped = resultMap(result);
    mapped.insert(QStringLiteral("canUndo"), !m_commentUndoTokens.isEmpty());
    mapped.insert(QStringLiteral("canRedo"), !m_commentRedoTokens.isEmpty());
    return mapped;
}
QVariantMap Part::undoCommentDeletion(int token) {
    QScopedValueRollback<bool> historyStep(m_commentHistoryStep, true);
    const auto result = commentService().undo(token);
    if (result.success) { m_commentUndoTokens.removeAll(token); m_commentRedoTokens.append(token); m_commentHistoryDocumentIndex[token] = m_document->undoHistoryIndex(); }
    updateUndoAction(); auto mapped = resultMap(result);
    mapped.insert(QStringLiteral("canUndo"), !m_commentUndoTokens.isEmpty());
    mapped.insert(QStringLiteral("canRedo"), !m_commentRedoTokens.isEmpty());
    return mapped;
}
QVariantMap Part::redoCommentDeletion(int token) {
    QScopedValueRollback<bool> historyStep(m_commentHistoryStep, true);
    const auto result = commentService().redo(token);
    if (result.success) { m_commentRedoTokens.removeAll(token); m_commentUndoTokens.append(token); m_commentHistoryDocumentIndex[token] = m_document->undoHistoryIndex(); }
    updateUndoAction(); auto mapped = resultMap(result);
    mapped.insert(QStringLiteral("canUndo"), !m_commentUndoTokens.isEmpty());
    mapped.insert(QStringLiteral("canRedo"), !m_commentRedoTokens.isEmpty());
    return mapped;
}
QVariantMap Part::deleteReviewAnnotations(const QVariantList &targets)
{
    QStringList ids;
    for (const auto &target : targets) {
        QString id = target.toMap().value(QStringLiteral("annotationId")).toString();
        if (id.startsWith(QLatin1String("id:"))) id.remove(0, 3);
        ids.append(Thoughts::logicalCommentId(id));
    }
    return deleteComments(ids);
}
}
