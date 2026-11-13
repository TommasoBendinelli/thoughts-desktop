// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "desktopsyncstore.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPair>
#include <QString>
#include <QStringList>

namespace Thoughts
{

enum class FrozenPublicationDisposition {
    Ready,
    NoChanges,
    RequiresLiveScan,
    Failed,
};

struct FrozenPublicationBundle {
    FrozenPublicationDisposition disposition = FrozenPublicationDisposition::Failed;
    QString directoryPath;
    QString manifestPath;
    QString sourceManifestPath;
    QString target;
    // Every PDF the bundle publishes, target first; one entry unless several
    // PDFs of a Project are published together.
    QStringList targets;
    QJsonArray acceptedEntries;
    qint64 frozenBytes = 0;
    QString error;

    bool ready() const;
};

class FrozenPublicationStager
{
public:
    static FrozenPublicationBundle stageScoped(const QString &docsRoot, const QString &stateRoot, const QString &target, const DesktopSyncSnapshot &snapshot, const QList<DesktopTrackedPath> &acceptedBaselines);
    static FrozenPublicationBundle
    stageScopedWithFreshInventory(const QString &docsRoot, const QString &stateRoot, const QString &target, const QJsonObject &targetInventory, const DesktopSyncSnapshot &snapshot, const QList<DesktopTrackedPath> &acceptedBaselines);
    // Several PDFs of one publication. PDFs no pending path touches are left
    // out; NoChanges when none remains.
    static FrozenPublicationBundle stageScopedWithFreshInventories(const QString &docsRoot,
                                                                   const QString &stateRoot,
                                                                   const QList<QPair<QString, QJsonObject>> &targetInventories,
                                                                   const DesktopSyncSnapshot &snapshot,
                                                                   const QList<DesktopTrackedPath> &acceptedBaselines);
    static FrozenPublicationBundle stageProjectTargetsWithFreshInventories(const QString &docsRoot,
                                                                           const QString &stateRoot,
                                                                           const QString &project,
                                                                           const QString &refreshedManifestPath,
                                                                           const QList<QPair<QString, QJsonObject>> &targetInventories,
                                                                           const DesktopSyncSnapshot &snapshot,
                                                                           const QList<DesktopTrackedPath> &acceptedBaselines);
    static FrozenPublicationBundle stageProjectTargetWithFreshInventory(const QString &docsRoot,
                                                                        const QString &stateRoot,
                                                                        const QString &project,
                                                                        const QString &target,
                                                                        const QString &refreshedManifestPath,
                                                                        const QJsonObject &targetInventory,
                                                                        const DesktopSyncSnapshot &snapshot,
                                                                        const QList<DesktopTrackedPath> &acceptedBaselines);
    static FrozenPublicationBundle
    stageGlobal(const QString &docsRoot, const QString &stateRoot, const QString &manifestPath, const QJsonArray &targetInventories, const DesktopSyncSnapshot &snapshot, const QList<DesktopTrackedPath> &acceptedBaselines);
    static bool discard(const FrozenPublicationBundle &bundle, QString *error = nullptr);
};

}

Q_DECLARE_METATYPE(Thoughts::FrozenPublicationDisposition)
