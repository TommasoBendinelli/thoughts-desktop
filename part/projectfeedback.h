// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef THOUGHTS_PROJECTFEEDBACK_H
#define THOUGHTS_PROJECTFEEDBACK_H
#include "feedbackversion.h"
#include "projectreading.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>

namespace Thoughts::ProjectFeedback {
// A Project read as one PDF numbers its pages across its documents, while document feedback is
// anchored to one document's own PDF and pages. The feedback layer sees the combined PDF as one
// "virtual" document: every document's records with their pages moved to where that document
// starts, and its current PDF's hash replaced by the reading's. What is written there is moved
// back to its own document before it is saved, so the record is the same as one given there.

// A UUIDv4-shaped ID for the reading with @p revision (the SHA-256 of its manifest): the same while
// that reading is open, never a Library document's.
inline QString virtualDocumentId(const QString &revision)
{
    QString hex = QString::fromLatin1(QCryptographicHash::hash(QByteArray(QByteArray("thoughts-project-feedback\n") + revision.toLatin1()), QCryptographicHash::Sha256).toHex());
    hex[12] = QLatin1Char('4');
    hex[16] = QLatin1Char("89ab"[QStringLiteral("0123456789abcdef").indexOf(hex[16]) & 3]);
    return hex.mid(0, 8) + QLatin1Char('-') + hex.mid(8, 4) + QLatin1Char('-') + hex.mid(12, 4) + QLatin1Char('-') + hex.mid(16, 4) + QLatin1Char('-')
        + hex.mid(20, 12);
}

// @p record with its regions' pages moved by @p offset, its document ID set to @p documentId, and
// @p fromSha (the PDF hash of its anchor and of its review_version) replaced by @p toSha.
inline QJsonObject moved(QJsonObject record, int offset, const QString &documentId, const QString &fromSha, const QString &toSha)
{
    QJsonObject anchor = record.value(QStringLiteral("anchor")).toObject();
    QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
    pdf.insert(QStringLiteral("document_id"), documentId);
    if (pdf.value(QStringLiteral("pdf_sha256")).toString().compare(fromSha, Qt::CaseInsensitive) == 0) pdf.insert(QStringLiteral("pdf_sha256"), toSha);
    QJsonArray regions;
    for (const QJsonValue &value : pdf.value(QStringLiteral("regions")).toArray()) {
        QJsonObject region = value.toObject();
        region.insert(QStringLiteral("page"), region.value(QStringLiteral("page")).toInt() + offset);
        regions.append(region);
    }
    pdf.insert(QStringLiteral("regions"), regions);
    anchor.insert(QStringLiteral("pdf"), pdf);
    record.insert(QStringLiteral("anchor"), anchor);
    QJsonObject version = record.value(FeedbackVersion::field()).toObject();
    if (version.value(QStringLiteral("pdf_sha256")).toString().compare(fromSha, Qt::CaseInsensitive) == 0) {
        version.insert(QStringLiteral("pdf_sha256"), toSha);
        record.insert(FeedbackVersion::field(), version);
    }
    return record;
}

// A record of @p document as the combined PDF shows it.
inline QJsonObject toProject(const QJsonObject &record, const ReadingDocument &document, const QString &virtualId, const QString &virtualSha)
{
    return moved(record, document.firstPage, virtualId, document.pdfSha256, virtualSha);
}

// A record written in the combined PDF, back on @p document's own pages and PDF.
inline QJsonObject fromProject(const QJsonObject &record, const ReadingDocument &document, const QString &virtualSha)
{
    return moved(record, -document.firstPage, document.id, virtualSha, document.pdfSha256);
}

// The document whose pages hold every region of @p record (1-based pages of the combined PDF);
// null when they lie in two documents or outside the reading.
inline const ReadingDocument *documentOf(const QJsonObject &record, const ProjectReading &reading)
{
    const ReadingDocument *found = nullptr;
    const QJsonArray regions = record.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray();
    for (const QJsonValue &value : regions) {
        const ReadingDocument *document = reading.locate(value.toObject().value(QStringLiteral("page")).toInt() - 1);
        if (!document || (found && found != document)) return nullptr;
        found = document;
    }
    return found;
}
}
#endif
