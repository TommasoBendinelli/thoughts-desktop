/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_AGENTPROPOSALPARSER_H
#define THOUGHTS_AGENTPROPOSALPARSER_H

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QSet>
#include <QVector>

namespace Thoughts
{

// Agent proposals stored in sources, as specified by the Thoughts server's
// protocol/agent-proposals.md (version 1). One format-neutral model with a
// LaTeX and a Markdown codec; the codec is chosen by file extension only.
// Offsets here are UTF-16 code units of the decoded QString; the spec and its
// fixtures count code points, so convert when exchanging offsets.

enum class AgentProposalFormat {
    Unsupported,
    Latex,
    Markdown,
};

// §2: .tex is LaTeX; .md and .markdown are Markdown (case-insensitive).
AgentProposalFormat agentProposalFormat(const QString &path);
bool isAgentProposalSourcePath(const QString &path);

struct AgentProposedEdit
{
    QString id;
    QString createdAt;          // exactly as written (always present: metadata is mandatory)
    QString conversationId;     // lowercase UUID (always present)
    QString conversationUrl;    // §3.6 link to the conversation, empty when not written
    QString explanation;        // LaTeX: argument 2 exactly as written
    QString original;           // replaced in on reject
    QString replacement;        // replaced in on accept
    int offset = 0;             // span of the whole wrapper
    int length = 0;
    int line = 0;               // 1-based line of the wrapper start
    // LaTeX only: contents of the explanation and replacement arguments.
    int explanationOffset = 0;
    int explanationLength = 0;
    int replacementOffset = 0;
    int replacementLength = 0;
};

// Stable issue codes of §6, in precedence order.
namespace AgentProposalIssueCode
{
inline constexpr char UnclosedGroup[] = "unclosed_group";
inline constexpr char MissingArgument[] = "missing_argument";
inline constexpr char MalformedMarker[] = "malformed_marker";
inline constexpr char UnmatchedMarker[] = "unmatched_marker";
inline constexpr char InvalidMetadata[] = "invalid_metadata";
inline constexpr char InvalidId[] = "invalid_id";
inline constexpr char NestedProposal[] = "nested_proposal";
inline constexpr char DuplicateId[] = "duplicate_id";
// Decision refusal for an ID no issue carries (§7).
inline constexpr char UnknownId[] = "unknown_id";
}

struct AgentProposalIssue
{
    QString code;
    QString id;         // meaningful only when hasId
    bool hasId = false; // the wrapper's ID text was read (it may be invalid or empty)
    int offset = 0;
    int length = 0;
    int line = 0;
};

struct AgentProposalParse
{
    QVector<AgentProposedEdit> proposals; // valid proposals, in source order
    QVector<AgentProposalIssue> issues;   // one per broken wrapper, in source order

    const AgentProposedEdit *proposal(const QString &id) const;
    // The code a decision on `id` is refused with, or "" when `id` names exactly one valid proposal.
    QString refusalCode(const QString &id) const;
    // IDs read from any wrapper, valid or broken (orphan end markers included).
    QSet<QString> ids() const;
};

AgentProposalParse parseLatexAgentProposals(const QString &source);
AgentProposalParse parseMarkdownAgentProposals(const QString &source);
// Dispatches by extension; other files have no proposals and no issues.
AgentProposalParse parseAgentProposals(const QString &path, const QString &source);
AgentProposalParse parseAgentProposals(AgentProposalFormat format, const QString &source);

// §7. `action` is "accept", or "reject" (also spelled "disregard"). On refusal
// nothing is produced and `code` names the spec refusal code.
bool decideAgentProposals(const QString &path, const QString &source, const QStringList &ids, const QString &action,
                          QString *result, QString *code = nullptr, QString *message = nullptr);

// A short explanation for people, e.g. in the Comments view.
QString agentProposalIssueMessage(const QString &code);
QString agentProposalIssueMessage(const AgentProposalIssue &issue);

// Move pairs (protocol/agent-proposals.md §12). A passage moved with its
// review-card markers is two proposals, "agent-<uuid>.cut" where it was and
// "agent-<uuid>.paste" where it goes, possibly in two files. Clients decide
// them together: a decision naming one half decides both, and a half whose
// partner is not pending is refused (move_incomplete).
namespace AgentProposalMove
{
inline constexpr char CutSuffix[] = ".cut";
inline constexpr char PasteSuffix[] = ".paste";
// Refusal codes of §12.
inline constexpr char Incomplete[] = "move_incomplete";
inline constexpr char Ambiguous[] = "move_ambiguous";
// The other half's ID, or "" when `id` is not a half of a move pair.
QString partner(const QString &id);
bool isHalf(const QString &id);
bool isCut(const QString &id);
// A new pair: {"agent-<uuid>.cut", "agent-<uuid>.paste"}.
QStringList newPair();
}

// The pending proposal `id` in the sources of the Library `docsRoot`: its
// canonical path and parse, searched in every .tex/.md/.markdown file outside
// hidden folders. `found` is false when none has it; `ambiguous` when more
// than one valid wrapper does.
struct AgentProposalLocation {
    bool found = false;
    bool ambiguous = false;
    QString path;
    QByteArray bytes;
    AgentProposedEdit proposal;
};
AgentProposalLocation locateAgentProposal(const QString &docsRoot, const QString &id, const QStringList &skipPaths = {});

// §3.2.
bool isValidAgentProposalTimestamp(const QString &value);
QDateTime agentProposalTimestampDateTime(const QString &value); // invalid when not a valid timestamp
QString formatAgentProposalTimestamp(const QDateTime &value);   // YYYY-MM-DDTHH:MM:SS.mmmZ
bool isValidAgentProposalId(const QString &value);
bool isValidAgentProposalConversationId(const QString &value);
// §3.6: https://claude.ai/code/session_<1-64 letters/digits> or codex://threads/<lowercase uuid>.
bool isValidAgentProposalConversationUrl(const QString &value);

// §8 writers. Each returns an empty string and sets `refusal` when the fields
// cannot be written as a wrapper that reads back unchanged:
//   invalid_id, invalid_metadata (timestamp, conversation ID or link, blank explanation),
//   nested_proposal (original or replacement contains a wrapper or marker),
//   unbalanced_braces, unterminated_comment (LaTeX original or replacement).
// The LaTeX writer escapes the explanation (§8.1) and appends a newline; the
// Markdown writer appends a newline to a non-empty replacement without one.
QString formatLatexAgentProposal(const AgentProposedEdit &proposal, QString *refusal = nullptr);
QString formatMarkdownAgentProposal(const AgentProposedEdit &proposal, QString *refusal = nullptr);
QString escapeLatexProposalExplanation(const QString &text);
// Checks `text` as an original or replacement of `format` (§8): "" when it can
// be written, else nested_proposal, unbalanced_braces or unterminated_comment.
QString agentProposalTextRefusal(AgentProposalFormat format, const QString &text);

// LaTeX: the ranges TeX does not show of each live wrapper, valid or broken:
// from the command to the end of its original argument. The replacement stays
// visible, and wrappers inside it are scanned as TeX expands them. A wrapper
// missing an argument is left as text; an unclosed group hides the rest of the
// file. `incomplete` reports either case. Used by the comment and navigation
// projections, so they locate wrappers exactly as the reader does.
QVector<QPair<int, int>> latexAgentProposalHiddenRanges(const QString &source, bool *incomplete = nullptr);

// Generated sources take no new or revised proposals (protocol/agent-proposals.md §8):
// a path with a tex/generated/ segment (`relativePath` is relative to the Library
// or its project folder), or a first non-empty line that starts with
// "% Generated artifact" or "% Generated by " (Markdown: "<!-- Generated artifact"
// or "<!-- Generated by "). Existing proposals in them are read and decided as usual.
bool isGeneratedAgentProposalSource(const QString &relativePath, const QString &text);

// Observations are durable metadata; they never replace the live TeX proposal.
bool observeAgentProposalSource(const QString &docsRoot, const QString &sourcePath, const QByteArray &source,
                                QString *error = nullptr, QHash<QString, QJsonObject> *latestBatch = nullptr);

// How a local decision is recorded (§10.5): every decided wrapper gets a
// decision claim in the desktop sync database, written before the source.
struct AgentProposalDecisionOptions {
    // §10.4.2 origin: desktop, vscode or thoughtsctl.
    QString origin = QStringLiteral("desktop");
    // When set, the decision is refused with source_changed unless every
    // decided wrapper still has this SHA-256 (§10.3), as the user saw it.
    QString expectedWrapperSha256;
    // A retry record (agent_request_id, agent_request_sha256, agent_result)
    // promoted with the source write; agent_result gains source_sha256.
    QJsonObject agentRequest;
    // The claim database; empty uses ProposalDecisions::databasePath(docsRoot).
    QString claimDatabase;
};

// Another file a decision wrote: the other half of a move pair (§12).
struct AgentProposalSourceChange {
    QString path;          // canonical
    QByteArray before;     // for Undo
    QString afterSha256;
};

// Decides `ids` in `sourcePath`. The other half of each move pair among them
// is decided too: in the same file, or in another source of the Library,
// which is then written together with this one (one journal, rolled forward
// after a crash) and reported in `otherSources`. A half whose partner is not
// pending is refused with move_incomplete, one found twice with move_ambiguous.
bool resolveLiveAgentProposedEdits(const QString &docsRoot,
                                   const QString &sourcePath,
                                   const QString &expectedSourceSha256,
                                   const QStringList &ids,
                                   const QString &action,
                                   QString *error = nullptr,
                                   QByteArray *previousContents = nullptr,
                                   QString *updatedSha256 = nullptr,
                                   QString *errorCode = nullptr,
                                   const AgentProposalDecisionOptions &options = {},
                                   QList<AgentProposalSourceChange> *otherSources = nullptr);
bool restoreLiveAgentProposalSource(const QString &docsRoot,
                                    const QString &sourcePath,
                                    const QString &expectedSourceSha256,
                                    const QByteArray &contents,
                                    QString *error = nullptr);
// Undo of a decision that also wrote `others` (a move pair across files):
// every file is restored, or none.
bool restoreLiveAgentProposalSources(const QString &docsRoot,
                                     const QString &sourcePath,
                                     const QString &expectedSourceSha256,
                                     const QByteArray &contents,
                                     const QList<AgentProposalSourceChange> &others,
                                     QString *error = nullptr);
QSet<QString> locallyResolvedAgentProposalIds(const QString &sourceBundleRoot);

}

#endif
