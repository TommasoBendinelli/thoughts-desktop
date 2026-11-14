/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "proposalauthoring.h"
#include "agentproposalparser.h"
#include "proposaldecisions.h"
#include "proposallinks.h"
#include "reviewdocumenttools.h"
#include "thoughtscommandprotocol.h"
#include "../part/proposalhistory.h"

#include <QDirIterator>
#include <QJsonArray>
#include <QRegularExpression>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace {
QJsonObject error(const QString &code, const QString &message) { return Thoughts::syncCommandError(code, message); }
// A pending proposal the request names is broken or absent: the spec's refusal code (§6, §7).
QJsonObject proposalError(const Thoughts::AgentProposalParse &parse, const QString &id)
{
    const QString code = parse.refusalCode(id);
    auto result = error(code, code == QLatin1String(Thoughts::AgentProposalIssueCode::UnknownId)
                                  ? QStringLiteral("The pending proposal was not found in this source.")
                                  : Thoughts::agentProposalIssueMessage(code));
    result.insert(QStringLiteral("proposal_id"), id);
    return result;
}
// The text cannot be written as a wrapper (§8): reason is the writer's refusal code.
QJsonObject writerError(const QString &reason)
{
    auto result = error(QStringLiteral("invalid_proposal"), reason == QLatin1String(Thoughts::AgentProposalIssueCode::NestedProposal)
                            ? QStringLiteral("Nested proposals are not allowed; provide only the proposed replacement text.")
                            : Thoughts::agentProposalIssueMessage(reason));
    result.insert(QStringLiteral("reason"), reason);
    return result;
}
bool validUuid(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"));
    return pattern.match(value).hasMatch() && !QUuid(value).isNull();
}
bool positiveInteger(const QJsonValue &value)
{
    return value.isDouble() && value.toDouble() >= 1 && value.toDouble() <= 2147483647 && std::floor(value.toDouble()) == value.toDouble();
}
// Offsets of the line starts of `text`, plus its end when it has no final newline.
QVector<int> lineBoundaries(const QString &text)
{
    QVector<int> boundaries {0};
    for (int i = 0; i < text.size(); ++i) if (text.at(i) == QLatin1Char('\n')) boundaries.append(i + 1);
    if (boundaries.last() != text.size()) boundaries.append(text.size());
    return boundaries;
}
// The spans of every wrapper and broken wrapper of a parse.
QVector<QPair<int, int>> occupiedSpans(const Thoughts::AgentProposalParse &parse)
{
    QVector<QPair<int, int>> occupied;
    for (const auto &proposal : parse.proposals) occupied.append({proposal.offset, proposal.offset + proposal.length});
    for (const auto &issue : parse.issues) occupied.append({issue.offset, issue.offset + issue.length});
    return occupied;
}
// A workspace source other than those in `skip` with a wrapper (valid or broken)
// carrying one of `ids`; hidden files and snapshots count (§8).
QString sourceWithAnyId(const QString &root, const QStringList &skip, const QStringList &ids, QString *readError)
{
    QDirIterator sources(root, {QStringLiteral("*.tex"), QStringLiteral("*.md"), QStringLiteral("*.markdown")}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (sources.hasNext()) {
        const QString other = sources.next();
        const QString canonicalOther = QFileInfo(other).canonicalFilePath();
        if (skip.contains(canonicalOther) || !canonicalOther.startsWith(root + QLatin1Char('/'))) continue;
        QFile candidate(other);
        if (!candidate.open(QIODevice::ReadOnly)) { *readError = other; return {}; }
        const QByteArray content = candidate.readAll();
        if (std::none_of(ids.cbegin(), ids.cend(), [&](const QString &id) { return content.contains(id.toUtf8()); })) continue;
        const QSet<QString> otherIds = Thoughts::parseAgentProposals(canonicalOther, QString::fromUtf8(content)).ids();
        if (std::any_of(ids.cbegin(), ids.cend(), [&](const QString &id) { return otherIds.contains(id); })) return other;
    }
    return {};
}

// ---------------------------------------------------------------- review card marker lines
// A review card's marker (\ReviewCardMaskStart{<card>}{<n>}% and \ReviewCardMaskEnd{<card>}{<n>}%)
// stands alone on its line: card tools (merging, moving, reading a card's passage) refuse a source
// where one shares its line with other text, a pending proposal's braces included. The rules and
// messages are thoughts-cli's too (authoring.py, lay_out_beside_card_markers).
const QLatin1String CardMarkerLineCode("card_marker_line");
bool isCardMarkerLine(const QString &line)
{
    static const QRegularExpression marker(QStringLiteral(R"(\A[ \t]*\\ReviewCardMask(?:Start|End)\{[^{}\n]*\}\{[^{}\n]*\}%?[ \t]*\r?\z)"));
    return line.contains(QLatin1String("ReviewCardMask")) && marker.match(line).hasMatch();
}
// The line that starts at `start`, without its LF.
QString lineAt(const QString &text, int start)
{
    const int end = int(text.indexOf(QLatin1Char('\n'), start));
    return end < 0 ? text.mid(start) : text.mid(start, end - start);
}
int lineOf(const QString &text, int offset) { return int(QStringView(text).left(offset).count(QLatin1Char('\n'))) + 1; }
// Whether TeX would not read text appended to this line as written: the line has a comment,
// or ends in a backslash that would escape the first appended character.
bool hidesAppendedText(const QString &line)
{
    for (int i = 0; i < line.size(); ++i) {
        if (line.at(i) == QLatin1Char('%')) return true;
        if (line.at(i) == QLatin1Char('\\')) {
            if (i + 1 == line.size()) return true;
            ++i;
        }
    }
    return false;
}
bool latexRefuses(const QString &text) { return !Thoughts::agentProposalTextRefusal(Thoughts::AgentProposalFormat::Latex, text).isEmpty(); }
QJsonObject cardMarkerError(const QString &message, int line, const QString &reason)
{
    auto result = error(CardMarkerLineCode, message + QStringLiteral(" Nothing was written."));
    result.insert(QStringLiteral("line"), line);
    result.insert(QStringLiteral("reason"), reason);
    return result;
}
// One proposal of a create request: its range and texts as given, and where its wrapper goes.
struct WrapperLayout {
    int start = 0, end = 0;  // the range of whole lines
    QString original, replacement;
    int wrapStart = 0, wrapEnd = 0;  // the wrapper's span and arguments
    QString wrapOriginal, wrapReplacement;
};
// Where each new LaTeX wrapper goes, so that every review card marker stays alone on its line.
// A wrapper replaces its whole lines exactly. When they end right before a card marker line, its
// closing brace would sit on the marker's line, so the wrapper ends before their last line break
// instead, which then follows it (2026-11-13):
// - a replacement: the original and the replacement both leave out their final line break;
// - a deletion or an insertion (one side empty): the wrapper takes the line break before its lines
//   instead and starts at the end of the previous line, which must be no card marker and must not
//   hide appended text (a comment).
// Accepted or rejected, the source reads exactly as with the plain wrapper. A proposal of the same
// request that ends where such a wrapper now starts ends early as well. Returns the refusal
// (card_marker_line, with what to do instead), or an empty object. `order` lists the layouts by range.
QJsonObject layOutBesideCardMarkers(const QString &source, QVector<WrapperLayout> &layouts, const QVector<int> &order, bool batch)
{
    for (auto &layout : layouts) {
        layout.wrapStart = layout.start;
        layout.wrapEnd = layout.end;
        layout.wrapOriginal = layout.original;
        layout.wrapReplacement = layout.replacement;
    }
    if (!source.contains(QLatin1String("ReviewCardMask"))) return {};
    int markerAt = -1;
    for (int position = int(order.size()) - 1; position >= 0; --position) {
        const int index = order[position];
        auto &entry = layouts[index];
        const bool pushed = position + 1 < order.size() && layouts[order[position + 1]].wrapStart < entry.wrapEnd;
        if (!pushed) {
            if (entry.end >= source.size() || !isCardMarkerLine(lineAt(source, entry.end))) continue;
            markerAt = entry.end;
        }
        const QString original = entry.original;
        const QString replacement = entry.replacement;
        if (latexRefuses(original)) continue;  // the writer refuses this original as before
        const int line = lineOf(source, markerAt);
        const QString marker = lineAt(source, markerAt).trimmed();
        const int first = lineOf(source, entry.start), last = lineOf(source, entry.end) - 1;
        const QString where = QStringLiteral("Line %1 is the review card marker %2, which must stay alone on its line").arg(line).arg(marker);
        const auto refuse = [&](QString message) {
            if (pushed)
                message = QStringLiteral("Proposals %1 and %2 touch right before the review card marker %3 on line %4, which must stay alone on its line: "
                                         "give them as one proposal (one range, one replacement).")
                              .arg(index + 1).arg(order[position + 1] + 1).arg(marker).arg(line);
            auto result = cardMarkerError(message, line, QStringLiteral("marker_after_proposal"));
            if (batch) result.insert(QStringLiteral("proposal_index"), index);
            return result;
        };
        if (!replacement.isEmpty() && !replacement.endsWith(QLatin1Char('\n')))
            return refuse(where + QStringLiteral(", and the replacement does not end with a line break, so its last line would run "
                                                 "into the marker. End the replacement with a line break."));
        if (!original.isEmpty() && !replacement.isEmpty()) {
            const QString keptOriginal = original.chopped(1), keptReplacement = replacement.chopped(1);
            if (!latexRefuses(keptOriginal) && !latexRefuses(keptReplacement)) {
                entry.wrapEnd = entry.end - 1;
                entry.wrapOriginal = keptOriginal;
                entry.wrapReplacement = keptReplacement;
                continue;
            }
            if (latexRefuses(keptOriginal)) {
                const int lastStart = entry.end >= 2 ? int(source.lastIndexOf(QLatin1Char('\n'), entry.end - 2)) + 1 : 0;
                if (isCardMarkerLine(lineAt(source, lastStart)))
                    return refuse(where + QStringLiteral(", and the selected lines end with another card marker (line %1). "
                                                         "Select only lines between card markers.").arg(last));
                return refuse(where + QStringLiteral(". A pending proposal that ends right before it must end before the line break of "
                                                     "line %1, but that line ends in a LaTeX comment (or a backslash), which would hide the "
                                                     "proposal's closing brace. Leave line %1 out of the range (end_line %1) if it stays "
                                                     "as it is, or ask the user to make this change.").arg(last));
            }
            return refuse(where + QStringLiteral(". A pending proposal that ends right before it must end before the line break of its "
                                                 "last line, but the replacement's last line ends in a LaTeX comment (or a backslash), which "
                                                 "would hide the proposal's closing brace. Write that line without the trailing comment."));
        }
        // A deletion or an insertion: the line break before the range goes into the wrapper instead.
        const QString what = original.isEmpty() ? QStringLiteral("insertion") : QStringLiteral("deletion");
        const int start = entry.start;
        if (start == 0)
            return refuse(where + QStringLiteral(", and a pending %1 at the start of the file cannot leave it so. "
                                                 "Ask the user to make this change.").arg(what));
        const int previousStart = start >= 2 ? int(source.lastIndexOf(QLatin1Char('\n'), start - 2)) + 1 : 0;
        const QString previous = source.mid(previousStart, start - 1 - previousStart);
        if (isCardMarkerLine(previous)) {
            if (!original.isEmpty())
                return refuse(where + QStringLiteral(", and so is line %1 before the selected lines: a pending deletion of "
                                                     "every line between two card markers cannot leave them alone on their lines. Keep at "
                                                     "least one of these lines (propose its new text instead), or ask the user to make "
                                                     "this change.").arg(first - 1));
            return refuse(where + QStringLiteral(", and so is line %1: a pending insertion between two card markers cannot "
                                                 "leave them alone on their lines. Insert the text before line %1 or after line "
                                                 "%2, or ask the user to add it.").arg(first - 1).arg(line));
        }
        if (hidesAppendedText(previous))
            return refuse(where + QStringLiteral(". A pending %1 right before it must start at the end of line %2, but "
                                                 "that line ends in a LaTeX comment (or a backslash), which would hide the proposal. Include "
                                                 "line %2 in the range with a replacement whose last line does not end in a "
                                                 "comment, or ask the user to make this change.").arg(what).arg(first - 1));
        const QString keptOriginal = original.isEmpty() ? QString() : QLatin1Char('\n') + original.chopped(1);
        const QString keptReplacement = replacement.isEmpty() ? QString() : QLatin1Char('\n') + replacement.chopped(1);
        if (latexRefuses(keptOriginal))
            return refuse(where + QStringLiteral(". A pending deletion right before it must end before the line break of line %1, "
                                                 "but that line ends in a LaTeX comment (or a backslash), which would hide the proposal's "
                                                 "closing brace. Ask the user to make this change.").arg(last));
        if (latexRefuses(keptReplacement))
            return refuse(where + QStringLiteral(". A pending insertion right before it must end before the line break of its last "
                                                 "line, but the inserted text's last line ends in a LaTeX comment (or a backslash), which "
                                                 "would hide the proposal's closing brace. Write that line without the trailing comment."));
        entry.wrapStart = start - 1;
        entry.wrapEnd = entry.end - 1;
        entry.wrapOriginal = keptOriginal;
        entry.wrapReplacement = keptReplacement;
    }
    return {};
}
// The replacement argument for a revision of a LaTeX wrapper laid out beside a card marker: the new
// replacement is given for whole lines, as on create, and written as layOutBesideCardMarkers writes it.
// A wrapper followed by a line break and a card marker line that starts after the text of the previous
// line, with an original and replacement that are empty or start with a line break, took the line
// break before its lines; one that starts a line with an original that does not end in a line break
// left out its final line break. Any other wrapper takes the replacement as it is.
// Returns the refusal, or an empty object.
QJsonObject revisedReplacement(const QString &source, const Thoughts::AgentProposedEdit &found, const QString &replacement, QString *written)
{
    *written = replacement;
    const int after = found.offset + found.length;
    if (source.mid(after, 1) != QLatin1String("\n") || !isCardMarkerLine(lineAt(source, after + 1))) return {};
    const int line = lineOf(source, after + 1);
    const QString marker = lineAt(source, after + 1).trimmed();
    const QString where = QStringLiteral("Line %1 is the review card marker %2, which must stay alone on its line").arg(line).arg(marker);
    const bool atLineStart = found.offset == 0 || source.at(found.offset - 1) == QLatin1Char('\n');
    const QString &original = found.original;
    const auto refuse = [&](const QString &message) {
        auto result = cardMarkerError(message, line, QStringLiteral("marker_after_proposal"));
        result.insert(QStringLiteral("proposal_id"), found.id);
        return result;
    };
    const auto emptyOrLineBreak = [](const QString &text) { return text.isEmpty() || text.startsWith(QLatin1Char('\n')); };
    if ((!atLineStart && emptyOrLineBreak(original) && emptyOrLineBreak(found.replacement))
        || (atLineStart && ((original.startsWith(QLatin1Char('\n')) && found.replacement.isEmpty())
                            || (original.isEmpty() && found.replacement.startsWith(QLatin1Char('\n')))))) {
        if (replacement.isEmpty()) {
            written->clear();
            return {};
        }
        if (!replacement.endsWith(QLatin1Char('\n')))
            return refuse(where + QStringLiteral(", and the replacement does not end with a line break, so its last line would run "
                                                 "into the marker. End the replacement with a line break."));
        *written = QLatin1Char('\n') + replacement.chopped(1);
    } else if (atLineStart && !original.isEmpty() && !original.endsWith(QLatin1Char('\n'))) {
        if (replacement.isEmpty())
            return refuse(where + QStringLiteral(", and this pending proposal keeps its last line break outside its text, so an "
                                                 "empty replacement would leave a blank line before the marker. To delete these lines, "
                                                 "withdraw this proposal (remove_proposal) and propose the deletion again."));
        *written = replacement.endsWith(QLatin1Char('\n')) ? replacement.chopped(1) : replacement;
    } else {
        return {};
    }
    if (latexRefuses(*written))
        return refuse(where + QStringLiteral(". This pending proposal ends right before it, so the replacement's last line break "
                                             "stays outside its text, but the replacement's last line ends in a LaTeX comment (or a "
                                             "backslash), which would hide the proposal's closing brace. Write that line without the "
                                             "trailing comment."));
    return {};
}
// The card marker lines of text that no span touches: marker -> their one-based line numbers.
QHash<QString, QList<int>> loneMarkers(const QString &text, const QVector<QPair<int, int>> &spans)
{
    QHash<QString, QList<int>> found;
    int start = 0, number = 0;
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        ++number;
        const int end = start + int(line.size());
        if (isCardMarkerLine(line)
            && std::none_of(spans.cbegin(), spans.cend(), [&](const auto &span) { return span.first < end && start < span.second; }))
            found[line.trimmed()].append(number);
        start = end + 1;
    }
    return found;
}
// The layout's safety net: refuse (card_marker_line) to write `updated` when a card marker line of
// `source` outside the `replaced` ranges no longer stands alone on its line outside the `written` wrappers.
QJsonObject checkCardMarkerLines(const QString &source, const QString &updated, const QVector<QPair<int, int>> &replaced, const QVector<QPair<int, int>> &written)
{
    if (!source.contains(QLatin1String("ReviewCardMask"))) return {};
    const auto before = loneMarkers(source, replaced), after = loneMarkers(updated, written);
    QList<QPair<int, QString>> missing;  // sorted, so the message names the first such line
    for (auto it = before.cbegin(); it != before.cend(); ++it)
        if (after.value(it.key()).size() < it.value().size()) missing.append({it.value().first(), it.key()});
    if (missing.isEmpty()) return {};
    std::sort(missing.begin(), missing.end());
    return cardMarkerError(QStringLiteral("Line %1 is the review card marker %2, which must stay alone on its line, and the "
                                          "proposal would put text on that line.").arg(missing.first().first).arg(missing.first().second),
                           missing.first().first, QStringLiteral("marker_line_shared"));
}
}

QJsonObject Thoughts::authorProposal(const QString &docsRoot, const QJsonObject &request, const QString &origin, const QString &claimDatabase)
{
    const auto command = request.value(QStringLiteral("command")).toString();
    // create-proposals creates several proposals at once, all with line
    // boundaries in the same source version, and writes them in one transaction.
    const bool batch = command == QLatin1String("create-proposals");
    const bool create = batch || command == QLatin1String("create-proposal");
    const bool remove = command == QLatin1String("remove-proposal");
    if (!create && !remove && command != QLatin1String("revise-proposal")) return error(QStringLiteral("invalid_request"), QStringLiteral("Unknown proposal operation."));
    QSet<QString> allowed {QStringLiteral("command"), QStringLiteral("source"), QStringLiteral("expected_source_sha256"), QStringLiteral("replacement"), QStringLiteral("explanation"), QStringLiteral("conversation_id"), QStringLiteral("request_id")};
    if (remove) allowed.remove(QStringLiteral("replacement"));
    if (batch) { allowed.remove(QStringLiteral("replacement")); allowed.remove(QStringLiteral("explanation")); allowed.insert(QStringLiteral("proposals")); }
    else if (create) { allowed.insert(QStringLiteral("start_line")); allowed.insert(QStringLiteral("end_line")); }
    else allowed.insert(QStringLiteral("proposal_id"));
    // Optional on create (§3.6): the link to the conversation, filled in by the MCP server.
    const QString conversationUrl = request.value(QStringLiteral("conversation_url")).toString();
    if (request.contains(QStringLiteral("conversation_url"))
        && (!create || !request.value(QStringLiteral("conversation_url")).isString() || !isValidAgentProposalConversationUrl(conversationUrl)))
        return error(QStringLiteral("invalid_request"), QStringLiteral("conversation_url must be a Claude Code session or Codex thread link, on create only."));
    for (auto it = request.begin(); it != request.end(); ++it)
        if (!allowed.contains(it.key()) && it.key() != QLatin1String("conversation_url"))
            return error(QStringLiteral("invalid_request"), QStringLiteral("Unexpected request field: %1").arg(it.key()));
    for (const auto &key : allowed) {
        if (key != QLatin1String("start_line") && key != QLatin1String("end_line") && key != QLatin1String("proposals") && !request.value(key).isString())
            return error(QStringLiteral("invalid_request"), QStringLiteral("Missing or invalid text field: %1").arg(key));
    }
    const QString requestId = request.value(QStringLiteral("request_id")).toString();
    const QString conversation = request.value(QStringLiteral("conversation_id")).toString();
    const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
    if (!validUuid(requestId) || !validUuid(conversation) || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(expected).hasMatch())
        return error(QStringLiteral("invalid_request"), QStringLiteral("Provide UUID request/conversation IDs and a SHA-256 source version."));
    // A single create is a batch of one; revise and removal use the first entry's text.
    // wrapStart..wrapReplacement: where its wrapper goes and what it holds, the range [start, end) with before
    // and the replacement except beside a review card marker (layOutBesideCardMarkers).
    struct Entry { QJsonObject fields; QString id; QString comment; QString before; int start = 0; int end = 0; int offset = 0;
                   int wrapStart = 0; int wrapEnd = 0; QString wrapOriginal; QString wrapReplacement; };
    QVector<Entry> entries;
    if (batch) {
        const auto list = request.value(QStringLiteral("proposals"));
        if (!list.isArray() || list.toArray().isEmpty() || list.toArray().size() > 500)
            return error(QStringLiteral("invalid_request"), QStringLiteral("proposals must be an array of 1 to 500 objects."));
        for (const auto &value : list.toArray()) {
            const auto fields = value.toObject();
            if (!value.isObject() || fields.size() != 4 || !fields.value(QStringLiteral("replacement")).isString() || !fields.value(QStringLiteral("explanation")).isString()
                || !fields.contains(QStringLiteral("start_line")) || !fields.contains(QStringLiteral("end_line")))
                return error(QStringLiteral("invalid_request"), QStringLiteral("Each proposal takes exactly start_line, end_line, replacement and explanation."));
            Entry entry;
            entry.fields = fields;
            entries.append(entry);
        }
    } else {
        Entry entry;
        entry.fields = request;
        entries.append(entry);
    }
    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    const QString requested = request.value(QStringLiteral("source")).toString();
    const QFileInfo fileInfo(QDir(root).absoluteFilePath(requested));
    const QString path = fileInfo.canonicalFilePath();
    const auto format = Thoughts::agentProposalFormat(path);
    const bool markdown = format == AgentProposalFormat::Markdown;
    for (auto &entry : entries) {
        QString replacement = entry.fields.value(QStringLiteral("replacement")).toString();
        const QString explanation = entry.fields.value(QStringLiteral("explanation")).toString();
        if (explanation.trimmed().isEmpty() || replacement.contains(QChar::Null) || explanation.contains(QChar::Null)
            || replacement.size() > 1000000 || explanation.size() > 100000)
            return error(QStringLiteral("invalid_request"), QStringLiteral("Provide a nonempty explanation; text must be within size limits and contain no NUL."));
        if (!remove) {
            if (const QString reason = agentProposalTextRefusal(format, replacement); !reason.isEmpty()) return writerError(reason);
        }
        if (create && (!positiveInteger(entry.fields.value(QStringLiteral("start_line"))) || !positiveInteger(entry.fields.value(QStringLiteral("end_line")))))
            return error(QStringLiteral("invalid_request"), QStringLiteral("Line boundaries must be positive integers."));
        // Markdown block markers occupy their own lines; that newline is part of
        // the replacement, so the source alone defines its complete contents.
        if (markdown && !remove && !replacement.isEmpty() && !replacement.endsWith(QLatin1Char('\n')))
            replacement += QLatin1Char('\n');
        entry.fields.insert(QStringLiteral("replacement"), replacement);
        entry.comment = markdown ? explanation.trimmed() : escapeLatexProposalExplanation(explanation.trimmed());
    }
    if (root.isEmpty() || requested.contains(QChar::Null) || !fileInfo.isFile() || !path.startsWith(root + QLatin1Char('/')) || !isAgentProposalSourcePath(path))
        return error(QStringLiteral("invalid_source"), QStringLiteral("The source must be an existing .tex, .md, or .markdown file inside the Thoughts documentation workspace."));
    const QString replacement = entries.first().fields.value(QStringLiteral("replacement")).toString();
    const QString explanation = entries.first().fields.value(QStringLiteral("explanation")).toString();
    QString detail;
    if (!QDir().mkpath(ProposalHistory::directory(root))) return error(QStringLiteral("write_failed"), QStringLiteral("Cannot create proposal history directory."));
    QLockFile lock(QDir(ProposalHistory::directory(root)).filePath(QStringLiteral("proposal_authoring.lock")));
    if (!lock.tryLock(5000)) return error(QStringLiteral("busy"), QStringLiteral("Another agent proposal operation is running."));
    if (!ProposalHistory::recover(root, &detail)) return error(QStringLiteral("history_failed"), detail);
    const QString requestHash = ProposalHistory::digest(QJsonDocument(request).toJson(QJsonDocument::Compact));
    // A retried request returns its recorded result instead of editing twice.
    if (const auto previous = ProposalHistory::agentRequests(root).value(requestId).toObject(); !previous.isEmpty()) {
        if (previous.value(QStringLiteral("sha256")).toString() != requestHash)
            return error(QStringLiteral("request_conflict"), QStringLiteral("This request ID was already used with different arguments."));
        auto result = previous.value(QStringLiteral("result")).toObject();
        result.insert(QStringLiteral("replayed"), true);
        return result;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return error(QStringLiteral("read_failed"), file.errorString());
    const QByteArray bytes = file.readAll();
    file.close();
    const QString source = QString::fromUtf8(bytes);
    if (source.toUtf8() != bytes || source.contains(QChar::Null)) return error(QStringLiteral("invalid_source"), QStringLiteral("Proposal authoring requires valid UTF-8 source without NUL."));
    // Generated files are rewritten by their generator; a withdrawal still cleans one up.
    if (!remove && isGeneratedAgentProposalSource(QDir(root).relativeFilePath(path), source))
        return error(QStringLiteral("generated_source"),
                     QStringLiteral("This file is generated; change its generator or the generator's inputs instead of proposing an edit here."));
    const QString actual = ProposalHistory::digest(bytes);
    if (actual != expected) {
        auto result = error(QStringLiteral("source_conflict"), QStringLiteral("The source changed; read it again before preparing this edit."));
        result.insert(QStringLiteral("current_source_sha256"), actual);
        return result;
    }
    // Broken wrappers are issues of their own; they do not block other proposals (§6).
    const AgentProposalParse parsed = parseAgentProposals(path, source);
    QString id = request.value(QStringLiteral("proposal_id")).toString();
    if (create) for (auto &entry : entries) entry.id = QStringLiteral("agent-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    else if (!isValidAgentProposalId(id)) return error(QStringLiteral("invalid_request"), QStringLiteral("Invalid proposal ID."));
    else entries.first().id = id;
    id = entries.first().id;
    // Check the entire source workspace, including hidden files and snapshots. Do
    // not follow directory symlinks, and compare canonical paths for aliases.
    QDirIterator sources(root, {QStringLiteral("*.tex"), QStringLiteral("*.md"), QStringLiteral("*.markdown")}, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (sources.hasNext()) {
        const QString other = sources.next();
        const QString canonicalOther = QFileInfo(other).canonicalFilePath();
        if (canonicalOther == path || !canonicalOther.startsWith(root + QLatin1Char('/'))) continue;
        QFile candidate(other);
        if (!candidate.open(QIODevice::ReadOnly)) return error(QStringLiteral("read_failed"), QStringLiteral("Cannot check proposal ID uniqueness in %1.").arg(other));
        const QByteArray content = candidate.readAll();
        if (std::none_of(entries.cbegin(), entries.cend(), [&](const auto &entry) { return content.contains(entry.id.toUtf8()); })) continue;
        // Every wrapper counts, including broken ones.
        const QSet<QString> otherIds = parseAgentProposals(canonicalOther, QString::fromUtf8(content)).ids();
        if (std::any_of(entries.cbegin(), entries.cend(), [&](const auto &entry) { return otherIds.contains(entry.id); }))
            return error(QStringLiteral("duplicate_id"), QStringLiteral("This proposal ID also occurs in %1.").arg(other));
    }
    if (remove) {
        const AgentProposedEdit *found = parsed.proposal(id);
        if (!found) return proposalError(parsed, id);
        // Withdrawing half of a move withdraws the whole move (§12), wherever its other half is.
        QStringList withdrawn {id};
        AgentProposalLocation partner;
        if (const QString other = AgentProposalMove::partner(id); !other.isEmpty()) {
            if (parsed.proposal(other)) withdrawn.append(other);
            else partner = locateAgentProposal(root, other, {path});
            if (partner.ambiguous)
                return error(QLatin1String(AgentProposalMove::Ambiguous), QStringLiteral("The other half of this move is pending in more than one source; withdraw it there first."));
        }
        QString updated, refusal;
        if (!decideAgentProposals(path, source, withdrawn, QStringLiteral("reject"), &updated, &refusal, &detail))
            return error(refusal, detail);
        const AgentProposalParse remaining = parseAgentProposals(path, updated);
        if (remaining.proposals.size() != parsed.proposals.size() - withdrawn.size() || remaining.issues.size() != parsed.issues.size() || remaining.ids().contains(id))
            return error(QStringLiteral("invalid_proposal"), QStringLiteral("The original text could not be restored safely."));
        QByteArray partnerAfter;
        if (partner.found) {
            QString otherUpdated;
            if (!decideAgentProposals(partner.path, QString::fromUtf8(partner.bytes), {partner.proposal.id}, QStringLiteral("reject"), &otherUpdated, &refusal, &detail))
                return error(refusal, detail);
            partnerAfter = otherUpdated.toUtf8();
        }
        const QByteArray afterBytes = updated.toUtf8();
        const QString afterHash = ProposalHistory::digest(afterBytes);
        QJsonObject result {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                            {QStringLiteral("state"), QStringLiteral("proposal_removed")}, {QStringLiteral("source"), path},
                            {QStringLiteral("proposal_id"), id}, {QStringLiteral("created_at"), found->createdAt},
                            {QStringLiteral("source_sha256"), afterHash}, {QStringLiteral("previous_source_sha256"), actual},
                            {QStringLiteral("original_text"), found->original}, {QStringLiteral("explanation"), explanation},
                            {QStringLiteral("request_id"), requestId}, {QStringLiteral("conversation_id"), found->conversationId},
                            {QStringLiteral("agent_conversation_id"), conversation}, {QStringLiteral("line"), found->line},
                            {QStringLiteral("replayed"), false},
                            {QStringLiteral("message"), QStringLiteral("Proposal withdrawn locally and original text restored; not synchronized.")}};
        if (withdrawn.size() > 1 || partner.found) {
            result.insert(QStringLiteral("withdrawn_proposal_ids"), QJsonArray{id, AgentProposalMove::partner(id)});
            if (partner.found) {
                result.insert(QStringLiteral("other_source"), partner.path);
                result.insert(QStringLiteral("other_source_sha256"), ProposalHistory::digest(partnerAfter));
            }
            result.insert(QStringLiteral("message"), QStringLiteral("Move withdrawn locally: both halves removed and their original text restored; not synchronized."));
        }
        if (!observeAgentProposalSource(root, path, bytes, &detail)) return error(QStringLiteral("history_failed"), detail);
        if (partner.found && !observeAgentProposalSource(root, partner.path, partner.bytes, &detail)) return error(QStringLiteral("history_failed"), detail);
        QJsonArray events;
        QList<QJsonObject> claims;
        const QString claimOrigin = origin == QLatin1String("mcp") ? origin : QStringLiteral("thoughtsctl");
        const auto withdraw = [&](const QString &file, const QString &text, const AgentProposedEdit &proposal, const QString &before, const QString &after) {
            const QString relative = QDir(root).relativeFilePath(file);
            const QJsonObject snapshot {{QStringLiteral("proposal_id"), proposal.id}, {QStringLiteral("source_file"), relative},
                                       {QStringLiteral("created_at"), proposal.createdAt}, {QStringLiteral("conversation_id"), proposal.conversationId},
                                       {QStringLiteral("comment"), proposal.explanation}, {QStringLiteral("before"), proposal.original}, {QStringLiteral("after"), proposal.replacement}};
            auto event = ProposalHistory::event(QStringLiteral("deleted_from_source"), snapshot, before, after);
            event.insert(QStringLiteral("context"), QJsonObject{{QStringLiteral("action"), QStringLiteral("withdraw")},
                                                               {QStringLiteral("explanation"), explanation}});
            event.insert(QStringLiteral("agent_conversation_id"), conversation);
            event.insert(QStringLiteral("agent_request_id"), requestId);
            event.insert(QStringLiteral("agent_request_sha256"), requestHash);
            event.insert(QStringLiteral("agent_result"), result);
            events.append(event);
            // The withdrawal is a decision (§10.1): the next Sync retires the proposal as withdrawn.
            // Only for a source Sync manages: no upload ever carries another file's claim.
            if (ProposalDecisions::claimApplies(root, relative))
                claims.append(ProposalDecisions::newClaim(proposal.id, QStringLiteral("withdraw"), ProposalDecisions::wrapperSha256(text, proposal), relative, claimOrigin));
        };
        for (const QString &withdrawnId : std::as_const(withdrawn)) withdraw(path, source, *parsed.proposal(withdrawnId), actual, afterHash);
        if (partner.found) withdraw(partner.path, QString::fromUtf8(partner.bytes), partner.proposal, ProposalHistory::digest(partner.bytes), ProposalHistory::digest(partnerAfter));
        const QString database = claimDatabase.isEmpty() ? ProposalDecisions::databasePath(root) : claimDatabase;
        QList<ProposalHistory::SourceWrite> writes {{path, bytes, afterBytes}};
        if (partner.found) writes.append({partner.path, partner.bytes, partnerAfter});
        if (!ProposalHistory::writeSources(root, writes, events, &detail, claims, database)) return error(QStringLiteral("write_failed"), detail);
        return result;
    }
    QString comment = entries.first().comment;
    QString updated = source;
    AgentProposedEdit original;
    QString created;
    QString before;
    int start = 0;
    if (create) {
        QVector<int> boundaries {0};
        for (int i = 0; i < source.size(); ++i) if (source.at(i) == QLatin1Char('\n')) boundaries.append(i + 1);
        if (boundaries.last() != source.size()) boundaries.append(source.size());
        for (auto &entry : entries) {
            const int first = entry.fields.value(QStringLiteral("start_line")).toInt() - 1;
            const int last = entry.fields.value(QStringLiteral("end_line")).toInt() - 1;
            if (first > last || first >= boundaries.size() || last >= boundaries.size()) return error(QStringLiteral("invalid_range"), QStringLiteral("Use one-based line boundaries with end_line exclusive; equal boundaries insert text."));
            entry.start = boundaries[first];
            entry.end = boundaries[last];
            // Broken wrappers occupy their span as well: wrapping one would nest it.
            QVector<QPair<int, int>> occupied;
            for (const auto &proposal : parsed.proposals) occupied.append({proposal.offset, proposal.offset + proposal.length});
            for (const auto &issue : parsed.issues) occupied.append({issue.offset, issue.offset + issue.length});
            for (const auto &[first, last] : std::as_const(occupied)) if ((entry.start < last && entry.end > first) || (entry.start == entry.end && entry.start > first && entry.start < last))
                return error(QStringLiteral("proposal_overlap"), QStringLiteral("The selected range intersects a pending proposal; revise that proposal instead."));
            entry.before = source.mid(entry.start, entry.end - entry.start);
            if (entry.before == entry.fields.value(QStringLiteral("replacement")).toString()) return error(QStringLiteral("no_change"), QStringLiteral("The replacement is identical to the selected text."));
        }
        // Ranges of one request may touch but not overlap, and two insertions
        // at the same boundary would have no defined order.
        QVector<int> order(entries.size());
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return std::pair(entries[a].start, entries[a].end) < std::pair(entries[b].start, entries[b].end); });
        for (int i = 1; i < order.size(); ++i) {
            const auto &previous = entries[order[i - 1]];
            const auto &next = entries[order[i]];
            if (previous.end > next.start || (previous.start == previous.end && next.start == next.end && previous.start == next.start))
                return error(QStringLiteral("proposal_overlap"), QStringLiteral("The proposals of one request must not overlap or insert at the same line."));
        }
        created = formatAgentProposalTimestamp(QDateTime::currentDateTimeUtc());
        // A review card marker line stays alone while the proposals are pending.
        QVector<WrapperLayout> layouts;
        for (const auto &entry : std::as_const(entries)) {
            WrapperLayout layout;
            layout.start = entry.start;
            layout.end = entry.end;
            layout.original = entry.before;
            layout.replacement = entry.fields.value(QStringLiteral("replacement")).toString();
            layouts.append(layout);
        }
        if (const auto refusal = layOutBesideCardMarkers(markdown ? QString() : source, layouts, order, batch); !refusal.isEmpty()) return refusal;
        for (int i = 0; i < entries.size(); ++i) {
            entries[i].wrapStart = layouts[i].wrapStart;
            entries[i].wrapEnd = layouts[i].wrapEnd;
            entries[i].wrapOriginal = layouts[i].wrapOriginal;
            entries[i].wrapReplacement = layouts[i].wrapReplacement;
        }
        // Original and replacement contents must round-trip byte-for-byte. A
        // trailing comment can only be captured safely when its newline is included.
        int shift = 0;
        for (const int index : order) {
            auto &entry = entries[index];
            AgentProposedEdit proposal;
            proposal.id = entry.id;
            proposal.createdAt = created;
            proposal.conversationId = conversation.toLower();
            proposal.conversationUrl = conversationUrl;
            proposal.explanation = entry.fields.value(QStringLiteral("explanation")).toString().trimmed();
            proposal.original = entry.wrapOriginal;
            proposal.replacement = entry.wrapReplacement;
            QString refusal;
            const QString block = markdown ? formatMarkdownAgentProposal(proposal, &refusal) : formatLatexAgentProposal(proposal, &refusal);
            if (block.isEmpty()) return writerError(refusal);
            entry.offset = entry.wrapStart + shift;
            updated.replace(entry.offset, entry.wrapEnd - entry.wrapStart, block);
            shift += block.size() - (entry.wrapEnd - entry.wrapStart);
        }
        before = entries.first().before;
        start = entries.first().offset;
    } else {
        const AgentProposedEdit *found = parsed.proposal(id);
        if (!found) return proposalError(parsed, id);
        // A half of a move carries the card's passage verbatim; changing one half alone would break the pair (§12).
        if (AgentProposalMove::isHalf(id))
            return error(QStringLiteral("move_proposal"), QStringLiteral("This proposal is half of a card move and cannot be revised; withdraw it with remove_proposal "
                                                                         "(which withdraws both halves) and move the card again."));
        original = *found;
        // Revisions never rewrite creation metadata, including the original conversation.
        created = original.createdAt;
        before = original.original;
        start = original.offset;
        QString written = replacement;
        if (markdown) {
            original.explanation = comment;
            original.replacement = replacement;
            QString refusal;
            const QString block = formatMarkdownAgentProposal(original, &refusal);
            if (block.isEmpty()) return writerError(refusal);
            updated.replace(original.offset, original.length, block);
        } else {
            // Revise in place, keeping the wrapper's own layout and metadata.
            if (const auto refusal = revisedReplacement(source, original, replacement, &written); !refusal.isEmpty()) return refusal;
            updated.replace(original.replacementOffset, original.replacementLength, written);
            updated.replace(original.explanationOffset, original.explanationLength, comment + QLatin1Char('\n'));
        }
        auto &entry = entries.first();
        entry.start = original.offset;
        entry.end = original.offset + original.length;
        entry.wrapOriginal = original.original;
        entry.wrapReplacement = written;
    }
    if (!create) {
        auto &entry = entries.first();
        entry.comment = comment;
        entry.before = before;
        entry.offset = start;
    }
    const AgentProposalParse resultParse = parseAgentProposals(path, updated);
    QVector<AgentProposedEdit> written;
    for (const auto &entry : entries) {
        const AgentProposedEdit *found = resultParse.proposal(entry.id);
        if (!found || found->offset != entry.offset || found->original != entry.wrapOriginal
            || found->replacement != entry.wrapReplacement || found->createdAt != created
            || found->explanation != (markdown ? entry.comment : entry.comment + QLatin1Char('\n')))
            return error(QStringLiteral("invalid_proposal"), markdown ? QStringLiteral("The Markdown proposal did not round-trip exactly.") : QStringLiteral("The proposed text does not round-trip as a valid TeX argument; check balanced braces and include newlines after trailing comments."));
        written.append(*found);
    }
    if (resultParse.proposals.size() != parsed.proposals.size() + (create ? entries.size() : 0) || resultParse.issues.size() != parsed.issues.size())
        return error(QStringLiteral("invalid_proposal"), QStringLiteral("The proposals changed other pending proposals; check balanced braces and include newlines after trailing comments."));
    if (!markdown) {
        // Every card marker outside the replaced text is still alone on its line (the layout's safety net).
        QVector<QPair<int, int>> replaced, spans;
        for (const auto &entry : std::as_const(entries)) replaced.append({entry.start, entry.end});
        for (const auto &proposal : std::as_const(written)) spans.append({proposal.offset, proposal.offset + proposal.length});
        if (const auto refusal = checkCardMarkerLines(source, updated, replaced, spans); !refusal.isEmpty()) return refusal;
    }
    // Every link the proposed text introduces must resolve as the build
    // resolves it; links the original already had are not this proposal's.
    QJsonArray linkIssues;
    for (int i = 0; i < entries.size(); ++i) {
        for (const auto &issue : introducedLinkIssues(root, path, entries[i].before, entries[i].fields.value(QStringLiteral("replacement")).toString(), updated)) {
            QJsonObject entry = issue.toJson();
            if (batch) entry.insert(QStringLiteral("proposal_index"), i);
            linkIssues.append(entry);
        }
    }
    if (!linkIssues.isEmpty()) {
        QString message = linkIssues.first().toObject().value(QStringLiteral("message")).toString();
        if (linkIssues.size() > 1) message += QStringLiteral(" (%1 more link problems are listed in links.)").arg(linkIssues.size() - 1);
        auto result = error(QStringLiteral("invalid_link"), message + QStringLiteral(" Nothing was written; fix the links and call again with a new request_id."));
        result.insert(QStringLiteral("links"), linkIssues);
        return result;
    }
    const QByteArray afterBytes = updated.toUtf8();
    const QString afterHash = ProposalHistory::digest(afterBytes);
    QJsonObject result {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                        {QStringLiteral("state"), batch ? QStringLiteral("proposals_created") : create ? QStringLiteral("proposal_created") : QStringLiteral("proposal_revised")},
                        {QStringLiteral("source"), path}, {QStringLiteral("source_sha256"), afterHash}, {QStringLiteral("previous_source_sha256"), actual},
                        {QStringLiteral("request_id"), requestId}, {QStringLiteral("agent_conversation_id"), conversation.toLower()}, {QStringLiteral("replayed"), false}};
    QJsonArray results;
    for (int i = 0; i < entries.size(); ++i) {
        results.append(QJsonObject{{QStringLiteral("proposal_id"), entries[i].id}, {QStringLiteral("created_at"), created},
                                   {QStringLiteral("original_text"), entries[i].before}, {QStringLiteral("replacement"), entries[i].fields.value(QStringLiteral("replacement"))},
                                   {QStringLiteral("explanation"), entries[i].fields.value(QStringLiteral("explanation"))}, {QStringLiteral("line"), written[i].line}});
    }
    if (!written.first().conversationUrl.isEmpty()) result.insert(QStringLiteral("conversation_url"), written.first().conversationUrl);
    if (batch) {
        result.insert(QStringLiteral("conversation_id"), conversation.toLower());
        result.insert(QStringLiteral("proposals"), results);
        result.insert(QStringLiteral("message"), QStringLiteral("Saved %1 pending proposals locally; not accepted or synchronized.").arg(entries.size()));
    } else {
        const auto single = results.first().toObject();
        for (auto it = single.begin(); it != single.end(); ++it) result.insert(it.key(), it.value());
        result.insert(QStringLiteral("conversation_id"), written.first().conversationId);
        result.insert(QStringLiteral("message"), QStringLiteral("Saved locally as a pending proposal; not accepted or synchronized."));
    }
    if (!create && !observeAgentProposalSource(root, path, bytes, &detail)) return error(QStringLiteral("history_failed"), detail);
    // Every event carries the whole result, so a retry of the request replays it.
    QJsonArray events;
    for (int i = 0; i < entries.size(); ++i) {
        // The wrapper's own texts, as later reads of the source find them.
        const QJsonObject snapshot {{QStringLiteral("proposal_id"), entries[i].id}, {QStringLiteral("source_file"), QDir(root).relativeFilePath(path)},
                                   {QStringLiteral("created_at"), created}, {QStringLiteral("conversation_id"), written[i].conversationId}, {QStringLiteral("comment"), written[i].explanation},
                                   {QStringLiteral("before"), written[i].original}, {QStringLiteral("after"), written[i].replacement}};
        QJsonObject linked = snapshot;
        if (!written[i].conversationUrl.isEmpty()) linked.insert(QStringLiteral("conversation_url"), written[i].conversationUrl);
        auto event = ProposalHistory::event(create ? QStringLiteral("first_seen") : QStringLiteral("changed"), linked, actual, afterHash);
        event.insert(QStringLiteral("agent_conversation_id"), conversation.toLower());
        event.insert(QStringLiteral("agent_request_id"), requestId);
        event.insert(QStringLiteral("agent_request_sha256"), requestHash);
        event.insert(QStringLiteral("agent_result"), result);
        events.append(event);
    }
    if (!ProposalHistory::writeSource(root, path, bytes, afterBytes, events, &detail)) return error(QStringLiteral("write_failed"), detail);
    return result;
}

QJsonObject Thoughts::authorProposalAfterSync(const QString &docsRoot, const QJsonObject &request, const QString &origin,
                                              const QJsonObject &before, const QString &syncError, const CardMoveContext *card)
{
    const bool skipped = before.isEmpty() && syncError.isEmpty();
    if (!skipped && !syncCommandSourcesSynced(before)) {
        const QString detail = before.isEmpty() ? syncError : before.value(QStringLiteral("message")).toString();
        auto failure = syncCommandError(QStringLiteral("sync_before_failed"),
            QStringLiteral("The Sync before writing the proposal did not synchronize the sources, so nothing was written: ") + detail);
        failure.insert(QStringLiteral("sync_before"), before);
        return failure;
    }
    auto result = request.value(QStringLiteral("command")).toString() == QLatin1String("move-card-passage")
        ? authorCardMove(docsRoot, request, origin, card ? *card : CardMoveContext{})
        : authorProposal(docsRoot, request, origin);
    if (result.isEmpty()) {
        return result;
    }
    result.insert(QStringLiteral("sync_before"), skipped ? QStringLiteral("skipped_recent") : before.value(QStringLiteral("operation_id")).toString());
    if (const QJsonObject warning = syncCommandBuildWarning(before, QStringLiteral("before")); !warning.isEmpty()) {
        result.insert(QStringLiteral("warnings"), QJsonArray {warning});
    }
    return result;
}

QJsonObject Thoughts::authorCardMove(const QString &docsRoot, const QJsonObject &request, const QString &origin, const CardMoveContext &card,
                                     const QString &claimDatabase)
{
    Q_UNUSED(origin)
    Q_UNUSED(claimDatabase)
    const QSet<QString> allowed {QStringLiteral("command"), QStringLiteral("card_id"), QStringLiteral("source"), QStringLiteral("expected_source_sha256"),
                                 QStringLiteral("line"), QStringLiteral("explanation"), QStringLiteral("conversation_id"), QStringLiteral("conversation_url"),
                                 QStringLiteral("request_id")};
    for (auto it = request.begin(); it != request.end(); ++it)
        if (!allowed.contains(it.key())) return error(QStringLiteral("invalid_request"), QStringLiteral("Unexpected request field: %1").arg(it.key()));
    for (const auto &key : allowed) {
        if (key == QLatin1String("line") || key == QLatin1String("conversation_url")) continue;
        if (!request.value(key).isString()) return error(QStringLiteral("invalid_request"), QStringLiteral("Missing or invalid text field: %1").arg(key));
    }
    const QString cardId = request.value(QStringLiteral("card_id")).toString();
    const QString requestId = request.value(QStringLiteral("request_id")).toString();
    const QString conversation = request.value(QStringLiteral("conversation_id")).toString().toLower();
    const QString expected = request.value(QStringLiteral("expected_source_sha256")).toString();
    const QString explanation = request.value(QStringLiteral("explanation")).toString().trimmed();
    const QString conversationUrl = request.value(QStringLiteral("conversation_url")).toString();
    if (!validUuid(cardId) || !validUuid(requestId) || !validUuid(conversation) || !QRegularExpression(QStringLiteral("^[0-9a-f]{64}$")).match(expected).hasMatch())
        return error(QStringLiteral("invalid_request"), QStringLiteral("Provide UUID card/request/conversation IDs and a SHA-256 source version."));
    if (!positiveInteger(request.value(QStringLiteral("line"))))
        return error(QStringLiteral("invalid_request"), QStringLiteral("line must be the one-based line before which the passage is inserted."));
    if (request.contains(QStringLiteral("conversation_url"))
        && (!request.value(QStringLiteral("conversation_url")).isString() || !isValidAgentProposalConversationUrl(conversationUrl)))
        return error(QStringLiteral("invalid_request"), QStringLiteral("conversation_url must be a Claude Code session or Codex thread link."));
    if (explanation.isEmpty() || explanation.contains(QChar::Null) || explanation.size() > 100000)
        return error(QStringLiteral("invalid_request"), QStringLiteral("Provide a nonempty explanation within size limits and without NUL."));
    if (!card.movePairsSupported)
        return error(QStringLiteral("server_unsupported"), QStringLiteral("Moving a card linked to source markers needs an updated Thoughts server, which decides both halves of the move "
                                                                          "together; until then move it by cutting its marked passage and pasting it into the other source."));
    if (!card.refusal.isEmpty()) return error(card.refusal, card.refusalMessage);

    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    const QString requested = request.value(QStringLiteral("source")).toString();
    const QFileInfo destinationInfo(QDir(root).absoluteFilePath(requested));
    const QString destination = destinationInfo.canonicalFilePath();
    if (root.isEmpty() || requested.contains(QChar::Null) || !destinationInfo.isFile() || !destination.startsWith(root + QLatin1Char('/')) || !isAgentProposalSourcePath(destination))
        return error(QStringLiteral("invalid_source"), QStringLiteral("The destination must be an existing .tex, .md, or .markdown file inside the Thoughts documentation workspace."));
    const QFileInfo originInfo(QDir(root).absoluteFilePath(card.sourceFile));
    const QString originPath = originInfo.canonicalFilePath();
    if (card.sourceFile.isEmpty() || !originInfo.isFile() || !originPath.startsWith(root + QLatin1Char('/')) || !isAgentProposalSourcePath(originPath))
        return error(QStringLiteral("invalid_markers"), QStringLiteral("The card's marked source %1 is not in the Library.").arg(card.sourceFile));
    const auto format = agentProposalFormat(destination);
    const bool markdown = format == AgentProposalFormat::Markdown;
    if (agentProposalFormat(originPath) != format)
        return error(QStringLiteral("format_mismatch"), QStringLiteral("A marked passage moves only between sources of the same format (LaTeX to LaTeX, Markdown to Markdown)."));
    const bool sameFile = originPath == destination;
    const QString originRelative = QDir(root).relativeFilePath(originPath);
    const QString destinationRelative = QDir(root).relativeFilePath(destination);

    QString detail;
    if (!QDir().mkpath(ProposalHistory::directory(root))) return error(QStringLiteral("write_failed"), QStringLiteral("Cannot create proposal history directory."));
    QLockFile lock(QDir(ProposalHistory::directory(root)).filePath(QStringLiteral("proposal_authoring.lock")));
    if (!lock.tryLock(5000)) return error(QStringLiteral("busy"), QStringLiteral("Another agent proposal operation is running."));
    if (!ProposalHistory::recover(root, &detail)) return error(QStringLiteral("history_failed"), detail);
    const QString requestHash = ProposalHistory::digest(QJsonDocument(request).toJson(QJsonDocument::Compact));
    if (const auto previous = ProposalHistory::agentRequests(root).value(requestId).toObject(); !previous.isEmpty()) {
        if (previous.value(QStringLiteral("sha256")).toString() != requestHash)
            return error(QStringLiteral("request_conflict"), QStringLiteral("This request ID was already used with different arguments."));
        auto result = previous.value(QStringLiteral("result")).toObject();
        result.insert(QStringLiteral("replayed"), true);
        return result;
    }
    const auto readText = [](const QString &path, QByteArray *bytes, QString *text) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) return false;
        *bytes = file.readAll();
        *text = QString::fromUtf8(*bytes);
        return text->toUtf8() == *bytes && !text->contains(QChar::Null);
    };
    QByteArray destinationBytes, originBytes;
    QString destinationText, originText;
    if (!readText(destination, &destinationBytes, &destinationText) || !readText(originPath, &originBytes, &originText))
        return error(QStringLiteral("invalid_source"), QStringLiteral("Proposal authoring requires readable, valid UTF-8 sources without NUL."));
    for (const auto &[relative, text] : {std::pair(destinationRelative, destinationText), std::pair(originRelative, originText)}) {
        if (isGeneratedAgentProposalSource(relative, text))
            return error(QStringLiteral("generated_source"),
                         QStringLiteral("%1 is generated; change its generator or the generator's inputs instead of proposing an edit here.").arg(relative));
    }
    const QString actual = ProposalHistory::digest(destinationBytes);
    if (actual != expected) {
        auto result = error(QStringLiteral("source_conflict"), QStringLiteral("The source changed; read it again before preparing this move."));
        result.insert(QStringLiteral("current_source_sha256"), actual);
        return result;
    }

    // The marked passage: whole lines from the card's first marker to its last.
    if (const auto anchor = readReviewSourceAnchor(originRelative, originBytes, cardId); !anchor.error.isEmpty())
        return error(QStringLiteral("invalid_markers"), QStringLiteral("The card's markers in %1 cannot be moved: %2").arg(originRelative, anchor.error));
    const QString escapedId = QRegularExpression::escape(cardId);
    const QRegularExpression marker(markdown ? QStringLiteral(R"(^ {0,3}<!-- thoughts:v1:(start|end) review:%1:answer:[0-9]+ -->\r?$)").arg(escapedId)
                                             : QStringLiteral(R"(^\\ReviewCardMask(Start|End)\{%1\}\{[0-9]+\}%$)").arg(escapedId));
    const QVector<int> originLines = lineBoundaries(originText);
    int firstLine = -1, lastLine = -1;
    for (int i = 0; i + 1 < originLines.size(); ++i) {
        QString line = originText.mid(originLines[i], originLines[i + 1] - originLines[i]);
        if (line.endsWith(QLatin1Char('\n'))) line.chop(1);
        if (!marker.match(line).hasMatch()) continue;
        if (firstLine < 0) firstLine = i;
        lastLine = i;
    }
    if (firstLine < 0) return error(QStringLiteral("invalid_markers"), QStringLiteral("The card's markers were not found in %1.").arg(originRelative));
    const int cutStart = originLines[firstLine];
    const int cutEnd = originLines[lastLine + 1];
    const QString passage = originText.mid(cutStart, cutEnd - cutStart);
    // Other cards inside the passage move with it, but only whole: never one of their markers alone.
    const QRegularExpression anyCard(markdown ? QStringLiteral("review:([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}):answer:")
                                              : QStringLiteral(R"(\\ReviewCardMask(?:Start|End)\{([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\})"));
    QStringList alsoMoved;
    for (auto it = anyCard.globalMatch(passage); it.hasNext();) {
        const QString other = it.next().captured(1);
        if (other != cardId && !alsoMoved.contains(other)) alsoMoved.append(other);
    }
    for (const QString &other : std::as_const(alsoMoved)) {
        if (originText.count(other) != passage.count(other))
            return error(QStringLiteral("split_card"), QStringLiteral("The passage of this card holds only part of another card's markers (%1); move or delete that card first.").arg(other));
    }
    const AgentProposalParse originParse = parseAgentProposals(originPath, originText);
    for (const auto &[first, last] : occupiedSpans(originParse))
        if (cutStart < last && cutEnd > first)
            return error(QStringLiteral("proposal_overlap"), QStringLiteral("The card's marked passage intersects a pending proposal; it can move once that proposal is decided or withdrawn."));
    if (const QString reason = agentProposalTextRefusal(format, passage); !reason.isEmpty()) return writerError(reason);

    const QVector<int> destinationLines = lineBoundaries(destinationText);
    const int lineIndex = request.value(QStringLiteral("line")).toInt() - 1;
    if (lineIndex >= destinationLines.size())
        return error(QStringLiteral("invalid_range"), QStringLiteral("line must be at most one past the destination's last line (%1).").arg(destinationLines.size()));
    const int insertAt = destinationLines[lineIndex];
    const AgentProposalParse destinationParse = sameFile ? originParse : parseAgentProposals(destination, destinationText);
    for (const auto &[first, last] : occupiedSpans(destinationParse))
        if (insertAt > first && insertAt < last)
            return error(QStringLiteral("proposal_overlap"), QStringLiteral("The insertion line is inside a pending proposal; choose a line outside it."));
    if (sameFile && insertAt > cutStart && insertAt < cutEnd)
        return error(QStringLiteral("invalid_range"), QStringLiteral("The insertion line is inside the passage being moved."));
    if (sameFile && (insertAt == cutStart || insertAt == cutEnd))
        return error(QStringLiteral("no_change"), QStringLiteral("The passage is already at that line."));
    // The insertion keeps the passage on whole lines.
    QString inserted = passage;
    if (!inserted.endsWith(QLatin1Char('\n'))) inserted += QLatin1Char('\n');
    if (insertAt == destinationText.size() && !destinationText.isEmpty() && !destinationText.endsWith(QLatin1Char('\n')))
        return error(QStringLiteral("invalid_range"), QStringLiteral("The destination's last line has no newline; insert before it, or add a final newline first."));

    const QStringList ids = AgentProposalMove::newPair();
    QString unreadable;
    if (const QString other = sourceWithAnyId(root, {}, ids, &unreadable); !other.isEmpty() || !unreadable.isEmpty())
        return unreadable.isEmpty() ? error(QStringLiteral("duplicate_id"), QStringLiteral("This proposal ID also occurs in %1.").arg(other))
                                    : error(QStringLiteral("read_failed"), QStringLiteral("Cannot check proposal ID uniqueness in %1.").arg(unreadable));
    const QString created = formatAgentProposalTimestamp(QDateTime::currentDateTimeUtc());
    const QString cardName = card.question.trimmed().isEmpty() ? QStringLiteral("its review card")
                                                              : QStringLiteral("its review card “%1”").arg(card.question.simplified());
    AgentProposedEdit cut, paste;
    for (auto *proposal : {&cut, &paste}) {
        proposal->createdAt = created;
        proposal->conversationId = conversation;
        proposal->conversationUrl = conversationUrl;
    }
    cut.id = ids[0];
    cut.explanation = explanation + QStringLiteral("\n\nMoves this passage with %1 to %2. Accepting or rejecting this proposal also decides the insertion there.")
                                        .arg(cardName, sameFile ? QStringLiteral("line %1 of this source").arg(lineIndex + 1) : destinationRelative);
    cut.original = passage;
    paste.id = ids[1];
    paste.explanation = explanation + QStringLiteral("\n\nInserts the passage moved here from %1 with %2. Accepting or rejecting this proposal also decides its removal there.")
                                          .arg(sameFile ? QStringLiteral("line %1 of this source").arg(firstLine + 1) : originRelative, cardName);
    paste.replacement = inserted;
    QString refusal;
    const QString cutBlock = markdown ? formatMarkdownAgentProposal(cut, &refusal) : formatLatexAgentProposal(cut, &refusal);
    if (cutBlock.isEmpty()) return writerError(refusal);
    const QString pasteBlock = markdown ? formatMarkdownAgentProposal(paste, &refusal) : formatLatexAgentProposal(paste, &refusal);
    if (pasteBlock.isEmpty()) return writerError(refusal);
    QString updatedOrigin = originText, updatedDestination = destinationText;
    if (sameFile) {
        // The later edit first, so the earlier offset stays valid.
        if (insertAt > cutStart) { updatedOrigin.insert(insertAt, pasteBlock); updatedOrigin.replace(cutStart, cutEnd - cutStart, cutBlock); }
        else { updatedOrigin.replace(cutStart, cutEnd - cutStart, cutBlock); updatedOrigin.insert(insertAt, pasteBlock); }
        updatedDestination = updatedOrigin;
    } else {
        updatedOrigin.replace(cutStart, cutEnd - cutStart, cutBlock);
        updatedDestination.insert(insertAt, pasteBlock);
    }
    // Both proposals read back exactly, and nothing else changed.
    const auto readsBack = [&](const QString &path, const QString &text, const AgentProposalParse &before, const QList<const AgentProposedEdit *> &expectedProposals) {
        const AgentProposalParse after = parseAgentProposals(path, text);
        if (after.proposals.size() != before.proposals.size() + expectedProposals.size() || after.issues.size() != before.issues.size()) return false;
        for (const auto *proposal : expectedProposals) {
            const AgentProposedEdit *found = after.proposal(proposal->id);
            const QString comment = markdown ? proposal->explanation : escapeLatexProposalExplanation(proposal->explanation) + QLatin1Char('\n');
            if (!found || found->original != proposal->original || found->replacement != proposal->replacement || found->createdAt != created
                || found->explanation != comment)
                return false;
        }
        return true;
    };
    if (sameFile ? !readsBack(originPath, updatedOrigin, originParse, {&cut, &paste})
                 : (!readsBack(originPath, updatedOrigin, originParse, {&cut}) || !readsBack(destination, updatedDestination, destinationParse, {&paste})))
        return error(QStringLiteral("invalid_proposal"), markdown ? QStringLiteral("The Markdown move did not round-trip exactly.")
                                                                 : QStringLiteral("The moved passage does not round-trip as a valid TeX argument; check balanced braces."));
    // Accepted, the card's markers must read back in the destination, and be gone from the origin.
    QString acceptedOrigin, acceptedDestination, code;
    if (sameFile) {
        if (!decideAgentProposals(originPath, updatedOrigin, ids, QStringLiteral("accept"), &acceptedOrigin, &code, &detail)) return error(code, detail);
        acceptedDestination = acceptedOrigin;
    } else if (!decideAgentProposals(originPath, updatedOrigin, {ids[0]}, QStringLiteral("accept"), &acceptedOrigin, &code, &detail)
               || !decideAgentProposals(destination, updatedDestination, {ids[1]}, QStringLiteral("accept"), &acceptedDestination, &code, &detail)) {
        return error(code, detail);
    }
    QStringList movedCards {cardId};
    movedCards.append(alsoMoved);
    for (const QString &moved : std::as_const(movedCards)) {
        const auto after = readReviewSourceAnchor(destinationRelative, acceptedDestination.toUtf8(), moved);
        if (!after.error.isEmpty() || (!sameFile && acceptedOrigin.contains(moved)))
            return error(QStringLiteral("invalid_destination"),
                         QStringLiteral("At line %1 of %2 the moved card's markers would not read back after acceptance (%3). Choose a line between complete paragraphs or blocks of the document body.")
                             .arg(lineIndex + 1).arg(destinationRelative, after.error.isEmpty() ? QStringLiteral("markers left behind") : after.error));
    }
    // Links in the moved text must resolve from its new place.
    QJsonArray linkIssues;
    for (const auto &issue : introducedLinkIssues(root, destination, QString(), inserted, updatedDestination)) linkIssues.append(issue.toJson());
    if (!sameFile && !linkIssues.isEmpty()) {
        QString message = linkIssues.first().toObject().value(QStringLiteral("message")).toString();
        if (linkIssues.size() > 1) message += QStringLiteral(" (%1 more link problems are listed in links.)").arg(linkIssues.size() - 1);
        auto result = error(QStringLiteral("invalid_link"), message + QStringLiteral(" Nothing was written; propose a fix of these links in the card's passage first, then move it."));
        result.insert(QStringLiteral("links"), linkIssues);
        return result;
    }

    const QByteArray originAfter = updatedOrigin.toUtf8();
    const QByteArray destinationAfter = updatedDestination.toUtf8();
    const AgentProposalParse writtenOrigin = parseAgentProposals(originPath, updatedOrigin);
    const AgentProposalParse writtenDestination = sameFile ? writtenOrigin : parseAgentProposals(destination, updatedDestination);
    const AgentProposedEdit *writtenCut = writtenOrigin.proposal(ids[0]);
    const AgentProposedEdit *writtenPaste = writtenDestination.proposal(ids[1]);
    QJsonObject result {{QStringLiteral("schema_version"), 1}, {QStringLiteral("accepted"), true}, {QStringLiteral("terminal"), true},
                        {QStringLiteral("state"), QStringLiteral("move_proposed")}, {QStringLiteral("card_id"), cardId},
                        {QStringLiteral("source"), destination}, {QStringLiteral("source_sha256"), ProposalHistory::digest(destinationAfter)},
                        {QStringLiteral("previous_source_sha256"), actual},
                        {QStringLiteral("origin_source"), originPath}, {QStringLiteral("origin_source_sha256"), ProposalHistory::digest(originAfter)},
                        {QStringLiteral("request_id"), requestId}, {QStringLiteral("conversation_id"), conversation},
                        {QStringLiteral("agent_conversation_id"), conversation}, {QStringLiteral("created_at"), created},
                        {QStringLiteral("proposals"), QJsonArray{
                             QJsonObject{{QStringLiteral("proposal_id"), ids[0]}, {QStringLiteral("role"), QStringLiteral("cut")}, {QStringLiteral("source"), originPath},
                                         {QStringLiteral("line"), writtenCut->line}, {QStringLiteral("original_text"), passage}, {QStringLiteral("replacement"), QString()}},
                             QJsonObject{{QStringLiteral("proposal_id"), ids[1]}, {QStringLiteral("role"), QStringLiteral("paste")}, {QStringLiteral("source"), destination},
                                         {QStringLiteral("line"), writtenPaste->line}, {QStringLiteral("original_text"), QString()}, {QStringLiteral("replacement"), inserted}}}},
                        {QStringLiteral("also_moved_card_ids"), QJsonArray::fromStringList(alsoMoved)},
                        {QStringLiteral("replayed"), false},
                        {QStringLiteral("message"), QStringLiteral("Saved the move as two linked pending proposals (remove here, insert there); accepting or rejecting either decides both. Not accepted or synchronized.")}};
    if (!conversationUrl.isEmpty()) result.insert(QStringLiteral("conversation_url"), conversationUrl);
    QJsonArray events;
    for (const auto &[proposal, path, before, after] : {std::tuple(writtenCut, originPath, originBytes, originAfter),
                                                         std::tuple(writtenPaste, destination, destinationBytes, destinationAfter)}) {
        QJsonObject snapshot {{QStringLiteral("proposal_id"), proposal->id}, {QStringLiteral("source_file"), QDir(root).relativeFilePath(path)},
                              {QStringLiteral("created_at"), created}, {QStringLiteral("conversation_id"), conversation}, {QStringLiteral("comment"), proposal->explanation},
                              {QStringLiteral("before"), proposal->original}, {QStringLiteral("after"), proposal->replacement}};
        if (!conversationUrl.isEmpty()) snapshot.insert(QStringLiteral("conversation_url"), conversationUrl);
        auto event = ProposalHistory::event(QStringLiteral("first_seen"), snapshot, ProposalHistory::digest(before), ProposalHistory::digest(after));
        event.insert(QStringLiteral("agent_conversation_id"), conversation);
        event.insert(QStringLiteral("agent_request_id"), requestId);
        event.insert(QStringLiteral("agent_request_sha256"), requestHash);
        event.insert(QStringLiteral("agent_result"), result);
        events.append(event);
    }
    QList<ProposalHistory::SourceWrite> writes {{originPath, originBytes, originAfter}};
    if (!sameFile) writes.append({destination, destinationBytes, destinationAfter});
    if (!ProposalHistory::writeSources(root, writes, events, &detail)) return error(QStringLiteral("write_failed"), detail);
    return result;
}
