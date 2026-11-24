// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Live HTML pages and snapshots (thoughts-server protocol/html-live-pages.md, capability
// html_live_pages_v1). A folder's HTML page whose HTML is a small stub with
// <meta name="thoughts-live-page" content="<address>"> is *live*: readers load that address
// instead of the stub (with the user's consent per origin, and none of the page bridge). Take
// Snapshot turns it into an ordinary embedded page that remembers its address
// (thoughts-live-source); Go Live Again writes the stub back.
//
// Everything here is pure or file-level, so tests run it without a web view.
#include "htmlattachment.h"
#include "okularcore_export.h"

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QUrl>

namespace Thoughts::LivePage
{
// §1: a stub is at most 8 KiB; an address at most 2048 characters.
constexpr qint64 StubLimit = 8 * 1024;
constexpr int AddressLimit = 2048;
// §3: a snapshot follows the size limit of any embedded page.
constexpr qint64 PageLimit = 100000000;

// An address as §1 allows it: an absolute http or https URL of at most 2048 characters, with a
// host and no user name or password. Invalid (empty) otherwise; @p error says why.
OKULARCORE_EXPORT QUrl validAddress(const QString &text, QString *error = nullptr);
// The content of the first <meta name="@p name"> of @p html (outside comments, entities
// decoded); a null string when there is none. Only the first @p scanLimit bytes are read.
OKULARCORE_EXPORT QString metaContent(const QByteArray &html, const QString &name, qint64 scanLimit = 256 * 1024);
// The text of the page's first <title>, simplified; empty when there is none.
OKULARCORE_EXPORT QString title(const QByteArray &html, qint64 scanLimit = 256 * 1024);
// The address a live stub names (§1), or an invalid URL: then the page is an embedded page,
// also when the stub is malformed (a bad address, over 8 KiB, a <script>, not UTF-8).
OKULARCORE_EXPORT QUrl stubAddress(const QByteArray &html);
// The same for a page file (reads at most StubLimit + 1 bytes).
OKULARCORE_EXPORT QUrl stubAddressOfFile(const QString &file);
// A snapshot's thoughts-live-source (validated), for an embedded page only; invalid otherwise.
OKULARCORE_EXPORT QUrl snapshotSource(const QByteArray &html);
OKULARCORE_EXPORT QUrl snapshotSourceOfFile(const QString &file);
// The origin the reader asks about and remembers (§2): scheme://host[:port], the port only when
// it is not the scheme's default. Empty for an invalid address.
OKULARCORE_EXPORT QString origin(const QUrl &address);
// Whether a navigation of the live page at @p address to @p target stays in the page view
// (§2: "navigate within itself"): the main frame only within the page's origin (or to
// about:blank, or from http to https on the same host), frames inside it to any web address. Anything else is stopped; a web or mailto
// link is the system browser's (the caller asks).
OKULARCORE_EXPORT bool navigationStays(const QUrl &address, const QUrl &target, bool mainFrame);
// Whether a stopped navigation to @p target may go to the system browser at all.
OKULARCORE_EXPORT bool isBrowserLink(const QUrl &target);

// The stub of §1 for @p address, titled @p title (the page's name in lists and tabs).
OKULARCORE_EXPORT QByteArray stubHtml(const QUrl &address, const QString &title);
// §3: the page the live page's serializer returned (@p serialized, the outer HTML of its root
// element, scripts already removed) as the snapshot file: a doctype and a UTF-8 charset, the two
// meta elements thoughts-live-source (@p address) and thoughts-snapshot-at (@p takenAt, RFC 3339
// UTC), the page's name @p title as its <title>, and the visible line "Snapshot of <address>,
// <@p shownTime>" at the top of the body. Any earlier thoughts-live-* or snapshot meta, <base>,
// charset or http-equiv meta and any <script> left are removed. Empty, with @p error, when the
// result is over the size limit.
OKULARCORE_EXPORT QByteArray finishSnapshot(const QString &serialized, const QUrl &address, const QString &title, const QDateTime &takenAt,
                                            const QString &shownTime, QString *error = nullptr);

// Ask once per origin (§2): the origins the user agreed to open on this device (QSettings).
OKULARCORE_EXPORT bool originAllowed(const QString &origin);
OKULARCORE_EXPORT void allowOrigin(const QString &origin);
OKULARCORE_EXPORT void forgetOrigins();

// What this device wrote over a page (a snapshot, or the stub again), shown instead of the
// version its PDF's source bundle carries until that bundle catches up (the PDFs rebuild after
// Sync publishes the page). Kept in <application data>/html-page-writes: a copy of the bytes by
// their SHA-256 and writes.json {path: sha256}.
OKULARCORE_EXPORT bool rememberWrite(const QString &path, const QByteArray &bytes, QString *error = nullptr);
// The written version of @p path when it should be shown instead of @p built: the Library's file
// <@p docsRoot>/<path> still has its bytes and @p built does not. A record the bundle caught up
// with, or that the Library file no longer matches, is forgotten. Invalid when there is none.
OKULARCORE_EXPORT HtmlAttachmentTarget writtenPage(const QString &path, const HtmlAttachmentTarget &built, const QString &docsRoot);
OKULARCORE_EXPORT void forgetWrites();
// Writes @p bytes as the Library's page file <@p docsRoot>/<@p path> (atomically), after
// checking that the page's docs folder is on this computer.
OKULARCORE_EXPORT bool writeLibraryPage(const QString &docsRoot, const QString &path, const QByteArray &bytes, QString *error = nullptr);
}
