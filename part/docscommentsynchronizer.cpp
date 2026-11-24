#include "sourceannotationindex.h"
#include "thoughtsplatform.h"
/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "docscommentsynchronizer.h"
#include "verifiedartifactcache.h"
#include "fileidentitycache.h"
#include "commenttexvisibility.h"
#include "sourcenavigationvisibility.h"
#include "sourceowner.h"
#include "sourcefoldernames.h"
#include "commentanchorstate.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QLockFile>
#include <QMutex>
#include <QMutexLocker>
#include <QCache>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>

#include <kzip.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <tuple>
#include <vector>

#ifdef Q_OS_MACOS
#include <glob.h>
#endif

namespace Okular
{

namespace
{

constexpr int StateVersion = 2;
constexpr int AnchorAttemptCacheVersion = 9;
constexpr int UnsafeSourceLinePolicyVersion = 6;
constexpr int SidecarSchemaVersion = 5;
const QString ThoughtsCreator = QStringLiteral("thoughts");
const QString MjPdfCreator = QStringLiteral("mj-pdf-reader");
const QString LogicalCommentsFilename = QStringLiteral("logical_comments.jsonl");

QFileInfoList legacyDocumentSidecars(const QDir &commentsDir)
{
    QFileInfoList result;
    const QFileInfoList candidates = commentsDir.entryInfoList({QStringLiteral("*_comments.jsonl")}, QDir::Files, QDir::Name);
    for (const QFileInfo &candidate : candidates) {
        if (candidate.fileName() != LogicalCommentsFilename) {
            result.append(candidate);
        }
    }
    return result;
}

const QSet<QString> ToolMetadataFields {
    QStringLiteral("review_key"),
    QStringLiteral("anchor_id"),
    QStringLiteral("anchor_status"),
    QStringLiteral("anchor_confidence"),
    QStringLiteral("anchor_kind"),
    QStringLiteral("anchor_scope"),
    QStringLiteral("tex_file"),
    QStringLiteral("tex_line"),
    QStringLiteral("tex_column"),
    QStringLiteral("source_build_id"),
    QStringLiteral("source_fingerprint"),
    QStringLiteral("source_context"),
    QStringLiteral("anchor_error_code"),
    QStringLiteral("anchor_error_detail"),
    QStringLiteral("linking_method"),
    QStringLiteral("selected_text"),
    QStringLiteral("annotation_pdf_sha256"),
    QStringLiteral("markup_status"),
    QStringLiteral("markup_subtype"),
    // An orphaned placement remembers why its source marker disappeared.
    QStringLiteral("orphan_reason"),
    QStringLiteral("orphaned_at"),
    // Overview-only metadata is authoritative in the shared sidecar. Carry it
    // through annotation snapshots so saving the PDF cannot discard phone
    // edits that have no native PDF representation.
    QStringLiteral("priority"),
    QStringLiteral("location"),
    QStringLiteral("chatgpt_url"),
    // Reply topology lives only in the sidecar; PDF annotations do not expose
    // it through Okular, so an in-memory refresh/save must carry it through.
    QStringLiteral("in_reply_to"),
    // Immutable creation provenance is independent of the last writer in
    // origin_id and must survive every PDF/sidecar reconciliation.
    QStringLiteral("created_in_app"),
};

const QSet<QString> AnchorMetadataFields {
    QStringLiteral("review_key"),
    QStringLiteral("anchor_id"),
    QStringLiteral("anchor_status"),
    QStringLiteral("anchor_confidence"),
    QStringLiteral("anchor_kind"),
    QStringLiteral("anchor_scope"),
    QStringLiteral("tex_file"),
    QStringLiteral("tex_line"),
    QStringLiteral("tex_column"),
    QStringLiteral("source_build_id"),
    QStringLiteral("source_fingerprint"),
    QStringLiteral("source_context"),
    QStringLiteral("anchor_error_code"),
    QStringLiteral("anchor_error_detail"),
    QStringLiteral("linking_method"),
    QStringLiteral("markup_status"),
    QStringLiteral("markup_subtype"),
    QStringLiteral("orphan_reason"),
    QStringLiteral("orphaned_at"),
};

bool isTextMarkupSubtype(const QString &subtype)
{
    return subtype == QLatin1String("Highlight") || subtype == QLatin1String("Underline") || subtype == QLatin1String("Squiggly") || subtype == QLatin1String("StrikeOut");
}

bool isTrackedCommentRecordImpl(const QJsonObject &record)
{
    return !isTextMarkupSubtype(record.value(QStringLiteral("subtype")).toString()) || !record.value(QStringLiteral("contents")).toString().trimmed().isEmpty();
}

void clearAnchorMetadata(QJsonObject *record)
{
    for (const QString &field : AnchorMetadataFields) {
        record->remove(field);
    }
}

const QSet<QString> VolatileFields = [] {
    QSet<QString> fields {
        QStringLiteral("comment_index"),
        QStringLiteral("source_pdf"),
        QStringLiteral("object_id"),
    };
    fields.unite(ToolMetadataFields);
    return fields;
}();

const QStringList SyntheticKeyFields {
    QStringLiteral("subtype"),
    QStringLiteral("page"),
    QStringLiteral("rect"),
    QStringLiteral("quad_points"),
    QStringLiteral("fragments"),
    QStringLiteral("author"),
    QStringLiteral("created"),
};

const QSet<QString> UnsafeEnvironments {
    QStringLiteral("DocCode"),
    QStringLiteral("LinkedDocCode"),
    QStringLiteral("Verbatim"),
    QStringLiteral("verbatim"),
    QStringLiteral("lstlisting"),
    QStringLiteral("align"),
    QStringLiteral("align*"),
    QStringLiteral("algorithm"),
    QStringLiteral("algorithmic"),
    QStringLiteral("displaymath"),
    QStringLiteral("equation"),
    QStringLiteral("equation*"),
    QStringLiteral("figure"),
    QStringLiteral("math"),
    QStringLiteral("table"),
    QStringLiteral("tabular"),
    QStringLiteral("tikzpicture"),
};

const QStringList UnsafeLinePrefixes {
    QStringLiteral("\\begin"),
    QStringLiteral("\\caption"),
    QStringLiteral("\\chapter"),
    QStringLiteral("\\docTitle"),
    QStringLiteral("\\end"),
    QStringLiteral("\\item"),
    QStringLiteral("\\label"),
    QStringLiteral("\\paragraph"),
    QStringLiteral("\\section"),
    QStringLiteral("\\subsection"),
    QStringLiteral("\\subsubsection"),
};

struct KeyedComments {
    QHash<QString, QJsonObject> values;
    QStringList order;
};

struct SourceLocation {
    bool valid = false;
    QString sourceId;
    QString fingerprint;
    int localPage = 0;
    QString sourcePath;
};

struct SourceSnapshot {
    QString sourceId;
    QString sourcePath;
    QString snapshotPath;
    int inputOrder = 0;
    bool markerEligible = true;
};

struct BuildInfo {
    bool valid = false;
    QString directory;
    QString buildId;
    QString sourceId;
    QString sourceFingerprint;
    QString synctexFingerprint;
    QString sourcePath;
    QString cachedPdf;
    QString snapshotSource;
    QList<SourceSnapshot> sourceSnapshots;
};

struct BuildRegistry {
    QHash<QString, BuildInfo> uniqueBuilds;
    QSet<QString> ambiguousBuilds;
    QHash<QString, BuildInfo> buildsById;
    QHash<QString, QString> uniqueSourceIdsByFingerprint;
    QSet<QString> ambiguousSourceFingerprints;
};

struct HistoricalBuildResolution {
    bool requested = false;
    bool valid = false;
    SourceLocation source;
    BuildInfo build;
};

struct SynctexRecord {
    QHash<QString, QString> fields;
};

struct TexIncludeSite {
    QString parentSourceId;
    QString parentPath;
    QString childPath;
    int line = 0;
    bool safe = false;
};

struct GeneratedParentResolution {
    bool valid = false;
    QString reason;
    QString sourceId;
    QString sourcePath;
    int line = 0;
};

QString canonicalOrAbsolutePath(const QString &path);
bool readTextLines(const QString &path, QStringList *lines, QString *error);
bool textFileHasTrailingNewline(const QString &path);
bool readSourceText(const QString &path, QString *text, bool *trailingNewline, QString *error);
bool writeSourceText(const QString &path, const QString &text, bool trailingNewline, QString *error);

QString sha256(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString fileSha256(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash digest(QCryptographicHash::Sha256);
    return digest.addData(&file) ? QString::fromLatin1(digest.result().toHex()) : QString();
}

QString commentSyncLockPath(const QString &docsRoot, const QString &stem)
{
    const QByteArray identity = QFileInfo(docsRoot).absoluteFilePath().toUtf8() + '\0' + stem.toUtf8();
    const QString lockRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/comment-sync-locks"));
    return QDir(lockRoot).filePath(QStringLiteral("comment-sync-%1.lock").arg(sha256(identity).left(16)));
}

QJsonValue normalizedJson(const QJsonValue &value)
{
    if (value.isDouble()) {
        const double rounded = std::round(value.toDouble() * 1000000.0) / 1000000.0;
        return rounded == 0.0 ? 0.0 : rounded;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue &item : value.toArray()) {
            result.append(normalizedJson(item));
        }
        return result;
    }
    if (value.isObject()) {
        QJsonObject result;
        const QJsonObject object = value.toObject();
        QStringList keys = object.keys();
        std::sort(keys.begin(), keys.end());
        for (const QString &key : keys) {
            result.insert(key, normalizedJson(object.value(key)));
        }
        return result;
    }
    return value;
}

QByteArray compactJson(const QJsonValue &value)
{
    if (value.isObject()) {
        return QJsonDocument(normalizedJson(value).toObject()).toJson(QJsonDocument::Compact);
    }
    if (value.isArray()) {
        return QJsonDocument(normalizedJson(value).toArray()).toJson(QJsonDocument::Compact);
    }
    QJsonArray wrapper;
    wrapper.append(normalizedJson(value));
    QByteArray encoded = QJsonDocument(wrapper).toJson(QJsonDocument::Compact);
    return encoded.mid(1, encoded.size() - 2);
}

QString commentKey(const QJsonObject &comment)
{
    const QString reviewKey = comment.value(QStringLiteral("review_key")).toString();
    if (!reviewKey.isEmpty()) {
        return reviewKey;
    }
    const QString id = comment.value(QStringLiteral("id")).toString();
    if (!id.isEmpty()) {
        return QStringLiteral("id:") + id;
    }

    QJsonObject payload;
    for (const QString &field : SyntheticKeyFields) {
        payload.insert(field, normalizedJson(comment.value(field)));
    }
    return QStringLiteral("synthetic:") + sha256(compactJson(payload));
}

KeyedComments keyedComments(const QList<QJsonObject> &comments)
{
    KeyedComments result;
    QHash<QString, int> counts;
    for (const QJsonObject &comment : comments) {
        const QString baseKey = commentKey(comment);
        const int count = counts.value(baseKey) + 1;
        counts.insert(baseKey, count);
        const QString key = count == 1 ? baseKey : baseKey + QStringLiteral("#duplicate-") + QString::number(count);
        result.values.insert(key, comment);
        result.order.append(key);
    }
    return result;
}

QJsonObject semanticComment(const QJsonObject &comment)
{
    QJsonObject semantic;
    for (auto it = comment.begin(); it != comment.end(); ++it) {
        if (!VolatileFields.contains(it.key())) {
            semantic.insert(it.key(), normalizedJson(it.value()));
        }
    }
    return semantic;
}

QDateTime parsePdfDate(const QJsonValue &value)
{
    return DocsCommentSynchronizer::parseCommentDate(value);
}

QDateTime commentTimestamp(const QJsonObject &comment)
{
    QDateTime value = parsePdfDate(comment.value(QStringLiteral("modified")));
    return value.isValid() ? value : parsePdfDate(comment.value(QStringLiteral("created")));
}

QString stableRecordId(const QJsonObject &record)
{
    QString value = record.value(QStringLiteral("id")).toString();
    if (value.isEmpty()) {
        value = record.value(QStringLiteral("review_key")).toString();
    }
    if (!value.isEmpty()) {
        return value;
    }
    QJsonObject identity;
    for (const QString &field : {
             QStringLiteral("page"),
             QStringLiteral("subtype"),
             QStringLiteral("rect"),
             QStringLiteral("quad_points"),
             QStringLiteral("fragments"),
             QStringLiteral("author"),
             QStringLiteral("created"),
             QStringLiteral("contents"),
         }) {
        identity.insert(field, record.value(field));
    }
    return sha256(compactJson(identity));
}

bool hasLegacyMjColor(const QJsonObject &record)
{
    const QJsonArray color = record.value(QStringLiteral("color")).toArray();
    if (color.size() < 3) {
        return false;
    }
    const auto channel = [&color](int index) {
        const double value = color.at(index).toDouble();
        return value > 1.0 ? value / 255.0 : value;
    };
    return std::abs(channel(0) - 250.0 / 255.0) < 0.00001 && std::abs(channel(1) - 217.0 / 255.0) < 0.00001 && std::abs(channel(2)) < 0.00001;
}

QString inferredCreatorApplication(const QJsonObject &record)
{
    const QString explicitCreator = record.value(QStringLiteral("created_in_app")).toString();
    if (explicitCreator == ThoughtsCreator || explicitCreator == MjPdfCreator) {
        return explicitCreator;
    }
    const QString id = record.value(QStringLiteral("id")).toString();
    if (id.startsWith(QLatin1String("okular-"))) {
        return ThoughtsCreator;
    }
    if (id.startsWith(QLatin1String("mjpdf-"))) {
        return MjPdfCreator;
    }
    static const QRegularExpression plainUuid(QStringLiteral("^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$"));
    const int flags = record.value(QStringLiteral("flags")).toInt();
    // A note from the old Android reader (MJ PDF Reader): a plain UUID, its flags and its yellow.
    if (record.value(QStringLiteral("subtype")).toString() == QLatin1String("Text") && plainUuid.match(id).hasMatch()
        && (flags == 28 || flags == 60) && hasLegacyMjColor(record)) {
        return MjPdfCreator;
    }
    return {};
}

QJsonObject upgradedRecord(QJsonObject record, const QString &originId = QStringLiteral("legacy"))
{
    const bool migrateLegacySidecar = record.contains(QStringLiteral("schema_version")) && record.value(QStringLiteral("schema_version")).toInt() < SidecarSchemaVersion;
    record.insert(QStringLiteral("schema_version"), SidecarSchemaVersion);
    record.insert(QStringLiteral("id"), stableRecordId(record));
    const QString creatorApplication = inferredCreatorApplication(record);
    if (!creatorApplication.isEmpty()) {
        record.insert(QStringLiteral("created_in_app"), creatorApplication);
    }
    if (migrateLegacySidecar && record.value(QStringLiteral("subtype")).toString() == QLatin1String("Text") && !record.value(QStringLiteral("deleted")).toBool()) {
        record.insert(QStringLiteral("color"), QJsonArray {1.0, 1.0, 0.0});
    }
    if (!record.contains(QStringLiteral("revision"))) {
        record.insert(QStringLiteral("revision"), 1);
    }
    if (!record.contains(QStringLiteral("updated_at"))) {
        QString timestamp = record.value(QStringLiteral("modified")).toString();
        if (timestamp.isEmpty()) {
            timestamp = record.value(QStringLiteral("created")).toString();
        }
        if (timestamp.isEmpty()) {
            timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        }
        record.insert(QStringLiteral("updated_at"), timestamp);
    }
    if (!record.contains(QStringLiteral("origin_id"))) {
        record.insert(QStringLiteral("origin_id"), originId);
    }
    if (record.value(QStringLiteral("in_reply_to")).isNull()) {
        record.remove(QStringLiteral("in_reply_to"));
    }
    return record;
}

std::tuple<int, qint64, QString> recordRevisionKey(const QJsonObject &raw)
{
    const QJsonObject record = upgradedRecord(raw);
    QDateTime timestamp = parsePdfDate(record.value(QStringLiteral("updated_at")));
    if (!timestamp.isValid()) {
        timestamp = parsePdfDate(record.value(QStringLiteral("deleted_at")));
    }
    if (!timestamp.isValid()) {
        timestamp = commentTimestamp(record);
    }
    return {
        record.value(QStringLiteral("revision")).toInt(),
        timestamp.isValid() ? timestamp.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min(),
        record.value(QStringLiteral("origin_id")).toString(),
    };
}

QDateTime tombstoneTimestamp(const QJsonObject &tombstone)
{
    QDateTime value = parsePdfDate(tombstone.value(QStringLiteral("deleted_at")));
    if (value.isValid()) {
        return value;
    }
    value = parsePdfDate(tombstone.value(QStringLiteral("observed_at")));
    if (value.isValid()) {
        return value;
    }
    return QDateTime::fromString(tombstone.value(QStringLiteral("deleted_at")).toString(), Qt::ISODate).toUTC();
}

QList<QJsonObject> reindexComments(const QList<QJsonObject> &comments)
{
    QList<QJsonObject> result;
    result.reserve(comments.size());
    int index = 1;
    for (QJsonObject comment : comments) {
        comment.insert(QStringLiteral("comment_index"), index++);
        result.append(comment);
    }
    return result;
}

bool readJsonObject(const QString &path, QJsonObject *result, QString *error)
{
    QFile file(path);
    if (!file.exists()) {
        *result = {};
        return true;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (error) {
            *error = parseError.errorString();
        }
        return false;
    }
    *result = document.object();
    return true;
}

bool writeJsonObject(const QString &path, const QJsonObject &object, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("could not create %1").arg(QFileInfo(path).absolutePath());
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    const QByteArray encoded = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(encoded) != encoded.size() || !file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

bool readJsonl(const QString &path, QList<QJsonObject> *comments, QString *error)
{
    comments->clear();
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    int lineNumber = 0;
    while (!file.atEnd()) {
        ++lineNumber;
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            if (error) {
                *error = QStringLiteral("%1:%2: %3").arg(path).arg(lineNumber).arg(parseError.errorString());
            }
            return false;
        }
        comments->append(document.object());
    }
    return true;
}

bool writeJsonl(const QString &path, const QList<QJsonObject> &comments, QString *error)
{
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        if (error) {
            *error = QStringLiteral("could not create %1").arg(QFileInfo(path).absolutePath());
        }
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    for (const QJsonObject &comment : comments) {
        const QByteArray line = compactJson(comment) + '\n';
        if (file.write(line) != line.size()) {
            if (error) {
                *error = file.errorString();
            }
            return false;
        }
    }
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

QFileInfoList syncConflictFiles(const QFileInfo &canonicalInfo, const QString &conflictPattern)
{
#ifdef Q_OS_MACOS
    // glob() only reads directory entries, so synchronization is needed only
    // between callers in this process. A named System V semaphore can remain
    // permanently acquired when an app is terminated, preventing every future
    // catalog reload from completing. A process-local mutex is released with
    // the process and cannot poison a later launch.
    static QMutex conflictScanMutex;
    const QMutexLocker locker(&conflictScanMutex);
    QFileInfoList result;
    glob_t matches {};
    const QByteArray encodedPattern = QFile::encodeName(canonicalInfo.dir().absoluteFilePath(conflictPattern));
    const int status = glob(encodedPattern.constData(), GLOB_NOSORT, nullptr, &matches);
    if (status == 0) {
        result.reserve(static_cast<qsizetype>(matches.gl_pathc));
        for (size_t index = 0; index < matches.gl_pathc; ++index) {
            const QFileInfo info(QFile::decodeName(matches.gl_pathv[index]));
            if (info.isFile()) {
                result.append(info);
            }
        }
    }
    globfree(&matches);
    std::sort(result.begin(), result.end(), [](const QFileInfo &left, const QFileInfo &right) { return left.fileName() < right.fileName(); });
    return result;
#else
    return canonicalInfo.dir().entryInfoList({conflictPattern}, QDir::Files, QDir::Name);
#endif
}

bool reconcileJsonl(const QString &path, QList<QJsonObject> *records, QString *error, int *conflictCount = nullptr)
{
    const QFileInfo canonicalInfo(path);
    const QString conflictPattern = canonicalInfo.completeBaseName() + QStringLiteral(".sync-conflict-*.jsonl");
    const QFileInfoList conflictInfos = syncConflictFiles(canonicalInfo, conflictPattern);
    QList<QString> sources {path};
    for (const QFileInfo &info : conflictInfos) {
        sources.append(info.absoluteFilePath());
    }

    QHash<QString, QJsonObject> merged;
    QStringList order;
    QList<QJsonObject> losing;
    for (const QString &source : std::as_const(sources)) {
        QList<QJsonObject> sourceRecords;
        if (!readJsonl(source, &sourceRecords, error)) {
            return false;
        }
        for (const QJsonObject &raw : std::as_const(sourceRecords)) {
            const QJsonObject record = upgradedRecord(raw);
            const QString id = stableRecordId(record);
            if (!merged.contains(id)) {
                merged.insert(id, record);
                order.append(id);
                continue;
            }
            const QJsonObject current = merged.value(id);
            if (compactJson(current) == compactJson(record)) {
                continue;
            }
            if (recordRevisionKey(record) > recordRevisionKey(current)) {
                losing.append(current);
                merged.insert(id, record);
            } else {
                losing.append(record);
            }
        }
    }

    records->clear();
    records->reserve(order.size());
    for (const QString &id : std::as_const(order)) {
        records->append(merged.value(id));
    }
    std::sort(records->begin(), records->end(), [](const QJsonObject &left, const QJsonObject &right) {
        const int leftPage = left.value(QStringLiteral("page")).toInt();
        const int rightPage = right.value(QStringLiteral("page")).toInt();
        return leftPage == rightPage ? stableRecordId(left) < stableRecordId(right) : leftPage < rightPage;
    });

    if (!conflictInfos.isEmpty() || !losing.isEmpty()) {
        const QString archiveDir = canonicalInfo.dir().filePath(QStringLiteral(".conflicts/") + canonicalInfo.completeBaseName());
        if (!QDir().mkpath(archiveDir)) {
            if (error) {
                *error = QStringLiteral("could not create %1").arg(archiveDir);
            }
            return false;
        }
        int sequence = 0;
        for (const QJsonObject &record : std::as_const(losing)) {
            QString safeId = stableRecordId(record);
            safeId.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9._-]")), QStringLiteral("_"));
            const QString archivePath = QDir(archiveDir).filePath(QStringLiteral("%1-%2-%3.json").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz"))).arg(++sequence).arg(safeId));
            if (!writeJsonObject(archivePath, record, error)) {
                return false;
            }
        }
        if (!writeJsonl(path, *records, error)) {
            return false;
        }
        for (const QFileInfo &info : conflictInfos) {
            if (!QFile::remove(info.absoluteFilePath()) && QFileInfo::exists(info.absoluteFilePath())) {
                if (error) {
                    *error = QStringLiteral("could not remove reconciled conflict %1").arg(info.absoluteFilePath());
                }
                return false;
            }
        }
    }
    if (conflictCount) {
        *conflictCount = losing.size();
    }
    return true;
}

// reconcileJsonl() of a file without sync conflicts, kept while the file is
// unchanged: such a file is only read (a merge of duplicate records writes it,
// which changes it, so that result is not kept). The Library reads every
// sidecar and the shared review_resolutions.jsonl once per document and load.
struct ReconciledJsonl {
    bool ok = false;
    QList<QJsonObject> records;
    QString error;
    qint64 bytes = 0;
};

bool reconcileJsonlKept(const QString &path, QList<QJsonObject> *records, QString *error)
{
    const QFileInfo canonicalInfo(path);
    if (!syncConflictFiles(canonicalInfo, canonicalInfo.completeBaseName() + QStringLiteral(".sync-conflict-*.jsonl")).isEmpty()) {
        return reconcileJsonl(path, records, error);
    }
    static Thoughts::FileIdentityCache<ReconciledJsonl> cache(96 * 1024 * 1024);
    const ReconciledJsonl result = cache.value(
        path,
        [](const QString &file) {
            ReconciledJsonl reconciled;
            reconciled.bytes = QFileInfo(file).size();
            reconciled.ok = reconcileJsonl(file, &reconciled.records, &reconciled.error);
            return reconciled;
        },
        [](const ReconciledJsonl &reconciled) { return qsizetype(reconciled.bytes) * Thoughts::ParsedJsonHeapFactor; });
    *records = result.records;
    if (!result.ok && error) {
        *error = result.error;
    }
    return result.ok;
}

QJsonObject metadataFrom(const QJsonObject &comment)
{
    QJsonObject result;
    for (const QString &field : ToolMetadataFields) {
        if (comment.contains(field)) {
            result.insert(field, comment.value(field));
        }
    }
    return result;
}

QList<QJsonObject> overlayToolMetadata(const QList<QJsonObject> &comments, const QList<QJsonObject> &metadataSources)
{
    QHash<QString, QJsonObject> byKey;
    for (const QJsonObject &source : metadataSources) {
        QJsonObject metadata = metadataFrom(source);
        if (!metadata.isEmpty()) {
            QJsonObject combined = byKey.value(commentKey(source));
            for (auto it = metadata.begin(); it != metadata.end(); ++it) {
                combined.insert(it.key(), it.value());
            }
            byKey.insert(commentKey(source), combined);
        }
    }

    QList<QJsonObject> result;
    result.reserve(comments.size());
    for (QJsonObject comment : comments) {
        const QJsonObject metadata = byKey.value(commentKey(comment));
        for (auto it = metadata.begin(); it != metadata.end(); ++it) {
            comment.insert(it.key(), it.value());
        }
        result.append(comment);
    }
    return result;
}

QJsonArray pdfRectFromSnapshot(const QJsonArray &rect, double width, double height)
{
    if (rect.size() != 4) {
        return {};
    }
    const double x0 = rect.at(0).toDouble() * width;
    const double y0 = (1.0 - rect.at(1).toDouble()) * height;
    const double x1 = rect.at(2).toDouble() * width;
    const double y1 = (1.0 - rect.at(3).toDouble()) * height;
    return {std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1)};
}

QJsonArray pdfQuadsFromSnapshot(const QJsonArray &points, double width, double height)
{
    QJsonArray result;
    for (qsizetype offset = 0; offset + 1 < points.size(); offset += 2) {
        result.append(points.at(offset).toDouble() * width);
        result.append((1.0 - points.at(offset + 1).toDouble()) * height);
    }
    return result;
}

std::optional<QJsonObject> pdfFragmentFromSnapshot(const QJsonObject &raw, QString *error)
{
    const double width = raw.value(QStringLiteral("page_width")).toDouble();
    const double height = raw.value(QStringLiteral("page_height")).toDouble();
    const int page = raw.value(QStringLiteral("page")).toInt();
    const QJsonArray rect = pdfRectFromSnapshot(raw.value(QStringLiteral("rect")).toArray(), width, height);
    if (!(width > 0.0) || !(height > 0.0) || page < 1 || rect.size() != 4) {
        if (error) {
            *error = QStringLiteral("annotation snapshot contains an invalid comment fragment");
        }
        return std::nullopt;
    }
    return QJsonObject {
        {QStringLiteral("page"), page},
        {QStringLiteral("rect"), rect},
        {QStringLiteral("quad_points"),
         raw.value(QStringLiteral("quad_points")).isArray() ? QJsonValue(pdfQuadsFromSnapshot(raw.value(QStringLiteral("quad_points")).toArray(), width, height))
                                                            : QJsonValue(QJsonValue::Null)},
    };
}

QList<QJsonObject> commentsFromSnapshot(const QJsonObject &snapshot, const QString &pdfPath, QString *error)
{
    const QJsonArray rawComments = snapshot.value(QStringLiteral("comments")).toArray();
    QList<QJsonObject> comments;
    comments.reserve(rawComments.size());
    int index = 1;
    for (const QJsonValue &value : rawComments) {
        if (!value.isObject()) {
            if (error) {
                *error = QStringLiteral("annotation snapshot contains a non-object comment");
            }
            return {};
        }
        const QJsonObject raw = value.toObject();
        const double width = raw.value(QStringLiteral("page_width")).toDouble();
        const double height = raw.value(QStringLiteral("page_height")).toDouble();
        if (!(width > 0.0) || !(height > 0.0)) {
            if (error) {
                *error = QStringLiteral("annotation snapshot is missing page dimensions");
            }
            return {};
        }

        QJsonObject comment {
            {QStringLiteral("comment_index"), index++},
            {QStringLiteral("source_pdf"), pdfPath},
            {QStringLiteral("page"), raw.value(QStringLiteral("page"))},
            {QStringLiteral("subtype"), raw.value(QStringLiteral("subtype"))},
            {QStringLiteral("object_id"), QJsonArray {QJsonValue::Null, QJsonValue::Null}},
            {QStringLiteral("id"), raw.value(QStringLiteral("id"))},
            {QStringLiteral("rect"), pdfRectFromSnapshot(raw.value(QStringLiteral("rect")).toArray(), width, height)},
            {QStringLiteral("quad_points"), raw.value(QStringLiteral("quad_points")).isArray() ? QJsonValue(pdfQuadsFromSnapshot(raw.value(QStringLiteral("quad_points")).toArray(), width, height)) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("author"), raw.value(QStringLiteral("author"))},
            {QStringLiteral("modified"), raw.value(QStringLiteral("modified"))},
            {QStringLiteral("created"), raw.value(QStringLiteral("created"))},
            {QStringLiteral("contents"), raw.value(QStringLiteral("contents"))},
            {QStringLiteral("selected_text"), raw.value(QStringLiteral("selected_text"))},
            {QStringLiteral("annotation_pdf_sha256"), raw.value(QStringLiteral("annotation_pdf_sha256"))},
            {QStringLiteral("color"), raw.value(QStringLiteral("color"))},
            {QStringLiteral("flags"), raw.value(QStringLiteral("flags"))},
            {QStringLiteral("opacity"), raw.value(QStringLiteral("opacity"))},
            {QStringLiteral("name"), raw.value(QStringLiteral("name"))},
            {QStringLiteral("_normalized_rect"), raw.value(QStringLiteral("rect"))},
            {QStringLiteral("_normalized_quad_points"), raw.value(QStringLiteral("quad_points"))},
            {QStringLiteral("_page_width"), width},
            {QStringLiteral("_page_height"), height},
            {QStringLiteral("_page_rotation"), raw.value(QStringLiteral("page_rotation"))},
        };
        // Some metadata has no standard PDF annotation field.  Preserve it
        // when the producer supplied it in the in-memory snapshot instead of
        // relying solely on a later sidecar overlay.  This is especially
        // important for immutable creation provenance on newly materialized
        // MJ PDF comments.
        for (const QString &field : ToolMetadataFields) {
            if (raw.contains(field)) {
                comment.insert(field, raw.value(field));
            }
        }

        const QJsonArray rawFragments = raw.value(QStringLiteral("fragments")).toArray();
        if (!rawFragments.isEmpty()) {
            if (rawFragments.size() > 1
                && (raw.value(QStringLiteral("subtype")).toString() == QLatin1String("Text")
                    || raw.value(QStringLiteral("id")).toString().isEmpty())) {
                if (error) {
                    *error = QStringLiteral("annotation snapshot contains an invalid logical fragment group");
                }
                return {};
            }
            QJsonArray fragments;
            int expectedPage = raw.value(QStringLiteral("page")).toInt();
            for (const QJsonValue &value : rawFragments) {
                if (!value.isObject()) {
                    if (error) {
                        *error = QStringLiteral("annotation snapshot contains a non-object comment fragment");
                    }
                    return {};
                }
                const std::optional<QJsonObject> fragment = pdfFragmentFromSnapshot(value.toObject(), error);
                if (!fragment) {
                    return {};
                }
                if (fragment->value(QStringLiteral("page")).toInt() != expectedPage) {
                    if (error) {
                        *error = QStringLiteral("annotation snapshot comment fragments must occupy consecutive pages");
                    }
                    return {};
                }
                fragments.append(*fragment);
                ++expectedPage;
            }
            const QJsonObject first = fragments.at(0).toObject();
            comment.insert(QStringLiteral("page"), first.value(QStringLiteral("page")));
            comment.insert(QStringLiteral("rect"), first.value(QStringLiteral("rect")));
            comment.insert(QStringLiteral("quad_points"), first.value(QStringLiteral("quad_points")));
            comment.insert(QStringLiteral("fragments"), fragments);
        }
        comments.append(comment);
    }
    return comments;
}

void removePrivateFields(QJsonObject *comment)
{
    comment->remove(QStringLiteral("_normalized_rect"));
    comment->remove(QStringLiteral("_normalized_quad_points"));
    comment->remove(QStringLiteral("_page_width"));
    comment->remove(QStringLiteral("_page_height"));
    comment->remove(QStringLiteral("_page_rotation"));
}

bool isDeletedRecord(const QJsonObject &record)
{
    return record.value(QStringLiteral("deleted")).toBool();
}

QList<QJsonObject> liveRecords(const QList<QJsonObject> &records)
{
    QList<QJsonObject> live;
    live.reserve(records.size());
    for (const QJsonObject &record : records) {
        if (!isDeletedRecord(record)) {
            live.append(record);
        }
    }
    return live;
}

QJsonValue effectivePdfPayloadValue(const QJsonObject &record, const QString &field)
{
    if (field == QLatin1String("author") || field == QLatin1String("modified") || field == QLatin1String("created")) {
        const QString value = record.value(field).toString();
        return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
    }
    if (field == QLatin1String("quad_points") || field == QLatin1String("in_reply_to")) {
        return record.value(field).isArray() || record.value(field).isString() ? record.value(field) : QJsonValue(QJsonValue::Null);
    }
    if (field == QLatin1String("color")) {
        const QJsonArray source = record.value(field).toArray();
        if (source.size() < 3) {
            return QJsonValue(QJsonValue::Null);
        }
        QJsonArray normalized;
        for (int index = 0; index < 3; ++index) {
            const double channel = source.at(index).toDouble();
            normalized.append(std::clamp(channel > 1.0 ? channel / 255.0 : channel, 0.0, 1.0));
        }
        return normalized;
    }
    if (field == QLatin1String("flags")) {
        return record.contains(field) ? record.value(field) : QJsonValue(4);
    }
    if (field == QLatin1String("opacity")) {
        return std::clamp(record.value(field).toDouble(1.0), 0.0, 1.0);
    }
    if (field == QLatin1String("name")) {
        if (record.value(QStringLiteral("subtype")).toString() == QLatin1String("Text")) {
            return QStringLiteral("Comment");
        }
        return record.value(field).isString() ? record.value(field) : QJsonValue(QJsonValue::Null);
    }
    return record.value(field);
}

bool pdfPayloadChanged(const QJsonObject &left, const QJsonObject &right)
{
    for (const QString &field : {
             QStringLiteral("page"),
             QStringLiteral("subtype"),
             QStringLiteral("rect"),
             QStringLiteral("quad_points"),
             QStringLiteral("fragments"),
             QStringLiteral("author"),
             QStringLiteral("modified"),
             QStringLiteral("created"),
             QStringLiteral("contents"),
             QStringLiteral("color"),
             QStringLiteral("flags"),
             QStringLiteral("opacity"),
             QStringLiteral("name"),
         }) {
        if (compactJson(normalizedJson(effectivePdfPayloadValue(left, field))) != compactJson(normalizedJson(effectivePdfPayloadValue(right, field)))) {
            return true;
        }
    }
    return isDeletedRecord(left) != isDeletedRecord(right);
}

QJsonObject revisionedExternalComment(QJsonObject comment, const std::optional<QJsonObject> &previous)
{
    removePrivateFields(&comment);
    comment = upgradedRecord(comment, QStringLiteral("mac-docs"));
    if (!previous) {
        return comment;
    }
    // PDF snapshots cannot represent all sidecar-only fields.  Carry any
    // missing metadata forward defensively even if an earlier overlay did not
    // supply it; values present in the current snapshot remain authoritative.
    const bool authoritativeAnchorState = comment.contains(QStringLiteral("anchor_status"));
    for (const QString &field : ToolMetadataFields) {
        if (authoritativeAnchorState && AnchorMetadataFields.contains(field)) {
            continue;
        }
        if (!comment.contains(field) && previous->contains(field)) {
            comment.insert(field, previous->value(field));
        }
    }
    if (!pdfPayloadChanged(*previous, comment)) {
        for (const QString &field : {
                 QStringLiteral("revision"),
                 QStringLiteral("updated_at"),
                 QStringLiteral("origin_id"),
             }) {
            if (previous->contains(field)) {
                comment.insert(field, previous->value(field));
            }
        }
        return comment;
    }
    const int revision = std::max(1, previous->value(QStringLiteral("revision")).toInt(1)) + 1;
    QString timestamp = comment.value(QStringLiteral("modified")).toString();
    if (timestamp.isEmpty()) {
        timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    }
    comment.insert(QStringLiteral("revision"), revision);
    comment.insert(QStringLiteral("updated_at"), timestamp);
    comment.insert(QStringLiteral("origin_id"), QStringLiteral("mac-docs"));
    comment.remove(QStringLiteral("deleted"));
    comment.remove(QStringLiteral("deleted_at"));
    return comment;
}

QJsonObject sidecarTombstone(QJsonObject previous, const QJsonObject &tombstone)
{
    removePrivateFields(&previous);
    const QString timestamp = tombstone.value(QStringLiteral("deleted_at")).toString(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    QString stableId = previous.value(QStringLiteral("id")).toString();
    if (stableId.isEmpty()) {
        stableId = previous.value(QStringLiteral("review_key")).toString();
    }
    if (stableId.isEmpty()) {
        stableId = sha256(compactJson(semanticComment(previous)));
    }
    previous.insert(QStringLiteral("schema_version"), SidecarSchemaVersion);
    previous.insert(QStringLiteral("id"), stableId);
    previous.insert(QStringLiteral("revision"), std::max(1, previous.value(QStringLiteral("revision")).toInt(1)) + 1);
    previous.insert(QStringLiteral("updated_at"), timestamp);
    previous.insert(QStringLiteral("deleted"), true);
    previous.insert(QStringLiteral("deleted_at"), timestamp);
    const bool sourceAnchorDeletion = tombstone.value(QStringLiteral("source_anchor_deletion")).toBool();
    previous.insert(QStringLiteral("origin_id"), sourceAnchorDeletion ? QStringLiteral("mac-tex-anchor") : QStringLiteral("mac-docs"));
    if (sourceAnchorDeletion) {
        previous.insert(QStringLiteral("source_anchor_deletion"), true);
        previous.insert(QStringLiteral("deletion_reason"), QStringLiteral("source_anchor_removed"));
    }
    return previous;
}

// A marker that vanished without an explicit Delete orphans the placement. The
// record stays live with its last known anchor so that the comment can be restored.
void markSidecarAnchorRemoved(QJsonObject *record, const QString &originId)
{
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    record->insert(QStringLiteral("anchor_status"), QStringLiteral("orphaned"));
    record->insert(QStringLiteral("anchor_confidence"), QStringLiteral("low"));
    record->insert(QStringLiteral("orphan_reason"), QStringLiteral("anchor_removed"));
    record->insert(QStringLiteral("orphaned_at"), timestamp);
    record->insert(QStringLiteral("revision"), std::max(1, record->value(QStringLiteral("revision")).toInt(1)) + 1);
    record->insert(QStringLiteral("updated_at"), timestamp);
    record->insert(QStringLiteral("origin_id"), originId);
    for (const QString &field : {QStringLiteral("deleted"), QStringLiteral("deleted_at"), QStringLiteral("deletion_reason"), QStringLiteral("source_anchor_deletion")}) {
        record->remove(field);
    }
}

void clearSidecarOrphanReason(QJsonObject *record)
{
    record->remove(QStringLiteral("orphan_reason"));
    record->remove(QStringLiteral("orphaned_at"));
}

QList<QJsonObject> resolutionTombstones(const QList<QJsonObject> &records)
{
    QList<QJsonObject> result;
    for (const QJsonObject &record : records) {
        if (!isDeletedRecord(record) || (!record.value(QStringLiteral("source_anchor_deletion")).toBool() && record.value(QStringLiteral("origin_id")).toString() != QLatin1String("mac-tex-anchor"))) {
            continue;
        }
        result.append(QJsonObject {
            {QStringLiteral("comment_key"), record.value(QStringLiteral("comment_key")).toString(commentKey(record))},
            {QStringLiteral("deleted_at"), record.value(QStringLiteral("deleted_at")).toString(record.value(QStringLiteral("updated_at")).toString())},
            {QStringLiteral("source_anchor_deletion"), true},
            {QStringLiteral("deletion_reason"), QStringLiteral("source_anchor_removed")},
        });
    }
    return result;
}

bool hasResolution(const QList<QJsonObject> &records, const QString &id)
{
    return std::any_of(records.cbegin(), records.cend(), [&id](const QJsonObject &record) { return stableRecordId(record) == id && isDeletedRecord(record) && record.value(QStringLiteral("source_anchor_deletion")).toBool(); });
}

bool appendResolution(QList<QJsonObject> *records, QJsonObject record)
{
    const QString id = stableRecordId(record);
    if (id.isEmpty() || hasResolution(*records, id)) {
        return false;
    }
    int existingIndex = -1;
    int existingRevision = 0;
    for (int index = 0; index < records->size(); ++index) {
        if (stableRecordId(records->at(index)) == id) {
            existingIndex = index;
            existingRevision = records->at(index).value(QStringLiteral("revision")).toInt();
            break;
        }
    }
    record = upgradedRecord(record, QStringLiteral("mac-tex-anchor"));
    record.insert(QStringLiteral("comment_key"), record.value(QStringLiteral("comment_key")).toString(QStringLiteral("id:") + id));
    record.insert(QStringLiteral("deleted"), true);
    record.insert(QStringLiteral("source_anchor_deletion"), true);
    record.insert(QStringLiteral("deletion_reason"), QStringLiteral("source_anchor_removed"));
    record.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-anchor"));
    record.insert(QStringLiteral("revision"), std::max(record.value(QStringLiteral("revision")).toInt(1), existingRevision + 1));
    if (existingIndex >= 0) {
        (*records)[existingIndex] = record;
    } else {
        records->append(record);
    }
    return true;
}

bool importSidecarTombstones(QJsonObject *state, QList<QJsonObject> *resolutionRecords, const QList<QJsonObject> &deletedRecords)
{
    QJsonObject stored = state->value(QStringLiteral("tombstones")).toObject();
    bool registryChanged = false;
    for (const QJsonObject &record : deletedRecords) {
        const QString key = commentKey(record);
        if (key.isEmpty()) {
            continue;
        }
        const bool sourceAnchorDeletion = record.value(QStringLiteral("source_anchor_deletion")).toBool() || record.value(QStringLiteral("origin_id")).toString() == QLatin1String("mac-tex-anchor");
        QJsonObject tombstone = stored.value(key).toObject();
        if (tombstone.isEmpty()) {
            QJsonObject previous = record;
            previous.remove(QStringLiteral("deleted"));
            previous.remove(QStringLiteral("deleted_at"));
            tombstone = {
                {QStringLiteral("comment_key"), key},
                {QStringLiteral("source_id"), QStringLiteral("sidecar")},
                {QStringLiteral("source_role"), QStringLiteral("sidecar")},
                {QStringLiteral("deleted_at"), record.value(QStringLiteral("deleted_at")).toString(record.value(QStringLiteral("updated_at")).toString())},
                {QStringLiteral("same_source_deletion"), true},
                {QStringLiteral("previous_annotation"), previous},
            };
        }
        if (sourceAnchorDeletion) {
            tombstone.insert(QStringLiteral("source_anchor_deletion"), true);
            tombstone.insert(QStringLiteral("deletion_reason"), QStringLiteral("source_anchor_removed"));
            QJsonObject resolution = record;
            resolution.insert(QStringLiteral("comment_key"), key);
            resolution.insert(QStringLiteral("source_anchor_deletion"), true);
            registryChanged = appendResolution(resolutionRecords, resolution) || registryChanged;
        }
        stored.insert(key, tombstone);
    }
    state->insert(QStringLiteral("tombstones"), stored);
    return registryChanged;
}

QString canonicalStem(const QFileInfo &pdf)
{
    return pdf.completeBaseName();
}

QJsonObject defaultState()
{
    return {
        {QStringLiteral("version"), StateVersion},
        {QStringLiteral("sources"), QJsonObject()},
        {QStringLiteral("tombstones"), QJsonObject()},
        {QStringLiteral("source_mirrors"), QJsonObject()},
    };
}

QJsonObject loadedState(const QJsonObject &value)
{
    QJsonObject state = value.isEmpty() ? defaultState() : value;
    state.insert(QStringLiteral("version"), StateVersion);
    if (!state.value(QStringLiteral("sources")).isObject()) {
        state.insert(QStringLiteral("sources"), QJsonObject());
    }
    if (!state.value(QStringLiteral("tombstones")).isObject()) {
        state.insert(QStringLiteral("tombstones"), QJsonObject());
    }
    if (!state.value(QStringLiteral("source_mirrors")).isObject()) {
        state.insert(QStringLiteral("source_mirrors"), QJsonObject());
    }
    return state;
}

QString observedAt(const QString &path)
{
    const QFileInfo info(path);
    const QDateTime time = info.exists() ? info.lastModified().toUTC() : QDateTime::currentDateTimeUtc();
    return time.toString(Qt::ISODateWithMs);
}

QJsonObject sourceAnnotationSummary(const QJsonObject &comment)
{
    QJsonObject result;
    for (const QString &field : {
             QStringLiteral("modified"),
             QStringLiteral("created"),
             QStringLiteral("page"),
             QStringLiteral("subtype"),
             QStringLiteral("review_key"),
             QStringLiteral("anchor_id"),
             QStringLiteral("anchor_status"),
             QStringLiteral("tex_file"),
         }) {
        result.insert(field, comment.value(field));
    }
    return result;
}

QList<QJsonObject> stateTombstones(const QJsonObject &state)
{
    QList<QJsonObject> result;
    const QJsonObject tombstones = state.value(QStringLiteral("tombstones")).toObject();
    QStringList keys = tombstones.keys();
    std::sort(keys.begin(), keys.end());
    for (const QString &key : keys) {
        if (tombstones.value(key).isObject()) {
            result.append(tombstones.value(key).toObject());
        }
    }
    return result;
}

bool tombstoneDeletesComment(const QJsonObject &tombstone, const QJsonObject &comment)
{
    if (tombstone.value(QStringLiteral("source_anchor_deletion")).toBool()) {
        return true;
    }
    const QDateTime deleted = tombstoneTimestamp(tombstone);
    const QDateTime changed = commentTimestamp(comment);
    return !deleted.isValid() || !changed.isValid() || deleted >= changed;
}

QList<QJsonObject> applyTombstones(const QList<QJsonObject> &comments, const QList<QJsonObject> &tombstones)
{
    QHash<QString, QList<QJsonObject>> byKey;
    for (const QJsonObject &tombstone : tombstones) {
        const QString key = tombstone.value(QStringLiteral("comment_key")).toString(tombstone.value(QStringLiteral("key")).toString());
        if (!key.isEmpty()) {
            byKey[key].append(tombstone);
        }
    }
    QList<QJsonObject> kept;
    const KeyedComments keyed = keyedComments(comments);
    for (const QString &key : keyed.order) {
        const QJsonObject comment = keyed.values.value(key);
        bool deleted = false;
        for (const QJsonObject &tombstone : byKey.value(key)) {
            if (tombstoneDeletesComment(tombstone, comment)) {
                deleted = true;
                break;
            }
        }
        if (!deleted) {
            kept.append(comment);
        }
    }
    return reindexComments(kept);
}

QList<QJsonObject> observeSource(QJsonObject *state, const QString &role, const QString &pdfPath, const QList<QJsonObject> &comments, const QHash<QString, QJsonObject> &previousFullComments, bool allowDeletions)
{
    QJsonObject sources = state->value(QStringLiteral("sources")).toObject();
    const QString sourceIdentifier = role + QLatin1Char(':') + QFileInfo(pdfPath).fileName();
    const QJsonObject previousSource = sources.value(sourceIdentifier).toObject();
    const QJsonObject previousAnnotations = previousSource.value(QStringLiteral("annotations")).toObject();
    const KeyedComments current = keyedComments(comments);
    const QString observationTime = observedAt(pdfPath);

    QList<QJsonObject> createdTombstones;
    QJsonObject storedTombstones = state->value(QStringLiteral("tombstones")).toObject();
    if (allowDeletions) {
        for (const QString &key : previousAnnotations.keys()) {
            if (current.values.contains(key) || storedTombstones.contains(key)) {
                continue;
            }
            const QJsonObject previous = previousFullComments.value(key, previousAnnotations.value(key).toObject());
            QJsonObject tombstone {
                {QStringLiteral("comment_key"), key},
                {QStringLiteral("source_id"), sourceIdentifier},
                {QStringLiteral("source_role"), role},
                {QStringLiteral("source_path"), pdfPath},
                {QStringLiteral("deleted_at"), observationTime},
                {QStringLiteral("same_source_deletion"), true},
                {QStringLiteral("previous_annotation"), previous},
            };
            storedTombstones.insert(key, tombstone);
            createdTombstones.append(tombstone);
        }
    }

    QJsonObject annotations;
    for (const QString &key : current.order) {
        annotations.insert(key, sourceAnnotationSummary(current.values.value(key)));
    }
    const QFileInfo pdfInfo(pdfPath);
    const QByteArray annotationBytes = compactJson(QJsonArray::fromVariantList([&comments] {
        QVariantList list;
        list.reserve(comments.size());
        for (const QJsonObject &comment : comments) {
            list.append(comment.toVariantMap());
        }
        return list;
    }()));
    const QJsonObject snapshot {
        {QStringLiteral("path"), pdfPath},
        {QStringLiteral("name"), pdfInfo.fileName()},
        {QStringLiteral("exists"), pdfInfo.exists()},
        {QStringLiteral("size"), pdfInfo.exists() ? QJsonValue(pdfInfo.size()) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("mtime_ns"), pdfInfo.exists() ? QJsonValue(static_cast<double>(pdfInfo.lastModified().toMSecsSinceEpoch()) * 1000000.0) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("observed_at"), observationTime},
        {QStringLiteral("fingerprint"), sha256(annotationBytes)},
        {QStringLiteral("fingerprint_kind"), QStringLiteral("annotations-v1")},
    };
    sources.insert(sourceIdentifier,
                   QJsonObject {
                       {QStringLiteral("source_id"), sourceIdentifier},
                       {QStringLiteral("source_role"), role},
                       {QStringLiteral("snapshot"), snapshot},
                       {QStringLiteral("observed_at"), observationTime},
                       {QStringLiteral("annotations"), annotations},
                   });
    state->insert(QStringLiteral("sources"), sources);
    state->insert(QStringLiteral("tombstones"), storedTombstones);
    return createdTombstones;
}

bool anchorGeometryEqual(const QJsonObject &left, const QJsonObject &right)
{
    for (const QString &field : {
             QStringLiteral("subtype"),
             QStringLiteral("page"),
             QStringLiteral("rect"),
             QStringLiteral("quad_points"),
         }) {
        if (compactJson(left.value(field)) != compactJson(right.value(field))) {
            return false;
        }
    }
    return true;
}

QString safeAnchorId(const QJsonObject &comment, const QString &reviewKey)
{
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._:-]{0,127}$"));
    const QString existing = comment.value(QStringLiteral("anchor_id")).toString().trimmed();
    if (safe.match(existing).hasMatch()) {
        return existing;
    }
    const QString id = comment.value(QStringLiteral("id")).toString().trimmed();
    if (safe.match(id).hasMatch()) {
        return id;
    }
    return QStringLiteral("comment-") + sha256(reviewKey.toUtf8()).left(32);
}

QString canonicalOrAbsolutePath(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return canonical.isEmpty() ? info.absoluteFilePath() : canonical;
}

QString managedProjectRoot(const QString &docsRoot, const QString &sourceId)
{
    // A docs-* folder or a Project's default folder; never a bare first segment.
    const QString projectName = Thoughts::SourceOwner::of(sourceId);
    if (projectName.isEmpty() || !(sourceId.endsWith(QLatin1String(".tex")) || sourceId.endsWith(QLatin1String(".md")) || sourceId.endsWith(QLatin1String(".markdown")))) {
        return {};
    }
    const QString projectRoot = canonicalOrAbsolutePath(QDir(docsRoot).filePath(projectName));
    const QString sourcePath = canonicalOrAbsolutePath(QDir(docsRoot).filePath(sourceId));
    const QString relative = QDir(projectRoot).relativeFilePath(sourcePath);
    return relative == QLatin1String("..") || relative.startsWith(QLatin1String("../")) ? QString() : projectRoot;
}

bool markerEligibleSourcePath(const QString &docsRoot, const QString &sourceId, const QString &path)
{
    static const QSet<QString> excludedDirectories {
        QStringLiteral(".git"),
        QStringLiteral("archive"),
        QStringLiteral("build"),
        QStringLiteral("generated"),
        QStringLiteral("pdf"),
        QStringLiteral("third_party"),
    };
    const QString projectRoot = managedProjectRoot(docsRoot, sourceId);
    if (projectRoot.isEmpty()) {
        return false;
    }
    const QString sourcePath = canonicalOrAbsolutePath(path);
    const QString relative = QDir(projectRoot).relativeFilePath(sourcePath);
    if (relative == QLatin1String("..") || relative.startsWith(QLatin1String("../")) || QFileInfo(sourcePath).suffix().compare(QLatin1String("tex"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    for (const QString &part : relative.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (excludedDirectories.contains(part.toLower())) {
            return false;
        }
    }
    return true;
}

SourceLocation sourceLocationForPage(const QString &docsRoot, const QString &canonicalName, int pageNumber)
{
    QJsonObject layout;
    QString ignored;
    const QString layoutPath = QDir(docsRoot).filePath(QStringLiteral("comments/.layout/%1.json").arg(canonicalName));
    if (!readJsonObject(layoutPath, &layout, &ignored) || layout.value(QStringLiteral("version")).toInt() != 1) {
        return {};
    }
    for (const QJsonValue &value : layout.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = value.toObject();
        const int start = source.value(QStringLiteral("start_page")).toInt();
        const int count = source.value(QStringLiteral("page_count")).toInt();
        if (pageNumber >= start && pageNumber < start + count) {
            const QString sourceId = source.value(QStringLiteral("source_id")).toString();
            const bool isTex = !managedProjectRoot(docsRoot, sourceId).isEmpty();
            return {
                isTex,
                sourceId,
                source.value(QStringLiteral("fingerprint")).toString(),
                pageNumber - start + 1,
                isTex ? QFileInfo(QDir(docsRoot).filePath(sourceId)).absoluteFilePath() : QString(),
            };
        }
    }
    return {};
}

QString buildRegistryKey(const QString &sourceId, const QString &sourceFingerprint)
{
    return sourceId + QChar(0x1f) + sourceFingerprint;
}

BuildRegistry loadBuildRegistry(const QString &docsRoot)
{
    BuildRegistry registry;
    const QString buildsRoot = QDir(docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds"));
    // Build caches are immutable, content-addressed direct children. Walking
    // recursively enters every cached source snapshot and PDF even though the
    // only registry input is <build-id>/mapping.json.
    const QFileInfoList buildDirectories = QDir(buildsRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &buildDirectory : buildDirectories) {
        const QString mappingPath = QDir(buildDirectory.absoluteFilePath()).filePath(QStringLiteral("mapping.json"));
        if (!QFileInfo::exists(mappingPath)) {
            continue;
        }
        QJsonObject mapping;
        QString ignored;
        if (!readJsonObject(mappingPath, &mapping, &ignored)) {
            continue;
        }
        const int mappingVersion = mapping.value(QStringLiteral("version")).toInt();
        if (mappingVersion != 1 && mappingVersion != 2) {
            continue;
        }
        const QString directory = QFileInfo(mappingPath).absolutePath();
        BuildInfo candidate;
        candidate.valid = true;
        candidate.directory = directory;
        candidate.buildId = mapping.value(QStringLiteral("build_id")).toString();
        candidate.sourceId = mapping.value(QStringLiteral("source_id")).toString();
        candidate.sourceFingerprint = mapping.value(QStringLiteral("source_fingerprint")).toString();
        candidate.synctexFingerprint = mapping.value(QStringLiteral("synctex_fingerprint")).toString();
        const QString sourceAbsolute = mapping.value(QStringLiteral("source_tex_absolute")).toString();
        candidate.sourcePath = sourceAbsolute.isEmpty() ? QDir(docsRoot).filePath(candidate.sourceId) : sourceAbsolute;
        candidate.cachedPdf = QDir(directory).filePath(mapping.value(QStringLiteral("pdf_name")).toString(QStringLiteral("document.pdf")));
        candidate.snapshotSource = QDir(directory).filePath(QStringLiteral("source.tex"));
        if (mappingVersion == 2) {
            for (const QJsonValue &value : mapping.value(QStringLiteral("source_snapshots")).toArray()) {
                const QJsonObject entry = value.toObject();
                const QString entrySourceId = entry.value(QStringLiteral("source_id")).toString();
                const QString absolute = entry.value(QStringLiteral("source_tex_absolute")).toString();
                const QString sourcePath = absolute.isEmpty() ? QDir(docsRoot).filePath(entrySourceId) : absolute;
                const QString snapshotPath = QDir(directory).filePath(entry.value(QStringLiteral("snapshot")).toString());
                if (entrySourceId.isEmpty() || !QFileInfo::exists(snapshotPath)) {
                    continue;
                }
                candidate.sourceSnapshots.append({
                    entrySourceId,
                    canonicalOrAbsolutePath(sourcePath),
                    snapshotPath,
                    entry.value(QStringLiteral("input_order")).toInt(),
                    entry.value(QStringLiteral("marker_eligible")).toBool(true),
                });
            }
        }
        if (candidate.sourceSnapshots.isEmpty() && QFileInfo::exists(candidate.snapshotSource)) {
            candidate.sourceSnapshots.append({
                candidate.sourceId,
                canonicalOrAbsolutePath(candidate.sourcePath),
                candidate.snapshotSource,
                0,
                true,
            });
        }
        const QString synctexPath = QDir(directory).filePath(mapping.value(QStringLiteral("synctex_name")).toString(QStringLiteral("document.synctex.gz")));
        if (!QFileInfo::exists(candidate.cachedPdf) || candidate.sourceSnapshots.isEmpty() || !QFileInfo::exists(synctexPath)) {
            continue;
        }
        if (!candidate.buildId.isEmpty()) {
            registry.buildsById.insert(candidate.buildId, candidate);
        }
        if (!candidate.sourceFingerprint.isEmpty() && !registry.ambiguousSourceFingerprints.contains(candidate.sourceFingerprint)) {
            const QString existingSourceId = registry.uniqueSourceIdsByFingerprint.value(candidate.sourceFingerprint);
            if (existingSourceId.isEmpty() || existingSourceId == candidate.sourceId) {
                registry.uniqueSourceIdsByFingerprint.insert(candidate.sourceFingerprint, candidate.sourceId);
            } else {
                registry.uniqueSourceIdsByFingerprint.remove(candidate.sourceFingerprint);
                registry.ambiguousSourceFingerprints.insert(candidate.sourceFingerprint);
            }
        }
        const QString key = buildRegistryKey(candidate.sourceId, candidate.sourceFingerprint);
        if (registry.ambiguousBuilds.contains(key)) {
            continue;
        }
        if (registry.uniqueBuilds.contains(key)) {
            registry.uniqueBuilds.remove(key);
            registry.ambiguousBuilds.insert(key);
            continue;
        }
        registry.uniqueBuilds.insert(key, candidate);
    }
    return registry;
}

BuildRegistry cachedBuildRegistry(const QString &docsRoot)
{
    struct CacheEntry {
        qint64 buildsRootModifiedMs = 0;
        int buildDirectoryCount = 0;
        BuildRegistry registry;
    };
    static QMutex mutex;
    static QHash<QString, CacheEntry> cache;

    const QString normalizedRoot = QFileInfo(docsRoot).absoluteFilePath();
    const QDir buildsDirectory(QDir(normalizedRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds")));
    const QFileInfoList buildDirectories = buildsDirectory.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    bool completeInventory = true;
    for (const QFileInfo &directory : buildDirectories) {
        if (!QFileInfo::exists(QDir(directory.absoluteFilePath()).filePath(QStringLiteral("mapping.json")))) {
            completeInventory = false;
            break;
        }
    }
    const qint64 modifiedMs = QFileInfo(buildsDirectory.absolutePath()).lastModified().toMSecsSinceEpoch();
    if (completeInventory) {
        QMutexLocker locker(&mutex);
        const auto found = cache.constFind(normalizedRoot);
        if (found != cache.cend() && found->buildsRootModifiedMs == modifiedMs && found->buildDirectoryCount == buildDirectories.size()) {
            return found->registry;
        }
    }

    const BuildRegistry registry = loadBuildRegistry(normalizedRoot);
    if (completeInventory) {
        QMutexLocker locker(&mutex);
        cache.insert(normalizedRoot, CacheEntry {modifiedMs, static_cast<int>(buildDirectories.size()), registry});
    }
    return registry;
}

SourceLocation currentSourceForId(const QString &docsRoot, const QString &canonicalName, const QString &sourceId, const QString &sourceFingerprint, int localPage, const BuildRegistry &registry)
{
    if (localPage < 1) {
        return {};
    }
    QJsonObject layout;
    QString ignored;
    const QString layoutPath = QDir(docsRoot).filePath(QStringLiteral("comments/.layout/%1.json").arg(canonicalName));
    if (!readJsonObject(layoutPath, &layout, &ignored) || layout.value(QStringLiteral("version")).toInt() != 1) {
        return {};
    }
    QList<QJsonObject> exactIdMatches;
    QList<QJsonObject> fingerprintMatches;
    QList<QJsonObject> linkedBuildMatches;
    for (const QJsonValue &value : layout.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = value.toObject();
        if (localPage > source.value(QStringLiteral("page_count")).toInt()) {
            continue;
        }
        if (source.value(QStringLiteral("source_id")).toString() == sourceId) {
            exactIdMatches.append(source);
        }
        if (!sourceFingerprint.isEmpty() && source.value(QStringLiteral("fingerprint")).toString() == sourceFingerprint) {
            fingerprintMatches.append(source);
        }
        const QString layoutSourceId = source.value(QStringLiteral("source_id")).toString();
        const QString layoutFingerprint = source.value(QStringLiteral("fingerprint")).toString();
        if (layoutSourceId.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) && !layoutFingerprint.isEmpty() && !registry.ambiguousSourceFingerprints.contains(layoutFingerprint) &&
            registry.uniqueSourceIdsByFingerprint.value(layoutFingerprint) == sourceId) {
            linkedBuildMatches.append(source);
        }
    }
    const QList<QJsonObject> &matches = !exactIdMatches.isEmpty() ? exactIdMatches : (!fingerprintMatches.isEmpty() ? fingerprintMatches : linkedBuildMatches);
    if (matches.size() != 1) {
        return {};
    }
    const QJsonObject source = matches.constFirst();
    const QString sourcePath = QFileInfo(QDir(docsRoot).filePath(sourceId)).absoluteFilePath();
    return {
        markerEligibleSourcePath(docsRoot, sourceId, sourcePath),
        sourceId,
        source.value(QStringLiteral("fingerprint")).toString(),
        localPage,
        sourcePath,
    };
}

HistoricalBuildResolution historicalBuildForComment(const QString &docsRoot, const QString &canonicalName, const QJsonObject &comment, const BuildRegistry &registry)
{
    HistoricalBuildResolution result;
    const QString pdfSha256 = comment.value(QStringLiteral("annotation_pdf_sha256")).toString().trimmed();
    if (pdfSha256.isEmpty()) {
        return result;
    }
    result.requested = true;
    static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
    if (!sha256Pattern.match(pdfSha256).hasMatch()) {
        return result;
    }
    QJsonObject provenance;
    QString ignored;
    const QString provenancePath = QDir(docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/published/%1.json").arg(pdfSha256));
    if (!readJsonObject(provenancePath, &provenance, &ignored) || provenance.value(QStringLiteral("version")).toInt() != 1 || provenance.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256 ||
        QFileInfo(provenance.value(QStringLiteral("pdf_filename")).toString()).completeBaseName() != canonicalName) {
        return result;
    }
    const int page = comment.value(QStringLiteral("page")).toInt();
    for (const QJsonValue &value : provenance.value(QStringLiteral("pages")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("page")).toInt() != page) {
            continue;
        }
        const QString sourceId = entry.value(QStringLiteral("source_id")).toString();
        const QString buildId = entry.value(QStringLiteral("source_build_id")).toString();
        const int localPage = entry.value(QStringLiteral("local_page")).toInt();
        const BuildInfo build = registry.buildsById.value(buildId);
        if (!build.valid || build.sourceId != sourceId) {
            return result;
        }
        const SourceLocation source = currentSourceForId(docsRoot, canonicalName, sourceId, build.sourceFingerprint, localPage, registry);
        if (!source.valid) {
            return result;
        }
        result.valid = true;
        result.source = source;
        result.build = build;
        result.build.sourcePath = source.sourcePath;
        return result;
    }
    return result;
}

BuildInfo buildForSource(const BuildRegistry &registry, const SourceLocation &source)
{
    if (!source.valid) {
        return {};
    }
    BuildInfo build = registry.uniqueBuilds.value(buildRegistryKey(source.sourceId, source.fingerprint));
    if (build.valid) {
        build.sourcePath = source.sourcePath;
    }
    return build;
}

QSet<QString> mappedSourceFilesForDocument(const QString &docsRoot, const QString &canonicalName, const QList<QJsonObject> &comments, QString *error)
{
    QSet<QString> paths;
    QJsonObject layout;
    if (!readJsonObject(QDir(docsRoot).filePath(QStringLiteral("comments/.layout/%1.json").arg(canonicalName)), &layout, error)) {
        return {};
    }

    const BuildRegistry builds = cachedBuildRegistry(docsRoot);
    for (const QJsonValue &value : layout.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject sourceObject = value.toObject();
        const QString sourceId = sourceObject.value(QStringLiteral("source_id")).toString();
        if (sourceId.isEmpty()) {
            continue;
        }
        const QString sourcePath = canonicalOrAbsolutePath(QDir(docsRoot).filePath(sourceId));
        const SourceLocation source {
            true,
            sourceId,
            sourceObject.value(QStringLiteral("fingerprint")).toString(),
            1,
            sourcePath,
        };
        const BuildInfo build = buildForSource(builds, source);
        if (build.valid) {
            for (const SourceSnapshot &snapshot : build.sourceSnapshots) {
                if (snapshot.markerEligible && markerEligibleSourcePath(docsRoot, snapshot.sourceId, snapshot.sourcePath)) {
                    paths.insert(canonicalOrAbsolutePath(snapshot.sourcePath));
                }
            }
        } else if (markerEligibleSourcePath(docsRoot, sourceId, sourcePath)) {
            paths.insert(sourcePath);
        }
    }

    // Per-comment metadata is a durable fallback when a cached build has been
    // pruned, and it also follows markers moved outside the original input
    // manifest after the next source reconciliation.
    for (const QJsonObject &comment : comments) {
        const QString texFile = comment.value(QStringLiteral("tex_file")).toString();
        if (texFile.isEmpty()) {
            continue;
        }
        const QFileInfo info(texFile);
        const QString path = info.isAbsolute() ? info.absoluteFilePath() : QDir(docsRoot).filePath(texFile);
        if (markerEligibleSourcePath(docsRoot, texFile, path)) {
            paths.insert(canonicalOrAbsolutePath(path));
        }
    }
    return paths;
}

QSet<QString> sourceFilesForAnchors(const QString &docsRoot, const QString &canonicalName, const QList<QJsonObject> &comments)
{
    QSet<QString> paths;
    QJsonObject layout;
    QString ignored;
    if (readJsonObject(QDir(docsRoot).filePath(QStringLiteral("comments/.layout/%1.json").arg(canonicalName)), &layout, &ignored)) {
        for (const QJsonValue &value : layout.value(QStringLiteral("sources")).toArray()) {
            const QJsonObject sourceObject = value.toObject();
            const QString sourceId = sourceObject.value(QStringLiteral("source_id")).toString();
            const QString sourcePath = canonicalOrAbsolutePath(QDir(docsRoot).filePath(sourceId));
            if (markerEligibleSourcePath(docsRoot, sourceId, sourcePath)) {
                paths.insert(sourcePath);
            }
        }
    }
    for (const QJsonObject &comment : comments) {
        const QString texFile = comment.value(QStringLiteral("tex_file")).toString();
        if (!texFile.isEmpty()) {
            const QFileInfo info(texFile);
            const QString path = info.isAbsolute() ? info.absoluteFilePath() : QFileInfo(QDir(docsRoot).filePath(texFile)).absoluteFilePath();
            if (markerEligibleSourcePath(docsRoot, texFile, path)) {
                paths.insert(canonicalOrAbsolutePath(path));
            }
        }
    }
    return paths;
}

QSet<QString> batchSourceFilesForAnchors(const QString &docsRoot)
{
    QSet<QString> paths;
    const QString buildsRoot = QDir(docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds"));
    QDirIterator mappings(buildsRoot, QStringList {QStringLiteral("mapping.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (mappings.hasNext()) {
        QJsonObject mapping;
        QString ignored;
        if (!readJsonObject(mappings.next(), &mapping, &ignored)) {
            continue;
        }
        QJsonArray snapshots = mapping.value(QStringLiteral("source_snapshots")).toArray();
        if (snapshots.isEmpty()) {
            snapshots.append(QJsonObject {
                {QStringLiteral("source_id"), mapping.value(QStringLiteral("source_id"))},
                {QStringLiteral("source_tex_absolute"), mapping.value(QStringLiteral("source_tex_absolute"))},
                {QStringLiteral("marker_eligible"), true},
            });
        }
        for (const QJsonValue &value : std::as_const(snapshots)) {
            const QJsonObject snapshot = value.toObject();
            if (!snapshot.value(QStringLiteral("marker_eligible")).toBool(true)) {
                continue;
            }
            const QString sourceId = snapshot.value(QStringLiteral("source_id")).toString();
            const QString absolute = snapshot.value(QStringLiteral("source_tex_absolute")).toString();
            const QString path = absolute.isEmpty() ? QDir(docsRoot).filePath(sourceId) : absolute;
            if (markerEligibleSourcePath(docsRoot, sourceId, path)) {
                paths.insert(canonicalOrAbsolutePath(path));
            }
        }
    }

    const QDir commentsDir(QDir(docsRoot).filePath(QStringLiteral("comments")));
    for (const QFileInfo &sidecar : legacyDocumentSidecars(commentsDir)) {
        QList<QJsonObject> records;
        QString ignored;
        if (!readJsonl(sidecar.absoluteFilePath(), &records, &ignored)) {
            continue;
        }
        for (const QJsonObject &record : std::as_const(records)) {
            const QString texFile = record.value(QStringLiteral("tex_file")).toString();
            if (texFile.isEmpty()) {
                continue;
            }
            const QFileInfo info(texFile);
            const QString path = info.isAbsolute() ? info.absoluteFilePath() : QDir(docsRoot).filePath(texFile);
            if (markerEligibleSourcePath(docsRoot, texFile, path)) {
                paths.insert(canonicalOrAbsolutePath(path));
            }
        }
    }
    return paths;
}

QSet<QString> managedSourceFiles(const QString &docsRoot)
{
    QSet<QString> paths;
    const QDir layoutDir(QDir(docsRoot).filePath(QStringLiteral("comments/.layout")));
    const BuildRegistry builds = cachedBuildRegistry(docsRoot);
    for (const QFileInfo &file : layoutDir.entryInfoList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QJsonObject layout;
        QString error;
        if (!readJsonObject(file.absoluteFilePath(), &layout, &error)) continue;
        for (const QJsonValue &value : layout.value(QStringLiteral("sources")).toArray()) {
            const QJsonObject source = value.toObject();
            const QString sourceId = source.value(QStringLiteral("source_id")).toString();
            const QString sourcePath = canonicalOrAbsolutePath(QDir(docsRoot).filePath(sourceId));
            const SourceLocation location {true, sourceId, source.value(QStringLiteral("fingerprint")).toString(), 1, sourcePath};
            const BuildInfo build = buildForSource(builds, location);
            if (build.valid) {
                for (const SourceSnapshot &snapshot : build.sourceSnapshots) {
                    if (snapshot.markerEligible && markerEligibleSourcePath(docsRoot, snapshot.sourceId, snapshot.sourcePath))
                        paths.insert(canonicalOrAbsolutePath(snapshot.sourcePath));
                }
            } else if (markerEligibleSourcePath(docsRoot, sourceId, sourcePath)) {
                paths.insert(sourcePath);
            }
        }
    }
    return paths;
}

// Read standalone Markdown metadata conservatively; literal fenced examples are
// never annotation records. Unknown syntax remains protected by raw-ID checks.
QHash<QString, QPair<int, QString>> markdownCommentMirrors(const QString &text)
{
    QHash<QString, QPair<int, QString>> result;
    QSet<QString> duplicates;
    QChar fence;
    int fenceLength = 0;
    const auto lines = text.split(QLatin1Char('\n'));
    QStringList visibleLines;
    for (const auto &raw : lines) {
        const auto line = raw.trimmed();
        if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
            int length = 0;
            while (length < line.size() && line.at(length) == line.at(0)) ++length;
            if (fence.isNull()) { fence = line.at(0); fenceLength = length; }
            else if (fence == line.at(0) && length >= fenceLength && line.mid(length).trimmed().isEmpty()) fence = QChar();
            visibleLines.append(QString());
        } else {
            visibleLines.append(fence.isNull() && !raw.contains(QLatin1Char('`')) && !raw.startsWith(QLatin1String("    ")) && !raw.startsWith(QLatin1Char('\t')) ? line : QString());
        }
    }
    const QRegularExpression token(QStringLiteral("<!--\\s*thoughts:v1:(start|end|point)\\s+(comment:[A-Za-z0-9_-]+:[A-Za-z0-9_-]+)\\s*-->"));
    QHash<QString, QStringList> actions;
    for (const auto &line : visibleLines) {
        auto matches = token.globalMatch(line);
        while (matches.hasNext()) {
            const auto match = matches.next();
            actions[match.captured(2)].append(match.captured(1));
        }
    }
    for (int index = 0; index < visibleLines.size(); ++index) {
        const auto line = visibleLines.at(index);
        if (!line.startsWith(QLatin1String("<!-- thoughts:v1:meta ")) || !line.endsWith(QLatin1String("-->"))) continue;
        const auto start = line.indexOf(QLatin1Char('{'));
        const auto end = line.lastIndexOf(QLatin1Char('}'));
        if (start < 0 || end < start) continue;
        const auto metadata = QJsonDocument::fromJson(line.mid(start, end - start + 1).toUtf8()).object();
        const auto key = metadata.value(QStringLiteral("key")).toString();
        const auto parts = key.split(QLatin1Char(':'));
        if (parts.size() != 3 || parts.at(0) != QLatin1String("comment")) continue;
        const auto events = actions.value(key);
        if (events.count(QStringLiteral("point")) != 1 &&
            !(events.count(QStringLiteral("start")) == 1 && events.count(QStringLiteral("end")) == 1)) continue;
        const auto id = parts.at(1);
        if (result.contains(id)) duplicates.insert(id);
        result.insert(id, {index + 1, metadata.value(QStringLiteral("contents")).toString()});
    }
    for (const auto &id : duplicates) result.remove(id);
    return result;
}

bool isMarkdownSourcePath(const QString &path)
{
    return path.endsWith(QLatin1String(".md")) || path.endsWith(QLatin1String(".markdown"));
}

// Lines that markdownCommentMirrors reads; fenced, indented and inline-code lines never carry markers.
QList<bool> markdownVisibleLineMask(const QStringList &lines)
{
    QList<bool> visible;
    QChar fence;
    int fenceLength = 0;
    for (const auto &raw : lines) {
        const auto line = raw.trimmed();
        if (line.startsWith(QLatin1String("```")) || line.startsWith(QLatin1String("~~~"))) {
            int length = 0;
            while (length < line.size() && line.at(length) == line.at(0)) ++length;
            if (fence.isNull()) { fence = line.at(0); fenceLength = length; }
            else if (fence == line.at(0) && length >= fenceLength && line.mid(length).trimmed().isEmpty()) fence = QChar();
            visible.append(false);
        } else {
            visible.append(fence.isNull() && !raw.contains(QLatin1Char('`')) && !raw.startsWith(QLatin1String("    ")) && !raw.startsWith(QLatin1Char('\t')));
        }
    }
    return visible;
}

QString markdownMetaJson(const QJsonObject &metadata)
{
    const auto encode = [](const QJsonValue &value) {
        const QByteArray array = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
        return QString::fromUtf8(array.mid(1, array.size() - 2));
    };
    QStringList keys = metadata.keys();
    keys.removeAll(QStringLiteral("key"));
    keys.removeAll(QStringLiteral("contents"));
    QStringList ordered;
    if (metadata.contains(QStringLiteral("key"))) ordered.append(QStringLiteral("key"));
    if (metadata.contains(QStringLiteral("contents"))) ordered.append(QStringLiteral("contents"));
    ordered.append(keys);
    QStringList parts;
    for (const auto &key : ordered) parts.append(encode(key) + QLatin1Char(':') + encode(metadata.value(key)));
    // An HTML comment ends at the first "-->"; JSON may escape '>' instead.
    QString json = QLatin1Char('{') + parts.join(QLatin1Char(',')) + QLatin1Char('}');
    return json.replace(QLatin1String("-->"), QLatin1String("--\\u003e"));
}

QString markdownMetaLine(const QJsonObject &metadata)
{
    return QStringLiteral("<!-- thoughts:v1:meta ") + markdownMetaJson(metadata) + QStringLiteral(" -->");
}

struct MarkdownCommentMarker {
    int metaIndex = -1;
    QString key;
    QJsonObject metadata;
};

// The unique live metadata line of one comment, as markdownCommentMirrors accepts it.
std::optional<MarkdownCommentMarker> markdownCommentMarker(const QStringList &lines, const QString &anchorId)
{
    const auto mirrors = markdownCommentMirrors(lines.join(QLatin1Char('\n')));
    const auto found = mirrors.constFind(anchorId);
    if (found == mirrors.cend()) return std::nullopt;
    const int index = found.value().first - 1;
    if (index < 0 || index >= lines.size()) return std::nullopt;
    const QString line = lines.at(index).trimmed();
    const auto start = line.indexOf(QLatin1Char('{'));
    const auto end = line.lastIndexOf(QLatin1Char('}'));
    if (start < 0 || end < start) return std::nullopt;
    MarkdownCommentMarker marker;
    marker.metaIndex = index;
    marker.metadata = QJsonDocument::fromJson(line.mid(start, end - start + 1).toUtf8()).object();
    marker.key = marker.metadata.value(QStringLiteral("key")).toString();
    if (marker.key.section(QLatin1Char(':'), 1, 1) != anchorId) return std::nullopt;
    return marker;
}

bool removeMarkdownAnchor(const QString &path, const QString &anchorId, QString *error)
{
    QString text;
    bool trailingNewline = false;
    if (!readSourceText(path, &text, &trailingNewline, error)) return false;
    QStringList lines = text.split(QLatin1Char('\n'));
    const auto marker = markdownCommentMarker(lines, anchorId);
    if (!marker) {
        if (error) *error = QStringLiteral("anchor marker was not uniquely removable");
        return false;
    }
    const QRegularExpression token(QStringLiteral("<!--\\s*thoughts:v1:(?:start|end|point)\\s+%1\\s*-->").arg(QRegularExpression::escape(marker->key)));
    const auto visible = markdownVisibleLineMask(lines);
    QList<int> removedLines{marker->metaIndex};
    for (int index = 0; index < lines.size(); ++index) {
        if (index == marker->metaIndex || !visible.value(index) || !token.match(lines.at(index)).hasMatch()) continue;
        QString stripped = lines.at(index);
        stripped.remove(token);
        if (stripped.trimmed().isEmpty()) removedLines.append(index);
        else lines[index] = stripped;
    }
    std::sort(removedLines.begin(), removedLines.end(), std::greater<int>());
    for (const int index : std::as_const(removedLines)) lines.removeAt(index);
    QString updated = lines.join(QLatin1Char('\n'));
    return writeSourceText(path, updated, trailingNewline, error);
}

bool rewriteMarkdownAnchorMirror(const QString &path, const QString &anchorId, const QString &contents, bool *changed, QString *error)
{
    *changed = false;
    QString text;
    bool trailingNewline = false;
    if (!readSourceText(path, &text, &trailingNewline, error)) return false;
    QStringList lines = text.split(QLatin1Char('\n'));
    const auto marker = markdownCommentMarker(lines, anchorId);
    if (!marker) {
        if (error) *error = QStringLiteral("anchor marker line is unavailable");
        return false;
    }
    if (marker->metadata.value(QStringLiteral("contents")).toString() == contents) return true;
    auto metadata = marker->metadata;
    metadata.insert(QStringLiteral("contents"), contents);
    const QString original = lines.at(marker->metaIndex);
    const QString indentation = original.left(original.indexOf(QLatin1String("<!--")));
    lines[marker->metaIndex] = indentation + markdownMetaLine(metadata);
    if (!writeSourceText(path, lines.join(QLatin1Char('\n')), trailingNewline, error)) return false;
    *changed = true;
    return true;
}

DocsAnchorOccurrences anchorOccurrences(const QSet<QString> &sourcePaths, bool *allFilesRead = nullptr)
{
    static const QRegularExpression expression(QStringLiteral("\\\\(?:CommentHighlight|CommentStrikethrough|CommentAnchor|ReviewAnchor)\\s*\\{([^{}]+)\\}"));
    DocsAnchorOccurrences occurrences;
    bool complete = true;
    for (const QString &path : sourcePaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            complete = false;
            continue;
        }
        const QString raw = QString::fromUtf8(file.readAll());
        if (isMarkdownSourcePath(path)) {
            const auto markers = markdownCommentMirrors(raw);
            for (auto it = markers.begin(); it != markers.end(); ++it)
                occurrences[it.key()].append({QFileInfo(path).absoluteFilePath(), it.value().first});
            continue;
        }
        const QString visible = visibleCommentSource(raw);
        auto iterator = expression.globalMatch(visible);
        while (iterator.hasNext()) {
            const auto match = iterator.next();
            const int lineNumber = visible.left(match.capturedStart()).count(QLatin1Char('\n')) + 1;
            occurrences[match.captured(1).trimmed()].append({QFileInfo(path).absoluteFilePath(), lineNumber});
        }
    }
    if (allFilesRead) {
        *allFilesRead = complete;
    }
    return occurrences;
}

void refreshAnchorOccurrencesForSource(DocsAnchorOccurrences *occurrences, const QString &sourcePath)
{
    const QString canonicalPath = canonicalOrAbsolutePath(sourcePath);
    bool sourceRead = false;
    const DocsAnchorOccurrences refreshed = anchorOccurrences({canonicalPath}, &sourceRead);
    if (!sourceRead) {
        return;
    }

    for (auto iterator = occurrences->begin(); iterator != occurrences->end();) {
        QList<DocsAnchorOccurrence> &known = iterator.value();
        known.erase(std::remove_if(known.begin(), known.end(), [&canonicalPath](const DocsAnchorOccurrence &occurrence) { return canonicalOrAbsolutePath(occurrence.path) == canonicalPath; }), known.end());
        if (known.isEmpty()) {
            iterator = occurrences->erase(iterator);
        } else {
            ++iterator;
        }
    }
    for (auto iterator = refreshed.constBegin(); iterator != refreshed.constEnd(); ++iterator) {
        (*occurrences)[iterator.key()].append(iterator.value());
    }
}

QString relativeToDocs(const QString &path, const QString &docsRoot)
{
    const QString absolutePath = canonicalOrAbsolutePath(path);
    const QString relative = QDir(canonicalOrAbsolutePath(docsRoot)).relativeFilePath(absolutePath);
    return relative.startsWith(QLatin1String("../")) ? absolutePath : relative;
}

bool sourceAnchorWasRemoved(const QString &docsRoot, DocsAnchorIndex *anchorIndex, const QJsonObject &comment)
{
    if (comment.value(QStringLiteral("anchor_status")).toString() != QLatin1String("anchored")) {
        return false;
    }
    const QString anchorId = comment.value(QStringLiteral("anchor_id")).toString().trimmed();
    const QString storedTexFile = comment.value(QStringLiteral("tex_file")).toString().trimmed();
    if (anchorId.isEmpty() || storedTexFile.isEmpty() || !anchorIndex->occurrences.value(anchorId).isEmpty()) {
        return false;
    }

    const QFileInfo storedInfo(storedTexFile);
    const QString sourcePath = canonicalOrAbsolutePath(storedInfo.isAbsolute() ? storedInfo.absoluteFilePath() : QDir(docsRoot).filePath(storedTexFile));
    const QString sourceId = relativeToDocs(sourcePath, docsRoot);
    if (!QFileInfo(sourcePath).isFile() || !markerEligibleSourcePath(docsRoot, sourceId, sourcePath)) {
        return false;
    }

    // A marker can legitimately be moved to another managed source file.
    // Complete the reusable index only on the first exceptional miss. The
    // traversal prunes build/archive trees and is shared by the whole batch.
    if (!anchorIndex->complete) {
        bool allFilesRead = false;
        DocsAnchorOccurrences completeOccurrences = anchorOccurrences(managedSourceFiles(docsRoot), &allFilesRead);
        if (!allFilesRead) {
            return false;
        }
        anchorIndex->occurrences = std::move(completeOccurrences);
        anchorIndex->complete = true;
    }
    return anchorIndex->occurrences.value(anchorId).isEmpty();
}

// One spelling for characters that TeX source and PDF text write differently: TeX prints
// `` '' ` ' -- --- as “ ” ‘ ’ – —, the PDF text may carry ligatures (ﬁ), a Unicode minus or
// the modifier apostrophe ʼ. A selected word "models’" then matches the source's models'.
QString foldedTypography(QString text)
{
    text.replace(QLatin1String("``"), QLatin1String("\""));
    text.replace(QLatin1String("''"), QLatin1String("\""));
    text.replace(QLatin1String("---"), QLatin1String("-"));
    text.replace(QLatin1String("--"), QLatin1String("-"));
    for (QChar &c : text) {
        switch (c.unicode()) {
        case 0x0060: case 0x02BB: case 0x02BC: case 0x02BD: case 0x2018: case 0x2019: case 0x201A: case 0x201B: case 0x2032:
            c = QLatin1Char('\'');
            break;
        case 0x201C: case 0x201D: case 0x201E: case 0x201F: case 0x2033:
            c = QLatin1Char('"');
            break;
        case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212:
            c = QLatin1Char('-');
            break;
        default:
            break;
        }
    }
    return text.normalized(QString::NormalizationForm_KC);
}

QString normalizedSourceLine(QString line)
{
    line = line.section(QLatin1Char('%'), 0, 0);
    if (line.trimmed().startsWith(QLatin1String("\\CommentAnchor"))
        || line.trimmed().startsWith(QLatin1String("\\ReviewAnchor"))) {
        return {};
    }
    line = foldedTypography(line);
    static const QRegularExpression commands(QStringLiteral("\\\\[A-Za-z@]+\\*?"));
    static const QRegularExpression markup(QStringLiteral("[{}\\[\\]~\\\\]"));
    static const QRegularExpression whitespace(QStringLiteral("\\s+"));
    line.replace(commands, QStringLiteral(" "));
    line.replace(markup, QStringLiteral(" "));
    return line.simplified().toLower();
}

int remapSnapshotLine(const QStringList &snapshotLines, const QStringList &currentLines, int lineNumber)
{
    if (lineNumber < 1 || lineNumber > snapshotLines.size()) {
        return 0;
    }
    const QString target = normalizedSourceLine(snapshotLines.at(lineNumber - 1));
    if (target.isEmpty()) {
        return 0;
    }
    if (lineNumber <= currentLines.size() && normalizedSourceLine(currentLines.at(lineNumber - 1)) == target) {
        return lineNumber;
    }
    int match = 0;
    int count = 0;
    for (int index = 0; index < currentLines.size(); ++index) {
        if (normalizedSourceLine(currentLines.at(index)) == target) {
            match = index + 1;
            ++count;
        }
    }
    return count == 1 ? match : 0;
}

QStringList sourceResolutionTokens(const QString &text)
{
    QString normalized = normalizedSourceLine(text);
    static const QRegularExpression generatedLabel(QStringLiteral("^(?:figure|table|listing|algorithm)\\s+[^:]{1,24}:\\s*"));
    normalized.remove(generatedLabel);

    static const QRegularExpression tokenExpression(QStringLiteral("[\\p{L}\\p{N}]+"), QRegularExpression::UseUnicodePropertiesOption);
    QStringList tokens;
    QRegularExpressionMatchIterator iterator = tokenExpression.globalMatch(normalized);
    while (iterator.hasNext() && tokens.size() < 64) {
        tokens.append(iterator.next().captured());
    }
    return tokens;
}

int orderedTokenOverlap(const QStringList &left, const QStringList &right)
{
    std::vector<int> previous(static_cast<size_t>(right.size() + 1));
    std::vector<int> current(static_cast<size_t>(right.size() + 1));
    for (int leftIndex = 0; leftIndex < left.size(); ++leftIndex) {
        std::fill(current.begin(), current.end(), 0);
        for (int rightIndex = 0; rightIndex < right.size(); ++rightIndex) {
            current[static_cast<size_t>(rightIndex + 1)] = left.at(leftIndex) == right.at(rightIndex)
                ? previous[static_cast<size_t>(rightIndex)] + 1
                : std::max(previous[static_cast<size_t>(rightIndex + 1)], current[static_cast<size_t>(rightIndex)]);
        }
        previous.swap(current);
    }
    return previous.back();
}

double selectedTextLineScore(const QStringList &selectedTokens, const QString &sourceLine, int *matchedTokens = nullptr)
{
    const QStringList sourceTokens = sourceResolutionTokens(sourceLine);
    const int overlap = orderedTokenOverlap(selectedTokens, sourceTokens);
    if (matchedTokens) {
        *matchedTokens = overlap;
    }
    return selectedTokens.isEmpty() || sourceTokens.isEmpty() ? 0.0 : (2.0 * overlap) / (selectedTokens.size() + sourceTokens.size());
}

int selectedTextLineNear(const QStringList &lines, int preferredLine, const QString &selectedText, int radius)
{
    const QStringList selectedTokens = sourceResolutionTokens(selectedText);
    if (selectedTokens.size() < 4 || preferredLine < 1) {
        return 0;
    }

    struct Candidate {
        int line = 0;
        int distance = 0;
        int matchedTokens = 0;
        double score = 0.0;
    };
    QList<Candidate> candidates;
    const int firstIndex = std::max(0, preferredLine - 1 - radius);
    const int lastIndex = std::min(static_cast<int>(lines.size()) - 1, preferredLine - 1 + radius);
    for (int index = firstIndex; index <= lastIndex; ++index) {
        int matchedTokens = 0;
        const double score = selectedTextLineScore(selectedTokens, lines.at(index), &matchedTokens);
        if (matchedTokens >= 4 && score >= 0.72) {
            candidates.append({index + 1, std::abs(index + 1 - preferredLine), matchedTokens, score});
        }
    }
    if (candidates.isEmpty()) {
        return 0;
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
        if (left.score != right.score) {
            return left.score > right.score;
        }
        if (left.matchedTokens != right.matchedTokens) {
            return left.matchedTokens > right.matchedTokens;
        }
        return left.distance < right.distance;
    });
    if (candidates.size() > 1 && candidates.at(1).score >= candidates.constFirst().score - 0.06) {
        return 0;
    }
    return candidates.constFirst().line;
}

int uniqueSelectedTextLine(const QStringList &lines, const QString &selectedText)
{
    const QString normalizedSelection = normalizedSourceLine(selectedText);
    if (normalizedSelection.isEmpty() || sourceResolutionTokens(selectedText).size() < 4) {
        return 0;
    }

    int match = 0;
    for (int index = 0; index < lines.size(); ++index) {
        if (!normalizedSourceLine(lines.at(index)).contains(normalizedSelection)) {
            continue;
        }
        if (match > 0) {
            return 0;
        }
        match = index + 1;
    }
    return match;
}

int remapSnapshotLineFromSelectedText(const QStringList &snapshotLines,
                                      const QStringList &currentLines,
                                      int synctexLine,
                                      const QString &selectedText)
{
    int snapshotSelectionLine = selectedTextLineNear(snapshotLines, synctexLine, selectedText, 16);
    if (snapshotSelectionLine < 1) {
        snapshotSelectionLine = uniqueSelectedTextLine(snapshotLines, selectedText);
    }
    if (snapshotSelectionLine < 1) {
        return 0;
    }
    const int direct = remapSnapshotLine(snapshotLines, currentLines, snapshotSelectionLine);
    if (direct > 0) {
        return direct;
    }

    const QStringList selectedTokens = sourceResolutionTokens(selectedText);
    QSet<int> projectedLines;
    for (int distance = 1; distance <= 8; ++distance) {
        for (const int direction : {-1, 1}) {
            const int neighborLine = snapshotSelectionLine + direction * distance;
            const int mappedNeighbor = remapSnapshotLine(snapshotLines, currentLines, neighborLine);
            if (mappedNeighbor < 1) {
                continue;
            }
            const int projectedLine = mappedNeighbor + snapshotSelectionLine - neighborLine;
            if (projectedLine < 1 || projectedLine > currentLines.size()) {
                continue;
            }
            int matchedTokens = 0;
            const double score = selectedTextLineScore(selectedTokens, currentLines.at(projectedLine - 1), &matchedTokens);
            if (matchedTokens >= 4 && score >= 0.72) {
                projectedLines.insert(projectedLine);
            }
        }
    }
    if (projectedLines.size() == 1) {
        return *projectedLines.constBegin();
    }
    if (!projectedLines.isEmpty()) {
        return 0;
    }
    const int nearbyCurrentLine = selectedTextLineNear(currentLines, synctexLine, selectedText, 24);
    return nearbyCurrentLine > 0 ? nearbyCurrentLine : uniqueSelectedTextLine(currentLines, selectedText);
}

int remapSourceNavigationLine(const QStringList &snapshotLines, const QStringList &currentLines, int oldLine, const QString &selectedText)
{
    const QStringList visibleSnapshot = sourceNavigationLines(snapshotLines);
    const QStringList visibleCurrent = snapshotLines == currentLines ? visibleSnapshot : sourceNavigationLines(currentLines);
    if (visibleSnapshot.isEmpty() || visibleCurrent.isEmpty()) {
        return 0;
    }
    const int direct = remapSnapshotLine(visibleSnapshot, visibleCurrent, oldLine);
    return direct > 0 || selectedText.trimmed().isEmpty()
        ? direct
        : remapSnapshotLineFromSelectedText(visibleSnapshot, visibleCurrent, oldLine, selectedText);
}

// @p snapshotOldLine: the snapshot's line found for the selection, also when it cannot be mapped.
int remapAnchorPassage(const QStringList &snapshotLines, const QStringList &currentLines, int mappedLine, const QString &selection,
                       int *snapshotOldLine = nullptr)
{
    const QStringList before = sourceNavigationLines(snapshotLines);
    const QStringList after = sourceNavigationLines(currentLines);
    int oldLine = mappedLine;
    const QString selected = normalizedSourceLine(selection);
    if (!selected.isEmpty()) {
        int bestDistance = 9, matches = 0;
        for (int i = std::max(0, mappedLine - 9); i < std::min(int(before.size()), mappedLine + 8); ++i) {
            if (!normalizedSourceLine(before.at(i)).contains(selected)) continue;
            const int distance = qAbs(i + 1 - mappedLine);
            if (distance < bestDistance) { oldLine = i + 1; bestDistance = distance; matches = 1; }
            else if (distance == bestDistance) ++matches;
        }
        if (matches != 1) return 0;
    }
    if (oldLine < 1 || oldLine > before.size()) return 0;
    if (snapshotOldLine) *snapshotOldLine = oldLine;
    if (before == after) return oldLine;
    const QString target = normalizedSourceLine(before.at(oldLine - 1));
    if (target.isEmpty()) return 0;
    QList<int> candidates;
    int originalOccurrences = 0;
    for (const QString &line : before) if (normalizedSourceLine(line) == target) ++originalOccurrences;
    for (int i = 0; i < after.size(); ++i) if (normalizedSourceLine(after.at(i)) == target) candidates.append(i + 1);
    if (candidates.size() == 1 && originalOccurrences == 1) return candidates.constFirst();
    int best = 0, bestScore = 0, ties = 0;
    for (const int candidate : candidates) {
        int score = 0;
        for (int offset : {-2, -1, 1, 2}) {
            const int a = oldLine - 1 + offset, b = candidate - 1 + offset;
            if (a < 0 || a >= before.size() || b < 0 || b >= after.size()) continue;
            const QString context = normalizedSourceLine(before.at(a));
            if (!context.isEmpty() && context == normalizedSourceLine(after.at(b))) ++score;
        }
        if (score > bestScore) { best = candidate; bestScore = score; ties = 1; }
        else if (score == bestScore) ++ties;
    }
    return bestScore >= 2 && ties == 1 ? best : 0;
}

QString texLineWithoutComment(const QString &line);

struct DocumentTitleSpan {
    int startLine = 0;
    int endLine = 0;
};

bool skipDelimitedArgument(const QString &text, int *position, QChar open, QChar close)
{
    if (!position || *position >= text.size() || text.at(*position) != open) {
        return false;
    }
    int depth = 1;
    int braceDepth = 0;
    ++(*position);
    while (*position < text.size()) {
        const QChar character = text.at(*position);
        if (character == QLatin1Char('\\')) {
            *position += *position + 1 < text.size() ? 2 : 1;
            continue;
        }
        if (open == QLatin1Char('[') && character == QLatin1Char('{')) {
            ++braceDepth;
        } else if (open == QLatin1Char('[') && character == QLatin1Char('}') && braceDepth > 0) {
            --braceDepth;
        } else if (braceDepth == 0 && character == open) {
            ++depth;
        } else if (braceDepth == 0 && character == close) {
            --depth;
            ++(*position);
            if (depth == 0) {
                return true;
            }
            continue;
        }
        ++(*position);
    }
    return false;
}

std::optional<DocumentTitleSpan> firstDocumentTitleSpan(const QStringList &lines)
{
    static const QRegularExpression beginDocument(QStringLiteral("\\\\begin\\s*\\{document\\}"));
    static const QString command = QStringLiteral("\\docTitle");

    for (int index = 0; index < lines.size(); ++index) {
        const QString uncommented = texLineWithoutComment(lines.at(index));
        int position = 0;
        const QRegularExpressionMatch beginMatch = beginDocument.match(uncommented);
        if (beginMatch.hasMatch()) {
            position = beginMatch.capturedEnd();
        }
        while (position < uncommented.size() && uncommented.at(position).isSpace()) {
            ++position;
        }
        if (!uncommented.mid(position).startsWith(command)) {
            continue;
        }
        const int commandEnd = position + command.size();
        if (commandEnd < uncommented.size() && (uncommented.at(commandEnd).isLetterOrNumber() || uncommented.at(commandEnd) == QLatin1Char('@'))) {
            continue;
        }

        QString titleText = uncommented.mid(position);
        for (int continuation = index + 1; continuation < lines.size(); ++continuation) {
            titleText += QLatin1Char('\n');
            titleText += texLineWithoutComment(lines.at(continuation));
        }

        int cursor = command.size();
        while (cursor < titleText.size() && titleText.at(cursor).isSpace()) {
            ++cursor;
        }
        if (cursor < titleText.size() && titleText.at(cursor) == QLatin1Char('[')) {
            if (!skipDelimitedArgument(titleText, &cursor, QLatin1Char('['), QLatin1Char(']'))) {
                return std::nullopt;
            }
            while (cursor < titleText.size() && titleText.at(cursor).isSpace()) {
                ++cursor;
            }
        }
        if (!skipDelimitedArgument(titleText, &cursor, QLatin1Char('{'), QLatin1Char('}'))) {
            return std::nullopt;
        }
        return DocumentTitleSpan {
            .startLine = index + 1,
            .endLine = index + 1 + static_cast<int>(titleText.left(cursor).count(QLatin1Char('\n'))),
        };
    }
    return std::nullopt;
}

QString environmentAtLine(const QStringList &lines, int lineNumber)
{
    static const QRegularExpression begin(QStringLiteral("\\\\begin\\{([^{}]+)\\}"));
    static const QRegularExpression end(QStringLiteral("\\\\end\\{([^{}]+)\\}"));
    QStringList stack;
    for (int index = 0; index < std::min(lineNumber - 1, static_cast<int>(lines.size())); ++index) {
        const QString line = lines.at(index).section(QLatin1Char('%'), 0, 0);
        auto beginIterator = begin.globalMatch(line);
        while (beginIterator.hasNext()) {
            stack.append(beginIterator.next().captured(1));
        }
        auto endIterator = end.globalMatch(line);
        while (endIterator.hasNext()) {
            const QString name = endIterator.next().captured(1);
            const int found = stack.lastIndexOf(name);
            if (found >= 0) {
                while (stack.size() > found) {
                    stack.removeLast();
                }
            }
        }
    }
    return stack.isEmpty() ? QString() : stack.constLast();
}

bool safeInsertionLine(const QStringList &lines, int lineNumber)
{
    if (lineNumber < 1 || lineNumber > lines.size()) {
        return false;
    }
    const QString stripped = lines.at(lineNumber - 1).trimmed();
    // A reverse SyncTeX lookup for otherwise empty trailing page space often
    // lands on the document boundary. The marker belongs immediately before
    // that boundary, which is exactly where insertOrMoveAnchor inserts it.
    if (stripped.section(QLatin1Char('%'), 0, 0).trimmed() == QLatin1String("\\end{document}")) {
        return true;
    }
    if (stripped.isEmpty() || stripped.startsWith(QLatin1Char('%')) || QStringLiteral("\\{}[]&").contains(stripped.front())) {
        return false;
    }
    for (const QString &prefix : UnsafeLinePrefixes) {
        if (stripped.startsWith(prefix)) {
            return false;
        }
    }
    return !UnsafeEnvironments.contains(environmentAtLine(lines, lineNumber));
}

int anchorInsertionLine(const QStringList &lines, int targetLine, bool allowCommandTarget = false)
{
    if (targetLine < 1 || targetLine > lines.size()) {
        return 0;
    }
    const std::optional<DocumentTitleSpan> title = firstDocumentTitleSpan(lines);
    if (title && targetLine <= title->endLine) {
        return title->endLine + 1;
    }
    return allowCommandTarget || safeInsertionLine(lines, targetLine) ? targetLine : 0;
}

QString texLineWithoutComment(const QString &line)
{
    for (int index = 0; index < line.size(); ++index) {
        if (line.at(index) != QLatin1Char('%')) {
            continue;
        }
        int precedingBackslashes = 0;
        for (int candidate = index - 1; candidate >= 0 && line.at(candidate) == QLatin1Char('\\'); --candidate) {
            ++precedingBackslashes;
        }
        if (precedingBackslashes % 2 == 0) {
            return line.left(index);
        }
    }
    return line;
}

bool hasGeneratedPathComponent(const QString &path)
{
    const QString normalized = QDir::fromNativeSeparators(path);
    for (const QString &part : normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (part.compare(QLatin1String("generated"), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }
    return false;
}

bool filesHaveEqualContents(const QString &leftPath, const QString &rightPath)
{
    QFile left(leftPath);
    QFile right(rightPath);
    if (!left.open(QIODevice::ReadOnly) || !right.open(QIODevice::ReadOnly) || left.size() != right.size()) {
        return false;
    }
    while (!left.atEnd()) {
        if (left.read(64 * 1024) != right.read(64 * 1024)) {
            return false;
        }
    }
    return right.atEnd();
}

bool previousGeneratedSnapshotMatches(const QString &docsRoot, const QString &previousBuildId, const QString &storedTexFile, const SourceSnapshot &currentSnapshot)
{
    static const QRegularExpression safeBuildId(QStringLiteral("^[A-Fa-f0-9]{64}$"));
    if (!safeBuildId.match(previousBuildId).hasMatch()) {
        return false;
    }

    const QString buildDirectory = canonicalOrAbsolutePath(QDir(docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds/") + previousBuildId));
    const QString buildsRoot = canonicalOrAbsolutePath(QDir(docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds")));
    const QString relativeBuild = QDir(buildsRoot).relativeFilePath(buildDirectory);
    if (relativeBuild == QLatin1String("..") || relativeBuild.startsWith(QLatin1String("../"))) {
        return false;
    }

    QJsonObject mapping;
    QString ignored;
    if (!readJsonObject(QDir(buildDirectory).filePath(QStringLiteral("mapping.json")), &mapping, &ignored)
        || mapping.value(QStringLiteral("build_id")).toString() != previousBuildId) {
        return false;
    }

    const QFileInfo storedInfo(storedTexFile);
    const QString storedSourceId =
        storedInfo.isAbsolute() ? relativeToDocs(canonicalOrAbsolutePath(storedInfo.absoluteFilePath()), docsRoot) : storedTexFile;
    for (const QJsonValue &value : mapping.value(QStringLiteral("source_snapshots")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("source_id")).toString() != storedSourceId) {
            continue;
        }
        const QString previousSnapshot = canonicalOrAbsolutePath(QDir(buildDirectory).filePath(entry.value(QStringLiteral("snapshot")).toString()));
        const QString relativeSnapshot = QDir(buildDirectory).relativeFilePath(previousSnapshot);
        if (relativeSnapshot == QLatin1String("..") || relativeSnapshot.startsWith(QLatin1String("../"))) {
            return false;
        }
        return filesHaveEqualContents(previousSnapshot, currentSnapshot.snapshotPath);
    }
    return false;
}

QString resolveMappedTexInclude(const QString &argument, const QString &parentPath, const QString &projectRoot, const QString &docsRoot, const QSet<QString> &mappedPaths)
{
    const QString trimmed = argument.trimmed();
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char('\\')) || trimmed.contains(QLatin1Char('#')) || trimmed.contains(QLatin1Char('{')) || trimmed.contains(QLatin1Char('}'))) {
        return {};
    }

    QStringList names {trimmed};
    if (QFileInfo(trimmed).suffix().isEmpty()) {
        names.append(trimmed + QStringLiteral(".tex"));
    }

    QStringList roots;
    if (QFileInfo(trimmed).isAbsolute()) {
        roots.append(QString());
    } else {
        roots.append(QFileInfo(parentPath).absolutePath());
        if (!projectRoot.isEmpty()) {
            roots.append(projectRoot);
        }
        roots.append(docsRoot);
    }
    roots.removeDuplicates();

    for (const QString &root : std::as_const(roots)) {
        for (const QString &name : std::as_const(names)) {
            const QString candidate = root.isEmpty() ? name : QDir(root).filePath(name);
            const QString normalized = canonicalOrAbsolutePath(QDir::cleanPath(candidate));
            if (mappedPaths.contains(normalized)) {
                return normalized;
            }
        }
    }
    return {};
}

QHash<QString, QList<TexIncludeSite>> texIncludeSites(const QString &docsRoot, const BuildInfo &build)
{
    QHash<QString, QList<TexIncludeSite>> incoming;
    QSet<QString> mappedPaths;
    for (const SourceSnapshot &snapshot : build.sourceSnapshots) {
        mappedPaths.insert(canonicalOrAbsolutePath(snapshot.sourcePath));
    }

    static const QRegularExpression includeCommand(
        QStringLiteral("\\\\(?:input|include)(?![A-Za-z@])\\s*(?:\\{([^{}\\r\\n]+)\\}|([^\\s%{}]+))|\\\\InputIfFileExists(?![A-Za-z@])\\s*\\{([^{}\\r\\n]+)\\}"));

    for (const SourceSnapshot &snapshot : build.sourceSnapshots) {
        QStringList lines;
        QString ignored;
        if (!readTextLines(snapshot.sourcePath, &lines, &ignored)) {
            continue;
        }
        QStringList uncommented;
        uncommented.reserve(lines.size());
        for (const QString &line : std::as_const(lines)) {
            uncommented.append(texLineWithoutComment(line));
        }
        const QString text = uncommented.join(QLatin1Char('\n'));
        const QString parentPath = canonicalOrAbsolutePath(snapshot.sourcePath);
        const QString projectRoot = managedProjectRoot(docsRoot, snapshot.sourceId);

        auto matches = includeCommand.globalMatch(text);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            QString argument;
            for (int capture = 1; capture <= 3 && argument.isEmpty(); ++capture) {
                argument = match.captured(capture);
            }
            const QString childPath = resolveMappedTexInclude(argument, parentPath, projectRoot, docsRoot, mappedPaths);
            if (childPath.isEmpty()) {
                continue;
            }

            const int lineStart = text.lastIndexOf(QLatin1Char('\n'), match.capturedStart()) + 1;
            const int line = text.left(match.capturedStart()).count(QLatin1Char('\n')) + 1;
            const bool startsLine = text.mid(lineStart, match.capturedStart() - lineStart).trimmed().isEmpty();
            const bool safe = startsLine && !UnsafeEnvironments.contains(environmentAtLine(lines, line));
            incoming[childPath].append({
                snapshot.sourceId,
                parentPath,
                childPath,
                line,
                safe,
            });
        }
    }
    return incoming;
}

GeneratedParentResolution generatedParentResolution(const QString &docsRoot, const BuildInfo &build, const SourceSnapshot &generatedSource)
{
    const QString generatedPath = canonicalOrAbsolutePath(generatedSource.sourcePath);
    const QHash<QString, QList<TexIncludeSite>> incoming = texIncludeSites(docsRoot, build);
    QHash<QString, const SourceSnapshot *> snapshotsByPath;
    for (const SourceSnapshot &snapshot : build.sourceSnapshots) {
        snapshotsByPath.insert(canonicalOrAbsolutePath(snapshot.sourcePath), &snapshot);
    }

    QList<QString> pending {generatedPath};
    QSet<QString> expanded;
    QHash<QString, TexIncludeSite> eligibleSites;
    while (!pending.isEmpty()) {
        const QString childPath = pending.takeLast();
        if (expanded.contains(childPath)) {
            continue;
        }
        expanded.insert(childPath);
        for (const TexIncludeSite &site : incoming.value(childPath)) {
            const auto parent = snapshotsByPath.constFind(site.parentPath);
            if (parent == snapshotsByPath.cend()) {
                continue;
            }
            if ((*parent)->markerEligible && markerEligibleSourcePath(docsRoot, (*parent)->sourceId, (*parent)->sourcePath)) {
                eligibleSites.insert(site.parentPath + QChar(0x1f) + QString::number(site.line), site);
            } else {
                pending.append(site.parentPath);
            }
        }
    }

    if (eligibleSites.isEmpty()) {
        return {.reason = QStringLiteral("generated_parent_missing")};
    }
    if (eligibleSites.size() != 1) {
        return {.reason = QStringLiteral("generated_parent_ambiguous")};
    }
    const TexIncludeSite site = eligibleSites.constBegin().value();
    if (!site.safe) {
        return {.reason = QStringLiteral("generated_parent_unsafe")};
    }
    return {
        true,
        {},
        site.parentSourceId,
        site.parentPath,
        site.line,
    };
}

QString generatedSourceDocument(const QString &docsRoot, const BuildInfo &build, const QString &generatedPath)
{
    const auto incoming = texIncludeSites(docsRoot, build);
    QList<QString> pending {canonicalOrAbsolutePath(generatedPath)};
    QSet<QString> visited;
    QSet<QString> documents;
    while (!pending.isEmpty()) {
        const QString child = pending.takeLast();
        if (visited.contains(child)) {
            continue;
        }
        visited.insert(child);
        for (const TexIncludeSite &site : incoming.value(child)) {
            if (markerEligibleSourcePath(docsRoot, site.parentSourceId, site.parentPath)) {
                const QFileInfo file(site.parentPath);
                if (file.isFile() && file.isReadable()) {
                    documents.insert(file.canonicalFilePath());
                }
            } else if (hasGeneratedPathComponent(site.parentPath)) {
                pending.append(site.parentPath);
            }
        }
    }
    // Repeated includes in one document are safe for file-only navigation.
    // Distinct parent documents remain ambiguous; no insertion point is inferred.
    return documents.size() == 1 ? *documents.constBegin() : QString();
}

QString sourceContext(const QStringList &lines, int lineNumber)
{
    QStringList selected;
    for (int candidate = std::max(0, lineNumber - 2); candidate < std::min(static_cast<int>(lines.size()), lineNumber + 1); ++candidate) {
        const QString normalized = normalizedSourceLine(lines.at(candidate));
        if (!normalized.isEmpty()) {
            selected.append(normalized);
        }
    }
    return selected.join(QLatin1Char(' '));
}

QList<SynctexRecord> parseSynctexRecords(const QString &output)
{
    QList<SynctexRecord> records;
    std::optional<SynctexRecord> current;
    for (const QString &rawLine : output.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        if (line == QLatin1String("SyncTeX result begin")) {
            current = SynctexRecord();
            continue;
        }
        if (line == QLatin1String("SyncTeX result end")) {
            if (current && !current->fields.isEmpty()) {
                records.append(*current);
            }
            current.reset();
            continue;
        }
        if (current && line.contains(QLatin1Char(':'))) {
            current->fields.insert(line.section(QLatin1Char(':'), 0, 0), line.section(QLatin1Char(':'), 1));
        }
    }
    return records;
}

QString normalizedCandidatePath(const QString &rawPath, const QString &docsRoot, const BuildInfo &build)
{
    QString value = rawPath.trimmed();
    if (value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) {
        value = value.mid(1, value.size() - 2);
    }
    if (value.isEmpty()) {
        return {};
    }
    const QFileInfo direct(value);
    const QStringList candidates = direct.isAbsolute() ? QStringList {direct.absoluteFilePath()}
                                                       : QStringList {
                                                             QFileInfo(QDir(QFileInfo(build.sourcePath).absolutePath()).filePath(value)).absoluteFilePath(),
                                                             QFileInfo(QDir(docsRoot).filePath(value)).absoluteFilePath(),
                                                         };
    for (const QString &candidate : candidates) {
        const QFileInfo info(candidate);
        if (info.exists()) {
            return info.canonicalFilePath().isEmpty() ? info.absoluteFilePath() : info.canonicalFilePath();
        }
    }
    return QFileInfo(candidates.constFirst()).absoluteFilePath();
}

const SourceSnapshot *snapshotForCandidate(const BuildInfo &build, const QString &candidatePath)
{
    if (candidatePath.isEmpty()) {
        return nullptr;
    }
    const QString normalized = canonicalOrAbsolutePath(candidatePath);
    for (const SourceSnapshot &snapshot : build.sourceSnapshots) {
        if (canonicalOrAbsolutePath(snapshot.sourcePath) == normalized) {
            return &snapshot;
        }
    }
    return nullptr;
}

bool readTextLines(const QString &path, QStringList *lines, QString *error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    *lines = QString::fromUtf8(file.readAll()).split(QLatin1Char('\n'));
    if (!lines->isEmpty() && lines->constLast().isEmpty()) {
        lines->removeLast();
    }
    return true;
}

bool writeTextLines(const QString &path, const QStringList &lines, bool trailingNewline, QString *error)
{
    const QFileInfo original(path);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    QByteArray encoded = lines.join(QLatin1Char('\n')).toUtf8();
    if (trailingNewline) {
        encoded.append('\n');
    }
    if (file.write(encoded) != encoded.size()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    file.setPermissions(original.permissions());
    if (!file.commit()) {
        if (error) {
            *error = file.errorString();
        }
        return false;
    }
    return true;
}

bool readSourceText(const QString &path, QString *text, bool *trailingNewline, QString *error)
{
    QStringList lines;
    if (!readTextLines(path, &lines, error)) {
        return false;
    }
    *trailingNewline = textFileHasTrailingNewline(path);
    *text = lines.join(QLatin1Char('\n'));
    return true;
}

bool writeSourceText(const QString &path, const QString &text, bool trailingNewline, QString *error)
{
    return writeTextLines(path, text.split(QLatin1Char('\n')), trailingNewline, error);
}

bool textFileHasTrailingNewline(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    if (file.size() == 0) {
        return true;
    }
    return file.seek(file.size() - 1) && file.read(1) == QByteArray("\n");
}

struct StandaloneCommentAnchor {
    QString id;
    QString mirror;
    bool hasMirror = false;
};

struct CommentAnchorSpan {
    QString id;
    QString mirror;
    QString body;
    bool hasMirror = false;
    bool hasBody = false;
    int commandStart = -1;
    int commandEnd = -1;
    int mirrorStart = -1;
    int mirrorEnd = -1;
    int line = 0;
};

QString texLineWithoutComment(const QString &line);

bool parseBracedArgument(const QString &line, int *position, QString *value)
{
    if (*position >= line.size() || line.at(*position) != QLatin1Char('{')) {
        return false;
    }
    const int valueStart = ++(*position);
    int depth = 1;
    while (*position < line.size()) {
        const QChar character = line.at(*position);
        if (character == QLatin1Char('\\')) {
            *position += *position + 1 < line.size() ? 2 : 1;
            continue;
        }
        if (character == QLatin1Char('%')) {
            const int newline = line.indexOf(QLatin1Char('\n'), *position);
            *position = newline < 0 ? line.size() : newline + 1;
            continue;
        }
        if (character == QLatin1Char('{')) {
            ++depth;
        } else if (character == QLatin1Char('}')) {
            --depth;
            if (depth == 0) {
                *value = line.mid(valueStart, *position - valueStart);
                ++(*position);
                return true;
            }
        }
        ++(*position);
    }
    return false;
}

QString commentAnchorCommandAt(const QString &text, int position)
{
    for (const QString &command : {QStringLiteral("\\CommentHighlight"), QStringLiteral("\\CommentStrikethrough"), QStringLiteral("\\CommentAnchor"), QStringLiteral("\\ReviewAnchor")}) {
        if (position >= 0 && text.mid(position).startsWith(command)) {
            return command;
        }
    }
    return QString();
}

void skipCommentHighlightSpace(const QString &text, int *position)
{
    while (*position < text.size()) {
        if (text.at(*position).isSpace()) {
            ++(*position);
        } else if (text.at(*position) == QLatin1Char('%')) {
            const int newline = text.indexOf(QLatin1Char('\n'), *position);
            *position = newline < 0 ? text.size() : newline + 1;
        } else {
            break;
        }
    }
}

std::optional<CommentAnchorSpan> commentAnchorSpanAt(const QString &text, int commandStart)
{
    const QString command = commentAnchorCommandAt(text, commandStart);
    if (command.isEmpty()) {
        return std::nullopt;
    }
    const int lineStart = commandStart > 0 ? text.lastIndexOf(QLatin1Char('\n'), commandStart - 1) + 1 : 0;
    const QString prefix = text.mid(lineStart, commandStart - lineStart);
    if (texLineWithoutComment(prefix).size() != prefix.size()) {
        return std::nullopt;
    }

    int position = commandStart + command.size();
    while (position < text.size() && text.at(position).isSpace()) {
        ++position;
    }
    if (command == QLatin1String("\\CommentHighlight") || command == QLatin1String("\\CommentStrikethrough")) {
        skipCommentHighlightSpace(text, &position);
    }
    CommentAnchorSpan span;
    span.commandStart = commandStart;
    span.line = text.left(commandStart).count(QLatin1Char('\n')) + 1;
    if (!parseBracedArgument(text, &position, &span.id) || span.id.trimmed().isEmpty()) {
        return std::nullopt;
    }
    span.id = span.id.trimmed();
    while (position < text.size() && text.at(position).isSpace()) {
        ++position;
    }
    if (command == QLatin1String("\\CommentHighlight") || command == QLatin1String("\\CommentStrikethrough")) {
        skipCommentHighlightSpace(text, &position);
    }
    if (position < text.size() && text.at(position) == QLatin1Char('{')) {
        span.mirrorStart = position + 1;
        if (!parseBracedArgument(text, &position, &span.mirror)) {
            return std::nullopt;
        }
        span.mirrorEnd = position - 1;
        span.hasMirror = true;
        int bodyPosition = position;
        const bool explicitHighlight = (command == QLatin1String("\\CommentHighlight") || command == QLatin1String("\\CommentStrikethrough"));
        if (explicitHighlight) {
            skipCommentHighlightSpace(text, &bodyPosition);
        }
        while (bodyPosition < text.size()
               && (text.at(bodyPosition) == QLatin1Char(' ')
                   || text.at(bodyPosition) == QLatin1Char('\t')
                   || text.at(bodyPosition) == QLatin1Char('\r')
                   || (explicitHighlight && text.at(bodyPosition).isSpace()))) {
            ++bodyPosition;
        }
        if (bodyPosition < text.size() && text.at(bodyPosition) == QLatin1Char('{')) {
            position = bodyPosition;
            if (!parseBracedArgument(text, &position, &span.body)) {
                return std::nullopt;
            }
            span.hasBody = true;
        }
    }
    if ((command == QLatin1String("\\CommentHighlight") || command == QLatin1String("\\CommentStrikethrough")) && (!span.hasMirror || !span.hasBody)) {
        return std::nullopt;
    }
    span.commandEnd = position;
    return span;
}

QList<CommentAnchorSpan> commentAnchorSpans(const QString &text, const QString &anchorId = QString())
{
    QList<CommentAnchorSpan> spans;
    static const QRegularExpression command(QStringLiteral("\\\\(?:CommentHighlight|CommentStrikethrough|CommentAnchor|ReviewAnchor)(?![A-Za-z@])"));
    const QString visible = visibleCommentSource(text);
    auto matches = command.globalMatch(visible);
    while (matches.hasNext()) {
        const int offset = matches.next().capturedStart();
        const std::optional<CommentAnchorSpan> span = commentAnchorSpanAt(text, offset);
        if (span && (anchorId.isEmpty() || span->id == anchorId)) {
            // Scan inside bodies too, because highlighted comments may overlap.
            spans.append(*span);
        }
    }
    return spans;
}

std::optional<StandaloneCommentAnchor> standaloneCommentAnchor(const QString &line)
{
    int position = 0;
    while (position < line.size() && line.at(position).isSpace()) {
        ++position;
    }
    const QString command = commentAnchorCommandAt(line, position);
    if (command.isEmpty() || (command == QLatin1String("\\CommentHighlight") || command == QLatin1String("\\CommentStrikethrough"))) {
        return std::nullopt;
    }
    position += command.size();
    while (position < line.size() && line.at(position).isSpace()) {
        ++position;
    }

    QString id;
    if (!parseBracedArgument(line, &position, &id) || id.trimmed().isEmpty()) {
        return std::nullopt;
    }
    while (position < line.size() && line.at(position).isSpace()) {
        ++position;
    }

    StandaloneCommentAnchor anchor {
        .id = id.trimmed(),
    };
    if (position < line.size() && line.at(position) == QLatin1Char('{')) {
        if (!parseBracedArgument(line, &position, &anchor.mirror)) {
            return std::nullopt;
        }
        anchor.hasMirror = true;
        while (position < line.size() && line.at(position).isSpace()) {
            ++position;
        }
    }
    if (position < line.size() && line.at(position) == QLatin1Char('%')) {
        ++position;
    }
    while (position < line.size() && line.at(position).isSpace()) {
        ++position;
    }
    return position == line.size() ? std::optional<StandaloneCommentAnchor>(anchor) : std::nullopt;
}

QString normalizedReviewContents(QString contents)
{
    return contents.simplified();
}

QString decodeCommentAnchorMirror(const QString &mirror)
{
    static const QList<QPair<QString, QChar>> namedEscapes {
        {QStringLiteral("\\textbackslash{}"), QLatin1Char('\\')},
        {QStringLiteral("\\textasciicircum{}"), QLatin1Char('^')},
        {QStringLiteral("\\textasciitilde{}"), QLatin1Char('~')},
    };
    static const QSet<QChar> escapedCharacters {
        QLatin1Char('{'),
        QLatin1Char('}'),
        QLatin1Char('%'),
        QLatin1Char('#'),
        QLatin1Char('&'),
        QLatin1Char('$'),
        QLatin1Char('_'),
    };

    QString decoded;
    decoded.reserve(mirror.size());
    int position = 0;
    while (position < mirror.size()) {
        bool matched = false;
        for (const auto &[encoded, character] : namedEscapes) {
            if (mirror.mid(position).startsWith(encoded)) {
                decoded += character;
                position += encoded.size();
                matched = true;
                break;
            }
        }
        if (matched) {
            continue;
        }
        if (mirror.at(position) == QLatin1Char('\\') && position + 1 < mirror.size() && escapedCharacters.contains(mirror.at(position + 1))) {
            decoded += mirror.at(position + 1);
            position += 2;
            continue;
        }
        decoded += mirror.at(position++);
    }
    return normalizedReviewContents(decoded);
}

QString commentAnchorMirror(QString contents)
{
    contents = normalizedReviewContents(contents);
    QString encoded;
    encoded.reserve(contents.size());
    for (const QChar character : std::as_const(contents)) {
        switch (character.unicode()) {
        case '\\':
            encoded += QStringLiteral("\\textbackslash{}");
            break;
        case '{':
            encoded += QStringLiteral("\\{");
            break;
        case '}':
            encoded += QStringLiteral("\\}");
            break;
        case '%':
            encoded += QStringLiteral("\\%");
            break;
        case '#':
            encoded += QStringLiteral("\\#");
            break;
        case '&':
            encoded += QStringLiteral("\\&");
            break;
        case '$':
            encoded += QStringLiteral("\\$");
            break;
        case '_':
            encoded += QStringLiteral("\\_");
            break;
        case '^':
            encoded += QStringLiteral("\\textasciicircum{}");
            break;
        case '~':
            encoded += QStringLiteral("\\textasciitilde{}");
            break;
        default:
            encoded += character;
            break;
        }
    }
    return encoded;
}

QString reviewContentsHash(const QString &contents)
{
    return sha256(normalizedReviewContents(contents).toUtf8());
}

struct SourceMirror {
    bool valid = false;
    bool hasMirror = false;
    QString contents;
    std::optional<QString> selectedText;
    QString path;
    qint64 modifiedMs = std::numeric_limits<qint64>::min();
};

SourceMirror sourceMirrorAt(const DocsAnchorOccurrence &occurrence, const QString &anchorId, QString *error)
{
    SourceMirror result;
    result.path = canonicalOrAbsolutePath(occurrence.path);
    QString text;
    bool trailingNewline = false;
    if (!readSourceText(result.path, &text, &trailingNewline, error)) {
        return result;
    }
    if (isMarkdownSourcePath(result.path)) {
        const auto markers = markdownCommentMirrors(text);
        const auto found = markers.constFind(anchorId);
        if (found != markers.cend() && found.value().first == occurrence.line) {
            result.valid = result.hasMirror = true;
            result.contents = found.value().second;
            result.modifiedMs = QFileInfo(result.path).lastModified().toMSecsSinceEpoch();
        }
        return result;
    }
    const QList<CommentAnchorSpan> matching = commentAnchorSpans(text, anchorId);
    const auto found = std::find_if(matching.cbegin(), matching.cend(), [&occurrence](const CommentAnchorSpan &span) {
        return span.line == occurrence.line;
    });
    if (found == matching.cend()) {
        if (error) {
            *error = QStringLiteral("anchor marker line is unavailable");
        }
        return result;
    }
    result.valid = true;
    result.hasMirror = found->hasMirror;
    result.contents = found->hasMirror ? decodeCommentAnchorMirror(found->mirror) : QString();
    if (found->hasBody) result.selectedText = decodeCommentAnchorMirror(found->body);
    result.modifiedMs = QFileInfo(result.path).lastModified().toUTC().toMSecsSinceEpoch();
    return result;
}

QString commentAnchorKind(const DocsAnchorOccurrence &occurrence, const QString &anchorId)
{
    QString text;
    bool trailingNewline = false;
    QString ignored;
    if (!readSourceText(occurrence.path, &text, &trailingNewline, &ignored)) {
        return QStringLiteral("standalone");
    }
    for (const CommentAnchorSpan &span : commentAnchorSpans(text, anchorId)) {
        if (span.line == occurrence.line) {
            return span.hasBody ? QStringLiteral("wrapped_highlight") : QStringLiteral("standalone");
        }
    }
    return QStringLiteral("standalone");
}

QDateTime canonicalRecordTimestamp(const QJsonObject &record, const QString &canonicalPath)
{
    QDateTime timestamp = parsePdfDate(record.value(QStringLiteral("updated_at")));
    if (!timestamp.isValid()) {
        timestamp = commentTimestamp(record);
    }
    if (!timestamp.isValid()) {
        timestamp = QFileInfo(canonicalPath).lastModified().toUTC();
    }
    return timestamp;
}

QJsonObject sourceMirrorState(const QString &key, const QJsonObject &record, const SourceMirror &source)
{
    return {
        {QStringLiteral("anchor_id"), record.value(QStringLiteral("anchor_id")).toString()},
        {QStringLiteral("contents_hash"), reviewContentsHash(record.value(QStringLiteral("contents")).toString())},
        {QStringLiteral("canonical_revision"), std::max(1, record.value(QStringLiteral("revision")).toInt(1))},
        {QStringLiteral("canonical_updated_at"), record.value(QStringLiteral("updated_at"))},
        {QStringLiteral("source_path"), source.path},
        {QStringLiteral("source_mtime_ms"), source.modifiedMs},
        {QStringLiteral("synchronized_at"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
        {QStringLiteral("comment_key"), key},
    };
}

bool hasUsableSourceMirrorState(const QJsonObject &mirrorState, const QString &anchorId)
{
    return !mirrorState.value(QStringLiteral("contents_hash")).toString().isEmpty() && mirrorState.value(QStringLiteral("anchor_id")).toString() == anchorId;
}

struct ReviewTextCandidate {
    enum class Origin {
        Source,
        Pdf,
        Canonical,
    };

    Origin origin = Origin::Pdf;
    QString contents;
    QDateTime timestamp;
    int tiePriority = 0;
};

bool applyLatestReviewText(QJsonObject *comment,
                           const std::optional<QJsonObject> &previous,
                           const QJsonObject &state,
                           const QString &canonicalPath,
                           const DocsAnchorOccurrences &occurrences,
                           bool *sourceImported,
                           QString *error)
{
    *sourceImported = false;
    const QString key = commentKey(*comment);
    const QString anchorId = comment->value(QStringLiteral("anchor_id")).toString().trimmed();
    const QList<DocsAnchorOccurrence> found = occurrences.value(anchorId);
    if (anchorId.isEmpty() || found.size() != 1) {
        return true;
    }
    const QJsonObject baseline = state.value(QStringLiteral("source_mirrors")).toObject().value(key).toObject();
    if (!hasUsableSourceMirrorState(baseline, anchorId)) {
        // Migration is deliberately conservative: establish a baseline from
        // the existing canonical/PDF flow before accepting source text.
        return true;
    }

    const SourceMirror source = sourceMirrorAt(found.constFirst(), anchorId, error);
    if (!source.valid) {
        return false;
    }
    const QString baselineHash = baseline.value(QStringLiteral("contents_hash")).toString();
    QList<ReviewTextCandidate> candidates;
    if (source.hasMirror && reviewContentsHash(source.contents) != baselineHash) {
        candidates.append({
            .origin = ReviewTextCandidate::Origin::Source,
            .contents = source.contents,
            .timestamp = source.modifiedMs == std::numeric_limits<qint64>::min() ? QDateTime() : QDateTime::fromMSecsSinceEpoch(source.modifiedMs, QTimeZone::UTC),
            .tiePriority = 1,
        });
    }
    if (reviewContentsHash(comment->value(QStringLiteral("contents")).toString()) != baselineHash) {
        QDateTime timestamp = commentTimestamp(*comment);
        if (!timestamp.isValid()) {
            timestamp = QDateTime::currentDateTimeUtc();
        }
        candidates.append({
            .origin = ReviewTextCandidate::Origin::Pdf,
            .contents = normalizedReviewContents(comment->value(QStringLiteral("contents")).toString()),
            .timestamp = timestamp,
            .tiePriority = 2,
        });
    }
    if (previous
        && (reviewContentsHash(previous->value(QStringLiteral("contents")).toString()) != baselineHash
            || previous->value(QStringLiteral("revision")).toInt(1) != baseline.value(QStringLiteral("canonical_revision")).toInt(1))) {
        candidates.append({
            .origin = ReviewTextCandidate::Origin::Canonical,
            .contents = normalizedReviewContents(previous->value(QStringLiteral("contents")).toString()),
            .timestamp = canonicalRecordTimestamp(*previous, canonicalPath),
            .tiePriority = 3,
        });
    }
    if (candidates.isEmpty()) {
        return true;
    }
    const auto winner = std::max_element(candidates.cbegin(), candidates.cend(), [](const ReviewTextCandidate &left, const ReviewTextCandidate &right) {
        const qint64 leftMs = left.timestamp.isValid() ? left.timestamp.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min();
        const qint64 rightMs = right.timestamp.isValid() ? right.timestamp.toMSecsSinceEpoch() : std::numeric_limits<qint64>::min();
        return leftMs == rightMs ? left.tiePriority < right.tiePriority : leftMs < rightMs;
    });
    comment->insert(QStringLiteral("contents"), winner->contents);
    if (winner->timestamp.isValid()) {
        comment->insert(QStringLiteral("modified"), winner->timestamp.toUTC().toString(Qt::ISODateWithMs));
    }
    *sourceImported = winner->origin == ReviewTextCandidate::Origin::Source;
    return true;
}

bool refreshSourceMirrorStates(QJsonObject *state, const QList<QJsonObject> &records, const DocsAnchorOccurrences &occurrences, QString *error)
{
    const QJsonObject previous = state->value(QStringLiteral("source_mirrors")).toObject();
    QJsonObject refreshed;
    for (const QJsonObject &record : records) {
        if (isDeletedRecord(record)) {
            continue;
        }
        const QString key = commentKey(record);
        const QString anchorId = record.value(QStringLiteral("anchor_id")).toString().trimmed();
        const QList<DocsAnchorOccurrence> found = occurrences.value(anchorId);
        if (anchorId.isEmpty() || found.size() != 1) {
            continue;
        }
        const SourceMirror source = sourceMirrorAt(found.constFirst(), anchorId, error);
        if (!source.valid) {
            return false;
        }
        if (source.hasMirror && reviewContentsHash(source.contents) == reviewContentsHash(record.value(QStringLiteral("contents")).toString())) {
            refreshed.insert(key, sourceMirrorState(key, record, source));
        } else if (previous.contains(key)) {
            // Preserve the old common ancestor. A later source-only pass can
            // then recognize and import a genuine edit instead of blessing a
            // mismatched value as synchronized.
            refreshed.insert(key, previous.value(key));
        }
    }
    state->insert(QStringLiteral("source_mirrors"), refreshed);
    return true;
}

QString commentAnchorLine(const QString &anchorId, const QString &contents)
{
    return QStringLiteral("\\CommentAnchor{") + anchorId + QStringLiteral("}{") + commentAnchorMirror(contents) + QStringLiteral("}%");
}

bool safeReviewHighlightBody(const QString &body)
{
    if (body.trimmed().isEmpty() || ((body.contains(QLatin1String("\\CommentHighlight")) || body.contains(QLatin1String("\\CommentStrikethrough"))) || body.contains(QLatin1String("\\CommentAnchor")) || body.contains(QLatin1String("\\ReviewAnchor")))
        || body.contains(QLatin1String("\\verb")) || body.contains(QLatin1String("\\documentclass"))
        || body.contains(QLatin1String("\\begin{document}")) || body.contains(QLatin1String("\\end{document}"))) {
        return false;
    }
    for (const QString &environment : UnsafeEnvironments) {
        if (body.contains(QStringLiteral("\\begin{%1}").arg(environment))
            || body.contains(QStringLiteral("\\end{%1}").arg(environment))) {
            return false;
        }
    }
    int depth = 0;
    for (int index = 0; index < body.size(); ++index) {
        const QChar character = body.at(index);
        if (character == QLatin1Char('\\')) {
            ++index;
            continue;
        }
        if (character == QLatin1Char('%')) {
            return false;
        }
        if (character == QLatin1Char('{')) {
            ++depth;
        } else if (character == QLatin1Char('}')) {
            if (--depth < 0) {
                return false;
            }
        }
    }
    return depth == 0;
}

struct UniqueSelectedTextResolution {
    bool valid = false;
    QString reason;
    QString sourceId;
    QString sourcePath;
    int line = 0;
};

UniqueSelectedTextResolution uniqueSelectedTextSource(const QString &docsRoot, const QString &selectedText)
{
    UniqueSelectedTextResolution result;
    const QString normalizedSelection = selectedText.simplified();
    if (sourceResolutionTokens(normalizedSelection).size() < 4) {
        result.reason = QStringLiteral("selected_text_not_distinctive");
        return result;
    }
    const QStringList tokens = normalizedSelection.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList escapedTokens;
    escapedTokens.reserve(tokens.size());
    for (const QString &token : tokens) {
        escapedTokens.append(QRegularExpression::escape(token));
    }
    const QRegularExpression selectionExpression(
        escapedTokens.join(QStringLiteral("\\s+")),
        QRegularExpression::UseUnicodePropertiesOption);
    if (!selectionExpression.isValid()) {
        result.reason = QStringLiteral("selected_text_invalid");
        return result;
    }

    const QSet<QString> managedPaths = managedSourceFiles(docsRoot);
    QStringList paths(managedPaths.begin(), managedPaths.end());
    std::sort(paths.begin(), paths.end());
    int matches = 0;
    bool inventoryComplete = true;
    bool safe = false;
    for (const QString &path : std::as_const(paths)) {
        QString text;
        bool trailingNewline = false;
        if (!readSourceText(path, &text, &trailingNewline, nullptr)) {
            inventoryComplete = false;
            continue;
        }
        QRegularExpressionMatchIterator iterator = selectionExpression.globalMatch(text);
        while (iterator.hasNext()) {
            const QRegularExpressionMatch match = iterator.next();
            ++matches;
            if (matches > 1) {
                result.reason = QStringLiteral("selected_text_ambiguous");
                return result;
            }
            result.sourcePath = path;
            result.sourceId = relativeToDocs(path, docsRoot);
            result.line = text.left(match.capturedStart()).count(QLatin1Char('\n')) + 1;
            safe = safeReviewHighlightBody(match.captured())
                && !UnsafeEnvironments.contains(environmentAtLine(text.split(QLatin1Char('\n')), result.line));
        }
    }
    if (!inventoryComplete) {
        result.reason = QStringLiteral("selected_text_inventory_incomplete");
        return result;
    }
    if (matches == 0) {
        result.reason = QStringLiteral("selected_text_not_found");
        return result;
    }
    if (!safe || !markerEligibleSourcePath(docsRoot, result.sourceId, result.sourcePath)) {
        result.reason = QStringLiteral("selected_text_not_marker_safe");
        return result;
    }
    result.valid = true;
    return result;
}

int unsafeHighlightBoundaryInsertionLine(const QStringList &lines,
                                         int targetLine,
                                         const QString &selectedText)
{
    Q_UNUSED(targetLine)
    const QString normalizedSelection = normalizedSourceLine(selectedText);
    if (normalizedSelection.isEmpty()) {
        return 0;
    }
    QList<int> candidates;
    for (int index = 0; index < lines.size(); ++index) {
        const QString normalized = normalizedSourceLine(lines.at(index));
        if (!normalized.contains(normalizedSelection)) {
            continue;
        }
        const int line = index + 1;
        if (UnsafeEnvironments.contains(environmentAtLine(lines, line))) {
            candidates.append(line);
        }
    }
    if (candidates.size() != 1) {
        return 0;
    }
    const QString environment = environmentAtLine(lines, candidates.constFirst());
    const QRegularExpression endExpression(
        QStringLiteral("\\\\end\\{%1\\}").arg(QRegularExpression::escape(environment)));
    for (int index = candidates.constFirst() - 1; index < lines.size(); ++index) {
        if (endExpression.match(texLineWithoutComment(lines.at(index))).hasMatch()) {
            return index + 2;
        }
    }
    return 0;
}

bool isUnsafeHighlightBoundaryAnchor(const QStringList &lines,
                                     int anchorLine,
                                     const QString &selectedText)
{
    const QString normalizedSelection = normalizedSourceLine(selectedText);
    if (normalizedSelection.isEmpty()) {
        return false;
    }
    for (int index = 0; index < lines.size(); ++index) {
        if (!normalizedSourceLine(lines.at(index)).contains(normalizedSelection)) {
            continue;
        }
        const int line = index + 1;
        const QString environment = environmentAtLine(lines, line);
        if (!UnsafeEnvironments.contains(environment)) {
            continue;
        }
        const QRegularExpression endExpression(
            QStringLiteral("\\\\end\\{%1\\}").arg(QRegularExpression::escape(environment)));
        for (int endIndex = index; endIndex < lines.size(); ++endIndex) {
            if (endExpression.match(texLineWithoutComment(lines.at(endIndex))).hasMatch()) {
                if (endIndex + 2 == anchorLine) {
                    return true;
                }
                break;
            }
        }
    }
    return false;
}

bool wrapHighlightedSource(const QString &sourcePath,
                           int targetLine,
                           const QString &anchorId,
                           const QString &contents,
                           const QString &selectedText,
                           const QString &subtype,
                           int *anchorLine,
                           QString *error,
                           bool dryRun = false)
{
    const QString normalizedSelection = selectedText.simplified();
    if (normalizedSelection.isEmpty()) {
        return false;
    }
    const QStringList tokens = normalizedSelection.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    QStringList escapedTokens;
    escapedTokens.reserve(tokens.size());
    for (const QString &token : tokens) {
        escapedTokens.append(QRegularExpression::escape(token));
    }
    const QRegularExpression selectionExpression(
        escapedTokens.join(QStringLiteral("\\s+")),
        QRegularExpression::UseUnicodePropertiesOption);
    if (!selectionExpression.isValid()) {
        return false;
    }

    QString text;
    bool trailingNewline = false;
    if (!readSourceText(sourcePath, &text, &trailingNewline, error)) {
        return false;
    }
    struct Candidate {
        int start = -1;
        int length = 0;
        int line = 0;
        int distance = 0;
    };
    QList<Candidate> candidates;
    QRegularExpressionMatchIterator iterator = selectionExpression.globalMatch(text);
    while (iterator.hasNext()) {
        const QRegularExpressionMatch match = iterator.next();
        const int line = text.left(match.capturedStart()).count(QLatin1Char('\n')) + 1;
        const int endLine = line + match.captured().count(QLatin1Char('\n'));
        const int distance = targetLine < line ? line - targetLine : (targetLine > endLine ? targetLine - endLine : 0);
        if (distance <= 8 && safeReviewHighlightBody(match.captured())) {
            candidates.append({
                static_cast<int>(match.capturedStart()),
                static_cast<int>(match.capturedLength()),
                line,
                distance,
            });
        }
    }
    if (candidates.isEmpty()) {
        int lineStart = 0;
        for (int line = 1; line < targetLine; ++line) {
            lineStart = text.indexOf(QLatin1Char('\n'), lineStart);
            if (lineStart < 0) {
                return false;
            }
            ++lineStart;
        }
        static const QRegularExpression headingCommand(
            QStringLiteral("[ \\t]*\\\\(?:chapter|section|subsection|subsubsection|paragraph)\\*?"));
        const QRegularExpressionMatch heading =
            headingCommand.match(text, lineStart, QRegularExpression::NormalMatch, QRegularExpression::AnchorAtOffsetMatchOption);
        if (!heading.hasMatch()) {
            return false;
        }
        int position = heading.capturedEnd();
        while (position < text.size() && (text.at(position) == QLatin1Char(' ') || text.at(position) == QLatin1Char('\t'))) {
            ++position;
        }
        if (position < text.size() && text.at(position) == QLatin1Char('[')) {
            if (!skipDelimitedArgument(text, &position, QLatin1Char('['), QLatin1Char(']'))) {
                return false;
            }
            while (position < text.size() && (text.at(position) == QLatin1Char(' ') || text.at(position) == QLatin1Char('\t'))) {
                ++position;
            }
        }
        if (position >= text.size() || text.at(position) != QLatin1Char('{')) {
            return false;
        }
        const int bodyStart = position + 1;
        QString headingBody;
        if (!parseBracedArgument(text, &position, &headingBody) || !safeReviewHighlightBody(headingBody)) {
            return false;
        }
        QString normalizedSelected = selectedText.simplified().toCaseFolded();
        static const QRegularExpression generatedNumber(QStringLiteral("^\\d+(?:\\.\\d+)*\\.?\\s+"));
        normalizedSelected.remove(generatedNumber);
        if (normalizedSelected != normalizedSourceLine(headingBody)) {
            return false;
        }
        candidates.append({
            bodyStart,
            position - bodyStart - 1,
            targetLine,
            0,
        });
    }
    std::sort(candidates.begin(), candidates.end(), [](const Candidate &left, const Candidate &right) {
        return std::tie(left.distance, left.start) < std::tie(right.distance, right.start);
    });
    if (candidates.size() > 1 && candidates.at(0).distance == candidates.at(1).distance) {
        return false;
    }

    const Candidate candidate = candidates.constFirst();
    const QString opening = (subtype == QLatin1String("StrikeOut") ? QStringLiteral("\\CommentStrikethrough{") : QStringLiteral("\\CommentHighlight{")) + anchorId + QStringLiteral("}{")
        + commentAnchorMirror(contents) + QStringLiteral("}{");
    text.insert(candidate.start + candidate.length, QLatin1Char('}'));
    text.insert(candidate.start, opening);
    if (!dryRun && !writeSourceText(sourcePath, text, trailingNewline, error)) {
        return false;
    }
    if (anchorLine) {
        *anchorLine = candidate.line;
    }
    return true;
}

bool rewriteAnchorMirror(const DocsAnchorOccurrence &occurrence, const QString &anchorId, const QString &contents, bool *changed, QString *error)
{
    *changed = false;
    if (isMarkdownSourcePath(occurrence.path)) return rewriteMarkdownAnchorMirror(occurrence.path, anchorId, contents, changed, error);
    QString text;
    bool trailingNewline = false;
    if (!readSourceText(occurrence.path, &text, &trailingNewline, error)) {
        return false;
    }
    const QList<CommentAnchorSpan> matching = commentAnchorSpans(text, anchorId);
    const auto found = std::find_if(matching.cbegin(), matching.cend(), [&occurrence](const CommentAnchorSpan &span) {
        return span.line == occurrence.line;
    });
    if (found == matching.cend() || !found->hasMirror) {
        if (error) {
            *error = QStringLiteral("anchor marker line is unavailable");
        }
        return false;
    }
    const QString expectedMirror = commentAnchorMirror(contents);
    if (found->mirror == expectedMirror) {
        return true;
    }
    text.replace(found->mirrorStart, found->mirrorEnd - found->mirrorStart, expectedMirror);
    if (!writeSourceText(occurrence.path, text, trailingNewline, error)) {
        return false;
    }
    *changed = true;
    return true;
}

bool ensureCommentAnchorDefinition(const QString &sourcePath, int *insertionLine, int *addedLines, QString *error)
{
    *insertionLine = 0;
    *addedLines = 0;
    QStringList lines;
    if (!readTextLines(sourcePath, &lines, error)) {
        return false;
    }
    QString text = lines.join(QLatin1Char('\n'));
    static const QRegularExpression definition(
        QStringLiteral("(\\\\(?:(?:provide|new|renew)command|DeclareRobustCommand)\\s*\\{\\s*\\\\CommentAnchor\\s*\\}\\s*)(?:\\[(\\d+)\\])?"));
    const QRegularExpressionMatch definitionMatch = definition.match(text);
    if (!text.contains(QLatin1String("\\documentclass"))) {
        return true;
    }
    if (text.contains(QLatin1String("\\usepackage{manual_style}"))) {
        return true;
    }
    if (definitionMatch.hasMatch()) {
        if (definitionMatch.captured(2) != QLatin1String("2")) {
            text.replace(definitionMatch.capturedStart(), definitionMatch.capturedLength(), definitionMatch.captured(1) + QStringLiteral("[2]"));
            lines = text.split(QLatin1Char('\n'));
        }
    }

    int insertionIndex = 1;
    static const QRegularExpression beginDocument(QStringLiteral("\\\\begin\\s*\\{document\\}"));
    for (int index = 0; index < lines.size(); ++index) {
        if (beginDocument.match(lines.at(index)).hasMatch()) {
            insertionIndex = index;
            break;
        }
    }
    QStringList support;
    if (!definitionMatch.hasMatch()) {
        support.append({
            QStringLiteral("% Source-review destinations used by the docs comment synchronizer."),
            QStringLiteral("\\DeclareRobustCommand{\\CommentAnchor}[2]{%"),
            QStringLiteral("  \\ifcsname hypertarget\\endcsname"),
            QStringLiteral("    \\hypertarget{review-anchor:\\detokenize{#1}}{}%"),
            QStringLiteral("  \\else"),
            QStringLiteral("    \\pdfdest name {review-anchor:#1} xyz%"),
            QStringLiteral("  \\fi"),
            QStringLiteral("}"),
        });
    }
    if (!text.contains(QLatin1String("\\DeclareRobustCommand{\\CommentHighlight}"))
        && !text.contains(QLatin1String("\\providecommand{\\CommentHighlight}"))) {
        support.append(QStringLiteral("\\DeclareRobustCommand{\\CommentHighlight}[3]{\\CommentAnchor{#1}{#2}{#3}}"));
    }
    if (!text.contains(QLatin1String("\\DeclareRobustCommand{\\CommentStrikethrough}"))
        && !text.contains(QLatin1String("\\providecommand{\\CommentStrikethrough}"))) {
        support.append(QStringLiteral("\\DeclareRobustCommand{\\CommentStrikethrough}[3]{\\CommentAnchor{#1}{#2}{#3}}"));
        support.append(QStringLiteral("\\ifcsname pdfstringdefDisableCommands\\endcsname\\pdfstringdefDisableCommands{\\def\\CommentStrikethrough#1#2#3{#3}}\\fi"));
    }
    if (!text.contains(QLatin1String("% Keep comment anchors out of PDF bookmark strings."))) {
        support.append({
            QStringLiteral("% Keep comment anchors out of PDF bookmark strings."),
            QStringLiteral("\\ifcsname pdfstringdefDisableCommands\\endcsname"),
            QStringLiteral("  \\pdfstringdefDisableCommands{%"),
            QStringLiteral("    \\def\\CommentHighlight#1#2#3{#3}%"),
            QStringLiteral("    \\def\\CommentAnchor#1#2{}%"),
            QStringLiteral("    \\def\\ReviewAnchor#1#2{}%"),
            QStringLiteral("  }"),
            QStringLiteral("\\fi"),
        });
    }
    if (support.isEmpty()) {
        return true;
    }
    for (int offset = 0; offset < support.size(); ++offset) {
        lines.insert(insertionIndex + offset, support.at(offset));
    }
    if (!writeTextLines(sourcePath, lines, true, error)) {
        return false;
    }
    *insertionLine = insertionIndex + 1;
    *addedLines = support.size();
    return true;
}

bool isStandaloneAnchor(const QString &line, const QString &anchorId)
{
    const std::optional<StandaloneCommentAnchor> parsed = standaloneCommentAnchor(line);
    return parsed && parsed->id == anchorId;
}

bool removeAnchor(const DocsAnchorOccurrence &occurrence, const QString &anchorId, QString *error);

bool insertOrMoveAnchor(const QString &oldPath, const QString &newPath, int *newLine, const QString &anchorId, const QString &contents, QString *error)
{
    if (!newLine) {
        if (error) {
            *error = QStringLiteral("anchor destination line is unavailable");
        }
        return false;
    }
    if (!oldPath.isEmpty() && QFileInfo(oldPath).absoluteFilePath() == QFileInfo(newPath).absoluteFilePath()) {
        QString oldText;
        bool oldTrailingNewline = false;
        if (!readSourceText(oldPath, &oldText, &oldTrailingNewline, error)) {
            return false;
        }
        const QList<CommentAnchorSpan> existing = commentAnchorSpans(oldText, anchorId);
        if (existing.size() == 1 && existing.constFirst().hasBody) {
            if (!removeAnchor({oldPath, existing.constFirst().line}, anchorId, error)) {
                return false;
            }
            return insertOrMoveAnchor(QString(), newPath, newLine, anchorId, contents, error);
        }
    }
    QStringList newLines;
    if (!readTextLines(newPath, &newLines, error)) {
        return false;
    }
    const bool newTrailingNewline = textFileHasTrailingNewline(newPath);

    if (!oldPath.isEmpty() && QFileInfo(oldPath).absoluteFilePath() == QFileInfo(newPath).absoluteFilePath()) {
        int removedBefore = 0;
        for (int index = newLines.size() - 1; index >= 0; --index) {
            if (isStandaloneAnchor(newLines.at(index), anchorId)) {
                if (index + 1 < *newLine) {
                    ++removedBefore;
                }
                newLines.removeAt(index);
            }
        }
        *newLine -= removedBefore;
    }
    const int insertionIndex = std::clamp(*newLine - 1, 0, static_cast<int>(newLines.size()));
    *newLine = insertionIndex + 1;
    newLines.insert(insertionIndex, commentAnchorLine(anchorId, contents));
    if (!writeTextLines(newPath, newLines, newTrailingNewline, error)) {
        return false;
    }

    if (!oldPath.isEmpty() && QFileInfo(oldPath).absoluteFilePath() != QFileInfo(newPath).absoluteFilePath()) {
        QString oldText;
        bool oldTrailingNewline = false;
        if (!readSourceText(oldPath, &oldText, &oldTrailingNewline, error)) {
            return false;
        }
        const QList<CommentAnchorSpan> existing = commentAnchorSpans(oldText, anchorId);
        if (existing.size() == 1 && existing.constFirst().hasBody) {
            return removeAnchor({oldPath, existing.constFirst().line}, anchorId, error);
        }
        QStringList oldLines;
        if (!readTextLines(oldPath, &oldLines, error)) {
            return false;
        }
        const int before = oldLines.size();
        oldLines.erase(std::remove_if(oldLines.begin(), oldLines.end(), [&anchorId](const QString &line) { return isStandaloneAnchor(line, anchorId); }), oldLines.end());
        if (oldLines.size() == before || !writeTextLines(oldPath, oldLines, true, error)) {
            return false;
        }
    }
    return true;
}

bool removeAnchor(const DocsAnchorOccurrence &occurrence, const QString &anchorId, QString *error)
{
    if (isMarkdownSourcePath(occurrence.path)) return removeMarkdownAnchor(occurrence.path, anchorId, error);
    QString text;
    bool trailingNewline = false;
    if (!readSourceText(occurrence.path, &text, &trailingNewline, error)) {
        return false;
    }
    const QList<CommentAnchorSpan> matching = commentAnchorSpans(text, anchorId);
    if (matching.size() != 1) {
        if (error) {
            *error = QStringLiteral("anchor marker was not uniquely removable");
        }
        return false;
    }
    const CommentAnchorSpan span = matching.constFirst();
    // An empty third argument, as in "\CommentAnchor{id}{text}{}%", is a
    // standalone marker: remove its whole line rather than leave a lone "%".
    if (span.hasBody && !span.body.isEmpty()) {
        text.replace(span.commandStart, span.commandEnd - span.commandStart, span.body);
    } else {
        int lineStart = span.commandStart > 0 ? text.lastIndexOf(QLatin1Char('\n'), span.commandStart - 1) + 1 : 0;
        int lineEnd = text.indexOf(QLatin1Char('\n'), span.commandEnd);
        if (lineEnd < 0) {
            lineEnd = text.size();
        } else {
            ++lineEnd;
        }
        const QString prefix = text.mid(lineStart, span.commandStart - lineStart).trimmed();
        const QString suffix = text.mid(span.commandEnd, lineEnd - span.commandEnd).trimmed();
        if (prefix.isEmpty() && (suffix.isEmpty() || suffix == QLatin1String("%"))) text.remove(lineStart, lineEnd - lineStart);
        else text.remove(span.commandStart, span.commandEnd - span.commandStart);
    }
    return writeSourceText(occurrence.path, text, trailingNewline, error);
}

bool removeOverviewDeletedAnchor(const DocsAnchorOccurrence &occurrence, const QString &anchorId, QString *error)
{
    QStringList lines;
    if (!readTextLines(occurrence.path, &lines, error)) {
        return false;
    }
    const int lineIndex = occurrence.line - 1;
    if (lineIndex >= 0 && lineIndex < lines.size() && isStandaloneAnchor(lines.at(lineIndex), anchorId)) {
        const bool trailingNewline = textFileHasTrailingNewline(occurrence.path);
        lines.removeAt(lineIndex);
        return writeTextLines(occurrence.path, lines, trailingNewline, error);
    }
    return removeAnchor(occurrence, anchorId, error);
}

bool removePlainMarkupAnchor(const DocsCommentSynchronizer::Options &options,
                             DocsAnchorIndex *anchorIndex,
                             const QJsonObject &record,
                             DocsCommentSynchronizer::Result *result,
                             bool *metadataCanBeCleared,
                             QString *error)
{
    *metadataCanBeCleared = false;
    const QString anchorId = record.value(QStringLiteral("anchor_id")).toString().trimmed();
    if (anchorId.isEmpty()) {
        *metadataCanBeCleared = true;
        return true;
    }
    if (!anchorIndex->complete) {
        bool allFilesRead = false;
        DocsAnchorOccurrences completeOccurrences = anchorOccurrences(managedSourceFiles(options.docsRoot), &allFilesRead);
        if (!allFilesRead) {
            result->logLines.append(QStringLiteral("warning: could not verify plain-markup anchor %1 across all managed sources; leaving source unchanged").arg(anchorId));
            return true;
        }
        anchorIndex->occurrences = std::move(completeOccurrences);
        anchorIndex->complete = true;
    }
    const QList<DocsAnchorOccurrence> found = anchorIndex->occurrences.value(anchorId);
    if (found.isEmpty()) {
        *metadataCanBeCleared = true;
        return true;
    }
    if (found.size() != 1) {
        result->logLines.append(QStringLiteral("warning: plain-markup anchor %1 has %2 managed-source matches; leaving source unchanged").arg(anchorId).arg(found.size()));
        return true;
    }
    if (!removeAnchor(found.constFirst(), anchorId, error)) {
        return false;
    }
    refreshAnchorOccurrencesForSource(&anchorIndex->occurrences, found.constFirst().path);
    ++result->anchorsRemoved;
    *metadataCanBeCleared = true;
    return true;
}

struct IndexedNormalizedSourceLine {
    QString text;
    int sourceLine = 0;
};

struct HistoricalSourceSnapshot {
    bool valid = false;
    QString sourceId;
    QString sourcePath;
    QString snapshotPath;
};

struct SourceMoveDestination {
    bool valid = false;
    bool markerAlreadyPresent = false;
    QString sourceId;
    QString sourcePath;
    int targetLine = 0;
    int insertionLine = 0;
};

struct PendingSourceMove {
    QString oldStem;
    QString newStem;
    QString key;
    QString oldSourceId;
    SourceMoveDestination destination;
    QJsonObject record;
};

struct SourceMoveBatchResult {
    bool success = true;
    QString failureReason;
    QStringList logLines;
    int moved = 0;
};

struct SourceAnchorRestorationBatchResult {
    bool success = true;
    QString failureReason;
    QStringList logLines;
    int restored = 0;
};

// The folder owning a docs-relative path: its source owner, else its first segment.
QString sourceOwnerOrTop(const QString &relative)
{
    const QString owner = Thoughts::SourceOwner::of(relative);
    return owner.isEmpty() ? relative.section(QLatin1Char('/'), 0, 0) : owner;
}

QString canonicalStemForManagedSource(const QString &sourceId, const Thoughts::SourceFolderRenames &renames = {})
{
    const QString normalized = QDir::fromNativeSeparators(sourceId);
    const QString owner = Thoughts::SourceOwner::of(normalized);
    // A default folder has no aggregate PDF: each document is its own
    // Library PDF, named after its full source id (spec Rule 2).
    if (Thoughts::SourceOwner::isDefaultFolder(owner)) {
        return normalized.size() > owner.size() + 1 ? QFileInfo(Thoughts::SourceOwner::defaultFolderOutput(normalized)).completeBaseName() : QString();
    }
    const QString projectName = normalized.section(QLatin1Char('/'), 0, 0);
    if (!projectName.startsWith(QLatin1String("docs-")) || projectName.size() <= 5) {
        return {};
    }
    // A renamed folder keeps the stem of its Library PDF, and so its sidecar
    // (source folder names spec, rule F).
    return Thoughts::SourceFolderNames::pdfShortName(projectName, renames);
}

QString canonicalStemForRecordSourcePdf(const QString &docsRoot, const QJsonObject &record)
{
    const QString sourcePdf = record.value(QStringLiteral("source_pdf")).toString().trimmed();
    if (sourcePdf.isEmpty()) {
        return {};
    }
    const QFileInfo sourceInfo(sourcePdf);
    const QString sourcePath = canonicalOrAbsolutePath(sourceInfo.isAbsolute() ? sourceInfo.absoluteFilePath() : QDir(docsRoot).filePath(sourcePdf));
    const QString currentPdfsPath = canonicalOrAbsolutePath(QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    const QFileInfo canonicalInfo(sourcePath);
    if (canonicalOrAbsolutePath(canonicalInfo.absolutePath()) != currentPdfsPath || canonicalInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return {};
    }
    return canonicalInfo.completeBaseName();
}

bool sameUserVisibleComment(const QJsonObject &left, const QJsonObject &right)
{
    for (const QString &field : {
             QStringLiteral("id"),
             QStringLiteral("review_key"),
             QStringLiteral("subtype"),
             QStringLiteral("contents"),
             QStringLiteral("selected_text"),
             QStringLiteral("author"),
             QStringLiteral("created"),
             QStringLiteral("anchor_id"),
         }) {
        if (compactJson(normalizedJson(left.value(field))) != compactJson(normalizedJson(right.value(field)))) {
            return false;
        }
    }
    return true;
}

HistoricalSourceSnapshot historicalSnapshotForComment(const QString &docsRoot, const QJsonObject &comment)
{
    const QString buildId = comment.value(QStringLiteral("source_build_id")).toString().trimmed();
    const QString storedSourceId = comment.value(QStringLiteral("tex_file")).toString().trimmed();
    if (buildId.isEmpty() || QFileInfo(buildId).fileName() != buildId || storedSourceId.isEmpty()) {
        return {};
    }

    const QString buildDirectory = QDir(docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds/") + buildId);
    QJsonObject mapping;
    QString ignored;
    if (!readJsonObject(QDir(buildDirectory).filePath(QStringLiteral("mapping.json")), &mapping, &ignored)) {
        return {};
    }
    const int version = mapping.value(QStringLiteral("version")).toInt();
    if (version == 2) {
        const QFileInfo storedInfo(storedSourceId);
        const QString storedPath = canonicalOrAbsolutePath(storedInfo.isAbsolute() ? storedInfo.absoluteFilePath() : QDir(docsRoot).filePath(storedSourceId));
        for (const QJsonValue &value : mapping.value(QStringLiteral("source_snapshots")).toArray()) {
            const QJsonObject entry = value.toObject();
            if (!entry.value(QStringLiteral("marker_eligible")).toBool(true)) {
                continue;
            }
            const QString sourceId = entry.value(QStringLiteral("source_id")).toString();
            const QString absolute = entry.value(QStringLiteral("source_tex_absolute")).toString();
            const QString sourcePath = canonicalOrAbsolutePath(absolute.isEmpty() ? QDir(docsRoot).filePath(sourceId) : absolute);
            if (sourceId != storedSourceId && sourcePath != storedPath) {
                continue;
            }
            const QString snapshotPath = QDir(buildDirectory).filePath(entry.value(QStringLiteral("snapshot")).toString());
            if (!QFileInfo(snapshotPath).isFile()) {
                return {};
            }
            return {
                true,
                sourceId,
                sourcePath,
                snapshotPath,
            };
        }
        return {};
    }
    if (version != 1 || mapping.value(QStringLiteral("source_id")).toString() != storedSourceId) {
        return {};
    }
    const QString snapshotPath = QDir(buildDirectory).filePath(QStringLiteral("source.tex"));
    if (!QFileInfo(snapshotPath).isFile()) {
        return {};
    }
    const QString absolute = mapping.value(QStringLiteral("source_tex_absolute")).toString();
    return {
        true,
        storedSourceId,
        canonicalOrAbsolutePath(absolute.isEmpty() ? QDir(docsRoot).filePath(storedSourceId) : absolute),
        snapshotPath,
    };
}

QList<IndexedNormalizedSourceLine> indexedNormalizedSourceLines(const QStringList &lines)
{
    QList<IndexedNormalizedSourceLine> normalized;
    normalized.reserve(lines.size());
    for (int index = 0; index < lines.size(); ++index) {
        const QString text = normalizedSourceLine(lines.at(index));
        if (!text.isEmpty()) {
            normalized.append({
                text,
                index + 1,
            });
        }
    }
    return normalized;
}

int commentAnchorInsertionLine(const QStringList &lines, int targetLine, bool allowCommandTarget = false)
{
    const int directInsertionLine = anchorInsertionLine(lines, targetLine, allowCommandTarget);
    if (directInsertionLine > 0) {
        return directInsertionLine;
    }
    if (targetLine < 1 || targetLine > lines.size()) {
        return 0;
    }

    // A standalone destination before \item would precede the first list
    // item, which is invalid TeX. Placing the zero-width marker immediately
    // after a text-bearing item line keeps it inside that item and before the
    // following source line.
    const QString stripped = texLineWithoutComment(lines.at(targetLine - 1)).trimmed();
    static const QRegularExpression textItem(QStringLiteral("^\\\\item(?:\\s*\\[[^\\]]*\\])?\\s+\\S"));
    if (textItem.match(stripped).hasMatch()) {
        return targetLine + 1;
    }
    return 0;
}

bool reviewHeadingCommandLine(const QStringList &lines, int targetLine);

struct StructuralAnchorPlacement {
    int line = 0;
    QString scope;
};

StructuralAnchorPlacement structuralAnchorPlacement(const QStringList &lines, int targetLine)
{
    if (targetLine < 1 || targetLine > lines.size()) {
        return {};
    }

    const QString environment = environmentAtLine(lines, targetLine);
    if (!environment.isEmpty() && UnsafeEnvironments.contains(environment)) {
        static const QRegularExpression begin(QStringLiteral("\\\\begin\\s*\\{([^{}]+)\\}"));
        static const QRegularExpression end(QStringLiteral("\\\\end\\s*\\{([^{}]+)\\}"));
        int depth = 1;
        for (int index = targetLine - 1; index < lines.size(); ++index) {
            const QString source = texLineWithoutComment(lines.at(index));
            auto beginMatches = begin.globalMatch(source);
            while (beginMatches.hasNext()) {
                if (beginMatches.next().captured(1) == environment && index + 1 != targetLine) {
                    ++depth;
                }
            }
            auto endMatches = end.globalMatch(source);
            while (endMatches.hasNext()) {
                if (endMatches.next().captured(1) == environment && --depth == 0) {
                    const QString scope = environment == QLatin1String("figure")
                        ? QStringLiteral("figure")
                        : (environment == QLatin1String("DocCode") || environment == QLatin1String("lstlisting")
                               ? QStringLiteral("verbatim_block")
                               : (environment.contains(QLatin1String("math")) || environment.startsWith(QLatin1String("equation"))
                                      || environment.startsWith(QLatin1String("align"))
                                      ? QStringLiteral("math_block")
                                      : QStringLiteral("macro")));
                    return {std::min(index + 2, static_cast<int>(lines.size()) + 1), scope};
                }
            }
        }
        return {};
    }

    const QString stripped = texLineWithoutComment(lines.at(targetLine - 1)).trimmed();
    if (reviewHeadingCommandLine(lines, targetLine)) {
        return {targetLine, QStringLiteral("heading")};
    }
    if (stripped.startsWith(QLatin1String("\\includegraphics")) || stripped.contains(QLatin1String("\\includegraphics"))) {
        return {std::min(targetLine + 1, static_cast<int>(lines.size()) + 1), QStringLiteral("figure")};
    }
    if (stripped.startsWith(QLatin1String("\\input")) || stripped.contains(QLatin1String("\\input"))) {
        return {std::min(targetLine + 1, static_cast<int>(lines.size()) + 1), QStringLiteral("generated_input")};
    }
    if (stripped.startsWith(QLatin1String("\\]")) || stripped.startsWith(QLatin1String("\\end"))) {
        return {std::min(targetLine + 1, static_cast<int>(lines.size()) + 1), QStringLiteral("math_block")};
    }
    if (stripped.startsWith(QLatin1Char('\\')) || stripped.startsWith(QLatin1Char('{')) || stripped.startsWith(QLatin1Char('['))) {
        return {std::min(targetLine + 1, static_cast<int>(lines.size()) + 1), QStringLiteral("macro")};
    }
    return {targetLine, QStringLiteral("paragraph")};
}

bool reviewHeadingCommandLine(const QStringList &lines, int targetLine)
{
    if (targetLine < 1 || targetLine > lines.size()) {
        return false;
    }
    static const QRegularExpression heading(
        QStringLiteral("^\\s*\\\\(?:chapter|section|subsection|subsubsection|paragraph)\\*?\\s*(?:\\[[^\\]]*\\]\\s*)?\\{"));
    return heading.match(texLineWithoutComment(lines.at(targetLine - 1))).hasMatch();
}

SourceMoveDestination sourceMoveDestination(const QString &docsRoot, const QJsonObject &comment, const DocsAnchorOccurrences &occurrences, const QSet<QString> &currentSources)
{
    const QString storedSourceId = comment.value(QStringLiteral("tex_file")).toString().trimmed();
    if (storedSourceId.isEmpty()) {
        return {};
    }
    const QFileInfo storedInfo(storedSourceId);
    const QString storedPath = canonicalOrAbsolutePath(storedInfo.isAbsolute() ? storedInfo.absoluteFilePath() : QDir(docsRoot).filePath(storedSourceId));
    const QString storedOwner = sourceOwnerOrTop(relativeToDocs(storedPath, docsRoot));

    const QString anchorId = comment.value(QStringLiteral("anchor_id")).toString().trimmed();
    const QList<DocsAnchorOccurrence> found = occurrences.value(anchorId);
    if (!anchorId.isEmpty() && found.size() == 1) {
        const QString destinationPath = canonicalOrAbsolutePath(found.constFirst().path);
        const QString destinationId = relativeToDocs(destinationPath, docsRoot);
        if (destinationPath != storedPath && sourceOwnerOrTop(destinationId) == storedOwner
            && QFileInfo(destinationPath).isFile() && markerEligibleSourcePath(docsRoot, destinationId, destinationPath)) {
            return {
                true,
                true,
                destinationId,
                destinationPath,
                found.constFirst().line,
                found.constFirst().line,
            };
        }
    }

    if (QFileInfo(storedPath).isFile()) {
        return {};
    }
    const HistoricalSourceSnapshot historical = historicalSnapshotForComment(docsRoot, comment);
    if (!historical.valid) {
        return {};
    }

    const QJsonValue recordedLineValue = comment.value(QStringLiteral("tex_line"));
    const bool lineOk = recordedLineValue.isDouble();
    const int recordedLine = recordedLineValue.toInt();
    QStringList snapshotLines;
    QString ignored;
    if (!lineOk || !readTextLines(historical.snapshotPath, &snapshotLines, &ignored)) {
        return {};
    }
    const QList<IndexedNormalizedSourceLine> normalizedSnapshot = indexedNormalizedSourceLines(snapshotLines);
    int targetIndex = -1;
    for (int index = 0; index < normalizedSnapshot.size(); ++index) {
        if (normalizedSnapshot.at(index).sourceLine == recordedLine) {
            targetIndex = index;
            break;
        }
    }
    if (targetIndex < 0) {
        return {};
    }
    const int signatureStart = std::max(0, targetIndex - 2);
    const int signatureEnd = std::min(static_cast<int>(normalizedSnapshot.size()), targetIndex + 3);
    if (signatureEnd - signatureStart < 3) {
        return {};
    }
    QStringList signature;
    for (int index = signatureStart; index < signatureEnd; ++index) {
        signature.append(normalizedSnapshot.at(index).text);
    }
    const int targetOffset = targetIndex - signatureStart;

    QList<SourceMoveDestination> matches;
    for (const QString &sourcePath : currentSources) {
        const QString canonicalPath = canonicalOrAbsolutePath(sourcePath);
        if (canonicalPath == storedPath || !QFileInfo(canonicalPath).isFile()) {
            continue;
        }
        const QString sourceId = relativeToDocs(canonicalPath, docsRoot);
        if (sourceOwnerOrTop(sourceId) != storedOwner
            || !markerEligibleSourcePath(docsRoot, sourceId, canonicalPath)) {
            continue;
        }
        QStringList currentLines;
        if (!readTextLines(canonicalPath, &currentLines, &ignored)) {
            continue;
        }
        const QList<IndexedNormalizedSourceLine> normalizedCurrent = indexedNormalizedSourceLines(currentLines);
        if (normalizedCurrent.size() < signature.size()) {
            continue;
        }
        for (int offset = 0; offset + signature.size() <= normalizedCurrent.size(); ++offset) {
            bool same = true;
            for (int signatureIndex = 0; signatureIndex < signature.size(); ++signatureIndex) {
                if (normalizedCurrent.at(offset + signatureIndex).text != signature.at(signatureIndex)) {
                    same = false;
                    break;
                }
            }
            if (!same) {
                continue;
            }
            const int targetLine = normalizedCurrent.at(offset + targetOffset).sourceLine;
            const int insertionLine = commentAnchorInsertionLine(currentLines, targetLine);
            if (insertionLine > 0) {
                matches.append({
                    true,
                    false,
                    sourceId,
                    canonicalPath,
                    targetLine,
                    insertionLine,
                });
            }
        }
    }
    return matches.size() == 1 ? matches.constFirst() : SourceMoveDestination {};
}

QJsonObject movedLiveRecord(const QString &docsRoot, const PendingSourceMove &move, int markerLine, const QStringList &destinationLines)
{
    QJsonObject record = upgradedRecord(move.record, QStringLiteral("mac-tex-move"));
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    record.insert(QStringLiteral("revision"), std::max(1, record.value(QStringLiteral("revision")).toInt(1)) + 1);
    record.insert(QStringLiteral("updated_at"), timestamp);
    record.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-move"));
    record.insert(QStringLiteral("source_pdf"), QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(move.newStem)));
    record.insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
    record.insert(QStringLiteral("anchor_confidence"), QStringLiteral("high"));
    record.insert(QStringLiteral("tex_file"), move.destination.sourceId);
    record.insert(QStringLiteral("tex_line"), markerLine);
    record.insert(QStringLiteral("tex_column"), -1);
    record.insert(QStringLiteral("source_context"), sourceContext(destinationLines, move.destination.targetLine));
    record.insert(QStringLiteral("moved_from_pdf_stem"), move.oldStem);
    record.insert(QStringLiteral("moved_from_tex_file"), move.oldSourceId);
    record.remove(QStringLiteral("source_build_id"));
    record.remove(QStringLiteral("source_fingerprint"));
    record.remove(QStringLiteral("deleted"));
    record.remove(QStringLiteral("deleted_at"));
    record.remove(QStringLiteral("source_anchor_deletion"));
    record.remove(QStringLiteral("deletion_reason"));
    return record;
}

QJsonObject movedSourceTombstone(const PendingSourceMove &move)
{
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QJsonObject tombstone = sidecarTombstone(move.record, QJsonObject {{QStringLiteral("deleted_at"), timestamp}});
    tombstone.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-move"));
    tombstone.insert(QStringLiteral("deletion_reason"), QStringLiteral("source_moved"));
    tombstone.insert(QStringLiteral("moved_to_pdf_stem"), move.newStem);
    tombstone.insert(QStringLiteral("moved_to_tex_file"), move.destination.sourceId);
    tombstone.remove(QStringLiteral("source_anchor_deletion"));
    return tombstone;
}

int indexOfRecord(const QList<QJsonObject> &records, const QString &key)
{
    for (int index = 0; index < records.size(); ++index) {
        if (commentKey(records.at(index)) == key) {
            return index;
        }
    }
    return -1;
}

SourceMoveBatchResult repairMisroutedSidecarRecords(const DocsCommentSynchronizer::Options &options, const QString &onlyStem = QString())
{
    SourceMoveBatchResult result;
    const QDir commentsDir(QDir(options.docsRoot).filePath(QStringLiteral("comments")));
    QHash<QString, QList<QJsonObject>> recordsByStem;
    QStringList stems;
    QString error;
    for (const QFileInfo &sidecar : legacyDocumentSidecars(commentsDir)) {
        QString stem = sidecar.fileName();
        stem.chop(QStringLiteral("_comments.jsonl").size());
        QList<QJsonObject> records;
        if (!reconcileJsonl(sidecar.absoluteFilePath(), &records, &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not read %1: %2").arg(sidecar.fileName(), error);
            return result;
        }
        stems.append(stem);
        recordsByStem.insert(stem, records);
    }

    struct Repair {
        QString oldStem;
        QString newStem;
        QString key;
        QJsonObject record;
    };
    QList<Repair> repairs;
    QHash<QString, QJsonObject> missingDestinationClaims;
    for (const QString &oldStem : std::as_const(stems)) {
        if (!onlyStem.isEmpty() && oldStem != onlyStem) {
            continue;
        }
        const QList<QJsonObject> records = recordsByStem.value(oldStem);
        for (const QJsonObject &record : records) {
            if (isDeletedRecord(record) || !isTrackedCommentRecordImpl(record)) {
                continue;
            }
            const QString newStem = canonicalStemForRecordSourcePdf(options.docsRoot, record);
            if (newStem.isEmpty() || newStem == oldStem) {
                continue;
            }
            if (!recordsByStem.contains(newStem)) {
                result.success = false;
                result.failureReason = QStringLiteral("misrouted comment %1 in %2 has no destination sidecar %3").arg(commentKey(record), oldStem, newStem);
                return result;
            }
            const QString key = commentKey(record);
            const int destinationIndex = indexOfRecord(recordsByStem.value(newStem), key);
            if (destinationIndex >= 0) {
                const QJsonObject destination = recordsByStem.value(newStem).at(destinationIndex);
                if (isDeletedRecord(destination) || !sameUserVisibleComment(record, destination)) {
                    result.success = false;
                    result.failureReason = QStringLiteral("misrouted comment %1 conflicts between %2 and %3; both records were preserved").arg(key, oldStem, newStem);
                    return result;
                }
            } else {
                const QString claim = newStem + QChar(0x1f) + key;
                if (missingDestinationClaims.contains(claim) && !sameUserVisibleComment(record, missingDestinationClaims.value(claim))) {
                    result.success = false;
                    result.failureReason = QStringLiteral("misrouted comment %1 has conflicting candidate records for %2; all records were preserved").arg(key, newStem);
                    return result;
                }
                missingDestinationClaims.insert(claim, record);
            }
            repairs.append({oldStem, newStem, key, record});
        }
    }
    if (repairs.isEmpty()) {
        return result;
    }

    QSet<QString> lockStems;
    for (const Repair &repair : std::as_const(repairs)) {
        lockStems.insert(repair.oldStem);
        lockStems.insert(repair.newStem);
    }
    QStringList sortedLockStems(lockStems.begin(), lockStems.end());
    std::sort(sortedLockStems.begin(), sortedLockStems.end());
    std::vector<std::unique_ptr<QLockFile>> locks;
    for (const QString &stem : std::as_const(sortedLockStems)) {
        const QString lockPath = commentSyncLockPath(options.docsRoot, stem);
        if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
            result.success = false;
            result.failureReason = QStringLiteral("could not create the synchronization lock directory");
            return result;
        }
        auto lock = std::make_unique<QLockFile>(lockPath);
        lock->setStaleLockTime(300000);
        if (!lock->tryLock(options.lockTimeoutMs)) {
            result.success = false;
            result.failureReason = QStringLiteral("another synchronization is already updating %1").arg(stem);
            return result;
        }
        locks.push_back(std::move(lock));
    }

    QSet<QString> destinationStems;
    QSet<QString> oldStems;
    for (const Repair &repair : std::as_const(repairs)) {
        int destinationIndex = indexOfRecord(recordsByStem.value(repair.newStem), repair.key);
        if (destinationIndex < 0) {
            QJsonObject destination = upgradedRecord(repair.record, QStringLiteral("mac-tex-move"));
            const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            destination.insert(QStringLiteral("revision"), std::max(1, destination.value(QStringLiteral("revision")).toInt(1)) + 1);
            destination.insert(QStringLiteral("updated_at"), timestamp);
            destination.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-move"));
            destination.insert(QStringLiteral("moved_from_pdf_stem"), repair.oldStem);
            destination.remove(QStringLiteral("deleted"));
            destination.remove(QStringLiteral("deleted_at"));
            destination.remove(QStringLiteral("deletion_reason"));
            destination.remove(QStringLiteral("source_anchor_deletion"));
            recordsByStem[repair.newStem].append(destination);
            destinationStems.insert(repair.newStem);
        }

        const int oldIndex = indexOfRecord(recordsByStem.value(repair.oldStem), repair.key);
        if (oldIndex >= 0) {
            const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
            QJsonObject tombstone = sidecarTombstone(repair.record, QJsonObject {{QStringLiteral("deleted_at"), timestamp}});
            tombstone.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-move"));
            tombstone.insert(QStringLiteral("deletion_reason"), QStringLiteral("source_moved"));
            tombstone.insert(QStringLiteral("moved_to_pdf_stem"), repair.newStem);
            tombstone.insert(QStringLiteral("moved_to_tex_file"), repair.record.value(QStringLiteral("tex_file")));
            tombstone.remove(QStringLiteral("source_anchor_deletion"));
            recordsByStem[repair.oldStem][oldIndex] = tombstone;
            oldStems.insert(repair.oldStem);
        }
        ++result.moved;
        result.logLines.append(QStringLiteral("retired misrouted %1: %2 -> %3").arg(repair.key, repair.oldStem, repair.newStem));
    }

    QStringList destinationOrder(destinationStems.begin(), destinationStems.end());
    QStringList oldOrder(oldStems.begin(), oldStems.end());
    std::sort(destinationOrder.begin(), destinationOrder.end());
    std::sort(oldOrder.begin(), oldOrder.end());
    for (const QString &stem : std::as_const(destinationOrder)) {
        if (!writeJsonl(commentsDir.filePath(stem + QStringLiteral("_comments.jsonl")), reindexComments(recordsByStem.value(stem)), &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not write repaired destination sidecar %1: %2").arg(stem, error);
            return result;
        }
    }
    for (const QString &stem : std::as_const(oldOrder)) {
        if (!writeJsonl(commentsDir.filePath(stem + QStringLiteral("_comments.jsonl")), reindexComments(recordsByStem.value(stem)), &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not retire misrouted comments from %1: %2").arg(stem, error);
            return result;
        }
    }
    return result;
}

bool isSourceAnchorDeletionRecord(const QJsonObject &record)
{
    return isDeletedRecord(record)
        && (record.value(QStringLiteral("source_anchor_deletion")).toBool()
            || (record.value(QStringLiteral("origin_id")).toString() == QLatin1String("mac-tex-anchor")
                && record.value(QStringLiteral("deletion_reason")).toString() == QLatin1String("source_anchor_removed")));
}

int indexOfStableRecord(const QList<QJsonObject> &records, const QString &id)
{
    for (int index = 0; index < records.size(); ++index) {
        if (stableRecordId(records.at(index)) == id) {
            return index;
        }
    }
    return -1;
}

QJsonObject restoredSourceAnchorRecord(const QString &docsRoot,
                                       const QJsonObject &deletedRecord,
                                       const QJsonObject &storedTombstone,
                                       const QJsonObject &resolution,
                                       const DocsAnchorOccurrence &occurrence,
                                       const SourceMirror &source)
{
    QJsonObject record = storedTombstone.value(QStringLiteral("previous_annotation")).toObject();
    if (record.isEmpty()) {
        record = deletedRecord;
    }
    record = upgradedRecord(record, QStringLiteral("mac-tex-anchor"));
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const int revision = std::max({
        1,
        record.value(QStringLiteral("revision")).toInt(1),
        deletedRecord.value(QStringLiteral("revision")).toInt(1),
        resolution.value(QStringLiteral("revision")).toInt(1),
    }) + 1;
    record.insert(QStringLiteral("revision"), revision);
    record.insert(QStringLiteral("updated_at"), timestamp);
    record.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-anchor"));
    record.insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
    record.insert(QStringLiteral("tex_file"), relativeToDocs(occurrence.path, docsRoot));
    record.insert(QStringLiteral("tex_line"), occurrence.line);
    record.insert(QStringLiteral("tex_column"), -1);
    if (source.hasMirror) {
        record.insert(QStringLiteral("contents"), source.contents);
    }

    const QString detectedKind = commentAnchorKind(occurrence, record.value(QStringLiteral("anchor_id")).toString());
    QString anchorKind = record.value(QStringLiteral("anchor_kind")).toString();
    if (detectedKind == QLatin1String("wrapped_highlight")) {
        anchorKind = detectedKind;
    } else if (anchorKind.isEmpty() || anchorKind == QLatin1String("document_inbox")) {
        anchorKind = QStringLiteral("standalone");
    }
    record.insert(QStringLiteral("anchor_kind"), anchorKind);
    record.insert(
        QStringLiteral("anchor_confidence"),
        anchorKind == QLatin1String("standalone_selected_text") || anchorKind == QLatin1String("structural") ? QStringLiteral("medium") : QStringLiteral("high"));

    for (const QString &field : {
             QStringLiteral("comment_key"),
             QStringLiteral("deleted"),
             QStringLiteral("deleted_at"),
             QStringLiteral("deletion_reason"),
             QStringLiteral("source_anchor_deletion"),
             QStringLiteral("orphan_reason"),
             QStringLiteral("orphaned_at"),
         }) {
        record.remove(field);
    }
    return record;
}

SourceAnchorRestorationBatchResult restoreReappearedSourceAnchors(const DocsCommentSynchronizer::Options &options,
                                                                  const QString &onlyStem = QString(),
                                                                  DocsAnchorIndex *sharedAnchorIndex = nullptr)
{
    SourceAnchorRestorationBatchResult result;
    DocsAnchorIndex localAnchorIndex;
    DocsAnchorIndex &anchorIndex = sharedAnchorIndex ? *sharedAnchorIndex : localAnchorIndex;

    const QString commentsRoot = QDir(options.docsRoot).filePath(QStringLiteral("comments"));
    const QDir commentsDir(commentsRoot);
    QStringList stems;
    if (!onlyStem.isEmpty()) {
        stems.append(onlyStem);
    } else {
        for (const QFileInfo &sidecar : legacyDocumentSidecars(commentsDir)) {
            QString stem = sidecar.fileName();
            stem.chop(QStringLiteral("_comments.jsonl").size());
            stems.append(stem);
        }
    }

    for (const QString &stem : std::as_const(stems)) {
        const QString canonicalPath = commentsDir.filePath(stem + QStringLiteral("_comments.jsonl"));
        if (!QFileInfo(canonicalPath).isFile()) {
            continue;
        }
        const QString statePath = commentsDir.filePath(QStringLiteral(".state/") + stem + QStringLiteral(".json"));
        const QString resolutionPath = commentsDir.filePath(QStringLiteral("review_resolutions.jsonl"));
        const QString lockPath = commentSyncLockPath(options.docsRoot, stem);
        if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
            result.success = false;
            result.failureReason = QStringLiteral("could not create the synchronization lock directory");
            return result;
        }
        QLockFile lock(lockPath);
        lock.setStaleLockTime(300000);
        if (!lock.tryLock(options.lockTimeoutMs)) {
            result.success = false;
            result.failureReason = QStringLiteral("another synchronization is already updating %1").arg(stem);
            return result;
        }

        QString error;
        QList<QJsonObject> records;
        QList<QJsonObject> resolutions;
        QJsonObject rawState;
        if (!reconcileJsonl(canonicalPath, &records, &error) || !reconcileJsonl(resolutionPath, &resolutions, &error) || !readJsonObject(statePath, &rawState, &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not read comment state for source restoration in %1: %2").arg(stem, error);
            return result;
        }
        QJsonObject state = loadedState(rawState);
        QJsonObject storedTombstones = state.value(QStringLiteral("tombstones")).toObject();
        QJsonObject mirrorStates = state.value(QStringLiteral("source_mirrors")).toObject();
        bool canonicalDirty = false;
        bool resolutionDirty = false;
        bool stateDirty = false;

        bool hasRestorationCandidate = false;
        for (const QJsonObject &record : std::as_const(records)) {
            if (!isTrackedCommentRecordImpl(record)) {
                continue;
            }
            const int resolutionIndex = indexOfStableRecord(resolutions, stableRecordId(record));
            const QJsonObject resolution = resolutionIndex >= 0 ? resolutions.at(resolutionIndex) : QJsonObject {};
            if (isSourceAnchorDeletionRecord(record) || (!isDeletedRecord(record) && isSourceAnchorDeletionRecord(resolution))) {
                hasRestorationCandidate = true;
                break;
            }
        }
        if (!hasRestorationCandidate) {
            continue;
        }
        if (sharedAnchorIndex) {
            if (!anchorIndex.complete) {
                bool allFilesRead = false;
                anchorIndex.occurrences = anchorOccurrences(managedSourceFiles(options.docsRoot), &allFilesRead);
                if (!allFilesRead) {
                    result.success = false;
                    result.failureReason = QStringLiteral("could not verify source anchors across all managed sources");
                    return result;
                }
                anchorIndex.complete = true;
            }
        } else {
            const QSet<QString> sourceFiles = sourceFilesForAnchors(options.docsRoot, stem, records);
            anchorIndex.occurrences = anchorOccurrences(sourceFiles);
            anchorIndex.complete = false;
        }

        for (int recordIndex = 0; recordIndex < records.size(); ++recordIndex) {
            const QJsonObject deletedRecord = records.at(recordIndex);
            const QString id = stableRecordId(deletedRecord);
            const int resolutionIndex = indexOfStableRecord(resolutions, id);
            const QJsonObject resolution = resolutionIndex >= 0 ? resolutions.at(resolutionIndex) : QJsonObject {};
            const bool sourceDeletedSidecar = isSourceAnchorDeletionRecord(deletedRecord);
            const bool liveSuppressedBySourceResolution = !isDeletedRecord(deletedRecord) && isSourceAnchorDeletionRecord(resolution);
            if (!isTrackedCommentRecordImpl(deletedRecord) || (!sourceDeletedSidecar && !liveSuppressedBySourceResolution)) {
                continue;
            }

            const QString anchorId = deletedRecord.value(QStringLiteral("anchor_id")).toString().trimmed();
            const QList<DocsAnchorOccurrence> found = anchorIndex.occurrences.value(anchorId);
            if (anchorId.isEmpty() || found.size() != 1) {
                continue;
            }
            const QString declaredOwner = canonicalStemForRecordSourcePdf(options.docsRoot, deletedRecord);
            if (!declaredOwner.isEmpty() && declaredOwner != stem) {
                continue;
            }
            const QString key = commentKey(deletedRecord);
            const QJsonObject storedTombstone = storedTombstones.value(key).toObject();
            const SourceMirror source = sourceMirrorAt(found.constFirst(), anchorId, &error);
            if (!source.valid) {
                result.success = false;
                result.failureReason = QStringLiteral("could not read reappeared source anchor %1: %2").arg(anchorId, error);
                return result;
            }
            QJsonObject restored = restoredSourceAnchorRecord(options.docsRoot, deletedRecord, storedTombstone, resolution, found.constFirst(), source);
            records[recordIndex] = restored;
            canonicalDirty = true;

            QJsonObject cancellation = restored;
            cancellation.insert(QStringLiteral("comment_key"), key);
            cancellation.remove(QStringLiteral("deleted"));
            cancellation.remove(QStringLiteral("deleted_at"));
            cancellation.remove(QStringLiteral("deletion_reason"));
            cancellation.remove(QStringLiteral("source_anchor_deletion"));
            if (resolutionIndex >= 0) {
                resolutions[resolutionIndex] = cancellation;
            } else {
                resolutions.append(cancellation);
            }
            resolutionDirty = true;

            storedTombstones.remove(key);
            if (source.hasMirror) {
                mirrorStates.insert(key, sourceMirrorState(key, restored, source));
            } else {
                mirrorStates.remove(key);
            }
            stateDirty = true;
            ++result.restored;
            result.logLines.append(QStringLiteral("restored reappeared source anchor %1 in %2:%3").arg(anchorId, relativeToDocs(found.constFirst().path, options.docsRoot)).arg(found.constFirst().line));
        }

        if (!canonicalDirty) {
            continue;
        }
        state.insert(QStringLiteral("tombstones"), storedTombstones);
        state.insert(QStringLiteral("source_mirrors"), mirrorStates);
        // Publish the higher-revision cancellation first. If a later write
        // fails, the still-deleted sidecar remains suppressive and the next
        // reconciliation can safely repeat the restoration.
        if ((resolutionDirty && !writeJsonl(resolutionPath, resolutions, &error))
            || !writeJsonl(canonicalPath, reindexComments(records), &error)
            || (stateDirty && !writeJsonObject(statePath, state, &error))) {
            result.success = false;
            result.failureReason = QStringLiteral("could not restore source anchors for %1: %2").arg(stem, error);
            return result;
        }
    }
    return result;
}

SourceMoveBatchResult relocateMovedSourceComments(const DocsCommentSynchronizer::Options &options, DocsAnchorIndex *anchorIndex)
{
    SourceMoveBatchResult result;
    const QString commentsPath = QDir(options.docsRoot).filePath(QStringLiteral("comments"));
    const QDir commentsDir(commentsPath);
    QHash<QString, QList<QJsonObject>> recordsByStem;
    QStringList stems;
    QString error;
    for (const QFileInfo &sidecar : legacyDocumentSidecars(commentsDir)) {
        QString stem = sidecar.fileName();
        stem.chop(QStringLiteral("_comments.jsonl").size());
        QList<QJsonObject> records;
        if (!reconcileJsonl(sidecar.absoluteFilePath(), &records, &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not read %1: %2").arg(sidecar.fileName(), error);
            return result;
        }
        stems.append(stem);
        recordsByStem.insert(stem, records);
    }

    const QSet<QString> currentSources = managedSourceFiles(options.docsRoot);
    const Thoughts::SourceFolderRenames folderRenames = Thoughts::SourceFolderNames::knownRenames(options.docsRoot);
    anchorIndex->occurrences = anchorOccurrences(currentSources);
    anchorIndex->complete = true;

    QList<PendingSourceMove> moves;
    QSet<QString> claimedDestinations;
    for (const QString &oldStem : std::as_const(stems)) {
        const QList<QJsonObject> records = recordsByStem.value(oldStem);
        for (const QJsonObject &record : records) {
            if (isDeletedRecord(record) || !isTrackedCommentRecordImpl(record)) {
                continue;
            }
            const SourceMoveDestination destination = sourceMoveDestination(options.docsRoot, record, anchorIndex->occurrences, currentSources);
            if (!destination.valid) {
                continue;
            }
            const QString newStem = canonicalStemForManagedSource(destination.sourceId, folderRenames);
            if (newStem.isEmpty()) {
                continue;
            }
            const QString key = commentKey(record);
            const QString claim = newStem + QChar(0x1f) + key;
            if (claimedDestinations.contains(claim)) {
                result.logLines.append(QStringLiteral("source move skipped for %1: destination identity is ambiguous").arg(key));
                continue;
            }

            if (!recordsByStem.contains(newStem)) {
                QList<QJsonObject> destinationRecords;
                const QString destinationPath = commentsDir.filePath(newStem + QStringLiteral("_comments.jsonl"));
                if (!reconcileJsonl(destinationPath, &destinationRecords, &error)) {
                    result.success = false;
                    result.failureReason = QStringLiteral("could not read %1: %2").arg(QFileInfo(destinationPath).fileName(), error);
                    return result;
                }
                recordsByStem.insert(newStem, destinationRecords);
            }
            if (newStem != oldStem) {
                const int destinationIndex = indexOfRecord(recordsByStem.value(newStem), key);
                if (destinationIndex >= 0) {
                    const QJsonObject existing = recordsByStem.value(newStem).at(destinationIndex);
                    const bool samePartialMove = !isDeletedRecord(existing) && existing.value(QStringLiteral("moved_from_pdf_stem")).toString() == oldStem &&
                        existing.value(QStringLiteral("moved_from_tex_file")).toString() == record.value(QStringLiteral("tex_file")).toString();
                    if (!samePartialMove) {
                        result.logLines.append(QStringLiteral("source move skipped for %1: destination sidecar already contains that identity").arg(key));
                        continue;
                    }
                }
            }
            claimedDestinations.insert(claim);
            moves.append({
                oldStem,
                newStem,
                key,
                record.value(QStringLiteral("tex_file")).toString(),
                destination,
                record,
            });
        }
    }
    if (moves.isEmpty()) {
        return result;
    }

    QSet<QString> lockStems;
    for (const PendingSourceMove &move : std::as_const(moves)) {
        lockStems.insert(move.oldStem);
        lockStems.insert(move.newStem);
    }
    QStringList sortedLockStems(lockStems.begin(), lockStems.end());
    std::sort(sortedLockStems.begin(), sortedLockStems.end());
    std::vector<std::unique_ptr<QLockFile>> locks;
    for (const QString &stem : std::as_const(sortedLockStems)) {
        const QString lockPath = commentSyncLockPath(options.docsRoot, stem);
        if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
            result.success = false;
            result.failureReason = QStringLiteral("could not create the synchronization lock directory");
            return result;
        }
        auto lock = std::make_unique<QLockFile>(lockPath);
        lock->setStaleLockTime(300000);
        if (!lock->tryLock(options.lockTimeoutMs)) {
            result.success = false;
            result.failureReason = QStringLiteral("another synchronization is already updating %1").arg(stem);
            return result;
        }
        locks.push_back(std::move(lock));
    }

    std::sort(moves.begin(), moves.end(), [](const PendingSourceMove &left, const PendingSourceMove &right) {
        if (left.destination.sourcePath != right.destination.sourcePath) {
            return left.destination.sourcePath < right.destination.sourcePath;
        }
        if (left.destination.insertionLine != right.destination.insertionLine) {
            return left.destination.insertionLine > right.destination.insertionLine;
        }
        return left.key < right.key;
    });

    QSet<QString> changedSources;
    for (PendingSourceMove &move : moves) {
        if (move.destination.markerAlreadyPresent) {
            continue;
        }
        QStringList destinationLines;
        if (!readTextLines(move.destination.sourcePath, &destinationLines, &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not read moved source %1: %2").arg(move.destination.sourceId, error);
            return result;
        }
        int insertionLine = move.destination.insertionLine;
        const QString reviewKey = move.record.value(QStringLiteral("review_key")).toString(commentKey(move.record));
        const QString anchorId = safeAnchorId(move.record, reviewKey);
        move.record.insert(QStringLiteral("review_key"), reviewKey);
        move.record.insert(QStringLiteral("anchor_id"), anchorId);
        if (!insertOrMoveAnchor(QString(), move.destination.sourcePath, &insertionLine, anchorId, move.record.value(QStringLiteral("contents")).toString(), &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not anchor moved comment in %1: %2").arg(move.destination.sourceId, error);
            return result;
        }
        changedSources.insert(move.destination.sourcePath);
    }

    for (const QString &sourcePath : std::as_const(changedSources)) {
        refreshAnchorOccurrencesForSource(&anchorIndex->occurrences, sourcePath);
    }

    QSet<QString> destinationStems;
    QSet<QString> oldStems;
    for (const PendingSourceMove &move : std::as_const(moves)) {
        const QString anchorId = move.record.value(QStringLiteral("anchor_id")).toString().trimmed();
        const QList<DocsAnchorOccurrence> found = anchorIndex->occurrences.value(anchorId);
        if (found.size() != 1) {
            result.success = false;
            result.failureReason = QStringLiteral("moved anchor %1 is not unique after insertion").arg(anchorId);
            return result;
        }
        QStringList destinationLines;
        if (!readTextLines(move.destination.sourcePath, &destinationLines, &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not verify moved source %1: %2").arg(move.destination.sourceId, error);
            return result;
        }
        const QJsonObject live = movedLiveRecord(options.docsRoot, move, found.constFirst().line, destinationLines);
        if (move.newStem == move.oldStem) {
            const int index = indexOfRecord(recordsByStem.value(move.oldStem), move.key);
            if (index >= 0) {
                recordsByStem[move.oldStem][index] = live;
                destinationStems.insert(move.oldStem);
            }
        } else {
            int destinationIndex = indexOfRecord(recordsByStem.value(move.newStem), move.key);
            if (destinationIndex >= 0) {
                recordsByStem[move.newStem][destinationIndex] = live;
            } else {
                recordsByStem[move.newStem].append(live);
            }
            const int oldIndex = indexOfRecord(recordsByStem.value(move.oldStem), move.key);
            if (oldIndex >= 0) {
                recordsByStem[move.oldStem][oldIndex] = movedSourceTombstone(move);
            }
            destinationStems.insert(move.newStem);
            oldStems.insert(move.oldStem);
        }
        ++result.moved;
        result.logLines.append(QStringLiteral("moved %1: %2/%3 -> %4/%5").arg(move.key, move.oldStem, move.oldSourceId, move.newStem, move.destination.sourceId));
    }

    // Destination records are durable before old records become invisible.
    QStringList destinationOrder(destinationStems.begin(), destinationStems.end());
    QStringList oldOrder(oldStems.begin(), oldStems.end());
    std::sort(destinationOrder.begin(), destinationOrder.end());
    std::sort(oldOrder.begin(), oldOrder.end());
    for (const QString &stem : std::as_const(destinationOrder)) {
        if (!writeJsonl(commentsDir.filePath(stem + QStringLiteral("_comments.jsonl")), reindexComments(recordsByStem.value(stem)), &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not write moved comments to %1: %2").arg(stem, error);
            return result;
        }
    }
    for (const QString &stem : std::as_const(oldOrder)) {
        if (destinationStems.contains(stem)) {
            continue;
        }
        if (!writeJsonl(commentsDir.filePath(stem + QStringLiteral("_comments.jsonl")), reindexComments(recordsByStem.value(stem)), &error)) {
            result.success = false;
            result.failureReason = QStringLiteral("could not retire moved comments from %1: %2").arg(stem, error);
            return result;
        }
    }
    return result;
}

QJsonObject attemptCache(const QString &docsRoot, const QString &pdfPath, QString *path)
{
    const QString digest = sha256(QFileInfo(pdfPath).absoluteFilePath().toUtf8());
    *path = QDir(docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/attempts/%1.json").arg(digest));
    QJsonObject loaded;
    QString ignored;
    if (!readJsonObject(*path, &loaded, &ignored) || loaded.value(QStringLiteral("version")).toInt() != AnchorAttemptCacheVersion) {
        loaded = {
            {QStringLiteral("version"), AnchorAttemptCacheVersion},
            {QStringLiteral("source_pdf"), QFileInfo(pdfPath).absoluteFilePath()},
            {QStringLiteral("attempts"), QJsonObject()},
        };
    }
    return loaded;
}

QString currentAnchorSourceSha256(const QString &docsRoot, const QJsonObject &comment, const SourceLocation &source)
{
    const QString recordedSourceId = comment.value(QStringLiteral("tex_file")).toString().trimmed();
    QString sourcePath;
    if (!recordedSourceId.isEmpty() && !managedProjectRoot(docsRoot, recordedSourceId).isEmpty()) {
        sourcePath = QDir(docsRoot).filePath(recordedSourceId);
    } else if (!source.sourcePath.isEmpty()) {
        sourcePath = source.sourcePath;
    }
    return sourcePath.isEmpty() ? QString() : fileSha256(sourcePath);
}

QString anchorSignature(const QString &docsRoot, const QJsonObject &comment, const SourceLocation &source, const BuildInfo &build)
{
    const QJsonObject payload {
        {QStringLiteral("version"), AnchorAttemptCacheVersion},
        {QStringLiteral("comment_key"), commentKey(comment)},
        {QStringLiteral("subtype"), comment.value(QStringLiteral("subtype"))},
        {QStringLiteral("page"), comment.value(QStringLiteral("page"))},
        {QStringLiteral("rect"), normalizedJson(comment.value(QStringLiteral("rect")))},
        {QStringLiteral("quad_points"), normalizedJson(comment.value(QStringLiteral("quad_points")))},
        {QStringLiteral("selected_text"), comment.value(QStringLiteral("selected_text"))},
        {QStringLiteral("annotation_pdf_sha256"), comment.value(QStringLiteral("annotation_pdf_sha256"))},
        {QStringLiteral("source_id"), source.sourceId},
        {QStringLiteral("source_fingerprint"), source.fingerprint},
        {QStringLiteral("current_source_sha256"), currentAnchorSourceSha256(docsRoot, comment, source)},
        {QStringLiteral("local_page"), source.localPage},
        {QStringLiteral("build_id"), build.buildId},
        {QStringLiteral("synctex_fingerprint"), build.synctexFingerprint},
    };
    return sha256(compactJson(payload));
}

QString resolveSynctexExecutable(const DocsCommentSynchronizer::Options &options)
{
    if (!options.synctexExecutable.isEmpty()) {
        return options.synctexExecutable;
    }
    const QStringList searchPaths = options.processEnvironment.value(QStringLiteral("PATH")).split(QDir::listSeparator(), Qt::SkipEmptyParts);
    return QStandardPaths::findExecutable(QStringLiteral("synctex"), searchPaths);
}

bool synchronizeAnchor(const DocsCommentSynchronizer::Options &options,
                       const DocsAnchorOccurrences &occurrences,
                       const std::function<SourceLocation(int)> &sourceForPage,
                       const std::function<BuildInfo(const SourceLocation &)> &buildForSource,
                       const std::function<HistoricalBuildResolution(const QJsonObject &)> &historicalBuild,
                       const std::function<DocsCommentSynchronizer::SourceResolutionResult(const QJsonObject &)> &publishedLookup,
                       QJsonObject *comment,
                       const std::optional<QJsonObject> &previous,
                       QJsonObject *attempts,
                       bool prepareUnanchored,
                       QString *failure,
                       bool *lookupRequired,
                       bool *attempted,
                       bool *inserted,
                       bool *mirrorUpdated)
{
    *lookupRequired = false;
    *attempted = false;
    *inserted = false;
    *mirrorUpdated = false;
    const QString reviewKey = comment->value(QStringLiteral("review_key")).toString(commentKey(*comment));
    const QString anchorId = safeAnchorId(*comment, reviewKey);
    comment->insert(QStringLiteral("review_key"), reviewKey);
    comment->insert(QStringLiteral("anchor_id"), anchorId);
    const QList<DocsAnchorOccurrence> found = occurrences.value(anchorId);

    const bool geometryChanged = previous && !anchorGeometryEqual(*previous, *comment);
    const QString previousAnchorStatus = previous ? previous->value(QStringLiteral("anchor_status")).toString() : QString();
    const bool previousNeedsAnchor = previous && (previousAnchorStatus.isEmpty() || previousAnchorStatus == QLatin1String("needs_anchor"));
    const bool legacyCachedAnchorWithoutMarker = previous
        && previousAnchorStatus == QLatin1String("anchored")
        && previous->value(QStringLiteral("tex_file")).toString().trimmed().isEmpty()
        && !previous->value(QStringLiteral("source_build_id")).toString().trimmed().isEmpty();
    const bool previousHadAnchorLocation = previous && !previous->value(QStringLiteral("tex_file")).toString().isEmpty() && previous->value(QStringLiteral("tex_line")).toInt() > 0;
    QString intendedMarkupSubtype = comment->value(QStringLiteral("markup_subtype")).toString();
    if (intendedMarkupSubtype.isEmpty() && previous) {
        intendedMarkupSubtype = previous->value(QStringLiteral("markup_subtype")).toString();
    }
    const bool legacyDowngradedMarkup = isTextMarkupSubtype(intendedMarkupSubtype)
        && (comment->value(QStringLiteral("subtype")).toString() == QLatin1String("Text")
            || comment->value(QStringLiteral("markup_status")).toString() == QLatin1String("structural_fallback")
            || (previous && previous->value(QStringLiteral("markup_status")).toString() == QLatin1String("structural_fallback")));
    if (legacyDowngradedMarkup) {
        comment->insert(QStringLiteral("subtype"), intendedMarkupSubtype);
        comment->insert(QStringLiteral("markup_subtype"), intendedMarkupSubtype);
        comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
        comment->insert(QStringLiteral("quad_points"), QJsonValue::Null);
        comment->remove(QStringLiteral("fragments"));
    }
    // Version 5 adds a safe authored-parent fallback for SyncTeX results in
    // generated TeX. Retry those existing records once on an ordinary save;
    // the attempt cache suppresses subsequent unchanged failures.
    const bool previousGeneratedSource = previousNeedsAnchor && hasGeneratedPathComponent(previous->value(QStringLiteral("tex_file")).toString());
    const bool retryUnchangedAnchor = found.isEmpty()
        && (previousNeedsAnchor || legacyCachedAnchorWithoutMarker)
        && (prepareUnanchored || previousGeneratedSource);
    // A unique marker is an explicit manual recovery for a comment that was not
    // anchored previously.  Its old observation may lack geometry, so requiring
    // geometry equality here would make that recovery impossible.
    if (found.size() == 1 && (!geometryChanged || previousNeedsAnchor)) {
        comment->insert(QStringLiteral("linking_method"), QStringLiteral("existing_source_anchor"));
        clearSidecarOrphanReason(comment);
        if (!rewriteAnchorMirror(found.constFirst(), anchorId, comment->value(QStringLiteral("contents")).toString(), mirrorUpdated, failure)) {
            return false;
        }
        QString anchorKind = commentAnchorKind(found.constFirst(), anchorId);
        const QString previousAnchorKind = previous ? previous->value(QStringLiteral("anchor_kind")).toString() : QString();
        if (previousAnchorKind == QLatin1String("structural") || previousAnchorKind == QLatin1String("document_inbox")) {
            anchorKind = previousAnchorKind;
        }
        const QString subtype = comment->value(QStringLiteral("subtype")).toString();
        const QString selectedText = comment->value(QStringLiteral("selected_text")).toString();
        if (legacyDowngradedMarkup) {
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            comment->insert(QStringLiteral("anchor_kind"), anchorKind);
            comment->insert(QStringLiteral("tex_file"), relativeToDocs(found.constFirst().path, options.docsRoot));
            comment->insert(QStringLiteral("tex_line"), found.constFirst().line);
            return true;
        }
        bool selectedTextBoundary = previous && previous->value(QStringLiteral("anchor_kind")).toString() == QLatin1String("standalone_selected_text");
        if (!selectedTextBoundary && anchorKind == QLatin1String("standalone") && subtype != QLatin1String("Text") && !selectedText.trimmed().isEmpty()) {
            QStringList sourceLines;
            if (!readTextLines(found.constFirst().path, &sourceLines, failure)) {
                return false;
            }
            selectedTextBoundary = isUnsafeHighlightBoundaryAnchor(sourceLines, found.constFirst().line, selectedText);
        }
        if (anchorKind == QLatin1String("standalone") && subtype != QLatin1String("Text") && !selectedText.trimmed().isEmpty() && !selectedTextBoundary) {
            const DocsAnchorOccurrence occurrence = found.constFirst();
            if (!removeAnchor(occurrence, anchorId, failure)) {
                return false;
            }
            int wrappedLine = occurrence.line;
            if (wrapHighlightedSource(occurrence.path, occurrence.line, anchorId, comment->value(QStringLiteral("contents")).toString(), selectedText, subtype, &wrappedLine, failure)) {
                anchorKind = QStringLiteral("wrapped_highlight");
                *inserted = true;
            } else {
                if (failure) {
                    failure->clear();
                }
                QStringList sourceLines;
                if (!readTextLines(occurrence.path, &sourceLines, failure)) {
                    return false;
                }
                int boundaryLine = unsafeHighlightBoundaryInsertionLine(sourceLines, occurrence.line, selectedText);
                if (boundaryLine > 0 && insertOrMoveAnchor(QString(), occurrence.path, &boundaryLine, anchorId, comment->value(QStringLiteral("contents")).toString(), failure)) {
                    attempts->remove(commentKey(*comment));
                    comment->insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
                    comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
                    comment->insert(QStringLiteral("anchor_kind"), QStringLiteral("standalone_selected_text"));
                    comment->insert(QStringLiteral("tex_file"), relativeToDocs(occurrence.path, options.docsRoot));
                    comment->insert(QStringLiteral("tex_line"), boundaryLine);
                    comment->remove(QStringLiteral("anchor_error_code"));
                    comment->remove(QStringLiteral("markup_status"));
                    *inserted = true;
                    return true;
                }
                if (failure) {
                    failure->clear();
                }
                comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
                comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
                comment->insert(QStringLiteral("markup_subtype"), subtype);
                comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
                comment->remove(QStringLiteral("anchor_kind"));
                *inserted = true;
                return true;
            }
        } else if (selectedTextBoundary) {
            anchorKind = QStringLiteral("standalone_selected_text");
        }
        // A failed move deliberately keeps the old marker in place while the
        // JSONL record says that the new geometry still needs attention. Do
        // not silently promote that record back to anchored on the next
        // text-only save.
        if (previousNeedsAnchor && previousHadAnchorLocation) {
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), previous->value(QStringLiteral("anchor_confidence")).toString(QStringLiteral("low")));
            return true;
        }
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
        comment->insert(QStringLiteral("anchor_kind"), anchorKind);
        if (previous && !previous->value(QStringLiteral("anchor_scope")).toString().isEmpty()) {
            comment->insert(QStringLiteral("anchor_scope"), previous->value(QStringLiteral("anchor_scope")));
        }
        if (comment->value(QStringLiteral("markup_status")).toString() == QLatin1String("needs_attention") && anchorKind != QLatin1String("wrapped_highlight")) {
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            return true;
        }
        comment->remove(QStringLiteral("anchor_error_code"));
        comment->remove(QStringLiteral("markup_status"));
        const bool generatedParentAnchor = previous && previous->value(QStringLiteral("anchor_confidence")).toString() == QLatin1String("medium") && previous->value(QStringLiteral("tex_column")).toInt() == -1;
        comment->insert(QStringLiteral("anchor_confidence"),
                        generatedParentAnchor || selectedTextBoundary || anchorKind == QLatin1String("structural") ? QStringLiteral("medium")
                                                                                                                   : (anchorKind == QLatin1String("document_inbox") ? QStringLiteral("low") : QStringLiteral("high")));
        comment->insert(QStringLiteral("tex_file"), relativeToDocs(found.constFirst().path, options.docsRoot));
        comment->insert(QStringLiteral("tex_line"), found.constFirst().line);
        if (generatedParentAnchor) {
            comment->insert(QStringLiteral("tex_column"), -1);
        }
        return true;
    }
    if (found.size() > 1) {
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("orphaned"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("low"));
        return true;
    }
    if (previous && !geometryChanged && !retryUnchangedAnchor) {
        const QString previousStatus = previous->value(QStringLiteral("anchor_status")).toString(QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_status"), previousStatus == QLatin1String("anchored") ? QStringLiteral("orphaned") : previousStatus);
        comment->insert(QStringLiteral("anchor_confidence"), previous->value(QStringLiteral("anchor_confidence")).toString(QStringLiteral("low")));
        return true;
    }

    *lookupRequired = true;
    // Published maps are the sole authority for source-linked PDFs. The map
    // connects the combined PDF to its component; their hashes are not equal.
    if (!options.sourceBundleRoot.isEmpty() || !options.allowLegacySourceResolution) {
        *attempted = true;
        comment->insert(QStringLiteral("linking_method"), QStringLiteral("saved_pdf_source_map"));
        const auto unresolved = [&](const QString &reason) {
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("low"));
            comment->insert(QStringLiteral("anchor_error_code"), QStringLiteral("source_map_unresolved"));
            comment->insert(QStringLiteral("anchor_error_detail"), reason);
            comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
            attempts->insert(commentKey(*comment), QJsonObject {{QStringLiteral("reason"), reason},
                {QStringLiteral("method"), QStringLiteral("saved_pdf_source_map")}});
            return true;
        };
        const auto resolved = publishedLookup(*comment);
        if (!resolved.success) return unresolved(resolved.failureReason);
        comment->insert(QStringLiteral("source_build_id"), resolved.buildId);
        if (!resolved.sourcePath.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) {
            return unresolved(QStringLiteral("This source format does not support automatic comment anchoring yet"));
        }
        const QString subtype = comment->value(QStringLiteral("subtype")).toString();
        const QString selectedText = comment->value(QStringLiteral("selected_text")).toString();
        int line = resolved.line;
        QStringList lines;
        if (!readTextLines(resolved.sourcePath, &lines, failure)) return false;
        if (!isTextMarkupSubtype(subtype) && !safeInsertionLine(lines, line)) {
            return unresolved(QStringLiteral("The mapped source position cannot safely hold a comment anchor"));
        }
        const bool markup = isTextMarkupSubtype(subtype);
        if (markup && !found.isEmpty()) {
            return unresolved(QStringLiteral("The existing highlight must be reviewed before moving its source anchor"));
        }
        if (markup && !wrapHighlightedSource(resolved.sourcePath, line, anchorId,
                comment->value(QStringLiteral("contents")).toString(), selectedText, subtype, nullptr, failure, true)) {
            if (failure) failure->clear();
            return unresolved(QStringLiteral("The mapped selection cannot be anchored without changing its meaning"));
        }
        int definitionLine = 0, addedLines = 0;
        if (!ensureCommentAnchorDefinition(resolved.sourcePath, &definitionLine, &addedLines, failure)) return false;
        if (addedLines > 0 && line >= definitionLine) line += addedLines;
        if (markup) {
            if (!wrapHighlightedSource(resolved.sourcePath, line, anchorId,
                    comment->value(QStringLiteral("contents")).toString(), selectedText, subtype, &line, failure)) {
                if (failure) failure->clear();
                return unresolved(QStringLiteral("The mapped selection cannot be anchored without changing its meaning"));
            }
        } else if (!insertOrMoveAnchor(found.size() == 1 ? found.constFirst().path : QString(),
                       resolved.sourcePath, &line, anchorId, comment->value(QStringLiteral("contents")).toString(), failure)) {
            return false;
        }
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("high"));
        comment->insert(QStringLiteral("anchor_kind"), markup ? QStringLiteral("wrapped_highlight") : QStringLiteral("standalone"));
        comment->insert(QStringLiteral("tex_file"), resolved.sourceId);
        comment->insert(QStringLiteral("tex_line"), line);
        comment->insert(QStringLiteral("tex_column"), resolved.column);
        comment->insert(QStringLiteral("source_fingerprint"), fileSha256(resolved.sourcePath));
        comment->remove(QStringLiteral("anchor_error_code"));
        comment->remove(QStringLiteral("anchor_error_detail"));
        comment->remove(QStringLiteral("markup_status"));
        if (readTextLines(resolved.sourcePath, &lines, nullptr)) comment->insert(QStringLiteral("source_context"), sourceContext(lines, line));
        attempts->remove(commentKey(*comment));
        *inserted = true;
        return true;
    }
    const auto recoverByUniqueSelectedText = [&](const QString &fallbackReason) -> std::optional<bool> {
        const QString subtype = comment->value(QStringLiteral("subtype")).toString();
        const QString selectedText = comment->value(QStringLiteral("selected_text")).toString();
        if (!prepareUnanchored || !isTextMarkupSubtype(subtype) || selectedText.trimmed().isEmpty()) {
            return std::nullopt;
        }
        *attempted = true;
        const UniqueSelectedTextResolution resolution = uniqueSelectedTextSource(options.docsRoot, selectedText);
        if (!resolution.valid) {
            const QString reason = resolution.reason.isEmpty() ? fallbackReason : resolution.reason;
            attempts->insert(commentKey(*comment),
                             QJsonObject {
                                 {QStringLiteral("signature"), anchorSignature(options.docsRoot, *comment, {}, {})},
                                 {QStringLiteral("reason"), reason},
                                 {QStringLiteral("fallback_reason"), fallbackReason},
                                 {QStringLiteral("confidence"), QStringLiteral("medium")},
                             });
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            comment->insert(QStringLiteral("anchor_error_code"), reason);
            comment->insert(QStringLiteral("markup_subtype"), subtype);
            comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
            return true;
        }

        int line = resolution.line;
        int definitionLine = 0;
        int definitionLinesAdded = 0;
        if (!ensureCommentAnchorDefinition(resolution.sourcePath, &definitionLine, &definitionLinesAdded, failure)) {
            return false;
        }
        if (definitionLinesAdded > 0 && line >= definitionLine) {
            line += definitionLinesAdded;
        }
        if (!wrapHighlightedSource(resolution.sourcePath,
                                   line,
                                   anchorId,
                                   comment->value(QStringLiteral("contents")).toString(),
                                   selectedText,
                                   comment->value(QStringLiteral("subtype")).toString(),
                                   &line,
                                   failure)) {
            if (failure) {
                failure->clear();
            }
            attempts->insert(commentKey(*comment),
                             QJsonObject {
                                 {QStringLiteral("signature"), anchorSignature(options.docsRoot, *comment, {}, {})},
                                 {QStringLiteral("reason"), QStringLiteral("selected_text_not_marker_safe")},
                                 {QStringLiteral("fallback_reason"), fallbackReason},
                                 {QStringLiteral("confidence"), QStringLiteral("medium")},
                             });
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            comment->insert(QStringLiteral("anchor_error_code"), QStringLiteral("selected_text_not_marker_safe"));
            comment->insert(QStringLiteral("markup_subtype"), subtype);
            comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
            return true;
        }

        attempts->remove(commentKey(*comment));
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("high"));
        comment->insert(QStringLiteral("anchor_kind"), QStringLiteral("wrapped_highlight"));
        comment->insert(QStringLiteral("tex_file"), resolution.sourceId);
        comment->insert(QStringLiteral("tex_line"), line);
        comment->insert(QStringLiteral("tex_column"), -1);
        comment->remove(QStringLiteral("source_build_id"));
        comment->insert(QStringLiteral("source_fingerprint"), fileSha256(resolution.sourcePath));
        comment->remove(QStringLiteral("anchor_error_code"));
        comment->remove(QStringLiteral("markup_status"));
        QStringList finalLines;
        if (readTextLines(resolution.sourcePath, &finalLines, nullptr)) {
            comment->insert(QStringLiteral("source_context"), sourceContext(finalLines, line));
        }
        *inserted = true;
        return true;
    };
    if (comment->value(QStringLiteral("_page_rotation")).toInt() % 4 != 0) {
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("low"));
        return true;
    }
    const HistoricalBuildResolution historical = historicalBuild(*comment);
    if (historical.requested && !historical.valid) {
        if (const std::optional<bool> recovered = recoverByUniqueSelectedText(QStringLiteral("historical_pdf_unavailable"))) {
            return *recovered;
        }
        attempts->insert(commentKey(*comment),
                         QJsonObject {
                             {QStringLiteral("signature"), anchorSignature(options.docsRoot, *comment, {}, {})},
                             {QStringLiteral("reason"), QStringLiteral("historical_pdf_unavailable")},
                             {QStringLiteral("confidence"), QStringLiteral("medium")},
        });
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
        comment->insert(QStringLiteral("anchor_error_code"), QStringLiteral("historical_pdf_unavailable"));
        return true;
    }
    const SourceLocation source = historical.valid ? historical.source : sourceForPage(comment->value(QStringLiteral("page")).toInt());
    if (!source.valid) {
        if (const std::optional<bool> recovered = recoverByUniqueSelectedText(QStringLiteral("source_not_linked"))) {
            return *recovered;
        }
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("low"));
        comment->insert(QStringLiteral("anchor_error_code"), QStringLiteral("source_not_linked"));
        return true;
    }
    const BuildInfo build = historical.valid ? historical.build : buildForSource(source);
    if (!build.valid) {
        if (const std::optional<bool> recovered = recoverByUniqueSelectedText(QStringLiteral("exact_synctex_build_unavailable"))) {
            return *recovered;
        }
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("low"));
        comment->insert(QStringLiteral("anchor_error_code"), QStringLiteral("exact_synctex_build_unavailable"));
        return true;
    }
    const QString annotationPdfSha256 = comment->value(QStringLiteral("annotation_pdf_sha256")).toString().trimmed().toLower();
    if (annotationPdfSha256.size() == 64 && fileSha256(build.cachedPdf) != annotationPdfSha256) {
        if (const std::optional<bool> recovered = recoverByUniqueSelectedText(QStringLiteral("historical_pdf_build_mismatch"))) {
            return *recovered;
        }
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
        comment->insert(QStringLiteral("anchor_error_code"), QStringLiteral("historical_pdf_build_mismatch"));
        return true;
    }

    const QString signature = anchorSignature(options.docsRoot, *comment, source, build);
    const QJsonObject cachedAttempt = attempts->value(commentKey(*comment)).toObject();
    const bool staleUnsafeSourceLinePolicy = cachedAttempt.value(QStringLiteral("reason")).toString() == QLatin1String("unsafe_source_line") && cachedAttempt.value(QStringLiteral("policy_version")).toInt() != UnsafeSourceLinePolicyVersion;
    if (retryUnchangedAnchor && cachedAttempt.value(QStringLiteral("signature")).toString() == signature && !staleUnsafeSourceLinePolicy) {
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), cachedAttempt.value(QStringLiteral("confidence")).toString(QStringLiteral("medium")));
        return true;
    }

    std::optional<SynctexRecord> matching;
    const SourceSnapshot *matchingSource = nullptr;
    // Version-4 records already contain the generated source selected by
    // reverse SyncTeX. Reuse that durable evidence for an unchanged retry
    // when the build is still exact, or when the old and current build
    // snapshots for that source are byte-identical. This avoids launching one
    // process per historical generated comment on its first v5 save.
    if (previousGeneratedSource && previous && !geometryChanged) {
        const QString previousBuildId = previous->value(QStringLiteral("source_build_id")).toString();
        const bool exactBuildEvidence = previousBuildId == build.buildId && previous->value(QStringLiteral("source_fingerprint")).toString() == source.fingerprint;
        const QString storedTexFile = previous->value(QStringLiteral("tex_file")).toString();
        const QFileInfo storedInfo(storedTexFile);
        const QString storedPath = canonicalOrAbsolutePath(storedInfo.isAbsolute() ? storedInfo.absoluteFilePath() : QDir(options.docsRoot).filePath(storedTexFile));
        for (const SourceSnapshot &snapshot : build.sourceSnapshots) {
            if ((snapshot.sourceId == storedTexFile || canonicalOrAbsolutePath(snapshot.sourcePath) == storedPath) && hasGeneratedPathComponent(snapshot.sourcePath) &&
                (exactBuildEvidence || previousGeneratedSnapshotMatches(options.docsRoot, previousBuildId, storedTexFile, snapshot))) {
                SynctexRecord stored;
                stored.fields.insert(QStringLiteral("Input"), snapshot.sourcePath);
                stored.fields.insert(QStringLiteral("Line"), QString::number(std::max(1, previous->value(QStringLiteral("tex_line")).toInt(1))));
                stored.fields.insert(QStringLiteral("Column"), QString::number(previous->value(QStringLiteral("tex_column")).toInt(-1)));
                matching = stored;
                matchingSource = &snapshot;
                break;
            }
        }
    }

    if (!matchingSource) {
        const QString executable = resolveSynctexExecutable(options);
        if (executable.isEmpty()) {
            if (failure) {
                *failure = QStringLiteral("synctex executable was not found");
            }
            return false;
        }

        const QJsonArray normalizedQuads = comment->value(QStringLiteral("_normalized_quad_points")).toArray();
        const QJsonArray normalizedRect = comment->value(QStringLiteral("_normalized_rect")).toArray();
        double normalizedX = 0.0;
        double normalizedY = 0.0;
        if (normalizedQuads.size() >= 8) {
            for (int index = 0; index < 8; index += 2) {
                normalizedX += normalizedQuads.at(index).toDouble();
                normalizedY += normalizedQuads.at(index + 1).toDouble();
            }
            normalizedX /= 4.0;
            normalizedY /= 4.0;
        } else if (normalizedRect.size() == 4) {
            normalizedX = (normalizedRect.at(0).toDouble() + normalizedRect.at(2).toDouble()) / 2.0;
            normalizedY = (normalizedRect.at(1).toDouble() + normalizedRect.at(3).toDouble()) / 2.0;
        } else {
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            return true;
        }
        const double x = normalizedX * comment->value(QStringLiteral("_page_width")).toDouble();
        const double y = normalizedY * comment->value(QStringLiteral("_page_height")).toDouble();

        *attempted = true;
        QProcess process;
        process.setWorkingDirectory(build.directory);
        process.setProcessEnvironment(options.processEnvironment);
        process.start(executable,
                      {
                          QStringLiteral("edit"),
                          QStringLiteral("-o"),
                          QStringLiteral("%1:%2:%3:%4").arg(source.localPage).arg(x, 0, 'f', 6).arg(y, 0, 'f', 6).arg(build.cachedPdf),
                          QStringLiteral("-d"),
                          build.directory,
                      });
        if (!process.waitForStarted(std::min(1000, options.synctexTimeoutMs))) {
            if (failure) {
                *failure = QStringLiteral("synctex could not start: %1").arg(process.errorString());
            }
            return false;
        }
        if (!process.waitForFinished(options.synctexTimeoutMs)) {
            process.kill();
            process.waitForFinished();
            if (failure) {
                *failure = QStringLiteral("synctex timed out");
            }
            return false;
        }
        if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
            if (failure) {
                *failure = QStringLiteral("synctex exited with code %1").arg(process.exitCode());
            }
            return false;
        }

        for (const SynctexRecord &record : parseSynctexRecords(QString::fromUtf8(process.readAllStandardOutput()))) {
            const QString candidate = normalizedCandidatePath(record.fields.value(QStringLiteral("Input")), options.docsRoot, build);
            if (const SourceSnapshot *snapshot = snapshotForCandidate(build, candidate)) {
                matching = record;
                matchingSource = snapshot;
                break;
            }
        }
    }
    if (!matching || !matchingSource) {
        attempts->insert(commentKey(*comment),
                         QJsonObject {
                             {QStringLiteral("signature"), signature},
                             {QStringLiteral("reason"), QStringLiteral("no_matching_synctex_record")},
                             {QStringLiteral("confidence"), QStringLiteral("medium")},
                         });
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
        return true;
    }

    bool oldLineOk = false;
    const int oldLine = matching->fields.value(QStringLiteral("Line")).toInt(&oldLineOk);
    const auto retainForUnavailableSource = [&](const QString &reason) {
        attempts->insert(commentKey(*comment),
                         QJsonObject {
                             {QStringLiteral("signature"), signature},
                             {QStringLiteral("reason"), reason},
                             {QStringLiteral("confidence"), QStringLiteral("medium")},
                         });
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
        comment->insert(QStringLiteral("tex_file"), matchingSource->sourceId);
        comment->insert(QStringLiteral("tex_line"), oldLineOk ? oldLine : 0);
        bool columnOk = false;
        const int column = matching->fields.value(QStringLiteral("Column")).toInt(&columnOk);
        comment->insert(QStringLiteral("tex_column"), columnOk ? column : -1);
        comment->insert(QStringLiteral("source_build_id"), build.buildId);
        comment->insert(QStringLiteral("source_fingerprint"), source.fingerprint);
        if (failure) {
            failure->clear();
        }
        return true;
    };

    if (!oldLineOk) {
        return retainForUnavailableSource(QStringLiteral("source_line_unavailable"));
    }
    QStringList snapshotLines;
    QStringList currentLines;
    if (!readTextLines(matchingSource->snapshotPath, &snapshotLines, failure)) {
        return retainForUnavailableSource(QStringLiteral("source_snapshot_unreadable"));
    }
    if (!QFileInfo(matchingSource->sourcePath).isFile()) {
        return retainForUnavailableSource(QStringLiteral("source_moved_or_missing"));
    }
    if (!readTextLines(matchingSource->sourcePath, &currentLines, failure)) {
        return retainForUnavailableSource(QStringLiteral("source_unreadable"));
    }
    int newLine = remapSnapshotLine(snapshotLines, currentLines, oldLine);
    bool generatedParentFallback = false;
    QString destinationSourceId = matchingSource->sourceId;
    QString destinationPath = matchingSource->sourcePath;
    QStringList destinationLines = currentLines;

    if (!matchingSource->markerEligible || !markerEligibleSourcePath(options.docsRoot, matchingSource->sourceId, matchingSource->sourcePath)) {
        if (hasGeneratedPathComponent(matchingSource->sourcePath)) {
            const GeneratedParentResolution parent = generatedParentResolution(options.docsRoot, build, *matchingSource);
            if (parent.valid) {
                generatedParentFallback = true;
                destinationSourceId = parent.sourceId;
                destinationPath = parent.sourcePath;
                newLine = parent.line;
                if (!readTextLines(destinationPath, &destinationLines, failure)) {
                    return false;
                }
            } else {
                attempts->insert(commentKey(*comment),
                                 QJsonObject {
                                     {QStringLiteral("signature"), signature},
                                     {QStringLiteral("reason"), parent.reason},
                                     {QStringLiteral("confidence"), QStringLiteral("medium")},
                                 });
                comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
                comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
                comment->insert(QStringLiteral("tex_file"), matchingSource->sourceId);
                comment->insert(QStringLiteral("tex_line"), newLine > 0 ? newLine : oldLine);
                comment->insert(QStringLiteral("tex_column"), matching->fields.value(QStringLiteral("Column")).toInt());
                comment->insert(QStringLiteral("source_build_id"), build.buildId);
                comment->insert(QStringLiteral("source_fingerprint"), source.fingerprint);
                return true;
            }
        } else {
            attempts->insert(commentKey(*comment),
                             QJsonObject {
                                 {QStringLiteral("signature"), signature},
                                 {QStringLiteral("reason"), QStringLiteral("source_not_marker_eligible")},
                                 {QStringLiteral("confidence"), QStringLiteral("medium")},
                             });
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            comment->insert(QStringLiteral("tex_file"), matchingSource->sourceId);
            comment->insert(QStringLiteral("tex_line"), newLine > 0 ? newLine : oldLine);
            comment->insert(QStringLiteral("tex_column"), matching->fields.value(QStringLiteral("Column")).toInt());
            comment->insert(QStringLiteral("source_build_id"), build.buildId);
            comment->insert(QStringLiteral("source_fingerprint"), source.fingerprint);
            return true;
        }
    }
    const QString subtype = comment->value(QStringLiteral("subtype")).toString();
    const QString selectedText = comment->value(QStringLiteral("selected_text")).toString();
    if (isTextMarkupSubtype(subtype) && selectedText.trimmed().isEmpty()) {
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
        comment->insert(QStringLiteral("markup_subtype"), subtype);
        comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
        return true;
    }
    const bool headingCommandTarget = reviewHeadingCommandLine(destinationLines, newLine);
    const bool headingHighlightTarget = subtype != QLatin1String("Text") && !selectedText.trimmed().isEmpty() && headingCommandTarget;
    const bool standaloneHeadingTarget = subtype == QLatin1String("Text") && headingCommandTarget;
    int insertionLine = headingHighlightTarget || standaloneHeadingTarget ? newLine : commentAnchorInsertionLine(destinationLines, newLine, generatedParentFallback);
    bool selectedTextBoundary = false;
    bool structuralFallback = false;
    QString anchorScope;
    if (insertionLine == 0 && subtype != QLatin1String("Text") && !selectedText.trimmed().isEmpty()) {
        insertionLine = unsafeHighlightBoundaryInsertionLine(destinationLines, newLine, selectedText);
        selectedTextBoundary = insertionLine > 0;
    }
    if (insertionLine == 0) {
        if (isTextMarkupSubtype(subtype)) {
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            comment->insert(QStringLiteral("markup_subtype"), subtype);
            comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
            return true;
        }
        const StructuralAnchorPlacement structural = structuralAnchorPlacement(destinationLines, newLine);
        if (structural.line > 0) {
            insertionLine = structural.line;
            anchorScope = structural.scope;
            structuralFallback = true;
        } else {
            attempts->insert(commentKey(*comment),
                             QJsonObject {
                                 {QStringLiteral("signature"), signature},
                                 {QStringLiteral("reason"), QStringLiteral("unsafe_source_line")},
                                 {QStringLiteral("confidence"), QStringLiteral("medium")},
                                 {QStringLiteral("policy_version"), UnsafeSourceLinePolicyVersion},
                             });
            comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
            comment->insert(QStringLiteral("tex_file"), matchingSource->sourceId);
            comment->insert(QStringLiteral("tex_line"), newLine > 0 ? newLine : oldLine);
            comment->insert(QStringLiteral("tex_column"), matching->fields.value(QStringLiteral("Column")).toInt());
            comment->insert(QStringLiteral("source_build_id"), build.buildId);
            comment->insert(QStringLiteral("source_fingerprint"), source.fingerprint);
            return true;
        }
    }
    newLine = insertionLine;

    int definitionLine = 0;
    int definitionLinesAdded = 0;
    if (!ensureCommentAnchorDefinition(source.sourcePath, &definitionLine, &definitionLinesAdded, failure)) {
        return false;
    }
    if (canonicalOrAbsolutePath(source.sourcePath) == canonicalOrAbsolutePath(destinationPath) && definitionLinesAdded > 0 && newLine >= definitionLine) {
        newLine += definitionLinesAdded;
    }

    const QString oldPath = found.size() == 1 ? found.constFirst().path : QString();
    bool wrappedHighlight = false;
    if (found.isEmpty() && subtype != QLatin1String("Text") && !selectedText.trimmed().isEmpty() && !selectedTextBoundary && !structuralFallback) {
        wrappedHighlight = wrapHighlightedSource(destinationPath, newLine, anchorId, comment->value(QStringLiteral("contents")).toString(), selectedText, subtype, &newLine, failure);
    }
    if (!wrappedHighlight && !structuralFallback && subtype != QLatin1String("Text") && !selectedText.trimmed().isEmpty() && !selectedTextBoundary) {
        comment->insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
        comment->insert(QStringLiteral("anchor_confidence"), QStringLiteral("medium"));
        comment->insert(QStringLiteral("markup_subtype"), subtype);
        comment->insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
        return true;
    }
    if (!wrappedHighlight && !insertOrMoveAnchor(oldPath, destinationPath, &newLine, anchorId, comment->value(QStringLiteral("contents")).toString(), failure)) {
        return false;
    }
    attempts->remove(commentKey(*comment));
    comment->insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
    comment->insert(QStringLiteral("anchor_confidence"), generatedParentFallback || selectedTextBoundary || structuralFallback ? QStringLiteral("medium") : QStringLiteral("high"));
    comment->insert(QStringLiteral("anchor_kind"),
                    structuralFallback     ? QStringLiteral("structural")
                        : wrappedHighlight ? QStringLiteral("wrapped_highlight")
                                           : (subtype == QLatin1String("Text") ? QStringLiteral("standalone") : QStringLiteral("standalone_selected_text")));
    if (structuralFallback) {
        comment->insert(QStringLiteral("anchor_scope"), anchorScope.isEmpty() ? QStringLiteral("paragraph") : anchorScope);
    } else {
        comment->remove(QStringLiteral("anchor_scope"));
    }
    comment->insert(QStringLiteral("tex_file"), destinationSourceId);
    comment->insert(QStringLiteral("tex_line"), newLine);
    comment->insert(QStringLiteral("tex_column"), generatedParentFallback ? -1 : matching->fields.value(QStringLiteral("Column")).toInt());
    comment->insert(QStringLiteral("source_build_id"), build.buildId);
    comment->insert(QStringLiteral("source_fingerprint"), source.fingerprint);
    comment->remove(QStringLiteral("anchor_error_code"));
    comment->remove(QStringLiteral("markup_status"));
    QStringList finalLines;
    if (readTextLines(destinationPath, &finalLines, nullptr)) {
        comment->insert(QStringLiteral("source_context"), sourceContext(finalLines, newLine));
    }
    *inserted = true;
    return true;
}
}

struct DocsCommentSynchronizer::SynchronizationCache {
    QString docsRoot;
    qint64 buildsRootModifiedMs = 0;
    bool buildsLoaded = false;
    BuildRegistry builds;
    DocsAnchorOccurrences commentOccurrences;
    bool commentOccurrencesLoaded = false;
    bool commentSourcesComplete = false;
};

namespace
{
struct PublishedSourceArtifactResolution {
    bool attempted = false;
    DocsCommentSynchronizer::SourceResolutionResult result;
};

struct PublishedSourceInput {
    QString sourceFile;
    QString sourceSha256;
};

struct PublishedSourceCandidate {
    QString sourceFile;
    int line = 0;
    double distance = std::numeric_limits<double>::infinity();
    double area = std::numeric_limits<double>::infinity();
};

bool validPublishedDigest(const QString &value)
{
    static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
    return digest.match(value).hasMatch();
}

bool safePublishedSourcePath(const QString &value)
{
    const bool markdown = value.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || value.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive);
    if ((!value.endsWith(QLatin1String(".tex")) && !markdown) || value.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]"))) || value.startsWith(QLatin1Char('/')) || value.contains(QLatin1Char('\\'))) {
        return false;
    }
    const QStringList parts = value.split(QLatin1Char('/'));
    return !parts.isEmpty() && std::none_of(parts.cbegin(), parts.cend(), [](const QString &part) {
        return part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..");
    });
}

QByteArray verifiedPublishedObject(const QDir &bundleRoot, const QString &digest, QHash<QString, QByteArray> *cache)
{
    if (!validPublishedDigest(digest)) {
        return {};
    }
    if (cache && cache->contains(digest)) return cache->value(digest);
    QFile file(bundleRoot.filePath(QStringLiteral("objects/%1").arg(digest)));
    if (!file.open(QIODevice::ReadOnly) || file.size() > 256 * 1024 * 1024) {
        return {};
    }
    const QByteArray contents = file.readAll();
    if (sha256(contents) != digest) return {};
    if (cache) cache->insert(digest, contents);
    return contents;
}

QStringList publishedSourceLines(const QByteArray &contents)
{
    QStringList lines = QString::fromUtf8(contents).split(QLatin1Char('\n'));
    if (!lines.isEmpty() && lines.constLast().isEmpty()) {
        lines.removeLast();
    }
    return lines;
}

std::optional<PublishedSourceCandidate> resolvePublishedPageIndex(
    const QString &path,
    int page,
    double x,
    double y,
    const QHash<QString, PublishedSourceInput> &authoredInputs,
    bool markdown,
    QHash<QString, QList<QList<QByteArray>>> *cache)
{
    const QString cacheKey = path + QLatin1Char(':') + QString::number(page) + (markdown ? QStringLiteral(":md") : QStringLiteral(":tex"));
    QList<QList<QByteArray>> rows;
    if (cache && cache->contains(cacheKey)) {
        rows = cache->value(cacheKey);
    } else {
        KZip archive(path);
        if (!archive.open(QIODevice::ReadOnly)) {
            return std::nullopt;
        }
        if (markdown) {
            const KArchiveEntry *format = archive.directory()->entry(QStringLiteral("format.json"));
            if (!format || !format->isFile()) return std::nullopt;
            const QJsonObject header = QJsonDocument::fromJson(static_cast<const KZipFileEntry *>(format)->data()).object();
            if (header.value(QStringLiteral("schema_version")).toInt() != 1 || header.value(QStringLiteral("format")).toString() != QLatin1String("thoughts-markdown-source-map-v1")) return std::nullopt;
        }
        const KArchiveEntry *pagesEntry = archive.directory()->entry(QStringLiteral("pages"));
        if (!pagesEntry || !pagesEntry->isDirectory()) {
            return std::nullopt;
        }
        const auto *pages = static_cast<const KArchiveDirectory *>(pagesEntry);
        const KArchiveEntry *pageEntry = pages->entry(QStringLiteral("%1.tsv").arg(page));
        if (!pageEntry || !pageEntry->isFile()) {
            return std::nullopt;
        }
        std::unique_ptr<QIODevice> device(static_cast<const KZipFileEntry *>(pageEntry)->createDevice());
        if (!device) {
            return std::nullopt;
        }

        for (const auto &line : device->readAll().split('\n')) rows.append(line.split('\t'));
        if (cache) cache->insert(cacheKey, rows);
    }
    std::optional<PublishedSourceCandidate> bestPoint;
    std::optional<PublishedSourceCandidate> bestBox;
    for (const auto &fields : std::as_const(rows)) {
        if (fields.size() != (markdown ? 11 : 8)) {
            continue;
        }
        const QString sourceKey = QString::fromUtf8(fields.at(1));
        const auto input = authoredInputs.constFind(sourceKey);
        bool lineOk = false;
        bool xOk = false;
        bool yOk = false;
        bool widthOk = false;
        bool heightOk = false;
        bool depthOk = false;
        const int sourceLine = fields.at(2).toInt(&lineOk);
        const double nodeX = fields.at(3).toDouble(&xOk);
        const double baselineY = fields.at(4).toDouble(&yOk);
        const double width = fields.at(5).toDouble(&widthOk);
        const double height = fields.at(6).toDouble(&heightOk);
        const double depth = fields.at(7).toDouble(&depthOk);
        if (input == authoredInputs.cend() || !lineOk || sourceLine < 1 || !xOk || !yOk || !widthOk || !heightOk || !depthOk) {
            continue;
        }
        const double left = std::min(nodeX, nodeX + width);
        const double right = std::max(nodeX, nodeX + width);
        const double top = baselineY - std::max(0.0, height);
        const double bottom = baselineY + std::max(0.0, depth);
        const double dx = x < left ? left - x : (x > right ? x - right : 0.0);
        const double dy = y < top ? top - y : (y > bottom ? y - bottom : 0.0);
        const PublishedSourceCandidate candidate {
            input->sourceFile,
            sourceLine,
            dx * dx + dy * dy,
            std::max(0.0, right - left) * std::max(0.0, bottom - top),
        };
        std::optional<PublishedSourceCandidate> &best = fields.constFirst() == QByteArrayLiteral("x") ? bestPoint : bestBox;
        if (!best || candidate.distance < best->distance || (candidate.distance == best->distance && candidate.area < best->area)) {
            best = candidate;
        }
    }
    return bestPoint ? bestPoint : bestBox;
}

PublishedSourceArtifactResolution resolvePublishedSourceArtifact(
    const DocsCommentSynchronizer::Options &options,
    const DocsCommentSynchronizer::SourceResolutionRequest &request,
    QHash<QString, QByteArray> *objects,
    QHash<QString, QList<QList<QByteArray>>> *pages,
    QHash<QString, QJsonDocument> *jsonObjects)
{
    PublishedSourceArtifactResolution attempt;
    if (options.sourceBundleRoot.isEmpty()) {
        return attempt;
    }
    attempt.attempted = true;
    const auto fail = [&attempt](const QString &reason) {
        attempt.result.failureReason = reason;
        return attempt;
    };
    const QString filename = request.pdfStem + QStringLiteral(".pdf");
    const QDir bundleRoot(options.sourceBundleRoot);
    QJsonObject pointer;
    QString error;
    if (!readJsonObject(bundleRoot.filePath(QStringLiteral("documents/%1.json").arg(filename)), &pointer, &error)) {
        return fail(QStringLiteral("the verified source artifact is not installed"));
    }
    const QString artifactSha256 = pointer.value(QStringLiteral("artifact_sha256")).toString();
    const QString pdfSha256 = pointer.value(QStringLiteral("pdf_sha256")).toString();
    if (pointer.value(QStringLiteral("schema_version")).toInt() != 1
        || pointer.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact-pointer")
        || pointer.value(QStringLiteral("filename")).toString() != filename
        || !validPublishedDigest(artifactSha256) || !validPublishedDigest(pdfSha256)) {
        return fail(QStringLiteral("the installed source-artifact pointer is invalid"));
    }
    const QString pdfPath = request.pdfPath.isEmpty()
        ? QDir(options.docsRoot).filePath(QStringLiteral("current_pdfs/%1").arg(filename))
        : request.pdfPath;
    static QMutex verifiedPdfMutex;
    static QCache<QString, bool> verifiedPdfs(128);
    const QString identity = Thoughts::artifactFileIdentity(pdfPath);
    const QString key = identity + QLatin1Char(':') + pdfSha256;
    bool verified = false;
    {
        QMutexLocker lock(&verifiedPdfMutex);
        verified = !identity.isEmpty() && verifiedPdfs.contains(key);
    }
    if (!verified) {
        QFile pdfFile(pdfPath);
        QCryptographicHash pdfHash(QCryptographicHash::Sha256);
        if (identity.isEmpty() || !pdfFile.open(QIODevice::ReadOnly) || !pdfHash.addData(&pdfFile)
            || QString::fromLatin1(pdfHash.result().toHex()) != pdfSha256 || Thoughts::artifactFileIdentity(pdfPath) != identity) {
            return fail(QStringLiteral("the installed source artifact belongs to a different PDF revision"));
        }
        QMutexLocker lock(&verifiedPdfMutex);
        verifiedPdfs.insert(key, new bool(true));
    }

    const auto jsonObject = [&](const QString &digest) {
        if (jsonObjects && jsonObjects->contains(digest)) return jsonObjects->value(digest);
        const auto parsed = QJsonDocument::fromJson(verifiedPublishedObject(bundleRoot, digest, objects));
        if (jsonObjects && parsed.isObject()) jsonObjects->insert(digest, parsed);
        return parsed;
    };
    const QJsonDocument manifestDocument = jsonObject(artifactSha256);
    const QJsonObject manifest = manifestDocument.object();
    const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
    if (!manifestDocument.isObject()
        || manifest.value(QStringLiteral("schema_version")).toInt() != 1
        || manifest.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact")
        || manifest.value(QStringLiteral("filename")).toString() != filename
        || pdf.value(QStringLiteral("filename")).toString() != filename
        || pdf.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256
        || request.pageNumber > pdf.value(QStringLiteral("page_count")).toInt()) {
        return fail(QStringLiteral("the installed source-artifact manifest is invalid"));
    }

    QJsonObject segment;
    for (const QJsonValue &value : pdf.value(QStringLiteral("segments")).toArray()) {
        const QJsonObject candidate = value.toObject();
        const int start = candidate.value(QStringLiteral("start_page")).toInt();
        const int count = candidate.value(QStringLiteral("page_count")).toInt();
        if (start > 0 && count > 0 && request.pageNumber >= start && request.pageNumber < start + count) {
            segment = candidate;
            break;
        }
    }
    if (segment.isEmpty() || !segment.value(QStringLiteral("source_available")).toBool()) {
        return fail(QStringLiteral("the selected PDF page is not source-linked"));
    }
    const bool markdown = segment.value(QStringLiteral("source_format")).toString() == QLatin1String("markdown");
    const QString lookupObject = segment.value(QStringLiteral("lookup_object")).toString();
    const QString lookupSha256 = segment.value(QStringLiteral("lookup_sha256")).toString();
    if (!validPublishedDigest(lookupObject) || lookupObject != lookupSha256
        || segment.value(QStringLiteral("lookup_format")).toString() != (markdown ? QLatin1String("thoughts-markdown-source-map-v1") : QLatin1String("thoughts-reverse-synctex-v1"))
        || verifiedPublishedObject(bundleRoot, lookupObject, objects).isEmpty()) {
        return fail(QStringLiteral("the compact SyncTeX lookup is unavailable"));
    }

    if (markdown) {
        const QString identity = segment.value(QStringLiteral("renderer_evidence_object")).toString();
        const QJsonObject evidence = jsonObject(identity).object();
        if (evidence.value(QStringLiteral("schema_version")).toInt() != 1
            || identity != segment.value(QStringLiteral("renderer_evidence_sha256")).toString()
            || evidence.value(QStringLiteral("kind")).toString() != QLatin1String("markdown_renderer_evidence")
            || evidence.value(QStringLiteral("pdf_sha256")) != segment.value(QStringLiteral("fingerprint"))
            || evidence.value(QStringLiteral("source_sha256")) != segment.value(QStringLiteral("primary_source_sha256"))) {
            return fail(QStringLiteral("the Markdown renderer evidence is unavailable or inconsistent"));
        }
    }
    QHash<QString, QJsonObject> sources;
    for (const QJsonValue &value : manifest.value(QStringLiteral("sources")).toArray()) {
        const QJsonObject source = value.toObject();
        const QString path = source.value(QStringLiteral("path")).toString();
        if (safePublishedSourcePath(path)) {
            sources.insert(path, source);
        }
    }
    QHash<QString, PublishedSourceInput> inputs;
    for (const QJsonValue &value : segment.value(QStringLiteral("inputs")).toArray()) {
        const QJsonObject input = value.toObject();
        const QString syncTexPath = input.value(QStringLiteral("synctex_path")).toString();
        const QString sourceFile = input.value(QStringLiteral("source_file")).toString();
        const QString sourceSha256 = input.value(QStringLiteral("source_sha256")).toString();
        const QJsonObject source = sources.value(sourceFile);
        if (syncTexPath.isEmpty() || !safePublishedSourcePath(sourceFile) || !validPublishedDigest(sourceSha256)
            || source.value(QStringLiteral("sha256")).toString() != sourceSha256) {
            return fail(QStringLiteral("the source-artifact input map is invalid"));
        }
        inputs.insert(syncTexPath, {sourceFile, sourceSha256});
    }
    if (inputs.isEmpty()) {
        return fail(QStringLiteral("the source-artifact input map is empty"));
    }
    const int localPage = request.pageNumber - segment.value(QStringLiteral("start_page")).toInt() + 1;
    const auto mapped = resolvePublishedPageIndex(
        bundleRoot.filePath(QStringLiteral("objects/%1").arg(lookupObject)),
        localPage,
        request.x,
        request.y,
        inputs, markdown, pages);
    if (!mapped) {
        return fail(QStringLiteral("reverse SyncTeX did not identify a mapped source"));
    }

    const QJsonObject source = sources.value(mapped->sourceFile);
    const QString sourceObject = source.value(QStringLiteral("object")).toString();
    const QString sourceSha256 = source.value(QStringLiteral("sha256")).toString();
    if (markdown && (sourceSha256 != segment.value(QStringLiteral("primary_source_sha256")).toString()
        || mapped->sourceFile != segment.value(QStringLiteral("primary_source_file")).toString())) {
        return fail(QStringLiteral("the Markdown source does not match the renderer evidence"));
    }
    const QByteArray snapshot = verifiedPublishedObject(bundleRoot, sourceObject, objects);
    if (!validPublishedDigest(sourceObject) || sourceObject != sourceSha256 || snapshot.isEmpty()) {
        return fail(QStringLiteral("the mapped source snapshot is unavailable"));
    }
    const QString sourcePath = canonicalOrAbsolutePath(QDir(options.docsRoot).filePath(mapped->sourceFile));
    const QString projectRoot = managedProjectRoot(options.docsRoot, mapped->sourceFile);
    const bool editableMarkdown = markdown && !projectRoot.isEmpty() && sourcePath.startsWith(projectRoot + QLatin1Char('/'))
        && safePublishedSourcePath(mapped->sourceFile);
    if (!editableMarkdown && !markerEligibleSourcePath(options.docsRoot, mapped->sourceFile, sourcePath)) {
        if (hasGeneratedPathComponent(sourcePath)) {
            attempt.result.problem = DocsCommentSynchronizer::SourceResolutionResult::Problem::GeneratedContent;
            BuildInfo build;
            for (auto it = sources.cbegin(); it != sources.cend(); ++it) {
                const QString object = it->value(QStringLiteral("object")).toString();
                if (object != it->value(QStringLiteral("sha256")).toString() || verifiedPublishedObject(bundleRoot, object, objects).isEmpty()) {
                    continue;
                }
                const QString path = canonicalOrAbsolutePath(QDir(options.docsRoot).filePath(it.key()));
                build.sourceSnapshots.append({it.key(), path, bundleRoot.filePath(QStringLiteral("objects/%1").arg(object)), 0,
                                              markerEligibleSourcePath(options.docsRoot, it.key(), path)});
            }
            attempt.result.fallbackSourcePath = generatedSourceDocument(options.docsRoot, build, sourcePath);
            return fail(QStringLiteral("the selected content comes from a generated source"));
        }
        return fail(QStringLiteral("the mapped source is not editable"));
    }
    QStringList currentLines;
    if (!readTextLines(sourcePath, &currentLines, &error)) {
        return fail(error.isEmpty() ? QStringLiteral("the mapped source is unavailable") : error);
    }
    const QStringList snapshotLines = publishedSourceLines(snapshot);
    const int line = request.requireExactPassage
        ? remapAnchorPassage(snapshotLines, currentLines, mapped->line, request.selectedText, &attempt.result.snapshotLine)
        : remapSourceNavigationLine(snapshotLines, currentLines, mapped->line, request.selectedText);
    if (line < 1) {
        attempt.result.problem = DocsCommentSynchronizer::SourceResolutionResult::Problem::AmbiguousLocation;
        attempt.result.fallbackSourcePath = sourcePath;
        return fail(QStringLiteral("the historical source line could not be mapped safely"));
    }

    attempt.result.success = true;
    attempt.result.sourceId = mapped->sourceFile;
    attempt.result.sourcePath = sourcePath;
    attempt.result.buildId = artifactSha256;
    attempt.result.line = line;
    attempt.result.column = -1;
    return attempt;
}
}

DocsCommentSynchronizer::SourceResolutionResult DocsCommentSynchronizer::resolveSource(const SourceResolutionRequest &request)
{
    SourceResolutionResult result;
    const auto fail = [&result](const QString &reason) {
        result.failureReason = reason;
        return result;
    };
    if (request.pdfStem.isEmpty() || request.pageNumber < 1 || !std::isfinite(request.x) || !std::isfinite(request.y) || request.x < 0.0 || request.y < 0.0) {
        return fail(QStringLiteral("invalid source-resolution request"));
    }

    const PublishedSourceArtifactResolution published = resolvePublishedSourceArtifact(m_options, request,
        m_options.cacheSourceResolution ? &m_resolutionObjects : nullptr,
        m_options.cacheSourceResolution ? &m_resolutionPages : nullptr,
        m_options.cacheSourceResolution ? &m_resolutionJson : nullptr);
    if (published.result.success || (published.attempted && !m_options.allowLegacySourceResolution)) {
        return published.result;
    }

    SourceLocation source = sourceLocationForPage(m_options.docsRoot, request.pdfStem, request.pageNumber);
    const QString buildsRoot = QDir(m_options.docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds"));
    const qint64 modifiedMs = QFileInfo(buildsRoot).lastModified().toMSecsSinceEpoch();
    if (!m_cache->buildsLoaded || m_cache->docsRoot != m_options.docsRoot || m_cache->buildsRootModifiedMs != modifiedMs) {
        m_cache->builds = cachedBuildRegistry(m_options.docsRoot);
        m_cache->docsRoot = m_options.docsRoot;
        m_cache->buildsRootModifiedMs = modifiedMs;
        m_cache->buildsLoaded = true;
    }
    if (!source.valid && source.sourceId.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) && !source.fingerprint.isEmpty()
        && !m_cache->builds.ambiguousSourceFingerprints.contains(source.fingerprint)) {
        const QString linkedSourceId = m_cache->builds.uniqueSourceIdsByFingerprint.value(source.fingerprint);
        const QString linkedSourcePath = QFileInfo(QDir(m_options.docsRoot).filePath(linkedSourceId)).absoluteFilePath();
        if (!linkedSourceId.isEmpty() && markerEligibleSourcePath(m_options.docsRoot, linkedSourceId, linkedSourcePath)) {
            source.valid = true;
            source.sourceId = linkedSourceId;
            source.sourcePath = linkedSourcePath;
        }
    }
    if (!source.valid) {
        return fail(QStringLiteral("the selected PDF page is not source-linked"));
    }
    const BuildInfo build = buildForSource(m_cache->builds, source);
    if (!build.valid) {
        return fail(QStringLiteral("the exact SyncTeX build is unavailable"));
    }
    if (!request.pdfPath.trimmed().isEmpty()) {
        const QString requestedPdfSha256 = fileSha256(request.pdfPath);
        if (requestedPdfSha256.isEmpty()) {
            return fail(QStringLiteral("the selected PDF is unavailable for source verification"));
        }
        if (fileSha256(build.cachedPdf) != requestedPdfSha256) {
            return fail(QStringLiteral("the exact SyncTeX build belongs to a different PDF revision"));
        }
    }
    const QString executable = resolveSynctexExecutable(m_options);
    if (executable.isEmpty()) {
        return fail(QStringLiteral("synctex executable was not found"));
    }

    QProcess process;
    process.setWorkingDirectory(build.directory);
    process.setProcessEnvironment(m_options.processEnvironment);
    process.start(executable,
                  {
                      QStringLiteral("edit"),
                      QStringLiteral("-o"),
                      QStringLiteral("%1:%2:%3:%4").arg(source.localPage).arg(request.x, 0, 'f', 6).arg(request.y, 0, 'f', 6).arg(build.cachedPdf),
                      QStringLiteral("-d"),
                      build.directory,
                  });
    if (!process.waitForStarted(std::min(1000, m_options.synctexTimeoutMs))) {
        return fail(QStringLiteral("synctex could not start: %1").arg(process.errorString()));
    }
    if (!process.waitForFinished(m_options.synctexTimeoutMs)) {
        process.kill();
        process.waitForFinished();
        return fail(QStringLiteral("synctex timed out"));
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        return fail(QStringLiteral("synctex exited with code %1").arg(process.exitCode()));
    }

    std::optional<SynctexRecord> matching;
    const SourceSnapshot *matchingSource = nullptr;
    for (const SynctexRecord &record : parseSynctexRecords(QString::fromUtf8(process.readAllStandardOutput()))) {
        const QString candidate = normalizedCandidatePath(record.fields.value(QStringLiteral("Input")), m_options.docsRoot, build);
        if (const SourceSnapshot *snapshot = snapshotForCandidate(build, candidate)) {
            matching = record;
            matchingSource = snapshot;
            break;
        }
    }
    if (!matching || !matchingSource) {
        return fail(QStringLiteral("reverse SyncTeX did not identify a mapped source"));
    }
    bool oldLineOk = false;
    const int oldLine = matching->fields.value(QStringLiteral("Line")).toInt(&oldLineOk);
    if (!oldLineOk || oldLine < 1) {
        return fail(QStringLiteral("reverse SyncTeX did not return a source line"));
    }

    QString sourceId = matchingSource->sourceId;
    QString sourcePath = matchingSource->sourcePath;
    if (!matchingSource->markerEligible || !markerEligibleSourcePath(m_options.docsRoot, sourceId, sourcePath)) {
        if (hasGeneratedPathComponent(sourcePath)) {
            result.problem = SourceResolutionResult::Problem::GeneratedContent;
            result.fallbackSourcePath = generatedSourceDocument(m_options.docsRoot, build, sourcePath);
            return fail(QStringLiteral("the selected content comes from a generated source"));
        }
        return fail(QStringLiteral("the mapped source is not editable"));
    }
    int resolvedLine = 0;
    QStringList snapshotLines;
    QStringList currentLines;
    QString error;
    if (!readTextLines(matchingSource->snapshotPath, &snapshotLines, &error) || !readTextLines(sourcePath, &currentLines, &error)) {
        return fail(error.isEmpty() ? QStringLiteral("the mapped source is unavailable") : error);
    }
    resolvedLine = remapSourceNavigationLine(snapshotLines, currentLines, oldLine, request.selectedText);
    if (resolvedLine < 1) {
        result.problem = SourceResolutionResult::Problem::AmbiguousLocation;
        result.fallbackSourcePath = canonicalOrAbsolutePath(sourcePath);
        return fail(QStringLiteral("the historical source line could not be mapped safely"));
    }

    result.success = true;
    result.sourceId = sourceId;
    result.sourcePath = canonicalOrAbsolutePath(sourcePath);
    result.buildId = build.buildId;
    result.line = resolvedLine;
    bool columnOk = false;
    result.column = matching->fields.value(QStringLiteral("Column")).toInt(&columnOk);
    if (!columnOk) {
        result.column = -1;
    }
    return result;
}

QDateTime DocsCommentSynchronizer::parseCommentDate(const QJsonValue &value, const QTimeZone &localTimeZone)
{
    const QString raw = value.toString().trimmed();
    if (raw.isEmpty() || !localTimeZone.isValid()) {
        return {};
    }
    if (!raw.startsWith(QLatin1String("D:"))) {
        QDateTime parsed = QDateTime::fromString(raw, Qt::ISODateWithMs);
        if (!parsed.isValid()) {
            parsed = QDateTime::fromString(raw, Qt::ISODate);
        }
        if (!parsed.isValid()) {
            return {};
        }
        if (parsed.timeSpec() == Qt::LocalTime) {
            return QDateTime(parsed.date(), parsed.time(), localTimeZone);
        }
        return parsed.toUTC();
    }

    static const QRegularExpression expression(QStringLiteral("^D:(\\d{4})(\\d{2})?(\\d{2})?(\\d{2})?(\\d{2})?(\\d{2})?([Zz]|[+-]\\d{2}'?\\d{2}'?)?$"));
    const QRegularExpressionMatch match = expression.match(raw);
    if (!match.hasMatch()) {
        return {};
    }
    const auto numberOr = [&match](int index, int fallback) { return match.captured(index).isEmpty() ? fallback : match.captured(index).toInt(); };
    const QDate date(numberOr(1, 1970), numberOr(2, 1), numberOr(3, 1));
    const QTime time(numberOr(4, 0), numberOr(5, 0), numberOr(6, 0));
    if (!date.isValid() || !time.isValid()) {
        return {};
    }

    const QString offset = match.captured(7);
    if (offset.isEmpty()) {
        return QDateTime(date, time, localTimeZone);
    }

    QDateTime parsed(date, time, QTimeZone::UTC);
    if (offset.compare(QLatin1String("Z"), Qt::CaseInsensitive) == 0) {
        return parsed;
    }
    QString digits = offset.mid(1);
    digits.remove(QLatin1Char('\''));
    if (digits.size() != 4) {
        return {};
    }
    const int hours = digits.left(2).toInt();
    const int minutes = digits.mid(2, 2).toInt();
    if (hours > 23 || minutes > 59) {
        return {};
    }
    const int seconds = (hours * 60 + minutes) * 60;
    return offset.startsWith(QLatin1Char('+')) ? parsed.addSecs(-seconds) : parsed.addSecs(seconds);
}

bool DocsCommentSynchronizer::isTrackedCommentRecord(const QJsonObject &record)
{
    return isTrackedCommentRecordImpl(record);
}

DocsCommentSynchronizer::DocsCommentSynchronizer(Options options)
    : m_options(std::move(options))
    , m_cache(std::make_unique<SynchronizationCache>())
{
}

DocsCommentSynchronizer::~DocsCommentSynchronizer() = default;

QJsonObject DocsCommentSynchronizer::sourceCommentFileIndex(const QString &path, const QStringList &identifiers)
{
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) return {};
    const auto bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) return {};
    const auto text = QString::fromUtf8(bytes);
    QJsonObject markers;
    QJsonArray mentioned, locations;
    for (const auto &id : identifiers) if (text.contains(id)) mentioned.append(id);
    const bool markdown = path.endsWith(QLatin1String(".md")) || path.endsWith(QLatin1String(".markdown"));
    if (markdown) {
        const auto mirrors = markdownCommentMirrors(text);
        for (auto it = mirrors.begin(); it != mirrors.end(); ++it) {
            markers.insert(it.key(), QJsonArray{QJsonObject{{QStringLiteral("line"), it.value().first}, {QStringLiteral("contents"), it.value().second},
                {QStringLiteral("kind"), QStringLiteral("markdown")}}});
            locations.append(QJsonObject{{QStringLiteral("type"), QStringLiteral("comment")}, {QStringLiteral("id"), it.key()},
                {QStringLiteral("start_line"), it.value().first}, {QStringLiteral("end_line"), it.value().first}});
        }
    } else {
        for (const auto &span : commentAnchorSpans(text)) {
            const QString command = commentAnchorCommandAt(text, span.commandStart);
            // "\CommentAnchor{id}{text}{}%" (written by the August 2026 anchor worker) is a point
            // comment: the macro takes two arguments, so the empty group highlights nothing.
            const bool explicitMarkup = command == QLatin1String("\\CommentHighlight") || command == QLatin1String("\\CommentStrikethrough");
            const bool body = span.hasBody && (explicitMarkup || !span.body.trimmed().isEmpty());
            const auto type = !body ? QStringLiteral("comment") : command == QLatin1String("\\CommentStrikethrough")
                ? QStringLiteral("strikethrough") : QStringLiteral("highlight");
            const int end = body ? text.left(span.commandEnd - 1).count(QLatin1Char('\n')) + 1 : span.line;
            locations.append(QJsonObject{{QStringLiteral("type"), type}, {QStringLiteral("id"), span.id},
                {QStringLiteral("start_line"), span.line}, {QStringLiteral("end_line"), end}});
            if (!span.hasMirror) continue;
            auto matches = markers.value(span.id).toArray();
            const QString kind = command == QLatin1String("\\CommentHighlight") ? QStringLiteral("highlight")
                : command == QLatin1String("\\CommentStrikethrough") ? QStringLiteral("strikethrough")
                : command == QLatin1String("\\ReviewAnchor") ? QStringLiteral("review_anchor") : QStringLiteral("anchor");
            QJsonObject marker{{QStringLiteral("line"), span.line}, {QStringLiteral("contents"), decodeCommentAnchorMirror(span.mirror)}, {QStringLiteral("kind"), kind}};
            if (body) marker.insert(QStringLiteral("selected_text"), decodeCommentAnchorMirror(span.body));
            matches.append(marker); markers.insert(span.id, matches);
        }
    }
    auto result = Thoughts::scanAnnotationLocations(path, bytes, locations);
    result.insert(QStringLiteral("annotation_parser_version"), Thoughts::annotationParserVersion);
    result.insert(QStringLiteral("markers"), markers);
    for (const auto &id : result.value(QStringLiteral("mentioned")).toArray()) if (!mentioned.contains(id)) mentioned.append(id);
    result.insert(QStringLiteral("mentioned"), mentioned);
    return result;
}

bool DocsCommentSynchronizer::sourceAuthoritativeComment(QJsonObject *comment, const QJsonObject &previous,
                                                        const PageGeometryProvider &geometry, bool mutate, QString *error)
{
    if (comment->value(QStringLiteral("subtype")).toString() == QLatin1String("Note")
        || !comment->value(QStringLiteral("parent_id")).toString().isEmpty()) return true;
    auto scope = comment->value(QStringLiteral("scope")).toObject();
    const auto oldScope = previous.value(QStringLiteral("scope")).toObject();
    const QString id = scope.value(QStringLiteral("anchor_id")).toString(comment->value(QStringLiteral("comment_id")).toString());
    if (id.isEmpty()) return false;
    const bool anchorRemoved = Thoughts::isAnchorRemovedScope(scope) || Thoughts::isAnchorRemovedScope(oldScope);
    // An anchor-removed comment is still projected so that a reappeared marker relinks it.
    const bool linked = scope.value(QStringLiteral("state")).toString() == QLatin1String("source_linked")
        || oldScope.value(QStringLiteral("state")).toString() == QLatin1String("source_linked") || anchorRemoved;
    if (!mutate && !linked && comment->value(QStringLiteral("source_authority")).toString() != QLatin1String("client-v1")) return true;
    bool complete = false;
    DocsAnchorOccurrences index;
    if (!mutate && m_options.cacheSourceProjection && m_cache->commentOccurrencesLoaded) {
        index = m_cache->commentOccurrences;
        complete = m_cache->commentSourcesComplete;
    } else {
        index = anchorOccurrences(commentSourceSet(), &complete);
        if (m_options.commentSourceFiles) complete = complete && m_options.commentSourceFilesComplete;
        if (!mutate && m_options.cacheSourceProjection) {
            m_cache->commentOccurrences = index;
            m_cache->commentSourcesComplete = complete;
            m_cache->commentOccurrencesLoaded = true;
        }
    }
    if (!mutate && !complete) return true; // Keep the saved state until the project can be checked completely.
    auto occurrences = index.value(id);
    const QString knownSource = scope.value(QStringLiteral("source_file")).toString(oldScope.value(QStringLiteral("source_file")).toString());
    if (knownSource.contains(QLatin1Char('/'))) {
        const QString project = sourceOwnerOrTop(knownSource);
        occurrences.removeIf([&](const auto &occurrence) {
            return sourceOwnerOrTop(QDir(canonicalOrAbsolutePath(m_options.docsRoot)).relativeFilePath(canonicalOrAbsolutePath(occurrence.path))) != project;
        });
    }
    const auto failed = [&](const QString &reason) {
        if (!mutate && linked && reason != QLatin1String("source_marker_ambiguous")) return true;
        scope.insert(QStringLiteral("state"), QStringLiteral("needs_attention"));
        scope.insert(QStringLiteral("reason"), reason);
        comment->insert(QStringLiteral("scope"), scope);
        return true;
    };
    if (mutate) comment->insert(QStringLiteral("source_authority"), QStringLiteral("client-v1"));
    if (occurrences.size() > 1) return failed(QStringLiteral("source_marker_ambiguous"));
    if (occurrences.size() == 1) {
        const auto occurrence = occurrences.first();
        const auto source = sourceMirrorAt(occurrence, id, error);
        if (!source.valid || !source.hasMirror) return failed(QStringLiteral("source_marker_invalid"));
        if (mutate && comment->value(QStringLiteral("deleted")).toBool()) {
            return removeAnchor(occurrence, id, error);
        }
        const bool edited = mutate && !previous.isEmpty()
            && comment->value(QStringLiteral("contents")) != previous.value(QStringLiteral("contents"));
        if (edited) {
            bool changed = false;
            if (!rewriteAnchorMirror(occurrence, id, comment->value(QStringLiteral("contents")).toString(), &changed, error)) return false;
        } else {
            comment->insert(QStringLiteral("contents"), source.contents);
            if (source.selectedText) comment->insert(QStringLiteral("selected_text"), *source.selectedText);
        }
        scope = {{QStringLiteral("state"), QStringLiteral("source_linked")}, {QStringLiteral("anchor_id"), id},
                 {QStringLiteral("source_file"), QDir(canonicalOrAbsolutePath(m_options.docsRoot)).relativeFilePath(canonicalOrAbsolutePath(occurrence.path))},
                 {QStringLiteral("source_line"), occurrence.line}, {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}};
        comment->insert(QStringLiteral("scope"), scope);
        return true;
    }
    if (anchorRemoved) {
        // No marker: keep the anchor-removed scope. Edits, deletes and Undo push
        // content only; they never relink from PDF geometry or re-insert a marker.
        const auto kept = Thoughts::isAnchorRemovedScope(scope) ? scope : oldScope;
        comment->insert(QStringLiteral("scope"), kept);
        return true;
    }
    const bool restoring = mutate && previous.value(QStringLiteral("deleted")).toBool()
        && !comment->value(QStringLiteral("deleted")).toBool();
    if (linked && !restoring) {
        // Absence is evidence only with a complete readable tree and a previously verified source.
        // A deleted source file (other than a document's own entry, which retires the
        // document on the server) also leaves the comment anchor removed.
        const QString path = scope.value(QStringLiteral("source_file")).toString(oldScope.value(QStringLiteral("source_file")).toString());
        // The project must be present locally: a device without its sources has no evidence.
        const QSet<QString> sources = commentSourceSet();
        const bool projectPresent = m_options.commentSourceFiles
            || ((QFileInfo::exists(QDir(m_options.docsRoot).filePath(path)) || QFileInfo(QDir(m_options.docsRoot).filePath(sourceOwnerOrTop(path))).isDir()) && !sources.isEmpty());
        if (complete && !path.isEmpty() && projectPresent) {
            bool rawIdPresent = false;
            for (const auto &file : sources) {
                QFile input(file);
                if (!input.open(QIODevice::ReadOnly)) return failed(QStringLiteral("source_unavailable"));
                if (input.readAll().contains(id.toUtf8())) { rawIdPresent = true; break; }
            }
            if (!rawIdPresent) {
                // The marker vanished without an explicit Delete: keep the comment as anchor removed.
                const auto lastLinked = scope.value(QStringLiteral("state")).toString() == QLatin1String("source_linked") ? scope : oldScope;
                comment->insert(QStringLiteral("scope"), Thoughts::anchorRemovedScope(lastLinked, *comment));
                return true;
            }
        }
        return failed(QStringLiteral("source_marker_unavailable"));
    }
    if (!mutate || comment->value(QStringLiteral("deleted")).toBool()) return true;
    const auto origin = comment->value(QStringLiteral("origin")).toObject();
    const QString stem = origin.value(QStringLiteral("collection")).toString();
    const QString pdfPath = QDir(m_options.docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(stem));
    if (fileSha256(pdfPath) != origin.value(QStringLiteral("pdf_sha256")).toString()) return failed(QStringLiteral("pdf_revision_unavailable"));
    const auto raw = origin.value(QStringLiteral("raw_pdf_geometry")).toObject();
    const int page = raw.value(QStringLiteral("page")).toInt(origin.value(QStringLiteral("page_index")).toInt() + 1);
    const auto rect = raw.value(QStringLiteral("rect")).toArray();
    const auto size = geometry ? geometry(pdfPath, page, error) : std::nullopt;
    if (rect.size() != 4 || !size || size->rotation != 0) return failed(QStringLiteral("pdf_geometry_unavailable"));
    const auto resolved = resolveSource({.pdfStem = stem, .pageNumber = page,
        .x = (rect[0].toDouble() + rect[2].toDouble()) / 2,
        .y = size->height - (rect[1].toDouble() + rect[3].toDouble()) / 2,
        .selectedText = comment->value(QStringLiteral("selected_text")).toString(), .pdfPath = pdfPath, .requireExactPassage = true});
    if (!resolved.success) return failed(resolved.failureReason);
    if (!resolved.sourcePath.endsWith(QLatin1String(".tex"))) return failed(QStringLiteral("unsupported_source_format"));
    int line = resolved.line;
    const QString subtype = comment->value(QStringLiteral("subtype")).toString();
    const QString contents = comment->value(QStringLiteral("contents")).toString();
    if (isTextMarkupSubtype(subtype)) {
        if (!wrapHighlightedSource(resolved.sourcePath, line, id, contents,
            comment->value(QStringLiteral("selected_text")).toString(), subtype, &line, error)) return failed(QStringLiteral("selection_not_uniquely_editable"));
    } else {
        QStringList lines;
        if (!readTextLines(resolved.sourcePath, &lines, error) || !safeInsertionLine(lines, line)) return failed(QStringLiteral("unsafe_source_position"));
        if (!insertOrMoveAnchor({}, resolved.sourcePath, &line, id, contents, error)) return false;
    }
    int definitionLine = 0, addedLines = 0;
    if (!ensureCommentAnchorDefinition(resolved.sourcePath, &definitionLine, &addedLines, error)) return false;
    if (addedLines > 0 && line >= definitionLine) line += addedLines;
    comment->insert(QStringLiteral("scope"), QJsonObject{{QStringLiteral("state"), QStringLiteral("source_linked")},
        {QStringLiteral("anchor_id"), id}, {QStringLiteral("source_file"), resolved.sourceId}, {QStringLiteral("source_line"), line},
        {QStringLiteral("reason"), QStringLiteral("unique_source_marker")}});
    return true;
}

QStringList DocsCommentSynchronizer::sourceFilesForDocument(const QString &docsRoot, const QString &pdfStem, QString *error)
{
    if (pdfStem.isEmpty() || QFileInfo(pdfStem).fileName() != pdfStem) {
        if (error) {
            *error = QStringLiteral("invalid PDF stem");
        }
        return {};
    }

    QList<QJsonObject> comments;
    const QString canonicalPath = QDir(docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
    if (!readJsonl(canonicalPath, &comments, error)) {
        return {};
    }
    const QSet<QString> mappedPaths = mappedSourceFilesForDocument(docsRoot, pdfStem, comments, error);
    QStringList paths(mappedPaths.begin(), mappedPaths.end());
    std::sort(paths.begin(), paths.end());
    return paths;
}

QList<QJsonObject> DocsCommentSynchronizer::visibleSidecarComments(const QString &docsRoot, const QString &pdfStem, QByteArray *fingerprint, QString *error, QSet<QString> *suppressedIds)
{
    if (suppressedIds) {
        suppressedIds->clear();
    }
    if (pdfStem.isEmpty() || QFileInfo(pdfStem).fileName() != pdfStem) {
        if (error) {
            *error = QStringLiteral("invalid PDF stem");
        }
        return {};
    }
    const QString commentsRoot = QDir(docsRoot).filePath(QStringLiteral("comments"));
    const QString sidecarPath = QDir(commentsRoot).filePath(pdfStem + QStringLiteral("_comments.jsonl"));
    const QString resolutionPath = QDir(commentsRoot).filePath(QStringLiteral("review_resolutions.jsonl"));
    // The visible records follow from the two files alone: kept while both
    // are unchanged (a missing one has no identity and stays missing).
    struct Visible {
        QString identities;
        QList<QJsonObject> visible;
        QSet<QString> suppressed;
        QByteArray fingerprint;
    };
    static QMutex visibleMutex;
    static QCache<QString, Visible> visibleCache(4096);
    // Sync conflict copies are merged into the file (reconcileJsonl()), which
    // writes it: never answered from memory.
    const auto identities = [&] {
        QString key;
        for (const QString &path : {sidecarPath, resolutionPath}) {
            const QFileInfo info(path);
            if (!syncConflictFiles(info, info.completeBaseName() + QStringLiteral(".sync-conflict-*.jsonl")).isEmpty()) return QString();
            const QString identity = Thoughts::artifactFileIdentity(path);
            if (identity.isEmpty() && QFileInfo::exists(path)) return QString();
            key += (identity.isEmpty() ? QStringLiteral("missing:") + path : identity) + QLatin1Char('\n');
        }
        return key;
    };
    const QString identitiesBefore = identities();
    if (!identitiesBefore.isEmpty()) {
        QMutexLocker lock(&visibleMutex);
        if (const Visible *kept = visibleCache.object(sidecarPath); kept && kept->identities == identitiesBefore) {
            Thoughts::fileReadCounts().hits += 2;
            if (suppressedIds) {
                *suppressedIds = kept->suppressed;
            }
            if (fingerprint) {
                *fingerprint = kept->fingerprint;
            }
            return kept->visible;
        }
    }
    QList<QJsonObject> records;
    QList<QJsonObject> resolutions;
    if (!reconcileJsonlKept(sidecarPath, &records, error) || !reconcileJsonlKept(resolutionPath, &resolutions, error)) {
        return {};
    }
    QSet<QString> suppressed;
    for (const QJsonObject &record : std::as_const(records)) {
        if (!isDeletedRecord(record)) {
            continue;
        }
        const QString id = stableRecordId(record);
        if (!id.isEmpty()) {
            suppressed.insert(id);
        }
    }
    const QList<QJsonObject> publicResolutions = resolutionTombstones(resolutions);
    for (const QJsonObject &resolution : publicResolutions) {
        QString id = resolution.value(QStringLiteral("comment_key")).toString(resolution.value(QStringLiteral("key")).toString());
        if (id.startsWith(QLatin1String("id:"))) {
            id.remove(0, 3);
        }
        if (!id.isEmpty()) {
            suppressed.insert(id);
        }
    }
    QList<QJsonObject> visible = applyTombstones(liveRecords(records), publicResolutions);
    if (suppressedIds) {
        *suppressedIds = suppressed;
    }
    const bool keep = !identitiesBefore.isEmpty() && identities() == identitiesBefore;
    if (fingerprint || keep) {
        QByteArray computed;
        QJsonArray encodedVisible;
        for (const QJsonObject &record : std::as_const(visible)) {
            encodedVisible.append(record);
        }
        QStringList encodedSuppressed(suppressed.begin(), suppressed.end());
        std::sort(encodedSuppressed.begin(), encodedSuppressed.end());
        QJsonArray encodedSuppressedIds;
        for (const QString &id : std::as_const(encodedSuppressed)) {
            encodedSuppressedIds.append(id);
        }
        computed = QCryptographicHash::hash(
            compactJson(QJsonObject {
                {QStringLiteral("visible"), encodedVisible},
                {QStringLiteral("suppressed_ids"), encodedSuppressedIds},
            }),
            QCryptographicHash::Sha256);
        if (fingerprint) {
            *fingerprint = computed;
        }
        if (keep) {
            QMutexLocker lock(&visibleMutex);
            visibleCache.insert(sidecarPath, new Visible {identitiesBefore, visible, suppressed, computed});
        }
    }
    return visible;
}

namespace
{

enum class CommentUpdateMode {
    Comment,
    ChatGptUrl,
};

DocsCommentSynchronizer::CommentMetadataUpdateResult updateSidecarCommentImpl(const QString &docsRoot,
                                                                               const QString &pdfStem,
                                                                               const QString &annotationId,
                                                                               const std::optional<QString> &contents,
                                                                               const QJsonObject &patch,
                                                                               int lockTimeoutMs,
                                                                               CommentUpdateMode mode)
{
    DocsCommentSynchronizer::CommentMetadataUpdateResult result;
    QElapsedTimer timer;
    timer.start();
    const auto fail = [&result, &timer](const QString &reason) {
        result.failureReason = reason;
        result.totalMs = timer.elapsed();
        return result;
    };
    if (pdfStem.isEmpty() || QFileInfo(pdfStem).fileName() != pdfStem) {
        return fail(QStringLiteral("invalid PDF stem"));
    }
    QString stableId = annotationId;
    if (stableId.startsWith(QLatin1String("id:"))) {
        stableId.remove(0, 3);
    }
    if (stableId.isEmpty()) {
        return fail(QStringLiteral("invalid comment identifier"));
    }
    const QStringList metadataFields = mode == CommentUpdateMode::ChatGptUrl
        ? QStringList {QStringLiteral("chatgpt_url")}
        : QStringList {QStringLiteral("priority"), QStringLiteral("location")};
    for (auto iterator = patch.begin(); iterator != patch.end(); ++iterator) {
        if (!metadataFields.contains(iterator.key())) {
            return fail(QStringLiteral("unsupported comment metadata field: %1").arg(iterator.key()));
        }
    }
    if (mode == CommentUpdateMode::ChatGptUrl && contents) {
        return fail(QStringLiteral("the ChatGPT link update cannot change comment contents"));
    }
    const QJsonValue priority = patch.value(QStringLiteral("priority"));
    if (!priority.isUndefined() && !priority.isNull() && (!priority.isString() || priority.toString().trimmed().isEmpty())) {
        return fail(QStringLiteral("priority must be a non-empty string or null"));
    }
    const QJsonValue location = patch.value(QStringLiteral("location"));
    if (!location.isUndefined() && !location.isNull() && !location.isObject()) {
        return fail(QStringLiteral("location must be an object or null"));
    }
    const QJsonValue chatGptUrl = patch.value(QStringLiteral("chatgpt_url"));
    if (!chatGptUrl.isUndefined() && !chatGptUrl.isNull() && (!chatGptUrl.isString() || chatGptUrl.toString().trimmed().isEmpty())) {
        return fail(QStringLiteral("chatgpt_url must be a non-empty string or null"));
    }
    if (patch.isEmpty() && !contents) {
        result.success = true;
        result.totalMs = timer.elapsed();
        return result;
    }

    const QString lockPath = commentSyncLockPath(docsRoot, pdfStem);
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
        return fail(QStringLiteral("could not create the comment synchronization lock directory"));
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(lockTimeoutMs)) {
        return fail(QStringLiteral("another synchronization is already updating this document"));
    }

    const QString canonicalPath = QDir(docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
    QList<QJsonObject> records;
    QString error;
    if (!reconcileJsonl(canonicalPath, &records, &error)) {
        return fail(QStringLiteral("could not read the canonical sidecar: %1").arg(error));
    }
    int recordIndex = -1;
    for (int index = 0; index < records.size(); ++index) {
        if (!isDeletedRecord(records.at(index)) && stableRecordId(records.at(index)) == stableId) {
            if (recordIndex >= 0) {
                return fail(QStringLiteral("the comment identifier is ambiguous"));
            }
            recordIndex = index;
        }
    }
    if (recordIndex < 0) {
        return fail(QStringLiteral("the comment could not be found in the canonical sidecar"));
    }

    QJsonObject updated = records.at(recordIndex);
    result.previousContents = updated.value(QStringLiteral("contents")).toString();
    result.currentContents = contents.value_or(result.previousContents);
    if (contents) {
        updated.insert(QStringLiteral("contents"), *contents);
    }
    for (const QString &field : metadataFields) {
        result.previousMetadata.insert(field, updated.contains(field) ? updated.value(field) : QJsonValue(QJsonValue::Null));
        if (!patch.contains(field)) {
            result.currentMetadata.insert(field, result.previousMetadata.value(field));
            continue;
        }
        const QJsonValue value = patch.value(field);
        if (value.isNull()) {
            updated.remove(field);
        } else {
            updated.insert(field, value);
        }
        result.currentMetadata.insert(field, value);
    }

    result.changed = updated.value(QStringLiteral("contents")).toString() != records.at(recordIndex).value(QStringLiteral("contents")).toString();
    for (const QString &field : metadataFields) {
        result.changed = result.changed || compactJson(updated.value(field)) != compactJson(records.at(recordIndex).value(field));
    }
    if (!result.changed) {
        result.success = true;
        result.totalMs = timer.elapsed();
        return result;
    }

    updated.insert(QStringLiteral("schema_version"), SidecarSchemaVersion);
    updated.insert(QStringLiteral("revision"), std::max(1, updated.value(QStringLiteral("revision")).toInt(1)) + 1);
    updated.insert(QStringLiteral("origin_id"), QStringLiteral("mac-overview"));
    if (mode == CommentUpdateMode::Comment) {
        const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        updated.insert(QStringLiteral("updated_at"), timestamp);
        if (contents) {
            updated.insert(QStringLiteral("modified"), timestamp);
        }
    }

    QString statePath;
    QJsonObject state;
    bool stateDirty = false;
    if (contents) {
        const QString anchorId = updated.value(QStringLiteral("anchor_id")).toString().trimmed();
        const QString texFile = updated.value(QStringLiteral("tex_file")).toString().trimmed();
        const int texLine = updated.value(QStringLiteral("tex_line")).toInt();
        if (!anchorId.isEmpty() && !texFile.isEmpty() && texLine > 0) {
            const QFileInfo texInfo(texFile);
            const QString texPath = canonicalOrAbsolutePath(texInfo.isAbsolute() ? texFile : QDir(docsRoot).filePath(texFile));
            const QList<DocsAnchorOccurrence> occurrences = anchorOccurrences({texPath}).value(anchorId);
            if (occurrences.size() != 1) {
                return fail(QStringLiteral("the comment source anchor is not uniquely available in %1").arg(texFile));
            }
            updated.insert(QStringLiteral("tex_line"), occurrences.constFirst().line);
            bool mirrorUpdated = false;
            if (!rewriteAnchorMirror(occurrences.constFirst(), anchorId, *contents, &mirrorUpdated, &error)) {
                return fail(QStringLiteral("could not update the comment source mirror: %1").arg(error));
            }
            result.sourceMirrorUpdated = mirrorUpdated;
            statePath = QDir(docsRoot).filePath(QStringLiteral("comments/.state/%1.json").arg(pdfStem));
            QJsonObject rawState;
            if (!readJsonObject(statePath, &rawState, &error)) {
                return fail(QStringLiteral("could not read comment state: %1").arg(error));
            }
            state = loadedState(rawState);
            const SourceMirror source = sourceMirrorAt(occurrences.constFirst(), anchorId, &error);
            if (!source.valid) {
                return fail(QStringLiteral("could not verify the updated comment source mirror: %1").arg(error));
            }
            QJsonObject mirrors = state.value(QStringLiteral("source_mirrors")).toObject();
            mirrors.insert(commentKey(updated), sourceMirrorState(commentKey(updated), updated, source));
            state.insert(QStringLiteral("source_mirrors"), mirrors);
            stateDirty = true;
        }
    }

    records[recordIndex] = updated;
    if (!writeJsonl(canonicalPath, reindexComments(records), &error) || (stateDirty && !writeJsonObject(statePath, state, &error))) {
        return fail(QStringLiteral("could not commit the Overview comment: %1").arg(error));
    }
    result.success = true;
    result.totalMs = timer.elapsed();
    return result;
}

}

DocsCommentSynchronizer::CommentMetadataUpdateResult DocsCommentSynchronizer::updateSidecarCommentMetadata(const QString &docsRoot,
                                                                                                             const QString &pdfStem,
                                                                                                             const QString &annotationId,
                                                                                                             const QJsonObject &patch,
                                                                                                             int lockTimeoutMs)
{
    return updateSidecarCommentImpl(docsRoot, pdfStem, annotationId, std::nullopt, patch, lockTimeoutMs, CommentUpdateMode::Comment);
}

DocsCommentSynchronizer::CommentMetadataUpdateResult DocsCommentSynchronizer::updateSidecarComment(const QString &docsRoot,
                                                                                                     const QString &pdfStem,
                                                                                                     const QString &annotationId,
                                                                                                     const QString &contents,
                                                                                                     const QJsonObject &metadataPatch,
                                                                                                     int lockTimeoutMs)
{
    return updateSidecarCommentImpl(docsRoot, pdfStem, annotationId, contents, metadataPatch, lockTimeoutMs, CommentUpdateMode::Comment);
}

DocsCommentSynchronizer::CommentMetadataUpdateResult DocsCommentSynchronizer::updateSidecarCommentChatGptUrl(const QString &docsRoot,
                                                                                                               const QString &pdfStem,
                                                                                                               const QString &annotationId,
                                                                                                               const QString &url,
                                                                                                               int lockTimeoutMs)
{
    QString error;
    const QString normalized = normalizedChatGptUrl(url, &error);
    if (!error.isEmpty()) {
        CommentMetadataUpdateResult result;
        result.failureReason = error;
        return result;
    }
    const QJsonValue value = normalized.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(normalized);
    return updateSidecarCommentImpl(docsRoot,
                                    pdfStem,
                                    annotationId,
                                    std::nullopt,
                                    QJsonObject {{QStringLiteral("chatgpt_url"), value}},
                                    lockTimeoutMs,
                                    CommentUpdateMode::ChatGptUrl);
}

DocsCommentSynchronizer::CommentDeletionResult DocsCommentSynchronizer::deleteSidecarComments(const QString &docsRoot,
                                                                                                const QString &pdfStem,
                                                                                                const QStringList &annotationIds,
                                                                                                int lockTimeoutMs)
{
    CommentDeletionResult result;
    QElapsedTimer timer;
    timer.start();
    const auto fail = [&result, &timer](const QString &reason) {
        result.failureReason = reason;
        result.totalMs = timer.elapsed();
        return result;
    };
    if (pdfStem.isEmpty() || QFileInfo(pdfStem).fileName() != pdfStem) {
        return fail(QStringLiteral("invalid PDF stem"));
    }

    QStringList stableIds;
    for (QString id : annotationIds) {
        id = id.trimmed();
        if (id.startsWith(QLatin1String("id:"))) {
            id.remove(0, 3);
        }
        if (!id.isEmpty() && !stableIds.contains(id)) {
            stableIds.append(id);
        }
    }
    if (stableIds.isEmpty()) {
        return fail(QStringLiteral("no comment identifiers were provided"));
    }

    const QString lockPath = commentSyncLockPath(docsRoot, pdfStem);
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
        return fail(QStringLiteral("could not create the comment synchronization lock directory"));
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(lockTimeoutMs)) {
        return fail(QStringLiteral("another synchronization is already updating this document"));
    }

    const QString canonicalPath = QDir(docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
    QList<QJsonObject> records;
    QString error;
    if (!reconcileJsonl(canonicalPath, &records, &error)) {
        return fail(QStringLiteral("could not read the canonical sidecar: %1").arg(error));
    }
    const QList<QJsonObject> originalRecords = records;

    QList<int> recordIndexes;
    for (const QString &stableId : std::as_const(stableIds)) {
        int recordIndex = -1;
        for (int index = 0; index < records.size(); ++index) {
            if (!isDeletedRecord(records.at(index)) && stableRecordId(records.at(index)) == stableId) {
                if (recordIndex >= 0) {
                    return fail(QStringLiteral("the comment identifier is ambiguous: %1").arg(stableId));
                }
                recordIndex = index;
            }
        }
        if (recordIndex < 0) {
            return fail(QStringLiteral("the comment could not be found in the canonical sidecar: %1").arg(stableId));
        }
        recordIndexes.append(recordIndex);
    }

    QHash<QString, QPair<QString, bool>> sourceSnapshots;
    const auto restoreSources = [&sourceSnapshots]() {
        QString ignored;
        for (auto iterator = sourceSnapshots.cbegin(); iterator != sourceSnapshots.cend(); ++iterator) {
            writeSourceText(iterator.key(), iterator.value().first, iterator.value().second, &ignored);
        }
    };
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (int offset = 0; offset < recordIndexes.size(); ++offset) {
        const int recordIndex = recordIndexes.at(offset);
        const QJsonObject record = records.at(recordIndex);
        QJsonObject previous = record;
        const QString anchorId = record.value(QStringLiteral("anchor_id")).toString().trimmed();
        const QString texFile = record.value(QStringLiteral("tex_file")).toString().trimmed();
        if (!anchorId.isEmpty() && !texFile.isEmpty()) {
            const QFileInfo texInfo(texFile);
            const QString sourcePath = canonicalOrAbsolutePath(texInfo.isAbsolute() ? texInfo.absoluteFilePath() : QDir(docsRoot).filePath(texFile));
            const QList<DocsAnchorOccurrence> occurrences = anchorOccurrences({sourcePath}).value(anchorId);
            if (occurrences.size() == 1) {
                if (!sourceSnapshots.contains(sourcePath)) {
                    QString text;
                    bool trailingNewline = false;
                    if (!readSourceText(sourcePath, &text, &trailingNewline, &error)) {
                        restoreSources();
                        return fail(QStringLiteral("could not snapshot the comment source anchor: %1").arg(error));
                    }
                    sourceSnapshots.insert(sourcePath, {text, trailingNewline});
                }
                if (!removeOverviewDeletedAnchor(occurrences.constFirst(), anchorId, &error)) {
                    restoreSources();
                    return fail(QStringLiteral("could not remove the comment source anchor: %1").arg(error));
                }
                previous.insert(QStringLiteral("_overview_removed_anchor_path"), sourcePath);
                previous.insert(QStringLiteral("_overview_removed_anchor_line"), occurrences.constFirst().line);
            }
        }
        result.previousRecords.append(previous);
        records[recordIndex] = sidecarTombstone(record, QJsonObject {{QStringLiteral("deleted_at"), timestamp}});
    }

    if (!writeJsonl(canonicalPath, reindexComments(records), &error)) {
        restoreSources();
                return fail(QStringLiteral("could not commit the Overview comment deletion: %1").arg(error));
    }
    for (const QJsonObject &previous : std::as_const(result.previousRecords)) {
        const QString sourcePath = previous.value(QStringLiteral("_overview_removed_anchor_path")).toString();
        const QString anchorId = previous.value(QStringLiteral("anchor_id")).toString();
        if (sourcePath.isEmpty() || anchorId.isEmpty()) {
            continue;
        }
        QList<DocsAnchorOccurrence> remaining = anchorOccurrences({sourcePath}).value(anchorId);
        // A source watcher that was already queued can finish between the
        // source edit and the sidecar commit. Retry once while the canonical
        // tombstone is held under the same document lock.
        if (remaining.size() == 1 && !removeOverviewDeletedAnchor(remaining.constFirst(), anchorId, &error)) {
            QString rollbackError;
            writeJsonl(canonicalPath, originalRecords, &rollbackError);
            restoreSources();
            return fail(QStringLiteral("could not finish removing the comment source anchor: %1").arg(error));
        }
        remaining = anchorOccurrences({sourcePath}).value(anchorId);
        if (!remaining.isEmpty()) {
            QString rollbackError;
            writeJsonl(canonicalPath, originalRecords, &rollbackError);
            restoreSources();
            return fail(QStringLiteral("the comment source anchor remained ambiguous after deletion: %1").arg(anchorId));
        }
    }
    for (QJsonObject &previous : result.previousRecords) {
        const QString sourcePath = previous.value(QStringLiteral("_overview_removed_anchor_path")).toString();
        if (sourcePath.isEmpty() || !sourceSnapshots.contains(sourcePath)) {
            continue;
        }
        QString deletedText;
        bool deletedTrailingNewline = false;
        if (!readSourceText(sourcePath, &deletedText, &deletedTrailingNewline, &error)) {
            QString rollbackError;
            writeJsonl(canonicalPath, originalRecords, &rollbackError);
            restoreSources();
            return fail(QStringLiteral("could not snapshot the deleted comment source state: %1").arg(error));
        }
        const QByteArray deletedBytes = deletedText.toUtf8() + (deletedTrailingNewline ? QByteArray("\n") : QByteArray());
        previous.insert(QStringLiteral("_overview_source_before"), sourceSnapshots.value(sourcePath).first);
        previous.insert(QStringLiteral("_overview_source_before_trailing_newline"), sourceSnapshots.value(sourcePath).second);
        previous.insert(QStringLiteral("_overview_source_after_sha256"), sha256(deletedBytes));
    }
    result.success = true;
    result.changed = true;
    result.deletedIds = stableIds;
    result.totalMs = timer.elapsed();
    return result;
}

DocsCommentSynchronizer::CommentDeletionResult DocsCommentSynchronizer::restoreSidecarComments(const QString &docsRoot,
                                                                                                 const QString &pdfStem,
                                                                                                 const QList<QJsonObject> &previousRecords,
                                                                                                 int lockTimeoutMs)
{
    CommentDeletionResult result;
    QElapsedTimer timer;
    timer.start();
    const auto fail = [&result, &timer](const QString &reason) {
        result.failureReason = reason;
        result.totalMs = timer.elapsed();
        return result;
    };
    if (pdfStem.isEmpty() || QFileInfo(pdfStem).fileName() != pdfStem) {
        return fail(QStringLiteral("invalid PDF stem"));
    }
    if (previousRecords.isEmpty()) {
        return fail(QStringLiteral("no deleted comments were provided"));
    }

    const QString lockPath = commentSyncLockPath(docsRoot, pdfStem);
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
        return fail(QStringLiteral("could not create the comment synchronization lock directory"));
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(lockTimeoutMs)) {
        return fail(QStringLiteral("another synchronization is already updating this document"));
    }

    const QString canonicalPath = QDir(docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
    const QString statePath = QDir(docsRoot).filePath(QStringLiteral("comments/.state/%1.json").arg(pdfStem));
    if (!QDir().mkpath(QFileInfo(statePath).absolutePath())) {
        return fail(QStringLiteral("could not create the comment state directory"));
    }
    QList<QJsonObject> records;
    QJsonObject rawState;
    QString error;
    if (!reconcileJsonl(canonicalPath, &records, &error) || !readJsonObject(statePath, &rawState, &error)) {
        return fail(QStringLiteral("could not read the deleted comment state: %1").arg(error));
    }
    const QList<QJsonObject> deletedRecords = records;
    QJsonObject state = loadedState(rawState);
    QJsonObject storedTombstones = state.value(QStringLiteral("tombstones")).toObject();

    QHash<QString, QPair<QString, bool>> sourceSnapshots;
    const auto restoreSources = [&sourceSnapshots]() {
        QString ignored;
        for (auto iterator = sourceSnapshots.cbegin(); iterator != sourceSnapshots.cend(); ++iterator) {
            writeSourceText(iterator.key(), iterator.value().first, iterator.value().second, &ignored);
        }
    };
    QHash<QString, QJsonObject> sourceRestorations;
    for (const QJsonObject &previous : previousRecords) {
        const QString stableId = stableRecordId(previous);
        const int recordIndex = indexOfStableRecord(records, stableId);
        if (stableId.isEmpty() || recordIndex < 0 || !isDeletedRecord(records.at(recordIndex))) {
            return fail(stableId.isEmpty() ? QStringLiteral("a deleted comment has no stable identifier")
                                           : QStringLiteral("the deleted comment could not be found in the canonical sidecar: %1").arg(stableId));
        }
        const QString sourcePath = previous.value(QStringLiteral("_overview_removed_anchor_path")).toString();
        if (sourcePath.isEmpty()) {
            continue;
        }
        if (sourceRestorations.contains(sourcePath)
            && (sourceRestorations.value(sourcePath).value(QStringLiteral("_overview_source_before")) != previous.value(QStringLiteral("_overview_source_before"))
                || sourceRestorations.value(sourcePath).value(QStringLiteral("_overview_source_after_sha256")) != previous.value(QStringLiteral("_overview_source_after_sha256")))) {
            return fail(QStringLiteral("the deleted comments have inconsistent source history: %1").arg(sourcePath));
        }
        sourceRestorations.insert(sourcePath, previous);
    }
    for (auto iterator = sourceRestorations.cbegin(); iterator != sourceRestorations.cend(); ++iterator) {
        QString currentText;
        bool currentTrailingNewline = false;
        if (!readSourceText(iterator.key(), &currentText, &currentTrailingNewline, &error)) {
            restoreSources();
            return fail(QStringLiteral("could not read the comment source during restoration: %1").arg(error));
        }
        const QByteArray currentBytes = currentText.toUtf8() + (currentTrailingNewline ? QByteArray("\n") : QByteArray());
        if (sha256(currentBytes) != iterator.value().value(QStringLiteral("_overview_source_after_sha256")).toString()) {
            restoreSources();
            return fail(QStringLiteral("the comment source changed after deletion: %1").arg(iterator.key()));
        }
        sourceSnapshots.insert(iterator.key(), {currentText, currentTrailingNewline});
        if (!writeSourceText(iterator.key(),
                             iterator.value().value(QStringLiteral("_overview_source_before")).toString(),
                             iterator.value().value(QStringLiteral("_overview_source_before_trailing_newline")).toBool(),
                             &error)) {
            restoreSources();
            return fail(QStringLiteral("could not restore the exact comment source: %1").arg(error));
        }
    }
    const QString timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    for (QJsonObject previous : previousRecords) {
        const QString stableId = stableRecordId(previous);
        if (stableId.isEmpty()) {
            restoreSources();
            return fail(QStringLiteral("a deleted comment has no stable identifier"));
        }
        const int recordIndex = indexOfStableRecord(records, stableId);
        if (recordIndex < 0 || !isDeletedRecord(records.at(recordIndex))) {
            restoreSources();
            return fail(QStringLiteral("the deleted comment could not be found in the canonical sidecar: %1").arg(stableId));
        }

        for (const QString &field : {
                 QStringLiteral("deleted"),
                 QStringLiteral("deleted_at"),
                 QStringLiteral("deletion_reason"),
                 QStringLiteral("source_anchor_deletion"),
                 QStringLiteral("_overview_removed_anchor_path"),
                 QStringLiteral("_overview_removed_anchor_line"),
                 QStringLiteral("_overview_source_before"),
                 QStringLiteral("_overview_source_before_trailing_newline"),
                 QStringLiteral("_overview_source_after_sha256"),
             }) {
            previous.remove(field);
        }
        previous.insert(QStringLiteral("schema_version"), SidecarSchemaVersion);
        previous.insert(QStringLiteral("revision"), std::max(previous.value(QStringLiteral("revision")).toInt(1), records.at(recordIndex).value(QStringLiteral("revision")).toInt(1)) + 1);
        previous.insert(QStringLiteral("updated_at"), timestamp);
        previous.insert(QStringLiteral("origin_id"), QStringLiteral("mac-overview"));
        records[recordIndex] = previous;
        result.deletedIds.append(stableId);

        const QString key = commentKey(previous);
        const QJsonObject tombstone = storedTombstones.value(key).toObject();
        if (!tombstone.value(QStringLiteral("source_anchor_deletion")).toBool()) {
            storedTombstones.remove(key);
        }
    }

    state.insert(QStringLiteral("tombstones"), storedTombstones);
    if (!writeJsonl(canonicalPath, reindexComments(records), &error)) {
        restoreSources();
        return fail(QStringLiteral("could not restore the Overview comments: %1").arg(error));
    }
    if (!writeJsonObject(statePath, state, &error)) {
        QString rollbackError;
        writeJsonl(canonicalPath, deletedRecords, &rollbackError);
        restoreSources();
        return fail(QStringLiteral("could not restore the Overview comment state: %1").arg(error));
    }

    result.success = true;
    result.changed = true;
    result.previousRecords = previousRecords;
    result.totalMs = timer.elapsed();
    return result;
}

DocsCommentSynchronizer::CommentMutationBatchResult DocsCommentSynchronizer::applySidecarMutations(const QString &docsRoot,
                                                                                                     const QString &pdfStem,
                                                                                                     const QList<QJsonObject> &mutations,
                                                                                                     int lockTimeoutMs)
{
    CommentMutationBatchResult result;
    const auto fail = [&result](const QString &reason) {
        result.failureReason = reason;
        return result;
    };
    if (pdfStem.isEmpty() || QFileInfo(pdfStem).fileName() != pdfStem) {
        return fail(QStringLiteral("invalid PDF stem"));
    }
    if (mutations.isEmpty()) {
        result.success = true;
        return result;
    }

    const QString lockPath = commentSyncLockPath(docsRoot, pdfStem);
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
        return fail(QStringLiteral("could not create the comment synchronization lock directory"));
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(30000);
    if (!lock.tryLock(lockTimeoutMs)) {
        return fail(QStringLiteral("another synchronization is already updating this document"));
    }

    const QString canonicalPath = QDir(docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
    QList<QJsonObject> records;
    QString error;
    if (!reconcileJsonl(canonicalPath, &records, &error)) {
        return fail(QStringLiteral("could not read the canonical sidecar: %1").arg(error));
    }
    QHash<QString, int> indexes;
    for (int index = 0; index < records.size(); ++index) {
        indexes.insert(stableRecordId(records.at(index)), index);
    }

    QHash<QString, QJsonObject> changedById;
    QStringList changedOrder;
    for (const QJsonObject &mutation : mutations) {
        if (mutation.value(QStringLiteral("schema_version")).toInt() != 1) {
            return fail(QStringLiteral("unsupported pending comment mutation schema"));
        }
        const QString operationId = mutation.value(QStringLiteral("operation_id")).toString();
        const QString kind = mutation.value(QStringLiteral("kind")).toString();
        const QString id = mutation.value(QStringLiteral("id")).toString().trimmed();
        if (operationId.isEmpty() || id.isEmpty() || id.size() > 256 || id.contains(QLatin1Char('/')) || id.contains(QLatin1Char('\\'))
            || (kind != QLatin1String("upsert") && kind != QLatin1String("delete") && kind != QLatin1String("restore"))) {
            return fail(QStringLiteral("invalid pending comment mutation"));
        }
        QString timestamp = mutation.value(QStringLiteral("occurred_at")).toString();
        if (!parsePdfDate(timestamp).isValid()) {
            timestamp = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        }
        const int existingIndex = indexes.value(id, -1);
        const std::optional<QJsonObject> previous = existingIndex >= 0 ? std::optional<QJsonObject>(records.at(existingIndex)) : std::nullopt;
        QJsonObject updated;
        if (kind == QLatin1String("delete")) {
            const QString annotationPdfSha256 = mutation.value(QStringLiteral("annotation_pdf_sha256")).toString().toLower();
            static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
            if (!sha256Pattern.match(annotationPdfSha256).hasMatch()) {
                return fail(QStringLiteral("pending comment mutation has no verified PDF identity"));
            }
            if (previous && isDeletedRecord(*previous) && previous->value(QStringLiteral("deleted_at")).toString() == timestamp
                && previous->value(QStringLiteral("annotation_pdf_sha256")).toString() == annotationPdfSha256) {
                updated = *previous;
            } else {
                updated = previous.value_or(mutation.value(QStringLiteral("previous_record")).toObject());
                updated.insert(QStringLiteral("id"), id);
                updated = sidecarTombstone(updated, QJsonObject {{QStringLiteral("deleted_at"), timestamp}});
                updated.insert(QStringLiteral("annotation_pdf_sha256"), annotationPdfSha256);
            }
        } else if (kind == QLatin1String("restore")) {
            updated = mutation.value(QStringLiteral("canonical_record")).toObject();
            if (stableRecordId(updated) != id) {
                return fail(QStringLiteral("pending comment restore has a mismatched identifier"));
            }
            const QString annotationPdfSha256 = mutation.value(QStringLiteral("annotation_pdf_sha256")).toString().toLower();
            static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
            if (!sha256Pattern.match(annotationPdfSha256).hasMatch()) {
                return fail(QStringLiteral("pending comment mutation has no verified PDF identity"));
            }
            if (previous && !isDeletedRecord(*previous) && previous->value(QStringLiteral("updated_at")).toString() == timestamp
                && previous->value(QStringLiteral("annotation_pdf_sha256")).toString() == annotationPdfSha256) {
                updated = *previous;
            } else {
                removePrivateFields(&updated);
                const int previousRevision = previous ? previous->value(QStringLiteral("revision")).toInt(1) : updated.value(QStringLiteral("revision")).toInt(1);
                updated.insert(QStringLiteral("revision"), std::max(1, previousRevision) + 1);
                updated.remove(QStringLiteral("deleted"));
                updated.remove(QStringLiteral("deleted_at"));
                updated.insert(QStringLiteral("annotation_pdf_sha256"), annotationPdfSha256);
            }
        } else {
            const QJsonObject rawRecord = mutation.value(QStringLiteral("record")).toObject();
            if (rawRecord.value(QStringLiteral("id")).toString() != id) {
                return fail(QStringLiteral("pending comment upsert has a mismatched identifier"));
            }
            const QJsonObject snapshot {
                {QStringLiteral("version"), 2},
                {QStringLiteral("coordinate_space"), QStringLiteral("normalized-default-page")},
                {QStringLiteral("source_pdf"), QDir(docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(pdfStem))},
                {QStringLiteral("comments"), QJsonArray {rawRecord}},
            };
            const QList<QJsonObject> converted = commentsFromSnapshot(snapshot, snapshot.value(QStringLiteral("source_pdf")).toString(), &error);
            if (converted.size() != 1) {
                return fail(QStringLiteral("could not normalize the pending comment: %1").arg(error));
            }
            updated = revisionedExternalComment(converted.constFirst(), previous);
            const QJsonObject patch = mutation.value(QStringLiteral("metadata_patch")).toObject();
            bool metadataChanged = false;
            for (auto iterator = patch.begin(); iterator != patch.end(); ++iterator) {
                if (iterator.key() != QLatin1String("priority") && iterator.key() != QLatin1String("location") && iterator.key() != QLatin1String("chatgpt_url")
                    && iterator.key() != QLatin1String("in_reply_to")) {
                    return fail(QStringLiteral("unsupported pending comment metadata field: %1").arg(iterator.key()));
                }
                const QJsonValue oldValue = updated.value(iterator.key());
                if (iterator.value().isNull()) {
                    updated.remove(iterator.key());
                    metadataChanged = metadataChanged || !oldValue.isUndefined();
                } else {
                    QJsonValue newValue = iterator.value();
                    if (iterator.key() == QLatin1String("in_reply_to")) {
                        if (!newValue.isString()) {
                            return fail(QStringLiteral("pending comment reply parent is not a string"));
                        }
                        QString parentId = newValue.toString().trimmed();
                        if (parentId.startsWith(QLatin1String("id:"))) {
                            parentId.remove(0, 3);
                        }
                        if (parentId.isEmpty() || parentId == id) {
                            return fail(QStringLiteral("pending comment reply has an invalid parent"));
                        }
                        if (previous && compactJson(oldValue) != compactJson(QJsonValue(parentId))) {
                            return fail(QStringLiteral("pending comment reply parent cannot be changed"));
                        }
                        const int parentIndex = indexes.value(parentId, -1);
                        if (parentIndex < 0 || isDeletedRecord(records.at(parentIndex))) {
                            return fail(QStringLiteral("pending comment reply parent does not exist"));
                        }
                        newValue = parentId;
                    }
                    updated.insert(iterator.key(), newValue);
                    metadataChanged = metadataChanged || compactJson(oldValue) != compactJson(newValue);
                }
            }
            const QString annotationPdfSha256 = mutation.value(QStringLiteral("annotation_pdf_sha256")).toString().toLower();
            static const QRegularExpression sha256Pattern(QStringLiteral("^[0-9a-f]{64}$"));
            if (!sha256Pattern.match(annotationPdfSha256).hasMatch()) {
                return fail(QStringLiteral("pending comment mutation has no verified PDF identity"));
            }
            updated.insert(QStringLiteral("annotation_pdf_sha256"), annotationPdfSha256);
            if (metadataChanged && previous && updated.value(QStringLiteral("revision")).toInt() <= previous->value(QStringLiteral("revision")).toInt()) {
                updated.insert(QStringLiteral("revision"), std::max(1, previous->value(QStringLiteral("revision")).toInt(1)) + 1);
            }
            updated.remove(QStringLiteral("deleted"));
            updated.remove(QStringLiteral("deleted_at"));
        }
        updated.insert(QStringLiteral("schema_version"), SidecarSchemaVersion);
        updated.insert(QStringLiteral("updated_at"), timestamp);
        updated.insert(QStringLiteral("origin_id"), Thoughts::Platform::desktopOriginId());
        if (existingIndex >= 0) {
            records[existingIndex] = updated;
        } else {
            indexes.insert(id, records.size());
            records.append(updated);
        }
        if (!changedById.contains(id)) {
            changedOrder.append(id);
        }
        changedById.insert(id, updated);
    }

    if (!writeJsonl(canonicalPath, reindexComments(records), &error)) {
        return fail(QStringLiteral("could not commit the pending comments: %1").arg(error));
    }
    for (const QString &id : std::as_const(changedOrder)) {
        result.changedRecords.append(changedById.value(id));
    }
    QString visibilityError;
    visibleSidecarComments(docsRoot, pdfStem, &result.visibleFingerprint, &visibilityError);
    if (!visibilityError.isEmpty()) {
        return fail(QStringLiteral("could not fingerprint the committed comments: %1").arg(visibilityError));
    }
    result.success = true;
    return result;
}

QString DocsCommentSynchronizer::normalizedChatGptUrl(const QString &url, QString *error)
{
    if (error) {
        error->clear();
    }
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    const QUrl parsed(trimmed, QUrl::StrictMode);
    const QString host = parsed.host().toLower();
    const bool chatGptHost = host == QLatin1String("chatgpt.com") || host.endsWith(QLatin1String(".chatgpt.com"));
    if (!parsed.isValid() || parsed.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) != 0 || !chatGptHost || !parsed.userName().isEmpty() || !parsed.password().isEmpty()) {
        if (error) {
            *error = QStringLiteral("Enter a valid HTTPS chatgpt.com link.");
        }
        return {};
    }
    return parsed.toString(QUrl::FullyEncoded);
}

DocsCommentSynchronizer::Result DocsCommentSynchronizer::synchronize(const QUrl &documentUrl, const QJsonObject &annotationSnapshot)
{
    if (documentUrl.isLocalFile()) {
        const QFileInfo pdfInfo(documentUrl.toLocalFile());
        const QString currentDir = QFileInfo(QDir(m_options.docsRoot).filePath(QStringLiteral("current_pdfs"))).absoluteFilePath();
        if (pdfInfo.absolutePath() == currentDir && pdfInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) == 0) {
            const SourceAnchorRestorationBatchResult restoration = restoreReappearedSourceAnchors(m_options, canonicalStem(pdfInfo));
            if (!restoration.success) {
                Result failure;
                failure.failureReason = restoration.failureReason;
                failure.logLines = restoration.logLines;
                failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
                return failure;
            }
            Result result = synchronize(documentUrl, annotationSnapshot, false, nullptr);
            result.logLines = restoration.logLines + result.logLines;
            return result;
        }
    }
    return synchronize(documentUrl, annotationSnapshot, false, nullptr);
}

DocsCommentSynchronizer::Result DocsCommentSynchronizer::reconcileSourceAnchorChanges(const QUrl &documentUrl)
{
    Result result;
    QElapsedTimer totalTimer;
    totalTimer.start();
    const auto fail = [&result, &totalTimer](const QString &reason) {
        result.failureReason = reason;
        result.totalMs = totalTimer.elapsed();
        result.logLines.append(QStringLiteral("error: %1").arg(reason));
        return result;
    };

    if (!documentUrl.isLocalFile()) {
        return fail(QStringLiteral("source reconciliation requires a local PDF"));
    }
    QFileInfo pdfInfo(documentUrl.toLocalFile());
    const QString currentDir = QFileInfo(QDir(m_options.docsRoot).filePath(QStringLiteral("current_pdfs"))).absoluteFilePath();
    if (pdfInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0) {
        return fail(QStringLiteral("source reconciliation requires a PDF in current_pdfs"));
    }
    if (pdfInfo.absolutePath() != currentDir) {
        // A Library PDF opened from its Project folder, e.g. one that arrived
        // seconds ago and is not in current_pdfs yet. Its current copy has the
        // same name; until that copy exists there is nothing to reconcile.
        const QFileInfo current(QDir(currentDir).filePath(pdfInfo.fileName()));
        if (!current.isFile()) {
            // Incomplete: the watches already set for this PDF are kept.
            result.success = true;
            result.logLines.append(QStringLiteral("skipped: %1 is not in current_pdfs yet").arg(pdfInfo.fileName()));
            result.totalMs = totalTimer.elapsed();
            return result;
        }
        pdfInfo = current;
    }

    const QString stem = canonicalStem(pdfInfo);
    const SourceAnchorRestorationBatchResult restoration = restoreReappearedSourceAnchors(m_options, stem);
    if (!restoration.success) {
        return fail(restoration.failureReason);
    }
    result.logLines.append(restoration.logLines);
    const QString commentsRoot = QDir(m_options.docsRoot).filePath(QStringLiteral("comments"));
    const QString canonicalPath = QDir(commentsRoot).filePath(stem + QStringLiteral("_comments.jsonl"));
    const QString resolutionPath = QDir(commentsRoot).filePath(QStringLiteral("review_resolutions.jsonl"));
    const QString statePath = QDir(commentsRoot).filePath(QStringLiteral(".state/") + stem + QStringLiteral(".json"));
    const QString lockPath = commentSyncLockPath(m_options.docsRoot, stem);
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
        return fail(QStringLiteral("could not create the synchronization lock directory"));
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(300000);
    if (!lock.tryLock(m_options.lockTimeoutMs)) {
        return fail(QStringLiteral("another synchronization is already updating this document"));
    }

    QElapsedTimer phaseTimer;
    phaseTimer.start();
    QString error;
    QList<QJsonObject> previousRecords;
    QList<QJsonObject> resolutionRecords;
    QJsonObject rawState;
    int sidecarConflicts = 0;
    int resolutionConflicts = 0;
    if (!reconcileJsonl(canonicalPath, &previousRecords, &error, &sidecarConflicts) || !reconcileJsonl(resolutionPath, &resolutionRecords, &error, &resolutionConflicts) || !readJsonObject(statePath, &rawState, &error)) {
        return fail(QStringLiteral("could not read comment state: %1").arg(error));
    }
    QJsonObject state = loadedState(rawState);
    QList<QJsonObject> previousDeletedRecords;
    for (const QJsonObject &record : std::as_const(previousRecords)) {
        if (isDeletedRecord(record)) {
            previousDeletedRecords.append(record);
        }
    }
    bool resolutionRegistryDirty = importSidecarTombstones(&state, &resolutionRecords, previousDeletedRecords);
    result.loadMs = phaseTimer.elapsed();

    phaseTimer.restart();
    const QSet<QString> sourceFiles = mappedSourceFilesForDocument(m_options.docsRoot, stem, previousRecords, &error);
    if (!error.isEmpty()) {
        return fail(QStringLiteral("could not resolve document sources: %1").arg(error));
    }
    DocsAnchorIndex anchorIndex;
    anchorIndex.occurrences = anchorOccurrences(sourceFiles);

    QList<QJsonObject> reconciledRecords;
    reconciledRecords.reserve(previousRecords.size());
    bool canonicalDirty = false;
    int skippedDeletedAnchors = 0;
    QJsonObject mirrorStates = state.value(QStringLiteral("source_mirrors")).toObject();
    for (const QJsonObject &record : std::as_const(previousRecords)) {
        if (isDeletedRecord(record)) {
            const QString key = commentKey(record);
            mirrorStates.remove(key);
            const QString anchorId = record.value(QStringLiteral("anchor_id")).toString().trimmed();
            const QList<DocsAnchorOccurrence> mapped = anchorIndex.occurrences.value(anchorId);
            if (!anchorId.isEmpty() && mapped.size() == 1) {
                if (!anchorIndex.complete) {
                    bool allFilesRead = false;
                    DocsAnchorOccurrences completeOccurrences = anchorOccurrences(managedSourceFiles(m_options.docsRoot), &allFilesRead);
                    if (!allFilesRead) {
                        ++skippedDeletedAnchors;
                        result.logLines.append(QStringLiteral("warning: could not verify unique deleted anchor %1 across all managed sources").arg(anchorId));
                    } else {
                        anchorIndex.occurrences = std::move(completeOccurrences);
                        anchorIndex.complete = true;
                    }
                }
                const QList<DocsAnchorOccurrence> found = anchorIndex.occurrences.value(anchorId);
                if (anchorIndex.complete && found.size() == 1) {
                    if (!removeAnchor(found.constFirst(), anchorId, &error)) {
                        return fail(QStringLiteral("could not remove deleted anchor: %1").arg(error));
                    }
                    refreshAnchorOccurrencesForSource(&anchorIndex.occurrences, found.constFirst().path);
                    ++result.anchorsRemoved;
                } else if (anchorIndex.complete) {
                    ++skippedDeletedAnchors;
                    result.logLines.append(QStringLiteral("warning: deleted anchor %1 has %2 managed-source matches; leaving source unchanged").arg(anchorId).arg(found.size()));
                }
            } else if (!anchorId.isEmpty() && mapped.size() > 1) {
                ++skippedDeletedAnchors;
                result.logLines.append(QStringLiteral("warning: deleted anchor %1 has %2 mapped-source matches; leaving source unchanged").arg(anchorId).arg(mapped.size()));
            }
            reconciledRecords.append(record);
            continue;
        }

        const QString key = commentKey(record);
        if (!isTrackedCommentRecordImpl(record)) {
            QJsonObject updated = record;
            bool metadataCanBeCleared = false;
            if (!removePlainMarkupAnchor(m_options, &anchorIndex, record, &result, &metadataCanBeCleared, &error)) {
                return fail(QStringLiteral("could not remove legacy plain-markup anchor: %1").arg(error));
            }
            if (metadataCanBeCleared) {
                clearAnchorMetadata(&updated);
                mirrorStates.remove(key);
            }
            canonicalDirty = canonicalDirty || compactJson(updated) != compactJson(record);
            reconciledRecords.append(updated);
            ++result.plainMarkups;
            continue;
        }
        if (sourceAnchorWasRemoved(m_options.docsRoot, &anchorIndex, record)) {
            QJsonObject orphaned = record;
            markSidecarAnchorRemoved(&orphaned, QStringLiteral("mac-tex-anchor"));
            reconciledRecords.append(orphaned);
            canonicalDirty = true;
            mirrorStates.remove(key);
            ++result.sourceAnchorsOrphaned;
            result.logLines.append(QStringLiteral("source anchor %1 was removed; the comment is kept as anchor removed").arg(record.value(QStringLiteral("anchor_id")).toString()));
            continue;
        }

        QJsonObject updated = record;
        const QString anchorId = record.value(QStringLiteral("anchor_id")).toString().trimmed();
        const QList<DocsAnchorOccurrence> found = anchorIndex.occurrences.value(anchorId);
        if (!anchorId.isEmpty() && found.size() == 1) {
            const SourceMirror source = sourceMirrorAt(found.constFirst(), anchorId, &error);
            if (!source.valid) {
                return fail(QStringLiteral("could not read anchor comment mirror: %1").arg(error));
            }
            const QJsonObject baseline = mirrorStates.value(key).toObject();
            const bool hasBaseline = hasUsableSourceMirrorState(baseline, anchorId);
            const QString baselineHash = baseline.value(QStringLiteral("contents_hash")).toString();
            const QString canonicalHash = reviewContentsHash(record.value(QStringLiteral("contents")).toString());
            const QString sourceHash = source.hasMirror ? reviewContentsHash(source.contents) : QString();
            const bool sourceChanged = hasBaseline && source.hasMirror && sourceHash != baselineHash;
            const bool canonicalChanged =
                hasBaseline && (canonicalHash != baselineHash || record.value(QStringLiteral("revision")).toInt(1) != baseline.value(QStringLiteral("canonical_revision")).toInt(1));

            bool importSource = sourceChanged;
            if (sourceChanged && canonicalChanged) {
                importSource = true;
            }

            if (importSource) {
                const QDateTime sourceTimestamp = source.modifiedMs == std::numeric_limits<qint64>::min() ? QDateTime::currentDateTimeUtc() : QDateTime::fromMSecsSinceEpoch(source.modifiedMs, QTimeZone::UTC);
                const QString timestamp = sourceTimestamp.toString(Qt::ISODateWithMs);
                updated.insert(QStringLiteral("contents"), source.contents);
                updated.insert(QStringLiteral("modified"), timestamp);
                updated.insert(QStringLiteral("updated_at"), timestamp);
                updated.insert(QStringLiteral("revision"), std::max(1, record.value(QStringLiteral("revision")).toInt(1)) + 1);
                updated.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-anchor"));
                updated.remove(QStringLiteral("deleted"));
                updated.remove(QStringLiteral("deleted_at"));
                ++result.sourceCommentEditsImported;
            }
            bool mirrorUpdated = false;
            if (!rewriteAnchorMirror(found.constFirst(), anchorId, updated.value(QStringLiteral("contents")).toString(), &mirrorUpdated, &error)) {
                return fail(QStringLiteral("could not update anchor comment mirror: %1").arg(error));
            }
            result.anchorMirrorsUpdated += mirrorUpdated ? 1 : 0;
            QString restoredMarkupSubtype = updated.value(QStringLiteral("markup_subtype")).toString();
            const bool unresolvedMarkup = isTextMarkupSubtype(restoredMarkupSubtype)
                && (updated.value(QStringLiteral("subtype")).toString() == QLatin1String("Text")
                    || updated.value(QStringLiteral("markup_status")).toString() == QLatin1String("structural_fallback")
                    || updated.value(QStringLiteral("markup_status")).toString() == QLatin1String("needs_attention"));
            if (unresolvedMarkup) {
                updated.insert(QStringLiteral("subtype"), restoredMarkupSubtype);
                updated.insert(QStringLiteral("markup_status"), QStringLiteral("needs_attention"));
                updated.insert(QStringLiteral("quad_points"), QJsonValue::Null);
                updated.remove(QStringLiteral("fragments"));
                updated.insert(QStringLiteral("anchor_status"), QStringLiteral("needs_anchor"));
            } else {
                updated.insert(QStringLiteral("anchor_status"), QStringLiteral("anchored"));
            }
            clearSidecarOrphanReason(&updated);
            QString anchorKind = updated.value(QStringLiteral("anchor_kind")).toString();
            const bool mediumConfidenceAnchor =
                anchorKind == QLatin1String("standalone_selected_text") || anchorKind == QLatin1String("structural")
                || (updated.value(QStringLiteral("anchor_confidence")).toString() == QLatin1String("medium")
                    && updated.value(QStringLiteral("tex_column")).toInt() == -1);
            updated.insert(QStringLiteral("anchor_confidence"),
                           unresolvedMarkup ? QStringLiteral("medium")
                                            : (anchorKind == QLatin1String("document_inbox")
                                                   ? QStringLiteral("low")
                                                   : (mediumConfidenceAnchor ? QStringLiteral("medium") : QStringLiteral("high"))));
            updated.insert(QStringLiteral("tex_file"), relativeToDocs(found.constFirst().path, m_options.docsRoot));
            updated.insert(QStringLiteral("tex_line"), found.constFirst().line);
            const SourceMirror resolvedSource = sourceMirrorAt(found.constFirst(), anchorId, &error);
            if (!resolvedSource.valid || !resolvedSource.hasMirror) {
                return fail(QStringLiteral("could not verify anchor comment mirror: %1").arg(error));
            }
            mirrorStates.insert(key, sourceMirrorState(key, updated, resolvedSource));
        } else if (!anchorId.isEmpty() && found.size() > 1) {
            updated.insert(QStringLiteral("anchor_status"), QStringLiteral("orphaned"));
            updated.insert(QStringLiteral("anchor_confidence"), QStringLiteral("low"));
            mirrorStates.remove(key);
        } else {
            mirrorStates.remove(key);
        }
        canonicalDirty = canonicalDirty || compactJson(updated) != compactJson(record);
        reconciledRecords.append(updated);
    }
    state.insert(QStringLiteral("source_mirrors"), mirrorStates);
    result.anchorMs = phaseTimer.elapsed();

    phaseTimer.restart();
    const bool stateDirty = compactJson(state) != compactJson(loadedState(rawState));
    if ((canonicalDirty && !writeJsonl(canonicalPath, reconciledRecords, &error)) || (stateDirty && !writeJsonObject(statePath, state, &error)) || (resolutionRegistryDirty && !writeJsonl(resolutionPath, resolutionRecords, &error))) {
        return fail(QStringLiteral("could not commit source reconciliation: %1").arg(error));
    }
    result.writeMs = phaseTimer.elapsed();
    result.visibleComments = 0;
    result.plainMarkups = 0;
    for (const QJsonObject &record : liveRecords(reconciledRecords)) {
        if (isTrackedCommentRecordImpl(record)) {
            ++result.visibleComments;
        } else {
            ++result.plainMarkups;
        }
    }
    result.deletions = result.sourceAnchorDeletions;
    QString finalSourceError;
    const QSet<QString> finalSourceFiles = mappedSourceFilesForDocument(m_options.docsRoot, stem, reconciledRecords, &finalSourceError);
    const QSet<QString> &watchedSourceFiles = finalSourceError.isEmpty() ? finalSourceFiles : sourceFiles;
    result.watchedSourceFiles = QStringList(watchedSourceFiles.begin(), watchedSourceFiles.end());
    std::sort(result.watchedSourceFiles.begin(), result.watchedSourceFiles.end());
    result.watchedSourceFilesComplete = finalSourceError.isEmpty();
    if (!finalSourceError.isEmpty()) {
        result.logLines.append(QStringLiteral("warning: could not refresh reconciled source paths: %1").arg(finalSourceError));
    }
    QString fingerprintError;
    visibleSidecarComments(m_options.docsRoot, stem, &result.visibleSidecarFingerprint, &fingerprintError);
    if (!fingerprintError.isEmpty()) {
        result.visibleSidecarFingerprint.clear();
        result.logLines.append(QStringLiteral("warning: could not fingerprint reconciled comments: %1").arg(fingerprintError));
    }
    result.totalMs = totalTimer.elapsed();
    result.success = true;
    result.logLines.append(QStringLiteral("native source reconciliation: comments=%1 markup=%2 source-anchors-orphaned=%3 source-text-imports=%4 mirror-updates=%5 watched-sources=%6 removed=%7 removal-skips=%8")
                               .arg(result.visibleComments)
                               .arg(result.plainMarkups)
                               .arg(result.sourceAnchorsOrphaned)
                               .arg(result.sourceCommentEditsImported)
                               .arg(result.anchorMirrorsUpdated)
                               .arg(sourceFiles.size())
                               .arg(result.anchorsRemoved)
                               .arg(skippedDeletedAnchors));
    result.logLines.append(QStringLiteral("source protocol: sidecar-conflicts=%1 resolution-conflicts=%2").arg(sidecarConflicts).arg(resolutionConflicts));
    result.logLines.append(QStringLiteral("native source timing: load-ms=%1 anchors-ms=%2 write-ms=%3 total-ms=%4").arg(result.loadMs).arg(result.anchorMs).arg(result.writeMs).arg(result.totalMs));
    return result;
}

DocsCommentSynchronizer::Result DocsCommentSynchronizer::synchronize(const QUrl &documentUrl,
                                                                     const QJsonObject &annotationSnapshot,
                                                                     bool prepareUnanchored,
                                                                     DocsAnchorIndex *sharedAnchorIndex,
                                                                     const QString &onlyAnnotationId,
                                                                     const QString &canonicalPdfStem)
{
    Result result;
    QElapsedTimer totalTimer;
    totalTimer.start();
    const auto fail = [&result, &totalTimer](const QString &reason) {
        result.failureReason = reason;
        result.totalMs = totalTimer.elapsed();
        result.logLines.append(QStringLiteral("error: %1").arg(reason));
        return result;
    };

    if (!documentUrl.isLocalFile()) {
        return fail(QStringLiteral("comment synchronization requires a local PDF"));
    }
    const QFileInfo pdfInfo(documentUrl.toLocalFile());
    const QString pdfPath = pdfInfo.absoluteFilePath();
    const QString currentDir = QFileInfo(QDir(m_options.docsRoot).filePath(QStringLiteral("current_pdfs"))).absoluteFilePath();
    const QString role = QStringLiteral("current");
    if (canonicalPdfStem.isEmpty()
        && (pdfInfo.absolutePath() != currentDir || pdfInfo.suffix().compare(QLatin1String("pdf"), Qt::CaseInsensitive) != 0)) {
        return fail(QStringLiteral("saved PDF is outside current_pdfs"));
    }
    const QString stem = canonicalPdfStem.isEmpty() ? canonicalStem(pdfInfo) : canonicalPdfStem;
    const QString commentsRoot = QDir(m_options.docsRoot).filePath(QStringLiteral("comments"));
    const QString canonicalPath = QDir(commentsRoot).filePath(stem + QStringLiteral("_comments.jsonl"));
    const QString resolutionPath = QDir(commentsRoot).filePath(QStringLiteral("review_resolutions.jsonl"));
    const QString statePath = QDir(commentsRoot).filePath(QStringLiteral(".state/") + stem + QStringLiteral(".json"));
    const QString lockPath = commentSyncLockPath(m_options.docsRoot, stem);
    if (!QDir().mkpath(QFileInfo(lockPath).absolutePath())) {
        return fail(QStringLiteral("could not create the synchronization lock directory"));
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(300000);
    if (!lock.tryLock(m_options.lockTimeoutMs)) {
        return fail(QStringLiteral("another synchronization is already updating this document"));
    }

    QElapsedTimer phaseTimer;
    phaseTimer.start();
    QString error;
    QList<QJsonObject> previousRecords;
    QList<QJsonObject> previousCanonical;
    QList<QJsonObject> previousDeletedRecords;
    QList<QJsonObject> resolutionRecords;
    QJsonObject rawState;
    int sidecarConflicts = 0;
    int resolutionConflicts = 0;
    if (!reconcileJsonl(canonicalPath, &previousRecords, &error, &sidecarConflicts) || !reconcileJsonl(resolutionPath, &resolutionRecords, &error, &resolutionConflicts) || !readJsonObject(statePath, &rawState, &error)) {
        return fail(QStringLiteral("could not read comment state: %1").arg(error));
    }
    previousCanonical = liveRecords(previousRecords);
    for (const QJsonObject &record : previousRecords) {
        if (isDeletedRecord(record)) {
            previousDeletedRecords.append(record);
        }
    }
    QJsonObject state = loadedState(rawState);
    bool resolutionRegistryDirty = importSidecarTombstones(&state, &resolutionRecords, previousDeletedRecords);
    QList<QJsonObject> snapshotComments = commentsFromSnapshot(annotationSnapshot, pdfPath, &error);
    if (!error.isEmpty()) {
        return fail(error);
    }

    const KeyedComments previousKeyed = keyedComments(previousCanonical);
    QHash<QString, QJsonObject> previousFull;
    for (const QString &key : previousKeyed.order) {
        previousFull.insert(key, previousKeyed.values.value(key));
    }

    QList<QJsonObject> comments;
    QList<QJsonObject> newTombstones;
    const QList<QJsonObject> publicResolutionTombstones = resolutionTombstones(resolutionRecords);
    // A non-deleted canonical sidecar record is authoritative over an older
    // local observation tombstone. This situation occurs when a published PDF
    // legitimately contains the comment again; keeping the stale state entry
    // would suppress the snapshot forever and prevent anchor preparation.
    QJsonObject localTombstones = state.value(QStringLiteral("tombstones")).toObject();
    for (const QString &key : previousKeyed.order) {
        const QJsonObject tombstone = localTombstones.value(key).toObject();
        if (!tombstone.isEmpty() && !tombstone.value(QStringLiteral("source_anchor_deletion")).toBool()) {
            localTombstones.remove(key);
        }
    }
    state.insert(QStringLiteral("tombstones"), localTombstones);
    const QList<QJsonObject> persistedTombstones = stateTombstones(state) + publicResolutionTombstones;
    snapshotComments = applyTombstones(snapshotComments, persistedTombstones);
    comments = overlayToolMetadata(snapshotComments, previousCanonical);
    newTombstones = observeSource(&state, role, pdfPath, comments, previousFull, true);
    result.loadMs = phaseTimer.elapsed();

    phaseTimer.restart();
    QString attemptPath;
    QJsonObject attemptRoot = attemptCache(m_options.docsRoot, pdfPath, &attemptPath);
    QJsonObject attempts = attemptRoot.value(QStringLiteral("attempts")).toObject();
    QSet<QString> sourceFiles;
    DocsAnchorIndex localAnchorIndex;
    if (!sharedAnchorIndex) {
        sourceFiles = sourceFilesForAnchors(m_options.docsRoot, stem, previousCanonical + comments);
        localAnchorIndex.occurrences = anchorOccurrences(sourceFiles);
    }
    DocsAnchorIndex &anchorIndex = sharedAnchorIndex ? *sharedAnchorIndex : localAnchorIndex;
    DocsAnchorOccurrences &occurrences = anchorIndex.occurrences;
    QHash<int, SourceLocation> sourceLocations;
    const auto sourceForPage = [&](int pageNumber) {
        if (!sourceLocations.contains(pageNumber)) {
            sourceLocations.insert(pageNumber, sourceLocationForPage(m_options.docsRoot, stem, pageNumber));
        }
        return sourceLocations.value(pageNumber);
    };
    const auto ensureBuildRegistry = [this]() {
        const QString buildsRoot = QDir(m_options.docsRoot).filePath(QStringLiteral(".runtime/comment_anchors/builds"));
        const qint64 modifiedMs = QFileInfo(buildsRoot).lastModified().toMSecsSinceEpoch();
        if (!m_cache->buildsLoaded || m_cache->docsRoot != m_options.docsRoot || m_cache->buildsRootModifiedMs != modifiedMs) {
            m_cache->builds = cachedBuildRegistry(m_options.docsRoot);
            m_cache->docsRoot = m_options.docsRoot;
            m_cache->buildsRootModifiedMs = modifiedMs;
            m_cache->buildsLoaded = true;
        }
    };
    const auto buildLookup = [this, &ensureBuildRegistry](const SourceLocation &source) {
        ensureBuildRegistry();
        return buildForSource(m_cache->builds, source);
    };
    QString currentPdfSha256;
    bool currentPdfSha256Loaded = false;
    const auto historicalLookup = [this, &ensureBuildRegistry, &stem, &pdfPath, &currentPdfSha256, &currentPdfSha256Loaded](const QJsonObject &comment) {
        const QString annotationPdfSha256 = comment.value(QStringLiteral("annotation_pdf_sha256")).toString().trimmed();
        if (!annotationPdfSha256.isEmpty()) {
            if (!currentPdfSha256Loaded) {
                currentPdfSha256 = fileSha256(pdfPath);
                currentPdfSha256Loaded = true;
            }
            if (annotationPdfSha256 == currentPdfSha256) {
                return HistoricalBuildResolution {};
            }
        }
        ensureBuildRegistry();
        return historicalBuildForComment(m_options.docsRoot, stem, comment, m_cache->builds);
    };

    const auto publishedLookup = [this, &stem, &pdfPath](const QJsonObject &comment) {
        const QJsonArray rect = comment.value(QStringLiteral("_normalized_rect")).toArray();
        if (rect.size() != 4) {
            SourceResolutionResult result;
            result.failureReason = QStringLiteral("The original highlight geometry is unavailable");
            return result;
        }
        return resolveSource({.pdfStem = stem,
            .pageNumber = comment.value(QStringLiteral("page")).toInt(),
            .x = (rect.at(0).toDouble() + rect.at(2).toDouble()) * 0.5 * comment.value(QStringLiteral("_page_width")).toDouble(),
            .y = (rect.at(1).toDouble() + rect.at(3).toDouble()) * 0.5 * comment.value(QStringLiteral("_page_height")).toDouble(),
            .selectedText = comment.value(QStringLiteral("selected_text")).toString(),
            .pdfPath = pdfPath, .requireExactPassage = true});
    };

    // A placement that disappeared from the PDF snapshot is an explicit user
    // deletion even when its source marker is also gone; it stays an ordinary
    // tombstone (explicit delete always wins over anchor-removed detection).
    QList<QJsonObject> synchronized;
    synchronized.reserve(comments.size());
    QSet<QString> sourceTextImports;
    for (QJsonObject comment : comments) {
        const QString key = commentKey(comment);
        const std::optional<QJsonObject> previous = previousFull.contains(key) ? std::optional<QJsonObject>(previousFull.value(key)) : std::nullopt;
        if (!onlyAnnotationId.isEmpty() && stableRecordId(comment) != onlyAnnotationId) {
            synchronized.append(previous.value_or(comment));
            continue;
        }
        if (!isTrackedCommentRecordImpl(comment)) {
            bool metadataCanBeCleared = false;
            const QJsonObject anchorBearingRecord = previous && !previous->value(QStringLiteral("anchor_id")).toString().trimmed().isEmpty() ? *previous : comment;
            if (!removePlainMarkupAnchor(m_options, &anchorIndex, anchorBearingRecord, &result, &metadataCanBeCleared, &error)) {
                return fail(QStringLiteral("could not remove legacy plain-markup anchor: %1").arg(error));
            }
            if (metadataCanBeCleared) {
                clearAnchorMetadata(&comment);
                attempts.remove(key);
                QJsonObject mirrorStates = state.value(QStringLiteral("source_mirrors")).toObject();
                mirrorStates.remove(key);
                state.insert(QStringLiteral("source_mirrors"), mirrorStates);
            }
            synchronized.append(comment);
            ++result.plainMarkups;
            continue;
        }
        if (sourceAnchorWasRemoved(m_options.docsRoot, &anchorIndex, comment)) {
            // Keep the comment; synchronizeAnchor keeps the orphaned status sticky
            // while the geometry is unchanged and re-anchors if the marker returns.
            markSidecarAnchorRemoved(&comment, QStringLiteral("mac-tex-anchor"));
            attempts.remove(key);
            ++result.sourceAnchorsOrphaned;
            result.logLines.append(QStringLiteral("source anchor %1 was removed; the comment is kept as anchor removed").arg(comment.value(QStringLiteral("anchor_id")).toString()));
        }
        bool sourceTextImported = false;
        if (!applyLatestReviewText(&comment, previous, state, canonicalPath, occurrences, &sourceTextImported, &error)) {
            return fail(QStringLiteral("could not reconcile source comment text: %1").arg(error));
        }
        if (sourceTextImported) {
            sourceTextImports.insert(key);
            ++result.sourceCommentEditsImported;
        }
        bool attempted = false;
        bool inserted = false;
        bool mirrorUpdated = false;
        bool lookupRequired = false;
        if (!synchronizeAnchor(m_options, occurrences, sourceForPage, buildLookup, historicalLookup, publishedLookup, &comment, previous, &attempts, prepareUnanchored, &error, &lookupRequired, &attempted, &inserted, &mirrorUpdated)) {
            return fail(error);
        }
        if (comment.value(QStringLiteral("anchor_status")).toString() == QLatin1String("anchored")) {
            comment.remove(QStringLiteral("anchor_error_code"));
            comment.remove(QStringLiteral("markup_status"));
        }
        result.anchorCandidates += lookupRequired ? 1 : 0;
        result.anchorAttempts += attempted ? 1 : 0;
        result.anchorsInserted += inserted ? 1 : 0;
        result.anchorMirrorsUpdated += mirrorUpdated ? 1 : 0;
        result.unchangedComments += previous && !lookupRequired ? 1 : 0;
        synchronized.append(comment);
        if (inserted) {
            // Inserting or moving a marker shifts every later line in this
            // source. Refresh just that file so the shared batch index stays
            // exact without rescanning unrelated projects.
            const QString texFile = comment.value(QStringLiteral("tex_file")).toString();
            const QFileInfo texInfo(texFile);
            const QString path = texInfo.isAbsolute() ? texInfo.absoluteFilePath() : QDir(m_options.docsRoot).filePath(texFile);
            refreshAnchorOccurrencesForSource(&occurrences, path);
        }
    }
    result.deletions = newTombstones.size();

    for (const QJsonObject &tombstone : newTombstones) {
        const QJsonObject previous = tombstone.value(QStringLiteral("previous_annotation")).toObject();
        const QString anchorId = previous.value(QStringLiteral("anchor_id")).toString();
        const QList<DocsAnchorOccurrence> found = occurrences.value(anchorId);
        if (!anchorId.isEmpty() && found.size() == 1) {
            if (!removeAnchor(found.constFirst(), anchorId, &error)) {
                return fail(QStringLiteral("could not remove deleted anchor: %1").arg(error));
            }
            refreshAnchorOccurrencesForSource(&occurrences, found.constFirst().path);
            ++result.anchorsRemoved;
        }
    }
    result.anchorMs = phaseTimer.elapsed();

    phaseTimer.restart();
    QList<QJsonObject> cleanComments;
    cleanComments.reserve(synchronized.size());
    for (QJsonObject comment : synchronized) {
        removePrivateFields(&comment);
        cleanComments.append(comment);
    }
    cleanComments = reindexComments(cleanComments);

    QList<QJsonObject> canonicalComments;
    canonicalComments.reserve(cleanComments.size() + previousCanonical.size());
    QSet<QString> visibleKeys;
    for (const QJsonObject &comment : cleanComments) {
        const QString key = commentKey(comment);
        visibleKeys.insert(key);
        QJsonObject revisioned = revisionedExternalComment(comment, previousFull.contains(key) ? std::optional<QJsonObject>(previousFull.value(key)) : std::nullopt);
        if (sourceTextImports.contains(key)) {
            revisioned.insert(QStringLiteral("origin_id"), QStringLiteral("mac-tex-anchor"));
        }
        canonicalComments.append(revisioned);
    }
    QSet<QString> deletedKeys;
    for (const QJsonObject &tombstone : newTombstones + publicResolutionTombstones) {
        deletedKeys.insert(tombstone.value(QStringLiteral("comment_key")).toString());
    }
    // The sidecar is authoritative. The saved PDF is one source observation:
    // preserve records created by MJ PDF (or any other source) that this exact
    // PDF never contained. Only an observed same-source disappearance or a
    // public source-anchor resolution may remove one.
    for (const QJsonObject &old : previousCanonical) {
        const QString key = commentKey(old);
        if (!visibleKeys.contains(key) && !deletedKeys.contains(key)) {
            canonicalComments.append(old);
            visibleKeys.insert(key);
        }
    }
    canonicalComments = reindexComments(canonicalComments);

    QHash<QString, QJsonObject> deletedByKey;
    QStringList deletedOrder;
    for (const QJsonObject &record : previousDeletedRecords) {
        const QString key = commentKey(record);
        if (!deletedByKey.contains(key)) {
            deletedOrder.append(key);
        }
        deletedByKey.insert(key, record);
    }
    for (const QJsonObject &tombstone : newTombstones) {
        QJsonObject previous = tombstone.value(QStringLiteral("previous_annotation")).toObject();
        const QString key = tombstone.value(QStringLiteral("comment_key")).toString();
        if (previous.isEmpty() && previousFull.contains(key)) {
            previous = previousFull.value(key);
        }
        if (previous.isEmpty() || key.isEmpty()) {
            continue;
        }
        if (!deletedByKey.contains(key)) {
            deletedOrder.append(key);
        }
        deletedByKey.insert(key, sidecarTombstone(previous, tombstone));
    }
    QSet<QString> ownedResolutionKeys;
    for (const QJsonObject &record : previousCanonical) {
        const QString declaredOwner = canonicalStemForRecordSourcePdf(m_options.docsRoot, record);
        if (declaredOwner.isEmpty() || declaredOwner == stem) {
            ownedResolutionKeys.insert(commentKey(record));
        }
    }
    for (const QJsonObject &record : snapshotComments) {
        ownedResolutionKeys.insert(commentKey(record));
    }
    for (const QJsonObject &resolution : resolutionRecords) {
        if (!isDeletedRecord(resolution) || !resolution.value(QStringLiteral("source_anchor_deletion")).toBool()) {
            continue;
        }
        const QString key = resolution.value(QStringLiteral("comment_key")).toString(commentKey(resolution));
        const QString declaredOwner = canonicalStemForRecordSourcePdf(m_options.docsRoot, resolution);
        if (key.isEmpty() || (!declaredOwner.isEmpty() ? declaredOwner != stem : !ownedResolutionKeys.contains(key))) {
            continue;
        }
        if (!deletedByKey.contains(key)) {
            deletedOrder.append(key);
        }
        deletedByKey.insert(key, resolution);
    }
    QSet<QString> liveKeys;
    for (const QJsonObject &comment : canonicalComments) {
        liveKeys.insert(commentKey(comment));
    }
    for (const QString &key : deletedOrder) {
        if (!liveKeys.contains(key)) {
            canonicalComments.append(deletedByKey.value(key));
        }
    }

    // Refresh the state summaries after native anchor metadata has been
    // assigned. No deletion can be created by this second observation because
    // the same visible key set was already recorded above.
    observeSource(&state, role, pdfPath, cleanComments, previousFull, false);
    if (!refreshSourceMirrorStates(&state, canonicalComments, occurrences, &error)) {
        return fail(QStringLiteral("could not record source mirror state: %1").arg(error));
    }
    attemptRoot.insert(QStringLiteral("version"), AnchorAttemptCacheVersion);
    attemptRoot.insert(QStringLiteral("source_pdf"), pdfPath);
    attemptRoot.insert(QStringLiteral("attempts"), attempts);

    if (!writeJsonl(canonicalPath, canonicalComments, &error) || !writeJsonObject(statePath, state, &error) || (resolutionRegistryDirty && !writeJsonl(resolutionPath, resolutionRecords, &error)) ||
        !writeJsonObject(attemptPath, attemptRoot, &error)) {
        return fail(QStringLiteral("could not commit comment synchronization: %1").arg(error));
    }
    result.writeMs = phaseTimer.elapsed();
    result.visibleComments = 0;
    result.plainMarkups = 0;
    for (const QJsonObject &record : liveRecords(canonicalComments)) {
        if (isTrackedCommentRecordImpl(record)) {
            ++result.visibleComments;
        } else {
            ++result.plainMarkups;
        }
    }
    sourceFiles.unite(sourceFilesForAnchors(m_options.docsRoot, stem, canonicalComments));
    result.watchedSourceFiles = QStringList(sourceFiles.begin(), sourceFiles.end());
    std::sort(result.watchedSourceFiles.begin(), result.watchedSourceFiles.end());
    QString fingerprintError;
    visibleSidecarComments(m_options.docsRoot, stem, &result.visibleSidecarFingerprint, &fingerprintError);
    if (!fingerprintError.isEmpty()) {
        result.visibleSidecarFingerprint.clear();
        result.logLines.append(QStringLiteral("warning: could not fingerprint synchronized comments: %1").arg(fingerprintError));
    }
    result.totalMs = totalTimer.elapsed();
    result.success = true;
    result.logLines.append(QStringLiteral("native synchronization: role=%1 comments=%2 markup=%3 deletions=%4 source-anchors-orphaned=%5 source-text-imports=%6 anchor-candidates=%7 anchor-attempts=%8 inserted=%9 mirror-updates=%10 removed=%11 unchanged=%12")
                               .arg(role)
                               .arg(result.visibleComments)
                               .arg(result.plainMarkups)
                               .arg(result.deletions)
                               .arg(result.sourceAnchorsOrphaned)
                               .arg(result.sourceCommentEditsImported)
                               .arg(result.anchorCandidates)
                               .arg(result.anchorAttempts)
                               .arg(result.anchorsInserted)
                               .arg(result.anchorMirrorsUpdated)
                               .arg(result.anchorsRemoved)
                               .arg(result.unchangedComments));
    const qsizetype synchronizedTrackedComments = std::count_if(cleanComments.cbegin(), cleanComments.cend(), isTrackedCommentRecordImpl);
    result.logLines.append(QStringLiteral("sidecar protocol: preserved=%1 sidecar-conflicts=%2 resolution-conflicts=%3").arg(result.visibleComments - synchronizedTrackedComments).arg(sidecarConflicts).arg(resolutionConflicts));
    result.logLines.append(QStringLiteral("native timing: load-ms=%1 anchors-ms=%2 write-ms=%3 total-ms=%4").arg(result.loadMs).arg(result.anchorMs).arg(result.writeMs).arg(result.totalMs));
    return result;
}

DocsCommentSynchronizer::Result DocsCommentSynchronizer::prepareSidecar(const QString &pdfStem, const PageGeometryProvider &pageGeometry)
{
    const SourceAnchorRestorationBatchResult restoration = restoreReappearedSourceAnchors(m_options, pdfStem);
    if (!restoration.success) {
        Result failure;
        failure.failureReason = restoration.failureReason;
        failure.logLines = restoration.logLines;
        failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
        return failure;
    }
    Result result = prepareSidecar(pdfStem, pageGeometry, nullptr);
    result.logLines = restoration.logLines + result.logLines;
    return result;
}

DocsCommentSynchronizer::Result DocsCommentSynchronizer::prepareAnnotation(const QString &pdfStem,
                                                                            const QString &annotationId,
                                                                            const PageGeometryProvider &pageGeometry,
                                                                            const QString &exactPdfPath)
{
    if (annotationId.trimmed().isEmpty()) {
        Result failure;
        failure.failureReason = QStringLiteral("invalid annotation identifier");
        failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
        return failure;
    }
    return prepareSidecar(pdfStem, pageGeometry, nullptr, annotationId, exactPdfPath);
}

DocsCommentSynchronizer::Result DocsCommentSynchronizer::prepareSidecar(const QString &pdfStem,
                                                                        const PageGeometryProvider &pageGeometry,
                                                                        DocsAnchorIndex *sharedAnchorIndex,
                                                                        const QString &onlyAnnotationId,
                                                                        const QString &exactPdfPath)
{
    Result failure;
    const QFileInfo stemInfo(pdfStem);
    if (pdfStem.isEmpty() || stemInfo.fileName() != pdfStem) {
        failure.failureReason = QStringLiteral("invalid PDF stem");
        failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
        return failure;
    }
    if (!pageGeometry) {
        failure.failureReason = QStringLiteral("no PDF page-geometry provider is available");
        failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
        return failure;
    }

    QString error;
    const QList<QJsonObject> comments = visibleSidecarComments(m_options.docsRoot, pdfStem, nullptr, &error);
    if (!error.isEmpty()) {
        failure.failureReason = QStringLiteral("could not read canonical sidecar: %1").arg(error);
        failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
        return failure;
    }
    QJsonObject targetComment;
    if (!onlyAnnotationId.isEmpty()) {
        QList<QJsonObject> matches;
        std::copy_if(comments.cbegin(), comments.cend(), std::back_inserter(matches), [&onlyAnnotationId](const QJsonObject &comment) {
            return stableRecordId(comment) == onlyAnnotationId;
        });
        if (matches.isEmpty()) {
            QList<QJsonObject> canonicalRecords;
            int conflicts = 0;
            const QString canonicalPath = QDir(m_options.docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
            if (!reconcileJsonl(canonicalPath, &canonicalRecords, &error, &conflicts)) {
                failure.failureReason = QStringLiteral("could not read canonical sidecar: %1").arg(error);
                failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
                return failure;
            }
            std::copy_if(canonicalRecords.cbegin(), canonicalRecords.cend(), std::back_inserter(matches), [&onlyAnnotationId](const QJsonObject &comment) {
                return isDeletedRecord(comment) && stableRecordId(comment) == onlyAnnotationId;
            });
        }
        if (matches.size() != 1) {
            failure.failureReason = matches.isEmpty() ? QStringLiteral("the target annotation is not present in the canonical sidecar")
                                                      : QStringLiteral("the target annotation identifier is ambiguous in the canonical sidecar");
            failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
            return failure;
        }
        targetComment = matches.constFirst();
    }

    if (!targetComment.isEmpty() && isDeletedRecord(targetComment)) {
        const QString anchorId = targetComment.value(QStringLiteral("anchor_id")).toString(stableRecordId(targetComment)).trimmed();
        const QSet<QString> sourceFiles = sourceFilesForAnchors(m_options.docsRoot, pdfStem, {targetComment});
        const QList<DocsAnchorOccurrence> found = anchorOccurrences(sourceFiles).value(anchorId);
        if (found.size() > 1) {
            failure.failureReason = QStringLiteral("the deleted annotation has ambiguous source anchors");
            failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
            return failure;
        }
        if (found.size() == 1 && !removeAnchor(found.constFirst(), anchorId, &error)) {
            failure.failureReason = QStringLiteral("could not remove deleted anchor: %1").arg(error);
            failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
            return failure;
        }
        Result result;
        result.success = true;
        result.visibleComments = comments.size();
        result.anchorsRemoved = found.size();
        result.sourceAnchorDeletions = found.size();
        result.logLines.append(QStringLiteral("native targeted deletion: removed=%1").arg(result.anchorsRemoved));
        return result;
    }

    const QString pdfPath = exactPdfPath.trimmed().isEmpty() ? QDir(m_options.docsRoot).filePath(QStringLiteral("current_pdfs/%1.pdf").arg(pdfStem))
                                                            : QFileInfo(exactPdfPath).absoluteFilePath();
    if (!QFileInfo::exists(pdfPath)) {
        // Historical/versioned sidecars may intentionally outlive their PDF.
        // They remain available to the global resolution registry, but there
        // is no trustworthy page geometry with which to create a new anchor.
        failure.success = true;
        failure.visibleComments = comments.size();
        failure.logLines.append(QStringLiteral("native preparation skipped: current PDF not found: %1").arg(pdfPath));
        return failure;
    }
    if (!exactPdfPath.trimmed().isEmpty()) {
        const QString expectedSha256 = targetComment.value(QStringLiteral("annotation_pdf_sha256")).toString().trimmed().toLower();
        const QString actualSha256 = fileSha256(pdfPath);
        if (expectedSha256.size() != 64 || actualSha256 != expectedSha256) {
            failure.failureReason = QStringLiteral("the exact PDF does not match the target annotation identity");
            failure.logLines.append(QStringLiteral("error: %1").arg(failure.failureReason));
            return failure;
        }
    }

    QHash<int, PageGeometry> geometries;
    QJsonArray snapshotComments;
    QStringList placementWarnings;
    QHash<QString, QString> invalidPlacements;
    const auto normalizedFragment = [&](const QJsonObject &fragment) -> std::optional<QJsonObject> {
        const int pageNumber = fragment.value(QStringLiteral("page")).toInt();
        if (pageNumber < 1) {
            error = QStringLiteral("sidecar comment has an invalid fragment page number");
            return std::nullopt;
        }
        if (!geometries.contains(pageNumber)) {
            const std::optional<PageGeometry> geometry = pageGeometry(pdfPath, pageNumber, &error);
            if (!geometry || !(geometry->width > 0.0) || !(geometry->height > 0.0)) {
                if (error.isEmpty()) {
                    error = QStringLiteral("could not read page %1 geometry from %2").arg(pageNumber).arg(pdfPath);
                }
                return std::nullopt;
            }
            geometries.insert(pageNumber, *geometry);
        }
        const PageGeometry geometry = geometries.value(pageNumber);
        const QJsonArray pdfRect = fragment.value(QStringLiteral("rect")).toArray();
        QJsonArray normalizedRect;
        if (pdfRect.size() == 4 && std::all_of(pdfRect.begin(), pdfRect.end(), [](const QJsonValue &v) { return v.isDouble() && std::isfinite(v.toDouble()); })) {
            normalizedRect = {
                pdfRect.at(0).toDouble() / geometry.width,
                1.0 - pdfRect.at(3).toDouble() / geometry.height,
                pdfRect.at(2).toDouble() / geometry.width,
                1.0 - pdfRect.at(1).toDouble() / geometry.height,
            };
        } else {
            error = QStringLiteral("sidecar comment has an invalid fragment rectangle");
            return std::nullopt;
        }
        QJsonValue normalizedQuads(QJsonValue::Null);
        if (fragment.value(QStringLiteral("quad_points")).isArray()) {
            QJsonArray values;
            const QJsonArray pdfQuads = fragment.value(QStringLiteral("quad_points")).toArray();
            for (qsizetype offset = 0; offset + 1 < pdfQuads.size(); offset += 2) {
                values.append(pdfQuads.at(offset).toDouble() / geometry.width);
                values.append(1.0 - pdfQuads.at(offset + 1).toDouble() / geometry.height);
            }
            normalizedQuads = values;
        }
        return QJsonObject {
            {QStringLiteral("page"), pageNumber},
            {QStringLiteral("page_width"), geometry.width},
            {QStringLiteral("page_height"), geometry.height},
            {QStringLiteral("page_rotation"), geometry.rotation},
            {QStringLiteral("rect"), normalizedRect},
            {QStringLiteral("quad_points"), normalizedQuads},
        };
    };

    for (const QJsonObject &comment : comments) {
        // A canonical comment can exist before placement is known. Keep it in
        // the catalogue; the anchor worker reports its unresolved location.
        if (comment.value(QStringLiteral("rect")).toArray().isEmpty()
            && comment.value(QStringLiteral("anchor_status")).toString() == QLatin1String("needs_anchor")) {
            continue;
        }
        const std::optional<QJsonObject> rootFragment = normalizedFragment(comment);
        if (!rootFragment) {
            placementWarnings.append(QStringLiteral("warning: comment %1: %2; retained without PDF placement").arg(stableRecordId(comment), error));
            invalidPlacements.insert(stableRecordId(comment), error);
            error.clear();
            continue;
        }
        QJsonObject raw {
            {QStringLiteral("page"), rootFragment->value(QStringLiteral("page"))},
            {QStringLiteral("page_width"), rootFragment->value(QStringLiteral("page_width"))},
            {QStringLiteral("page_height"), rootFragment->value(QStringLiteral("page_height"))},
            {QStringLiteral("page_rotation"), rootFragment->value(QStringLiteral("page_rotation"))},
            {QStringLiteral("subtype"), comment.value(QStringLiteral("subtype"))},
            {QStringLiteral("id"), comment.value(QStringLiteral("id"))},
            {QStringLiteral("rect"), rootFragment->value(QStringLiteral("rect"))},
            {QStringLiteral("quad_points"), rootFragment->value(QStringLiteral("quad_points"))},
            {QStringLiteral("author"), comment.value(QStringLiteral("author"))},
            {QStringLiteral("modified"), comment.value(QStringLiteral("modified"))},
            {QStringLiteral("created"), comment.value(QStringLiteral("created"))},
            {QStringLiteral("contents"), comment.value(QStringLiteral("contents"))},
            {QStringLiteral("selected_text"), comment.value(QStringLiteral("selected_text"))},
            {QStringLiteral("color"), comment.value(QStringLiteral("color"))},
            {QStringLiteral("flags"), comment.value(QStringLiteral("flags"))},
            {QStringLiteral("opacity"), comment.value(QStringLiteral("opacity"))},
            {QStringLiteral("name"), comment.value(QStringLiteral("name"))},
        };
        for (const QString &field : ToolMetadataFields) {
            if (comment.contains(field)) {
                raw.insert(field, comment.value(field));
            }
        }
        const QJsonArray sourceFragments = comment.value(QStringLiteral("fragments")).toArray();
        if (!sourceFragments.isEmpty()) {
            QJsonArray normalizedFragments;
            bool invalidFragment = false;
            for (const QJsonValue &value : sourceFragments) {
                if (!value.isObject()) {
                    error = QStringLiteral("sidecar comment contains a non-object fragment");
                    invalidFragment = true;
                    break;
                }
                const std::optional<QJsonObject> normalized = normalizedFragment(value.toObject());
                if (!normalized) {
                    invalidFragment = true;
                    break;
                }
                normalizedFragments.append(*normalized);
            }
            if (invalidFragment) {
                placementWarnings.append(QStringLiteral("warning: comment %1: %2; retained without PDF placement").arg(stableRecordId(comment), error));
                invalidPlacements.insert(stableRecordId(comment), error);
                error.clear();
                continue;
            }
            raw.insert(QStringLiteral("fragments"), normalizedFragments);
        }
        snapshotComments.append(raw);
    }

    const QJsonObject snapshot {
        {QStringLiteral("version"), 2},
        {QStringLiteral("coordinate_space"), QStringLiteral("normalized-default-page")},
        {QStringLiteral("source_pdf"), QFileInfo(pdfPath).absoluteFilePath()},
        {QStringLiteral("comments"), snapshotComments},
    };
    Result result = synchronize(QUrl::fromLocalFile(pdfPath), snapshot, true, sharedAnchorIndex, onlyAnnotationId, exactPdfPath.trimmed().isEmpty() ? QString() : pdfStem);
    if (result.success && !invalidPlacements.isEmpty()) {
        const QString sidecar = QDir(m_options.docsRoot).filePath(QStringLiteral("comments/%1_comments.jsonl").arg(pdfStem));
        QList<QJsonObject> retained;
        if (!readJsonl(sidecar, &retained, &error)) {
            result.success = false;
            result.failureReason = error;
        } else {
            for (auto &comment : retained) {
                const QString reason = invalidPlacements.value(stableRecordId(comment));
                if (reason.isEmpty() || isDeletedRecord(comment)) continue;
                comment.insert(QStringLiteral("anchor_status"), QStringLiteral("needs_attention"));
                comment.insert(QStringLiteral("anchor_error_code"), QStringLiteral("invalid_pdf_placement"));
                comment.insert(QStringLiteral("anchor_error"), reason);
            }
            if (!writeJsonl(sidecar, retained, &error)) {
                result.success = false;
                result.failureReason = error;
            }
        }
    }
    result.logLines = placementWarnings + result.logLines;
    return result;
}

DocsCommentSynchronizer::BatchResult DocsCommentSynchronizer::prepareAllSidecars(const PageGeometryProvider &pageGeometry,
                                                                                  const QString &onlyPdfStem,
                                                                                  const QSet<QString> &excludedPdfStems)
{
    BatchResult batch;
    QElapsedTimer timer;
    timer.start();

    const SourceMoveBatchResult ownershipRepair = repairMisroutedSidecarRecords(m_options, onlyPdfStem);
    if (!ownershipRepair.success) {
        batch.failureReason = ownershipRepair.failureReason;
        batch.totalMs = timer.elapsed();
        return batch;
    }
    batch.sourceCommentsMoved = ownershipRepair.moved;

    DocsAnchorIndex sharedAnchorIndex;
    if (onlyPdfStem.isEmpty()) {
        const SourceAnchorRestorationBatchResult restoration = restoreReappearedSourceAnchors(m_options, {}, &sharedAnchorIndex);
        if (!restoration.success) {
            batch.failureReason = restoration.failureReason;
            batch.totalMs = timer.elapsed();
            return batch;
        }
        const SourceMoveBatchResult moved = relocateMovedSourceComments(m_options, &sharedAnchorIndex);
        if (!moved.success) {
            batch.failureReason = moved.failureReason;
            batch.totalMs = timer.elapsed();
            return batch;
        }
        batch.sourceCommentsMoved += moved.moved;
    } else {
        sharedAnchorIndex.occurrences = anchorOccurrences(batchSourceFilesForAnchors(m_options.docsRoot));
        const SourceAnchorRestorationBatchResult restoration = restoreReappearedSourceAnchors(m_options, onlyPdfStem, &sharedAnchorIndex);
        if (!restoration.success) {
            batch.failureReason = restoration.failureReason;
            batch.totalMs = timer.elapsed();
            return batch;
        }
    }

    QStringList stems;
    if (!onlyPdfStem.isEmpty()) {
        if (!excludedPdfStems.contains(onlyPdfStem)) {
            stems.append(onlyPdfStem);
        }
    } else {
        const QDir commentsDir(QDir(m_options.docsRoot).filePath(QStringLiteral("comments")));
        for (const QFileInfo &sidecar : legacyDocumentSidecars(commentsDir)) {
            QString stem = sidecar.fileName();
            stem.chop(QStringLiteral("_comments.jsonl").size());
            if (!excludedPdfStems.contains(stem)) {
                stems.append(stem);
            }
        }
    }

    for (const QString &stem : std::as_const(stems)) {
        DocumentResult document {
            .pdfStem = stem,
            .result = prepareSidecar(stem, pageGeometry, &sharedAnchorIndex),
        };
        batch.visibleComments += document.result.visibleComments;
        batch.plainMarkups += document.result.plainMarkups;
        batch.anchorsInserted += document.result.anchorsInserted;
        batch.anchorMirrorsUpdated += document.result.anchorMirrorsUpdated;
        batch.sourceAnchorDeletions += document.result.sourceAnchorDeletions;
        batch.sourceAnchorsOrphaned += document.result.sourceAnchorsOrphaned;
        batch.sourceCommentEditsImported += document.result.sourceCommentEditsImported;
        batch.sourceCommentsMoved += document.result.sourceCommentsMoved;
        batch.documents.append(document);
        if (!document.result.success) {
            batch.failureReason = QStringLiteral("%1: %2").arg(stem, document.result.failureReason);
            batch.totalMs = timer.elapsed();
            return batch;
        }
    }

    batch.success = true;
    batch.totalMs = timer.elapsed();
    return batch;
}


QSet<QString> DocsCommentSynchronizer::commentSourceSet() const
{
    if (m_options.commentSourceFiles) return *m_options.commentSourceFiles;
    QSet<QString> paths = managedSourceFiles(m_options.docsRoot);
    for (const QString &path : m_options.additionalCommentSourceFiles) paths.insert(canonicalOrAbsolutePath(path));
    return paths;
}

bool DocsCommentSynchronizer::removeCommentAnchor(const QString &sourcePath, const QString &anchorId, QString *error)
{
    bool read = false;
    const auto found = anchorOccurrences({canonicalOrAbsolutePath(sourcePath)}, &read).value(anchorId);
    if (!read || found.size() != 1) {
        if (error) *error = read ? QStringLiteral("anchor marker was not uniquely removable") : QStringLiteral("source file is unreadable");
        return false;
    }
    return removeAnchor(found.constFirst(), anchorId, error);
}

bool DocsCommentSynchronizer::removeMissingCommentMarker(const QString &sourcePath, const QString &anchorId, QString *error)
{
    const QString path = canonicalOrAbsolutePath(sourcePath);
    const auto fail = [error](const QString &reason) {
        if (error) *error = reason;
        return false;
    };
    QFile originalFile(path);
    if (!originalFile.open(QIODevice::ReadOnly)) return fail(QStringLiteral("The source file could not be read."));
    const QByteArray originalBytes = originalFile.readAll();
    originalFile.close();
    bool read = false;
    const DocsAnchorOccurrences before = anchorOccurrences({path}, &read);
    if (!read) return fail(QStringLiteral("The source file could not be read."));
    const auto found = before.value(anchorId);
    if (found.isEmpty()) return fail(QStringLiteral("The marker is no longer in the source."));
    if (found.size() != 1) return fail(QStringLiteral("The marker appears %1 times in this file; remove the extra copies by hand.").arg(found.size()));
    QString removeError;
    if (!removeAnchor(found.constFirst(), anchorId, &removeError)) return fail(QStringLiteral("The marker could not be removed: %1").arg(removeError));
    // Verify: this marker is gone and every other marker is still there; otherwise restore the original bytes.
    bool verified = false;
    DocsAnchorOccurrences after = anchorOccurrences({path}, &verified);
    bool intact = verified && after.value(anchorId).isEmpty();
    for (auto it = before.cbegin(); intact && it != before.cend(); ++it) {
        if (it.key() != anchorId && after.value(it.key()).size() != it.value().size()) intact = false;
    }
    for (auto it = after.cbegin(); intact && it != after.cend(); ++it) {
        if (!before.contains(it.key())) intact = false;
    }
    if (!intact) {
        QSaveFile restore(path);
        if (restore.open(QIODevice::WriteOnly)) {
            restore.write(originalBytes);
            restore.commit();
        }
        return fail(QStringLiteral("The marker removal could not be verified; the file was left unchanged."));
    }
    return true;
}

namespace
{
QString restoreLocation(const QString &file, int line)
{
    return line > 0 ? QStringLiteral("%1:%2").arg(file).arg(line) : file;
}

bool markdownSafeRestoreLine(const QStringList &lines, const QList<bool> &visible, int frontMatterEnd, int line)
{
    if (line < 1 || line > lines.size() || line <= frontMatterEnd || !visible.value(line - 1)) return false;
    // Never split an HTML comment or the metadata/token pair of another marker.
    bool inComment = false;
    for (int index = 0; index < line - 1; ++index) {
        const QString &text = lines.at(index);
        int position = 0;
        while (true) {
            if (!inComment) {
                const int open = text.indexOf(QLatin1String("<!--"), position);
                if (open < 0) break;
                inComment = true;
                position = open + 4;
            } else {
                const int close = text.indexOf(QLatin1String("-->"), position);
                if (close < 0) break;
                inComment = false;
                position = close + 3;
            }
        }
    }
    if (inComment) return false;
    if (line > 1 && lines.at(line - 2).trimmed().startsWith(QLatin1String("<!-- thoughts:v1:meta "))) return false;
    return true;
}
}

DocsCommentSynchronizer::AnchorRestoreResult DocsCommentSynchronizer::restoreRemovedAnchor(const QJsonObject &rootPayload)
{
    using Placement = AnchorRestoreResult::Placement;
    AnchorRestoreResult result;
    const auto fail = [&result](const QString &reason) {
        result.success = false;
        result.failureReason = reason;
        result.message = reason;
        return result;
    };
    QJsonObject scope = rootPayload.value(QStringLiteral("scope")).toObject();
    if (scope.isEmpty() && rootPayload.contains(QStringLiteral("anchor_id"))) {
        // A sidecar placement record keeps the last known anchor in its own fields.
        scope = {{QStringLiteral("anchor_id"), rootPayload.value(QStringLiteral("anchor_id"))},
                 {QStringLiteral("source_file"), rootPayload.value(QStringLiteral("tex_file"))},
                 {QStringLiteral("source_line"), rootPayload.value(QStringLiteral("tex_line"))}};
        const QString subtype = rootPayload.value(QStringLiteral("subtype")).toString();
        if (rootPayload.value(QStringLiteral("anchor_kind")).toString() == QLatin1String("wrapped_highlight") || isTextMarkupSubtype(subtype))
            scope.insert(QStringLiteral("anchor_kind"), subtype == QLatin1String("StrikeOut") ? QStringLiteral("CommentStrikethrough") : QStringLiteral("CommentHighlight"));
    }
    const QString id = scope.value(QStringLiteral("anchor_id")).toString(rootPayload.value(QStringLiteral("comment_id")).toString(rootPayload.value(QStringLiteral("id")).toString())).trimmed();
    static const QRegularExpression safeId(QStringLiteral("^[A-Za-z0-9._:-]+$"));
    if (id.isEmpty() || !safeId.match(id).hasMatch()) return fail(QStringLiteral("The comment has no restorable anchor identity."));
    QString contents = rootPayload.value(QStringLiteral("contents")).toString();
    if (contents.trimmed().isEmpty()) contents = scope.value(QStringLiteral("anchor_mirror")).toString();
    const QString root = canonicalOrAbsolutePath(m_options.docsRoot);
    const auto relative = [&root](const QString &path) {
        const QString value = QDir(root).relativeFilePath(canonicalOrAbsolutePath(path));
        return value.startsWith(QLatin1String("../")) ? canonicalOrAbsolutePath(path) : value;
    };

    // Pre-check: a live marker anywhere means there is nothing to restore; raw text without a marker is malformed.
    const QSet<QString> sources = commentSourceSet();
    bool complete = false;
    const DocsAnchorOccurrences index = anchorOccurrences(sources, &complete);
    const QList<DocsAnchorOccurrence> present = index.value(id);
    if (present.size() == 1) {
        result.success = true;
        result.placement = Placement::AlreadyPresent;
        result.sourceFile = relative(present.constFirst().path);
        result.line = present.constFirst().line;
        result.message = QStringLiteral("Anchor already present at %1.").arg(restoreLocation(result.sourceFile, result.line));
        return result;
    }
    if (present.size() > 1) return fail(QStringLiteral("The anchor appears %1 times in the sources; fix them by hand.").arg(present.size()));
    QStringList sortedSources(sources.cbegin(), sources.cend());
    std::sort(sortedSources.begin(), sortedSources.end());
    for (const QString &path : std::as_const(sortedSources)) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray bytes = file.readAll();
        const qsizetype offset = bytes.indexOf(id.toUtf8());
        if (offset >= 0)
            return fail(QStringLiteral("The anchor text is still in %1 but malformed; fix it by hand.").arg(restoreLocation(relative(path), bytes.left(offset).count('\n') + 1)));
    }
    if (!complete) return fail(QStringLiteral("Some project sources could not be read; the anchor was not restored."));

    // File: the last known source, otherwise the primary source of the comment's document.
    const QString knownSource = scope.value(QStringLiteral("source_file")).toString();
    QString path;
    if (!knownSource.isEmpty()) {
        const QFileInfo info(knownSource);
        const QString candidate = canonicalOrAbsolutePath(info.isAbsolute() ? info.absoluteFilePath() : QDir(m_options.docsRoot).filePath(knownSource));
        const bool tracked = std::any_of(sources.cbegin(), sources.cend(), [&candidate](const QString &source) { return canonicalOrAbsolutePath(source) == candidate; });
        if (QFileInfo(candidate).isFile() && (tracked || markerEligibleSourcePath(m_options.docsRoot, knownSource, candidate))) path = candidate;
    }
    bool fallback = false;
    if (path.isEmpty()) {
        QStringList candidates;
        const QString stem = rootPayload.value(QStringLiteral("origin")).toObject().value(QStringLiteral("collection")).toString();
        if (!stem.isEmpty()) {
            QString ignored;
            candidates = sourceFilesForDocument(m_options.docsRoot, stem, &ignored);
        }
        const QString owner = knownSource.isEmpty() ? QString() : sourceOwnerOrTop(knownSource);
        for (const QString &source : std::as_const(sortedSources)) {
            if (!owner.isEmpty() && sourceOwnerOrTop(relative(source)) == owner && !candidates.contains(source)) candidates.append(source);
        }
        static const QRegularExpression beginDocument(QStringLiteral("\\\\begin\\s*\\{document\\}"));
        for (const QString &candidate : std::as_const(candidates)) {
            if (!candidate.endsWith(QLatin1String(".tex")) || !QFileInfo(candidate).isFile()) continue;
            QFile file(candidate);
            if (file.open(QIODevice::ReadOnly) && beginDocument.match(QString::fromUtf8(file.readAll())).hasMatch()) { path = canonicalOrAbsolutePath(candidate); break; }
        }
        for (const QString &candidate : std::as_const(candidates)) {
            if (path.isEmpty() && isMarkdownSourcePath(candidate) && QFileInfo(candidate).isFile()) path = canonicalOrAbsolutePath(candidate);
        }
        if (path.isEmpty()) return fail(QStringLiteral("The source file no longer exists; link the comment manually."));
        fallback = true;
    }
    const bool markdown = isMarkdownSourcePath(path);
    static const QRegularExpression markdownId(QStringLiteral("^[A-Za-z0-9_-]+$"));
    if (markdown && !markdownId.match(id).hasMatch()) return fail(QStringLiteral("The anchor identity cannot be written to Markdown."));

    QFile originalFile(path);
    if (!originalFile.open(QIODevice::ReadOnly)) return fail(QStringLiteral("%1 could not be read.").arg(relative(path)));
    const QByteArray originalBytes = originalFile.readAll();
    originalFile.close();
    QString error;
    QStringList lines;
    if (!readTextLines(path, &lines, &error)) return fail(QStringLiteral("%1 could not be read: %2").arg(relative(path), error));
    const bool trailingNewline = textFileHasTrailingNewline(path);
    const int lineCount = lines.size();
    const int requested = scope.value(QStringLiteral("source_line")).toInt();
    result.requestedLine = requested;
    result.sourceFile = relative(path);

    // Candidate range: the document body for TeX, after any front matter for Markdown.
    int first = 1, last = lineCount, endDocument = 0, frontMatterEnd = 0;
    QList<bool> visible;
    if (markdown) {
        visible = markdownVisibleLineMask(lines);
        if (!lines.isEmpty() && lines.constFirst().trimmed() == QLatin1String("---")) {
            for (int index = 1; index < lines.size(); ++index) {
                const QString line = lines.at(index).trimmed();
                if (line == QLatin1String("---") || line == QLatin1String("...")) { frontMatterEnd = index + 1; break; }
            }
        }
    } else {
        static const QRegularExpression beginDocument(QStringLiteral("^\\s*\\\\begin\\s*\\{document\\}"));
        static const QRegularExpression endDocumentLine(QStringLiteral("^\\s*\\\\end\\s*\\{document\\}"));
        for (int index = 0; index < lines.size(); ++index) {
            if (beginDocument.match(lines.at(index)).hasMatch()) first = index + 2;
            if (endDocumentLine.match(lines.at(index)).hasMatch()) { endDocument = index + 1; last = index; break; }
        }
    }
    const auto safeLine = [&](int candidate) {
        if (candidate < first || candidate > last) return 0;
        if (markdown) return markdownSafeRestoreLine(lines, visible, frontMatterEnd, candidate) ? candidate : 0;
        return anchorInsertionLine(lines, candidate);
    };
    const QString kind = scope.value(QStringLiteral("anchor_kind")).toString();
    const QString selected = rootPayload.value(QStringLiteral("selected_text")).toString();
    const bool wantsWrap = !markdown && !selected.trimmed().isEmpty()
        && (kind == QLatin1String("CommentHighlight") || kind == QLatin1String("CommentStrikethrough")
            || (kind.isEmpty() && isTextMarkupSubtype(rootPayload.value(QStringLiteral("subtype")).toString())));
    const QString wrapSubtype = kind == QLatin1String("CommentStrikethrough") || rootPayload.value(QStringLiteral("subtype")).toString() == QLatin1String("StrikeOut")
        ? QStringLiteral("StrikeOut") : QStringLiteral("Highlight");

    int insertion = 0;
    int wrapLine = 0;
    if (!fallback && lineCount > 0) {
        const int target = std::clamp(requested > 0 ? requested : 1, 1, lineCount);
        QList<int> order{target};
        for (int offset = 1; offset <= lineCount; ++offset) {
            if (target + offset <= lineCount) order.append(target + offset);
            if (target - offset >= 1) order.append(target - offset);
        }
        for (const int candidate : std::as_const(order)) {
            if (wantsWrap && !wrapLine && std::abs(candidate - target) <= 8 && candidate >= first && candidate <= last
                && lines.at(candidate - 1).count(selected) == 1) {
                int dryLine = 0;
                if (wrapHighlightedSource(path, candidate, id, contents, selected, wrapSubtype, &dryLine, &error, true) && dryLine == candidate) wrapLine = candidate;
            }
            if (!insertion) insertion = safeLine(candidate);
            if (insertion && (!wantsWrap || wrapLine || std::abs(candidate - target) > 8)) break;
        }
    }

    int line = 0;
    if (wrapLine) {
        if (!wrapHighlightedSource(path, wrapLine, id, contents, selected, wrapSubtype, &line, &error)) return fail(QStringLiteral("The highlight could not be restored: %1").arg(error));
    } else {
        result.pointAnchorFallback = wantsWrap;
        line = insertion ? insertion : (markdown || !endDocument ? lineCount + 1 : endDocument);
        if (markdown) {
            const QString key = QStringLiteral("comment:%1:s1").arg(id);
            lines.insert(line - 1, markdownMetaLine(QJsonObject{{QStringLiteral("key"), key}, {QStringLiteral("contents"), contents}}));
            lines.insert(line, QStringLiteral("<!-- thoughts:v1:point %1 -->").arg(key));
            if (!writeTextLines(path, lines, trailingNewline || lineCount == 0, &error)) return fail(QStringLiteral("%1 could not be written: %2").arg(result.sourceFile, error));
        } else if (!insertOrMoveAnchor({}, path, &line, id, contents, &error)) {
            return fail(QStringLiteral("%1 could not be written: %2").arg(result.sourceFile, error));
        }
    }
    const auto rollback = [&](const QString &reason) {
        QSaveFile restore(path);
        if (restore.open(QIODevice::WriteOnly)) {
            restore.write(originalBytes);
            restore.commit();
        }
        return fail(reason);
    };
    if (!markdown) {
        int definitionLine = 0, addedLines = 0;
        if (!ensureCommentAnchorDefinition(path, &definitionLine, &addedLines, &error)) return rollback(QStringLiteral("The anchor definition could not be added: %1").arg(error));
        if (addedLines > 0 && line >= definitionLine) line += addedLines;
    }
    // Verify: exactly one live marker with this identity, otherwise leave the source untouched.
    bool verified = false;
    const QList<DocsAnchorOccurrence> written = anchorOccurrences({path}, &verified).value(id);
    if (!verified || written.size() != 1) return rollback(QStringLiteral("The restored anchor could not be verified; %1 was left unchanged.").arg(result.sourceFile));
    result.line = written.constFirst().line;
    result.success = true;
    if (fallback) {
        result.placement = Placement::FallbackFile;
        result.message = knownSource.isEmpty() ? QStringLiteral("Anchor restored at the end of %1.").arg(result.sourceFile)
                                               : QStringLiteral("%1 no longer exists; anchor restored at the end of %2.").arg(knownSource, result.sourceFile);
    } else if (!insertion && !wrapLine) {
        result.placement = Placement::EndOfFile;
        result.message = QStringLiteral("No line near %1 can hold an anchor; anchor restored at the end of %2.").arg(restoreLocation(result.sourceFile, requested), result.sourceFile);
    } else if ((wrapLine ? wrapLine : insertion) == requested) {
        result.placement = Placement::Exact;
        result.message = QStringLiteral("Anchor restored at %1.").arg(restoreLocation(result.sourceFile, result.line));
    } else {
        result.placement = Placement::NearestLine;
        result.message = requested > lineCount ? QStringLiteral("Line %1 no longer exists in %2; anchor restored at line %3.").arg(requested).arg(result.sourceFile).arg(result.line)
                                               : QStringLiteral("Line %1 of %2 cannot hold an anchor; anchor restored at line %3.").arg(requested).arg(result.sourceFile).arg(result.line);
    }
    if (result.pointAnchorFallback) result.message += QStringLiteral(" The highlight was restored as a point anchor.");
    return result;
}

}
