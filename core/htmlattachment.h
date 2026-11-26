// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "okularcore_export.h"
#include <QObject>
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QFileInfo>
#include <QRegularExpression>
#include <functional>
namespace Thoughts {
// HTML of a source folder's tex/ or markdown/ tree: a connected docs-* folder or a Project's default
// folder .thoughts/projects/<64 hex>/default (legacy .../documents), as part/sourceowner.h and the
// server's source_resources.is_html_source_path define them.
inline QString htmlSourceFolder(const QString &path)
{
    static const QRegularExpression owner(QStringLiteral("^(docs-[A-Za-z0-9_.-]+|\\.thoughts/projects/[0-9a-f]{64}/(?:default|documents))/"));
    const auto folder = owner.match(path);
    return folder.hasMatch() ? folder.captured(1) : QString();
}

inline bool isHtmlSourcePath(const QString &path)
{
    const QString folder = htmlSourceFolder(path);
    if (folder.isEmpty()) return false;
    const auto parts = path.mid(folder.size() + 1).split(QLatin1Char('/'));
    if (parts.size() < 2 || (parts[0] != QLatin1String("tex") && parts[0] != QLatin1String("markdown"))
        || path.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f\\\\?#%]")))) return false;
    for (const auto &part : parts) if (part.isEmpty() || part.startsWith(QLatin1Char('.'))) return false;
    const auto suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("html") || suffix == QLatin1String("htm");
}

// A review card made inside an HTML attachment page (thoughts-html-attachment-cards-SPEC.md):
// anchored by its remembered text and page state, never by source markers or PDF geometry.
inline bool isHtmlAttachmentCard(const QJsonObject &card)
{
    const QJsonObject primary = card.value(QStringLiteral("primary")).toObject();
    return primary.value(QStringLiteral("created_in")).toString() == QLatin1String("html_attachment")
        || card.value(QStringLiteral("source_link")).toObject().value(QStringLiteral("state")).toString() == QLatin1String("html_attachment");
}

// The attachment a PDF links to: its logical path, the verified object file and its SHA-256.
struct HtmlAttachmentTarget {
    QString path;
    QString file;
    QString sha256;
    bool isValid() const
    {
        return !file.isEmpty();
    }
};

OKULARCORE_EXPORT QString htmlAttachmentPath(const QUrl &link);

// A thoughts-html: link clicked inside an embedded page: another page of the same source folder,
// opened at the page state its fragment names ("#slide=3", the page's own #hash route).
struct HtmlPageLink {
    QString path;
    QString hash; // "#…", or empty for the page's start
    bool isValid() const
    {
        return !path.isEmpty();
    }
};
// The page @p link names, seen from the page at @p fromPath. Invalid for any other link: another
// scheme, a host or query, a path that is no page (htmlAttachmentPath), a page of another folder,
// or a fragment no page state can be (longer than 511 characters, control characters).
OKULARCORE_EXPORT HtmlPageLink htmlPageLink(const QUrl &link, const QString &fromPath);

OKULARCORE_EXPORT QString htmlAttachmentFile(const QString &bundleRoot, const QString &pdfPath, const QString &path);
// The source bundles that can hold the attachments of @p pdfPath (the app's, then the Library's runtime).
OKULARCORE_EXPORT QStringList htmlAttachmentBundleRoots(const QString &pdfPath);
// The attachment at @p path of the installed generation of @p pdfPath; @p error says why not.
OKULARCORE_EXPORT HtmlAttachmentTarget resolveHtmlAttachment(const QString &path, const QString &pdfPath, QString *error);
// A verified attachment object by its SHA-256 from the same bundles (an earlier version a card was made on).
OKULARCORE_EXPORT QString htmlAttachmentObject(const QString &sha256, const QString &pdfPath);
// Serves @p html on a fresh loopback origin with the attachment sandbox CSP. Without @p owner the
// server lives 12 hours (a browser tab); with one, as long as @p owner (an in-app view).
OKULARCORE_EXPORT QUrl serveHtmlAttachmentFile(const QString &html, QObject *owner = nullptr);
// Opens the attachment in the default browser (the "Open in Browser" path).
OKULARCORE_EXPORT bool openHtmlAttachment(const QUrl &link, const QUrl &pdf, QString *error);
// The reader that shows the attachments linked from @p document's PDF inside Thoughts (the Part):
// thoughts-html: links of that document go to @p handler while @p receiver lives. Without one,
// a link opens the page in the browser.
OKULARCORE_EXPORT void setHtmlAttachmentHandler(const QObject *document, QObject *receiver, std::function<void(const QUrl &link)> handler);
// Called by the document for an activated thoughts-html: link: true when a reader took it.
OKULARCORE_EXPORT bool handleHtmlAttachmentLink(const QObject *document, const QUrl &link);
// The reader of @p document that opens some of its PDF's web links inside Thoughts (the Part: a
// page of this computer's Thoughts server that opens a Library document): an activated http(s)
// link goes to @p handler while @p receiver lives, and opens in the browser when the handler
// returns false or there is none. A thoughts-pdf-link: (a place in a Library PDF) goes to the
// handler too, and nowhere else.
OKULARCORE_EXPORT void setWebLinkHandler(const QObject *document, QObject *receiver, std::function<bool(const QUrl &link)> handler);
// Called by the document for an activated http(s) or thoughts-pdf-link: link: true when a reader took it.
OKULARCORE_EXPORT bool handleWebLink(const QObject *document, const QUrl &link);
}
