/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "agentproposalparser.h"
#include "proposaldecisions.h"
#include "../part/proposalhistory.h"
#include "../part/currentannotationindex.h"
#include "../part/sourceowner.h"
#include "../part/thoughtignore.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTimeZone>
#include <QUuid>

#include <algorithm>
#include <cmath>

// The codecs below follow protocol/agent-proposals.md of the Thoughts server
// section by section; its fixtures run in autotests/agentproposalconformancetest.

namespace
{
using namespace Thoughts;

bool fail(QString *error, const QString &message)
{
    if (error) {
        *error = message;
    }
    return false;
}

bool isAsciiSpace(QChar c)
{
    const char16_t u = c.unicode();
    return u == u' ' || u == u'\t' || u == u'\n' || u == u'\r' || u == u'\f' || u == u'\v';
}

bool isAsciiDigit(QChar c)
{
    return c.unicode() >= u'0' && c.unicode() <= u'9';
}

bool isLowerHex(QChar c)
{
    return isAsciiDigit(c) || (c.unicode() >= u'a' && c.unicode() <= u'f');
}

QString trimmedAscii(QStringView value)
{
    qsizetype first = 0, last = value.size();
    while (first < last && isAsciiSpace(value.at(first))) ++first;
    while (last > first && isAsciiSpace(value.at(last - 1))) --last;
    return value.mid(first, last - first).toString();
}

bool blank(QStringView value)
{
    return std::all_of(value.begin(), value.end(), isAsciiSpace);
}

// 1-based line numbers for many offsets of one source.
class LineIndex
{
public:
    explicit LineIndex(const QString &source)
    {
        for (qsizetype i = 0; i < source.size(); ++i)
            if (source.at(i) == QLatin1Char('\n')) m_newlines.append(int(i));
    }
    int lineOf(int offset) const
    {
        return int(std::lower_bound(m_newlines.cbegin(), m_newlines.cend(), offset) - m_newlines.cbegin()) + 1;
    }

private:
    QVector<int> m_newlines;
};

// A wrapper found by a codec: a valid proposal (code empty) or a broken one.
struct Wrapper {
    QString code;
    int start = 0;
    int end = 0;
    QString id;
    bool hasId = false;
    bool orphanEnd = false; // Markdown end marker no start consumed: its ID is not counted for duplicates.
    AgentProposedEdit proposal;
};

// §6: duplicate IDs, then proposals and issues in source order.
AgentProposalParse finish(const QString &source, QVector<Wrapper> wrappers)
{
    QHash<QString, int> counts;
    for (const Wrapper &w : std::as_const(wrappers))
        if (w.hasId && !w.orphanEnd) ++counts[w.id];
    const LineIndex lines(source);
    AgentProposalParse parse;
    for (Wrapper &w : wrappers) {
        if (w.code.isEmpty() && counts.value(w.id) > 1) w.code = QLatin1String(AgentProposalIssueCode::DuplicateId);
        if (w.code.isEmpty()) {
            w.proposal.offset = w.start;
            w.proposal.length = w.end - w.start;
            w.proposal.line = lines.lineOf(w.start);
            parse.proposals.append(w.proposal);
        } else {
            AgentProposalIssue issue;
            issue.code = w.code;
            issue.id = w.id;
            issue.hasId = w.hasId;
            issue.offset = w.start;
            issue.length = w.end - w.start;
            issue.line = lines.lineOf(w.start);
            parse.issues.append(issue);
        }
    }
    std::stable_sort(parse.issues.begin(), parse.issues.end(), [](const auto &a, const auto &b) { return a.offset < b.offset; });
    return parse;
}

// ------------------------------------------------------------------ LaTeX (§4)

// §4.1: preceded by an odd number of consecutive backslashes.
bool escapedAt(const QString &text, int position)
{
    int count = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) ++count;
    return count % 2 == 1;
}

bool isControlWordLetter(QChar c)
{
    const char16_t u = c.unicode();
    return (u >= u'A' && u <= u'Z') || (u >= u'a' && u <= u'z') || u == u'@';
}

// Whitespace and comments between the name, the metadata and the arguments.
int skipSpace(const QString &text, int i)
{
    const int n = int(text.size());
    while (i < n) {
        if (isAsciiSpace(text.at(i))) {
            ++i;
        } else if (text.at(i) == QLatin1Char('%') && !escapedAt(text, i)) {
            const int newline = int(text.indexOf(QLatin1Char('\n'), i));
            i = newline < 0 ? n : newline + 1;
        } else {
            break;
        }
    }
    return i;
}

struct Group {
    int contentStart = 0;
    int contentEnd = 0;
    int end = 0;
};

// text[i] == '{'. False when the group is not closed before end of file.
bool readGroup(const QString &text, int i, Group *group)
{
    const int n = int(text.size());
    int depth = 1;
    int j = i + 1;
    while (j < n) {
        const QChar c = text.at(j);
        if (c == QLatin1Char('\\')) {
            j += 2;
            continue;
        }
        if (c == QLatin1Char('%')) {
            const int newline = int(text.indexOf(QLatin1Char('\n'), j));
            if (newline < 0) return false;
            j = newline + 1;
            continue;
        }
        if (c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char('}') && --depth == 0) {
            *group = {i + 1, j, j + 1};
            return true;
        }
        ++j;
    }
    return false;
}

const QSet<QString> &definers()
{
    static const QSet<QString> words {QStringLiteral("def"), QStringLiteral("gdef"), QStringLiteral("edef"), QStringLiteral("xdef"), QStringLiteral("let"),
                                      QStringLiteral("newcommand"), QStringLiteral("renewcommand"), QStringLiteral("providecommand"),
                                      QStringLiteral("DeclareRobustCommand"), QStringLiteral("NewDocumentCommand"), QStringLiteral("RenewDocumentCommand"),
                                      QStringLiteral("ProvideDocumentCommand"), QStringLiteral("DeclareDocumentCommand"), QStringLiteral("newrobustcmd"),
                                      QStringLiteral("renewrobustcmd"), QStringLiteral("providerobustcmd")};
    return words;
}

const QSet<QString> &verbatimEnvironments()
{
    static const QSet<QString> names {QStringLiteral("verbatim"), QStringLiteral("verbatim*"), QStringLiteral("Verbatim"), QStringLiteral("Verbatim*"),
                                      QStringLiteral("BVerbatim"), QStringLiteral("BVerbatim*"), QStringLiteral("LVerbatim"), QStringLiteral("LVerbatim*"),
                                      QStringLiteral("lstlisting"), QStringLiteral("minted"), QStringLiteral("DocCode")};
    return names;
}

// §4.2 (4): preceded, ignoring whitespace, by an optional '{', an optional '*'
// and an unescaped defining control word.
bool isDefinition(const QString &text, int start)
{
    int k = start;
    const auto back = [&] { while (k > 0 && isAsciiSpace(text.at(k - 1))) --k; };
    back();
    if (k > 0 && text.at(k - 1) == QLatin1Char('{')) {
        --k;
        back();
    }
    if (k > 0 && text.at(k - 1) == QLatin1Char('*')) {
        --k;
        back();
    }
    int j = k;
    while (j > 0 && isControlWordLetter(text.at(j - 1))) --j;
    return j < k && j > 0 && text.at(j - 1) == QLatin1Char('\\') && !escapedAt(text, j - 1) && definers().contains(text.mid(j, k - j));
}

const QString &wrapperName()
{
    static const QString name = QStringLiteral("agentProposedEdit");
    return name;
}

// The next live \agentProposedEdit in [i, hi) (§4.2). Comments, verbatim
// regions, definitions and mentions are skipped.
bool nextCandidate(const QString &text, int i, int hi, int *start, int *afterName)
{
    const int n = int(text.size());
    while (i < hi) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('%')) {
            const int newline = int(text.indexOf(QLatin1Char('\n'), i));
            i = newline < 0 || newline >= hi ? hi : newline + 1;
            continue;
        }
        if (c != QLatin1Char('\\')) {
            ++i;
            continue;
        }
        int after = i + 1;
        while (after < hi && isControlWordLetter(text.at(after))) ++after;
        if (after == i + 1) {
            i += 2;
            continue;
        }
        const QStringView word = QStringView(text).mid(i + 1, after - i - 1);
        if (word == QLatin1String("begin")) {
            int k = after;
            while (k < hi && (text.at(k) == QLatin1Char(' ') || text.at(k) == QLatin1Char('\t'))) ++k;
            if (k < hi && text.at(k) == QLatin1Char('{')) {
                int close = int(text.indexOf(QLatin1Char('}'), k + 1));
                if (close >= hi) close = -1;
                const QString name = close >= 0 ? text.mid(k + 1, close - k - 1) : QString();
                if (verbatimEnvironments().contains(name)) {
                    const QString closing = QStringLiteral("\\end{") + name + QLatin1Char('}');
                    const int end = int(text.indexOf(closing, close + 1));
                    i = end < 0 || end + closing.size() > hi ? hi : end + int(closing.size());
                    continue;
                }
            }
        }
        if (word == QLatin1String("verb") || word == QLatin1String("lstinline")) {
            int k = after;
            if (word == QLatin1String("verb") && after < hi && text.at(after) == QLatin1Char('*')) ++k;
            if (k < hi) {
                // The delimiter is one code point, which may be a surrogate pair.
                const int width = text.at(k).isHighSurrogate() && k + 1 < n && text.at(k + 1).isLowSurrogate() ? 2 : 1;
                const char32_t delimiter = width == 2 ? QChar::surrogateToUcs4(text.at(k), text.at(k + 1)) : text.at(k).unicode();
                const bool letter = QChar::isLetter(delimiter);
                if (!isAsciiSpace(text.at(k)) && text.at(k) != QLatin1Char('[') && text.at(k) != QLatin1Char('{') && !letter) {
                    int close = int(text.indexOf(QStringView(text).mid(k, width), k + width));
                    if (close >= 0 && close + width > hi) close = -1;
                    int newline = int(text.indexOf(QLatin1Char('\n'), k + 1));
                    if (newline >= hi) newline = -1;
                    if (close >= 0 && (newline < 0 || close < newline)) {
                        i = close + width;
                        continue;
                    }
                }
            }
        }
        if (word == wrapperName() && !isDefinition(text, i)) {
            const int k = skipSpace(text, after);
            if (k < n && (text.at(k) == QLatin1Char('{') || text.at(k) == QLatin1Char('['))) {
                *start = i;
                *afterName = after;
                return true;
            }
        }
        i = after;
    }
    return false;
}

bool containsLiveWrapper(const QString &text, int from, int to)
{
    int start = 0, after = 0;
    return nextCandidate(text, from, to, &start, &after);
}

// §4.3 metadata, mandatory since 2026-10-10: exactly created=<ts>,conversation_id=<uuid>,
// optionally followed by ,conversation_url=<link> (§3.6).
bool readLatexMetadata(const QString &content, QString *created, QString *conversation, QString *url)
{
    static const QString createdKey = QStringLiteral("created=");
    static const QString conversationKey = QStringLiteral(",conversation_id=");
    static const QString urlKey = QStringLiteral(",conversation_url=");
    if (!content.startsWith(createdKey)) return false;
    const QString rest = content.mid(createdKey.size());
    const int comma = int(rest.indexOf(QLatin1Char(',')));
    if (comma < 0) return false;
    *created = rest.left(comma);
    const QString tail = rest.mid(comma);
    if (!tail.startsWith(conversationKey)) return false;
    *conversation = tail.mid(conversationKey.size());
    url->clear();
    const int linkComma = int(conversation->indexOf(QLatin1Char(',')));
    if (linkComma >= 0) {
        const QString link = conversation->mid(linkComma);
        if (!link.startsWith(urlKey)) return false;
        *url = link.mid(urlKey.size());
        conversation->truncate(linkComma);
        if (!isValidAgentProposalConversationUrl(*url)) return false;
    }
    return isValidAgentProposalConversationId(*conversation) && isValidAgentProposalTimestamp(*created);
}

// ------------------------------------------------------------------ Markdown (§5)

const QString &markerPrefix()
{
    static const QString prefix = QStringLiteral("<!-- thoughts:agent-proposal:");
    return prefix;
}

// The next marker line start at or after i whose prefix lies before limit.
int nextMarker(const QString &text, int i, int limit)
{
    while (true) {
        const int s = int(text.indexOf(markerPrefix(), i));
        if (s < 0 || s + markerPrefix().size() > limit) return -1;
        if (s == 0 || text.at(s - 1) == QLatin1Char('\n')) return s;
        i = s + 1;
    }
}

struct Line {
    QString content;
    int end = 0;
    bool terminated = false;
};

Line lineAt(const QString &text, int s)
{
    const int newline = int(text.indexOf(QLatin1Char('\n'), s));
    if (newline < 0) return {text.mid(s), int(text.size()), false};
    return {text.mid(s, newline - s), newline + 1, true};
}

struct MarkdownFields {
    QString createdAt;
    QString conversationId;
    QString conversationUrl;
    bool hasConversationUrl = false; // an empty link line is invalid, not absent
    QString explanation;
    QString original;
};

bool decodeBlock(const QStringList &lines, int *position, const QString &label, QString *value)
{
    if (*position >= lines.size() || lines.at(*position) != label) return false;
    ++*position;
    QStringList parts;
    while (*position < lines.size() && lines.at(*position).startsWith(QStringLiteral("| "))) {
        QString part = lines.at((*position)++).mid(2);
        part.replace(QStringLiteral("--&gt;"), QStringLiteral("-->"));
        part.replace(QStringLiteral("&amp;"), QStringLiteral("&"));
        parts.append(part);
    }
    *value = parts.join(QLatin1Char('\n'));
    return true;
}

// §5.2 header. `id` is empty when there is no readable id line.
bool readV2Header(const QStringList &lines, MarkdownFields *fields, QString *id)
{
    *id = !lines.isEmpty() && lines.first().startsWith(QStringLiteral("id: ")) ? lines.first().mid(4) : QString();
    static const QString created = QStringLiteral("created_at: ");
    static const QString conversation = QStringLiteral("conversation_id: ");
    if (lines.size() < 3 || !lines.at(1).startsWith(created) || !lines.at(2).startsWith(conversation)) return false;
    fields->createdAt = lines.at(1).mid(created.size());
    fields->conversationId = lines.at(2).mid(conversation.size());
    int position = 3;
    static const QString url = QStringLiteral("conversation_url: ");
    if (position < lines.size() && lines.at(position).startsWith(url)) {
        fields->conversationUrl = lines.at(position++).mid(url.size());
        fields->hasConversationUrl = true;
    }
    if (position < lines.size() && lines.at(position).startsWith(QStringLiteral("replacement_length: "))) ++position;
    if (!decodeBlock(lines, &position, QStringLiteral("explanation:"), &fields->explanation)) return false;
    if (!decodeBlock(lines, &position, QStringLiteral("original_text:"), &fields->original)) return false;
    return position == lines.size();
}

// §5.3 data. `id` is empty when the data has no string id.
bool readV1Header(const QString &encoded, MarkdownFields *fields, QString *id)
{
    *id = QString();
    static const QRegularExpression alphabet(QStringLiteral("\\A[A-Za-z0-9_-]+\\z"));
    if (!alphabet.match(encoded).hasMatch()) return false;
    const auto decoded = QByteArray::fromBase64Encoding(encoded.toLatin1(), QByteArray::Base64UrlEncoding | QByteArray::AbortOnBase64DecodingErrors);
    if (!decoded) return false;
    const QByteArray raw = *decoded;
    if (QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)) != encoded) return false;
    if (QString::fromUtf8(raw).toUtf8() != raw) return false;
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(raw, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return false;
    const QJsonObject data = document.object();
    if (data.value(QStringLiteral("id")).isString()) *id = data.value(QStringLiteral("id")).toString();
    for (const char *key : {"created_at", "conversation_id", "explanation", "original_text"})
        if (!data.value(QLatin1String(key)).isString()) return false;
    fields->createdAt = data.value(QStringLiteral("created_at")).toString();
    fields->conversationId = data.value(QStringLiteral("conversation_id")).toString();
    fields->explanation = data.value(QStringLiteral("explanation")).toString();
    fields->original = data.value(QStringLiteral("original_text")).toString();
    if (data.contains(QStringLiteral("conversation_url"))) {
        if (!data.value(QStringLiteral("conversation_url")).isString()) return false;
        fields->conversationUrl = data.value(QStringLiteral("conversation_url")).toString();
        fields->hasConversationUrl = true;
    }
    return true;
}

// The end marker line for (version, id) at a line start, at or after i.
int findEndMarker(const QString &text, const QString &version, const QString &id, int i, int *end)
{
    const QString line = markerPrefix() + version + QStringLiteral(":end ") + id + QStringLiteral(" -->\n");
    while (true) {
        const int e = int(text.indexOf(line, i));
        if (e < 0) return -1;
        if (e == 0 || text.at(e - 1) == QLatin1Char('\n')) {
            *end = e + int(line.size());
            return e;
        }
        i = e + 1;
    }
}

bool startsMarkerLine(const QString &text)
{
    return nextMarker(text, 0, int(text.size())) >= 0;
}

QString issueText(const char *code)
{
    return QLatin1String(code);
}

}

namespace Thoughts
{

AgentProposalFormat agentProposalFormat(const QString &path)
{
    if (path.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive)) return AgentProposalFormat::Latex;
    if (path.endsWith(QLatin1String(".md"), Qt::CaseInsensitive) || path.endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive))
        return AgentProposalFormat::Markdown;
    return AgentProposalFormat::Unsupported;
}

bool isAgentProposalSourcePath(const QString &path)
{
    return agentProposalFormat(path) != AgentProposalFormat::Unsupported;
}

const AgentProposedEdit *AgentProposalParse::proposal(const QString &id) const
{
    const AgentProposedEdit *found = nullptr;
    for (const auto &candidate : proposals) {
        if (candidate.id != id) continue;
        if (found) return nullptr;
        found = &candidate;
    }
    return found;
}

QString AgentProposalParse::refusalCode(const QString &id) const
{
    if (proposal(id)) return {};
    for (const auto &issue : issues)
        if (issue.hasId && issue.id == id) return issue.code;
    return QLatin1String(AgentProposalIssueCode::UnknownId);
}

QSet<QString> AgentProposalParse::ids() const
{
    QSet<QString> result;
    for (const auto &proposal : proposals) result.insert(proposal.id);
    for (const auto &issue : issues)
        if (issue.hasId) result.insert(issue.id);
    return result;
}

bool isValidAgentProposalId(const QString &value)
{
    if (value.isEmpty() || value.size() > 128) return false;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const char16_t c = value.at(i).unicode();
        const bool alnum = (c >= u'a' && c <= u'z') || (c >= u'0' && c <= u'9');
        if (!alnum && (i == 0 || (c != u'.' && c != u'_' && c != u':' && c != u'-'))) return false;
    }
    return true;
}

bool isValidAgentProposalConversationId(const QString &value)
{
    if (value.size() != 36) return false;
    for (qsizetype i = 0; i < value.size(); ++i) {
        const bool hyphen = i == 8 || i == 13 || i == 18 || i == 23;
        if (hyphen ? value.at(i) != QLatin1Char('-') : !isLowerHex(value.at(i))) return false;
    }
    return true;
}

bool isValidAgentProposalConversationUrl(const QString &value)
{
    static const QString claude = QStringLiteral("https://claude.ai/code/session_");
    static const QString codex = QStringLiteral("codex://threads/");
    if (value.startsWith(codex)) return isValidAgentProposalConversationId(value.mid(codex.size()));
    if (!value.startsWith(claude)) return false;
    const QStringView session = QStringView(value).mid(claude.size());
    if (session.isEmpty() || session.size() > 64) return false;
    for (const QChar c : session) {
        const char16_t u = c.unicode();
        if (!((u >= u'a' && u <= u'z') || (u >= u'A' && u <= u'Z') || (u >= u'0' && u <= u'9'))) return false;
    }
    return true;
}

namespace
{
// YYYY-MM-DDTHH:MM:SS[.F]Z with a real date (§3.2).
bool readTimestamp(const QString &value, int parts[6], int *milliseconds)
{
    if (value.size() < 20 || value.size() > 30 || value.back() != QLatin1Char('Z')) return false;
    static const char layout[] = "dddd-dd-ddTdd:dd:dd";
    for (int i = 0; i < 19; ++i) {
        if (layout[i] == 'd' ? !isAsciiDigit(value.at(i)) : value.at(i) != QLatin1Char(layout[i])) return false;
    }
    const int fraction = int(value.size()) - 20; // characters between seconds and Z
    if (fraction == 1 || fraction > 10) return false;
    if (fraction > 0) {
        if (value.at(19) != QLatin1Char('.')) return false;
        for (int i = 20; i < int(value.size()) - 1; ++i)
            if (!isAsciiDigit(value.at(i))) return false;
    }
    const auto number = [&](int from, int count) { return value.mid(from, count).toInt(); };
    parts[0] = number(0, 4);
    parts[1] = number(5, 2);
    parts[2] = number(8, 2);
    parts[3] = number(11, 2);
    parts[4] = number(14, 2);
    parts[5] = number(17, 2);
    const int year = parts[0], month = parts[1], day = parts[2];
    if (year < 1 || month < 1 || month > 12 || day < 1) return false;
    static const int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (day > days[month - 1] + (month == 2 && leap ? 1 : 0)) return false;
    if (parts[3] > 23 || parts[4] > 59 || parts[5] > 59) return false;
    if (milliseconds) *milliseconds = fraction > 0 ? QString(value.mid(20, fraction - 1) + QStringLiteral("00")).left(3).toInt() : 0;
    return true;
}
}

bool isValidAgentProposalTimestamp(const QString &value)
{
    int parts[6];
    return readTimestamp(value, parts, nullptr);
}

QDateTime agentProposalTimestampDateTime(const QString &value)
{
    int parts[6];
    int milliseconds = 0;
    if (!readTimestamp(value, parts, &milliseconds)) return {};
    return QDateTime(QDate(parts[0], parts[1], parts[2]), QTime(parts[3], parts[4], parts[5], milliseconds), QTimeZone::UTC);
}

QString formatAgentProposalTimestamp(const QDateTime &value)
{
    const QDateTime utc = value.toUTC();
    const QDate date = utc.date();
    const QTime time = utc.time();
    return QString::asprintf("%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", date.year(), date.month(), date.day(), time.hour(), time.minute(), time.second(), time.msec());
}

AgentProposalParse parseLatexAgentProposals(const QString &text)
{
    QVector<Wrapper> wrappers;
    if (!text.contains(QStringLiteral("\\agentProposedEdit"))) return {};
    const int n = int(text.size());
    int i = 0;
    int start = 0, cursor = 0;
    while (nextCandidate(text, i, n, &start, &cursor)) {
        Wrapper wrapper;
        wrapper.start = start;
        bool metadataError = true; // a wrapper without metadata is invalid_metadata (§4.3)
        QString created, conversation, conversationUrl;
        int k = skipSpace(text, cursor);
        if (k < n && text.at(k) == QLatin1Char('[')) {
            const int close = int(text.indexOf(QLatin1Char(']'), k + 1));
            if (close < 0) {
                wrapper.code = issueText(AgentProposalIssueCode::UnclosedGroup);
                wrapper.end = n;
                wrappers.append(wrapper);
                break;
            }
            metadataError = !readLatexMetadata(trimmedAscii(QStringView(text).mid(k + 1, close - k - 1)), &created, &conversation, &conversationUrl);
            if (metadataError) created.clear(), conversation.clear(), conversationUrl.clear();
            cursor = close + 1;
        }
        QVector<Group> groups;
        bool unclosed = false;
        while (groups.size() < 4) {
            k = skipSpace(text, cursor);
            if (k >= n || text.at(k) != QLatin1Char('{')) break;
            Group group;
            if (!readGroup(text, k, &group)) {
                unclosed = true;
                break;
            }
            groups.append(group);
            cursor = group.end;
            if (groups.size() == 1) {
                wrapper.id = trimmedAscii(QStringView(text).mid(group.contentStart, group.contentEnd - group.contentStart));
                wrapper.hasId = true;
            }
        }
        if (unclosed) {
            wrapper.code = issueText(AgentProposalIssueCode::UnclosedGroup);
            wrapper.end = n;
            wrappers.append(wrapper);
            break;
        }
        wrapper.end = cursor;
        i = cursor;
        if (groups.size() < 4) {
            wrapper.code = issueText(AgentProposalIssueCode::MissingArgument);
            wrappers.append(wrapper);
            continue;
        }
        const auto content = [&](const Group &group) { return text.mid(group.contentStart, group.contentEnd - group.contentStart); };
        const QString explanation = content(groups[1]);
        if (metadataError || blank(explanation)) {
            wrapper.code = issueText(AgentProposalIssueCode::InvalidMetadata);
        } else if (!isValidAgentProposalId(wrapper.id)) {
            wrapper.code = issueText(AgentProposalIssueCode::InvalidId);
        } else if (containsLiveWrapper(text, groups[0].contentStart, groups[3].contentEnd)) {
            wrapper.code = issueText(AgentProposalIssueCode::NestedProposal);
        }
        if (wrapper.code.isEmpty()) {
            AgentProposedEdit &proposal = wrapper.proposal;
            proposal.id = wrapper.id;
            proposal.createdAt = created;
            proposal.conversationId = conversation;
            proposal.conversationUrl = conversationUrl;
            proposal.explanation = explanation;
            proposal.original = content(groups[2]);
            proposal.replacement = content(groups[3]);
            proposal.explanationOffset = groups[1].contentStart;
            proposal.explanationLength = groups[1].contentEnd - groups[1].contentStart;
            proposal.replacementOffset = groups[3].contentStart;
            proposal.replacementLength = groups[3].contentEnd - groups[3].contentStart;
        }
        wrappers.append(wrapper);
    }
    return finish(text, wrappers);
}

QVector<QPair<int, int>> latexAgentProposalHiddenRanges(const QString &text, bool *incomplete)
{
    QVector<QPair<int, int>> ranges;
    if (incomplete) *incomplete = false;
    if (!text.contains(QStringLiteral("\\agentProposedEdit"))) return ranges;
    const int n = int(text.size());
    int i = 0, start = 0, cursor = 0;
    while (nextCandidate(text, i, n, &start, &cursor)) {
        int k = skipSpace(text, cursor);
        if (k < n && text.at(k) == QLatin1Char('[')) {
            const int close = int(text.indexOf(QLatin1Char(']'), k + 1));
            if (close < 0) {
                if (incomplete) *incomplete = true;
                ranges.append({start, n});
                break;
            }
            cursor = close + 1;
        }
        QVector<Group> groups;
        bool unclosed = false;
        while (groups.size() < 4) {
            k = skipSpace(text, cursor);
            if (k >= n || text.at(k) != QLatin1Char('{')) break;
            Group group;
            if (!readGroup(text, k, &group)) {
                unclosed = true;
                break;
            }
            groups.append(group);
            cursor = group.end;
        }
        if (unclosed) {
            if (incomplete) *incomplete = true;
            ranges.append({start, n});
            break;
        }
        if (groups.size() < 4) {
            if (incomplete) *incomplete = true;
            i = cursor;
            continue;
        }
        ranges.append({start, groups[2].end});
        i = groups[3].contentStart;
    }
    return ranges;
}

AgentProposalParse parseMarkdownAgentProposals(const QString &text)
{
    QVector<Wrapper> wrappers;
    if (!text.contains(markerPrefix())) return {};
    static const QString v2Start = QStringLiteral("<!-- thoughts:agent-proposal:v2:start");
    static const QRegularExpression v1Start(QStringLiteral("\\A<!-- thoughts:agent-proposal:v1:start ([^ ]*) -->\\z"));
    static const QRegularExpression endMarker(QStringLiteral("\\A<!-- thoughts:agent-proposal:(v1|v2):end (.*) -->\\z"),
                                              QRegularExpression::DotMatchesEverythingOption);
    const int n = int(text.size());
    int i = 0;
    while (true) {
        const int s = nextMarker(text, i, n);
        if (s < 0) break;
        const Line marker = lineAt(text, s);
        QString version;
        int headerEnd = -1;
        MarkdownFields fields;
        bool fieldsValid = false;
        QString id;
        QRegularExpressionMatch match;
        if (marker.terminated && marker.content == v2Start) {
            version = QStringLiteral("v2");
            QStringList lines;
            int j = marker.end;
            while (j < n) {
                const Line line = lineAt(text, j);
                if (line.terminated && line.content == QLatin1String("-->")) {
                    headerEnd = line.end;
                    break;
                }
                lines.append(line.content);
                j = line.end;
            }
            if (headerEnd < 0) {
                Wrapper wrapper;
                wrapper.code = issueText(AgentProposalIssueCode::UnclosedGroup);
                wrapper.start = s;
                wrapper.end = n;
                wrappers.append(wrapper);
                break;
            }
            fieldsValid = readV2Header(lines, &fields, &id);
        } else if (marker.terminated && (match = v1Start.match(marker.content)).hasMatch()) {
            version = QStringLiteral("v1");
            headerEnd = marker.end;
            fieldsValid = readV1Header(match.captured(1), &fields, &id);
        } else if (marker.terminated && (match = endMarker.match(marker.content)).hasMatch()) {
            // The ID names the issue for refusals but does not count towards duplicates.
            Wrapper wrapper;
            wrapper.code = issueText(AgentProposalIssueCode::UnmatchedMarker);
            wrapper.start = s;
            wrapper.end = marker.end;
            wrapper.id = match.captured(2);
            wrapper.hasId = true;
            wrapper.orphanEnd = true;
            wrappers.append(wrapper);
            i = marker.end;
            continue;
        } else {
            Wrapper wrapper;
            wrapper.code = issueText(AgentProposalIssueCode::MalformedMarker);
            wrapper.start = s;
            wrapper.end = marker.end;
            wrappers.append(wrapper);
            i = marker.end;
            continue;
        }
        Wrapper wrapper;
        wrapper.start = s;
        if (id.isEmpty()) { // no id, or an empty one: the start cannot be paired with an end marker
            wrapper.code = issueText(AgentProposalIssueCode::InvalidMetadata);
            wrapper.end = headerEnd;
            wrappers.append(wrapper);
            i = headerEnd;
            continue;
        }
        wrapper.id = id;
        wrapper.hasId = true;
        int end = 0;
        const int e = findEndMarker(text, version, id, headerEnd, &end);
        if (e < 0) {
            wrapper.code = issueText(AgentProposalIssueCode::UnmatchedMarker);
            wrapper.end = headerEnd;
            wrappers.append(wrapper);
            i = headerEnd;
            continue;
        }
        i = end;
        wrapper.end = end;
        if (!fieldsValid || !isValidAgentProposalTimestamp(fields.createdAt) || !isValidAgentProposalConversationId(fields.conversationId)
            || blank(fields.explanation) || (fields.hasConversationUrl && !isValidAgentProposalConversationUrl(fields.conversationUrl))) {
            wrapper.code = issueText(AgentProposalIssueCode::InvalidMetadata);
        } else if (!isValidAgentProposalId(id)) {
            wrapper.code = issueText(AgentProposalIssueCode::InvalidId);
        } else if (nextMarker(text, headerEnd, e) >= 0) {
            wrapper.code = issueText(AgentProposalIssueCode::NestedProposal);
        } else {
            AgentProposedEdit &proposal = wrapper.proposal;
            proposal.id = id;
            proposal.createdAt = fields.createdAt;
            proposal.conversationId = fields.conversationId;
            proposal.conversationUrl = fields.conversationUrl;
            proposal.explanation = fields.explanation;
            proposal.original = fields.original;
            proposal.replacement = text.mid(headerEnd, e - headerEnd);
            proposal.replacementOffset = headerEnd;
            proposal.replacementLength = e - headerEnd;
        }
        wrappers.append(wrapper);
    }
    return finish(text, wrappers);
}

AgentProposalParse parseAgentProposals(AgentProposalFormat format, const QString &source)
{
    switch (format) {
    case AgentProposalFormat::Latex:
        return parseLatexAgentProposals(source);
    case AgentProposalFormat::Markdown:
        return parseMarkdownAgentProposals(source);
    case AgentProposalFormat::Unsupported:
        break;
    }
    return {};
}

AgentProposalParse parseAgentProposals(const QString &path, const QString &source)
{
    return parseAgentProposals(agentProposalFormat(path), source);
}

QString agentProposalIssueMessage(const QString &code)
{
    if (code == QLatin1String(AgentProposalIssueCode::UnclosedGroup))
        return QStringLiteral("A brace, bracket or header of this proposal is never closed, so it runs to the end of the file.");
    if (code == QLatin1String(AgentProposalIssueCode::MissingArgument))
        return QStringLiteral("This \\agentProposedEdit has fewer than four braced arguments.");
    if (code == QLatin1String(AgentProposalIssueCode::MalformedMarker))
        return QStringLiteral("This proposal marker line is not a valid marker (check for extra text or a trailing carriage return).");
    if (code == QLatin1String(AgentProposalIssueCode::UnmatchedMarker))
        return QStringLiteral("This proposal marker has no matching start or end marker.");
    if (code == QLatin1String(AgentProposalIssueCode::InvalidMetadata))
        return QStringLiteral("The proposal's metadata, creation time, conversation ID or explanation is invalid.");
    if (code == QLatin1String(AgentProposalIssueCode::InvalidId))
        return QStringLiteral("The proposal ID is invalid: use 1 to 128 characters a-z, 0-9, '.', '_', ':' or '-', starting with a letter or digit.");
    if (code == QLatin1String(AgentProposalIssueCode::NestedProposal))
        return QStringLiteral("Another proposal is nested inside this one.");
    if (code == QLatin1String(AgentProposalIssueCode::DuplicateId))
        return QStringLiteral("Another proposal in this file has the same ID.");
    if (code == QLatin1String(AgentProposalIssueCode::UnknownId))
        return QStringLiteral("No pending proposal with this ID is in the source.");
    if (code == QLatin1String("unbalanced_braces"))
        return QStringLiteral("The braces of the text do not balance, so it cannot be a TeX argument.");
    if (code == QLatin1String("unterminated_comment"))
        return QStringLiteral("The text ends inside a TeX comment; include the newline after a trailing comment.");
    return QStringLiteral("The proposal is invalid (%1).").arg(code);
}

QString agentProposalIssueMessage(const AgentProposalIssue &issue)
{
    return agentProposalIssueMessage(issue.code);
}

bool decideAgentProposals(const QString &path, const QString &source, const QStringList &ids, const QString &action,
                          QString *result, QString *code, QString *message)
{
    const auto refuse = [&](const QString &refusal) {
        if (code) *code = refusal;
        if (message) *message = agentProposalIssueMessage(refusal);
        return false;
    };
    const bool accept = action == QLatin1String("accept");
    if (!result || (!accept && action != QLatin1String("reject") && action != QLatin1String("disregard"))) {
        if (code) *code = QStringLiteral("invalid_request");
        if (message) *message = QStringLiteral("The proposal decision is invalid.");
        return false;
    }
    if (ids.isEmpty()) return refuse(QLatin1String(AgentProposalIssueCode::UnknownId));
    const AgentProposalParse parse = parseAgentProposals(path, source);
    QVector<const AgentProposedEdit *> chosen;
    for (const QString &id : ids) {
        const QString refusal = parse.refusalCode(id);
        if (!refusal.isEmpty()) return refuse(refusal);
        const AgentProposedEdit *proposal = parse.proposal(id);
        if (!chosen.contains(proposal)) chosen.append(proposal);
    }
    std::sort(chosen.begin(), chosen.end(), [](const auto *a, const auto *b) { return a->offset > b->offset; });
    QString output = source;
    for (const AgentProposedEdit *proposal : std::as_const(chosen))
        output.replace(proposal->offset, proposal->length, accept ? proposal->replacement : proposal->original);
    *result = output;
    if (code) code->clear();
    return true;
}

QString escapeLatexProposalExplanation(const QString &text)
{
    QString result;
    for (const QChar ch : text) {
        if (QStringLiteral("{}%$&#_").contains(ch)) result += QLatin1Char('\\') + QString(ch);
        else if (ch == QLatin1Char('\\')) result += QStringLiteral("\\textbackslash{}");
        else if (ch == QLatin1Char('^')) result += QStringLiteral("\\textasciicircum{}");
        else if (ch == QLatin1Char('~')) result += QStringLiteral("\\textasciitilde{}");
        else result += ch;
    }
    return result;
}

QString agentProposalTextRefusal(AgentProposalFormat format, const QString &text)
{
    if (format == AgentProposalFormat::Markdown) return startsMarkerLine(text) ? QLatin1String(AgentProposalIssueCode::NestedProposal) : QString();
    if (format != AgentProposalFormat::Latex) return {};
    int depth = 0;
    const int n = int(text.size());
    for (int i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (c == QLatin1Char('\\')) {
            // A final backslash would escape the closing brace of the argument.
            if (++i >= n) return QStringLiteral("unbalanced_braces");
        } else if (c == QLatin1Char('%')) {
            const int newline = int(text.indexOf(QLatin1Char('\n'), i));
            if (newline < 0) return QStringLiteral("unterminated_comment");
            i = newline;
        } else if (c == QLatin1Char('{')) {
            ++depth;
        } else if (c == QLatin1Char('}') && --depth < 0) {
            return QStringLiteral("unbalanced_braces");
        }
    }
    if (depth != 0) return QStringLiteral("unbalanced_braces");
    return containsLiveWrapper(text, 0, n) ? QLatin1String(AgentProposalIssueCode::NestedProposal) : QString();
}

namespace
{
QString commonRefusal(const AgentProposedEdit &proposal)
{
    if (!isValidAgentProposalId(proposal.id)) return QLatin1String(AgentProposalIssueCode::InvalidId);
    if (!isValidAgentProposalTimestamp(proposal.createdAt) || !isValidAgentProposalConversationId(proposal.conversationId) || blank(proposal.explanation)
        || (!proposal.conversationUrl.isEmpty() && !isValidAgentProposalConversationUrl(proposal.conversationUrl)))
        return QLatin1String(AgentProposalIssueCode::InvalidMetadata);
    return {};
}

QString refused(QString *refusal, const QString &code)
{
    if (refusal) *refusal = code;
    return {};
}
}

QString formatLatexAgentProposal(const AgentProposedEdit &proposal, QString *refusal)
{
    QString code = commonRefusal(proposal);
    if (code.isEmpty()) code = agentProposalTextRefusal(AgentProposalFormat::Latex, proposal.original);
    if (code.isEmpty()) code = agentProposalTextRefusal(AgentProposalFormat::Latex, proposal.replacement);
    if (!code.isEmpty()) return refused(refusal, code);
    if (refusal) refusal->clear();
    return QStringLiteral("\\agentProposedEdit[created=") + proposal.createdAt + QStringLiteral(",conversation_id=") + proposal.conversationId
        + (proposal.conversationUrl.isEmpty() ? QString() : QStringLiteral(",conversation_url=") + proposal.conversationUrl)
        + QStringLiteral("]{") + proposal.id + QStringLiteral("}{") + escapeLatexProposalExplanation(proposal.explanation.trimmed())
        + QStringLiteral("\n}{") + proposal.original + QStringLiteral("}{") + proposal.replacement + QLatin1Char('}');
}

QString formatMarkdownAgentProposal(const AgentProposedEdit &proposal, QString *refusal)
{
    QString code = commonRefusal(proposal);
    if (code.isEmpty()) code = agentProposalTextRefusal(AgentProposalFormat::Markdown, proposal.original);
    if (code.isEmpty()) code = agentProposalTextRefusal(AgentProposalFormat::Markdown, proposal.replacement);
    if (!code.isEmpty()) return refused(refusal, code);
    if (refusal) refusal->clear();
    auto block = [](const QString &value) {
        QString lines;
        if (value.isEmpty()) return lines;
        for (QString line : value.split(QLatin1Char('\n'), Qt::KeepEmptyParts)) {
            line.replace(QStringLiteral("&"), QStringLiteral("&amp;"));
            line.replace(QStringLiteral("-->"), QStringLiteral("--&gt;"));
            lines += QStringLiteral("| ") + line + QLatin1Char('\n');
        }
        return lines;
    };
    const QString start = QStringLiteral("<!-- thoughts:agent-proposal:v2:start\n")
        + QStringLiteral("id: ") + proposal.id + QLatin1Char('\n')
        + QStringLiteral("created_at: ") + proposal.createdAt + QLatin1Char('\n')
        + QStringLiteral("conversation_id: ") + proposal.conversationId + QLatin1Char('\n')
        + (proposal.conversationUrl.isEmpty() ? QString() : QStringLiteral("conversation_url: ") + proposal.conversationUrl + QLatin1Char('\n'))
        + QStringLiteral("explanation:\n") + block(proposal.explanation.trimmed())
        + QStringLiteral("original_text:\n") + block(proposal.original)
        + QStringLiteral("-->\n");
    const QString end = QStringLiteral("<!-- thoughts:agent-proposal:v2:end ") + proposal.id + QStringLiteral(" -->\n");
    const bool newline = !proposal.replacement.isEmpty() && !proposal.replacement.endsWith(QLatin1Char('\n'));
    return start + proposal.replacement + (newline ? QStringLiteral("\n") : QString()) + end;
}


bool isGeneratedAgentProposalSource(const QString &relativePath, const QString &text)
{
    if (QString(QLatin1Char('/') + QDir::fromNativeSeparators(relativePath)).contains(QLatin1String("/tex/generated/"))) return true;
    for (const QStringView line : QStringView(text).split(QLatin1Char('\n'))) {
        if (blank(line)) continue;
        for (const char *prefix : {"% Generated artifact", "% Generated by ", "<!-- Generated artifact", "<!-- Generated by "}) {
            if (line.startsWith(QLatin1String(prefix))) return true;
        }
        return false;
    }
    return false;
}

static QJsonObject proposalSnapshot(const AgentProposedEdit &proposal, const QString &path)
{
    QJsonObject snapshot {{QStringLiteral("proposal_id"), proposal.id}, {QStringLiteral("source_file"), path},
            {QStringLiteral("comment"), proposal.explanation}, {QStringLiteral("before"), proposal.original}, {QStringLiteral("after"), proposal.replacement}};
    if (!proposal.createdAt.isEmpty()) snapshot.insert(QStringLiteral("created_at"), proposal.createdAt);
    if (!proposal.conversationId.isEmpty()) snapshot.insert(QStringLiteral("conversation_id"), proposal.conversationId);
    if (!proposal.conversationUrl.isEmpty()) snapshot.insert(QStringLiteral("conversation_url"), proposal.conversationUrl);
    return snapshot;
}

bool observeAgentProposalSource(const QString &docsRoot, const QString &sourcePath, const QByteArray &source,
                                QString *error, QHash<QString, QJsonObject> *latestBatch)
{
    if (!isAgentProposalSourcePath(sourcePath)) return fail(error, QStringLiteral("Unsupported proposal source format."));
    const AgentProposalParse parse = parseAgentProposals(sourcePath, QString::fromUtf8(source));
    const QVector<AgentProposedEdit> &proposals = parse.proposals;
    if (!latestBatch && !ProposalHistory::recover(docsRoot, error)) return false;
    const auto path = QDir(QFileInfo(docsRoot).canonicalFilePath()).relativeFilePath(QFileInfo(sourcePath).canonicalFilePath());
    // A broken wrapper is still in the source: it was neither accepted nor rejected.
    const QSet<QString> present = parse.ids();
    // Open proposals recorded for this source whose markers are gone were
    // resolved outside Thoughts, e.g. accepted or rejected in an editor.
    const auto vanishedFrom = [&](const QHash<QString, QJsonObject> &records) {
        QStringList ids;
        for (auto it = records.cbegin(); it != records.cend(); ++it) {
            if (!present.contains(it.key()) && it.value().value(QStringLiteral("snapshot")).toObject().value(QStringLiteral("source_file")).toString() == path) ids.append(it.key());
        }
        return ids;
    };
    QHash<QString, QJsonObject> latest;
    if (latestBatch) latest = *latestBatch;
    else if (!ProposalHistory::latest(docsRoot, &latest, error)) return false;
    if (proposals.isEmpty() && vanishedFrom(latest).isEmpty()) return true;
    if (!QDir().mkpath(ProposalHistory::directory(docsRoot))) return fail(error, QStringLiteral("Cannot create proposal history directory."));
    QLockFile observationLock(QDir(ProposalHistory::directory(docsRoot)).filePath(QStringLiteral("proposal_observation.lock")));
    if (!observationLock.tryLock(5000)) return fail(error, QStringLiteral("Proposal history observation is busy."));
    QFile currentSource(sourcePath);
    if (!currentSource.open(QIODevice::ReadOnly) || currentSource.readAll() != source) return fail(error, QStringLiteral("The proposal source changed during observation."));
    if (!latestBatch && !ProposalHistory::latest(docsRoot, &latest, error)) return false;
    QJsonArray additions;
    for (const QString &id : vanishedFrom(latest)) {
        const auto previous = latest.value(id);
        additions.append(ProposalHistory::event(QStringLiteral("deleted_from_source"), previous.value(QStringLiteral("snapshot")).toObject(),
                                                previous.value(QStringLiteral("source_after_sha256")).toString(), ProposalHistory::digest(source)));
    }
    for (const auto &proposal : proposals) {
        const auto snapshot = proposalSnapshot(proposal, path);
        const auto previous = latest.value(proposal.id);
        QString kind = QStringLiteral("first_seen");
        if (!previous.isEmpty()) {
            const auto previousKind = previous.value(QStringLiteral("kind")).toString();
            if (previousKind == QLatin1String("accepted") || previousKind == QLatin1String("rejected") || previousKind == QLatin1String("deleted_from_source")) kind = QStringLiteral("restored");
            else if (previous.value(QStringLiteral("snapshot")).toObject() == snapshot) continue;
            else {
                auto previousContent = previous.value(QStringLiteral("snapshot")).toObject();
                const QString previousRelative = previousContent.value(QStringLiteral("source_file")).toString();
                if (previousRelative != path && !previousRelative.isEmpty()) {
                    // A docs-* folder or a Project's default folder applies its own policy.
                    QString previousOwner = SourceOwner::of(previousRelative);
                    if (previousOwner.isEmpty()) previousOwner = previousRelative.section(QLatin1Char('/'), 0, 0);
                    const QFileInfo previousInfo(QDir(docsRoot).filePath(previousRelative));
                    const ThoughtIgnore ignored(QDir(docsRoot).filePath(previousOwner));
                    const bool previousIncluded = ignored.error().isEmpty()
                        && !ignored.excludes(previousRelative.mid(previousOwner.size() + 1));
                    if (previousIncluded && previousInfo.isFile() && !previousInfo.isSymLink()) {
                        QFile previousFile(previousInfo.absoluteFilePath());
                        if (!previousFile.open(QIODevice::ReadOnly) || !isAgentProposalSourcePath(previousInfo.absoluteFilePath()))
                            return fail(error, QStringLiteral("Cannot verify whether proposal %1 is duplicated in its previous source.").arg(proposal.id));
                        const auto previousProposals = parseAgentProposals(previousInfo.absoluteFilePath(), QString::fromUtf8(previousFile.readAll())).proposals;
                        if (std::any_of(previousProposals.cbegin(), previousProposals.cend(), [&](const AgentProposedEdit &candidate) { return candidate.id == proposal.id; }))
                            return fail(error, QStringLiteral("Proposal %1 is ambiguous: it exists in more than one included source file.").arg(proposal.id));
                    }
                    if (!previousIncluded) {
                        previousContent.insert(QStringLiteral("source_file"), path);
                        if (previousContent == snapshot) continue;
                        kind = QStringLiteral("first_seen");
                        additions.append(ProposalHistory::event(kind, snapshot, {}, ProposalHistory::digest(source)));
                        continue;
                    }
                }
                previousContent.insert(QStringLiteral("source_file"), path);
                kind = previousContent == snapshot ? QStringLiteral("moved") : QStringLiteral("changed");
            }
        }
        additions.append(ProposalHistory::event(kind, snapshot, previous.value(QStringLiteral("source_after_sha256")).toString(), ProposalHistory::digest(source)));
    }
    if (!ProposalHistory::append(docsRoot, additions, error)) return false;
    if (latestBatch) for (const auto &value : additions) {
        const auto event = value.toObject();
        const QString id = event.value(QStringLiteral("proposal_id")).toString();
        // The current state keeps only open proposals.
        if (event.value(QStringLiteral("kind")).toString() == QLatin1String("deleted_from_source")) latestBatch->remove(id);
        else latestBatch->insert(id, event);
    }
    return true;
}

namespace AgentProposalMove
{
QString partner(const QString &id)
{
    static const QRegularExpression pair(QStringLiteral("^(agent-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})\\.(cut|paste)$"));
    const auto match = pair.match(id);
    if (!match.hasMatch()) return {};
    return match.captured(1) + QLatin1String(match.captured(2) == QLatin1String("cut") ? PasteSuffix : CutSuffix);
}

bool isHalf(const QString &id)
{
    return !partner(id).isEmpty();
}

bool isCut(const QString &id)
{
    return isHalf(id) && id.endsWith(QLatin1String(CutSuffix));
}

QStringList newPair()
{
    const QString base = QStringLiteral("agent-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    return {base + QLatin1String(CutSuffix), base + QLatin1String(PasteSuffix)};
}
}

namespace
{
// A source of the Library (relative to its root): a file of a connected
// folder docs-<name> or of a Project's default folder
// (.thoughts/projects/<64 hex>/default, or /documents before the rename),
// anywhere in it but in a hidden or generated part. A review card's passage
// lives only there (reviewstore.cpp, editableReviewSource). Everything else
// is a copy: staging copies and snapshots in hidden folders, builds and
// scratch copies in build/, tmp/ and the like, archives outside the folders.
bool isLibraryProposalSource(const QString &relative)
{
    static const QStringList generated{QStringLiteral("__pycache__"), QStringLiteral("build"), QStringLiteral("current_pdfs"),
        QStringLiteral("node_modules"), QStringLiteral("pdf"), QStringLiteral("tmp"), QStringLiteral("venv")};
    const QString owner = SourceOwner::of(relative);
    if (owner.isEmpty() || relative.size() <= owner.size() + 1) return false;
    const QStringList parts = relative.mid(owner.size() + 1).split(QLatin1Char('/'));
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const QString &part = parts.at(index);
        if (part.isEmpty() || part.startsWith(QLatin1Char('.')) || (index + 1 < parts.size() && generated.contains(part))) return false;
    }
    return true;
}

// The folders that hold the Library's sources, as absolute paths below the
// canonical root: each docs-* folder, then each Project's default folder (the
// one on disk; default/ wins when both spellings exist). Symbolic links are
// not followed, as a walk of the whole Library would not follow them.
QStringList librarySourceFolders(const QString &root)
{
    QStringList folders;
    const QDir library(root);
    for (const QString &name : library.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name))
        if (SourceOwner::isConnectedFolder(name)) folders.append(library.filePath(name));
    const QDir projects(DefaultFolderLocation::projectsDirectory(root));
    for (const QString &hash : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name)) {
        if (hash.size() != SourceOwner::HashLength || !SourceOwner::isLowerHex(hash)) continue;
        const QString folder = DefaultFolderLocation::existing(root, hash);
        if (!folder.isEmpty() && !QFileInfo(library.filePath(folder)).isSymLink()) folders.append(library.filePath(folder));
    }
    return folders;
}
}

AgentProposalLocation locateAgentProposal(const QString &docsRoot, const QString &id, const QStringList &skipPaths)
{
    AgentProposalLocation location;
    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    if (root.isEmpty() || !isValidAgentProposalId(id)) return location;
    const QByteArray needle = id.toUtf8();
    // A Project's default folder is inside the hidden .thoughts folder, so it is
    // walked on its own; without QDir::Hidden no walk enters a hidden folder.
    for (const QString &folder : librarySourceFolders(root)) {
        QDirIterator files(folder, {QStringLiteral("*.tex"), QStringLiteral("*.md"), QStringLiteral("*.markdown")}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            const QString canonical = QFileInfo(path).canonicalFilePath();
            if (canonical.isEmpty() || !canonical.startsWith(root + QLatin1Char('/')) || skipPaths.contains(canonical)) continue;
            if (!isLibraryProposalSource(QDir(root).relativeFilePath(path)) || !isLibraryProposalSource(QDir(root).relativeFilePath(canonical))) continue;
            QFile file(canonical);
            if (!file.open(QIODevice::ReadOnly)) continue;
            const QByteArray bytes = file.readAll();
            if (!bytes.contains(needle)) continue;
            const AgentProposalParse parse = parseAgentProposals(canonical, QString::fromUtf8(bytes));
            const AgentProposedEdit *proposal = parse.refusalCode(id).isEmpty() ? parse.proposal(id) : nullptr;
            if (!proposal) continue;
            if (location.found && location.path != canonical) {
                location.ambiguous = true;
                return location;
            }
            location.found = true;
            location.path = canonical;
            location.bytes = bytes;
            location.proposal = *proposal;
        }
    }
    return location;
}

bool resolveLiveAgentProposedEdits(const QString &docsRoot,
                                   const QString &sourcePath,
                                   const QString &expectedSourceSha256,
                                   const QStringList &ids,
                                   const QString &action,
                                   QString *error,
                                   QByteArray *previousContents,
                                   QString *updatedSha256,
                                   QString *errorCode,
                                   const AgentProposalDecisionOptions &options,
                                   QList<AgentProposalSourceChange> *otherSources)
{
    if (errorCode) errorCode->clear();
    const QFileInfo rootInfo(docsRoot);
    const QString canonicalRoot = rootInfo.canonicalFilePath().isEmpty() ? rootInfo.absoluteFilePath() : rootInfo.canonicalFilePath();
    const QFileInfo requestedInfo(sourcePath);
    const QFileInfo sourceInfo(requestedInfo.isAbsolute() ? requestedInfo.absoluteFilePath() : QDir(canonicalRoot).absoluteFilePath(sourcePath));
    const QString canonicalSource = sourceInfo.canonicalFilePath();
    const bool texSource = !sourceInfo.fileName().endsWith(QLatin1String(".md"), Qt::CaseInsensitive)
        && !sourceInfo.fileName().endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive);
    if (canonicalRoot.isEmpty() || !sourceInfo.isFile() || canonicalSource.isEmpty() || !isAgentProposalSourcePath(canonicalSource)
        || (canonicalSource != canonicalRoot && !canonicalSource.startsWith(canonicalRoot + QLatin1Char('/')))) {
        return fail(error, texSource ? QStringLiteral("The local TeX source is missing or outside the documentation folder.") : QStringLiteral("The local proposal source is missing or outside the documentation folder."));
    }

    QFile input(canonicalSource);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(error, texSource ? QStringLiteral("The local TeX source could not be read.") : QStringLiteral("The local proposal source could not be read."));
    }
    const QByteArray sourceBytes = input.readAll();
    // Windows cannot replace a file that is still open.
    input.close();
    const QString sourceSha256 = QString::fromLatin1(QCryptographicHash::hash(sourceBytes, QCryptographicHash::Sha256).toHex());
    if (expectedSourceSha256.size() != 64 || sourceSha256 != expectedSourceSha256) {
        return fail(error, texSource ? QStringLiteral("The TeX source changed before this proposal could be applied.") : QStringLiteral("The source changed before this proposal could be applied."));
    }

    const QString decodedSource = QString::fromUtf8(sourceBytes);
    const AgentProposalParse parsed = parseAgentProposals(canonicalSource, decodedSource);
    // §12: the other half of each move pair is decided with it, here or in another file.
    QStringList here, elsewhere;
    for (const QString &id : ids) {
        const QString partner = AgentProposalMove::partner(id);
        if (!parsed.proposal(id) && !partner.isEmpty() && ids.contains(partner) && parsed.proposal(partner)) {
            if (!elsewhere.contains(id)) elsewhere.append(id);
        } else if (!here.contains(id)) {
            here.append(id);
        }
    }
    for (const QString &id : QStringList(here)) {
        const QString partner = AgentProposalMove::partner(id);
        if (partner.isEmpty() || !parsed.proposal(id) || here.contains(partner) || elsewhere.contains(partner)) continue;
        if (parsed.proposal(partner)) here.append(partner);
        else elsewhere.append(partner);
    }
    QString transformed;
    QString refusal;
    if (!decideAgentProposals(canonicalSource, decodedSource, here, action, &transformed, &refusal, error)) {
        if (errorCode) *errorCode = refusal;
        // An ID that is no longer pending means the source moved on since it was shown.
        if (error && refusal == QLatin1String(AgentProposalIssueCode::UnknownId))
            *error = texSource ? QStringLiteral("The TeX source changed before this proposal could be applied.") : QStringLiteral("The source changed before this proposal could be applied.");
        return false;
    }
    const QByteArray updatedBytes = transformed.toUtf8();
    const QVector<AgentProposedEdit> &proposals = parsed.proposals;
    if (!options.expectedWrapperSha256.isEmpty()) {
        for (const auto &proposal : proposals) {
            if (ids.contains(proposal.id) && ProposalDecisions::wrapperSha256(decodedSource, proposal) != options.expectedWrapperSha256) {
                if (errorCode) *errorCode = QStringLiteral("source_changed");
                return fail(error, QStringLiteral("The proposal changed since it was shown."));
            }
        }
    }
    // The other halves, by file.
    struct Other { QString path; QByteArray before; QByteArray after; QStringList ids; };
    QList<Other> others;
    for (const QString &id : std::as_const(elsewhere)) {
        const AgentProposalLocation location = locateAgentProposal(canonicalRoot, id, {canonicalSource});
        if (!location.found || location.ambiguous) {
            if (errorCode) *errorCode = QLatin1String(location.ambiguous ? AgentProposalMove::Ambiguous : AgentProposalMove::Incomplete);
            return fail(error, location.ambiguous
                ? QStringLiteral("This proposal is half of a move whose other half is pending in more than one source, so Thoughts cannot tell which one to decide with it.")
                : QStringLiteral("This proposal is half of a move (a passage removed in one place and inserted in another) whose other half is no longer pending, so it cannot be decided alone. Ask the agent to withdraw the move, or edit the source by hand."));
        }
        auto known = std::find_if(others.begin(), others.end(), [&](const Other &other) { return other.path == location.path; });
        if (known == others.end()) others.append({location.path, location.bytes, {}, {id}});
        else known->ids.append(id);
    }
    for (auto &other : others) {
        QString otherResult, otherRefusal, otherMessage;
        if (!decideAgentProposals(other.path, QString::fromUtf8(other.before), other.ids, action, &otherResult, &otherRefusal, &otherMessage)) {
            if (errorCode) *errorCode = otherRefusal;
            return fail(error, otherMessage);
        }
        other.after = otherResult.toUtf8();
    }
    if (!observeAgentProposalSource(docsRoot, canonicalSource, sourceBytes, error)) return false;
    for (const auto &other : std::as_const(others))
        if (!observeAgentProposalSource(docsRoot, other.path, other.before, error)) return false;
    const QString claimAction = action == QLatin1String("accept") ? QStringLiteral("accept") : QStringLiteral("reject");
    QJsonArray events;
    QList<QJsonObject> claims;
    const auto record = [&](const QString &path, const QByteArray &before, const QByteArray &after, const QStringList &decided) {
        const QString relative = QDir(QFileInfo(docsRoot).canonicalFilePath()).relativeFilePath(path);
        const QString text = QString::fromUtf8(before);
        // A file Sync never uploads cannot carry a claim (tex/generated/, ignored files).
        const bool claimed = ProposalDecisions::claimApplies(docsRoot, relative);
        for (const auto &proposal : parseAgentProposals(path, text).proposals) {
            if (!decided.contains(proposal.id)) continue;
            QJsonObject event = ProposalHistory::event(action == QLatin1String("accept") ? QStringLiteral("accepted") : QStringLiteral("rejected"),
                proposalSnapshot(proposal, relative), ProposalHistory::digest(before), ProposalHistory::digest(after));
            if (events.isEmpty() && !options.agentRequest.isEmpty()) {
                for (auto it = options.agentRequest.begin(); it != options.agentRequest.end(); ++it) event.insert(it.key(), it.value());
                QJsonObject result = options.agentRequest.value(QStringLiteral("agent_result")).toObject();
                result.insert(QStringLiteral("source_sha256"), ProposalHistory::digest(updatedBytes));
                event.insert(QStringLiteral("agent_result"), result);
            }
            events.append(event);
            if (claimed) claims.append(ProposalDecisions::newClaim(proposal.id, claimAction, ProposalDecisions::wrapperSha256(text, proposal), relative, options.origin));
        }
    };
    record(canonicalSource, sourceBytes, updatedBytes, here);
    QList<ProposalHistory::SourceWrite> writes{{canonicalSource, sourceBytes, updatedBytes}};
    for (const auto &other : std::as_const(others)) {
        record(other.path, other.before, other.after, other.ids);
        writes.append({other.path, other.before, other.after});
    }
    const QString claimDatabase = options.claimDatabase.isEmpty() ? ProposalDecisions::databasePath(docsRoot) : options.claimDatabase;
    if (!ProposalHistory::writeSources(docsRoot, writes, events, error, claims, claimDatabase)) return false;
    if (previousContents) {
        *previousContents = sourceBytes;
    }
    if (updatedSha256) {
        *updatedSha256 = QString::fromLatin1(QCryptographicHash::hash(updatedBytes, QCryptographicHash::Sha256).toHex());
    }
    if (otherSources) {
        otherSources->clear();
        for (const auto &other : std::as_const(others)) otherSources->append({other.path, other.before, ProposalHistory::digest(other.after)});
    }
    return true;
}

bool restoreLiveAgentProposalSource(const QString &docsRoot,
                                    const QString &sourcePath,
                                    const QString &expectedSourceSha256,
                                    const QByteArray &contents,
                                    QString *error)
{
    const QFileInfo rootInfo(docsRoot);
    const QString canonicalRoot = rootInfo.canonicalFilePath().isEmpty() ? rootInfo.absoluteFilePath() : rootInfo.canonicalFilePath();
    const QFileInfo requestedInfo(sourcePath);
    const QFileInfo sourceInfo(requestedInfo.isAbsolute() ? requestedInfo.absoluteFilePath() : QDir(canonicalRoot).absoluteFilePath(sourcePath));
    const QString canonicalSource = sourceInfo.canonicalFilePath();
    const bool texSource = !sourceInfo.fileName().endsWith(QLatin1String(".md"), Qt::CaseInsensitive)
        && !sourceInfo.fileName().endsWith(QLatin1String(".markdown"), Qt::CaseInsensitive);
    if (canonicalRoot.isEmpty() || !sourceInfo.isFile() || canonicalSource.isEmpty() || !isAgentProposalSourcePath(canonicalSource)
        || (canonicalSource != canonicalRoot && !canonicalSource.startsWith(canonicalRoot + QLatin1Char('/')))) {
        return fail(error, texSource ? QStringLiteral("The local TeX source is missing or outside the documentation folder.") : QStringLiteral("The local proposal source is missing or outside the documentation folder."));
    }

    QFile input(canonicalSource);
    if (!input.open(QIODevice::ReadOnly)) {
        return fail(error, texSource ? QStringLiteral("The local TeX source could not be read.") : QStringLiteral("The local proposal source could not be read."));
    }
    const QByteArray currentContents = input.readAll();
    input.close();
    const QString currentSha256 = QString::fromLatin1(QCryptographicHash::hash(currentContents, QCryptographicHash::Sha256).toHex());
    if (expectedSourceSha256.size() != 64 || currentSha256 != expectedSourceSha256) {
        return fail(error, texSource ? QStringLiteral("Undo is no longer safe because the TeX source changed.") : QStringLiteral("Undo is no longer safe because the source changed."));
    }

    const QVector<AgentProposedEdit> proposals = parseAgentProposals(canonicalSource, QString::fromUtf8(contents)).proposals;
    const QSet<QString> currentIds = parseAgentProposals(canonicalSource, QString::fromUtf8(currentContents)).ids();
    QJsonArray events;
    for (const auto &proposal : proposals) {
        if (!currentIds.contains(proposal.id)) events.append(ProposalHistory::event(QStringLiteral("restored"),
            proposalSnapshot(proposal, QDir(QFileInfo(docsRoot).canonicalFilePath()).relativeFilePath(canonicalSource)), currentSha256, ProposalHistory::digest(contents)));
    }
    return ProposalHistory::writeSource(docsRoot, canonicalSource, currentContents, contents, events, error);
}

bool restoreLiveAgentProposalSources(const QString &docsRoot,
                                     const QString &sourcePath,
                                     const QString &expectedSourceSha256,
                                     const QByteArray &contents,
                                     const QList<AgentProposalSourceChange> &others,
                                     QString *error)
{
    if (others.isEmpty()) return restoreLiveAgentProposalSource(docsRoot, sourcePath, expectedSourceSha256, contents, error);
    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    QList<AgentProposalSourceChange> all{{QFileInfo(QDir(root).absoluteFilePath(sourcePath)).canonicalFilePath(), contents, expectedSourceSha256}};
    all.append(others);
    QList<ProposalHistory::SourceWrite> writes;
    QJsonArray events;
    for (const auto &change : std::as_const(all)) {
        QFile input(change.path);
        if (root.isEmpty() || change.path.isEmpty() || !change.path.startsWith(root + QLatin1Char('/')) || !input.open(QIODevice::ReadOnly))
            return fail(error, QStringLiteral("Undo is no longer safe because a source of this move is missing."));
        const QByteArray current = input.readAll();
        input.close();
        if (ProposalHistory::digest(current) != change.afterSha256)
            return fail(error, QStringLiteral("Undo is no longer safe because %1 changed.").arg(QFileInfo(change.path).fileName()));
        const QSet<QString> currentIds = parseAgentProposals(change.path, QString::fromUtf8(current)).ids();
        for (const auto &proposal : parseAgentProposals(change.path, QString::fromUtf8(change.before)).proposals)
            if (!currentIds.contains(proposal.id)) events.append(ProposalHistory::event(QStringLiteral("restored"),
                proposalSnapshot(proposal, QDir(root).relativeFilePath(change.path)), change.afterSha256, ProposalHistory::digest(change.before)));
        writes.append({change.path, current, change.before});
    }
    return ProposalHistory::writeSources(docsRoot, writes, events, error);
}

QSet<QString> locallyResolvedAgentProposalIds(const QString &sourceBundleRoot)
{
    const QString stateRoot = QDir::cleanPath(QDir(sourceBundleRoot).absoluteFilePath(QStringLiteral("..")));
    QJsonObject current;
    QJsonObject resolved;
    if (CurrentAnnotationIndex::read(stateRoot, &current)) {
        resolved = current.value(QStringLiteral("resolved_agent_proposals")).toObject();
    } else {
        QFile stateFile(QDir(stateRoot).filePath(QStringLiteral("repository.json")));
        if (!stateFile.open(QIODevice::ReadOnly)) return {};
        resolved = QJsonDocument::fromJson(stateFile.readAll()).object()
                       .value(QStringLiteral("resolved_agent_proposals")).toObject();
    }
    QSet<QString> ids;
    for (auto iterator = resolved.begin(); iterator != resolved.end(); ++iterator) {
        if (!iterator.key().isEmpty() && iterator.value().isObject()) {
            ids.insert(iterator.key());
        }
    }
    return ids;
}

}
