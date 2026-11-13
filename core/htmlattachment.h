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
inline bool isHtmlSourcePath(const QString &path)
{
    const auto parts = path.split(QLatin1Char('/'));
    if (parts.size() < 3 || !QRegularExpression(QStringLiteral("^docs-[A-Za-z0-9_.-]+$")).match(parts[0]).hasMatch()
        || (parts[1] != QLatin1String("tex") && parts[1] != QLatin1String("markdown"))
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
}
