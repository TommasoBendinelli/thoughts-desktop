#include "projectconfiguration.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#include "frozenpublicationbundle.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace Thoughts
{

namespace
{

bool validSha256(const QString &value)
{
    static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
    return expression.match(value).hasMatch();
}

const QString &emptySourceBaseSha256()
{
    // Fedora represents an absent source with the SHA-256 identity of empty
    // content.  This value crosses the frozen-publication protocol boundary,
    // so an arbitrary all-zero sentinel would make every newly created source
    // look as though Fedora had changed after Desktop froze it.
    static const QString value = QString::fromLatin1(
        QCryptographicHash::hash(QByteArray(), QCryptographicHash::Sha256).toHex());
    return value;
}

bool safeRelativePath(const QString &value)
{
    const QString clean = QDir::cleanPath(value);
    return !clean.isEmpty() && clean == value && clean != QLatin1String(".") && !QDir::isAbsolutePath(clean) && clean != QLatin1String("..") && !clean.startsWith(QLatin1String("../")) && !clean.contains(QLatin1Char('\\')) &&
        !clean.contains(QChar::Null);
}

bool safeTarget(const QString &value)
{
    return !value.isEmpty() && QFileInfo(value).fileName() == value && value.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) && value != QLatin1String(".pdf") && value != QLatin1String("..pdf") && !value.contains(QLatin1Char('\\')) &&
        !value.contains(QChar::Null);
}

bool safeProject(const QString &value)
{
    return value.startsWith(QLatin1String("docs-")) && value.size() > 5 && QFileInfo(value).fileName() == value && !value.contains(QLatin1Char('\\')) && !value.contains(QChar::Null);
}

bool safeAdd(qint64 left, qint64 right, qint64 *result)
{
    if (!result || left < 0 || right < 0 || right > std::numeric_limits<qint64>::max() - left) {
        return false;
    }
    *result = left + right;
    return true;
}

bool snapshotContainsTarget(const DesktopPendingFile &file, const QString &target)
{
    return file.affectedTargets.contains(target);
}

QString sourceKind(const DesktopTrackedPath &path)
{
    return path.metadata.value(QStringLiteral("source_kind")).toString();
}

bool collectAcceptedSourceBases(const QList<DesktopTrackedPath> &acceptedBaselines, QHash<QString, QString> *basesByPath, QString *error)
{
    if (!basesByPath) {
        return false;
    }
    basesByPath->clear();
    for (const DesktopTrackedPath &baseline : acceptedBaselines) {
        if (baseline.metadata.value(QStringLiteral("kind")).toString() != QLatin1String("file") || sourceKind(baseline) != QLatin1String("source")) {
            continue;
        }
        if (!safeRelativePath(baseline.path) || basesByPath->contains(baseline.path) || (baseline.exists && !validSha256(baseline.sha256))) {
            if (error) {
                *error = QStringLiteral("The accepted source base identities are incomplete or malformed.");
            }
            basesByPath->clear();
            return false;
        }
        basesByPath->insert(baseline.path, baseline.exists ? baseline.sha256 : emptySourceBaseSha256());
    }
    return true;
}

QString sourceBaseSha256(const QString &path, const QHash<QString, QString> &basesByPath)
{
    return basesByPath.value(path, emptySourceBaseSha256());
}

QJsonObject buildEntry(const DesktopTrackedPath &path, bool reuseVerified)
{
    QJsonObject entry {{QStringLiteral("path"), path.path},
                       {QStringLiteral("action"), QStringLiteral("upsert")},
                       {QStringLiteral("sha256"), path.sha256},
                       {QStringLiteral("size_bytes"), path.sizeBytes},
                       {QStringLiteral("media_type"), path.metadata.value(QStringLiteral("media_type"))},
                       {QStringLiteral("role"), path.metadata.value(QStringLiteral("role"))}};
    if (reuseVerified) {
        entry.insert(QStringLiteral("reuse_verified"), true);
    }
    return entry;
}

QJsonObject publicEntry(const QJsonObject &entry, const QString &kind, const QString &target = {})
{
    QJsonObject value = entry;
    value.insert(QStringLiteral("kind"), kind);
    if (!target.isEmpty()) {
        value.insert(QStringLiteral("target"), target);
    }
    return value;
}

void sortEntries(QJsonArray *values, bool publicOrder)
{
    if (!values) {
        return;
    }
    QList<QJsonObject> entries;
    entries.reserve(values->size());
    for (const QJsonValue &value : std::as_const(*values)) {
        entries.append(value.toObject());
    }
    std::sort(entries.begin(), entries.end(), [publicOrder](const QJsonObject &left, const QJsonObject &right) {
        if (publicOrder) {
            const QString leftKind = left.value(QStringLiteral("kind")).toString();
            const QString rightKind = right.value(QStringLiteral("kind")).toString();
            if (leftKind != rightKind) {
                return leftKind < rightKind;
            }
            const QString leftTarget = left.value(QStringLiteral("target")).toString().toCaseFolded();
            const QString rightTarget = right.value(QStringLiteral("target")).toString().toCaseFolded();
            if (leftTarget != rightTarget) {
                return leftTarget < rightTarget;
            }
        }
        return left.value(QStringLiteral("path")).toString() < right.value(QStringLiteral("path")).toString();
    });
    *values = {};
    for (const QJsonObject &entry : std::as_const(entries)) {
        values->append(entry);
    }
}

bool writeFrozenObject(const QString &sourcePath, const QString &destinationPath, const QString &expectedSha256, qint64 expectedSize, qint64 *frozenBytes, QString *error)
{
    const QFileInfo existing(destinationPath);
    if (existing.isFile()) {
        return existing.size() == expectedSize;
    }
    QFile source(sourcePath);
    QSaveFile destination(destinationPath);
    destination.setDirectWriteFallback(false);
    if (!source.open(QIODevice::ReadOnly) || !destination.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = QStringLiteral("The changed file could not be frozen: %1").arg(sourcePath);
        }
        return false;
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    qint64 copied = 0;
    while (!source.atEnd()) {
        const QByteArray chunk = source.read(1024 * 1024);
        if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
            destination.cancelWriting();
            if (error) {
                *error = QStringLiteral("The changed file could not be read while it was being frozen: %1").arg(sourcePath);
            }
            return false;
        }
        if (!chunk.isEmpty()) {
            if (destination.write(chunk) != chunk.size()) {
                destination.cancelWriting();
                if (error) {
                    *error = QStringLiteral("The immutable publication object could not be written: %1").arg(sourcePath);
                }
                return false;
            }
            digest.addData(chunk);
        }
        copied += chunk.size();
    }
    if (copied != expectedSize || QString::fromLatin1(digest.result().toHex()) != expectedSha256) {
        destination.cancelWriting();
        if (error) {
            *error = QStringLiteral("The changed file was edited while synchronization was freezing it: %1").arg(sourcePath);
        }
        return false;
    }
    if (!destination.commit()) {
        if (error) {
            *error = QStringLiteral("The immutable publication object could not be committed: %1").arg(sourcePath);
        }
        return false;
    }
    QFile::setPermissions(destinationPath, QFileDevice::ReadOwner);
    return safeAdd(*frozenBytes, copied, frozenBytes);
}

bool validBuildIdentity(const DesktopTrackedPath &path)
{
    const QString role = path.metadata.value(QStringLiteral("role")).toString();
    static const QSet<QString> roles {QStringLiteral("asset"), QStringLiteral("generated"), QStringLiteral("external_pdf")};
    const QString mediaType = path.metadata.value(QStringLiteral("media_type")).toString();
    return safeRelativePath(path.path) && path.exists && validSha256(path.sha256) && path.sizeBytes >= 0 && roles.contains(role) && !mediaType.isEmpty() && !mediaType.contains(QLatin1Char('\r')) && !mediaType.contains(QLatin1Char('\n'));
}

bool parseFreshTargetInventory(const QString &target, const QJsonObject &inventory, QHash<QString, QJsonObject> *entries, QString *error)
{
    if (!entries) {
        return false;
    }
    entries->clear();
    const QString project = inventory.value(QStringLiteral("project")).toString();
    const QJsonArray values = inventory.value(QStringLiteral("entries")).toArray();
    if (inventory.value(QStringLiteral("schema_version")).toInt() < 3 || inventory.value(QStringLiteral("target")).toString() != target || project.isEmpty() || !inventory.value(QStringLiteral("entries")).isArray()) {
        if (error) {
            *error = QStringLiteral("The fresh target build-input inventory is malformed.");
        }
        return false;
    }
    static const QSet<QString> roles {QStringLiteral("asset"), QStringLiteral("generated"), QStringLiteral("external_pdf")};
    for (const QJsonValue &value : values) {
        const QJsonObject inventoryEntry = value.toObject();
        const QString path = inventoryEntry.value(QStringLiteral("path")).toString();
        const QString sha256 = inventoryEntry.value(QStringLiteral("sha256")).toString();
        const qint64 sizeBytes = inventoryEntry.value(QStringLiteral("size_bytes")).toInteger(-1);
        const QString mediaType = inventoryEntry.value(QStringLiteral("media_type")).toString();
        const QString role = inventoryEntry.value(QStringLiteral("role")).toString();
        if (!safeRelativePath(path) || entries->contains(path) || !validSha256(sha256) || sizeBytes < 0 || !roles.contains(role) || mediaType.isEmpty() || mediaType.contains(QLatin1Char('\r')) || mediaType.contains(QLatin1Char('\n'))) {
            if (error) {
                *error = QStringLiteral("The fresh target build-input inventory contains an invalid identity.");
            }
            entries->clear();
            return false;
        }
        entries->insert(path,
                        QJsonObject {{QStringLiteral("path"), path},
                                     {QStringLiteral("action"), QStringLiteral("upsert")},
                                     {QStringLiteral("sha256"), sha256},
                                     {QStringLiteral("size_bytes"), sizeBytes},
                                     {QStringLiteral("media_type"), mediaType},
                                     {QStringLiteral("role"), role}});
    }
    return true;
}

bool acceptedBuildIdentityMatches(const DesktopTrackedPath &baseline, const QString &target, const QJsonObject &inventoryEntry)
{
    return validBuildIdentity(baseline) && baseline.affectedTargets.contains(target) && baseline.path == inventoryEntry.value(QStringLiteral("path")).toString() &&
        baseline.sha256 == inventoryEntry.value(QStringLiteral("sha256")).toString() && baseline.sizeBytes == inventoryEntry.value(QStringLiteral("size_bytes")).toInteger(-1) &&
        baseline.metadata.value(QStringLiteral("role")) == inventoryEntry.value(QStringLiteral("role")) && baseline.metadata.value(QStringLiteral("media_type")) == inventoryEntry.value(QStringLiteral("media_type"));
}

}

bool FrozenPublicationBundle::ready() const
{
    return disposition == FrozenPublicationDisposition::Ready && !directoryPath.isEmpty() && !manifestPath.isEmpty();
}

FrozenPublicationBundle FrozenPublicationStager::stageScoped(const QString &docsRoot, const QString &stateRoot, const QString &target, const DesktopSyncSnapshot &snapshot, const QList<DesktopTrackedPath> &acceptedBaselines)
{
    FrozenPublicationBundle result;
    result.target = target;
    if (!safeTarget(target) || snapshot.isEmpty()) {
        result.error = QStringLiteral("The frozen publication request is empty or invalid.");
        return result;
    }

    QHash<QString, QString> sourceBasesByPath;
    if (!collectAcceptedSourceBases(acceptedBaselines, &sourceBasesByPath, &result.error)) {
        return result;
    }

    QHash<QString, DesktopTrackedPath> buildInputs;
    for (const DesktopTrackedPath &baseline : acceptedBaselines) {
        if (baseline.metadata.value(QStringLiteral("kind")).toString() == QLatin1String("file") && sourceKind(baseline) == QLatin1String("build_input") && baseline.affectedTargets.contains(target)) {
            if (!validBuildIdentity(baseline) || buildInputs.contains(baseline.path)) {
                result.error = QStringLiteral("The accepted build-input closure is incomplete or malformed.");
                return result;
            }
            buildInputs.insert(baseline.path, baseline);
        }
    }

    QList<DesktopPendingFile> pendingFiles;
    for (const DesktopPendingFile &file : snapshot.files) {
        if (file.affectedTargets.isEmpty()) {
            result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
            result.error = QStringLiteral("A changed path has not yet been resolved to a PDF target.");
            return result;
        }
        if (!snapshotContainsTarget(file, target)) {
            continue;
        }
        if (!file.hasTrackedState || !safeRelativePath(file.path) || file.trackedState.metadata.value(QStringLiteral("kind")).toString() != QLatin1String("file")) {
            result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
            result.error = QStringLiteral("The affected dependency closure must be rescanned before publication.");
            return result;
        }
        const QString kind = sourceKind(file.trackedState);
        if (kind != QLatin1String("source") && kind != QLatin1String("build_input")) {
            result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
            result.error = QStringLiteral("A changed path has an unresolved publication role.");
            return result;
        }
        pendingFiles.append(file);
        if (kind == QLatin1String("build_input")) {
            if (file.action == QLatin1String("delete")) {
                buildInputs.remove(file.path);
            } else if (!validBuildIdentity(file.trackedState)) {
                result.error = QStringLiteral("A changed build input has no complete immutable identity.");
                return result;
            } else {
                buildInputs.insert(file.path, file.trackedState);
            }
        }
    }
    if (pendingFiles.isEmpty()) {
        result.disposition = FrozenPublicationDisposition::NoChanges;
        return result;
    }

    const QString rootPath = QFileInfo(docsRoot).canonicalFilePath();
    const QString statePath = QFileInfo(stateRoot).absoluteFilePath();
    if (rootPath.isEmpty() || statePath.isEmpty()) {
        result.error = QStringLiteral("The publication roots are unavailable.");
        return result;
    }
    const QString bundlesRoot = QDir(statePath).filePath(QStringLiteral("frozen-publications"));
    const QString bundleId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    result.directoryPath = QDir(bundlesRoot).filePath(bundleId);
    const QString objectsPath = QDir(result.directoryPath).filePath(QStringLiteral("objects"));
    if (!QDir().mkpath(objectsPath)) {
        result.error = QStringLiteral("The immutable publication directory could not be created.");
        result.directoryPath.clear();
        return result;
    }

    QJsonArray sources;
    QJsonArray accepted;
    QHash<QString, QJsonObject> buildEntries;
    for (auto iterator = buildInputs.cbegin(); iterator != buildInputs.cend(); ++iterator) {
        const QJsonObject entry = buildEntry(iterator.value(), true);
        buildEntries.insert(iterator.key(), entry);
    }
    for (const DesktopPendingFile &file : std::as_const(pendingFiles)) {
        const DesktopTrackedPath &tracked = file.trackedState;
        const QString kind = sourceKind(tracked);
        QJsonObject entry {{QStringLiteral("path"), file.path}, {QStringLiteral("action"), file.action}};
        if (kind == QLatin1String("build_input")) {
            entry.insert(QStringLiteral("role"), tracked.metadata.value(QStringLiteral("role")));
        }
        if (file.action == QLatin1String("upsert")) {
            if (!tracked.exists || !validSha256(tracked.sha256) || tracked.sizeBytes < 0) {
                result.error = QStringLiteral("A changed file has no complete immutable identity.");
                discard(result, nullptr);
                return result;
            }
            if (!writeFrozenObject(QDir(rootPath).filePath(file.path), QDir(objectsPath).filePath(tracked.sha256), tracked.sha256, tracked.sizeBytes, &result.frozenBytes, &result.error)) {
                discard(result, nullptr);
                return result;
            }
            entry.insert(QStringLiteral("sha256"), tracked.sha256);
            entry.insert(QStringLiteral("size_bytes"), tracked.sizeBytes);
            if (kind == QLatin1String("build_input")) {
                entry.insert(QStringLiteral("media_type"), tracked.metadata.value(QStringLiteral("media_type")));
            }
        }
        if (kind == QLatin1String("source")) {
            entry.insert(QStringLiteral("base_sha256"), sourceBaseSha256(file.path, sourceBasesByPath));
            sources.append(entry);
            accepted.append(publicEntry(entry, QStringLiteral("source")));
        } else {
            buildEntries.insert(file.path, entry);
        }
    }

    QJsonArray buildArray;
    for (auto iterator = buildEntries.cbegin(); iterator != buildEntries.cend(); ++iterator) {
        buildArray.append(iterator.value());
        accepted.append(publicEntry(iterator.value(), QStringLiteral("build_input"), target));
    }
    sortEntries(&sources, false);
    sortEntries(&buildArray, false);
    sortEntries(&accepted, true);

    const QJsonObject manifest {{QStringLiteral("schema_version"), 2},
                                {QStringLiteral("source_root"), rootPath},
                                {QStringLiteral("objects_dir"), QStringLiteral("objects")},
                                {QStringLiteral("sources"), sources},
                                {QStringLiteral("build_inputs"), QJsonObject {{target, buildArray}}}};
    result.manifestPath = QDir(result.directoryPath).filePath(QStringLiteral("inputs.json"));
    QSaveFile output(result.manifestPath);
    output.setDirectWriteFallback(false);
    const QByteArray encoded = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(encoded) != encoded.size() || !output.commit()) {
        result.error = QStringLiteral("The immutable publication manifest could not be committed.");
        // A short write leaves the temporary file open, which would keep
        // Windows from removing the bundle folder.
        output.cancelWriting();
        discard(result, nullptr);
        return result;
    }
    QFile::setPermissions(result.manifestPath, QFileDevice::ReadOwner);
    result.acceptedEntries = accepted;
    result.disposition = FrozenPublicationDisposition::Ready;
    result.error.clear();
    return result;
}

FrozenPublicationBundle FrozenPublicationStager::stageScopedWithFreshInventory(const QString &docsRoot,
                                                                               const QString &stateRoot,
                                                                               const QString &target,
                                                                               const QJsonObject &targetInventory,
                                                                               const DesktopSyncSnapshot &snapshot,
                                                                               const QList<DesktopTrackedPath> &acceptedBaselines)
{
    return stageScopedWithFreshInventories(docsRoot, stateRoot, {qMakePair(target, targetInventory)}, snapshot, acceptedBaselines);
}

FrozenPublicationBundle FrozenPublicationStager::stageScopedWithFreshInventories(const QString &docsRoot,
                                                                                 const QString &stateRoot,
                                                                                 const QList<QPair<QString, QJsonObject>> &targetInventories,
                                                                                 const DesktopSyncSnapshot &snapshot,
                                                                                 const QList<DesktopTrackedPath> &acceptedBaselines)
{
    FrozenPublicationBundle result;
    result.target = targetInventories.isEmpty() ? QString() : targetInventories.constFirst().first;
    QSet<QString> requestedTargets;
    for (const auto &[target, inventory] : targetInventories) {
        if (!safeTarget(target) || requestedTargets.contains(target)) {
            result.error = QStringLiteral("The inventory-backed frozen publication request is empty or invalid.");
            return result;
        }
        requestedTargets.insert(target);
    }
    if (targetInventories.isEmpty() || snapshot.isEmpty()) {
        result.error = QStringLiteral("The inventory-backed frozen publication request is empty or invalid.");
        return result;
    }

    QHash<QString, QString> sourceBasesByPath;
    if (!collectAcceptedSourceBases(acceptedBaselines, &sourceBasesByPath, &result.error)) {
        return result;
    }

    // One publication may cover several PDFs of a Project. Each PDF keeps its
    // own fresh closure and accepted identities; a pending source is frozen
    // once for all of them.
    struct StagedTarget {
        QString target;
        QHash<QString, QJsonObject> inventoryEntries;
        QHash<QString, DesktopTrackedPath> acceptedByPath;
    };
    QList<StagedTarget> staged;
    QList<DesktopPendingFile> pendingSources;
    QSet<QString> pendingSourcePaths;
    for (const auto &[target, targetInventory] : targetInventories) {
        StagedTarget current;
        current.target = target;
        if (!parseFreshTargetInventory(target, targetInventory, &current.inventoryEntries, &result.error)) {
            return result;
        }
        for (const DesktopTrackedPath &baseline : acceptedBaselines) {
            if (baseline.metadata.value(QStringLiteral("kind")).toString() != QLatin1String("file") || sourceKind(baseline) != QLatin1String("build_input") || !baseline.affectedTargets.contains(target)) {
                continue;
            }
            if (current.acceptedByPath.contains(baseline.path)) {
                result.error = QStringLiteral("The accepted target build-input identities are duplicated.");
                return result;
            }
            current.acceptedByPath.insert(baseline.path, baseline);
        }

        QSet<QString> relevantPaths;
        bool hasRelevantChange = false;
        for (const DesktopPendingFile &file : snapshot.files) {
            const bool inFreshClosure = current.inventoryEntries.contains(file.path);
            const bool hasTargetHint = snapshotContainsTarget(file, target);
            if (!inFreshClosure && !hasTargetHint) {
                continue;
            }
            hasRelevantChange = true;
            if (!safeRelativePath(file.path) || relevantPaths.contains(file.path)) {
                result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
                result.error = QStringLiteral("The pending target generation contains an invalid or duplicated path.");
                return result;
            }
            relevantPaths.insert(file.path);

            if (inFreshClosure) {
                const QJsonObject inventoryEntry = current.inventoryEntries.value(file.path);
                if (file.action != QLatin1String("upsert") || !file.hasTrackedState || !file.trackedState.exists || !validSha256(file.trackedState.sha256) || file.trackedState.sizeBytes < 0 ||
                    file.trackedState.sha256 != inventoryEntry.value(QStringLiteral("sha256")).toString() || file.trackedState.sizeBytes != inventoryEntry.value(QStringLiteral("size_bytes")).toInteger(-1)) {
                    result.error = QStringLiteral("A fresh build-input identity does not match the frozen pending generation.");
                    return result;
                }
                continue;
            }

            if (!file.hasTrackedState || file.trackedState.metadata.value(QStringLiteral("kind")).toString() != QLatin1String("file")) {
                result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
                result.error = QStringLiteral("The affected source identity must be resolved before publication.");
                return result;
            }
            const QString kind = sourceKind(file.trackedState);
            if (kind == QLatin1String("build_input") || kind == QLatin1String("ignored")) {
                // The fresh inventory is authoritative for this target. A former
                // dependency absent from it must not be uploaded or retained in
                // the replacement closure.
                continue;
            }
            if (kind != QLatin1String("source")) {
                result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
                result.error = QStringLiteral("A changed target path has an unresolved source role.");
                return result;
            }
            if (!pendingSourcePaths.contains(file.path)) {
                pendingSourcePaths.insert(file.path);
                pendingSources.append(file);
            }
        }
        // A PDF no pending path touches has nothing to publish.
        if (hasRelevantChange) {
            staged.append(current);
        }
    }
    if (staged.isEmpty()) {
        result.disposition = FrozenPublicationDisposition::NoChanges;
        return result;
    }
    result.target = staged.constFirst().target;
    for (const StagedTarget &current : std::as_const(staged)) {
        result.targets.append(current.target);
    }

    const QString rootPath = QFileInfo(docsRoot).canonicalFilePath();
    const QString statePath = QFileInfo(stateRoot).absoluteFilePath();
    if (rootPath.isEmpty() || statePath.isEmpty()) {
        result.error = QStringLiteral("The publication roots are unavailable.");
        return result;
    }
    const QString bundlesRoot = QDir(statePath).filePath(QStringLiteral("frozen-publications"));
    result.directoryPath = QDir(bundlesRoot).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString objectsPath = QDir(result.directoryPath).filePath(QStringLiteral("objects"));
    if (!QDir().mkpath(objectsPath)) {
        result.error = QStringLiteral("The immutable publication directory could not be created.");
        result.directoryPath.clear();
        return result;
    }

    QJsonArray sources;
    QJsonArray accepted;
    for (const DesktopPendingFile &file : std::as_const(pendingSources)) {
        const DesktopTrackedPath &tracked = file.trackedState;
        QJsonObject entry {{QStringLiteral("path"), file.path}, {QStringLiteral("action"), file.action}};
        if (file.action == QLatin1String("upsert")) {
            if (!tracked.exists || !validSha256(tracked.sha256) || tracked.sizeBytes < 0) {
                result.error = QStringLiteral("A changed source has no complete immutable identity.");
                discard(result, nullptr);
                return result;
            }
            if (!writeFrozenObject(QDir(rootPath).filePath(file.path), QDir(objectsPath).filePath(tracked.sha256), tracked.sha256, tracked.sizeBytes, &result.frozenBytes, &result.error)) {
                discard(result, nullptr);
                return result;
            }
            entry.insert(QStringLiteral("sha256"), tracked.sha256);
            entry.insert(QStringLiteral("size_bytes"), tracked.sizeBytes);
        } else if (file.action != QLatin1String("delete") || tracked.exists) {
            result.error = QStringLiteral("A changed source has an invalid frozen action.");
            discard(result, nullptr);
            return result;
        }
        entry.insert(QStringLiteral("base_sha256"), sourceBaseSha256(file.path, sourceBasesByPath));
        sources.append(entry);
        accepted.append(publicEntry(entry, QStringLiteral("source")));
    }

    QJsonObject buildInputsByTarget;
    for (const StagedTarget &current : std::as_const(staged)) {
        QJsonArray buildInputs;
        QStringList inventoryPaths = current.inventoryEntries.keys();
        std::sort(inventoryPaths.begin(), inventoryPaths.end());
        for (const QString &path : std::as_const(inventoryPaths)) {
            QJsonObject entry = current.inventoryEntries.value(path);
            const DesktopTrackedPath acceptedBaseline = current.acceptedByPath.value(path);
            if (acceptedBuildIdentityMatches(acceptedBaseline, current.target, entry)) {
                entry.insert(QStringLiteral("reuse_verified"), true);
            } else if (!writeFrozenObject(QDir(rootPath).filePath(path),
                                          QDir(objectsPath).filePath(entry.value(QStringLiteral("sha256")).toString()),
                                          entry.value(QStringLiteral("sha256")).toString(),
                                          entry.value(QStringLiteral("size_bytes")).toInteger(-1),
                                          &result.frozenBytes,
                                          &result.error)) {
                discard(result, nullptr);
                return result;
            }
            buildInputs.append(entry);
            accepted.append(publicEntry(entry, QStringLiteral("build_input"), current.target));
        }
        buildInputsByTarget.insert(current.target, buildInputs);
    }
    sortEntries(&sources, false);
    sortEntries(&accepted, true);

    const QJsonObject manifest {{QStringLiteral("schema_version"), 2},
                                {QStringLiteral("source_root"), rootPath},
                                {QStringLiteral("objects_dir"), QStringLiteral("objects")},
                                {QStringLiteral("sources"), sources},
                                {QStringLiteral("build_inputs"), buildInputsByTarget}};
    result.manifestPath = QDir(result.directoryPath).filePath(QStringLiteral("inputs.json"));
    QSaveFile output(result.manifestPath);
    output.setDirectWriteFallback(false);
    const QByteArray encoded = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(encoded) != encoded.size() || !output.commit()) {
        result.error = QStringLiteral("The immutable publication manifest could not be committed.");
        // A short write leaves the temporary file open, which would keep
        // Windows from removing the bundle folder.
        output.cancelWriting();
        discard(result, nullptr);
        return result;
    }
    QFile::setPermissions(result.manifestPath, QFileDevice::ReadOwner);
    result.acceptedEntries = accepted;
    result.disposition = FrozenPublicationDisposition::Ready;
    result.error.clear();
    return result;
}

FrozenPublicationBundle FrozenPublicationStager::stageProjectTargetWithFreshInventory(const QString &docsRoot,
                                                                                      const QString &stateRoot,
                                                                                      const QString &project,
                                                                                      const QString &target,
                                                                                      const QString &refreshedManifestPath,
                                                                                      const QJsonObject &targetInventory,
                                                                                      const DesktopSyncSnapshot &snapshot,
                                                                                      const QList<DesktopTrackedPath> &acceptedBaselines)
{
    return stageProjectTargetsWithFreshInventories(docsRoot, stateRoot, project, refreshedManifestPath, {qMakePair(target, targetInventory)}, snapshot, acceptedBaselines);
}

FrozenPublicationBundle FrozenPublicationStager::stageProjectTargetsWithFreshInventories(const QString &docsRoot,
                                                                                        const QString &stateRoot,
                                                                                        const QString &project,
                                                                                        const QString &refreshedManifestPath,
                                                                                        const QList<QPair<QString, QJsonObject>> &targetInventories,
                                                                                        const DesktopSyncSnapshot &snapshot,
                                                                                        const QList<DesktopTrackedPath> &acceptedBaselines)
{
    FrozenPublicationBundle result;
    result.target = targetInventories.isEmpty() ? QString() : targetInventories.constFirst().first;
    QStringList requestedTargets;
    for (const auto &[target, inventory] : targetInventories) {
        if (!safeTarget(target) || requestedTargets.contains(target)) {
            result.error = QStringLiteral("The project-scoped source manifest is unavailable or invalid.");
            return result;
        }
        requestedTargets.append(target);
    }
    QFile manifestFile(refreshedManifestPath);
    if (!safeProject(project) || requestedTargets.isEmpty() || !manifestFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("The project-scoped source manifest is unavailable or invalid.");
        return result;
    }
    const QByteArray manifestBytes = manifestFile.readAll();
    const QJsonDocument document = QJsonDocument::fromJson(manifestBytes);
    const QJsonArray projects = document.object().value(QStringLiteral("projects")).toArray();
    if (!document.isObject() || document.object().value(QStringLiteral("schema_version")).toInt() != 3 || !document.object().value(QStringLiteral("projects")).isArray()) {
        result.error = QStringLiteral("The project-scoped source manifest is malformed.");
        return result;
    }

    QHash<QString, QString> selectedSources;
    QSet<QString> allPaths;
    QSet<QString> allTargets;
    int selectedProjects = 0;
    for (const QJsonValue &projectValue : projects) {
        const QJsonObject projectObject = projectValue.toObject();
        const QString projectName = projectObject.value(QStringLiteral("project")).toString();
        const QJsonArray targets = projectObject.value(QStringLiteral("targets")).toArray();
        const QJsonArray files = projectObject.value(QStringLiteral("files")).toArray();
        // Connected source folders may have no selected PDF target. Ownership
        // of the requested target is checked separately for the selected project.
        // A Project's default folder is listed too; only docs-* folders are
        // selected here, as default folders publish globally.
        if (!(safeProject(projectName) || SourceOwner::isDefaultFolder(projectName)) || !projectObject.value(QStringLiteral("targets")).isArray() || !projectObject.value(QStringLiteral("files")).isArray()) {
            result.error = QStringLiteral("The project-scoped source manifest contains an invalid project.");
            return result;
        }
        QSet<QString> projectTargets;
        for (const QJsonValue &targetValue : targets) {
            const QString candidate = targetValue.toString();
            if (!safeTarget(candidate) || allTargets.contains(candidate)) {
                result.error = QStringLiteral("The project-scoped source manifest contains an invalid target.");
                return result;
            }
            allTargets.insert(candidate);
            projectTargets.insert(candidate);
        }
        if (projectName == project) {
            ++selectedProjects;
            if (!std::all_of(requestedTargets.cbegin(), requestedTargets.cend(), [&projectTargets](const QString &target) { return projectTargets.contains(target); })) {
                result.error = QStringLiteral("The project-scoped source target is not owned by the selected project.");
                return result;
            }
        }
        for (const QJsonValue &fileValue : files) {
            const QJsonObject file = fileValue.toObject();
            const QString path = file.value(QStringLiteral("path")).toString();
            const QString role = file.value(QStringLiteral("role")).toString();
            if (!safeRelativePath(path) || allPaths.contains(path) || role.isEmpty() || role.contains(QLatin1Char('\r')) || role.contains(QLatin1Char('\n'))) {
                result.error = QStringLiteral("The project-scoped source manifest contains an invalid path.");
                return result;
            }
            allPaths.insert(path);
            if (projectName == project) {
                selectedSources.insert(path, role);
            }
        }
    }
    for (const auto &raw : document.object().value(QStringLiteral("shared_files")).toArray()) {
        const auto entry = raw.toObject(); const auto path = entry.value(QStringLiteral("path")).toString();
        if (!QRegularExpression(QStringLiteral("^\\.thoughts/projects/[a-f0-9]{64}/(default|documents)/thoughts-project\\.json$")).match(path).hasMatch()) {
            result.error = QStringLiteral("Invalid Project configuration path."); return result;
        }
        const auto configuration = ProjectConfiguration::read(QDir(docsRoot).filePath(path), &result.error);
        if (!result.error.isEmpty()) return result;
        for (const auto &folder : configuration.value(QStringLiteral("source_folders")).toArray())
            if (folder.toObject().value(QStringLiteral("path")).toString() == project)
                selectedSources.insert(path, QStringLiteral("render_manifest"));
    }
    if (selectedProjects != 1) {
        result.error = QStringLiteral("The project-scoped source manifest has no unique selected project.");
        return result;
    }

    DesktopSyncSnapshot resolvedSnapshot = snapshot;
    for (DesktopPendingFile &file : resolvedSnapshot.files) {
        if (!selectedSources.contains(file.path) || !file.hasTrackedState) {
            continue;
        }
        QJsonObject metadata = file.trackedState.metadata;
        const QString currentProject = metadata.value(QStringLiteral("project")).toString();
        if (!currentProject.isEmpty() && currentProject != project) {
            result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
            result.error = QStringLiteral("A changed source is assigned to a different project.");
            return result;
        }
        file.affectedTargets.removeIf([&](const QString &previous) { return !allTargets.contains(previous); });
        for (const QString &target : std::as_const(requestedTargets)) {
            if (!file.affectedTargets.contains(target)) file.affectedTargets.append(target);
        }
        file.trackedState.affectedTargets = file.affectedTargets;
        metadata.remove(QStringLiteral("_tracking_resolution"));
        metadata.remove(QStringLiteral("_affected_target_hints"));
        metadata.insert(QStringLiteral("kind"), QStringLiteral("file"));
        metadata.insert(QStringLiteral("source_kind"), QStringLiteral("source"));
        metadata.insert(QStringLiteral("project"), project);
        metadata.insert(QStringLiteral("role"), selectedSources.value(file.path));
        file.metadata = metadata;
        file.trackedState.metadata = metadata;
    }

    // Discovery can add sources which the watcher has never observed. A
    // scoped bundle only freezes pending paths, so accepting the refreshed
    // manifest would otherwise require source bases that were never published.
    // Let the coordinator freeze the complete inventory before any upload.
    QSet<QString> coveredSources;
    for (const DesktopTrackedPath &baseline : acceptedBaselines) {
        if (baseline.exists && sourceKind(baseline) == QLatin1String("source")
            && validSha256(baseline.sha256)) {
            coveredSources.insert(baseline.path);
        }
    }
    for (const DesktopPendingFile &file : resolvedSnapshot.files) {
        const bool requested = std::any_of(requestedTargets.cbegin(), requestedTargets.cend(), [&file](const QString &target) { return file.affectedTargets.contains(target); });
        if (requested && file.hasTrackedState
            && file.action == QLatin1String("upsert") && file.trackedState.exists
            && sourceKind(file.trackedState) == QLatin1String("source")) {
            coveredSources.insert(file.path);
        }
    }
    for (auto source = selectedSources.cbegin(); source != selectedSources.cend(); ++source) {
        if (!coveredSources.contains(source.key())) {
            result.disposition = FrozenPublicationDisposition::RequiresLiveScan;
            result.error = QStringLiteral("A discovered source has no accepted or frozen identity: %1.").arg(source.key());
            return result;
        }
    }

    result = stageScopedWithFreshInventories(docsRoot, stateRoot, targetInventories, resolvedSnapshot, acceptedBaselines);
    if (!result.ready()) {
        return result;
    }
    result.sourceManifestPath = QDir(result.directoryPath).filePath(QStringLiteral("source-manifest.json"));
    QSaveFile output(result.sourceManifestPath);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(manifestBytes) != manifestBytes.size() || !output.commit()) {
        result.error = QStringLiteral("The refreshed project source manifest could not be frozen.");
        // A short write leaves the temporary file open, which would keep
        // Windows from removing the bundle folder.
        output.cancelWriting();
        discard(result, nullptr);
        result.directoryPath.clear();
        result.manifestPath.clear();
        result.sourceManifestPath.clear();
        result.disposition = FrozenPublicationDisposition::Failed;
        return result;
    }
    QFile::setPermissions(result.sourceManifestPath, QFileDevice::ReadOwner);
    return result;
}

FrozenPublicationBundle FrozenPublicationStager::stageGlobal(const QString &docsRoot,
                                                             const QString &stateRoot,
                                                             const QString &manifestPath,
                                                             const QJsonArray &targetInventories,
                                                             const DesktopSyncSnapshot &snapshot,
                                                             const QList<DesktopTrackedPath> &acceptedBaselines)
{
    FrozenPublicationBundle result;
    const QString rootPath = QFileInfo(docsRoot).canonicalFilePath();
    const QString statePath = QFileInfo(stateRoot).absoluteFilePath();
    QFile manifestFile(manifestPath);
    if (rootPath.isEmpty() || statePath.isEmpty() || !manifestFile.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("The global source inventory is unavailable.");
        return result;
    }
    const QByteArray sourceManifestBytes = manifestFile.readAll();
    const QJsonDocument manifestDocument = QJsonDocument::fromJson(sourceManifestBytes);
    const QJsonObject manifestObject = manifestDocument.object();
    const int manifestSchema = manifestObject.value(QStringLiteral("schema_version")).toInt();
    const QJsonArray projects = manifestDocument.object().value(QStringLiteral("projects")).toArray();
    // An empty project list is valid until this device installs a Project's folder.
    if (!manifestDocument.isObject() || manifestSchema != 3 || !manifestObject.value(QStringLiteral("projects")).isArray() ||
        (manifestObject.contains(QStringLiteral("shared_files")) && !manifestObject.value(QStringLiteral("shared_files")).isArray())) {
        result.error = QStringLiteral("The discovered global source manifest is malformed.");
        return result;
    }

    QHash<QString, QString> sourceBasesByPath;
    if (!collectAcceptedSourceBases(acceptedBaselines, &sourceBasesByPath, &result.error)) {
        return result;
    }

    const QString bundlesRoot = QDir(statePath).filePath(QStringLiteral("frozen-publications"));
    result.directoryPath = QDir(bundlesRoot).filePath(QUuid::createUuid().toString(QUuid::WithoutBraces));
    const QString objectsPath = QDir(result.directoryPath).filePath(QStringLiteral("objects"));
    if (!QDir().mkpath(objectsPath)) {
        result.error = QStringLiteral("The immutable global publication directory could not be created.");
        result.directoryPath.clear();
        return result;
    }
    result.sourceManifestPath = QDir(result.directoryPath).filePath(QStringLiteral("source-manifest.json"));
    QSaveFile sourceManifestOutput(result.sourceManifestPath);
    sourceManifestOutput.setDirectWriteFallback(false);
    if (!sourceManifestOutput.open(QIODevice::WriteOnly) || sourceManifestOutput.write(sourceManifestBytes) != sourceManifestBytes.size() || !sourceManifestOutput.commit()) {
        result.error = QStringLiteral("The discovered source manifest could not be frozen.");
        // A short write leaves the temporary file open, which would keep
        // Windows from removing the bundle folder.
        sourceManifestOutput.cancelWriting();
        discard(result, nullptr);
        return result;
    }
    QFile::setPermissions(result.sourceManifestPath, QFileDevice::ReadOwner);

    const auto freezeUpsert = [&rootPath, &objectsPath, &result](const QString &path, const QString &expectedSha256, qint64 expectedSize, QJsonObject *entry) {
        if (!entry || !safeRelativePath(path) || !validSha256(expectedSha256) || expectedSize < 0 ||
            !writeFrozenObject(QDir(rootPath).filePath(path), QDir(objectsPath).filePath(expectedSha256), expectedSha256, expectedSize, &result.frozenBytes, &result.error)) {
            return false;
        }
        entry->insert(QStringLiteral("path"), path);
        entry->insert(QStringLiteral("action"), QStringLiteral("upsert"));
        entry->insert(QStringLiteral("sha256"), expectedSha256);
        entry->insert(QStringLiteral("size_bytes"), expectedSize);
        return true;
    };

    QJsonArray sources;
    QJsonArray accepted;
    QHash<QString, QJsonObject> manifestSources;
    for (const auto &raw : manifestObject.value(QStringLiteral("shared_files")).toArray()) {
        const auto entry = raw.toObject(); const auto path = entry.value(QStringLiteral("path")).toString();
        if (!QRegularExpression(QStringLiteral("^\\.thoughts/projects/[a-f0-9]{64}/(default|documents)/thoughts-project\\.json$")).match(path).hasMatch()
            || entry.value(QStringLiteral("role")).toString() != QLatin1String("render_manifest")) {
            result.error = QStringLiteral("The discovered manifest contains unmanaged shared sources.");
            discard(result, nullptr); return result;
        }
        manifestSources.insert(path, entry);
    }
    for (const QJsonValue &projectValue : projects) {
        const QJsonObject project = projectValue.toObject();
        const QJsonArray files = project.value(QStringLiteral("files")).toArray();
        if (project.value(QStringLiteral("project")).toString().isEmpty() || !project.value(QStringLiteral("files")).isArray()) {
            result.error = QStringLiteral("The discovered global source manifest contains an invalid project.");
            discard(result, nullptr);
            return result;
        }
        for (const QJsonValue &fileValue : files) {
            const QJsonObject sourceEntry = fileValue.toObject();
            const QString path = sourceEntry.value(QStringLiteral("path")).toString();
            const QString role = sourceEntry.value(QStringLiteral("role")).toString();
            if (!safeRelativePath(path) || manifestSources.contains(path) || role.isEmpty() || role.contains(QLatin1Char('\r')) || role.contains(QLatin1Char('\n'))) {
                result.error = QStringLiteral("The discovered global source manifest contains an invalid path.");
                discard(result, nullptr);
                return result;
            }
            manifestSources.insert(path, sourceEntry);
        }
    }

    // A global dependency refresh must freeze every target's build-input
    // inventory, but it must not turn every locally divergent managed source
    // into a source mutation. Source mutations come only from this exact
    // frozen pending generation, plus paths newly introduced by the refreshed
    // manifest and explicit tombstones for paths which left it.
    QHash<QString, DesktopPendingFile> pendingSources;
    for (const DesktopPendingFile &file : snapshot.files) {
        if (!safeRelativePath(file.path) || pendingSources.contains(file.path)) {
            result.error = QStringLiteral("The frozen global source generation contains an invalid or duplicated path.");
            discard(result, nullptr);
            return result;
        }
        const QString kind = file.hasTrackedState ? sourceKind(file.trackedState) : QString();
        const bool managedByManifest = manifestSources.contains(file.path);
        const bool explicitSourceTombstone = file.action == QLatin1String("delete") && kind == QLatin1String("source") && sourceBasesByPath.contains(file.path);
        if (!managedByManifest && !explicitSourceTombstone) {
            continue;
        }
        if (!file.hasTrackedState || file.trackedState.metadata.value(QStringLiteral("kind")).toString() != QLatin1String("file")) {
            result.error = QStringLiteral("A frozen global source has no resolved immutable identity.");
            discard(result, nullptr);
            return result;
        }
        pendingSources.insert(file.path, file);
    }

    QStringList sourcePaths = pendingSources.keys();
    for (auto iterator = manifestSources.cbegin(); iterator != manifestSources.cend(); ++iterator) {
        if (!sourceBasesByPath.contains(iterator.key()) && !pendingSources.contains(iterator.key())) {
            sourcePaths.append(iterator.key());
        }
    }
    std::sort(sourcePaths.begin(), sourcePaths.end());
    QSet<QString> emittedSourcePaths;
    for (const QString &path : std::as_const(sourcePaths)) {
        const auto pending = pendingSources.constFind(path);
        if (pending != pendingSources.cend() && pending->action == QLatin1String("delete")) {
            if (pending->trackedState.exists) {
                result.error = QStringLiteral("A frozen global source deletion still exists locally.");
                discard(result, nullptr);
                return result;
            }
            QJsonObject deletion {{QStringLiteral("path"), path}, {QStringLiteral("action"), QStringLiteral("delete")}, {QStringLiteral("base_sha256"), sourceBaseSha256(path, sourceBasesByPath)}};
            sources.append(deletion);
            accepted.append(publicEntry(deletion, QStringLiteral("source")));
            emittedSourcePaths.insert(path);
            continue;
        }

        QString digest;
        qint64 size = -1;
        if (pending != pendingSources.cend()) {
            if (pending->action != QLatin1String("upsert") || !pending->trackedState.exists || !validSha256(pending->trackedState.sha256) || pending->trackedState.sizeBytes < 0) {
                result.error = QStringLiteral("A frozen global source update has no complete immutable identity.");
                discard(result, nullptr);
                return result;
            }
            digest = pending->trackedState.sha256;
            size = pending->trackedState.sizeBytes;
        } else {
            QFile source(QDir(rootPath).filePath(path));
            if (!source.open(QIODevice::ReadOnly)) {
                result.error = QStringLiteral("A newly managed source could not be read: %1").arg(path);
                discard(result, nullptr);
                return result;
            }
            QCryptographicHash hash(QCryptographicHash::Sha256);
            size = 0;
            while (!source.atEnd()) {
                const QByteArray chunk = source.read(1024 * 1024);
                if (chunk.isEmpty() && source.error() != QFileDevice::NoError) {
                    result.error = QStringLiteral("A newly managed source could not be read: %1").arg(path);
                    discard(result, nullptr);
                    return result;
                }
                hash.addData(chunk);
                if (!safeAdd(size, chunk.size(), &size)) {
                    result.error = QStringLiteral("A newly managed source is too large: %1").arg(path);
                    discard(result, nullptr);
                    return result;
                }
            }
            digest = QString::fromLatin1(hash.result().toHex());
        }
        QJsonObject entry;
        if (!freezeUpsert(path, digest, size, &entry)) {
            discard(result, nullptr);
            return result;
        }
        entry.insert(QStringLiteral("base_sha256"), sourceBaseSha256(path, sourceBasesByPath));
        sources.append(entry);
        accepted.append(publicEntry(entry, QStringLiteral("source")));
        emittedSourcePaths.insert(path);
    }

    QHash<QString, QHash<QString, QJsonObject>> buildByTarget;
    QHash<QString, QString> projectsByTarget;
    QHash<QString, DesktopTrackedPath> acceptedBuildByPath;
    for (const DesktopTrackedPath &baseline : acceptedBaselines) {
        if (baseline.metadata.value(QStringLiteral("kind")).toString() != QLatin1String("file") || sourceKind(baseline) != QLatin1String("build_input")) {
            continue;
        }
        if (acceptedBuildByPath.contains(baseline.path)) {
            result.error = QStringLiteral("The accepted global build-input identities are duplicated.");
            discard(result, nullptr);
            return result;
        }
        acceptedBuildByPath.insert(baseline.path, baseline);
    }
    for (const QJsonValue &inventoryValue : targetInventories) {
        const QJsonObject inventory = inventoryValue.toObject();
        const QString target = inventory.value(QStringLiteral("target")).toString();
        const QString project = inventory.value(QStringLiteral("project")).toString();
        const QJsonArray entries = inventory.value(QStringLiteral("entries")).toArray();
        if (!safeTarget(target) || project.isEmpty() || projectsByTarget.contains(target) || !inventory.value(QStringLiteral("entries")).isArray()) {
            result.error = QStringLiteral("A discovered build-input inventory is malformed.");
            discard(result, nullptr);
            return result;
        }
        projectsByTarget.insert(target, project);
        QHash<QString, QJsonObject> targetEntries;
        for (const QJsonValue &entryValue : entries) {
            const QJsonObject inventoryEntry = entryValue.toObject();
            const QString path = inventoryEntry.value(QStringLiteral("path")).toString();
            const QString digest = inventoryEntry.value(QStringLiteral("sha256")).toString();
            const qint64 size = inventoryEntry.value(QStringLiteral("size_bytes")).toInteger(-1);
            const QString mediaType = inventoryEntry.value(QStringLiteral("media_type")).toString();
            const QString role = inventoryEntry.value(QStringLiteral("role")).toString();
            QJsonObject entry;
            if (targetEntries.contains(path) || mediaType.isEmpty() || role.isEmpty() || !safeRelativePath(path) || !validSha256(digest) || size < 0) {
                result.error = QStringLiteral("A discovered build-input identity is malformed.");
                discard(result, nullptr);
                return result;
            }
            // Discovery has freshly verified this identity. Reuse the accepted
            // server object instead of reading and copying the same bytes again.
            const auto baseline = acceptedBuildByPath.constFind(path);
            if (baseline != acceptedBuildByPath.cend() && acceptedBuildIdentityMatches(*baseline, target, inventoryEntry)) {
                entry = buildEntry(*baseline, true);
            } else if (!freezeUpsert(path, digest, size, &entry)) {
                if (result.error.isEmpty()) {
                    result.error = QStringLiteral("A discovered build-input identity is malformed.");
                }
                discard(result, nullptr);
                return result;
            }
            entry.insert(QStringLiteral("media_type"), mediaType);
            entry.insert(QStringLiteral("role"), role);
            targetEntries.insert(path, entry);
        }
        buildByTarget.insert(target, targetEntries);
    }

    // Preserve explicit tombstones from the frozen generation.  They make a
    // removed source visible even after it left the regenerated manifest and
    // keep build-input acceptance exact for each affected PDF.
    for (const DesktopPendingFile &file : snapshot.files) {
        if (file.action != QLatin1String("delete") || !safeRelativePath(file.path)) {
            continue;
        }
        const QString kind = sourceKind(file.trackedState);
        if (kind == QLatin1String("source") && !emittedSourcePaths.contains(file.path)) {
            QJsonObject deletion {{QStringLiteral("path"), file.path}, {QStringLiteral("action"), QStringLiteral("delete")}, {QStringLiteral("base_sha256"), sourceBaseSha256(file.path, sourceBasesByPath)}};
            sources.append(deletion);
            accepted.append(publicEntry(deletion, QStringLiteral("source")));
            emittedSourcePaths.insert(file.path);
        } else if (kind == QLatin1String("build_input")) {
            const QString role = file.trackedState.metadata.value(QStringLiteral("role")).toString();
            if (role.isEmpty()) {
                result.error = QStringLiteral("A removed build input has no discovered role.");
                discard(result, nullptr);
                return result;
            }
            for (const QString &target : file.affectedTargets) {
                if (!projectsByTarget.contains(target)) {
                    continue;
                }
                QJsonObject deletion {{QStringLiteral("path"), file.path}, {QStringLiteral("action"), QStringLiteral("delete")}, {QStringLiteral("role"), role}};
                buildByTarget[target].insert(file.path, deletion);
            }
        }
    }

    QJsonObject buildInputs;
    QStringList targets = buildByTarget.keys();
    std::sort(targets.begin(), targets.end(), [](const QString &left, const QString &right) { return left.compare(right, Qt::CaseInsensitive) < 0; });
    for (const QString &target : std::as_const(targets)) {
        QJsonArray entries;
        QStringList paths = buildByTarget.value(target).keys();
        std::sort(paths.begin(), paths.end());
        for (const QString &path : std::as_const(paths)) {
            const QJsonObject entry = buildByTarget.value(target).value(path);
            entries.append(entry);
            accepted.append(publicEntry(entry, QStringLiteral("build_input"), target));
        }
        buildInputs.insert(target, entries);
    }
    sortEntries(&sources, false);
    sortEntries(&accepted, true);
    const QJsonObject frozenManifest {
        {QStringLiteral("schema_version"), 2}, {QStringLiteral("source_root"), rootPath}, {QStringLiteral("objects_dir"), QStringLiteral("objects")}, {QStringLiteral("sources"), sources}, {QStringLiteral("build_inputs"), buildInputs}};
    result.manifestPath = QDir(result.directoryPath).filePath(QStringLiteral("inputs.json"));
    QSaveFile output(result.manifestPath);
    output.setDirectWriteFallback(false);
    const QByteArray encoded = QJsonDocument(frozenManifest).toJson(QJsonDocument::Compact);
    if (!output.open(QIODevice::WriteOnly) || output.write(encoded) != encoded.size() || !output.commit()) {
        result.error = QStringLiteral("The immutable global publication manifest could not be committed.");
        // A short write leaves the temporary file open, which would keep
        // Windows from removing the bundle folder.
        output.cancelWriting();
        discard(result, nullptr);
        return result;
    }
    QFile::setPermissions(result.manifestPath, QFileDevice::ReadOwner);
    result.acceptedEntries = accepted;
    result.disposition = FrozenPublicationDisposition::Ready;
    result.error.clear();
    return result;
}

bool FrozenPublicationStager::discard(const FrozenPublicationBundle &bundle, QString *error)
{
    if (error) {
        error->clear();
    }
    if (bundle.directoryPath.isEmpty()) {
        return true;
    }
    const QFileInfo directory(bundle.directoryPath);
    const QFileInfo parent(directory.absolutePath());
    const QUuid id(directory.fileName());
    if (parent.fileName() != QLatin1String("frozen-publications") || id.isNull() || !directory.isDir() || !QDir(bundle.directoryPath).removeRecursively()) {
        if (error) {
            *error = QStringLiteral("The immutable publication bundle could not be removed safely.");
        }
        return false;
    }
    return true;
}

}
