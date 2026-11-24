// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../part/documentidentity.h"
#include "../part/verifiedartifactcache.h"
#include "thoughtspagelinkurl.h"

#include <KLocalizedString>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <limits>
#include <optional>

namespace Thoughts
{
// The Library PDF of @p documentId: its copy in current_pdfs, when exactly one PDF of the
// synchronized Library carries that document identity. Empty when there is none.
inline QString pageLinkDocumentPath(const QString &docsRoot, const QString &documentId)
{
    QFile file(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QString filename;
    int matches = 0;
    for (const QJsonValue &value : QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("document_id")).toString() == documentId) {
            filename = entry.value(QStringLiteral("filename")).toString();
            ++matches;
        }
    }
    if (matches != 1 || filename.isEmpty() || QFileInfo(filename).fileName() != filename || filename.contains(QLatin1Char('\\'))
        || !filename.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive)) {
        return {};
    }
    // The stable Library path: current_pdfs may point to a replaceable publication stage.
    const QString path = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/") + filename);
    return QFileInfo(path).isFile() ? path : QString();
}

// The SHA-256 of the PDF at @p path: the Library's published hash once the file is verified
// to have it, else the hash of the file as it is.
inline QString pageLinkPdfSha256(const QString &docsRoot, const QString &path)
{
    const QString published = DocumentIdentityStore(docsRoot).forFile(path).value(QStringLiteral("pdf_sha256")).toString();
    return !published.isEmpty() ? published : QString::fromLatin1(artifactFileSha256(path));
}

// The page a link opens in a PDF of @p pageCount pages, and the note shown with it (the
// phone's texts), if any.
struct PageLinkOpening {
    int page = 1;
    QString message;
};

inline PageLinkOpening pageLinkOpening(int targetPage, int pageCount, bool versionChanged)
{
    PageLinkOpening opening;
    opening.page = std::clamp(targetPage, 1, std::max(pageCount, 1));
    const bool clamped = opening.page != targetPage;
    if (versionChanged && clamped) {
        opening.message = i18n("Page %1 is not available in the current PDF, so page %2 was opened. Its pagination may have changed since this link was created.", targetPage, opening.page);
    } else if (versionChanged) {
        opening.message = i18n("Opened page %1 in the current PDF. Its pagination may have changed since this link was created.", opening.page);
    } else if (clamped) {
        opening.message = i18n("Page %1 is not available. Opened page %2 instead.", targetPage, opening.page);
    }
    return opening;
}

inline QString pageLinkInvalidMessage()
{
    return i18n("This Thoughts page link is invalid.");
}

inline QString pageLinkDocumentMissingMessage()
{
    return i18n("This PDF is not in the synchronized Thoughts Library. Sync the Library and try again.");
}
}
