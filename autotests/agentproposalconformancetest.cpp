/* SPDX-License-Identifier: GPL-2.0-or-later */
// Every case of the agent-proposal specification's fixtures, strictly: proposals
// with all fields and spans, issues with code, span and ID, decision outputs and
// refusal codes, and the writer cases. Spans in the fixtures are code points.
#undef QT_NO_CAST_FROM_ASCII
#undef QT_NO_CAST_TO_ASCII
#include "../shell/agentproposalparser.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTest>
#include <QTimeZone>

using namespace Thoughts;

namespace
{
QJsonObject corpus()
{
    static const QJsonObject loaded = [] {
        QFile file(QStringLiteral(KDESRCDIR "data/agent-proposals/cases.json"));
        if (!file.open(QIODevice::ReadOnly)) return QJsonObject();
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return loaded;
}

int codePoints(const QString &source, int offset)
{
    return int(QStringView(source).left(offset).toUcs4().size());
}

QJsonObject span(const QString &source, int offset, int length)
{
    return {{QStringLiteral("start"), codePoints(source, offset)}, {QStringLiteral("end"), codePoints(source, offset + length)}};
}

QString compact(const QJsonValue &value)
{
    return QString::fromUtf8(QJsonDocument(value.isArray() ? QJsonDocument(value.toArray()) : QJsonDocument(value.toObject())).toJson(QJsonDocument::Compact));
}
}

class AgentProposalConformanceTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void initTestCase()
    {
        QVERIFY2(!corpus().isEmpty(), "Fixture file missing or invalid");
        QCOMPARE(corpus().value(QStringLiteral("cases")).toArray().size(), 171);
    }

    void cases_data()
    {
        QTest::addColumn<QJsonObject>("item");
        for (const QJsonValue &value : corpus().value(QStringLiteral("cases")).toArray()) {
            const QJsonObject item = value.toObject();
            QTest::newRow(qPrintable(item.value(QStringLiteral("id")).toString())) << item;
        }
    }

    void cases()
    {
        QFETCH(QJsonObject, item);
        const QString path = item.value(QStringLiteral("path")).toString();
        const QString source = item.value(QStringLiteral("source")).toString();
        const QJsonObject expect = item.value(QStringLiteral("expect")).toObject();
        const AgentProposalParse parse = parseAgentProposals(path, source);

        QJsonArray proposals;
        for (const AgentProposedEdit &p : parse.proposals) {
            QJsonObject proposal {{QStringLiteral("id"), p.id},
                                  {QStringLiteral("created_at"), p.createdAt},
                                  {QStringLiteral("conversation_id"), p.conversationId},
                                  {QStringLiteral("explanation"), p.explanation},
                                  {QStringLiteral("original"), p.original},
                                  {QStringLiteral("replacement"), p.replacement},
                                  {QStringLiteral("span"), span(source, p.offset, p.length)}};
            if (!p.conversationUrl.isEmpty()) proposal.insert(QStringLiteral("conversation_url"), p.conversationUrl);
            proposals.append(proposal);
        }
        QCOMPARE(compact(proposals), compact(expect.value(QStringLiteral("proposals"))));

        QJsonArray issues;
        for (const AgentProposalIssue &issue : parse.issues) {
            QJsonObject entry {{QStringLiteral("code"), issue.code}, {QStringLiteral("span"), span(source, issue.offset, issue.length)}};
            if (issue.hasId) entry.insert(QStringLiteral("id"), issue.id);
            issues.append(entry);
        }
        QCOMPARE(compact(issues), compact(expect.value(QStringLiteral("issues"))));

        for (const QJsonValue &value : item.value(QStringLiteral("decisions")).toArray()) {
            const QJsonObject decision = value.toObject();
            QStringList ids;
            for (const QJsonValue &id : decision.value(QStringLiteral("ids")).toArray()) ids.append(id.toString());
            const QString action = decision.value(QStringLiteral("action")).toString();
            const QJsonObject wanted = decision.value(QStringLiteral("expect")).toObject();
            QString output, code;
            const bool decided = decideAgentProposals(path, source, ids, action, &output, &code);
            const QString label = action + QLatin1Char(' ') + ids.join(QLatin1Char(','));
            if (wanted.contains(QStringLiteral("output"))) {
                QVERIFY2(decided, qPrintable(label + QStringLiteral(" refused: ") + code));
                QCOMPARE(output, wanted.value(QStringLiteral("output")).toString());
            } else {
                QVERIFY2(!decided, qPrintable(label + QStringLiteral(" was not refused")));
                QCOMPARE(code, wanted.value(QStringLiteral("refused")).toString());
            }
        }
    }

    void writers_data()
    {
        QTest::addColumn<QJsonObject>("item");
        for (const QJsonValue &value : corpus().value(QStringLiteral("writers")).toArray()) {
            const QJsonObject item = value.toObject();
            QTest::newRow(qPrintable(item.value(QStringLiteral("id")).toString())) << item;
        }
    }

    void writers()
    {
        QFETCH(QJsonObject, item);
        const QJsonObject fields = item.value(QStringLiteral("fields")).toObject();
        AgentProposedEdit p;
        p.id = fields.value(QStringLiteral("id")).toString();
        p.createdAt = fields.value(QStringLiteral("created_at")).toString();
        p.conversationId = fields.value(QStringLiteral("conversation_id")).toString();
        p.conversationUrl = fields.value(QStringLiteral("conversation_url")).toString();
        p.explanation = fields.value(QStringLiteral("explanation")).toString();
        p.original = fields.value(QStringLiteral("original")).toString();
        p.replacement = fields.value(QStringLiteral("replacement")).toString();
        const bool tex = item.value(QStringLiteral("format")).toString() == QLatin1String("tex");
        QString refusal;
        const QString written = tex ? formatLatexAgentProposal(p, &refusal) : formatMarkdownAgentProposal(p, &refusal);
        QVERIFY2(refusal.isEmpty(), qPrintable(refusal));
        QCOMPARE(written, item.value(QStringLiteral("expect")).toString());

        // What a writer produces reads back as the same proposal.
        const AgentProposalParse parse = parseAgentProposals(tex ? QStringLiteral("doc.tex") : QStringLiteral("doc.md"), written);
        QVERIFY(parse.issues.isEmpty());
        QCOMPARE(parse.proposals.size(), 1);
        const AgentProposedEdit &read = parse.proposals.first();
        QCOMPARE(read.id, p.id);
        QCOMPARE(read.createdAt, p.createdAt);
        QCOMPARE(read.conversationId, p.conversationId);
        QCOMPARE(read.conversationUrl, p.conversationUrl);
        QCOMPARE(read.explanation, tex ? escapeLatexProposalExplanation(p.explanation) + QLatin1Char('\n') : p.explanation);
        QCOMPARE(read.original, p.original);
        QCOMPARE(read.replacement, !tex && !p.replacement.isEmpty() && !p.replacement.endsWith(QLatin1Char('\n')) ? p.replacement + QLatin1Char('\n') : p.replacement);
    }

    void writerRefusals_data()
    {
        QTest::addColumn<bool>("tex");
        QTest::addColumn<QString>("field");
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("refusal");
        QTest::newRow("tex-unbalanced-close") << true << "replacement" << "Bad } text.\n" << "unbalanced_braces";
        QTest::newRow("tex-unbalanced-open") << true << "original" << "\\begin{x}{\n" << "unbalanced_braces";
        QTest::newRow("tex-final-backslash") << true << "replacement" << "Ends with \\" << "unbalanced_braces";
        QTest::newRow("tex-escaped-braces-ok") << true << "replacement" << "\\{ and \\} and 50\\%\n" << "";
        QTest::newRow("tex-trailing-comment") << true << "replacement" << "Text % comment" << "unterminated_comment";
        QTest::newRow("tex-comment-with-newline") << true << "replacement" << "Text % comment }\n" << "";
        QTest::newRow("tex-nested") << true << "replacement" << "\\agentProposedEdit{p}{Why.}{Old.}{New.}" << "nested_proposal";
        QTest::newRow("tex-commented-wrapper-ok") << true << "replacement" << "% \\agentProposedEdit{p}{Why.}{Old.}{New.}\n" << "";
        QTest::newRow("tex-agent-remove-is-text") << true << "replacement" << "\\agentRemove{a}{b}{c}\n" << "";
        QTest::newRow("tex-offset-timestamp") << true << "created_at" << "2026-10-10T12:00:00+02:00" << "invalid_metadata";
        QTest::newRow("tex-blank-explanation") << true << "explanation" << " \n" << "invalid_metadata";
        QTest::newRow("tex-invalid-id") << true << "id" << "Agent-1" << "invalid_id";
        QTest::newRow("md-nested") << false << "replacement" << "a\n<!-- thoughts:agent-proposal:v2:end x -->\n" << "nested_proposal";
        QTest::newRow("md-inline-marker-ok") << false << "replacement" << "See <!-- thoughts:agent-proposal:v2:start\n" << "";
        QTest::newRow("md-tex-wrapper-is-text") << false << "replacement" << "\\agentProposedEdit{p}{w}{a}{b}\n" << "";
        QTest::newRow("md-uppercase-conversation") << false << "conversation_id" << "0B6C5B7E-1D2A-4C3B-9F8E-112233445566" << "invalid_metadata";
        QTest::newRow("tex-claude-link-ok") << true << "conversation_url" << "https://claude.ai/code/session_01ExampleSessionAbcdEfgh" << "";
        QTest::newRow("md-codex-link-ok") << false << "conversation_url" << "codex://threads/0b6c5b7e-1d2a-4c3b-9f8e-112233445566" << "";
        QTest::newRow("tex-foreign-link") << true << "conversation_url" << "https://example.com/x" << "invalid_metadata";
        QTest::newRow("md-link-injection") << false << "conversation_url" << "https://claude.ai/code/session_01ab\nexplanation:" << "invalid_metadata";
    }

    void writerRefusals()
    {
        QFETCH(bool, tex);
        QFETCH(QString, field);
        QFETCH(QString, value);
        QFETCH(QString, refusal);
        AgentProposedEdit p;
        p.id = QStringLiteral("agent-1");
        p.createdAt = QStringLiteral("2026-10-10T12:00:00.000Z");
        p.conversationId = QStringLiteral("0b6c5b7e-1d2a-4c3b-9f8e-112233445566");
        p.explanation = QStringLiteral("Why.");
        p.original = QStringLiteral("Old.\n");
        p.replacement = QStringLiteral("New.\n");
        if (field == QLatin1String("replacement")) p.replacement = value;
        else if (field == QLatin1String("original")) p.original = value;
        else if (field == QLatin1String("created_at")) p.createdAt = value;
        else if (field == QLatin1String("explanation")) p.explanation = value;
        else if (field == QLatin1String("id")) p.id = value;
        else if (field == QLatin1String("conversation_id")) p.conversationId = value;
        else if (field == QLatin1String("conversation_url")) p.conversationUrl = value;
        QString code = QStringLiteral("unset");
        const QString written = tex ? formatLatexAgentProposal(p, &code) : formatMarkdownAgentProposal(p, &code);
        QCOMPARE(code, refusal);
        QCOMPARE(written.isEmpty(), !refusal.isEmpty());
        if (refusal.isEmpty()) {
            const auto parse = parseAgentProposals(tex ? QStringLiteral("a.tex") : QStringLiteral("a.md"), written);
            QCOMPARE(parse.proposals.size(), 1);
            QVERIFY(parse.issues.isEmpty());
        }
    }

    void timestamps_data()
    {
        QTest::addColumn<QString>("value");
        QTest::addColumn<bool>("valid");
        QTest::newRow("milliseconds") << "2026-10-10T12:00:00.000Z" << true;
        QTest::newRow("no-fraction") << "2026-10-10T12:00:00Z" << true;
        QTest::newRow("nine-digits") << "2026-10-10T12:00:00.123456789Z" << true;
        QTest::newRow("ten-digits") << "2026-10-10T12:00:00.1234567890Z" << false;
        QTest::newRow("empty-fraction") << "2026-10-10T12:00:00.Z" << false;
        QTest::newRow("offset") << "2026-09-12T12:13:36+02:00" << false;
        QTest::newRow("lowercase-z") << "2026-10-10T12:00:00z" << false;
        QTest::newRow("lowercase-t") << "2026-10-10t12:00:00Z" << false;
        QTest::newRow("date-only") << "2026-10-10" << false;
        QTest::newRow("no-zone") << "2026-10-10T12:00:00" << false;
        QTest::newRow("leap-day") << "2024-02-29T00:00:00Z" << true;
        QTest::newRow("not-leap-year") << "2100-02-29T00:00:00Z" << false;
        QTest::newRow("leap-second") << "2026-06-30T23:59:60Z" << false;
        QTest::newRow("hour-24") << "2026-10-10T24:00:00Z" << false;
        QTest::newRow("year-zero") << "0000-01-01T00:00:00Z" << false;
        QTest::newRow("non-ascii-digit") << QString::fromUtf8("2026-10-1\xd9\xa1T12:00:00Z") << false;
    }

    void timestamps()
    {
        QFETCH(QString, value);
        QFETCH(bool, valid);
        QCOMPARE(isValidAgentProposalTimestamp(value), valid);
        QCOMPARE(agentProposalTimestampDateTime(value).isValid(), valid);
    }

    void writerTimestamp()
    {
        const QDateTime moment(QDate(2026, 10, 10), QTime(14, 5, 6, 7), QTimeZone::fromSecondsAheadOfUtc(2 * 3600));
        QCOMPARE(formatAgentProposalTimestamp(moment), QStringLiteral("2026-10-10T12:05:06.007Z"));
        QCOMPARE(agentProposalTimestampDateTime(QStringLiteral("2026-10-10T12:05:06.0071234Z")), moment);
    }
};

QTEST_GUILESS_MAIN(AgentProposalConformanceTest)
#include "agentproposalconformancetest.moc"
