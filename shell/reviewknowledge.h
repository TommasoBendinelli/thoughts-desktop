// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "reviewstore.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

/**
 * What the user knows, from their review cards: read-only search over the cards and a
 * knowledge profile of a topic, for agents (`thoughtsctl search-review-cards` and
 * `thoughtsctl knowledge-profile`, and the MCP tools of the same names).
 *
 * Everything is read from the review query index Thoughts maintains
 * (thoughts-server/review-query.sqlite) and from the Library's sources and PDFs; nothing is
 * written, so it works while Thoughts runs and when it does not.
 */
namespace Thoughts::ReviewKnowledge
{

// The index as the projections Thoughts built it from: cards, events, tag merges and groups.
struct IndexSnapshot {
    QJsonObject projections;
    QString indexedAt;
    // False for an index written by a Thoughts without review_tag_records (no merges or groups).
    bool hasTagRecords = false;
};
// Reads @p databasePath without writing or creating any file: while Thoughts is not writing it,
// without locks (and only if the file did not change meanwhile); otherwise read-only through its
// existing write-ahead log. @p errorCode is index_unavailable or index_unreadable on failure.
bool readIndex(const QString &databasePath, IndexSnapshot *snapshot, QString *error, QString *errorCode = nullptr);
// thoughtsDataRoot()/thoughts-server/review-query.sqlite.
QString indexPath();

struct Library {
    QString docsRoot;
    QList<ReviewCard> cards; // Not deleted, not candidates.
    QList<ReviewTags::TagGroup> groups;
    QHash<QString, QString> merges;
    QJsonArray projects; // LibraryProjectStore::projects(false)
    QString indexedAt;
    bool tagRecords = false;
    // card_id -> its answer feedback records (deleted ones included), from the index.
    QHash<QString, QList<QJsonObject>> feedback;
    // card_id -> record of the cards above: the cards whose feedback may be shared with another.
    QHash<QString, QJsonObject> cardRecords;
    // Feedback on documents (document-feedback.md), not deleted, oldest first, from the index.
    QList<QJsonObject> documentFeedback;
};
Library libraryFromSnapshot(const IndexSnapshot &snapshot, const QString &docsRoot);

// Tag -> knowledge level ("solid", "shaky", "unknown", "own" (the user's own idea) or "ignore").
struct Levels {
    QList<QPair<QString, QString>> tags; // In file order.
    QString source;                      // "default mapping" or the file read.
};
QStringList levelNames();
Levels defaultLevels();
// thoughtsDataRoot()/knowledge-tag-levels.json.
QString levelsPath();
// The file at @p path, or the default mapping when there is none. False (and @p error) when the
// file exists but is not a valid mapping.
bool loadLevels(const QString &path, Levels *levels, QString *error);
// Writes the default mapping to @p path; refuses to replace an existing file.
bool initLevels(const QString &path, QString *error);
QJsonObject levelsDocument(const Levels &levels);
// For a tag reorganization (thoughtsctl review-tags apply): the user's file at @p path, when it
// exists, is copied to "<path>.backup-<UTC time>" (@p backupPath) and rewritten with @p levels
// first, then its own entries for tags neither in @p levels nor in @p mergedAway. Without a file
// nothing is written (the default mapping applies) and @p backupPath stays empty.
bool updateLevels(const QString &path, const QList<QPair<QString, QString>> &levels, const QStringList &mergedAway, QString *backupPath,
                  QString *message);

// The command results, schema_version 1; failures carry error_code and message.
QJsonObject search(const Library &library, const QJsonObject &request, const QDateTime &now = QDateTime::currentDateTimeUtc());
QJsonObject profile(const Library &library, const QJsonObject &request, const Levels &levels,
                    const QDateTime &now = QDateTime::currentDateTimeUtc());
// The profile as text to paste into any chat app before asking about the topic: what the user
// knows about it, and (with "learner") what all their cards say about them in general.
QString agentContext(const QJsonObject &profileResult);
// Reads the index and the levels and answers @p command (search-review-cards, knowledge-profile
// or agent-context, which is knowledge-profile with "learner" on by default plus "text").
QJsonObject run(const QString &command, const QJsonObject &request, const QString &databasePath, const QString &docsRoot,
                const QString &levelsFile, const QDateTime &now = QDateTime::currentDateTimeUtc());
// Reads the index into @p library; a failure carries error_code and message, as run() returns them.
bool readLibrary(const QString &databasePath, const QString &docsRoot, Library *library, QJsonObject *failure);

// list-document-feedback: what the user marked while reading Library documents (likes, dislikes,
// questions, drawings and text boxes), newest first, each with its document and the PDF version it
// was given on, and per version the text of the pages that carry feedback, as that version shows
// them. Comments (kind "other") are left out: they never go to agents. @p versionPath gives the
// file of a PDF version (by its SHA-256), or an empty path and the reason it is not available; it
// is asked once per version, in the order documentFeedbackVersions() lists them.
// Request: "since" (ISO 8601: feedback created then or later), "include_pages" (default true),
// "images_dir" (a folder: each drawing and text box gets a PNG of its page with its marks on it).
using VersionPath = std::function<QString(const QString &pdfSha256, QString *unavailable)>;
QJsonObject documentFeedback(const Library &library, const QJsonObject &request, const VersionPath &versionPath);
// The PDF versions documentFeedback() reads for @p request, newest feedback first (empty when the
// request is invalid).
QStringList documentFeedbackVersions(const Library &library, const QJsonObject &request);
QJsonObject error(const QString &code, const QString &message);
}
