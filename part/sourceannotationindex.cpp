/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sourceannotationindex.h"
#include "commenttexvisibility.h"
#include "../shell/agentproposalparser.h"
#include <QCryptographicHash>
#include <QRegularExpression>

namespace Thoughts {
namespace {
QString key(const QString &type, const QString &id) { return type + QLatin1Char('\n') + id; }
}

QString commentAnnotationType(const QString &subtype)
{
    if (subtype == QLatin1String("StrikeOut")) return QStringLiteral("strikethrough");
    if (subtype == QLatin1String("Highlight") || subtype == QLatin1String("Underline") || subtype == QLatin1String("Squiggly")) return QStringLiteral("highlight");
    return QStringLiteral("comment");
}

QJsonObject scanAnnotationLocations(const QString &path, const QByteArray &source, const QJsonArray &comments)
{
    const QString text = QString::fromUtf8(source);
    bool valid = text.toUtf8() == source;
    QJsonObject entries;
    const auto add = [&](const QString &type, const QString &id, int first, int last) {
        auto matches = entries.value(key(type, id)).toArray();
        matches.append(QJsonObject{{QStringLiteral("start_line"), first}, {QStringLiteral("end_line"), last}});
        entries.insert(key(type, id), matches);
    };
    for (const auto &value : (path.endsWith(QLatin1String(".md")) || path.endsWith(QLatin1String(".markdown"))) ? QJsonArray() : comments) {
        const auto c = value.toObject();
        add(c.value(QStringLiteral("type")).toString(), c.value(QStringLiteral("id")).toString(),
            c.value(QStringLiteral("start_line")).toInt(), c.value(QStringLiteral("end_line")).toInt());
    }
    const bool markdown = path.endsWith(QLatin1String(".md")) || path.endsWith(QLatin1String(".markdown"));
    QString visible = markdown ? text : Okular::visibleCommentSource(text);
    if (markdown) {
        // Mask literal examples without changing offsets or physical line numbers.
        QChar fence; int fenceSize = 0, offset = 0;
        for (const auto &line : text.split(QLatin1Char('\n'))) {
            const auto trimmed = line.trimmed();
            bool hidden = !fence.isNull() || line.startsWith(QLatin1String("    ")) || line.startsWith(QLatin1Char('\t')) || line.contains(QLatin1Char('`'));
            if (trimmed.startsWith(QLatin1String("```")) || trimmed.startsWith(QLatin1String("~~~"))) {
                int n = 0; while (n < trimmed.size() && trimmed[n] == trimmed[0]) ++n;
                hidden = true;
                if (fence.isNull()) { fence = trimmed[0]; fenceSize = n; }
                else if (fence == trimmed[0] && n >= fenceSize && trimmed.mid(n).isEmpty()) fence = QChar();
            }
            if (hidden) visible.replace(offset, line.size(), QString(line.size(), QLatin1Char(' ')));
            offset += line.size() + 1;
        }
    }
    if (markdown) {
        QSet<QString> known;
        for (const auto &value : comments) known.insert(value.toObject().value(QStringLiteral("id")).toString());
        const QRegularExpression identities(QStringLiteral(R"(thoughts:v1:(?:start|end|point)\s+comment:([A-Za-z0-9_-]+):)"));
        auto ids = identities.globalMatch(visible);
        while (ids.hasNext()) if (!known.contains(ids.next().captured(1))) valid = false;
        if (visible.count(QStringLiteral("thoughts:v1:meta")) != comments.size()) valid = false;
        for (const auto &value : comments) {
            const auto c = value.toObject(); const auto id = c.value(QStringLiteral("id")).toString();
            const QRegularExpression token(QStringLiteral("<!--\\s*thoughts:v1:(start|end|point)\\s+comment:%1:[A-Za-z0-9_-]+\\s*-->").arg(QRegularExpression::escape(id)));
            auto tokens = token.globalMatch(visible); int first = 0; bool point = false; int count = 0;
            while (tokens.hasNext()) {
                const auto t = tokens.next(); const int line = text.left(t.capturedStart()).count(QLatin1Char('\n')) + 1;
                if (t.captured(1) == QLatin1String("point")) { add(QStringLiteral("comment"), id, line, line); point = true; ++count; }
                else if (t.captured(1) == QLatin1String("start")) { if (first) valid = false; first = line; }
                else if (!first || line < first) valid = false;
                else { add(QStringLiteral("comment"), id, first, line); first = 0; ++count; }
            }
            if (first || count == 0 || (point && count > 1)) valid = false;
        }
    }
    const QRegularExpression marker(markdown
        ? QStringLiteral(R"(<!--\s*thoughts:v1:(start|end)\s+review:([A-Za-z0-9_-]+):answer:([0-9]+)\s*-->)")
        : QStringLiteral(R"(\\ReviewCardMask(Start|End)\s*\{([^}]+)\}\s*\{([0-9]+)\})"));
    QHash<QString, int> starts, bodyStarts;
    QJsonObject reviews;
    QSet<QString> seen;
    auto matches = marker.globalMatch(visible);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const QString id = match.captured(2), segment = id + QLatin1Char(':') + match.captured(3);
        const int line = text.left(match.capturedStart()).count(QLatin1Char('\n')) + 1;
        if (match.captured(1).compare(QLatin1String("start"), Qt::CaseInsensitive) == 0) {
            if (starts.contains(segment) || seen.contains(segment)) valid = false;
            starts.insert(segment, line);
            int bodyStart = match.capturedEnd();
            if (markdown) {
                const int first = match.capturedStart() > 0 ? text.lastIndexOf(QLatin1Char('\n'), match.capturedStart() - 1) + 1 : 0;
                const int end = text.indexOf(QLatin1Char('\n'), bodyStart);
                if (end >= 0 && text.mid(first, match.capturedStart() - first).trimmed().isEmpty()
                    && text.mid(bodyStart, end - bodyStart).trimmed().isEmpty()) bodyStart = end + 1;
            }
            bodyStarts.insert(segment, bodyStart);
        } else {
            if (!starts.contains(segment)) { valid = false; continue; }
            add(QStringLiteral("review_card"), id, starts.take(segment), line);
            auto evidence = reviews.value(id).toArray();
            const int bodyStart = bodyStarts.take(segment);
            const QByteArray body = text.mid(bodyStart, match.capturedStart() - bodyStart).toUtf8();
            evidence.append(QJsonObject{{QStringLiteral("segment_id"), match.captured(3)},
                {QStringLiteral("content_sha256"), QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex())}});
            reviews.insert(id, evidence);
            seen.insert(segment);
        }
    }
    if (!starts.isEmpty()) valid = false;
    if (!markdown && visible.count(QStringLiteral("\\ReviewCardMaskStart")) + visible.count(QStringLiteral("\\ReviewCardMaskEnd")) != seen.size() * 2) valid = false;
    if (!markdown) {
        // The complete wrapper is indexed, including both alternatives. Other broken
        // wrappers are issues of their own and do not hide the others, but after an
        // unclosed group or a missing argument TeX reads the rest differently.
        const auto parsed = parseLatexAgentProposals(text);
        for (const auto &issue : parsed.issues)
            if (issue.code == QLatin1String(AgentProposalIssueCode::UnclosedGroup) || issue.code == QLatin1String(AgentProposalIssueCode::MissingArgument)) valid = false;
        for (const auto &p : parsed.proposals)
            add(QStringLiteral("proposal"), p.id, p.line, p.line + int(QStringView(text).mid(p.offset, p.length - 1).count(QLatin1Char('\n'))));
    }
    QJsonArray mentioned;
    static const QRegularExpression identity(QStringLiteral(R"([0-9a-fA-F]{8}-(?:[0-9a-fA-F]{4}-){3}[0-9a-fA-F]{12}|agent-[a-zA-Z0-9._:-]+)"));
    auto identities = identity.globalMatch(text);
    while (identities.hasNext()) { const auto id = identities.next().captured(); if (!mentioned.contains(id)) mentioned.append(id); }
    return {{QStringLiteral("mentioned"), mentioned}, {QStringLiteral("sha256"), QString::fromLatin1(QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex())},
        {QStringLiteral("locations"), entries}, {QStringLiteral("reviews"), reviews}, {QStringLiteral("valid"), valid}};
}

QJsonObject annotationSourceLocation(const QJsonObject &index, const QString &type, const QString &id, const QString &project, const QString &sourceFile)
{
    const auto unavailable = QJsonObject{{QStringLiteral("state"), QStringLiteral("unavailable")}};
    if (project.isEmpty() || !index.value(QStringLiteral("projects")).toObject().value(project).toBool()) return unavailable;
    QJsonArray found;
    bool mentioned = false;
    const auto files = index.value(QStringLiteral("files")).toObject();
    const bool hasPreferred = files.value(sourceFile).toObject().value(QStringLiteral("locations")).toObject().contains(key(type, id));
    for (auto it = files.begin(); it != files.end(); ++it) {
        if (hasPreferred && it.key() != sourceFile) continue;
        const auto file = it.value().toObject();
        if (file.value(QStringLiteral("project")).toString() != project) continue;
        if (!file.value(QStringLiteral("valid")).toBool()) return unavailable;
        mentioned = mentioned || file.value(QStringLiteral("mentioned")).toArray().contains(id);
        for (const auto &value : file.value(QStringLiteral("locations")).toObject().value(key(type, id)).toArray()) {
            auto location = value.toObject();
            location.insert(QStringLiteral("source_file"), it.key());
            location.insert(QStringLiteral("source_sha256"), file.value(QStringLiteral("sha256")));
            found.append(location);
        }
    }
    if (found.isEmpty() && mentioned) return unavailable;
    if (found.isEmpty()) return {{QStringLiteral("state"), QStringLiteral("missing")}};
    // A valid file never repeats a card segment, so one card's several ranges in a single file
    // are its passages. The same card in two files remains ambiguous.
    bool oneFile = true;
    for (const auto &value : found)
        oneFile = oneFile && value.toObject().value(QStringLiteral("source_file")) == found.first().toObject().value(QStringLiteral("source_file"));
    if (found.size() != 1 && !(type == QLatin1String("review_card") && oneFile)) return {{QStringLiteral("state"), QStringLiteral("ambiguous")}};
    QJsonObject result = found.first().toObject();
    for (const auto &value : found)
        if (value.toObject().value(QStringLiteral("start_line")).toInt() < result.value(QStringLiteral("start_line")).toInt()) result = value.toObject();
    result.insert(QStringLiteral("state"), QStringLiteral("linked"));
    if (found.size() > 1) result.insert(QStringLiteral("ranges"), found); // file order, like the card's evidence
    return result;
}

QJsonObject annotationSourceLocation(const QString &root, const QString &type, const QString &id, const QString &project, const QString &sourceFile)
{ return annotationSourceLocation(sourceLocationIndex(root), type, id, project, sourceFile); }

QJsonObject commentSourceLocation(const QJsonObject &index, const QString &subtype, const QString &id, const QString &project, const QString &sourceFile)
{
    const QString preferred = commentAnnotationType(subtype);
    const auto location = annotationSourceLocation(index, preferred, id, project, sourceFile);
    if (location.value(QStringLiteral("state")).toString() == QLatin1String("linked")) return location;
    for (const auto &type : {QStringLiteral("comment"), QStringLiteral("highlight"), QStringLiteral("strikethrough")}) {
        if (type == preferred) continue;
        const auto other = annotationSourceLocation(index, type, id, project, sourceFile);
        if (other.value(QStringLiteral("state")).toString() == QLatin1String("linked")) return other;
    }
    return location;
}

}
