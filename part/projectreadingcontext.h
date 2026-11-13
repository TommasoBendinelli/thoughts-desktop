// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef THOUGHTS_PROJECTREADINGCONTEXT_H
#define THOUGHTS_PROJECTREADINGCONTEXT_H
#include "projectreading.h"
#include "verifiedartifactcache.h"
#include <QDir>
#include <QFileInfo>

namespace Thoughts {
struct ReadingDocumentContext {
    ReadingDocument document;
    QString filename, path;
    QJsonObject package;
    QString stem() const { return QFileInfo(filename).completeBaseName(); }
};
struct ProjectReadingContext {
    ProjectReading reading;
    QString revision;
    QList<ReadingDocumentContext> documents;
    static std::optional<ProjectReadingContext> load(const QString &path)
    {
        if (!path.endsWith(QLatin1String(".thoughts-project"))) return std::nullopt;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size() > 16 * 1024 * 1024) return std::nullopt;
        const auto bytes = file.readAll();
        const auto manifest = ProjectReading::parse(bytes);
        if (!manifest) return std::nullopt;
        ProjectReadingContext result;
        result.reading = *manifest;
        result.revision = QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
        const QDir root(QFileInfo(path).absolutePath());
        for (const auto &document : manifest->documents) {
            QByteArray raw;
            if (!readVerifiedArtifact(root.filePath(QStringLiteral("objects/") + document.packageSha256), document.packageSha256, &raw)) return std::nullopt;
            const auto package = QJsonDocument::fromJson(raw).object();
            const auto pdf = package.contains(QStringLiteral("pdf")) ? package.value(QStringLiteral("pdf")).toObject() : package;
            const auto filename = package.value(QStringLiteral("filename")).toString();
            const auto kind = package.value(QStringLiteral("kind")).toString();
            if ((kind != QLatin1String("thoughts-document-source-artifact") && kind != QLatin1String("thoughts-document-reading-package"))
                || !QRegularExpression(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$")).match(filename).hasMatch()
                || pdf.value(QStringLiteral("pdf_sha256")).toString() != document.pdfSha256
                || pdf.value(QStringLiteral("page_count")).toInt(-1) != document.pages
                || (kind == QLatin1String("thoughts-document-reading-package") && package.value(QStringLiteral("document_id")).toString() != document.id)) return std::nullopt;
            result.documents.append({document, filename, root.filePath(QStringLiteral("objects/") + document.pdfSha256), package});
        }
        return result;
    }
    const ReadingDocumentContext *locate(int page) const
    {
        for (const auto &value : documents)
            if (page >= value.document.firstPage && page < value.document.firstPage + value.document.pages) return &value;
        return nullptr;
    }
    const ReadingDocumentContext *forStem(const QString &stem) const
    {
        for (const auto &value : documents) if (value.stem() == stem) return &value;
        return nullptr;
    }
    const ReadingDocumentContext *forOrigin(const QJsonObject &origin) const
    {
        for (const auto &value : documents)
            if (origin.value(QStringLiteral("pdf_sha256")).toString() == value.document.pdfSha256
                && (origin.value(QStringLiteral("document_id")).toString() == value.document.id
                    || origin.value(QStringLiteral("collection")).toString() == value.stem())) return &value;
        return nullptr;
    }
};
inline QJsonObject offsetCommentPages(QJsonObject record, int offset)
{
    if (record.value(QStringLiteral("page")).toInt() > 0)
        record.insert(QStringLiteral("page"), record.value(QStringLiteral("page")).toInt() + offset);
    QJsonArray fragments;
    for (const auto &value : record.value(QStringLiteral("fragments")).toArray()) fragments.append(offsetCommentPages(value.toObject(), offset));
    if (record.contains(QStringLiteral("fragments"))) record.insert(QStringLiteral("fragments"), fragments);
    return record;
}
}
#endif
