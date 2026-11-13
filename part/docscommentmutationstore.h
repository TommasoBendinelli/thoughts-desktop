#include <QMap>
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace Okular
{

struct DocsCommentMutationFlushResult {
    bool success = false;
    QString failureReason;
    QString docsRoot;
    QString pdfStem;
    QStringList appliedLines;
    QList<QJsonObject> changedRecords;
    QSet<QString> createdCommentIds;
    QByteArray visibleFingerprint;
};

/**
 * Crash-safe private journal used as the synchronous persistence boundary for
 * Thoughts comments.  Canonical sidecar rewrites happen later on a worker.
 */
class DocsCommentMutationStore
{
public:
    explicit DocsCommentMutationStore(QString stateRoot);

    bool queue(const QString &docsRoot, const QString &pdfStem, const QList<QJsonObject> &mutations, QString *error = nullptr) const;
    bool queueBatches(const QString &docsRoot, const QMap<QString, QList<QJsonObject>> &batches, QString *error = nullptr) const;
    DocsCommentMutationFlushResult flush(const QString &docsRoot, const QString &pdfStem, int lockTimeoutMs = 2000) const;
    bool acknowledge(const QString &docsRoot, const QString &pdfStem, const QStringList &appliedLines, QString *error = nullptr) const;
    bool hasPending(const QString &docsRoot, const QString &pdfStem) const;
    QString journalPath(const QString &docsRoot, const QString &pdfStem) const;

private:
    QString m_stateRoot;
};

}
