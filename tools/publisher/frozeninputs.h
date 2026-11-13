// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "buildinputs.h"

#include <QList>
#include <QSet>
#include <QString>

#include <optional>

// A frozen publication bundle (thoughts_server.source_agent.
// FrozenPublicationInputs): the content-addressed source and build-input
// bytes Thoughts captured when Sync started, so later edits cannot leak in.
namespace Thoughts::Publisher
{

struct FrozenEntry {
    QString path;
    QString action;
    std::optional<QString> sha256;
    std::optional<qint64> sizeBytes;
    std::optional<QString> mediaType;
    std::optional<QString> role;
    std::optional<QByteArray> content;
    bool reuseVerified = false;
    std::optional<QString> baseSha256;

    // public_identity(kind=..., target=...)
    Py::Json publicIdentity(const QString &kind, const std::optional<QString> &target = std::nullopt) const;
};

class FrozenInputs
{
public:
    // FrozenPublicationInputs.load: raises ValueError with Python's messages.
    static FrozenInputs load(const Py::Path &path, const Py::Path &sourceRoot);

    Py::Path sourceRoot;
    // In bundle order.
    QList<FrozenEntry> sources;
    QList<QPair<QString, QList<FrozenEntry>>> buildInputs;

    const FrozenEntry *source(const QString &path) const;
    QStringList sourcePaths() const;
    bool hasBuildTarget(const QString &target) const;
    QStringList buildTargets() const;

    void validateSources(const QSet<QString> &manifestPaths, const QSet<QString> &recordedPaths, const std::optional<QString> &scopeProject) const;
    // build_entries(target, project): nothing when the bundle lacks the target.
    std::optional<QList<BuildInput>> buildEntries(const QString &target, const QString &project) const;
    Py::Json acceptedEntries(const QSet<QString> &sourcePaths, const QSet<QString> &buildTargets) const;
};

}
