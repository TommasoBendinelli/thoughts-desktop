// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDir>
#include <QHash>
#include <QSet>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QUrl>

namespace Thoughts {
// A component's permanent ID disambiguates documents sharing one assembled PDF.
inline QString readerDisplayTitle(const QJsonArray &projects, const QJsonArray &publications, const QString &docsRoot,
                                  const QUrl &url, const QString &documentId = {}, const QString &projectId = {},
                                  const QString &fallbackTitle = {})
{
    const QFileInfo file(url.toLocalFile());
    const auto samePath = [](const QString &a, const QString &b) {
        if (a.isEmpty() || b.isEmpty()) return false;
        const QFileInfo first(a), second(b);
        return !first.canonicalFilePath().isEmpty() ? first.canonicalFilePath() == second.canonicalFilePath()
                                                  : first.absoluteFilePath() == second.absoluteFilePath();
    };
    const bool canonical = url.isLocalFile() && samePath(file.absolutePath(), QDir(docsRoot).filePath(QStringLiteral("current_pdfs")));
    QHash<QString, QString> documentMatches;
    QSet<QString> projectMatches;
    for (const auto &value : projects) {
        const auto project = value.toObject();
        if (!projectId.isEmpty() && project.value(QStringLiteral("id")).toString() == projectId)
            return project.value(QStringLiteral("name")).toString(fallbackTitle);
        if (canonical && project.value(QStringLiteral("target")).toString() == file.fileName())
            projectMatches.insert(project.value(QStringLiteral("name")).toString());
        for (const auto &raw : project.value(QStringLiteral("documents")).toArray()) {
            const auto document = raw.toObject();
            const auto id = document.value(QStringLiteral("document_id")).toString();
            const auto name = document.value(QStringLiteral("name")).toString();
            // Source IDs are accepted only as an explicit selection from older outline entries.
            if (!documentId.isEmpty() && (id == documentId || document.value(QStringLiteral("id")).toString() == documentId))
                return name.isEmpty() ? fallbackTitle : name;
            if ((canonical && document.value(QStringLiteral("filename")).toString() == file.fileName())
                || (url.isLocalFile() && samePath(document.value(QStringLiteral("local_path")).toString(), file.absoluteFilePath())))
                documentMatches.insert(id, name);
        }
    }
    if (!documentId.isEmpty() || !projectId.isEmpty()) return fallbackTitle;
    projectMatches.remove(QString());
    if (projectMatches.size() == 1) return *projectMatches.cbegin();
    if (documentMatches.size() == 1 && !documentMatches.cbegin().value().isEmpty()) return documentMatches.cbegin().value();
    if (canonical) for (const auto &raw : publications) {
        const auto publication = raw.toObject();
        if (publication.value(QStringLiteral("filename")).toString() == file.fileName()) {
            const auto name = publication.value(QStringLiteral("display_name")).toString();
            if (!name.isEmpty()) return name;
        }
    }
    return fallbackTitle.isEmpty() ? url.fileName() : fallbackTitle;
}
}
