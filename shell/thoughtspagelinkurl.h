// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The addresses of page links, without the Library or any text shown: the thoughts:// page link
// and the Thoughts server's "Open in Thoughts" page of a document. Qt Core only, so the proposal
// link checks (proposallinks.cpp) can read them too; thoughtspagelink.h opens them.

#include "../part/thoughtsserveraddress.h"

#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

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

// A link to a reading overlay (thoughts-server protocol/reading-overlays.md §3.1, the open_url of
// add_reading_overlay): thoughts://open/document?document_id=<id>&overlay=<overlay_id>, which opens
// the document with the Overlay tab on that overlay. The page link's form with an overlay,
// thoughts://open/document/<id>?…&overlay=<overlay_id>, is read the same way. Both ids are UUIDs
// (read in lower case); other parameters are ignored.
struct OverlayLink {
    QString documentId;
    QString overlayId;
};

inline std::optional<OverlayLink> parseOverlayLink(const QUrl &url)
{
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"), QRegularExpression::CaseInsensitiveOption);
    if (!url.isValid() || url.scheme().compare(QLatin1String("thoughts"), Qt::CaseInsensitive) != 0 || url.authority(QUrl::FullyEncoded) != QLatin1String("open")
        || !url.hasQuery()) {
        return std::nullopt;
    }
    const QUrlQuery query(url);
    if (!query.hasQueryItem(QStringLiteral("overlay")) || query.allQueryItemValues(QStringLiteral("overlay")).size() != 1) {
        return std::nullopt;
    }
    OverlayLink link;
    link.overlayId = query.queryItemValue(QStringLiteral("overlay"), QUrl::FullyDecoded).trimmed().toLower();
    const QString path = url.path(QUrl::FullyEncoded);
    if (path == QLatin1String("/document")) {
        if (query.allQueryItemValues(QStringLiteral("document_id")).size() != 1) {
            return std::nullopt;
        }
        link.documentId = query.queryItemValue(QStringLiteral("document_id"), QUrl::FullyDecoded).trimmed().toLower();
    } else if (path.startsWith(QLatin1String("/document/"))) {
        link.documentId = path.mid(int(qstrlen("/document/"))).toLower();
    } else {
        return std::nullopt;
    }
    if (!uuid.match(link.documentId).hasMatch() || !uuid.match(link.overlayId).hasMatch()) {
        return std::nullopt;
    }
    return link;
}

// The origins (https://host[:port]) the Thoughts server this computer uses serves its pages
// from: that of @p configuredAddress (ServerAddress::configured()) and, when it is the public
// server or its earlier private address, both of those, which are the same server. Empty for
// an address ServerAddress refuses.
inline QStringList ownServerOrigins(const QString &configuredAddress)
{
    const QString own = ServerAddress::origin(configuredAddress);
    if (own.isEmpty()) {
        return {};
    }
    // The build's default server and its earlier address, when the build has one (empty: none).
    QStringList owners;
    for (const QString &address : {ServerAddress::defaultUrl(), ServerAddress::legacyUrl()}) {
        const QString origin = ServerAddress::origin(address);
        if (!origin.isEmpty() && !owners.contains(origin)) owners.append(origin);
    }
    return owners.contains(own) ? owners : QStringList{own};
}

inline QStringList ownServerOrigins()
{
    return ownServerOrigins(ServerAddress::configured());
}

// The origin of a web address in the form ServerAddress::origin() writes (lower-case host, the
// port only when it is not the scheme's), or empty when @p url is no http(s) address of a host
// or names a user.
inline QString webOrigin(const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || (scheme != QLatin1String("https") && scheme != QLatin1String("http")) || url.host().isEmpty() || !url.userInfo().isEmpty()) {
        return {};
    }
    const int defaultPort = scheme == QLatin1String("https") ? 443 : 80;
    QString origin = scheme + QStringLiteral("://") + url.host(QUrl::FullyEncoded).toLower();
    if (url.port(defaultPort) != defaultPort) {
        origin += QLatin1Char(':') + QString::number(url.port());
    }
    return origin;
}

// Whether @p url is a page of the Thoughts server at one of @p origins under /open/document/,
// whether or not it names a document: the server's "Open in Thoughts" page.
inline bool isServerDocumentPageUrl(const QUrl &url, const QStringList &origins)
{
    return origins.contains(webOrigin(url)) && url.path(QUrl::FullyEncoded).startsWith(QLatin1String("/open/document/"));
}

// The page link behind the server's "Open in Thoughts" page of a document (thoughts-server
// GET /open/document/{document_id}, which a browser shows for a page link and which offers the
// thoughts:// link): <origin>/open/document/<id>?page=N&first=F&version=V on the Thoughts server
// at one of @p origins (ownServerOrigins()). Agents have written such links into notes, with
// only some of the parameters, so they are read leniently, unlike parsePageLink: a missing or
// unreadable page or first is 1, a missing or malformed version leaves version empty (the
// current version, without the note that the PDF changed), other parameters and a fragment are
// ignored, and the id may be upper-case. Nothing for any other address, which stays the
// browser's (the server's /app/ and every other page).
inline std::optional<PageLink> serverPageLink(const QUrl &url, const QStringList &origins)
{
    static const QRegularExpression uuid(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression number(QStringLiteral("^[0-9]{1,9}$"));
    static const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    if (!isServerDocumentPageUrl(url, origins)) {
        return std::nullopt;
    }
    QString id = url.path(QUrl::FullyEncoded).mid(int(qstrlen("/open/document/")));
    if (id.endsWith(QLatin1Char('/'))) {
        id.chop(1);
    }
    if (!uuid.match(id).hasMatch()) {
        return std::nullopt;
    }
    const QUrlQuery query(url);
    const auto positive = [&query](const QString &key) {
        const QString value = query.queryItemValue(key, QUrl::FullyDecoded).trimmed();
        const int parsed = number.match(value).hasMatch() ? value.toInt() : 0;
        return parsed > 0 ? parsed : 1;
    };
    PageLink link;
    link.documentId = id.toLower();
    // At most nine digits each: first + page - 1 stays an int.
    link.page = positive(QStringLiteral("page"));
    link.first = positive(QStringLiteral("first"));
    const QString version = query.queryItemValue(QStringLiteral("version"), QUrl::FullyDecoded).trimmed().toLower();
    link.version = sha256.match(version).hasMatch() ? version : QString();
    return link;
}
}
