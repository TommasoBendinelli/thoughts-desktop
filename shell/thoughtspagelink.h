// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../part/documentidentity.h"
#include "../part/verifiedartifactcache.h"

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
// A page link agents put in chats, in the form the server's "Open in Thoughts" button opens:
// thoughts://open/document/<document_id>?page=N&first=F&version=<pdf sha256>. N is the page
// of the PDF given to the chat, whose first page was page F of the Library PDF; version is
// the PDF the link was made from. The phone reads the same links (ThoughtsCitationLinks.kt
// parseThoughtsCitationLink), and this parse accepts exactly what the phone accepts.
struct PageLink {
    QString documentId;
    int page = 0;
    int first = 0;
    QString version;

    // The page of the Library PDF, counted from 1.
    int targetPage() const
    {
        return first + page - 1;
    }
};

// Whether @p url is meant as a page link (thoughts://open/...), valid or not.
inline bool isPageLinkUrl(const QUrl &url)
{
    return url.scheme().compare(QLatin1String("thoughts"), Qt::CaseInsensitive) == 0 && url.host() == QLatin1String("open");
}

inline std::optional<PageLink> parsePageLink(const QUrl &url)
{
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"));
    static const QRegularExpression token(QStringLiteral("^[A-Za-z0-9._~-]+$"));
    static const QRegularExpression digits(QStringLiteral("^[0-9]+$"));
    static const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    // The authority is the host alone: no user info, not even an empty one, and no port.
    if (!url.isValid() || url.scheme() != QLatin1String("thoughts") || url.authority(QUrl::FullyEncoded) != QLatin1String("open") || url.hasFragment()
        || !url.hasQuery()) {
        return std::nullopt;
    }
    static const QLatin1String prefix("/document/");
    const QString path = url.path(QUrl::FullyEncoded);
    if (!path.startsWith(prefix)) {
        return std::nullopt;
    }
    PageLink link;
    link.documentId = path.mid(prefix.size());
    if (!uuid.match(link.documentId).hasMatch()) {
        return std::nullopt;
    }

    QHash<QString, QString> query;
    for (const QString &part : url.query(QUrl::FullyEncoded).split(QLatin1Char('&'))) {
        const qsizetype separator = part.indexOf(QLatin1Char('='));
        if (separator <= 0 || separator == part.size() - 1) {
            return std::nullopt;
        }
        const QString key = part.left(separator);
        const QString value = part.mid(separator + 1);
        if (!token.match(key).hasMatch() || !token.match(value).hasMatch() || query.contains(key)) {
            return std::nullopt;
        }
        query.insert(key, value);
    }
    const QSet<QString> keys(query.keyBegin(), query.keyEnd());
    if (keys != QSet<QString>{QStringLiteral("page"), QStringLiteral("first"), QStringLiteral("version")}) {
        return std::nullopt;
    }
    const auto positive = [](const QString &value) {
        bool ok = false;
        const int number = digits.match(value).hasMatch() ? value.toInt(&ok) : 0;
        return ok && number > 0 ? number : 0;
    };
    link.page = positive(query.value(QStringLiteral("page")));
    link.first = positive(query.value(QStringLiteral("first")));
    link.version = query.value(QStringLiteral("version"));
    if (!link.page || !link.first || !sha256.match(link.version).hasMatch()
        || qint64(link.first) + qint64(link.page) - 1 > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return link;
}

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
