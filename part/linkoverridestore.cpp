// SPDX-License-Identifier: GPL-2.0-or-later
#include "linkoverridestore.h"

#include "desktopsyncstore.h"

#include <QJsonValue>
#include <QUuid>
#include <algorithm>
#include <cmath>

namespace Thoughts::PdfLinkOverrides
{
QString entityType()
{
    return QStringLiteral("pdf_link_override");
}

QString collection()
{
    return QStringLiteral("link-overrides");
}

QString capability()
{
    return QStringLiteral("pdf_link_overrides_v1");
}

QString projectionKey(const QString &overrideId)
{
    return entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n') + overrideId;
}

namespace
{
std::optional<double> number(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    if (!value.isDouble() || !std::isfinite(value.toDouble())) return std::nullopt;
    return value.toDouble();
}

bool validUuid(const QString &id)
{
    const QUuid uuid(id);
    return !uuid.isNull() && uuid.toString(QUuid::WithoutBraces) == id.toLower();
}

QDateTime parsedTime(const QString &value)
{
    QDateTime time = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!time.isValid()) time = QDateTime::fromString(value, Qt::ISODate);
    return time;
}
}

bool Link::isValid() const
{
    if (kind == QLatin1String("uri")) return !uri.isEmpty();
    // The phone knows pages only, so the page is always stored.
    return kind == QLatin1String("goto") && destPage >= 0 && (!destName.isEmpty() || destY.has_value());
}

QJsonObject Link::toJson() const
{
    QJsonObject object{{QStringLiteral("kind"), kind}};
    if (kind == QLatin1String("uri")) {
        object.insert(QStringLiteral("uri"), uri);
        return object;
    }
    if (!destName.isEmpty()) object.insert(QStringLiteral("dest_name"), destName);
    if (destPage >= 0) object.insert(QStringLiteral("dest_page"), destPage);
    if (destX) object.insert(QStringLiteral("dest_x"), *destX);
    if (destY) object.insert(QStringLiteral("dest_y"), *destY);
    return object;
}

Link Link::fromJson(const QJsonObject &object)
{
    Link link;
    link.kind = object.value(QStringLiteral("kind")).toString();
    link.destName = object.value(QStringLiteral("dest_name")).toString();
    const QJsonValue page = object.value(QStringLiteral("dest_page"));
    link.destPage = page.isDouble() ? page.toInt(-1) : -1;
    link.destX = number(object, QStringLiteral("dest_x"));
    link.destY = number(object, QStringLiteral("dest_y"));
    link.uri = object.value(QStringLiteral("uri")).toString();
    return link;
}

bool matches(const QJsonObject &record, const Link &link)
{
    if (record.value(QStringLiteral("deleted")).toBool()) return false;
    const Link stored = Link::fromJson(record.value(QStringLiteral("link")).toObject());
    if (stored.kind != link.kind) return false;
    if (link.kind == QLatin1String("uri")) return !link.uri.isEmpty() && stored.uri == link.uri;
    if (link.kind != QLatin1String("goto")) return false;
    if (!stored.destName.isEmpty() && !link.destName.isEmpty()) return stored.destName == link.destName;
    // Without names on both sides: same page and position. A page alone is too broad.
    if (stored.destName.isEmpty() && !stored.destY) return false;
    if (stored.destPage < 0 || stored.destPage != link.destPage) return false;
    if (stored.destY && (!link.destY || std::abs(*stored.destY - *link.destY) > positionTolerance)) return false;
    if (stored.destX && link.destX && std::abs(*stored.destX - *link.destX) > positionTolerance) return false;
    return true;
}

bool newer(const QJsonObject &a, const QJsonObject &b)
{
    const QDateTime at = parsedTime(a.value(QStringLiteral("updated_at")).toString());
    const QDateTime bt = parsedTime(b.value(QStringLiteral("updated_at")).toString());
    if (at.isValid() != bt.isValid()) return at.isValid();
    if (at.isValid() && at != bt) return at > bt;
    const qint64 ar = a.value(QStringLiteral("revision")).toInteger();
    const qint64 br = b.value(QStringLiteral("revision")).toInteger();
    if (ar != br) return ar > br;
    return a.value(QStringLiteral("override_id")).toString() > b.value(QStringLiteral("override_id")).toString();
}

bool anyOverrides(const QJsonObject &projections)
{
    const QString prefix = entityType() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (it.key().startsWith(prefix)) return true;
    }
    return false;
}

QList<QJsonObject> overridesFor(const QJsonObject &projections, const QString &sourceDocumentId)
{
    QList<QJsonObject> result;
    if (sourceDocumentId.isEmpty()) return result;
    const QString prefix = entityType() + QLatin1Char('\n') + collection() + QLatin1Char('\n');
    for (auto it = projections.constBegin(); it != projections.constEnd(); ++it) {
        if (!it.key().startsWith(prefix) || !it.value().isObject()) continue;
        const QJsonObject record = it.value().toObject();
        if (record.value(QStringLiteral("deleted")).toBool() || record.value(QStringLiteral("source_document_id")).toString() != sourceDocumentId) continue;
        result.append(record);
    }
    return result;
}

QList<QJsonObject> matchingOverrides(const QJsonObject &projections, const QString &sourceDocumentId, const Link &link)
{
    QList<QJsonObject> result;
    for (const QJsonObject &record : overridesFor(projections, sourceDocumentId)) {
        if (matches(record, link)) result.append(record);
    }
    std::sort(result.begin(), result.end(), [](const QJsonObject &a, const QJsonObject &b) { return newer(a, b); });
    return result;
}

QJsonObject findOverride(const QJsonObject &projections, const QString &sourceDocumentId, const Link &link)
{
    const QList<QJsonObject> matching = matchingOverrides(projections, sourceDocumentId, link);
    return matching.isEmpty() ? QJsonObject() : matching.constFirst();
}

QString timestamp(const QDateTime &now, const QString &notBefore)
{
    QDateTime at = now.toUTC();
    const QDateTime previous = parsedTime(notBefore);
    if (previous.isValid() && previous.toUTC() >= at) at = previous.toUTC().addMSecs(1);
    return at.toString(Qt::ISODateWithMs);
}

QJsonObject create(const QString &sourceDocumentId, const QString &sourcePdfSha256, const Link &link, const QString &targetDocumentId,
                   const QString &originId, const QDateTime &now, const QString &notBefore)
{
    const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QJsonObject record{
        {QStringLiteral("override_id"), id},
        {QStringLiteral("source_document_id"), sourceDocumentId},
        {QStringLiteral("link"), link.toJson()},
        {QStringLiteral("target_document_id"), targetDocumentId},
        {QStringLiteral("revision"), 1},
        {QStringLiteral("updated_at"), timestamp(now, notBefore)},
        {QStringLiteral("origin_id"), originId},
        {QStringLiteral("deleted"), false},
    };
    if (!sourcePdfSha256.isEmpty()) record.insert(QStringLiteral("source_pdf_sha256"), sourcePdfSha256);
    return record;
}

QJsonObject retarget(const QJsonObject &record, const QString &targetDocumentId, const QString &originId, const QDateTime &now, const QString &notBefore)
{
    QJsonObject next = record;
    QString after = record.value(QStringLiteral("updated_at")).toString();
    if (!notBefore.isEmpty() && parsedTime(notBefore) > parsedTime(after)) after = notBefore;
    next.insert(QStringLiteral("target_document_id"), targetDocumentId);
    next.insert(QStringLiteral("revision"), record.value(QStringLiteral("revision")).toInteger() + 1);
    next.insert(QStringLiteral("updated_at"), timestamp(now, after));
    next.insert(QStringLiteral("origin_id"), originId);
    next.insert(QStringLiteral("deleted"), false);
    return next;
}

QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now)
{
    QJsonObject next = record;
    next.insert(QStringLiteral("revision"), record.value(QStringLiteral("revision")).toInteger() + 1);
    next.insert(QStringLiteral("updated_at"), timestamp(now, record.value(QStringLiteral("updated_at")).toString()));
    next.insert(QStringLiteral("origin_id"), originId);
    next.insert(QStringLiteral("deleted"), true);
    return next;
}

QString validationError(const QJsonObject &record)
{
    if (!validUuid(record.value(QStringLiteral("override_id")).toString())) return QStringLiteral("The link override has no valid override_id.");
    if (!validUuid(record.value(QStringLiteral("source_document_id")).toString())) return QStringLiteral("The link override has no valid source document.");
    if (record.value(QStringLiteral("revision")).toInteger() < 1) return QStringLiteral("The link override has no valid revision.");
    if (!parsedTime(record.value(QStringLiteral("updated_at")).toString()).isValid()) return QStringLiteral("The link override has no valid updated_at.");
    if (record.value(QStringLiteral("deleted")).toBool()) return QString();
    if (!validUuid(record.value(QStringLiteral("target_document_id")).toString())) return QStringLiteral("The link override has no valid target document.");
    if (!Link::fromJson(record.value(QStringLiteral("link")).toObject()).isValid()) return QStringLiteral("The link override does not describe a link that can be matched.");
    return QString();
}

bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error)
{
    const QString problem = validationError(record);
    if (!problem.isEmpty()) {
        if (error) *error = problem;
        return false;
    }
    QString queueError;
    if (!outbox.ready(&queueError)
        || outbox.enqueue(entityType(), record.value(QStringLiteral("override_id")).toString(), collection(), record.value(QStringLiteral("revision")).toInteger() - 1, record,
                          record.value(QStringLiteral("updated_at")).toString()).isEmpty()) {
        if (error) *error = queueError.isEmpty() ? QStringLiteral("The link could not be added to the local sync queue.") : queueError;
        return false;
    }
    return true;
}
}
