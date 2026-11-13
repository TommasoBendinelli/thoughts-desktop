// SPDX-License-Identifier: GPL-2.0-or-later
#include "sourcechangetracker.h"
#include "docschangemonitor.h"
#include "thoughtignore.h"
#include "core/htmlattachment.h"
#include "thoughtsplatform.h"

#include <QDateTime>
#include <QDir>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QMimeDatabase>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrentRun>

#include <algorithm>
#include <limits>
#include <utility>

namespace Thoughts
{

namespace
{

QThreadPool *sourceTrackerPool()
{
    // Filesystem metadata and directory enumeration can enter an uninterruptible
    // File Provider call on macOS. Never put those jobs on Qt's global pool:
    // QCoreApplication waits for that pool during shutdown. This deliberately
    // process-lifetime pool is not destroyed (and therefore never waits for a
    // stuck provider worker); the OS ends any remaining thread at process exit.
    static QThreadPool *const pool = [] {
        auto *value = new QThreadPool;
        value->setMaxThreadCount(2);
        value->setExpiryTimeout(5000);
        value->setObjectName(QStringLiteral("thoughts-source-tracker"));
        return value;
    }();
    return pool;
}

QString cleanRelativePath(const QString &path)
{
    const QString clean = QDir::cleanPath(path.trimmed());
    if (clean.isEmpty() || clean == QLatin1String(".") || QDir::isAbsolutePath(clean)
        || clean == QLatin1String("..") || clean.startsWith(QLatin1String("../"))) {
        return {};
    }
    return clean;
}

QStringList cleanTargets(QStringList targets)
{
    for (QString &target : targets) {
        target = target.trimmed();
    }
    targets.removeAll(QString());
    targets.removeDuplicates();
    std::sort(targets.begin(), targets.end());
    return targets;
}

bool isDirectorySentinel(const DesktopTrackedPath &path)
{
    return path.metadata.value(QStringLiteral("kind")).toString() == QLatin1String("directory");
}

bool isCoverageSentinel(const DesktopTrackedPath &path)
{
    const QString scope = path.metadata.value(QStringLiteral("_tracking_scope")).toString();
    return isDirectorySentinel(path)
        && path.metadata.value(QStringLiteral("_monitoring_mode")).toString()
            == QLatin1String("discovery")
        && (scope == QLatin1String("project") || scope == QLatin1String("global"))
        && !cleanRelativePath(path.metadata.value(QStringLiteral("_coverage_root")).toString()).isEmpty();
}

bool isUnresolved(const DesktopTrackedPath &path)
{
    return path.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved");
}

bool isIgnoredDescriptor(const DesktopTrackedPath &path)
{
    return !isDirectorySentinel(path)
        && path.metadata.value(QStringLiteral("source_kind")).toString()
            == QLatin1String("ignored");
}

bool isBackgroundSource(const DesktopTrackedPath &path)
{
    if (isDirectorySentinel(path) || isIgnoredDescriptor(path) || path.affectedTargets.isEmpty()) {
        return false;
    }
    const QString suffix = QFileInfo(path.path).suffix();
    return suffix.compare(QLatin1String("tex"), Qt::CaseInsensitive) == 0
        || suffix.compare(QLatin1String("md"), Qt::CaseInsensitive) == 0
        || suffix.compare(QLatin1String("markdown"), Qt::CaseInsensitive) == 0;
}

bool validSha256(const QString &sha256)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(sha256).hasMatch();
}

QString defaultHashExecutable()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("/usr/bin/shasum");
#elif defined(Q_OS_WIN)
    // Hashed in-process; see hashFileWithHelper.
    return QString();
#else
    const QString sha256sum = QStandardPaths::findExecutable(QStringLiteral("sha256sum"));
    return !sha256sum.isEmpty() ? sha256sum : QStandardPaths::findExecutable(QStringLiteral("shasum"));
#endif
}

QString boundedDiagnostic(QByteArray diagnostic)
{
    diagnostic = diagnostic.trimmed();
    constexpr qsizetype maximumBytes = 512;
    if (diagnostic.size() > maximumBytes) {
        diagnostic = diagnostic.left(maximumBytes);
        diagnostic.append("...");
    }
    return QString::fromUtf8(diagnostic);
}

bool reconciliationCancelled(const std::shared_ptr<std::atomic<quint64>> &requestedGeneration,
                            quint64 generation);

std::optional<QString> hashFileWithHelper(const QString &executable,
                                          const QString &absolutePath,
                                          const QString &relativePath,
                                          int timeoutMs,
                                          QString *error,
                                          const std::shared_ptr<std::atomic<quint64>> &requestedGeneration,
                                          quint64 generation)
{
    if (executable.isEmpty()) {
#ifdef Q_OS_WIN
        // Windows has no SHA-256 command; hash here, still honouring the
        // timeout and cancellation the helper process would.
        QFile file(absolutePath);
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) {
                *error = QStringLiteral("could not read tracked source path %1: %2").arg(relativePath, file.errorString());
            }
            return std::nullopt;
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        QElapsedTimer elapsed;
        elapsed.start();
        QByteArray buffer(1024 * 1024, Qt::Uninitialized);
        for (;;) {
            if (reconciliationCancelled(requestedGeneration, generation)) {
                if (error) {
                    *error = QStringLiteral("source reconciliation was cancelled");
                }
                return std::nullopt;
            }
            if (elapsed.elapsed() > std::max(1, timeoutMs)) {
                if (error) {
                    *error = QStringLiteral("hashing tracked source path %1 timed out").arg(relativePath);
                }
                return std::nullopt;
            }
            const qint64 count = file.read(buffer.data(), buffer.size());
            if (count < 0) {
                if (error) {
                    *error = QStringLiteral("could not read tracked source path %1: %2").arg(relativePath, file.errorString());
                }
                return std::nullopt;
            }
            if (count == 0) {
                break;
            }
            hash.addData(QByteArrayView(buffer.constData(), count));
        }
        return QString::fromLatin1(hash.result().toHex());
#else
        if (error) {
            *error = QStringLiteral("no SHA-256 helper is available for tracked source path %1").arg(relativePath);
        }
        return std::nullopt;
#endif
    }
    QStringList arguments;
    if (QFileInfo(executable).fileName().contains(QLatin1String("sha256sum"))) {
        arguments = {QStringLiteral("--"), absolutePath};
    } else {
        arguments = {QStringLiteral("-a"), QStringLiteral("256"), QStringLiteral("--"), absolutePath};
    }
    QProcess process;
    process.setProgram(executable);
    process.setArguments(arguments);
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);
    const int boundedTimeout = std::max(1, timeoutMs);
    if (!process.waitForStarted(std::min(2000, boundedTimeout))) {
        if (error) {
            *error = QStringLiteral("could not start SHA-256 helper for tracked source path %1: %2")
                         .arg(relativePath, process.errorString());
        }
        return std::nullopt;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (process.state() != QProcess::NotRunning) {
        if (reconciliationCancelled(requestedGeneration, generation)) {
            process.kill();
            process.waitForFinished(1000);
            if (error) {
                *error = QStringLiteral("source reconciliation was cancelled");
            }
            return std::nullopt;
        }
        const int remaining = boundedTimeout - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0 || !process.waitForFinished(std::min(50, remaining))) {
            if (remaining > 0) {
                continue;
            }
            process.kill();
            process.waitForFinished(1000);
            if (error) {
                *error = QStringLiteral("timed out hashing tracked source path %1 after %2 ms; make the file available locally and retry")
                             .arg(relativePath)
                             .arg(boundedTimeout);
            }
            return std::nullopt;
        }
    }
    const QByteArray standardOutput = process.readAllStandardOutput();
    const QString diagnostic = boundedDiagnostic(process.readAllStandardError());
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error) {
            *error = QStringLiteral("could not hash tracked source path %1%2")
                         .arg(relativePath,
                              diagnostic.isEmpty() ? QString() : QStringLiteral(": %1").arg(diagnostic));
        }
        return std::nullopt;
    }
    static const QRegularExpression digestPattern(QStringLiteral("^([0-9a-fA-F]{64})(?:\\s|$)"));
    const QRegularExpressionMatch match = digestPattern.match(QString::fromLatin1(standardOutput));
    if (!match.hasMatch()) {
        if (error) {
            *error = QStringLiteral("SHA-256 helper returned malformed output for tracked source path %1").arg(relativePath);
        }
        return std::nullopt;
    }
    return match.captured(1).toLower();
}

QString fileIdentity(const QString &absolutePath, const QFileInfo &info)
{
    const QString physical = Thoughts::Platform::physicalFileIdentity(absolutePath);
    if (!physical.isEmpty()) {
        return physical;
    }
    return QStringLiteral("%1:%2").arg(info.canonicalFilePath()).arg(info.birthTime().toMSecsSinceEpoch());
}

bool sameCandidateIdentity(const DesktopTrackedPath &left, const DesktopTrackedPath &right)
{
    return left.path == right.path
        && left.exists == right.exists
        && left.sizeBytes == right.sizeBytes
        && left.modifiedAtMs == right.modifiedAtMs
        && left.metadata.value(QStringLiteral("_local_ctime_ms")) == right.metadata.value(QStringLiteral("_local_ctime_ms"))
        && left.metadata.value(QStringLiteral("_local_file_identity")) == right.metadata.value(QStringLiteral("_local_file_identity"));
}

bool reconciliationCancelled(const std::shared_ptr<std::atomic<quint64>> &requestedGeneration,
                            quint64 generation)
{
    return requestedGeneration
        && requestedGeneration->load(std::memory_order_acquire) != generation;
}

QString commonMetadataValue(const QList<DesktopTrackedPath> &siblings, const QString &key)
{
    QString value;
    for (const DesktopTrackedPath &sibling : siblings) {
        const QString candidate = sibling.metadata.value(key).toString();
        if (candidate.isEmpty()) {
            continue;
        }
        if (value.isEmpty()) {
            value = candidate;
        } else if (value != candidate) {
            return {};
        }
    }
    return value;
}

DesktopTrackedPath candidateDescriptor(const DesktopTrackedPath &physical,
                                        const DesktopTrackedPath &sentinel,
                                        const QList<DesktopTrackedPath> &tracked,
                                        const QList<DesktopTrackedPath> &deleted)
{
    DesktopTrackedPath result = physical;
    result.affectedTargets = sentinel.affectedTargets;

    QList<DesktopTrackedPath> identityMatches;
    const QString identity = physical.metadata.value(QStringLiteral("_local_file_identity")).toString();
    for (const DesktopTrackedPath &old : deleted) {
        const QString oldIdentity = old.metadata.value(QStringLiteral("_local_file_identity")).toString();
        const bool identityMatch = !identity.isEmpty() && identity == oldIdentity;
        const bool contentMatch = validSha256(physical.sha256) && physical.sha256 == old.sha256;
        if (!isUnresolved(old) && (identityMatch || contentMatch)) {
            identityMatches.append(old);
        }
    }
    if (identityMatches.size() == 1) {
        const DesktopTrackedPath renamed = identityMatches.constFirst();
        result.metadata = renamed.metadata;
        result.metadata.insert(QStringLiteral("_local_ctime_ms"), physical.metadata.value(QStringLiteral("_local_ctime_ms")));
        result.metadata.insert(QStringLiteral("_local_file_identity"), physical.metadata.value(QStringLiteral("_local_file_identity")));
        result.affectedTargets.append(renamed.affectedTargets);
        result.affectedTargets = cleanTargets(result.affectedTargets);
        return result;
    }

    QList<DesktopTrackedPath> siblings;
    const QString parent = QFileInfo(physical.path).path();
    for (const DesktopTrackedPath &known : tracked) {
        if (!isDirectorySentinel(known) && !isUnresolved(known)
            && QFileInfo(known.path).path() == parent
            && known.metadata.value(QStringLiteral("kind")).toString() == QLatin1String("file")) {
            siblings.append(known);
        }
    }

    QJsonObject metadata {{QStringLiteral("kind"), QStringLiteral("file")}};
    QString project = commonMetadataValue(siblings, QStringLiteral("project"));
    QString sourceKind = commonMetadataValue(siblings, QStringLiteral("source_kind"));
    QString role = commonMetadataValue(siblings, QStringLiteral("role"));
    QString mediaType = commonMetadataValue(siblings, QStringLiteral("media_type"));
    if (project.isEmpty()) {
        project = sentinel.metadata.value(QStringLiteral("project")).toString();
    }
    if (Thoughts::isHtmlSourcePath(physical.path)) {
        sourceKind = QStringLiteral("build_input");
        role = QStringLiteral("asset");
        mediaType = QStringLiteral("text/html");
    }
    if (sourceKind == QLatin1String("build_input") && mediaType.isEmpty()) {
        mediaType = QMimeDatabase().mimeTypeForFile(physical.path, QMimeDatabase::MatchExtension).name();
    }
    if (!project.isEmpty()) {
        metadata.insert(QStringLiteral("project"), project);
    }
    if (!sourceKind.isEmpty()) {
        metadata.insert(QStringLiteral("source_kind"), sourceKind);
    }
    if (!role.isEmpty()) {
        metadata.insert(QStringLiteral("role"), role);
    }
    if (!mediaType.isEmpty() && sourceKind == QLatin1String("build_input")) {
        metadata.insert(QStringLiteral("media_type"), mediaType);
    }
    const QString trackingScope = sentinel.metadata.value(QStringLiteral("_tracking_scope")).toString();
    const QString coverageRoot = cleanRelativePath(sentinel.metadata.value(QStringLiteral("_coverage_root")).toString());
    const QString monitoringMode = sentinel.metadata.value(QStringLiteral("_monitoring_mode")).toString();
    if (monitoringMode == QLatin1String("discovery")
        && (trackingScope == QLatin1String("project") || trackingScope == QLatin1String("global"))
        && !coverageRoot.isEmpty()) {
        metadata.insert(QStringLiteral("_monitoring_mode"), monitoringMode);
        metadata.insert(QStringLiteral("_tracking_scope"), trackingScope);
        metadata.insert(QStringLiteral("_coverage_root"), coverageRoot);
    }
    const bool resolvedSource = sourceKind == QLatin1String("source")
        && !project.isEmpty() && !role.isEmpty();
    const bool resolvedBuildInput = sourceKind == QLatin1String("build_input")
        && !project.isEmpty() && !role.isEmpty() && !mediaType.isEmpty();
    if (!resolvedSource && !resolvedBuildInput) {
        metadata.insert(QStringLiteral("_tracking_resolution"), QStringLiteral("unresolved"));
        QJsonArray targetHints;
        for (const QString &target : std::as_const(result.affectedTargets)) {
            targetHints.append(target);
        }
        metadata.insert(QStringLiteral("_affected_target_hints"), targetHints);
    }
    metadata.insert(QStringLiteral("_local_ctime_ms"), physical.metadata.value(QStringLiteral("_local_ctime_ms")));
    metadata.insert(QStringLiteral("_local_file_identity"), physical.metadata.value(QStringLiteral("_local_file_identity")));
    result.metadata = metadata;
    return result;
}

bool excludedProjectPath(const QString &rootPath, const QString &relative, bool directory,
                         QHash<QString, std::shared_ptr<ThoughtIgnore>> *cache, QString *error)
{
    // A docs-* folder or a Project's default folder applies its own policy.
    const QString project = SourceOwner::of(relative);
    if (project.isEmpty() || relative.size() <= project.size()) return false;
    const qsizetype separator = project.size();
    auto ignored = cache->value(project);
    if (!ignored) {
        ignored = std::make_shared<ThoughtIgnore>(QDir(rootPath).filePath(project));
        cache->insert(project, ignored);
    }
    if (!ignored->error().isEmpty()) {
        if (error) *error = project + QStringLiteral(": ") + ignored->error();
        return true;
    }
    return ignored->excludes(relative.mid(separator + 1), directory);
}

QHash<QString, DesktopTrackedPath> discoveredDirectoryEntries(const QString &rootPath,
                                                              const QList<DesktopTrackedPath> &tracked,
                                                              QString *error)
{
    QHash<QString, DesktopTrackedPath> discoveries;
    QHash<QString, std::shared_ptr<ThoughtIgnore>> ignoreCache;
    QSet<QString> knownPaths;
    for (const DesktopTrackedPath &path : tracked) {
        knownPaths.insert(path.path);
    }

    const QDir root(rootPath);
    QSet<QString> scannedDirectories;
    for (const DesktopTrackedPath &sentinel : tracked) {
        if (!isCoverageSentinel(sentinel)) {
            continue;
        }
        const QString sentinelPath = cleanRelativePath(sentinel.path);
        const QString absoluteSentinel = root.absoluteFilePath(sentinelPath);
        if (sentinelPath.isEmpty() || scannedDirectories.contains(sentinelPath)
            || !QFileInfo(absoluteSentinel).isDir()) {
            continue;
        }
        scannedDirectories.insert(sentinelPath);
        const QFileInfoList entries = QDir(absoluteSentinel).entryInfoList(
            QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot | QDir::Readable,
            QDir::Name | QDir::DirsFirst);
        for (const QFileInfo &entry : entries) {
            if (Platform::isDirectoryLink(entry)) {
                continue;
            }
            const QString relative = cleanRelativePath(root.relativeFilePath(entry.absoluteFilePath()));
            if (relative.isEmpty() || knownPaths.contains(relative)) {
                continue;
            }
            if (excludedProjectPath(rootPath, relative, entry.isDir(), &ignoreCache, error)) {
                if (error && !error->isEmpty()) return discoveries;
                continue;
            }
            // Inside docs-* projects .ThoughtIgnore above is the whole policy.
            if (entry.isDir() ? !isEligibleDesktopSourceDirectory(relative)
                              : (!entry.isFile() || !isPotentialDesktopSourcePath(relative))) {
                continue;
            }
            DesktopTrackedPath descriptor = sentinel;
            descriptor.path = relative;
            // Bootstrap state belongs only to the registered directory that
            // owns the initial census. Descendants must not inherit it or a
            // later file can be mistaken for part of that census forever.
            descriptor.metadata.remove(QStringLiteral("_tracking_bootstrap"));
            if (entry.isDir()) {
                // Preserve the registered project/root identity as coverage is
                // extended into a newly observed subtree. Directory sensors
                // never become publication work themselves.
                descriptor.metadata.insert(QStringLiteral("kind"), QStringLiteral("directory"));
                descriptor.metadata.remove(QStringLiteral("_tracking_resolution"));
            } else {
                descriptor.metadata.insert(QStringLiteral("kind"), QStringLiteral("file"));
            }
            discoveries.insert(relative, descriptor);
        }
    }
    return discoveries;
}

}

SourceChangeTracker::SourceChangeTracker(QString rootPath, DesktopSyncStore *store, QObject *parent)
    : QObject(parent)
    , m_rootPath(QDir::cleanPath(QDir(std::move(rootPath)).absolutePath()))
    , m_hashExecutable(defaultHashExecutable())
    , m_store(store)
    , m_debounceTimer(new QTimer(this))
    , m_integrityAuditTimer(new QTimer(this))
    , m_requestedGeneration(std::make_shared<std::atomic<quint64>>(0))
{
    m_debounceTimer->setSingleShot(true);
    m_debounceTimer->setInterval(180);
    // One recursive stream for the whole root, shared with the rest of the
    // application; only batches touching tracked sources start a scan.
    DocsChangeMonitor *monitor = DocsChangeMonitor::forRoot(m_rootPath);
    connect(monitor, &DocsChangeMonitor::pathsChanged, this, [this](const QStringList &paths, const QStringList &entryPaths) {
        const QSet<QString> entries(entryPaths.cbegin(), entryPaths.cend());
        if (std::any_of(paths.cbegin(), paths.cend(), [&](const QString &path) { return isRelevantChange(path, entries.contains(path)); })) {
            scheduleReconciliation();
        }
    });
    connect(monitor, &DocsChangeMonitor::rescanRequired, this, [this] {
        if (!m_trackedPaths.isEmpty()) {
            scheduleReconciliation();
        }
    });
    connect(m_debounceTimer, &QTimer::timeout, this, &SourceChangeTracker::startAsyncReconciliation);
    m_integrityAuditTimer->setInterval(30000);
    connect(m_integrityAuditTimer, &QTimer::timeout, this, [this] {
        constexpr qsizetype pathsPerAudit = 8;
        if (m_backgroundSources.isEmpty() || m_reconciliationInProgress || m_metadataProbeInProgress) {
            return;
        }
        QList<DesktopTrackedPath> paths;
        paths.reserve(std::min(pathsPerAudit, m_backgroundSources.size()));
        while (paths.size() < std::min(pathsPerAudit, m_backgroundSources.size())) {
            if (m_integrityAuditIndex >= m_backgroundSources.size()) {
                m_integrityAuditIndex = 0;
            }
            const DesktopTrackedPath &tracked = m_backgroundSources.at(m_integrityAuditIndex++);
            paths.append(tracked);
        }
        startMetadataProbe(paths);
    });
}

SourceChangeTracker::~SourceChangeTracker()
{
    m_requestedGeneration->fetch_add(1, std::memory_order_release);
}

DesktopTrackedPath SourceChangeTracker::coverageSentinel(QString path,
                                                         QString project,
                                                         QStringList affectedTargets)
{
    DesktopTrackedPath sentinel;
    sentinel.path = cleanRelativePath(path);
    project = project.trimmed();
    sentinel.affectedTargets = cleanTargets(std::move(affectedTargets));
    if (sentinel.path.isEmpty()) {
        return sentinel;
    }
    sentinel.exists = true;
    sentinel.sizeBytes = -1;
    sentinel.modifiedAtMs = -1;
    sentinel.metadata = QJsonObject {
        {QStringLiteral("kind"), QStringLiteral("directory")},
        {QStringLiteral("_monitoring_mode"), QStringLiteral("discovery")},
        {QStringLiteral("_tracking_scope"), project.isEmpty() ? QStringLiteral("global") : QStringLiteral("project")},
        {QStringLiteral("_coverage_root"), sentinel.path},
    };
    if (!project.isEmpty()) {
        sentinel.metadata.insert(QStringLiteral("project"), project);
    }
    return sentinel;
}

QString SourceChangeTracker::rootPath() const
{
    return m_rootPath;
}

bool SourceChangeTracker::authoritative() const
{
    return m_authoritative;
}

QList<DesktopTrackedPath> SourceChangeTracker::trackedPaths() const
{
    return m_trackedPaths;
}

int SourceChangeTracker::debounceInterval() const
{
    return m_debounceTimer->interval();
}

bool SourceChangeTracker::reconciliationInProgress() const
{
    return m_reconciliationInProgress || m_debounceTimer->isActive();
}

bool SourceChangeTracker::settlingCandidates() const
{
    return !m_candidateStates.isEmpty();
}

QVariantMap SourceChangeTracker::diagnostics() const
{
    return {{QStringLiteral("tracked_paths"), m_trackedPaths.size()},
            {QStringLiteral("background_sources"), m_backgroundSources.size()},
            {QStringLiteral("watched_files"), m_backgroundSources.size()},
            {QStringLiteral("watched_directories"), m_watchedDirectoryCount},
            {QStringLiteral("setup_ms"), m_lastSetupMs},
            {QStringLiteral("first_setup_ms"), m_firstSetupMs},
            {QStringLiteral("scan_ms"), m_lastScanMs},
            {QStringLiteral("first_scan_ms"), m_firstScanMs},
            {QStringLiteral("store_ms"), m_lastStoreMs},
            {QStringLiteral("first_store_ms"), m_firstStoreMs},
            {QStringLiteral("finish_ms"), m_lastFinishMs},
            {QStringLiteral("observed_paths"), m_lastObservedCount},
            {QStringLiteral("hashed_paths"), m_lastHashedCount},
            {QStringLiteral("completed_at_ms"), m_lastCompletedAtMs},
            {QStringLiteral("running"), reconciliationInProgress()},
            {QStringLiteral("settling_candidates"), m_candidateStates.size()},
            {QStringLiteral("authoritative"), m_authoritative}};
}

int SourceChangeTracker::pruneIgnoredTombstones(QString *error)
{
    if (error) {
        error->clear();
    }
    if (m_reconciliationInProgress || !m_store || !m_store->initialized() || m_rootPath.isEmpty()) {
        return 0;
    }
    QHash<QString, std::shared_ptr<ThoughtIgnore>> ignoreCache;
    QSet<QString> ignored;
    for (const DesktopTrackedPath &tracked : std::as_const(m_trackedPaths)) {
        QString ignoreError;
        // An unreadable .ThoughtIgnore excludes conservatively during scans;
        // never treat that as permission to forget a path.
        if (!tracked.exists && excludedProjectPath(m_rootPath, tracked.path, isDirectorySentinel(tracked), &ignoreCache, &ignoreError)
            && ignoreError.isEmpty()) {
            ignored.insert(tracked.path);
        }
    }
    if (ignored.isEmpty()) {
        return 0;
    }
    int removed = 0;
    if (!m_store->removeTombstones(QStringList(ignored.cbegin(), ignored.cend()), &removed, error)) {
        return -1;
    }
    // Rows with pending changes are kept by the store; keep tracking those.
    QString persistedError;
    QSet<QString> persisted;
    for (const DesktopTrackedPath &path : m_store->trackingPaths(&persistedError)) persisted.insert(path.path);
    if (!persistedError.isEmpty()) {
        if (error) *error = persistedError;
        return -1;
    }
    QList<DesktopTrackedPath> remaining;
    remaining.reserve(m_trackedPaths.size());
    for (const DesktopTrackedPath &tracked : std::as_const(m_trackedPaths)) {
        if (!ignored.contains(tracked.path) || persisted.contains(tracked.path)) remaining.append(tracked);
    }
    return setTrackedPaths(remaining, error) ? removed : -1;
}

bool SourceChangeTracker::setTrackedPaths(const QList<DesktopTrackedPath> &paths, QString *error)
{
    QElapsedTimer setupElapsed;
    setupElapsed.start();
    if (error) {
        error->clear();
    }
    if (!m_store || !m_store->initialized() || m_rootPath.isEmpty()) {
        if (error) {
            *error = QStringLiteral("source change tracker is not initialized");
        }
        return false;
    }
    QList<DesktopTrackedPath> requestedPaths = paths;
    QSet<QString> registeredCoverageRoots;
    QSet<QString> requestedPathNames;
    for (const DesktopTrackedPath &path : std::as_const(requestedPaths)) {
        requestedPathNames.insert(cleanRelativePath(path.path));
        if (isCoverageSentinel(path)) {
            registeredCoverageRoots.insert(cleanRelativePath(path.metadata.value(QStringLiteral("_coverage_root")).toString()));
        }
    }
    if (!registeredCoverageRoots.isEmpty()) {
        QString persistedError;
        const QList<DesktopTrackedPath> persisted = m_store->trackingPaths(&persistedError);
        if (!persistedError.isEmpty()) {
            if (error) {
                *error = persistedError;
            }
            return false;
        }
        for (const DesktopTrackedPath &path : persisted) {
            const QString root = cleanRelativePath(path.metadata.value(QStringLiteral("_coverage_root")).toString());
            const QString cleanPath = cleanRelativePath(path.path);
            if (isCoverageSentinel(path) && registeredCoverageRoots.contains(root)
                && !requestedPathNames.contains(cleanPath)) {
                requestedPaths.append(path);
                requestedPathNames.insert(cleanPath);
            }
        }
    }

    QList<DesktopTrackedPath> normalized;
    QSet<QString> seen;
    normalized.reserve(requestedPaths.size());
    for (DesktopTrackedPath path : std::as_const(requestedPaths)) {
        path.path = cleanRelativePath(path.path);
        path.affectedTargets = cleanTargets(path.affectedTargets);
        const QString kind = path.metadata.value(QStringLiteral("kind")).toString();
        if (path.path.isEmpty() || seen.contains(path.path)
            || (!kind.isEmpty() && kind != QLatin1String("file") && kind != QLatin1String("directory"))) {
            if (error) {
                *error = QStringLiteral("invalid or duplicate tracked source path");
            }
            return false;
        }
        seen.insert(path.path);
        normalized.append(path);
    }
    std::sort(normalized.begin(), normalized.end(), [](const DesktopTrackedPath &left, const DesktopTrackedPath &right) {
        return left.path < right.path;
    });
    m_trackedPaths = normalized;
    refreshBackgroundSources();
    m_candidateStates.clear();
    m_settlementKnown = false;
    setAuthoritative(false);
    if (!m_store->markReconciliationRequired(error)) {
        return false;
    }
    if (!configureWatcher(error)) {
        return false;
    }
    m_lastSetupMs = setupElapsed.elapsed();
    if (m_firstSetupMs < 0) m_firstSetupMs = m_lastSetupMs;
    if (m_reconciliationInProgress) {
        m_requestedGeneration->fetch_add(1, std::memory_order_acq_rel);
        if (!m_reconciliationQueued) {
            m_reconciliationQueued = true;
            Q_EMIT reconciliationScheduled();
        }
    } else {
        scheduleReconciliation();
    }
    return true;
}

void SourceChangeTracker::setDebounceInterval(int milliseconds)
{
    m_debounceTimer->setInterval(std::max(0, milliseconds));
}

void SourceChangeTracker::setHashHelperForTests(QString executable, int timeoutMs)
{
    m_hashExecutable = std::move(executable);
    m_hashTimeoutMs = std::max(1, timeoutMs);
}

bool SourceChangeTracker::reconcileNow(QString *error)
{
    m_debounceTimer->stop();
    if (error) {
        error->clear();
    }
    if (m_reconciliationInProgress) {
        if (error) {
            *error = QStringLiteral("asynchronous source reconciliation is already running");
        }
        return false;
    }
    if (!m_store || !m_store->initialized()) {
        m_settlementKnown = false;
        setAuthoritative(false);
        if (error) {
            *error = QStringLiteral("source change tracker store is not open");
        }
        return false;
    }
    m_requestedGeneration->fetch_add(1, std::memory_order_acq_rel);
    ScanResult scan = scanPaths(m_rootPath,
                                m_trackedPaths,
                                m_candidateStates,
                                std::max(1, m_debounceTimer->interval()),
                                m_hashExecutable,
                                m_hashTimeoutMs,
                                m_requestedGeneration,
                                m_requestedGeneration->load(std::memory_order_acquire));
    if (!scan.success) {
        m_settlementKnown = false;
        setAuthoritative(false);
        if (error) {
            *error = scan.error;
        }
        return false;
    }
    for (const QString &path : scan.hashedPaths) {
        Q_EMIT fileHashed(path);
    }
    const bool bootstrapComplete = !scan.waitingForStableCandidate
        && !scan.discoveredDirectoryAdded && scan.candidateStates.isEmpty();
    if (!m_store->reconcileStartupMetadata(scan.observed, bootstrapComplete, error)) {
        m_settlementKnown = false;
        setAuthoritative(false);
        return false;
    }
    const QHash<QString, QList<DesktopPendingFile>> pending = m_store->pendingFilesByTarget(error);
    if (error && !error->isEmpty()) {
        m_settlementKnown = false;
        setAuthoritative(false);
        return false;
    }
    for (auto iterator = pending.cbegin(); iterator != pending.cend(); ++iterator) {
        for (const DesktopPendingFile &file : *iterator) {
            if (file.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved")) {
                scan.unresolved = true;
                scan.unresolvedPaths.append(file.path);
            }
        }
    }
    const QList<DesktopTrackedPath> persistedTracking = m_store->trackingPaths(error);
    if (error && !error->isEmpty()) {
        m_settlementKnown = false;
        setAuthoritative(false);
        return false;
    }
    m_trackedPaths = persistedTracking;
    refreshBackgroundSources();
    m_candidateStates = scan.candidateStates;
    if (!configureWatcher(error)) {
        m_store->markReconciliationRequired(nullptr);
        m_settlementKnown = false;
        setAuthoritative(false);
        return false;
    }
    recordSettlement(scan);
    setAuthoritative(!scan.waitingForStableCandidate
                     && !scan.discoveredDirectoryAdded
                     && m_candidateStates.isEmpty()
                     && !scan.unresolved);
    Q_EMIT reconciliationFinished();
    if (scan.waitingForStableCandidate || scan.discoveredDirectoryAdded || !m_candidateStates.isEmpty()) {
        m_debounceTimer->start(std::max(1, m_debounceTimer->interval()));
    }
    return true;
}

SourceChangeTracker::ScanResult SourceChangeTracker::scanPaths(
    const QString &rootPath,
    const QList<DesktopTrackedPath> &trackedPaths,
    const QHash<QString, CandidateState> &candidateStates,
    int stabilityDelayMs,
    const QString &hashExecutable,
    int hashTimeoutMs,
    const std::shared_ptr<std::atomic<quint64>> &requestedGeneration,
    quint64 generation)
{
    ScanResult result;
    result.candidateStates = candidateStates;
    result.observed.reserve(trackedPaths.size());
    QList<DesktopTrackedPath> deleted;
    QHash<QString, std::shared_ptr<ThoughtIgnore>> ignoreCache;
    for (const DesktopTrackedPath &tracked : trackedPaths) {
        if (reconciliationCancelled(requestedGeneration, generation)) {
            result.error = QStringLiteral("source reconciliation was cancelled");
            return result;
        }
        if (isIgnoredDescriptor(tracked)) {
            result.observed.append(tracked);
            continue;
        }
        if (excludedProjectPath(rootPath, tracked.path, isDirectorySentinel(tracked), &ignoreCache, &result.error)) {
            if (!result.error.isEmpty()) return result;
            DesktopTrackedPath absent = tracked;
            absent.exists = false;
            absent.sha256.clear();
            absent.sizeBytes = -1;
            absent.modifiedAtMs = -1;
            absent.metadata.remove(QStringLiteral("_local_ctime_ms"));
            absent.metadata.remove(QStringLiteral("_local_file_identity"));
            result.observed.append(absent);
            if (!isDirectorySentinel(tracked) && tracked.exists) deleted.append(tracked);
            continue;
        }
        const std::optional<DesktopTrackedPath> path = observedPath(rootPath,
                                                                    tracked,
                                                                    hashExecutable,
                                                                    hashTimeoutMs,
                                                                    &result.error,
                                                                    &result.hashedPaths,
                                                                    requestedGeneration,
                                                                    generation);
        if (!path) {
            return result;
        }
        result.observed.append(*path);
        if (!isDirectorySentinel(tracked) && tracked.exists && !path->exists) {
            deleted.append(tracked);
        }
    }

    const QHash<QString, DesktopTrackedPath> discoveries = discoveredDirectoryEntries(rootPath, trackedPaths, &result.error);
    if (!result.error.isEmpty()) return result;
    QSet<QString> currentCandidates;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 stabilityDelay = std::max(1, stabilityDelayMs);
    QStringList discoveryPaths = discoveries.keys();
    std::sort(discoveryPaths.begin(), discoveryPaths.end());
    for (const QString &path : std::as_const(discoveryPaths)) {
        if (reconciliationCancelled(requestedGeneration, generation)) {
            result.error = QStringLiteral("source reconciliation was cancelled");
            return result;
        }
        currentCandidates.insert(path);
        DesktopTrackedPath physical = discoveries.value(path);
        const QString absolutePath = QDir(rootPath).absoluteFilePath(path);
        const QFileInfo info(absolutePath);
        const bool directory = isDirectorySentinel(physical);
        physical.exists = !Platform::isDirectoryLink(info) && (directory ? info.isDir() : info.isFile());
        physical.sizeBytes = physical.exists && !directory ? info.size() : -1;
        physical.modifiedAtMs = physical.exists ? info.lastModified().toMSecsSinceEpoch() : -1;
        if (physical.exists) {
            physical.metadata.insert(QStringLiteral("_local_ctime_ms"), info.metadataChangeTime().toMSecsSinceEpoch());
            physical.metadata.insert(QStringLiteral("_local_file_identity"), fileIdentity(absolutePath, info));
        }
        if (!directory) {
            physical = candidateDescriptor(physical, discoveries.value(path), trackedPaths, deleted);
        }
        const auto previous = result.candidateStates.constFind(path);
        if (previous == result.candidateStates.cend() || !sameCandidateIdentity(previous->observed, physical)) {
            result.candidateStates.insert(path, CandidateState {physical, now});
            result.waitingForStableCandidate = true;
            continue;
        }
        if (now - previous->firstSeenAtMs < stabilityDelay) {
            result.waitingForStableCandidate = true;
            continue;
        }
        if (directory) {
            result.observed.append(physical);
            result.candidateStates.remove(path);
            result.discoveredDirectoryAdded = true;
            result.addedDirectories.append(path);
            continue;
        }
        const std::optional<DesktopTrackedPath> hashed = observedPath(rootPath,
                                                                      physical,
                                                                      hashExecutable,
                                                                      hashTimeoutMs,
                                                                      &result.error,
                                                                      &result.hashedPaths,
                                                                      requestedGeneration,
                                                                      generation);
        if (!hashed) {
            return result;
        }
        DesktopTrackedPath resolved = candidateDescriptor(*hashed, discoveries.value(path), trackedPaths, deleted);
        resolved.sha256 = hashed->sha256;
        resolved.sizeBytes = hashed->sizeBytes;
        resolved.modifiedAtMs = hashed->modifiedAtMs;
        resolved.exists = hashed->exists;
        result.observed.append(resolved);
        result.candidateStates.remove(path);
    }
    for (auto iterator = result.candidateStates.begin(); iterator != result.candidateStates.end();) {
        if (!currentCandidates.contains(iterator.key())) {
            iterator = result.candidateStates.erase(iterator);
        } else {
            ++iterator;
        }
    }
    result.unresolved = false;
    result.success = true;
    return result;
}

void SourceChangeTracker::scheduleReconciliation()
{
    m_settlementKnown = false;
    setAuthoritative(false);
    if (m_reconciliationInProgress) {
        if (!m_reconciliationQueued) {
            m_reconciliationQueued = true;
            Q_EMIT reconciliationScheduled();
        }
        return;
    }
    m_requestedGeneration->fetch_add(1, std::memory_order_acq_rel);
    Q_EMIT reconciliationScheduled();
    m_debounceTimer->start();
}

void SourceChangeTracker::reconcileAsync()
{
    if (m_reconciliationInProgress) {
        m_settlementKnown = false;
        setAuthoritative(false);
        if (!m_reconciliationQueued) {
            m_reconciliationQueued = true;
            Q_EMIT reconciliationScheduled();
        }
        return;
    }
    m_requestedGeneration->fetch_add(1, std::memory_order_acq_rel);
    m_settlementKnown = false;
    setAuthoritative(false);
    Q_EMIT reconciliationScheduled();
    m_debounceTimer->stop();
    startAsyncReconciliation();
}

void SourceChangeTracker::startAsyncReconciliation()
{
    if (m_reconciliationInProgress) {
        return;
    }
    if (!m_store || !m_store->initialized()) {
        m_settlementKnown = false;
        setAuthoritative(false);
        Q_EMIT reconciliationFailed(QStringLiteral("source change tracker store is not open"));
        return;
    }
    const quint64 generation = m_requestedGeneration->load(std::memory_order_acquire);
    const QString rootPath = m_rootPath;
    const QString databasePath = m_store->databasePath();
    const QList<DesktopTrackedPath> trackedPaths = m_trackedPaths;
    const QHash<QString, CandidateState> candidateStates = m_candidateStates;
    const int stabilityDelayMs = std::max(1, m_debounceTimer->interval());
    const QString hashExecutable = m_hashExecutable;
    const int hashTimeoutMs = m_hashTimeoutMs;
    const std::shared_ptr<std::atomic<quint64>> requestedGeneration = m_requestedGeneration;

    m_reconciliationInProgress = true;
    auto *watcher = new QFutureWatcher<AsyncResult>(this);
    connect(watcher, &QFutureWatcher<AsyncResult>::finished, this, [this, watcher] {
        const AsyncResult result = watcher->result();
        watcher->deleteLater();
        finishAsyncReconciliation(result);
    });
    watcher->setFuture(QtConcurrent::run(sourceTrackerPool(), [rootPath,
                                          databasePath,
                                          trackedPaths,
                                          candidateStates,
                                          stabilityDelayMs,
                                          hashExecutable,
                                          hashTimeoutMs,
                                          generation,
                                          requestedGeneration] {
        AsyncResult result;
        result.generation = generation;
        QElapsedTimer scanElapsed;
        scanElapsed.start();
        result.scan = scanPaths(rootPath,
                                trackedPaths,
                                candidateStates,
                                stabilityDelayMs,
                                hashExecutable,
                                hashTimeoutMs,
                                requestedGeneration,
                                generation);
        result.scanMs = scanElapsed.elapsed();
        if (requestedGeneration->load(std::memory_order_acquire) != generation) {
            result.obsolete = true;
            return result;
        }
        if (!result.scan.success) {
            return result;
        }
        if (requestedGeneration->load(std::memory_order_acquire) != generation) {
            result.obsolete = true;
            return result;
        }
        if (databasePath.isEmpty() || databasePath == QLatin1String(":memory:")) {
            result.scan.success = false;
            result.scan.error = QStringLiteral("asynchronous source reconciliation requires a durable store");
            return result;
        }
        DesktopSyncStore workerStore(databasePath);
        QElapsedTimer storeElapsed;
        storeElapsed.start();
        if (!workerStore.open(&result.scan.error)) {
            result.scan.success = false;
            return result;
        }
        if (requestedGeneration->load(std::memory_order_acquire) != generation) {
            result.obsolete = true;
            return result;
        }
        const bool bootstrapComplete = !result.scan.waitingForStableCandidate
            && !result.scan.discoveredDirectoryAdded && result.scan.candidateStates.isEmpty();
        if (!workerStore.reconcileStartupMetadata(result.scan.observed,
                                                   bootstrapComplete,
                                                   &result.scan.error)) {
            result.scan.success = false;
            return result;
        }
        const QHash<QString, QList<DesktopPendingFile>> pending =
            workerStore.pendingFilesByTarget(&result.scan.error);
        if (!result.scan.error.isEmpty()) {
            result.scan.success = false;
            return result;
        }
        for (auto iterator = pending.cbegin(); iterator != pending.cend(); ++iterator) {
            for (const DesktopPendingFile &file : *iterator) {
                if (file.metadata.value(QStringLiteral("_tracking_resolution")).toString() == QLatin1String("unresolved")) {
                    result.scan.unresolved = true;
                    result.scan.unresolvedPaths.append(file.path);
                }
            }
        }
        result.storeCommitted = true;
        result.storeMs = storeElapsed.elapsed();
        result.obsolete = requestedGeneration->load(std::memory_order_acquire) != generation;
        return result;
    }));
}

void SourceChangeTracker::finishAsyncReconciliation(const AsyncResult &result)
{
    QElapsedTimer finishElapsed;
    finishElapsed.start();
    m_reconciliationInProgress = false;
    const quint64 currentGeneration = m_requestedGeneration->load(std::memory_order_acquire);
    if (result.obsolete || result.generation != currentGeneration) {
        m_reconciliationQueued = false;
        m_debounceTimer->stop();
        startAsyncReconciliation();
        return;
    }
    m_lastScanMs = result.scanMs;
    m_lastStoreMs = result.storeMs;
    if (m_firstScanMs < 0) m_firstScanMs = m_lastScanMs;
    if (m_firstStoreMs < 0 && m_lastStoreMs >= 0) m_firstStoreMs = m_lastStoreMs;
    m_lastObservedCount = result.scan.observed.size();
    m_lastHashedCount = result.scan.hashedPaths.size();
    m_lastCompletedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (!result.scan.success || !result.storeCommitted) {
        m_reconciliationQueued = false;
        m_settlementKnown = false;
        setAuthoritative(false);
        Q_EMIT reconciliationFailed(result.scan.error.isEmpty()
                                        ? QStringLiteral("source reconciliation did not commit")
                                        : result.scan.error);
        return;
    }
    for (const QString &path : result.scan.hashedPaths) {
        Q_EMIT fileHashed(path);
    }
    QString trackingError;
    const QList<DesktopTrackedPath> persistedTracking = m_store->trackingPaths(&trackingError);
    if (!trackingError.isEmpty()) {
        m_store->markReconciliationRequired(nullptr);
        m_settlementKnown = false;
        setAuthoritative(false);
        Q_EMIT reconciliationFailed(trackingError);
        return;
    }
    m_trackedPaths = persistedTracking;
    refreshBackgroundSources();
    m_candidateStates = result.scan.candidateStates;
    QString watcherError;
    if (!configureWatcher(&watcherError)) {
        m_store->markReconciliationRequired(nullptr);
        m_settlementKnown = false;
        setAuthoritative(false);
        Q_EMIT reconciliationFailed(watcherError);
        return;
    }
    recordSettlement(result.scan);
    setAuthoritative(!result.scan.waitingForStableCandidate
                     && !result.scan.discoveredDirectoryAdded
                     && m_candidateStates.isEmpty()
                     && !result.scan.unresolved);
    m_lastFinishMs = finishElapsed.elapsed();
    const bool queued = std::exchange(m_reconciliationQueued, false);
    if (result.scan.waitingForStableCandidate
        || result.scan.discoveredDirectoryAdded
        || !m_candidateStates.isEmpty()
        || queued) {
        m_requestedGeneration->fetch_add(1, std::memory_order_acq_rel);
        m_debounceTimer->start(std::max(1, m_debounceTimer->interval()));
    }
    Q_EMIT reconciliationFinished();
}

void SourceChangeTracker::refreshBackgroundSources()
{
    m_backgroundSources.clear();
    for (const DesktopTrackedPath &path : std::as_const(m_trackedPaths)) {
        if (isBackgroundSource(path)) {
            m_backgroundSources.append(path);
        }
    }
}

void SourceChangeTracker::startMetadataProbe(const QList<DesktopTrackedPath> &paths)
{
    if (paths.isEmpty() || m_metadataProbeInProgress) {
        return;
    }
    const QString rootPath = m_rootPath;
    const quint64 generation = m_requestedGeneration->load(std::memory_order_acquire);
    m_metadataProbeInProgress = true;
    auto *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this, [this, watcher, generation] {
        const bool changed = watcher->result();
        watcher->deleteLater();
        m_metadataProbeInProgress = false;
        if (changed && generation == m_requestedGeneration->load(std::memory_order_acquire)) {
            scheduleReconciliation();
        }
    });
    watcher->setFuture(QtConcurrent::run(sourceTrackerPool(), [rootPath, paths] {
        const QDir root(rootPath);
        for (const DesktopTrackedPath &tracked : paths) {
            const QString absolutePath = root.absoluteFilePath(tracked.path);
            const QFileInfo info(absolutePath);
            const bool directory = isDirectorySentinel(tracked);
            const bool exists = info.exists() && (directory ? info.isDir() : info.isFile());
            const bool metadataMatches = exists == tracked.exists
                && (!exists
                    || (tracked.modifiedAtMs == info.lastModified().toMSecsSinceEpoch()
                        && (directory || tracked.sizeBytes == info.size())
                        && tracked.metadata.value(QStringLiteral("_local_ctime_ms")).toInteger(-1) == info.metadataChangeTime().toMSecsSinceEpoch()
                        && tracked.metadata.value(QStringLiteral("_local_file_identity")).toString() == fileIdentity(absolutePath, info)));
            if (!metadataMatches) {
                return true;
            }
        }
        return false;
    }));
}

std::optional<DesktopTrackedPath> SourceChangeTracker::observedPath(const QString &rootPath,
                                                                    const DesktopTrackedPath &tracked,
                                                                    const QString &hashExecutable,
                                                                    int hashTimeoutMs,
                                                                    QString *error,
                                                                    QStringList *hashedPaths,
                                                                    const std::shared_ptr<std::atomic<quint64>> &requestedGeneration,
                                                                    quint64 generation)
{
    if (reconciliationCancelled(requestedGeneration, generation)) {
        if (error) {
            *error = QStringLiteral("source reconciliation was cancelled");
        }
        return std::nullopt;
    }
    DesktopTrackedPath observed = tracked;
    observed.metadata.remove(QStringLiteral("_local_ctime_ms"));
    observed.metadata.remove(QStringLiteral("_local_file_identity"));
    const QString absolutePath = QDir(rootPath).absoluteFilePath(tracked.path);
    const QFileInfo info(absolutePath);
    const bool directory = isDirectorySentinel(tracked);
    observed.exists = info.exists() && (directory ? info.isDir() : info.isFile());
    observed.sha256.clear();
    observed.sizeBytes = -1;
    observed.modifiedAtMs = -1;
    if (!observed.exists) {
        return observed;
    }
    observed.modifiedAtMs = info.lastModified().toMSecsSinceEpoch();
    const qint64 changedAtMs = info.metadataChangeTime().toMSecsSinceEpoch();
    const QString identity = fileIdentity(absolutePath, info);
    observed.metadata.insert(QStringLiteral("_local_ctime_ms"), changedAtMs);
    observed.metadata.insert(QStringLiteral("_local_file_identity"), identity);
    if (directory) {
        return observed;
    }
    observed.sizeBytes = info.size();
    const qint64 trackedChangedAtMs = tracked.metadata.value(QStringLiteral("_local_ctime_ms")).toInteger(-1);
    const QString trackedIdentity = tracked.metadata.value(QStringLiteral("_local_file_identity")).toString();
    if (tracked.exists
        && tracked.sizeBytes == observed.sizeBytes
        && tracked.modifiedAtMs == observed.modifiedAtMs
        && trackedChangedAtMs == changedAtMs
        && !trackedIdentity.isEmpty()
        && trackedIdentity == identity
        && validSha256(tracked.sha256)) {
        observed.sha256 = tracked.sha256;
        return observed;
    }
    const std::optional<QString> digest = hashFileWithHelper(hashExecutable,
                                                             absolutePath,
                                                             tracked.path,
                                                             hashTimeoutMs,
                                                             error,
                                                             requestedGeneration,
                                                             generation);
    if (!digest) {
        return std::nullopt;
    }
    observed.sha256 = *digest;
    if (hashedPaths) {
        hashedPaths->append(tracked.path);
    }
    return observed;
}

bool SourceChangeTracker::configureWatcher(QString *error)
{
    if (error) {
        error->clear();
    }
    // The shared recursive stream already covers the root. Passive detection
    // follows document text sources and their directories (atomic saves
    // replace the file); an explicit Sync still reconciles the complete
    // inventory, so regenerated assets never start background scans.
    const QDir root(m_rootPath);
    m_interestingPaths.clear();
    m_interestingDirectories.clear();
    QSet<QString> lexicalDirectories;
    for (const DesktopTrackedPath &tracked : std::as_const(m_backgroundSources)) {
        const QString absolutePath = QDir::cleanPath(root.absoluteFilePath(tracked.path));
        m_interestingPaths.insert(absolutePath);
        lexicalDirectories.insert(QFileInfo(absolutePath).absolutePath());
        m_interestingDirectories.insert(QFileInfo(absolutePath).absolutePath());
        // Reported paths are resolved, so also match sources reached through
        // a symbolic link.
        const QString canonicalDirectory = QFileInfo(QFileInfo(absolutePath).absolutePath()).canonicalFilePath();
        if (!canonicalDirectory.isEmpty()) {
            m_interestingDirectories.insert(canonicalDirectory);
            m_interestingPaths.insert(canonicalDirectory + QLatin1Char('/') + QFileInfo(absolutePath).fileName());
        }
    }
    m_watchedDirectoryCount = lexicalDirectories.size();
    m_sortedInterestingPaths = QStringList(m_interestingPaths.cbegin(), m_interestingPaths.cend());
    std::sort(m_sortedInterestingPaths.begin(), m_sortedInterestingPaths.end());
    if (m_backgroundSources.isEmpty()) {
        m_integrityAuditIndex = 0;
        m_integrityAuditTimer->stop();
    } else {
        if (m_integrityAuditIndex >= m_backgroundSources.size()) {
            m_integrityAuditIndex = 0;
        }
        m_integrityAuditTimer->start();
    }
    return true;
}

bool SourceChangeTracker::isRelevantChange(const QString &absolutePath, bool entryChanged) const
{
    if (m_interestingPaths.contains(absolutePath)) {
        return true;
    }
    // An entry created, removed or renamed next to a source (a new chapter or
    // folder, an atomic save) may extend the source closure. Edits to other
    // siblings, such as build outputs written in place, do not.
    if (!entryChanged) {
        return false;
    }
    if (m_interestingDirectories.contains(QFileInfo(absolutePath).absolutePath())) {
        return true;
    }
    // A renamed or removed directory can hide sources beneath it.
    const QString prefix = absolutePath + QLatin1Char('/');
    const auto below = std::lower_bound(m_sortedInterestingPaths.cbegin(), m_sortedInterestingPaths.cend(), prefix);
    return below != m_sortedInterestingPaths.cend() && below->startsWith(prefix);
}

void SourceChangeTracker::recordSettlement(const ScanResult &scan)
{
    m_unsettledPaths = scan.unresolvedPaths + scan.addedDirectories + m_candidateStates.keys();
    m_settlementKnown = true;
}

bool SourceChangeTracker::sourceSettled(const QString &project) const
{
    if (m_authoritative) return true;
    if (!m_settlementKnown || project.isEmpty()) return false;
    for (const QString &path : m_unsettledPaths) {
        // A path outside every source folder (a shared style, say) may feed any project.
        const QString owner = SourceOwner::of(path);
        if (owner.isEmpty() || owner == project) return false;
    }
    return true;
}

void SourceChangeTracker::setAuthoritative(bool authoritative)
{
    if (m_authoritative == authoritative) {
        return;
    }
    m_authoritative = authoritative;
    Q_EMIT authoritativeChanged(authoritative);
}

bool waitForSourceTrackerWork(int timeoutMs)
{
    return sourceTrackerPool()->waitForDone(timeoutMs);
}

}
