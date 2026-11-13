/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "proposaldecisions.h"
#include "../part/proposalhistory.h"
#include "../part/sourceowner.h"
#include "../part/thoughtignore.h"
#include "../part/thoughtshome.h"
#include "../part/thoughtsplatform.h"
#include "../tools/publisher/managedsource.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

#include <algorithm>
#include <cstdlib>

namespace Thoughts::ProposalDecisions
{
namespace
{
const QStringList &claimActions()
{
    static const QStringList actions{QStringLiteral("accept"), QStringLiteral("reject"), QStringLiteral("partial"), QStringLiteral("withdraw")};
    return actions;
}

const QStringList &claimOrigins()
{
    static const QStringList origins{QStringLiteral("desktop"), QStringLiteral("vscode"), QStringLiteral("thoughtsctl"), QStringLiteral("mcp"),
                                     QStringLiteral("phone_editor")};
    return origins;
}

bool isSha256(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool isUuid(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern.match(value).hasMatch() && !QUuid(value).isNull();
}

QString digest(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

bool fail(QString *error, const QString &message)
{
    if (error) *error = message;
    return false;
}

// The application data folder of Thoughts itself. thoughtsctl and the tests
// run under other application names, which QStandardPaths puts in the path.
QString thoughtsApplicationDataRoot()
{
#ifdef Q_OS_WIN
    return Platform::applicationDataRoot();
#else
    if (!QCoreApplication::instance() || QCoreApplication::applicationName() == QLatin1String("thoughts")) return Platform::applicationDataRoot();
    const QString name = QCoreApplication::applicationName();
    QCoreApplication::setApplicationName(QStringLiteral("thoughts"));
    const QString root = Platform::applicationDataRoot();
    QCoreApplication::setApplicationName(name);
    return root;
#endif
}

// One short-lived connection per call: the app, thoughtsctl and the
// publisher share the database file, which runs in WAL mode.
class ClaimDatabase
{
public:
    ClaimDatabase(const QString &path, bool create)
        : m_name(QStringLiteral("proposal-decisions-") + QUuid::createUuid().toString(QUuid::WithoutBraces))
    {
        if (path.isEmpty()) {
            m_error = QStringLiteral("No claim database.");
            return;
        }
        if (!create && !QFileInfo(path).isFile()) {
            m_missing = true;
            return;
        }
        if (create && !QDir().mkpath(QFileInfo(path).absolutePath())) {
            m_error = QStringLiteral("Cannot create the synchronization database folder.");
            return;
        }
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), m_name);
        database.setDatabaseName(path);
        if (!database.open()) {
            m_error = database.lastError().text();
            return;
        }
        m_open = true;
        QSqlQuery pragma(database);
        pragma.exec(QStringLiteral("PRAGMA busy_timeout=5000"));
        if (create) {
            pragma.exec(QStringLiteral("PRAGMA journal_mode=WAL"));
            if (!exec(QStringLiteral("CREATE TABLE IF NOT EXISTS %1(decision_id TEXT PRIMARY KEY, proposal_id TEXT NOT NULL, source_file TEXT NOT NULL, "
                                     "claim_json TEXT NOT NULL, recorded_at_ms INTEGER NOT NULL)").arg(QLatin1String(TableName)))) {
                return;
            }
        } else {
            QSqlQuery table(database);
            if (!table.exec(QStringLiteral("SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = '%1'").arg(QLatin1String(TableName)))) {
                m_error = table.lastError().text();
                return;
            }
            m_missing = !table.next();
        }
    }
    ~ClaimDatabase()
    {
        if (QSqlDatabase::contains(m_name)) {
            {
                QSqlDatabase database = QSqlDatabase::database(m_name, false);
                if (database.isOpen()) database.close();
            }
            QSqlDatabase::removeDatabase(m_name);
        }
    }
    bool ok() const { return m_open && m_error.isEmpty(); }
    // The database or its table does not exist yet: nothing is stored.
    bool missing() const { return m_missing; }
    QString error() const { return m_error; }
    QSqlDatabase database() const { return QSqlDatabase::database(m_name, false); }
    bool exec(const QString &statement)
    {
        QSqlQuery query(database());
        if (query.exec(statement)) return true;
        m_error = query.lastError().text();
        return false;
    }

private:
    QString m_name;
    QString m_error;
    bool m_open = false;
    bool m_missing = false;
};

QJsonObject commandError(const QString &code, const QString &message)
{
    const bool retryable = code == QLatin1String("busy") || code == QLatin1String("recovery_pending");
    return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), false}, {QStringLiteral("terminal"), !retryable},
            {QStringLiteral("state"), code == QLatin1String("busy") ? QStringLiteral("busy") : QStringLiteral("failed")},
            {QStringLiteral("error_code"), code}, {QStringLiteral("retryable"), retryable}, {QStringLiteral("message"), message}};
}

// 1-based line of an offset.
int lineOf(const QString &text, int offset)
{
    return int(QStringView(text).left(offset).count(QLatin1Char('\n'))) + 1;
}

// Offset of the start of 1-based line `line`, or the end of the text when it has fewer lines.
int lineStart(const QString &text, int line)
{
    int offset = 0;
    for (int current = 1; current < line; ++current) {
        const int newline = int(text.indexOf(QLatin1Char('\n'), offset));
        if (newline < 0) return int(text.size());
        offset = newline + 1;
    }
    return offset;
}

QString latexWrapper(const QString &id, const QJsonObject &snapshot)
{
    const QString created = snapshot.value(QStringLiteral("created_at")).toString();
    const QString conversation = snapshot.value(QStringLiteral("conversation_id")).toString();
    QString metadata;
    // §10.6: metadata is written as stored. Without both values the wrapper is
    // invalid_metadata (§4.3), so the re-parse below refuses not_restorable.
    if (!created.isEmpty()) {
        metadata = QStringLiteral("[created=") + created;
        if (!conversation.isEmpty()) metadata += QStringLiteral(",conversation_id=") + conversation;
        const QString url = snapshot.value(QStringLiteral("conversation_url")).toString();
        if (!url.isEmpty()) metadata += QStringLiteral(",conversation_url=") + url;
        metadata += QLatin1Char(']');
    }
    // The explanation is already TeX (§3.4): written as stored, not escaped again.
    return QStringLiteral("\\agentProposedEdit") + metadata + QLatin1Char('{') + id + QStringLiteral("}{") + snapshot.value(QStringLiteral("comment")).toString()
        + QStringLiteral("}{") + snapshot.value(QStringLiteral("before")).toString() + QStringLiteral("}{") + snapshot.value(QStringLiteral("after")).toString()
        + QLatin1Char('}');
}

// §8.2 with the stored explanation exactly (a Markdown proposal is always written as v2).
QString markdownWrapper(const QString &id, const QJsonObject &snapshot)
{
    const auto block = [](const QString &value) {
        QString lines;
        if (value.isEmpty()) return lines;
        for (QString line : value.split(QLatin1Char('\n'), Qt::KeepEmptyParts)) {
            line.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
            line.replace(QStringLiteral("-->"), QStringLiteral("--&gt;"));
            lines += QStringLiteral("| ") + line + QLatin1Char('\n');
        }
        return lines;
    };
    QString replacement = snapshot.value(QStringLiteral("after")).toString();
    if (!replacement.isEmpty() && !replacement.endsWith(QLatin1Char('\n'))) replacement += QLatin1Char('\n');
    const QString url = snapshot.value(QStringLiteral("conversation_url")).toString();
    return QStringLiteral("<!-- thoughts:agent-proposal:v2:start\n") + QStringLiteral("id: ") + id + QLatin1Char('\n') + QStringLiteral("created_at: ")
        + snapshot.value(QStringLiteral("created_at")).toString() + QLatin1Char('\n') + QStringLiteral("conversation_id: ")
        + snapshot.value(QStringLiteral("conversation_id")).toString() + QLatin1Char('\n')
        + (url.isEmpty() ? QString() : QStringLiteral("conversation_url: ") + url + QLatin1Char('\n')) + QStringLiteral("explanation:\n")
        + block(snapshot.value(QStringLiteral("comment")).toString()) + QStringLiteral("original_text:\n") + block(snapshot.value(QStringLiteral("before")).toString())
        + QStringLiteral("-->\n") + replacement + QStringLiteral("<!-- thoughts:agent-proposal:v2:end ") + id + QStringLiteral(" -->\n");
}

QString nowTimestamp()
{
    return formatAgentProposalTimestamp(QDateTime::currentDateTimeUtc());
}

// A Library source: inside the root, owned by a docs-* folder or a Project's
// default folder, a proposal format, and not excluded by .ThoughtIgnore.
bool isLibrarySource(const QString &canonicalRoot, const QString &canonicalPath)
{
    if (canonicalRoot.isEmpty() || canonicalPath.isEmpty() || !canonicalPath.startsWith(canonicalRoot + QLatin1Char('/')) || !isAgentProposalSourcePath(canonicalPath)) {
        return false;
    }
    const QString relative = QDir(canonicalRoot).relativeFilePath(canonicalPath);
    const QString owner = SourceOwner::of(relative);
    if (owner.isEmpty()) return false;
    const ThoughtIgnore ignored(QDir(canonicalRoot).filePath(owner));
    return ignored.error().isEmpty() && !ignored.excludes(relative.mid(owner.size() + 1));
}
}

QString databasePath(const QString &docsRoot)
{
    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    if (root.isEmpty() || root != QFileInfo(Thoughts::thoughtsHome()).canonicalFilePath()) return {};
    const QString data = thoughtsApplicationDataRoot();
    // Another Library chosen with THOUGHTS_HOME (a test's temporary one) does
    // not belong to the account's own Thoughts data: without a private HOME,
    // CFFIXED_USER_HOME or Qt test mode its claims would land in the real
    // database, which the real Library's Sync then carries.
    if (root != QFileInfo(Thoughts::defaultThoughtsHome()).canonicalFilePath() && Platform::isAccountDataRoot(data)) return {};
    return QDir(data).filePath(QStringLiteral("thoughts-server/desktop-sync.sqlite"));
}

bool claimApplies(const QString &docsRoot, const QString &sourceFile)
{
    return Publisher::isManagedLibrarySource(docsRoot, sourceFile).value_or(true);
}

QString wrapperSha256(const QString &source, const AgentProposedEdit &proposal)
{
    return digest(source.mid(proposal.offset, proposal.length).toUtf8());
}

QJsonObject newClaim(const QString &proposalId, const QString &action, const QString &wrapperSha256, const QString &sourceFile, const QString &origin)
{
    return {{QStringLiteral("decision_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("proposal_id"), proposalId},
            {QStringLiteral("action"), action},
            {QStringLiteral("wrapper_sha256"), wrapperSha256},
            {QStringLiteral("source_file"), sourceFile},
            {QStringLiteral("decided_at"), nowTimestamp()},
            {QStringLiteral("origin"), origin}};
}

bool isValidClaim(const QJsonObject &claim)
{
    const QString sourceFile = claim.value(QStringLiteral("source_file")).toString();
    return claim.size() == 7 && isUuid(claim.value(QStringLiteral("decision_id")).toString())
        && isValidAgentProposalId(claim.value(QStringLiteral("proposal_id")).toString())
        && claimActions().contains(claim.value(QStringLiteral("action")).toString()) && isSha256(claim.value(QStringLiteral("wrapper_sha256")).toString())
        && !sourceFile.isEmpty() && !QDir::isAbsolutePath(sourceFile) && !sourceFile.split(QLatin1Char('/')).contains(QStringLiteral(".."))
        && isValidAgentProposalTimestamp(claim.value(QStringLiteral("decided_at")).toString())
        && claimOrigins().contains(claim.value(QStringLiteral("origin")).toString());
}

bool recordClaims(const QString &database, const QList<QJsonObject> &claims, QString *error)
{
    if (claims.isEmpty()) return true;
    for (const QJsonObject &claim : claims) {
        if (!isValidClaim(claim)) return fail(error, QStringLiteral("Invalid proposal decision claim."));
    }
    ClaimDatabase db(database, true);
    if (!db.ok()) return fail(error, db.error());
    QSqlDatabase connection = db.database();
    if (!connection.transaction()) return fail(error, connection.lastError().text());
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    for (const QJsonObject &claim : claims) {
        QSqlQuery remove(connection);
        remove.prepare(QStringLiteral("DELETE FROM %1 WHERE proposal_id = ?").arg(QLatin1String(TableName)));
        remove.addBindValue(claim.value(QStringLiteral("proposal_id")).toString());
        QSqlQuery insert(connection);
        insert.prepare(QStringLiteral("INSERT INTO %1(decision_id, proposal_id, source_file, claim_json, recorded_at_ms) VALUES(?, ?, ?, ?, ?)").arg(QLatin1String(TableName)));
        insert.addBindValue(claim.value(QStringLiteral("decision_id")).toString());
        insert.addBindValue(claim.value(QStringLiteral("proposal_id")).toString());
        insert.addBindValue(claim.value(QStringLiteral("source_file")).toString());
        insert.addBindValue(QString::fromUtf8(QJsonDocument(claim).toJson(QJsonDocument::Compact)));
        insert.addBindValue(now);
        if (!remove.exec() || !insert.exec()) {
            const QString message = remove.lastError().isValid() ? remove.lastError().text() : insert.lastError().text();
            connection.rollback();
            return fail(error, message);
        }
    }
    if (!connection.commit()) {
        const QString message = connection.lastError().text();
        connection.rollback();
        return fail(error, message);
    }
    return true;
}

bool forgetClaims(const QString &database, const QStringList &decisionIds, QString *error)
{
    if (decisionIds.isEmpty()) return true;
    ClaimDatabase db(database, false);
    if (db.missing()) return true;
    if (!db.ok()) return fail(error, db.error());
    QSqlDatabase connection = db.database();
    if (!connection.transaction()) return fail(error, connection.lastError().text());
    for (const QString &id : decisionIds) {
        QSqlQuery remove(connection);
        remove.prepare(QStringLiteral("DELETE FROM %1 WHERE decision_id = ?").arg(QLatin1String(TableName)));
        remove.addBindValue(id);
        if (!remove.exec()) {
            const QString message = remove.lastError().text();
            connection.rollback();
            return fail(error, message);
        }
    }
    if (!connection.commit()) {
        const QString message = connection.lastError().text();
        connection.rollback();
        return fail(error, message);
    }
    return true;
}

QList<QJsonObject> storedClaims(const QString &database, QString *error, const QSet<QString> &paths)
{
    if (error) error->clear();
    QList<QJsonObject> claims;
    ClaimDatabase db(database, false);
    if (db.missing()) return claims;
    if (!db.ok()) {
        fail(error, db.error());
        return claims;
    }
    QSqlQuery query(db.database());
    if (!query.exec(QStringLiteral("SELECT source_file, claim_json FROM %1 ORDER BY rowid").arg(QLatin1String(TableName)))) {
        fail(error, query.lastError().text());
        return claims;
    }
    while (query.next()) {
        if (!paths.isEmpty() && !paths.contains(query.value(0).toString())) continue;
        const QJsonObject claim = QJsonDocument::fromJson(query.value(1).toString().toUtf8()).object();
        if (isValidClaim(claim)) claims.append(claim);
    }
    return claims;
}

ClaimSelection selectClaimsForUpload(const QList<QJsonObject> &claims, const QHash<QString, std::optional<QString>> &uploads)
{
    ClaimSelection selection;
    QHash<QString, QSet<QString>> idsByPath;
    QSet<QString> sentIds;
    for (const QJsonObject &claim : claims) {
        const QString path = claim.value(QStringLiteral("source_file")).toString();
        const auto upload = uploads.constFind(path);
        if (upload == uploads.cend()) continue; // waits for an upload of its source
        const QString proposalId = claim.value(QStringLiteral("proposal_id")).toString();
        if (upload->has_value()) {
            if (!idsByPath.contains(path)) idsByPath.insert(path, parseAgentProposals(path, **upload).ids());
            // The wrapper is back (a local Undo): the decision was undone.
            if (idsByPath.value(path).contains(proposalId)) {
                selection.dropped.append(claim.value(QStringLiteral("decision_id")).toString());
                continue;
            }
        }
        if (sentIds.contains(proposalId) || selection.sent.size() >= MaximumClaimsPerUpload) continue;
        sentIds.insert(proposalId);
        selection.sent.append(claim);
    }
    return selection;
}

QStringList decisionIds(const QList<QJsonObject> &claims)
{
    QStringList ids;
    for (const QJsonObject &claim : claims) ids.append(claim.value(QStringLiteral("decision_id")).toString());
    return ids;
}

QList<QJsonObject> claimsForConflictResolution(const QString &database, const QString &path, const QString &contents, QString *error)
{
    const QList<QJsonObject> stored = storedClaims(database, error, {path});
    if (stored.isEmpty()) return {};
    const ClaimSelection selection = selectClaimsForUpload(stored, {{path, contents}});
    QString forgetError;
    if (!forgetClaims(database, selection.dropped, &forgetError) && error) *error = forgetError;
    return selection.sent;
}

RestoreEdit restoreEdit(const QString &path, const std::optional<QString> &current, const QJsonObject &snapshot, int line)
{
    RestoreEdit edit;
    const QString id = snapshot.value(QStringLiteral("proposal_id")).toString();
    const auto refuse = [&edit](const char *code) {
        edit.code = QLatin1String(code);
        return edit;
    };
    const AgentProposalFormat format = agentProposalFormat(path);
    if (format == AgentProposalFormat::Unsupported || !isValidAgentProposalId(id)) return refuse("not_restorable");
    if (!current) return refuse("source_missing");
    const QString &text = *current;
    const AgentProposalParse parse = parseAgentProposals(format, text);
    // 1. Already back: it becomes pending with the next Sync.
    if (parse.proposal(id) && parse.refusalCode(id).isEmpty()) {
        edit.code = QStringLiteral("already_present");
        edit.line = parse.proposal(id)->line;
        return edit;
    }
    // 3. A broken wrapper or a mention of the ID: the user repairs it.
    if (text.contains(id)) return refuse("id_in_source");
    // 4. Whole-line occurrences of either text outside every wrapper and issue.
    QVector<QPair<int, int>> occupied;
    for (const AgentProposedEdit &proposal : parse.proposals) occupied.append({proposal.offset, proposal.offset + proposal.length});
    for (const AgentProposalIssue &issue : parse.issues) occupied.append({issue.offset, issue.offset + issue.length});
    const auto free = [&occupied](int start, int end) {
        return std::none_of(occupied.cbegin(), occupied.cend(), [&](const auto &span) { return start < span.second && span.first < end; });
    };
    QVector<QPair<int, int>> candidates;
    for (const QString &needle : {snapshot.value(QStringLiteral("after")).toString(), snapshot.value(QStringLiteral("before")).toString()}) {
        if (needle.isEmpty()) continue;
        for (qsizetype at = text.indexOf(needle); at >= 0; at = text.indexOf(needle, at + 1)) {
            const int start = int(at);
            const int end = start + int(needle.size());
            const bool startsLine = start == 0 || text.at(start - 1) == QLatin1Char('\n');
            const bool endsLine = end == text.size() || text.at(end - 1) == QLatin1Char('\n');
            if (startsLine && endsLine && free(start, end) && !candidates.contains({start, end})) candidates.append({start, end});
        }
    }
    const bool emptyText = snapshot.value(QStringLiteral("after")).toString().isEmpty() || snapshot.value(QStringLiteral("before")).toString().isEmpty();
    if (candidates.isEmpty() && emptyText) {
        const int start = lineStart(text, qMax(1, line));
        candidates.append({start, start});
    }
    // 5. The candidate nearest to the line where the wrapper was.
    if (candidates.isEmpty()) return refuse("text_changed");
    int best = -1;
    int bestDistance = 0;
    bool tie = false;
    for (int index = 0; index < candidates.size(); ++index) {
        const int distance = std::abs(lineOf(text, candidates.at(index).first) - line);
        if (best < 0 || distance < bestDistance) {
            best = index;
            bestDistance = distance;
            tie = false;
        } else if (distance == bestDistance) {
            tie = true;
        }
    }
    if (tie) return refuse("ambiguous");
    // 6. The wrapper as §8 writes it, from the stored snapshot.
    const auto [start, end] = candidates.at(best);
    const QString wrapper = format == AgentProposalFormat::Latex ? latexWrapper(id, snapshot) : markdownWrapper(id, snapshot);
    QString output = text;
    output.replace(start, end - start, wrapper);
    // 7. It must read back as exactly this proposal, with no new issue.
    const AgentProposalParse restored = parseAgentProposals(format, output);
    const AgentProposedEdit *proposal = restored.proposal(id);
    QString expectedAfter = snapshot.value(QStringLiteral("after")).toString();
    if (format == AgentProposalFormat::Markdown && !expectedAfter.isEmpty() && !expectedAfter.endsWith(QLatin1Char('\n'))) expectedAfter += QLatin1Char('\n');
    const bool exact = proposal && restored.refusalCode(id).isEmpty() && restored.proposals.size() == parse.proposals.size() + 1
        && restored.issues.size() == parse.issues.size() && proposal->explanation == snapshot.value(QStringLiteral("comment")).toString()
        && proposal->original == snapshot.value(QStringLiteral("before")).toString() && proposal->replacement == expectedAfter
        && proposal->createdAt == snapshot.value(QStringLiteral("created_at")).toString()
        && proposal->conversationId == snapshot.value(QStringLiteral("conversation_id")).toString()
        && proposal->conversationUrl == snapshot.value(QStringLiteral("conversation_url")).toString();
    if (!exact) return refuse("not_restorable");
    edit.output = output;
    edit.line = proposal->line;
    return edit;
}

QString restoreRefusalMessage(const QString &code)
{
    if (code == QLatin1String("already_present")) return QStringLiteral("The proposal is already in its source; it is pending again after the next Sync.");
    if (code == QLatin1String("source_missing")) return QStringLiteral("Its source file no longer exists.");
    if (code == QLatin1String("id_in_source"))
        return QStringLiteral("The proposal's ID still occurs in its source, in a broken wrapper or as text; repair it by hand.");
    if (code == QLatin1String("ambiguous"))
        return QStringLiteral("The proposed or original text occurs at several places equally near; Thoughts cannot tell which one the proposal covered.");
    if (code == QLatin1String("text_changed"))
        return QStringLiteral("The proposed and the original text were both edited since the markup was removed; Thoughts cannot tell where the proposal belongs.");
    if (code == QLatin1String("not_restorable")) return QStringLiteral("The proposal cannot be written back so that it reads back unchanged.");
    return QStringLiteral("The proposal cannot be restored (%1).").arg(code);
}

bool isRemovedOutside(const QJsonObject &row)
{
    return row.value(QStringLiteral("status")).toString() == QLatin1String("removed_outside") && !row.value(QStringLiteral("deleted")).toBool()
        && isValidAgentProposalId(row.value(QStringLiteral("proposal_id")).toString());
}

QJsonObject dismissedPayload(const QJsonObject &row)
{
    QJsonObject payload = row;
    payload.insert(QStringLiteral("status"), QStringLiteral("dismissed"));
    payload.insert(QStringLiteral("kind"), QStringLiteral("dismissed"));
    payload.insert(QStringLiteral("deleted"), true);
    return payload;
}

QString copyableText(const QJsonObject &row)
{
    const QJsonObject snapshot = row.value(QStringLiteral("snapshot")).toObject();
    const QString file = row.value(QStringLiteral("source_file")).toString(snapshot.value(QStringLiteral("source_file")).toString());
    const int line = row.value(QStringLiteral("removal")).toObject().value(QStringLiteral("source_line")).toInt();
    QString text = QStringLiteral("Proposal %1 in %2").arg(row.value(QStringLiteral("proposal_id")).toString(), line > 0 ? QStringLiteral("%1:%2").arg(file).arg(line) : file);
    text += QStringLiteral("\n\nExplanation:\n") + snapshot.value(QStringLiteral("comment")).toString().trimmed();
    text += QStringLiteral("\n\nOriginal text:\n") + snapshot.value(QStringLiteral("before")).toString();
    text += QStringLiteral("\n\nProposed text:\n") + snapshot.value(QStringLiteral("after")).toString();
    return text;
}

QJsonObject restoreRemovedProposal(const QString &docsRoot, const QJsonObject &row)
{
    const QJsonObject snapshot = row.value(QStringLiteral("snapshot")).toObject();
    const QString sourceFile = row.value(QStringLiteral("source_file")).toString(snapshot.value(QStringLiteral("source_file")).toString());
    const int line = row.value(QStringLiteral("removal")).toObject().value(QStringLiteral("source_line")).toInt();
    const QString location = line > 0 ? QStringLiteral("%1:%2").arg(sourceFile).arg(line) : sourceFile;
    QJsonObject result{{QStringLiteral("success"), false}, {QStringLiteral("proposal_id"), row.value(QStringLiteral("proposal_id"))},
                       {QStringLiteral("source_file"), sourceFile}, {QStringLiteral("line"), line}};
    const auto refuse = [&](const QString &code, const QString &message) {
        result.insert(QStringLiteral("code"), code);
        result.insert(QStringLiteral("message"), message);
        return result;
    };
    if (!isRemovedOutside(row) || snapshot.value(QStringLiteral("proposal_id")).toString() != row.value(QStringLiteral("proposal_id")).toString())
        return refuse(QStringLiteral("not_restorable"), QStringLiteral("This proposal is no longer removed outside Thoughts."));
    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    const QStringList parts = sourceFile.split(QLatin1Char('/'));
    if (root.isEmpty() || sourceFile.isEmpty() || QDir::isAbsolutePath(sourceFile) || parts.contains(QStringLiteral("..")) || parts.contains(QString()))
        return refuse(QStringLiteral("not_restorable"), QStringLiteral("The proposal's source path is invalid."));
    const QString path = QDir(root).filePath(sourceFile);
    std::optional<QString> current;
    QByteArray before;
    if (QFileInfo(path).isFile()) {
        const QString canonical = QFileInfo(path).canonicalFilePath();
        if (!canonical.startsWith(root + QLatin1Char('/'))) return refuse(QStringLiteral("not_restorable"), QStringLiteral("The proposal's source is outside the Library."));
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return refuse(QStringLiteral("not_restorable"), QStringLiteral("%1 cannot be read.").arg(sourceFile));
        before = file.readAll();
        current = QString::fromUtf8(before);
        if (current->toUtf8() != before) return refuse(QStringLiteral("not_restorable"), QStringLiteral("%1 is not valid UTF-8.").arg(sourceFile));
    }
    const RestoreEdit edit = restoreEdit(path, current, snapshot, line);
    if (edit.code == QLatin1String("already_present")) {
        result.insert(QStringLiteral("success"), true);
        result.insert(QStringLiteral("code"), edit.code);
        result.insert(QStringLiteral("line"), edit.line);
        result.insert(QStringLiteral("message"), QStringLiteral("Proposal already in %1:%2; it is pending again after the next Sync.").arg(sourceFile).arg(edit.line));
        return result;
    }
    if (!edit.code.isEmpty()) return refuse(edit.code, restoreRefusalMessage(edit.code) + QStringLiteral(" (%1)").arg(location));
    const QByteArray after = edit.output.toUtf8();
    QJsonObject eventSnapshot = snapshot;
    eventSnapshot.insert(QStringLiteral("source_file"), sourceFile);
    const QJsonArray events{ProposalHistory::event(QStringLiteral("restored"), eventSnapshot, digest(before), digest(after))};
    QString error;
    if (!ProposalHistory::writeSource(root, QFileInfo(path).canonicalFilePath(), before, after, events, &error))
        return refuse(QStringLiteral("write_failed"), QStringLiteral("%1 could not be written: %2").arg(sourceFile, error));
    // Verify what is on disk; put the previous text back when it is not exactly the restored proposal.
    QFile written(path);
    const QByteArray onDisk = written.open(QIODevice::ReadOnly) ? written.readAll() : QByteArray();
    written.close();
    const QString id = row.value(QStringLiteral("proposal_id")).toString();
    const AgentProposalParse parse = parseAgentProposals(path, QString::fromUtf8(onDisk));
    if (onDisk != after || !parse.proposal(id) || !parse.refusalCode(id).isEmpty()) {
        QString rollbackDetail;
        // Only our own write is undone; a later edit by someone else is kept.
        if (onDisk == after && !Platform::replaceFileContents(path, before)) rollbackDetail = QStringLiteral(" Its previous text could not be put back.");
        return refuse(QStringLiteral("not_restorable"), QStringLiteral("The restored proposal could not be verified; %1 was left unchanged.%2").arg(sourceFile, rollbackDetail));
    }
    result.insert(QStringLiteral("success"), true);
    result.insert(QStringLiteral("code"), QString());
    result.insert(QStringLiteral("line"), edit.line);
    result.insert(QStringLiteral("source_sha256"), digest(after));
    result.insert(QStringLiteral("message"), QStringLiteral("Proposal restored at %1:%2; it is pending again after the next Sync.").arg(sourceFile).arg(edit.line));
    return result;
}

QJsonObject decideProposal(const QString &docsRoot, const QJsonObject &request, const QString &claimDatabase)
{
    static const QSet<QString> allowed{QStringLiteral("schema_version"), QStringLiteral("command"), QStringLiteral("request_id"), QStringLiteral("source"),
                                       QStringLiteral("expected_source_sha256"), QStringLiteral("proposal_id"), QStringLiteral("wrapper_sha256"),
                                       QStringLiteral("action"), QStringLiteral("origin")};
    for (auto it = request.begin(); it != request.end(); ++it) {
        if (!allowed.contains(it.key())) return commandError(QStringLiteral("invalid_request"), QStringLiteral("Unexpected request field: %1").arg(it.key()));
    }
    for (const QString &key : allowed) {
        if (key != QLatin1String("schema_version") && key != QLatin1String("command") && !request.value(key).isString())
            return commandError(QStringLiteral("invalid_request"), QStringLiteral("Missing or invalid text field: %1").arg(key));
    }
    const QString requestId = request.value(QStringLiteral("request_id")).toString();
    const QString source = request.value(QStringLiteral("source")).toString();
    const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
    const QString id = request.value(QStringLiteral("proposal_id")).toString();
    const QString wrapper = request.value(QStringLiteral("wrapper_sha256")).toString();
    const QString action = request.value(QStringLiteral("action")).toString();
    const QString origin = request.value(QStringLiteral("origin")).toString();
    // Whole decisions only (§7): there are no partial decisions through thoughtsctl,
    // and agents never decide (MCP does not expose this command).
    if (action != QLatin1String("accept") && action != QLatin1String("reject"))
        return commandError(QStringLiteral("invalid_request"), QStringLiteral("action must be accept or reject."));
    if (origin != QLatin1String("vscode") && origin != QLatin1String("thoughtsctl"))
        return commandError(QStringLiteral("invalid_request"), QStringLiteral("origin must be vscode or thoughtsctl."));
    if (!isUuid(requestId) || !isSha256(expected) || !isSha256(wrapper) || !isValidAgentProposalId(id) || source.contains(QChar::Null) || !QDir::isAbsolutePath(source))
        return commandError(QStringLiteral("invalid_request"),
                            QStringLiteral("Provide a UUID request_id, an absolute source path, SHA-256 source and wrapper versions and a valid proposal ID."));

    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    if (root.isEmpty()) return commandError(QStringLiteral("not_in_library"), QStringLiteral("The Thoughts Library is unavailable."));
    if (!QDir().mkpath(ProposalHistory::directory(root))) return commandError(QStringLiteral("write_failed"), QStringLiteral("Cannot create the proposal history folder."));
    // The same lock and retry record as proposal authoring in thoughtsctl.
    QLockFile lock(QDir(ProposalHistory::directory(root)).filePath(QStringLiteral("proposal_authoring.lock")));
    if (!lock.tryLock(5000)) return commandError(QStringLiteral("busy"), QStringLiteral("Another proposal operation is running; try again."));
    QString detail;
    if (!ProposalHistory::recover(root, &detail)) return commandError(QStringLiteral("recovery_pending"), detail);
    QJsonObject identity = request;
    identity.remove(QStringLiteral("schema_version"));
    const QString requestHash = digest(QJsonDocument(identity).toJson(QJsonDocument::Compact));
    if (const QJsonObject previous = ProposalHistory::agentRequests(root).value(requestId).toObject(); !previous.isEmpty()) {
        if (previous.value(QStringLiteral("sha256")).toString() != requestHash)
            return commandError(QStringLiteral("request_conflict"), QStringLiteral("This request ID was already used with different arguments."));
        QJsonObject result = previous.value(QStringLiteral("result")).toObject();
        result.insert(QStringLiteral("replayed"), true);
        return result;
    }

    const QString path = QFileInfo(source).canonicalFilePath();
    if (!QFileInfo(source).isFile() || !isLibrarySource(root, path))
        return commandError(QStringLiteral("not_in_library"), QStringLiteral("The file is not a .tex or Markdown source of the Thoughts Library."));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return commandError(QStringLiteral("read_failed"), file.errorString());
    const QByteArray bytes = file.readAll();
    file.close();
    const auto changed = [&](const QString &message) {
        QJsonObject result = commandError(QStringLiteral("source_changed"), message);
        result.insert(QStringLiteral("current_source_sha256"), digest(bytes));
        return result;
    };
    if (digest(bytes) != expected) return changed(QStringLiteral("The source changed since it was saved; reload it and decide again."));
    const QString text = QString::fromUtf8(bytes);
    const AgentProposalParse parse = parseAgentProposals(path, text);
    if (const QString refusal = parse.refusalCode(id); !refusal.isEmpty()) {
        QJsonObject result = commandError(refusal, refusal == QLatin1String(AgentProposalIssueCode::UnknownId)
                                                       ? QStringLiteral("No pending proposal with this ID is in the source.")
                                                       : agentProposalIssueMessage(refusal));
        result.insert(QStringLiteral("proposal_id"), id);
        return result;
    }
    if (wrapperSha256(text, *parse.proposal(id)) != wrapper) return changed(QStringLiteral("The proposal changed since it was shown; reload it and decide again."));

    const QString database = claimDatabase.isEmpty() ? databasePath(root) : claimDatabase;
    const QString relative = QDir(root).relativeFilePath(path);
    const QString claim = database.isEmpty() ? QStringLiteral("not_recorded") : (claimApplies(root, relative) ? QStringLiteral("recorded") : QStringLiteral("not_applicable"));
    const bool accept = action == QLatin1String("accept");
    const QJsonObject result{{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                             {QStringLiteral("state"), accept ? QStringLiteral("accepted") : QStringLiteral("rejected")},
                             {QStringLiteral("proposal_id"), id}, {QStringLiteral("source"), path},
                             {QStringLiteral("previous_source_sha256"), expected}, {QStringLiteral("request_id"), requestId},
                             {QStringLiteral("claim"), claim},
                             {QStringLiteral("sync"), QStringLiteral("pending")}, {QStringLiteral("replayed"), false},
                             {QStringLiteral("message"), QString((accept ? QStringLiteral("Proposal accepted") : QStringLiteral("Proposal rejected"))
                                                                     + (claim == QLatin1String("not_applicable")
                                                                            ? QStringLiteral("; Sync does not manage this file, so no decision is recorded.")
                                                                            : QStringLiteral("; the next Sync records the decision.")))}};
    AgentProposalDecisionOptions options;
    options.origin = origin;
    options.expectedWrapperSha256 = wrapper;
    options.claimDatabase = database;
    options.agentRequest = {{QStringLiteral("agent_request_id"), requestId}, {QStringLiteral("agent_request_sha256"), requestHash},
                            {QStringLiteral("agent_result"), result}};
    QString error, code, updated;
    QList<AgentProposalSourceChange> others;
    if (!resolveLiveAgentProposedEdits(root, path, expected, {id}, action, &error, nullptr, &updated, &code, options, &others)) {
        if (!code.isEmpty()) {
            QJsonObject refusal = code == QLatin1String("source_changed") ? changed(error) : commandError(code, error);
            refusal.insert(QStringLiteral("proposal_id"), id);
            return refusal;
        }
        if (error.contains(QLatin1String("Another proposal edit")) || error.contains(QLatin1String("busy")))
            return commandError(QStringLiteral("busy"), error);
        return commandError(QStringLiteral("write_failed"), error.isEmpty() ? QStringLiteral("The decision could not be saved.") : error);
    }
    QJsonObject answer = result;
    answer.insert(QStringLiteral("source_sha256"), updated);
    // The other half of a move pair, decided with it in its own file (§12).
    if (!others.isEmpty()) {
        QJsonArray written;
        for (const auto &other : std::as_const(others))
            written.append(QJsonObject{{QStringLiteral("source"), other.path}, {QStringLiteral("source_sha256"), other.afterSha256}});
        answer.insert(QStringLiteral("other_sources"), written);
    }
    return answer;
}

}
