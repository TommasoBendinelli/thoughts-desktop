// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef THOUGHTS_PROJECTREADING_H
#define THOUGHTS_PROJECTREADING_H

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <optional>

namespace Thoughts {
struct ReadingDocument {
    QString id, title, pdfSha256, packageSha256;
    qint64 size = 0;
    int pages = 0;
    int firstPage = 0;
};

struct ProjectReading {
    QString id, title;
    QList<ReadingDocument> documents;
    int pages = 0;

    static std::optional<ProjectReading> parse(const QByteArray &bytes)
    {
        const auto json = QJsonDocument::fromJson(bytes);
        const auto value = json.object();
        if (!json.isObject() || value.value(QStringLiteral("schema_version")).toInt() != 1
            || value.value(QStringLiteral("kind")) != QStringLiteral("thoughts-project-reading")
            || !value.value(QStringLiteral("documents")).isArray()) return std::nullopt;
        const auto identity = [](const QJsonValue &v) {
            const auto s = v.toString();
            return !s.isEmpty() && s.size() <= 1024 && !s.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")));
        };
        const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
        if (!identity(value.value(QStringLiteral("project_id"))) || !identity(value.value(QStringLiteral("title")))) return std::nullopt;
        ProjectReading result;
        result.id = value.value(QStringLiteral("project_id")).toString();
        result.title = value.value(QStringLiteral("title")).toString();
        QSet<QString> ids;
        QHash<QString, qint64> sizes;
        const auto entries = value.value(QStringLiteral("documents")).toArray();
        if (entries.size() > 10000) return std::nullopt;
        for (const auto &entry : entries) {
            if (!entry.isObject()) return std::nullopt;
            const auto row = entry.toObject();
            if (!identity(row.value(QStringLiteral("document_id"))) || !identity(row.value(QStringLiteral("title")))) return std::nullopt;
            ReadingDocument document;
            document.id = row.value(QStringLiteral("document_id")).toString();
            document.title = row.value(QStringLiteral("title")).toString();
            document.pdfSha256 = row.value(QStringLiteral("pdf_sha256")).toString();
            document.packageSha256 = row.value(QStringLiteral("package_sha256")).toString();
            document.size = row.value(QStringLiteral("pdf_size_bytes")).toInteger(-1);
            document.pages = row.value(QStringLiteral("page_count")).toInt(-1);
            document.firstPage = result.pages;
            if (ids.contains(document.id) || !digest.match(document.pdfSha256).hasMatch()
                || !digest.match(document.packageSha256).hasMatch() || document.size < 1
                || document.pages < 1 || document.pages > 1000000 || result.pages > 10000000 - document.pages
                || (sizes.contains(document.pdfSha256) && sizes.value(document.pdfSha256) != document.size)) return std::nullopt;
            ids.insert(document.id);
            sizes.insert(document.pdfSha256, document.size);
            result.documents.append(document);
            result.pages += document.pages;
        }
        if (value.value(QStringLiteral("page_count")).toInteger(-1) != result.pages) return std::nullopt;
        return result;
    }

    const ReadingDocument *locate(int page) const
    {
        if (page < 0 || page >= pages) return nullptr;
        for (const auto &document : documents)
            if (page < document.firstPage + document.pages) return &document;
        return nullptr;
    }

    int projectPage(const QString &documentId, int page) const
    {
        for (const auto &document : documents)
            if (document.id == documentId && page >= 0 && page < document.pages) return document.firstPage + page;
        return -1;
    }
};
}
#endif
