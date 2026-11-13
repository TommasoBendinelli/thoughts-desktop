// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <optional>

namespace Thoughts
{
class DesktopSyncOutbox;

// PDF link overrides: a link of a Library PDF (a citation, a URL) opens a chosen
// Library document instead of its own destination. One synced record per override
// (entity_type pdf_link_override, collection link-overrides, entity_id = override_id),
// last writer wins on revision, deletion is a tombstone. The phone matches the same
// records, so everything here follows the shared rules exactly.
namespace PdfLinkOverrides
{
QString entityType();
QString collection();
// Servers that accept pdf_link_override; until one does, local edits stay queued.
QString capability();
QString projectionKey(const QString &overrideId);

// Destination positions closer than this (PDF points) are the same destination.
constexpr double positionTolerance = 4.0;

// A link as both clients describe it. Positions are PDF user-space points with the
// origin at the bottom left of the page, like the /XYZ destination of the link.
struct Link {
    QString kind; // "goto" (same PDF) or "uri"
    QString destName;
    int destPage = -1; // 0-based
    std::optional<double> destX;
    std::optional<double> destY;
    QString uri;

    // Only links that can be matched again: a URI, or an internal link with a
    // named destination or a known y (a page alone would match too much).
    bool isValid() const;
    QJsonObject toJson() const;
    static Link fromJson(const QJsonObject &object);
};

// Whether the stored override (whole record) applies to the clicked @p link.
bool matches(const QJsonObject &record, const Link &link);
// Whether the projections hold any override at all (a cheap test before describing a click).
bool anyOverrides(const QJsonObject &projections);
// Non-deleted overrides of @p sourceDocumentId among projections keyed "entity_type\ncollection\nentity_id".
QList<QJsonObject> overridesFor(const QJsonObject &projections, const QString &sourceDocumentId);
// Every override of the document that applies to @p link, the winner (latest updated_at) first.
QList<QJsonObject> matchingOverrides(const QJsonObject &projections, const QString &sourceDocumentId, const Link &link);
// The winning override for @p link, or an empty object.
QJsonObject findOverride(const QJsonObject &projections, const QString &sourceDocumentId, const Link &link);
// Whether @p a was updated after @p b (updated_at, then revision, then override_id).
bool newer(const QJsonObject &a, const QJsonObject &b);

// A timestamp at @p now, but never before or equal to @p notBefore (a device whose
// clock runs ahead must not make a newer choice lose).
QString timestamp(const QDateTime &now, const QString &notBefore = QString());

QJsonObject create(const QString &sourceDocumentId, const QString &sourcePdfSha256, const Link &link, const QString &targetDocumentId,
                   const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc(), const QString &notBefore = QString());
// The next revision of @p record pointing at another document; unknown fields are kept.
QJsonObject retarget(const QJsonObject &record, const QString &targetDocumentId, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc(),
                     const QString &notBefore = QString());
// The tombstone of @p record; unknown fields are kept.
QJsonObject remove(const QJsonObject &record, const QString &originId, const QDateTime &now = QDateTime::currentDateTimeUtc());

// Empty when @p record is a well-formed override, otherwise why not.
QString validationError(const QJsonObject &record);
// Queues @p record for the server (base revision = revision - 1).
bool enqueue(DesktopSyncOutbox &outbox, const QJsonObject &record, QString *error = nullptr);
}
}
