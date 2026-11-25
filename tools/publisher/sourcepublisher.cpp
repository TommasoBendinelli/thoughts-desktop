// SPDX-License-Identifier: GPL-2.0-or-later
#include "sourcepublisher.h"
#include "proposalclaims.h"

#include "pyio.h"
#include "pyops.h"
#include "pystr.h"
#include "serverids.h"
#include "sha256.h"
#include "sourceinventory.h"
#include "sourceowner.h"
#include "../../part/sourcefoldernames.h"
#include "sourcemerge.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>
#include <chrono>
#include <ctime>

#ifdef Q_OS_WIN
#include <cwchar>
#endif

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

QString newUuid()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

// datetime.now(UTC).isoformat().replace("+00:00", "Z")
QString utcTimestamp()
{
    const auto now = std::chrono::system_clock::now();
    const qint64 micros = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    const QDateTime time = QDateTime::fromMSecsSinceEpoch(micros / 1000, QTimeZone::UTC);
    QString text = time.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    const qint64 fraction = micros % 1000000;
    if (fraction != 0) {
        text += QStringLiteral(".%1").arg(fraction, 6, 10, QLatin1Char('0'));
    }
    return text + QLatin1Char('Z');
}

// Fedora stores device sequences in PostgreSQL INTEGER columns.
constexpr qint64 maximumDeviceSequence = 2147483647;
// The sources sent alone to /v1/sources/apply, by path, while Fedora's answer is
// outstanding: a retry after a lost answer repeats the same operation.
const QString pendingAppliesKey = QStringLiteral("pending_source_applies");
// Earlier bases of a path the Desktop's own record (desktop_source_baselines)
// may still name, newest last: see recordSupersededBase.
const QString supersededBasesKey = QStringLiteral("superseded_bases");
constexpr qsizetype maximumSupersededBases = 4;
// Renumbering is bounded: a writer that keeps taking this device's next sequence
// (another tool using the publisher's token) must not make Sync loop forever.
constexpr int maximumSequenceRecoveries = 3;

QString homeDirectory()
{
#ifdef Q_OS_WIN
    QString home = qEnvironmentVariable("USERPROFILE");
    if (home.isEmpty()) {
        home = qEnvironmentVariable("HOMEDRIVE") + qEnvironmentVariable("HOMEPATH");
    }
#else
    QString home = qEnvironmentVariable("HOME");
    if (home.isEmpty()) {
        home = QDir::homePath();
    }
#endif
    return Py::Path(home).str();
}

// PurePosixPath(path).as_posix() for a relative path, or nothing when unsafe.
std::optional<QString> safePath(const std::optional<QString> &path)
{
    if (!path || path->isEmpty() || path->startsWith(QLatin1Char('/'))) {
        return std::nullopt;
    }
    QStringList parts;
    for (const QString &part : path->split(QLatin1Char('/'))) {
        if (part == QLatin1String("..")) {
            return std::nullopt;
        }
        if (!part.isEmpty() && part != QLatin1String(".")) {
            parts.append(part);
        }
    }
    const QString posix = parts.isEmpty() ? QStringLiteral(".") : parts.join(QLatin1Char('/'));
    return Py::leftCodePoints(posix, 512);
}

const Json &field(const Json &object, const char *key)
{
    return object.value(QLatin1String(key));
}

Json baseOf(const Json &item)
{
    return Json::object({{QStringLiteral("revision"), Py::subscript(item, QStringLiteral("revision"))},
                         {QStringLiteral("sha256"), Py::subscript(item, QStringLiteral("sha256"))},
                         {QStringLiteral("object_sha256"), Py::subscript(item, QStringLiteral("object_sha256"))}});
}

// The heads a publication's results left their files on: applied, or Fedora's merge.
QHash<QString, Json> publishedHeads(const Json &result)
{
    QHash<QString, Json> heads;
    for (const Json &item : Py::iterate(Py::get(result, QStringLiteral("results"), Json::array()))) {
        if (!item.isObject() || !item.value(QStringLiteral("path")).isString()) {
            continue;
        }
        const Json outcome = item.value(QStringLiteral("outcome"));
        const Json revision = item.value(QStringLiteral("revision"));
        if ((outcome != Json(QStringLiteral("applied")) && outcome != Json(QStringLiteral("merged")))
            || (revision.type() != Json::Type::Int && revision.type() != Json::Type::BigInt) || !item.value(QStringLiteral("sha256")).isString()
            || !item.value(QStringLiteral("object_sha256")).isString()) {
            continue;
        }
        heads.insert(item.value(QStringLiteral("path")).toString(), baseOf(item));
    }
    return heads;
}

qsizetype localIndex(const SourcePublisher::LocalSources &local, const QString &path)
{
    for (qsizetype index = 0; index < local.size(); ++index) {
        if (local.at(index).first == path) {
            return index;
        }
    }
    return -1;
}

QString pathRepr(const Json &path)
{
    return path.isString() ? Py::repr(path.toString()) : Py::pythonStr(path);
}

bool isUnsafeSourcePath(const QString &path)
{
    return path.startsWith(QLatin1Char('/')) || path.split(QLatin1Char('/')).contains(QStringLiteral("..")) || path.contains(QLatin1Char('\\'));
}

void requireUtf8(const QByteArray &content)
{
    QString error;
    if (!Py::decodeUtf8(content, &error)) {
        Py::raise(QStringLiteral("UnicodeDecodeError"), error);
    }
}
}

QString sha256Of(const QByteArray &content)
{
    return sha256Hex(content);
}

QString formatEventBytes(qint64 size)
{
    char buffer[64];
    if (size < 1000) {
        return QStringLiteral("%1 B").arg(size);
    }
    if (size < 1000000) {
        std::snprintf(buffer, sizeof(buffer), "%.1f kB", double(size) / 1000);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.1f MB", double(size) / 1000000);
    }
    return QString::fromLatin1(buffer);
}

QString collapseWhitespace(const QString &text)
{
    QStringList words;
    QString current;
    for (const char32_t codePoint : Py::toCodePoints(text)) {
        if (Py::isSpace(codePoint)) {
            if (!current.isEmpty()) {
                words.append(current);
                current.clear();
            }
        } else {
            current += Py::fromCodePoints(std::u32string(1, codePoint));
        }
    }
    if (!current.isEmpty()) {
        words.append(current);
    }
    return words.join(QLatin1Char(' '));
}

QString stableErrorCode(const Py::Exception &error, const QString &phase, const QString &operation)
{
    Q_UNUSED(phase);
    const QString detail = Py::lower(error.message);
    if (detail.contains(QLatin1String("timed out")) || detail.contains(QLatin1String("timeout"))) {
        if (operation == QLatin1String("build_input")) {
            return QStringLiteral("build_input_upload_timeout");
        }
        if (operation == QLatin1String("source")) {
            return QStringLiteral("source_upload_timeout");
        }
        return QStringLiteral("network_timeout");
    }
    if (error.type == Py::apiErrorType) {
        return error.status > 0 ? QStringLiteral("http_%1").arg(error.status) : QStringLiteral("network_error");
    }
    if (Py::isValueError(error)) {
        return QStringLiteral("invalid_source_publication");
    }
    return QStringLiteral("source_publication_failed");
}

ProgressReporter::ProgressReporter(const QString &target, const std::optional<Py::Path> &root)
    : m_target(target)
{
    if (root) {
        m_root = Py::resolve(*root).str();
    }
}

QString ProgressReporter::safeMessage(const QString &message) const
{
    QString value = collapseWhitespace(message);
    if (m_root) {
        value.replace(*m_root, QStringLiteral("."));
    }
    value.replace(homeDirectory(), QStringLiteral("~"));
    return Py::leftCodePoints(value, 600);
}

void ProgressReporter::report(const QString &message, const ProgressFields &fields)
{
    if (fields.phase && !fields.phase->isEmpty()) {
        m_phase = *fields.phase;
    }
    const QString safe = safeMessage(message);
    const qint64 milliseconds = QDateTime::currentMSecsSinceEpoch();
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(milliseconds);
    const std::time_t seconds = std::time_t(milliseconds / 1000);
    QString zone;
#ifdef Q_OS_WIN
    std::tm parts{};
    localtime_s(&parts, &seconds);
    wchar_t wideZone[128] = {};
    std::wcsftime(wideZone, 128, L"%Z", &parts);
    zone = QString::fromWCharArray(wideZone);
#else
    std::tm parts{};
    localtime_r(&seconds, &parts);
    char narrowZone[128] = {};
    std::strftime(narrowZone, sizeof(narrowZone), "%Z", &parts);
    zone = QString::fromLocal8Bit(narrowZone);
#endif
    Py::printError(QStringLiteral("[%1 %2] %3").arg(local.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")), zone, safe));
    const int offset = local.offsetFromUtc();
    const int offsetMinutes = std::abs(offset) / 60;
    const QString at = local.toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss.zzz"))
        + QStringLiteral("%1%2:%3").arg(offset < 0 ? QLatin1Char('-') : QLatin1Char('+')).arg(offsetMinutes / 60, 2, 10, QLatin1Char('0')).arg(offsetMinutes % 60, 2, 10, QLatin1Char('0'));
    Json event = Json::object({{QStringLiteral("schema_version"), Json(1)},
                               {QStringLiteral("type"), Json(QStringLiteral("thoughts_sync_event"))},
                               {QStringLiteral("at"), Json(at)},
                               {QStringLiteral("phase"), Json(m_phase)},
                               {QStringLiteral("state"), Json(fields.state)},
                               {QStringLiteral("message"), Json(safe)}});
    if (!m_target.isEmpty()) {
        event.insert(QStringLiteral("target"), Json(m_target));
    }
    if (const std::optional<QString> path = safePath(fields.path)) {
        event.insert(QStringLiteral("path"), Json(*path));
    }
    const auto nonNegative = [](qint64 value) {
        return Json(qMax<qint64>(0, value));
    };
    if (fields.transferredBytes) {
        event.insert(QStringLiteral("bytes_transferred"), nonNegative(*fields.transferredBytes));
    }
    if (fields.totalBytes) {
        event.insert(QStringLiteral("total_bytes"), nonNegative(*fields.totalBytes));
    }
    if (fields.project && !fields.project->isEmpty()
        && ((Py::Path(*fields.project).name() == *fields.project && fields.project->startsWith(QLatin1String("docs-")))
            || SourceOwner::isDefaultFolder(*fields.project))) {
        event.insert(QStringLiteral("project"), Json(Py::leftCodePoints(*fields.project, 255)));
    }
    if (fields.selectedFiles) {
        event.insert(QStringLiteral("selected_files"), nonNegative(*fields.selectedFiles));
    }
    if (fields.errorCode && !fields.errorCode->isEmpty()) {
        event.insert(QStringLiteral("error_code"), Json(*fields.errorCode));
    }
    if (fields.uploadedBytes) {
        event.insert(QStringLiteral("uploaded_bytes"), nonNegative(*fields.uploadedBytes));
    }
    if (fields.downloadedBytes) {
        event.insert(QStringLiteral("downloaded_bytes"), nonNegative(*fields.downloadedBytes));
    }
    if (fields.uploadTotalBytes) {
        event.insert(QStringLiteral("upload_total_bytes"), nonNegative(*fields.uploadTotalBytes));
    }
    if (fields.downloadTotalBytes) {
        event.insert(QStringLiteral("download_total_bytes"), nonNegative(*fields.downloadTotalBytes));
    }
    if (fields.state == QLatin1String("failed")) {
        m_failureEmitted = true;
    }
    Py::DumpOptions options;
    options.sortKeys = true;
    options.separators = QPair<QString, QString>(QStringLiteral(","), QStringLiteral(":"));
    Py::printError(QByteArrayLiteral("THOUGHTS_SYNC_EVENT ") + Py::dumpsChecked(event, options));
}

void ProgressReporter::failure(const Py::Exception &error)
{
    const QString code = stableErrorCode(error, m_phase);
    QString detail = safeMessage(error.message);
    if (detail.isEmpty()) {
        detail = Py::className(error);
    }
    while (detail.endsWith(QLatin1Char('.'))) {
        detail.chop(1);
    }
    const QString note = !m_target.isEmpty() && m_phase != QLatin1String("building") ? QStringLiteral(" The server build was not started.") : QString();
    ProgressFields fields;
    fields.state = QStringLiteral("failed");
    fields.errorCode = code;
    report(QStringLiteral("Source publication failed: %1.%2").arg(detail, note), fields);
}

SourceHeads::SourceHeads(const QList<Json> &items)
{
    for (const Json &item : items) {
        const Json path = Py::subscript(item, QStringLiteral("path"));
        const QString key = Py::hashKey(path, "dict key");
        if (!m_items.contains(key)) {
            m_order.append(key);
        }
        m_items.insert(key, {path, item});
    }
}

std::optional<Json> SourceHeads::get(const QString &path) const
{
    const auto found = m_items.constFind(Py::hashKey(Json(path)));
    if (found == m_items.constEnd()) {
        return std::nullopt;
    }
    return found->second;
}

QList<QPair<Json, Json>> SourceHeads::items() const
{
    QList<QPair<Json, Json>> result;
    for (const QString &key : m_order) {
        result.append(m_items.value(key));
    }
    return result;
}

SourcePublisher::SourcePublisher(const Py::Path &rootPath, const Py::Path &manifest, const Py::Path &statePathArgument, SourceApi &sourceApi,
                                 const std::optional<Py::Path> &bootstrapState, const std::optional<QSet<QString>> &includePaths,
                                 const std::optional<QString> &scopeProject, const FrozenInputs *frozen, ProgressCallback progress)
    : root(Py::resolve(rootPath))
    , manifestPath(Py::resolve(manifest))
    , statePath(Py::resolve(statePathArgument))
    , api(sourceApi)
    , m_includePaths(includePaths)
    , m_scopeProject(scopeProject)
    , m_frozen(frozen)
    , m_progress(std::move(progress))
{
    if (bootstrapState) {
        m_bootstrapStatePath = Py::resolve(*bootstrapState);
    }
    state = readState();
    if (m_frozen) {
        const QStringList frozenList = m_frozen->sourcePaths();
        const QSet<QString> frozenPaths(frozenList.cbegin(), frozenList.cend());
        if (m_includePaths && !(*m_includePaths - frozenPaths).isEmpty()) {
            Py::raiseValueError(QStringLiteral("requested source paths have no frozen identity"));
        }
        if (!m_includePaths) {
            m_includePaths = frozenPaths;
        }
        QSet<QString> manifestPaths;
        for (const Json &path : manifestPayloadPaths(readManifest())) {
            if (path.isString()) {
                manifestPaths.insert(path.toString());
            }
        }
        QSet<QString> recorded;
        for (const Json &path : Py::iterate(Py::get(state, QStringLiteral("bases"), Json::object()))) {
            if (path.isString()) {
                recorded.insert(path.toString());
            }
        }
        m_frozen->validateSources(manifestPaths, recorded, m_scopeProject);
    }
}

void SourcePublisher::report(const QString &message, const ProgressFields &fields)
{
    if (m_progress) {
        m_progress(message, fields);
    }
}

Json SourcePublisher::readState() const
{
    if (!Py::isFile(statePath)) {
        Json fresh = Json::object({{QStringLiteral("schema_version"), Json(1)},
                                   {QStringLiteral("next_device_sequence"), Json(1)},
                                   {QStringLiteral("bases"), Json::object()}});
        if (!m_bootstrapStatePath || !Py::isFile(*m_bootstrapStatePath)) {
            return fresh;
        }
        const Json legacy = Py::loadsText(Py::readText(*m_bootstrapStatePath));
        const Json files = legacy.isObject() ? legacy.value(QStringLiteral("files")) : Json();
        if (!legacy.isObject() || legacy.value(QStringLiteral("schema_version")) != Json(1) || !files.isObject()) {
            Py::raiseValueError(QStringLiteral("source-publication bootstrap state has an unsupported schema"));
        }
        const Json sequence = legacy.contains(QStringLiteral("next_device_sequence")) ? legacy.value(QStringLiteral("next_device_sequence")) : Json(1);
        fresh.insert(QStringLiteral("next_device_sequence"), Json(qMax<qint64>(1, Py::toPythonInt(sequence))));
        Json bases = Json::object();
        for (const auto &[path, value] : files.members()) {
            if (value.isObject() && value.contains(QStringLiteral("revision")) && value.contains(QStringLiteral("sha256"))
                && value.contains(QStringLiteral("object_sha256"))) {
                bases.insert(path, baseOf(value));
            }
        }
        fresh.insert(QStringLiteral("bases"), bases);
        return fresh;
    }
    Json value = Py::loadsText(Py::readText(statePath));
    if (Py::get(value, QStringLiteral("schema_version")) != Json(1)) {
        Py::raiseValueError(QStringLiteral("source-publication state has an unsupported schema"));
    }
    if (!value.contains(QStringLiteral("next_device_sequence"))) {
        value.insert(QStringLiteral("next_device_sequence"), Json(1));
    }
    if (!value.contains(QStringLiteral("bases"))) {
        value.insert(QStringLiteral("bases"), Json::object());
    }
    followSourceFolderRenames(value);
    return value;
}

// Source folder names spec: Fedora moved docs-<old>/... to docs-<new>/...
// before this computer renamed its folders. A base still recorded under an
// old folder is Fedora's own move, never a deletion to publish: it follows
// the folder (or yields to the base the migration already wrote). While the
// local rename is unfinished nothing is published at all.
void SourcePublisher::followSourceFolderRenames(Json &value) const
{
    Thoughts::Publisher::followSourceFolderRenames(statePath, value);
}

void followSourceFolderRenames(const Py::Path &statePath, Py::Json &value)
{
    const Py::Path journalPath = statePath.parent() / QStringLiteral("source-folder-names-migration.json");
    if (!Py::isFile(journalPath)) {
        return;
    }
    QFile file(journalPath.str());
    if (!file.open(QIODevice::ReadOnly)) {
        Py::raiseValueError(QStringLiteral("the source folder rename journal could not be read"));
    }
    const QJsonObject journal = QJsonDocument::fromJson(file.readAll()).object();
    const SourceFolderRenames renames = SourceFolderNames::fromJson(journal.value(QStringLiteral("renames")).toArray());
    if (renames.isEmpty()) {
        return;
    }
    if (journal.value(QStringLiteral("phase")).toString() != QLatin1String("done")) {
        Py::raiseValueError(QStringLiteral("the source folder rename on this computer is unfinished; Sync renames the folders before publishing"));
    }
    Json bases = value.value(QStringLiteral("bases"));
    if (!bases.isObject()) {
        return;
    }
    bool changed = false;
    for (const QString &path : bases.keys()) {
        const QString renamed = SourceFolderNames::rewrite(path, renames);
        if (renamed == path) {
            continue;
        }
        if (!bases.contains(renamed)) {
            bases.insert(renamed, bases.value(path));
        }
        bases.remove(path);
        changed = true;
    }
    if (changed) {
        value.insert(QStringLiteral("bases"), bases);
    }
}

void SourcePublisher::writeState()
{
    Py::atomicWrite(statePath, Py::dumpsFile(state, true));
}

SourceConflictListings &SourcePublisher::conflictListings()
{
    if (!m_conflictListings) {
        m_conflictListings.emplace(api);
    }
    return *m_conflictListings;
}

Json SourcePublisher::readManifest() const
{
    return Py::loadsText(Py::readText(manifestPath));
}

QStringList SourcePublisher::paths() const
{
    const Json value = readManifest();
    if (!value.isObject()) {
        Py::raiseValueError(QStringLiteral("source manifest has an unsupported schema"));
    }
    QList<Json> values = manifestPayloadPaths(value);
    QSet<QString> seen;
    for (const Json &path : values) {
        seen.insert(Py::hashKey(path));
    }
    if (m_frozen) {
        for (const QString &path : m_frozen->sourcePaths()) {
            if (!seen.contains(Py::hashKey(Json(path)))) {
                seen.insert(Py::hashKey(Json(path)));
                values.append(Json(path));
            }
        }
    }
    for (const Json &path : std::as_const(values)) {
        if (!path.isString()) {
            Py::raiseValueError(QStringLiteral("unsafe or duplicate source-manifest path: ") + pathRepr(path));
        }
    }
    QStringList output;
    for (const Json &path : std::as_const(values)) {
        output.append(path.toString());
    }
    std::sort(output.begin(), output.end(), Py::codePointLess);
    for (const QString &path : std::as_const(output)) {
        if (isUnsafeSourcePath(path)) {
            Py::raiseValueError(QStringLiteral("unsafe or duplicate source-manifest path: ") + Py::repr(path));
        }
    }
    if (!m_includePaths) {
        return output;
    }
    QStringList include(m_includePaths->cbegin(), m_includePaths->cend());
    for (const QString &path : std::as_const(include)) {
        if (isUnsafeSourcePath(path)) {
            Py::raiseValueError(QStringLiteral("unsafe requested source path: ") + Py::repr(path));
        }
    }
    const QSet<QString> outputSet(output.cbegin(), output.cend());
    QSet<QString> recorded;
    for (const Json &path : Py::iterate(Py::get(state, QStringLiteral("bases"), Json::object()))) {
        recorded.insert(path.toString());
    }
    const QSet<QString> missing = *m_includePaths - outputSet;
    QStringList untracked;
    for (const QString &path : missing) {
        if (!recorded.contains(path)) {
            untracked.append(path);
        }
    }
    if (!untracked.isEmpty()) {
        std::sort(untracked.begin(), untracked.end(), Py::codePointLess);
        Py::raiseValueError(QStringLiteral("requested source paths are absent from the manifest and recorded bases: ") + untracked.join(QStringLiteral(", ")));
    }
    QStringList selected;
    for (const QString &path : std::as_const(output)) {
        if (m_includePaths->contains(path)) {
            selected.append(path);
        }
    }
    QStringList sortedMissing(missing.cbegin(), missing.cend());
    std::sort(sortedMissing.begin(), sortedMissing.end(), Py::codePointLess);
    for (const QString &path : std::as_const(sortedMissing)) {
        if (!selected.contains(path)) {
            selected.append(path);
        }
    }
    if (m_scopeProject && !m_frozen) {
        const QString prefix = *m_scopeProject + QLatin1Char('/');
        QStringList bases(recorded.cbegin(), recorded.cend());
        std::sort(bases.begin(), bases.end(), Py::codePointLess);
        for (const QString &path : std::as_const(bases)) {
            if (path.startsWith(prefix) && !selected.contains(path) && !outputSet.contains(path)) {
                selected.append(path);
            }
        }
    }
    return selected;
}

void SourcePublisher::checkManifestRetirement(const QString &path) const
{
    if (isManifestPath(path) && Py::isFile(root / path)) {
        Py::raiseValueError(QStringLiteral("existing Project manifest is absent from the source inventory; refresh the inventory before publishing: ") + path);
    }
}

SourcePublisher::LocalSources SourcePublisher::localSources()
{
    QSet<QString> manifestPaths;
    for (const Json &path : manifestPayloadPaths(readManifest())) {
        if (path.isString()) {
            manifestPaths.insert(path.toString());
        }
    }
    LocalSources sources;
    for (const QString &path : paths()) {
        if (m_frozen) {
            if (const FrozenEntry *entry = m_frozen->source(path)) {
                sources.append({path, entry->action == QLatin1String("upsert") ? entry->content : std::nullopt});
                continue;
            }
        }
        if (m_includePaths && !manifestPaths.contains(path)) {
            checkManifestRetirement(path);
            sources.append({path, std::nullopt});
            continue;
        }
        const Py::Path target = Py::resolve(root / path);
        if (!target.isRelativeTo(root)) {
            Py::raiseValueError(QStringLiteral("managed source escapes the local root: ") + path);
        }
        if (!Py::isFile(target)) {
            sources.append({path, std::nullopt});
            continue;
        }
        const QByteArray content = Py::readBytes(target);
        requireUtf8(content);
        if (content.size() > maximumManagedSourceBytes) {
            Py::raiseValueError(QStringLiteral("managed source exceeds the size limit: ") + path);
        }
        sources.append({path, content});
    }
    return sources;
}

void SourcePublisher::activateCleanRemote(const QString &path, const QByteArray &content)
{
    requireUtf8(content);
    const Py::Path destination = Py::resolve(root / path);
    if (!destination.isRelativeTo(root)) {
        Py::raiseValueError(QStringLiteral("source path escapes the local root"));
    }
    Py::atomicWrite(destination, content);
}

bool SourcePublisher::activateFrozenRemoteSha(const QString &path, const QString &frozenSha256, const QByteArray &canonical, bool requireUnchanged)
{
    requireUtf8(canonical);
    const Py::Path destination = Py::resolve(root / path);
    const QString changed = QStringLiteral("managed source changed after Desktop froze it; nothing was activated: ") + path;
    if (!destination.isRelativeTo(root) || !Py::isFile(destination)) {
        if (requireUnchanged) {
            Py::raiseValueError(changed);
        }
        return false;
    }
    QByteArray live;
    try {
        live = Py::readBytes(destination);
    } catch (const Py::Exception &error) {
        if (!Py::isOsError(error)) {
            throw;
        }
        if (requireUnchanged) {
            Py::raiseValueError(QStringLiteral("managed source could not be verified before remote activation: ") + path);
        }
        return false;
    }
    const QString liveSha256 = sha256Hex(live);
    if (liveSha256 == sha256Hex(canonical)) {
        return true;
    }
    if (liveSha256 != frozenSha256) {
        if (requireUnchanged) {
            Py::raiseValueError(changed);
        }
        return false;
    }
    Py::atomicWrite(destination, canonical);
    return true;
}

void SourcePublisher::activatePublicationResults(const Json &result, const QHash<QString, QString> &frozenSha256ByPath)
{
    for (const Json &accepted : Py::iterate(Py::get(result, QStringLiteral("results"), Json::array()))) {
        const Json path = Py::get(accepted, QStringLiteral("path"));
        Py::hashKey(path, "dict key");
        if (!path.isString() || !frozenSha256ByPath.contains(path.toString())) {
            continue;
        }
        const QString frozenSha256 = frozenSha256ByPath.value(path.toString());
        const Json outcome = Py::get(accepted, QStringLiteral("outcome"));
        Py::hashKey(outcome);
        if (outcome != Json(QStringLiteral("applied")) && outcome != Json(QStringLiteral("merged"))) {
            continue;
        }
        activateMergedResult(path.toString(), frozenSha256, Py::get(accepted, QStringLiteral("sha256")), Py::get(accepted, QStringLiteral("object_sha256")));
    }
}

bool SourcePublisher::activateMergedResult(const QString &path, const QString &frozenSha256, const Json &canonicalSha256, const Json &canonicalObject,
                                           std::optional<QByteArray> *installed)
{
    if (canonicalSha256 == Json(frozenSha256)) {
        return true;
    }
    if (!canonicalSha256.isString() || !canonicalObject.isString()) {
        Py::raiseValueError(QStringLiteral("the server returned an invalid merged source identity: ") + path);
    }
    // Fedora's result replaces the bytes Desktop sent: until Desktop records
    // it, its base for the path is those bytes or the base before them.
    const Json previous = Py::get(Py::get(state, QStringLiteral("bases"), Json::object()), path);
    if (previous.isObject() && previous.value(QStringLiteral("sha256")).isString()) {
        recordSupersededBase(path, previous.value(QStringLiteral("sha256")).toString());
    }
    recordSupersededBase(path, frozenSha256);
    const QByteArray canonical = api.download(canonicalObject.toString());
    if (sha256Hex(canonical) != canonicalSha256.toString()) {
        Py::raiseValueError(QStringLiteral("merged source from the server failed identity verification: ") + path);
    }
    if (!activateFrozenRemoteSha(path, frozenSha256, canonical, false)) {
        return false;
    }
    if (installed) {
        *installed = canonical;
    }
    return true;
}

// The installer and the publisher move a path's base in this state; Desktop
// records its own copy (desktop_source_baselines) afterwards, in another step
// that can fail (a busy database) or decline (the file changed meanwhile).
// Until it catches up, the frozen copies it sends name an earlier base. Those
// earlier bases are kept here, so a frozen copy written on one of them is
// merged against it (laggingFrozenBase) instead of being refused forever.
void SourcePublisher::recordSupersededBase(const QString &path, const QString &sha256)
{
    if (sha256.isEmpty() || sha256 == emptySha256) {
        return;
    }
    Json all = Py::get(state, supersededBasesKey, Json::object());
    if (!all.isObject()) {
        all = Json::object();
    }
    QStringList shas = supersededBases(path);
    shas.removeAll(sha256);
    shas.append(sha256);
    while (shas.size() > maximumSupersededBases) {
        shas.removeFirst();
    }
    Json list = Json::array();
    for (const QString &sha : std::as_const(shas)) {
        list.append(Json(sha));
    }
    all.insert(path, list);
    state.insert(supersededBasesKey, all);
}

QStringList SourcePublisher::supersededBases(const QString &path) const
{
    const Json all = state.value(supersededBasesKey);
    QStringList shas;
    if (!all.isObject()) {
        return shas;
    }
    const Json list = all.value(path);
    if (list.isArray()) {
        for (const Json &sha : list.items()) {
            if (sha.isString() && Py::isLowerHex(sha.toString(), 64)) {
                shas.append(sha.toString());
            }
        }
    }
    return shas;
}

void SourcePublisher::forgetSupersededBases(const QString &path)
{
    Json all = state.value(supersededBasesKey);
    if (!all.isObject() || !all.contains(path)) {
        return;
    }
    all.remove(path);
    if (all.size() > 0) {
        state.insert(supersededBasesKey, all);
    } else {
        state.remove(supersededBasesKey);
    }
}

Json SourcePublisher::laggingFrozenBase(const QString &path, const QByteArray &content, const Json &recordedBase, const QString &desktopBase)
{
    const QString refused = QStringLiteral("frozen Desktop source has no matching authoring or server base: ") + path;
    QByteArray earlier;
    try {
        earlier = api.download(desktopBase);
    } catch (const Py::Exception &error) {
        if (error.type != Py::apiErrorType || error.status != 404) {
            throw;
        }
        Py::raiseValueError(refused + QStringLiteral(" (the server no longer holds its earlier base)"));
    }
    if (sha256Hex(earlier) != desktopBase) {
        Py::raiseValueError(QStringLiteral("earlier source base failed identity verification: ") + path);
    }
    const QByteArray recorded = api.download(recordedBase.value(QStringLiteral("object_sha256")).toString());
    if (Json(sha256Hex(recorded)) != recordedBase.value(QStringLiteral("sha256"))) {
        Py::raiseValueError(QStringLiteral("recorded source base failed identity verification: ") + path);
    }
    const auto [merged, conflictCount] = mergeSources(earlier, recorded, content, path);
    if (!conflictCount && merged == content) {
        // Everything the recorded base changed since the earlier one is in the
        // file: it was written on the recorded base (the installer's merge or
        // install, or Fedora's result), so that is its base.
        report(QStringLiteral("Desktop still names an earlier base of %1; the file holds every change since, so it is published against revision %2.")
                   .arg(path, Py::pythonStr(recordedBase.value(QStringLiteral("revision")))));
        return recordedBase;
    }
    // The user accepted or rejected, here, proposals the recorded revision
    // brought (proposals of an agent that Fedora's merge installed, decided
    // before Desktop recorded the merge). The file holds the decisions
    // where the recorded base holds the wrappers, so the merge above clashes,
    // but its decision claims, made on those very wrappers, say the file was
    // written on the recorded base: it holds every change since, with them decided.
    if (const std::optional<QByteArray> decided = withClaimedDecisions(*this, path, recorded)) {
        const auto [again, againConflicts] = mergeSources(earlier, *decided, content, path);
        if (!againConflicts && again == content) {
            report(QStringLiteral("Desktop still names an earlier base of %1; the file holds every change since, with proposals decided here, so it is published "
                                  "against revision %2.")
                       .arg(path, Py::pythonStr(recordedBase.value(QStringLiteral("revision")))));
            return recordedBase;
        }
    }
    // The file may lack changes Fedora made since the earlier base (an editor
    // that saved its older copy over the installed one): merged against that
    // base, Fedora keeps them; published against the recorded one, it would
    // take them back.
    report(QStringLiteral("Desktop still names an earlier base of %1 and the file lacks changes made since; it is merged against that earlier base.").arg(path));
    return Json::object({{QStringLiteral("revision"), recordedBase.value(QStringLiteral("revision"))},
                         {QStringLiteral("sha256"), Json(desktopBase)},
                         {QStringLiteral("object_sha256"), Json(desktopBase)}});
}

void SourcePublisher::recordBases(const std::optional<Json> &givenSnapshot, const QHash<QString, Json> &held)
{
    const Json snapshot = givenSnapshot ? *givenSnapshot : api.sourceTree();
    const SourceHeads current(Py::iterate(Py::get(snapshot, QStringLiteral("sources"), Json::array())));
    Json bases = m_includePaths ? Py::get(state, QStringLiteral("bases"), Json::object()) : Json::object();
    for (const QString &path : paths()) {
        const std::optional<Json> item = current.get(path);
        const bool present = item && !Py::truthy(Py::get(*item, QStringLiteral("deleted"), Json(false)));
        const auto kept = held.constFind(path);
        if (kept != held.cend() && !(present && Py::get(*item, QStringLiteral("sha256")) == kept->value(QStringLiteral("sha256")))) {
            // Fedora changed the file again after this run saw it (a card move
            // settling on the proposal just accepted here, before the tree was
            // read back). The file still holds the held revision, so
            // that stays its base: the installer brings the later one in, where
            // recording it would have the next publication send the file back
            // over it.
            report(QStringLiteral("The server changed %1 again after this run (revision %2); the next Sync installs it.")
                       .arg(path, present ? Py::pythonStr(Py::get(*item, QStringLiteral("revision"))) : QStringLiteral("deleted")));
            bases.insert(path, *kept);
            continue;
        }
        if (present) {
            bases.insert(path, baseOf(*item));
        } else {
            bases.remove(path);
        }
    }
    state.insert(QStringLiteral("bases"), bases);
}

namespace
{
Py::Path sourceTreeCachePath(const Py::Path &statePath)
{
    return statePath.parent() / QStringLiteral("source-tree-cache.json");
}

// The frontier of a well-formed tree, or nothing.
std::optional<qint64> treeFrontier(const Json &tree)
{
    const Json frontier = Py::get(tree, QStringLiteral("source_frontier"));
    if (!tree.isObject() || frontier.type() != Json::Type::Int || !Py::get(tree, QStringLiteral("sources")).isArray()) {
        return std::nullopt;
    }
    return Py::toPythonInt(frontier);
}
}

Json SourcePublisher::currentSourceTree(const std::optional<qint64> &atLeast)
{
    const Py::Path cachePath = sourceTreeCachePath(statePath);
    try {
        if (Py::isFile(cachePath)) {
            const Json kept = Py::loadsBytes(Py::readBytes(cachePath));
            const Json tree = Py::get(kept, QStringLiteral("tree"));
            const std::optional<qint64> frontier = treeFrontier(tree);
            if (frontier && Py::get(kept, QStringLiteral("schema_version")) == Json(1) && Py::get(kept, QStringLiteral("server")) == Json(api.baseUrl())
                && (!atLeast || *frontier >= *atLeast)) {
                const Json serverFrontier = Py::get(api.syncState(), QStringLiteral("source_frontier"));
                if (serverFrontier.type() == Json::Type::Int && Py::toPythonInt(serverFrontier) == *frontier) {
                    return tree;
                }
            }
        }
    } catch (const Py::Exception &) {
        // An unreadable kept tree, or a server that cannot say its frontier: read the tree.
    }
    const Json tree = api.sourceTree();
    keepSourceTree(tree);
    return tree;
}

void SourcePublisher::keepSourceTree(const Json &tree)
{
    const Py::Path cachePath = sourceTreeCachePath(statePath);
    try {
        if (!treeFrontier(tree)) {
            if (Py::isFile(cachePath)) {
                Py::atomicWrite(cachePath, QByteArrayLiteral("{}\n"));
            }
            return;
        }
        Py::atomicWrite(cachePath, Py::dumpsChecked(Json::object({{QStringLiteral("schema_version"), Json(1)}, {QStringLiteral("server"), Json(api.baseUrl())}, {QStringLiteral("tree"), tree}})));
    } catch (const Py::Exception &) {
        // Only a saving: the next run reads the tree from the server.
    }
}

bool SourcePublisher::onlyOwnChanges(const Json &before, const Json &after, const QList<Json> &mutations, const Json &result)
{
    // Every head, as text: the tree's paths with revision, digests and deletion.
    const auto headText = [](const Json &revision, const Json &sha256, const Json &object, bool deleted) {
        return QStringLiteral("%1 %2 %3 %4").arg(Py::pythonStr(revision), Py::pythonStr(sha256), Py::pythonStr(object), deleted ? QStringLiteral("deleted") : QStringLiteral("present"));
    };
    const auto heads = [&headText](const Json &tree, QHash<QString, QString> *out) {
        const Json sources = Py::get(tree, QStringLiteral("sources"));
        if (!sources.isArray()) {
            return false;
        }
        for (const Json &item : sources.items()) {
            if (!item.isObject() || !item.value(QStringLiteral("path")).isString()) {
                return false;
            }
            out->insert(item.value(QStringLiteral("path")).toString(),
                        headText(item.value(QStringLiteral("revision")), item.value(QStringLiteral("sha256")), item.value(QStringLiteral("object_sha256")),
                                 Py::truthy(Py::get(item, QStringLiteral("deleted"), Json(false)))));
        }
        return true;
    };
    QHash<QString, QString> expected;
    QHash<QString, QString> actual;
    if (!heads(before, &expected) || !heads(after, &actual)) {
        return false;
    }
    QHash<QString, bool> deletions;
    for (const Json &mutation : mutations) {
        deletions.insert(Py::pythonStr(Py::get(mutation, QStringLiteral("path"))), Py::get(mutation, QStringLiteral("action")) == Json(QStringLiteral("delete")));
    }
    const Json results = Py::get(result, QStringLiteral("results"));
    if (!results.isArray() || results.size() != mutations.size()) {
        return false;
    }
    for (const Json &item : results.items()) {
        const Json outcome = Py::get(item, QStringLiteral("outcome"));
        const Json path = Py::get(item, QStringLiteral("path"));
        if (!path.isString() || !deletions.contains(path.toString())
            || !(outcome == Json(QStringLiteral("applied")) || outcome == Json(QStringLiteral("merged")) || outcome == Json(QStringLiteral("duplicate")))) {
            return false;
        }
        if (deletions.value(path.toString())) {
            // A deletion keeps the path; its head is compared by revision and deletion only.
            const QString head = actual.value(path.toString());
            if (!head.startsWith(Py::pythonStr(Py::get(item, QStringLiteral("revision"))) + QLatin1Char(' ')) || !head.endsWith(QLatin1String(" deleted"))) {
                return false;
            }
            expected.insert(path.toString(), head);
        } else {
            expected.insert(path.toString(),
                            headText(Py::get(item, QStringLiteral("revision")), Py::get(item, QStringLiteral("sha256")), Py::get(item, QStringLiteral("object_sha256")), false));
        }
    }
    return expected == actual;
}

QSet<QString> SourcePublisher::reconcilePreservedConflicts(LocalSources &local, const SourceHeads &current)
{
    Json conflicts = state.value(QStringLiteral("conflicts"));
    if (!conflicts.isObject() || conflicts.size() == 0) {
        return {};
    }
    if (!state.contains(QStringLiteral("bases"))) {
        state.insert(QStringLiteral("bases"), Json::object());
    }
    Json bases = state.value(QStringLiteral("bases"));
    bool changed = false;
    QSet<QString> resolvedPaths;
    std::optional<QSet<QString>> resolvedIds;
    const auto members = conflicts.members();
    for (const auto &[path, conflict] : members) {
        const qsizetype index = localIndex(local, path);
        if (index < 0) {
            continue;
        }
        const QString malformed = QStringLiteral("recorded Desktop source conflict is malformed: ") + path;
        if (!conflict.isObject()) {
            Py::raiseValueError(malformed);
        }
        Json conflictId = conflict.value(QStringLiteral("conflict_id"));
        Json serverRevision = conflict.value(QStringLiteral("server_revision"));
        const std::optional<Json> remote = current.get(path);
        const bool revisionIsInt = serverRevision.type() == Json::Type::Int || serverRevision.isBool();
        if (!conflictId.isString() || conflictId.toString().isEmpty() || !revisionIsInt || !remote
            || Py::truthy(Py::get(*remote, QStringLiteral("deleted"), Json(false)))) {
            Py::raiseValueError(malformed);
        }
        const QString waiting = QStringLiteral("source conflict is still waiting for resolution on the server: ") + path;
        if (Py::toPythonInt(Py::get(*remote, QStringLiteral("revision"), Json(-1))) <= Py::toPythonInt(serverRevision)) {
            Py::raiseValueError(waiting);
        }
        if (!resolvedIds) {
            resolvedIds = api.resolvedConflictIds();
        }
        Json record = conflict;
        if (!resolvedIds->contains(Py::hashKey(conflictId))) {
            // A resolution on any device that met a newer Fedora version
            // superseded the recorded conflict: its successor is the one to wait for.
            const std::optional<Json> successor = followSupersededConflict(conflictListings(), path, conflict);
            if (!successor) {
                Py::raiseValueError(waiting);
            }
            record = *successor;
            conflicts.insert(path, record);
            state.insert(QStringLiteral("conflicts"), conflicts);
            writeState();
            conflictId = record.value(QStringLiteral("conflict_id"));
            serverRevision = record.value(QStringLiteral("server_revision"));
            if (Py::toPythonInt(Py::get(*remote, QStringLiteral("revision"), Json(-1))) <= Py::toPythonInt(serverRevision)
                || !resolvedIds->contains(Py::hashKey(conflictId))) {
                Py::raiseValueError(waiting);
            }
        }
        const std::optional<QByteArray> live = local.at(index).second;
        const Json remoteSha = Py::subscript(*remote, QStringLiteral("sha256"));
        QSet<QString> accepted{Py::hashKey(remoteSha)};
        for (const QString &digest : acceptedLocalDigests(record)) {
            accepted.insert(Py::hashKey(Json(digest)));
        }
        if (!live || !accepted.contains(Py::hashKey(Json(sha256Hex(*live))))) {
            Py::raiseValueError(QStringLiteral("source changed locally after its conflict; preserve the later edit before resolving: ") + path);
        }
        // The id becomes a folder name below: refuse a malformed one before anything is fetched.
        requireConflictId(conflictId.toString());
        const QByteArray canonical = api.download(Py::pythonStr(Py::subscript(*remote, QStringLiteral("object_sha256"))));
        if (Json(sha256Hex(canonical)) != remoteSha) {
            Py::raiseValueError(QStringLiteral("resolved source from the server failed identity verification: ") + path);
        }
        Py::atomicWrite(conflictCopyPath(statePath.parent(), conflictId.toString(), path), *live);
        activateFrozenRemoteSha(path, sha256Hex(*live), canonical, true);
        local[index].second = canonical;
        bases.insert(path, baseOf(*remote));
        state.insert(QStringLiteral("bases"), bases);
        conflicts.remove(path);
        changed = true;
        resolvedPaths.insert(path);
    }
    if (changed) {
        state.insert(QStringLiteral("conflicts"), conflicts);
        writeState();
    }
    return resolvedPaths;
}

qint64 SourcePublisher::adoptRemoteBases(const QSet<QString> &adopted)
{
    if (adopted.isEmpty()) {
        return 0;
    }
    const QStringList managedList = paths();
    const QSet<QString> managed(managedList.cbegin(), managedList.cend());
    QStringList missing;
    for (const QString &path : adopted) {
        if (!managed.contains(path)) {
            missing.append(path);
        }
    }
    if (!missing.isEmpty()) {
        std::sort(missing.begin(), missing.end(), Py::codePointLess);
        Py::raiseValueError(QStringLiteral("remote-base paths are absent from the selected manifest: ") + missing.join(QStringLiteral(", ")));
    }
    const Json snapshot = api.sourceTree();
    QList<Json> items;
    for (const Json &item : Py::iterate(Py::get(snapshot, QStringLiteral("sources"), Json::array()))) {
        if (item.isObject() && item.value(QStringLiteral("path")).isString()) {
            items.append(item);
        }
    }
    const SourceHeads current(items);
    Json bases = Py::get(state, QStringLiteral("bases"), Json::object());
    QStringList ordered(adopted.cbegin(), adopted.cend());
    std::sort(ordered.begin(), ordered.end(), Py::codePointLess);
    for (const QString &path : std::as_const(ordered)) {
        if (bases.contains(path)) {
            Py::raiseValueError(QStringLiteral("remote base is already recorded: ") + path);
        }
        const std::optional<Json> item = current.get(path);
        if (!item || Py::truthy(Py::get(*item, QStringLiteral("deleted"), Json(false)))) {
            Py::raiseValueError(QStringLiteral("remote source is unavailable for base adoption: ") + path);
        }
        bases.insert(path, baseOf(*item));
    }
    state.insert(QStringLiteral("bases"), bases);
    writeState();
    return adopted.size();
}

qint64 SourcePublisher::pruneRetiredManifestPaths()
{
    Json payload = readManifest();
    const QList<Json> allPaths = manifestPayloadPaths(payload);
    QSet<QString> selected;
    if (m_includePaths) {
        for (const QString &path : *m_includePaths) {
            selected.insert(Py::hashKey(Json(path)));
        }
    } else {
        for (const Json &path : allPaths) {
            selected.insert(Py::hashKey(path));
        }
    }
    const Json bases = Py::get(state, QStringLiteral("bases"), Json::object());
    const auto retained = [&](const Json &entry) {
        const Json path = Py::subscript(entry, QStringLiteral("path"));
        return !(selected.contains(Py::hashKey(path)) && !Py::isFile(Py::resolve(root / Py::pythonStr(path))) && !bases.contains(path.toString()));
    };
    Json shared = Json::array();
    for (const Json &entry : Py::iterate(Py::get(payload, QStringLiteral("shared_files"), Json::array()))) {
        if (retained(entry)) {
            shared.append(entry);
        }
    }
    payload.insert(QStringLiteral("shared_files"), shared);
    Json projects = Json::array();
    for (const Json &project : Py::iterate(Py::subscript(payload, QStringLiteral("projects")))) {
        Json files = Json::array();
        for (const Json &entry : Py::iterate(Py::subscript(project, QStringLiteral("files")))) {
            if (retained(entry)) {
                files.append(entry);
            }
        }
        Json updated = project;
        updated.insert(QStringLiteral("files"), files);
        projects.append(updated);
    }
    payload.insert(QStringLiteral("projects"), projects);
    const qint64 removed = allPaths.size() - manifestPayloadPaths(payload).size();
    if (removed) {
        Py::atomicWrite(manifestPath, Py::dumpsFile(payload));
        report(QStringLiteral("Removed %1 retired path(s) from the managed source manifest.").arg(removed));
    }
    return removed;
}

Json SourcePublisher::newMutation(const QString &action, const QString &path, const Json &current, const std::optional<QByteArray> &content)
{
    const qint64 sequence = Py::toPythonInt(Py::subscript(state, QStringLiteral("next_device_sequence")));
    if (sequence < 1 || sequence > maximumDeviceSequence) {
        Py::raiseValueError(QStringLiteral("the source publisher's device sequence %1 is outside the server's range 1..%2").arg(sequence).arg(maximumDeviceSequence));
    }
    state.insert(QStringLiteral("next_device_sequence"), Json(sequence + 1));
    Json mutation = Json::object({{QStringLiteral("action"), Json(action)},
                                  {QStringLiteral("operation_id"), Json(newUuid())},
                                  {QStringLiteral("device_sequence"), Json(sequence)},
                                  {QStringLiteral("path"), Json(path)},
                                  {QStringLiteral("base_revision"), Json(Py::toPythonInt(Py::get(current, QStringLiteral("revision"), Json(0))))},
                                  {QStringLiteral("base_sha256"), Py::get(current, QStringLiteral("sha256"), Json(emptySha256))},
                                  {QStringLiteral("base_object_sha256"), Py::get(current, QStringLiteral("object_sha256"), Json(emptySha256))},
                                  {QStringLiteral("client_timestamp"), Json(utcTimestamp())}});
    if (content) {
        mutation.insert(QStringLiteral("content_sha256"), Json(sha256Hex(*content)));
    }
    return mutation;
}

std::optional<qint64> SourcePublisher::collidingSequenceNext(const Py::Exception &error)
{
    if (error.type != Py::apiErrorType || error.status != 409) {
        return std::nullopt;
    }
    Json body;
    try {
        body = Py::loadsBytes(error.message.toUtf8());
    } catch (const Py::Exception &) {
        return std::nullopt;
    }
    // Fedora answers a retry of the same operation or publication from its
    // record. This refusal therefore means another operation owns the number and
    // nothing of the refused request was applied, so it may be renumbered.
    // Servers before error_code send the same detail without it.
    if (!body.isObject() || body.value(QStringLiteral("detail")) != Json(QStringLiteral("device sequence is already assigned"))) {
        return std::nullopt;
    }
    const Json code = Py::get(body, QStringLiteral("error_code"));
    if (!(code.isNull() || code == Json(QStringLiteral("device_sequence_assigned")))) {
        return std::nullopt;
    }
    Json next = Py::get(body, QStringLiteral("next_device_sequence"));
    if (!next.isInt()) {
        next = Py::get(api.deviceSequence(), QStringLiteral("next_device_sequence"));
    }
    if (!next.isInt() || next.toInt() < 1) {
        Py::raiseValueError(QStringLiteral("the server returned an invalid device sequence"));
    }
    return next.toInt();
}

qint64 SourcePublisher::reserveSequences(qint64 serverNext, qint64 count)
{
    const qint64 first = std::max(Py::toPythonInt(Py::subscript(state, QStringLiteral("next_device_sequence"))), serverNext);
    if (first < 1 || count < 0 || first > maximumDeviceSequence - count + 1) {
        Py::raiseValueError(QStringLiteral("the source publisher's device sequences are exhausted (the server's next is %1)").arg(serverNext));
    }
    state.insert(QStringLiteral("next_device_sequence"), Json(first + count));
    return first;
}

Json SourcePublisher::publishPending(Json publication)
{
    for (int recoveries = 0;; ++recoveries) {
        try {
            return api.publish(publication);
        } catch (const Py::Exception &error) {
            const std::optional<qint64> serverNext = recoveries < maximumSequenceRecoveries ? collidingSequenceNext(error) : std::nullopt;
            if (!serverNext) {
                throw;
            }
            // Keep the publication and operation IDs: if this renumbered request
            // is applied and its answer lost, the retry is answered from its record.
            QList<Json> mutations = Py::iterate(Py::subscript(publication, QStringLiteral("mutations")));
            qint64 sequence = reserveSequences(*serverNext, mutations.size());
            const qint64 first = sequence;
            for (Json &mutation : mutations) {
                mutation.insert(QStringLiteral("device_sequence"), Json(sequence++));
            }
            publication.insert(QStringLiteral("mutations"), Json::array(mutations));
            state.insert(QStringLiteral("pending_publication"), publication);
            writeState();
            report(QStringLiteral("The server assigned a device sequence of this publication to another change; renumbered %1 pending source change(s) from %2.")
                       .arg(mutations.size())
                       .arg(first));
        }
    }
}

Json SourcePublisher::applyMutation(Json mutation)
{
    for (int recoveries = 0;; ++recoveries) {
        try {
            return api.apply(mutation);
        } catch (const Py::Exception &error) {
            const std::optional<qint64> serverNext = recoveries < maximumSequenceRecoveries ? collidingSequenceNext(error) : std::nullopt;
            if (!serverNext) {
                throw;
            }
            const qint64 sequence = reserveSequences(*serverNext, 1);
            mutation.insert(QStringLiteral("device_sequence"), Json(sequence));
            // Same operation ID: if the renumbered request is applied and its
            // answer lost, the retry is answered from Fedora's record.
            storePendingApply(mutation);
            report(QStringLiteral("The server assigned this source change's device sequence to another change; renumbered it to %1.").arg(sequence));
        }
    }
}

void SourcePublisher::storePendingApply(const Json &mutation)
{
    Json pending = Py::get(state, pendingAppliesKey, Json::object());
    if (!pending.isObject()) {
        pending = Json::object();
    }
    pending.insert(Py::subscript(mutation, QStringLiteral("path")).toString(), mutation);
    state.insert(pendingAppliesKey, pending);
    writeState();
}

void SourcePublisher::forgetPendingApplies(const QSet<QString> &paths)
{
    if (!state.contains(pendingAppliesKey)) {
        return;
    }
    Json pending = state.value(pendingAppliesKey);
    if (pending.isObject()) {
        for (const QString &path : paths) {
            pending.remove(path);
        }
    }
    if (pending.isObject() && pending.size() > 0) {
        state.insert(pendingAppliesKey, pending);
    } else {
        state.remove(pendingAppliesKey);
    }
}

Json SourcePublisher::sourceApplyMutation(const QString &path, const Json &base, const QByteArray &content)
{
    const Json applies = Py::get(state, pendingAppliesKey, Json::object());
    const Json pending = applies.isObject() ? Py::get(applies, path) : Json();
    const Json revision(Py::toPythonInt(Py::get(base, QStringLiteral("revision"), Json(0))));
    if (pending.isObject() && Py::get(pending, QStringLiteral("path")) == Json(path) && Py::get(pending, QStringLiteral("content_sha256")) == Json(sha256Hex(content))
        && Py::get(pending, QStringLiteral("base_revision")) == revision
        && Py::get(pending, QStringLiteral("base_sha256")) == Py::get(base, QStringLiteral("sha256"), Json(emptySha256))
        && Py::get(pending, QStringLiteral("base_object_sha256")) == Py::get(base, QStringLiteral("object_sha256"), Json(emptySha256))
        && Py::get(pending, QStringLiteral("operation_id")).isString() && Py::get(pending, QStringLiteral("device_sequence")).isInt()
        && Py::get(pending, QStringLiteral("client_timestamp")).isString()) {
        report(QStringLiteral("Repeating the unanswered source change %1 with its operation ID.").arg(path));
        return pending;
    }
    Json mutation = newMutation(QStringLiteral("upsert"), path, base, content);
    ensureEmptyBase({mutation});
    mutation.remove(QStringLiteral("action"));
    // Stored with the apply, so a repeated request carries the same claims.
    if (const std::optional<Json> claims = applyProposalClaims(*this, path, content)) {
        mutation.insert(QStringLiteral("proposal_decisions"), *claims);
    }
    storePendingApply(mutation);
    return mutation;
}

std::optional<SourcePublisher::SettledApply> SourcePublisher::settleMergedApply(const QString &path, const QByteArray &content, const Json &result,
                                                                               const std::optional<Json> &previousBase)
{
    const Json outcome = Py::get(result, QStringLiteral("outcome"));
    const bool duplicate = outcome == Json(QStringLiteral("duplicate"));
    if (!duplicate && outcome != Json(QStringLiteral("applied")) && outcome != Json(QStringLiteral("merged"))) {
        return std::nullopt;
    }
    Json head = result;
    if (duplicate) {
        // Fedora committed this operation before (its answer was lost) and
        // answers with its current version, which already contains the change.
        const SourceHeads heads(Py::iterate(Py::get(api.sourceTree(), QStringLiteral("sources"), Json::array())));
        const std::optional<Json> item = heads.get(path);
        if (!item || Py::truthy(Py::get(*item, QStringLiteral("deleted"), Json(false)))) {
            forgetPendingApplies({path});
            writeState();
            Py::raiseValueError(QStringLiteral("source was deleted on the server after the server merged this computer's change: ") + path);
        }
        head = *item;
    }
    const Json revision = Py::get(head, QStringLiteral("revision"));
    const Json resultSha256 = Py::get(head, QStringLiteral("sha256"));
    const Json resultObject = Py::get(head, QStringLiteral("object_sha256"));
    if (Py::get(head, QStringLiteral("path")) != Json(path) || !revision.isInt() || revision.toInt() < 1 || !resultSha256.isString()
        || resultSha256.toString().size() != 64 || !resultObject.isString() || resultObject.toString().size() != 64) {
        Py::raiseValueError(QStringLiteral("the server returned an inconsistent source merge result: ") + path);
    }
    // Fedora's merge is authoritative: it reads agent proposals, removed and
    // moved review-card markers and adjacent edits, which this computer's
    // merge does not, and it has committed its result. That result replaces
    // the file the merge started from, as a merged publication result does,
    // and becomes its base.
    SettledApply settled;
    const bool current = activateMergedResult(path, sha256Hex(content), resultSha256, resultObject, &settled.installed);
    // Edited again after Desktop froze it: the newer edit keeps the previous
    // base, so the next Sync merges it with Fedora's result.
    settled.base = current ? std::optional<Json>(baseOf(head)) : previousBase;
    Json bases = Py::get(state, QStringLiteral("bases"), Json::object());
    if (settled.base) {
        bases.insert(path, *settled.base);
    } else {
        bases.remove(path);
    }
    state.insert(QStringLiteral("bases"), bases);
    forgetPendingApplies({path});
    writeState();
    if (current) {
        report(QStringLiteral("The server merged source change %1 into revision %2.").arg(path, Py::pythonStr(revision)));
    } else {
        report(QStringLiteral("The server merged source change %1 into revision %2; the file changed after Desktop froze it, so the next Sync merges the newer edit.")
                   .arg(path, Py::pythonStr(revision)));
    }
    return settled;
}

void SourcePublisher::ensureEmptyBase(const QList<Json> &mutations)
{
    for (const Json &item : mutations) {
        if (Py::subscript(item, QStringLiteral("action")) == Json(QStringLiteral("delete"))
            || Py::get(item, QStringLiteral("base_object_sha256")) == Json(emptySha256)) {
            api.uploadObject(QByteArray());
            return;
        }
    }
}

SourcePublisher::Report SourcePublisher::publishOnce()
{
    const auto uploadingFields = [](const QString &path, qint64 transferred, qint64 total) {
        ProgressFields fields;
        fields.phase = QStringLiteral("uploading");
        fields.path = path;
        fields.transferredBytes = transferred;
        fields.totalBytes = total;
        return fields;
    };
    const auto phaseFields = [](const QString &phase) {
        ProgressFields fields;
        fields.phase = phase;
        return fields;
    };
    const Json pending = state.value(QStringLiteral("pending_publication"));
    if (pending.isObject()) {
        const QList<Json> pendingMutations = Py::iterate(Py::subscript(pending, QStringLiteral("mutations")));
        report(QStringLiteral("Retrying metadata for %1 pending source change(s).").arg(Py::subscript(pending, QStringLiteral("mutations")).size()),
               phaseFields(QStringLiteral("uploading")));
        ensureEmptyBase(pendingMutations);
        const Json result = publishPending(pending);
        const Json pendingFrozen = Py::get(state, QStringLiteral("pending_frozen_activations"), Json::object());
        bool valid = pendingFrozen.isObject();
        QHash<QString, QString> frozenSha256ByPath;
        for (const auto &[path, digest] : pendingFrozen.members()) {
            valid = valid && digest.isString();
            frozenSha256ByPath.insert(path, digest.toString());
        }
        if (!valid) {
            Py::raiseValueError(QStringLiteral("pending frozen source activations are malformed"));
        }
        activatePublicationResults(result, frozenSha256ByPath);
        Report report;
        for (const Json &item : pendingMutations) {
            const Json action = Py::get(item, QStringLiteral("action"));
            report.upserted += action == Json(QStringLiteral("upsert")) ? 1 : 0;
            report.deleted += action == Json(QStringLiteral("delete")) ? 1 : 0;
        }
        report.sourceFrontier = Json(Py::toPythonInt(Py::subscript(result, QStringLiteral("source_frontier"))));
        report.publicationId = Py::subscript(result, QStringLiteral("publication_id"));
        state.remove(QStringLiteral("pending_publication"));
        state.remove(QStringLiteral("pending_frozen_activations"));
        expireProposalClaims(*this, result);
        state.insert(QStringLiteral("last_publication"), result);
        recordBases(std::nullopt, publishedHeads(result));
        pruneRetiredManifestPaths();
        writeState();
        return report;
    }

    this->report(QStringLiteral("Inspecting the selected managed source identities."), phaseFields(QStringLiteral("preparing")));
    LocalSources local = localSources();
    if (!m_includePaths) {
        // Retired paths keep their recorded base until a deletion is published.
        QStringList retired;
        for (const Json &path : Py::iterate(Py::get(state, QStringLiteral("bases"), Json::object()))) {
            if (localIndex(local, path.toString()) < 0) {
                retired.append(path.toString());
            }
        }
        std::sort(retired.begin(), retired.end(), Py::codePointLess);
        for (const QString &path : std::as_const(retired)) {
            checkManifestRetirement(path);
            local.append({path, std::nullopt});
        }
    }
    qsizetype missingCount = 0;
    for (const auto &entry : std::as_const(local)) {
        missingCount += entry.second ? 0 : 1;
    }
    this->report(QStringLiteral("Read %1 managed path(s); %2 path(s) are absent locally.").arg(local.size()).arg(missingCount));
    this->report(QStringLiteral("Checking the current server source frontier."), phaseFields(QStringLiteral("checking_server")));
    const Json snapshot = currentSourceTree();
    const SourceHeads current(Py::iterate(Py::get(snapshot, QStringLiteral("sources"), Json::array())));
    QSet<QString> localPaths;
    for (const auto &entry : std::as_const(local)) {
        localPaths.insert(entry.first);
    }
    adoptReviewSourceBases(*this, current, localPaths);
    const QSet<QString> resolvedConflictPaths = reconcilePreservedConflicts(local, current);
    const Json bases = Py::get(state, QStringLiteral("bases"), Json::object());
    QList<Json> mutations;
    QList<QPair<QString, QByteArray>> frozenIncoming;
    QList<QPair<QString, QPair<QByteArray, QByteArray>>> remoteActivations;
    QStringList preservedConflicts;
    qint64 unchanged = 0;
    const Json emptyBase = Json::object({{QStringLiteral("revision"), Json(0)},
                                         {QStringLiteral("sha256"), Json(emptySha256)},
                                         {QStringLiteral("object_sha256"), Json(emptySha256)}});
    const auto uploadSource = [&](const QString &path, const QByteArray &content) {
        this->report(QStringLiteral("Uploading source change %1 (%2).").arg(path, formatEventBytes(content.size())), uploadingFields(path, 0, content.size()));
        try {
            api.uploadObject(content);
        } catch (const Py::Exception &error) {
            QString detail = collapseWhitespace(error.message);
            if (detail.isEmpty()) {
                detail = Py::className(error);
            }
            ProgressFields fields = uploadingFields(path, 0, content.size());
            fields.state = QStringLiteral("failed");
            fields.errorCode = stableErrorCode(error, QStringLiteral("uploading"), QStringLiteral("source"));
            this->report(QStringLiteral("Uploading source change %1 failed: %2. Source publication did not complete.").arg(path, detail), fields);
            throw;
        }
        this->report(QStringLiteral("Uploaded source change %1 (%2).").arg(path, formatEventBytes(content.size())),
                     uploadingFields(path, content.size(), content.size()));
    };
    // Sources Fedora merged alone: the base each keeps (none: no base) over
    // the heads recorded at the end, and Fedora's results now installed.
    QHash<QString, std::optional<Json>> settledBases;
    QList<QPair<QString, QByteArray>> settledContents;
    // This computer's three-way merge conflicts: the source goes to Fedora
    // alone, whose merge decides. Fedora merges more than this computer does,
    // so it may merge it cleanly; otherwise it preserves the conflict.
    const auto sendAlone = [&](const QString &path, const QByteArray &content, const Json &sentBase, const std::optional<Json> &previousBase) {
        api.uploadObject(content);
        const Json mutation = sourceApplyMutation(path, sentBase, content);
        const Json result = applyMutation(mutation);
        expireApplyProposalClaims(*this, mutation, result);
        if (const std::optional<SettledApply> settled = settleMergedApply(path, content, result, previousBase)) {
            settledBases.insert(path, settled->base);
            if (settled->installed) {
                settledContents.append({path, *settled->installed});
            }
            return;
        }
        const Json conflictId = Py::get(result, QStringLiteral("conflict_id"));
        const Json revision = Py::get(result, QStringLiteral("revision"));
        if (Py::get(result, QStringLiteral("outcome")) != Json(QStringLiteral("conflict")) || !conflictId.isString() || conflictId.toString().isEmpty()
            || !(revision.type() == Json::Type::Int || revision.isBool())) {
            Py::raiseValueError(QStringLiteral("the server returned an inconsistent source merge result: ") + path);
        }
        // Recorded now and used as a folder name when the conflict is settled.
        requireConflictId(conflictId.toString());
        Json conflicts = state.contains(QStringLiteral("conflicts")) ? state.value(QStringLiteral("conflicts")) : Json::object();
        conflicts.insert(path, Json::object({{QStringLiteral("conflict_id"), conflictId},
                                             {QStringLiteral("local_sha256"), Json(sha256Hex(content))},
                                             {QStringLiteral("server_revision"), revision}}));
        state.insert(QStringLiteral("conflicts"), conflicts);
        forgetPendingApplies({path});
        writeState();
        preservedConflicts.append(path);
    };
    // After the heads are recorded at the end of a run.
    const auto finishSettledApplies = [&] {
        Json recorded = Py::get(state, QStringLiteral("bases"), Json::object());
        for (auto it = settledBases.cbegin(); it != settledBases.cend(); ++it) {
            if (it.value()) {
                recorded.insert(it.key(), *it.value());
            } else {
                recorded.remove(it.key());
            }
        }
        if (!settledBases.isEmpty()) {
            state.insert(QStringLiteral("bases"), recorded);
        }
        // This run examined these paths: an older unanswered apply of one is settled.
        forgetPendingApplies(localPaths);
    };
    for (const auto &[path, content] : std::as_const(local)) {
        const std::optional<Json> remote = current.get(path);
        const std::optional<Json> base = bases.contains(path) ? std::optional<Json>(bases.value(path)) : std::nullopt;
        const FrozenEntry *frozen = m_frozen && !resolvedConflictPaths.contains(path) ? m_frozen->source(path) : nullptr;
        const bool remoteDeleted = remote && Py::truthy(Py::get(*remote, QStringLiteral("deleted"), Json(false)));
        if (frozen) {
            const bool remoteExists = remote && !remoteDeleted;
            if (frozen->baseSha256 && base && base->isObject() && base->value(QStringLiteral("sha256")) == Json(*frozen->baseSha256)) {
                // Desktop has caught up with the recorded base.
                forgetSupersededBases(path);
            }
            if (!content) {
                if (!remoteExists) {
                    ++unchanged;
                    continue;
                }
                if (!frozen->baseSha256) {
                    Py::raiseValueError(QStringLiteral("frozen source has no declared server base: ") + path);
                }
                if (*frozen->baseSha256 == emptySha256 || Py::get(*remote, QStringLiteral("sha256")) != Json(*frozen->baseSha256)) {
                    Py::raiseValueError(QStringLiteral("managed source was deleted in a frozen Desktop snapshot, but the server's copy changed after its declared base: ") + path);
                }
                this->report(QStringLiteral("Scheduling verified frozen source deletion: ") + path);
                mutations.append(newMutation(QStringLiteral("delete"), path, *remote, std::nullopt));
                continue;
            }
            const QString digest = sha256Hex(*content);
            if (remoteExists && Py::get(*remote, QStringLiteral("sha256")) == Json(digest)) {
                ++unchanged;
                continue;
            }
            if (!frozen->baseSha256) {
                Py::raiseValueError(QStringLiteral("frozen source has no declared server base: ") + path);
            }
            const std::optional<Json> recordedBase = base && base->isObject() ? base : std::nullopt;
            const Json recordedBaseSha256 = recordedBase ? recordedBase->value(QStringLiteral("sha256")) : Json();
            if (recordedBase) {
                const Json revision = recordedBase->value(QStringLiteral("revision"));
                if (!(revision.type() == Json::Type::Int || revision.isBool() || revision.type() == Json::Type::BigInt)
                    || !recordedBase->value(QStringLiteral("sha256")).isString() || !recordedBase->value(QStringLiteral("object_sha256")).isString()) {
                    Py::raiseValueError(QStringLiteral("recorded server source base is malformed: ") + path);
                }
            }
            if (recordedBase && Json(digest) == recordedBaseSha256 && remoteExists && Py::get(*remote, QStringLiteral("sha256")) != Json(digest)) {
                const QByteArray remoteContent = api.download(Py::pythonStr(Py::subscript(*remote, QStringLiteral("object_sha256"))));
                if (Json(sha256Hex(remoteContent)) != Py::subscript(*remote, QStringLiteral("sha256"))) {
                    Py::raiseValueError(QStringLiteral("remote source failed identity verification: ") + path);
                }
                remoteActivations.append({path, {*content, remoteContent}});
                ++unchanged;
                continue;
            }
            if (!recordedBase && remoteExists) {
                // No authoring base: Fedora's copy is not this file's ancestor,
                // so publishing against it would silently replace it (a Project
                // file this computer wrote before Fedora's later revisions).
                // Merge against an empty ancestor as Fedora would: an equivalent
                // file takes Fedora's copy, anything else is a conflict.
                const QByteArray remoteContent = api.download(Py::pythonStr(Py::subscript(*remote, QStringLiteral("object_sha256"))));
                if (Json(sha256Hex(remoteContent)) != Py::subscript(*remote, QStringLiteral("sha256"))) {
                    Py::raiseValueError(QStringLiteral("remote source failed identity verification: ") + path);
                }
                const auto [merged, conflictCount] = mergeSources(QByteArray(), remoteContent, *content, path);
                if (!conflictCount && merged == remoteContent) {
                    remoteActivations.append({path, {*content, remoteContent}});
                    ++unchanged;
                    continue;
                }
                if (conflictCount) {
                    sendAlone(path, *content, emptyBase, std::nullopt);
                    continue;
                }
                // Fedora's copy is empty: this file only adds to it.
            }
            Json authoringBase = emptyBase;
            if (recordedBase && Py::truthy(*recordedBase)) {
                authoringBase = *recordedBase;
            } else if (remote && Py::truthy(*remote)) {
                authoringBase = *remote;
            }
            // The base the frozen bytes are merged from: the recorded one,
            // or an earlier one Desktop has not moved on from.
            std::optional<Json> mergeBase = recordedBase;
            if (recordedBase) {
                const QSet<QString> allowed{Py::hashKey(recordedBaseSha256),
                                            Py::hashKey(remoteExists ? Py::get(*remote, QStringLiteral("sha256")) : Json(emptySha256))};
                if (!allowed.contains(Py::hashKey(Json(*frozen->baseSha256)))) {
                    const Json receipt = Py::get(Py::get(state, QStringLiteral("review_source_base_receipts"), Json::object()), path, Json::object());
                    if (Py::get(receipt, QStringLiteral("sha256")) != recordedBaseSha256
                        || Py::get(receipt, QStringLiteral("previous_base_sha256")) != Json(*frozen->baseSha256)) {
                        if (!supersededBases(path).contains(*frozen->baseSha256)) {
                            Py::raiseValueError(QStringLiteral("frozen Desktop source has no matching authoring or server base: ") + path);
                        }
                        mergeBase = laggingFrozenBase(path, *content, *recordedBase, *frozen->baseSha256);
                        authoringBase = *mergeBase;
                    }
                }
            }
            const Json mergeBaseSha256 = mergeBase ? mergeBase->value(QStringLiteral("sha256")) : Json();
            if (mergeBase && remoteExists && Py::get(*remote, QStringLiteral("sha256")) != mergeBaseSha256) {
                const QByteArray baseContent = api.download(mergeBase->value(QStringLiteral("object_sha256")).toString());
                const QByteArray remoteContent = api.download(Py::pythonStr(Py::subscript(*remote, QStringLiteral("object_sha256"))));
                if (Json(sha256Hex(baseContent)) != mergeBaseSha256) {
                    Py::raiseValueError(QStringLiteral("recorded source base failed identity verification: ") + path);
                }
                if (Json(sha256Hex(remoteContent)) != Py::subscript(*remote, QStringLiteral("sha256"))) {
                    Py::raiseValueError(QStringLiteral("remote source failed identity verification: ") + path);
                }
                const auto [merged, conflictCount] = mergeSources(baseContent, remoteContent, *content, path);
                if (isManifestPath(path) && !conflictCount && merged == remoteContent) {
                    remoteActivations.append({path, {*content, remoteContent}});
                    ++unchanged;
                    continue;
                }
                if (conflictCount) {
                    sendAlone(path, *content, *mergeBase, *recordedBase);
                    continue;
                }
            }
            uploadSource(path, *content);
            this->report(QStringLiteral("Scheduling frozen source update: ") + path);
            mutations.append(newMutation(QStringLiteral("upsert"), path, authoringBase, content));
            frozenIncoming.append({path, *content});
            continue;
        }
        if (!content) {
            if (!base) {
                if (!remote || remoteDeleted) {
                    ++unchanged;
                    continue;
                }
                Py::raiseValueError(QStringLiteral("managed source is missing without a recorded server base: ") + path);
            }
            if (!remote || remoteDeleted) {
                ++unchanged;
                continue;
            }
            for (const char *key : {"revision", "sha256", "object_sha256"}) {
                if (Py::get(*remote, QLatin1String(key)) != Py::get(*base, QLatin1String(key))) {
                    Py::raiseValueError(QStringLiteral("managed source was deleted locally, but the server's copy changed after the recorded base: ") + path);
                }
            }
            this->report(QStringLiteral("Scheduling verified source deletion: ") + path);
            mutations.append(newMutation(QStringLiteral("delete"), path, *base, std::nullopt));
            continue;
        }
        const QString digest = sha256Hex(*content);
        Json effectiveBase;
        if (base) {
            effectiveBase = *base;
        } else if (remote && !remoteDeleted) {
            if (Py::get(*remote, QStringLiteral("sha256")) != Json(digest)) {
                Py::raiseValueError(QStringLiteral("local source has no recorded server base and differs remotely: ") + path);
            }
            effectiveBase = *remote;
        } else if (remote) {
            effectiveBase = *remote;
        } else {
            effectiveBase = emptyBase;
        }
        if (Json(digest) == Py::subscript(effectiveBase, QStringLiteral("sha256"))) {
            if (remote && !remoteDeleted && Py::subscript(*remote, QStringLiteral("sha256")) != Json(digest)) {
                const QByteArray remoteContent = api.download(Py::pythonStr(Py::subscript(*remote, QStringLiteral("object_sha256"))));
                if (Json(sha256Hex(remoteContent)) != Py::subscript(*remote, QStringLiteral("sha256"))) {
                    Py::raiseValueError(QStringLiteral("remote source failed identity verification: ") + path);
                }
                if (!m_frozen || !m_frozen->source(path)) {
                    activateCleanRemote(path, remoteContent);
                }
            }
            ++unchanged;
            continue;
        }
        uploadSource(path, *content);
        this->report(QStringLiteral("Scheduling source update: ") + path);
        mutations.append(newMutation(QStringLiteral("upsert"), path, effectiveBase, content));
    }
    for (const auto &[path, installed] : std::as_const(settledContents)) {
        local[localIndex(local, path)].second = installed;
    }
    if (!preservedConflicts.isEmpty()) {
        Py::raiseValueError(QStringLiteral("source conflict was preserved on the server for resolution: ") + preservedConflicts.join(QStringLiteral(", ")));
    }
    for (const auto &[path, contents] : std::as_const(remoteActivations)) {
        activateFrozenRemoteSha(path, sha256Hex(contents.first), contents.second, true);
    }
    // The heads the files hold once this run is done: what `result` published,
    // or the head of `snapshot` a file was found equal to (or activated from).
    // Fedora may move a path past it before the tree is read back.
    const auto heldHeads = [&](const Json &result) {
        QHash<QString, Json> held = publishedHeads(result);
        const auto holds = [&](const QString &path, const QByteArray &bytes) {
            const std::optional<Json> head = current.get(path);
            if (!held.contains(path) && head && !Py::truthy(Py::get(*head, QStringLiteral("deleted"), Json(false)))
                && Py::get(*head, QStringLiteral("sha256")) == Json(sha256Hex(bytes))) {
                held.insert(path, baseOf(*head));
            }
        };
        for (const auto &[path, content] : std::as_const(local)) {
            if (content) {
                holds(path, *content);
            }
        }
        for (const auto &[path, contents] : std::as_const(remoteActivations)) {
            holds(path, contents.second);
        }
        return held;
    };
    const qint64 mergedAlone = settledBases.size();
    if (mutations.isEmpty()) {
        prepareProposalClaims(*this, local, mutations);
        // Fedora committed what it merged alone after `snapshot` was read.
        const Json heads = mergedAlone ? api.sourceTree() : snapshot;
        if (mergedAlone) {
            keepSourceTree(heads);
        }
        Report result;
        result.unchanged = unchanged;
        result.upserted = mergedAlone;
        result.sourceFrontier = Json(Py::toPythonInt(Py::get(heads, QStringLiteral("source_frontier"), Json(0))));
        if (mergedAlone) {
            this->report(QStringLiteral("Publication complete at source frontier %1: %2 updated, 0 deleted, %3 unchanged.")
                             .arg(Py::pythonStr(result.sourceFrontier))
                             .arg(result.upserted)
                             .arg(result.unchanged));
        } else {
            this->report(QStringLiteral("No source changes need publication."));
        }
        recordBases(heads, heldHeads(Json::object()));
        finishSettledApplies();
        pruneRetiredManifestPaths();
        writeState();
        return result;
    }
    if (mutations.size() > 1000) {
        Py::raiseValueError(QStringLiteral("atomic source publication exceeds the 1000-file protocol limit"));
    }
    ensureEmptyBase(mutations);
    Json publication = Json::object({{QStringLiteral("publication_id"), Json(newUuid())}, {QStringLiteral("mutations"), Json::array(mutations)}});
    if (const std::optional<Json> claims = prepareProposalClaims(*this, local, mutations)) {
        publication.insert(QStringLiteral("proposal_decisions"), *claims);
    }
    state.insert(QStringLiteral("pending_publication"), publication);
    QHash<QString, QString> frozenSha256ByPath;
    if (!frozenIncoming.isEmpty()) {
        Json activations = Json::object();
        for (const auto &[path, content] : std::as_const(frozenIncoming)) {
            activations.insert(path, Json(sha256Hex(content)));
            frozenSha256ByPath.insert(path, sha256Hex(content));
        }
        state.insert(QStringLiteral("pending_frozen_activations"), activations);
    }
    writeState();
    this->report(QStringLiteral("Uploading metadata for %1 source change(s).").arg(mutations.size()), phaseFields(QStringLiteral("uploading")));
    const Json result = publishPending(publication);
    activatePublicationResults(result, frozenSha256ByPath);
    Report report;
    report.unchanged = unchanged;
    report.upserted = mergedAlone;
    for (const Json &item : std::as_const(mutations)) {
        const Json action = item.value(QStringLiteral("action"));
        report.upserted += action == Json(QStringLiteral("upsert")) ? 1 : 0;
        report.deleted += action == Json(QStringLiteral("delete")) ? 1 : 0;
    }
    report.sourceFrontier = Json(Py::toPythonInt(Py::subscript(result, QStringLiteral("source_frontier"))));
    report.publicationId = Py::subscript(result, QStringLiteral("publication_id"));
    state.remove(QStringLiteral("pending_publication"));
    state.remove(QStringLiteral("pending_frozen_activations"));
    expireProposalClaims(*this, result);
    state.insert(QStringLiteral("last_publication"), result);
    // The server's tree after the publication, read once: the bases, and whether it is
    // the tree read before with exactly this publication's changes. If so, the editable-
    // source installer has nothing to find up to this frontier, and the app need not run
    // it again only to see what was just sent.
    const Json after = api.sourceTree();
    keepSourceTree(after);
    recordBases(after, heldHeads(result));
    if (!mergedAlone && onlyOwnChanges(snapshot, after, mutations, result)) {
        const Json before = Py::get(snapshot, QStringLiteral("source_frontier"));
        const Json through = Py::get(after, QStringLiteral("source_frontier"));
        if (before.type() == Json::Type::Int && through.type() == Json::Type::Int) {
            report.sourceTreeVerified = QPair<qint64, qint64>(Py::toPythonInt(before), Py::toPythonInt(through));
        }
    }
    finishSettledApplies();
    pruneRetiredManifestPaths();
    writeState();
    this->report(QStringLiteral("Publication complete at source frontier %1: %2 updated, %3 deleted, %4 unchanged.")
                     .arg(Py::pythonStr(report.sourceFrontier))
                     .arg(report.upserted)
                     .arg(report.deleted)
                     .arg(report.unchanged));
    return report;
}

}
