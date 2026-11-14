/* SPDX-License-Identifier: GPL-2.0-or-later */
#undef QT_NO_CAST_FROM_ASCII
#undef QT_NO_CAST_TO_ASCII
#include "../shell/proposalauthoring.h"
#include "../shell/thoughtscommandprotocol.h"
#include "../shell/agentproposalparser.h"
#include "../shell/reviewdocumenttools.h"
#include "../part/proposalhistory.h"
#include "../part/proposalprovenance.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QTest>
#include <numeric>

class ProposalAuthoringTest : public QObject {
    Q_OBJECT
    static void put(const QString &path, const QByteArray &bytes) { QFile f(path); QVERIFY(f.open(QIODevice::WriteOnly)); QCOMPARE(f.write(bytes), bytes.size()); }
    static QByteArray get(const QString &path) { QFile f(path); if (!f.open(QIODevice::ReadOnly)) return {}; return f.readAll(); }
    static QString decide(const QString &path, const QString &source, const QStringList &ids, const QString &action) {
        QString result, code;
        if (!Thoughts::decideAgentProposals(path, source, ids, action, &result, &code)) return "refused:" + code;
        return result;
    }
    static QJsonObject request(const QString &path, bool create = true) {
        return {{"command", create ? "create-proposal" : "revise-proposal"}, {"source", path}, {"expected_source_sha256", Thoughts::ProposalHistory::digest(get(path))},
                {"replacement", "New text.\n"}, {"explanation", "Fix the description."}, {"conversation_id", "0190a000-0000-7000-8000-00000000000a"},
                {"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    }

    // A one-page PDF whose text annotations carry the given contents (doc-anchor:<name>).
    static QByteArray pdfWithAnnotations(const QStringList &contents) {
        QList<QByteArray> objects{"<< /Type /Catalog /Pages 2 0 R >>", "<< /Type /Pages /Kids [3 0 R] /Count 1 >>", QByteArray()};
        QByteArray annots;
        for (int i = 0; i < contents.size(); ++i) {
            const int number = 4 + i;
            annots += QByteArray::number(number) + " 0 R ";
            objects.append("<< /Type /Annot /Subtype /Text /Rect [100 500 120 520] /Contents (" + contents[i].toUtf8() + ") >>");
        }
        objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 600 800] /Annots [" + annots + "] >>";
        QByteArray pdf = "%PDF-1.4\n";
        QList<qsizetype> offsets;
        for (int i = 0; i < objects.size(); ++i) {
            offsets.append(pdf.size());
            pdf += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const qsizetype xref = pdf.size();
        pdf += "xref\n0 " + QByteArray::number(objects.size() + 1) + "\n0000000000 65535 f \n";
        for (const qsizetype offset : offsets) pdf += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        pdf += "trailer\n<< /Size " + QByteArray::number(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        return pdf;
    }
    static QString projectHash(const QString &id) { return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex()); }
    // A Project configuration as the publisher reads it: a default folder for
    // imported PDFs, one docs-* folder with LaTeX/Markdown documents.
    static void writeProject(const QString &root, const QString &id, const QString &name, const QString &folder, const QStringList &sources,
                             const QMap<QString, QByteArray> &pdfs, const QStringList &unselectedPdfs = {}, const QStringList &leadingFolders = {}) {
        int counter = 0;
        const auto uuid = [&] { return QStringLiteral("%1-0000-4000-8000-%2").arg(projectHash(id).left(8)).arg(++counter, 12, 10, QLatin1Char('0')); };
        const QString defaultFolder = ".thoughts/projects/" + projectHash(id) + "/documents";
        const QString defaultId = uuid(), folderId = uuid();
        QJsonArray documents;
        QJsonObject rendering;
        for (const QString &source : sources)
            documents.append(QJsonObject{{"document_id", uuid()}, {"source", QJsonObject{{"folder_id", folderId}, {"path", source}}},
                                         {"format", source.endsWith(".md") ? "markdown" : "latex"}, {"included", true}});
        for (auto it = pdfs.cbegin(); it != pdfs.cend(); ++it) {
            const QString documentId = uuid();
            const bool included = !unselectedPdfs.contains(it.key());
            documents.append(QJsonObject{{"document_id", documentId}, {"source", QJsonObject{{"folder_id", defaultId}, {"path", it.key()}}},
                                         {"format", "pdf"}, {"included", included}});
            rendering.insert(documentId, QJsonObject{{"sha256", QString::fromLatin1(QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex())}});
            QVERIFY(QDir(root).mkpath(defaultFolder));
            put(QDir(root).filePath(defaultFolder + "/" + it.key()), it.value());
        }
        QJsonArray folders{QJsonObject{{"folder_id", defaultId}, {"path", defaultFolder}, {"document_id", uuid()}}};
        for (const QString &leading : leadingFolders) folders.append(QJsonObject{{"folder_id", uuid()}, {"path", leading}, {"document_id", uuid()}});
        folders.append(QJsonObject{{"folder_id", folderId}, {"path", folder}, {"document_id", uuid()}});
        QJsonObject value{{"schema_version", 5}, {"kind", "thoughts-project"}, {"id", id}, {"name", name}, {"default_folder_id", defaultId},
                          {"source_folders", folders}, {"documents", documents}};
        if (!rendering.isEmpty()) value.insert("rendering", rendering);
        QVERIFY(QDir(root).mkpath(defaultFolder));
        put(QDir(root).filePath(defaultFolder + "/thoughts-project.json"), QJsonDocument(value).toJson());
    }
    // Two Projects: Concepts (docs-concepts, its own PDF own.pdf with anchor
    // hyp) and Followup (docs-followup, the imported PDF paper_abc.pdf).
    static void writeLinkLibrary(const QString &root) {
        QVERIFY(QDir(root).mkpath("docs-concepts/tex/source"));
        QVERIFY(QDir(root).mkpath("docs-concepts/markdown"));
        QVERIFY(QDir(root).mkpath("docs-followup/tex/source"));
        put(root + "/docs-concepts/tex/source/grpo.tex", "Intro.\nOld claim.\nSee \\PdfLink{gone}{document-start}{the lost paper}.\nEnd.\n");
        put(root + "/docs-concepts/tex/source/other.tex", "\\SourceLabel{known}Other.\n\\label{sec:other}\n");
        put(root + "/docs-concepts/markdown/notes.md", "# Notes\n\nOld line.\n");
        put(root + "/docs-concepts/markdown/target.md", "# Target {#source-label:here}\n");
        put(root + "/docs-followup/tex/source/main.tex", "Main.\n");
        writeProject(root, "test:concepts", "concepts", "docs-concepts",
                     {"tex/source/grpo.tex", "tex/source/other.tex", "markdown/notes.md", "markdown/target.md"},
                     {{"own.pdf", pdfWithAnnotations({"doc-anchor:hyp"})}, {"unselected.pdf", pdfWithAnnotations({})}}, {"unselected.pdf"});
        writeProject(root, "test:followup", "Benchmark-followup", "docs-followup", {"tex/source/main.tex"},
                     {{"paper_abc.pdf", pdfWithAnnotations({"doc-anchor:intro"})}});
    }
    static QJsonObject create(const QString &root, const QString &path, int start, int end, const QString &replacement) {
        auto req = request(path); req.insert("start_line", start); req.insert("end_line", end); req["replacement"] = replacement;
        return Thoughts::authorProposal(root, req);
    }
    static constexpr const char *Card = "6f1c2a0e-3b4d-4c5e-8f60-718293a4b5c6";
    static constexpr const char *Conversation = "0190a000-0000-7000-8000-00000000000a";
    // A source whose `line` (one-based) is marked as the answer of `card`.
    static QByteArray marked(const QString &relative, const QByteArray &text, int line, const QString &card = Card) {
        const auto plan = Thoughts::markReviewSourceSelection(relative, text, line, line + 1, card);
        return plan.error.isEmpty() ? plan.replacement : QByteArray("marking failed: " + plan.error.toUtf8());
    }
    static Thoughts::CardMoveContext movable(const QString &sourceFile) {
        Thoughts::CardMoveContext card;
        card.movePairsSupported = true;
        card.sourceFile = sourceFile;
        card.question = "What is the answer?";
        return card;
    }
    static QJsonObject moveRequest(const QString &destination, int line) {
        return {{"command", "move-card-passage"}, {"card_id", Card}, {"source", destination},
                {"expected_source_sha256", Thoughts::ProposalHistory::digest(get(destination))}, {"line", line},
                {"explanation", "Merge the notes into one."}, {"conversation_id", Conversation},
                {"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    }
private Q_SLOTS:
    // 2026-10-14: the Sync before a proposal gates on sources only. A failed
    // PDF build after the sources synced is a warning; the proposal may fix it.
    void gatesOnSourceSyncNotBuilds() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("docs-x/tex/source"));
        const QString path = root.filePath("docs-x/tex/source/main.tex");
        put(path, "See \\Cref{tab:x}.\nTwo.\n");
        const auto result = [](const QString &status, const QString &code) {
            QJsonObject operation{{"id", QString("op-" + status + code)}, {"status", status}, {"phase", status}};
            if (!code.isEmpty()) operation.insert("last_error_code", code);
            return Thoughts::syncCommandOperationResult(operation);
        };
        const auto proposal = [&] { auto req = request(path); req.insert("start_line", 1); req.insert("end_line", 2); req["replacement"] = "See Table~\\ref{tab:x}.\n"; return req; };

        // A real Sync failure (e.g. a conflict) or no answer writes nothing.
        for (const QJsonObject &before : {result("failed", "sync_failed"), result("failed", QString())}) {
            const QByteArray original = get(path);
            const auto refused = Thoughts::authorProposalAfterSync(root.path(), proposal(), "thoughtsctl", before);
            QCOMPARE(refused["error_code"].toString(), "sync_before_failed");
            QCOMPARE(refused["sync_before"].toObject()["operation_id"].toString(), before["operation_id"].toString());
            QCOMPARE(get(path), original);
        }
        const auto unanswered = Thoughts::authorProposalAfterSync(root.path(), proposal(), "thoughtsctl", {}, "Thoughts did not answer in time.");
        QCOMPARE(unanswered["error_code"].toString(), "sync_before_failed");
        QVERIFY(unanswered["message"].toString().contains("did not answer"));

        // Sources synced, PDF build failed: written, with the build failure as a warning.
        const auto written = Thoughts::authorProposalAfterSync(root.path(), proposal(), "thoughtsctl", result("failed", "pdf_build_failed"));
        QVERIFY2(written["accepted"].toBool(), qPrintable(QJsonDocument(written).toJson()));
        QCOMPARE(written["sync_before"].toString(), "op-failedpdf_build_failed");
        QCOMPARE(written["warnings"].toArray().size(), 1);
        QCOMPARE(written["warnings"].toArray().first().toObject()["code"].toString(), "pdf_build_failed");
        QVERIFY(get(path).contains("\\agentProposedEdit"));

        // The same from a Sync that succeeded with the build failure as its warning
        // (Sync no longer waits for the server's builds).
        put(path, "See \\Cref{tab:x}.\nTwo.\n");
        QJsonObject warned{{"id", "op-warned"}, {"status", "succeeded"}, {"phase", "succeeded"}, {"warning_code", "pdf_build_failed"},
                           {"warning_message", "PDF build failed\nYour changes are synced with the server.\npaper.pdf: latex_error"}};
        const auto writtenAfterWarning = Thoughts::authorProposalAfterSync(root.path(), proposal(), "thoughtsctl", Thoughts::syncCommandOperationResult(warned));
        QVERIFY2(writtenAfterWarning["accepted"].toBool(), qPrintable(QJsonDocument(writtenAfterWarning).toJson()));
        QCOMPARE(writtenAfterWarning["sync_before"].toString(), "op-warned");
        QCOMPARE(writtenAfterWarning["warnings"].toArray().size(), 1);
        const QJsonObject warning = writtenAfterWarning["warnings"].toArray().first().toObject();
        QCOMPARE(warning["code"].toString(), "pdf_build_failed");
        QCOMPARE(warning["sync"].toString(), "before");
        QCOMPARE(warning["operation_id"].toString(), "op-warned");
        QVERIFY(warning["message"].toString().contains("latex_error"));
        QVERIFY(get(path).contains("\\agentProposedEdit"));

        // A succeeded or skipped Sync: no warning.
        put(path, "One.\nTwo.\n");
        const auto clean = Thoughts::authorProposalAfterSync(root.path(), proposal(), "thoughtsctl", result("succeeded", QString()));
        QVERIFY2(clean["accepted"].toBool(), qPrintable(QJsonDocument(clean).toJson()));
        QVERIFY(!clean.contains("warnings"));
        put(path, "One.\nTwo.\n");
        const auto skipped = Thoughts::authorProposalAfterSync(root.path(), proposal(), "thoughtsctl", {});
        QVERIFY2(skipped["accepted"].toBool(), qPrintable(QJsonDocument(skipped).toJson()));
        QCOMPARE(skipped["sync_before"].toString(), "skipped_recent");
    }
    // Generated files take no new or revised proposals; existing ones can still be withdrawn.
    void refusesGeneratedSources() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("docs-x/tex/generated/tables"));
        QVERIFY(QDir(root.path()).mkpath("docs-x/tex/support"));
        const QString generated = root.filePath("docs-x/tex/generated/tables/t.tex");
        const QString headed = root.filePath("docs-x/tex/support/config.tex");
        const QString scripted = root.filePath("docs-x/tex/support/layout.tex");
        const QString markdown = root.filePath("docs-x/notes.md");
        const QString authored = root.filePath("docs-x/tex/support/authored.tex");
        put(generated, "One.\nTwo.\n");
        put(headed, "\n% Generated artifact. Provenance: scripts/make.py\nOne.\nTwo.\n");
        put(scripted, "% Generated by docs/render_all_pdf.sh. Direct builds inherit it.\nOne.\n");
        put(markdown, "<!-- Generated artifact: notes.py -->\nOne.\n");
        put(authored, "One.\n% Generated artifact (a quoted marker, not the header).\n");
        for (const QString &path : {generated, headed, scripted, markdown}) {
            const QByteArray before = get(path);
            auto create = request(path); create.insert("start_line", 2); create.insert("end_line", 3);
            const auto result = Thoughts::authorProposal(root.path(), create);
            QCOMPARE(result["error_code"].toString(), "generated_source");
            QVERIFY(result["message"].toString().contains("change its generator"));
            QCOMPARE(get(path), before);
            auto batch = request(path); batch.remove("replacement"); batch.remove("explanation"); batch["command"] = "create-proposals";
            batch.insert("proposals", QJsonArray{QJsonObject{{"start_line", 1}, {"end_line", 2}, {"replacement", "X.\n"}, {"explanation", "Why."}}});
            QCOMPARE(Thoughts::authorProposal(root.path(), batch)["error_code"].toString(), "generated_source");
        }
        auto ordinary = request(authored); ordinary.insert("start_line", 1); ordinary.insert("end_line", 2);
        QVERIFY2(!Thoughts::authorProposal(root.path(), ordinary).contains("error_code"), "a marker on a later line is not a generated header");
        // An existing proposal in a generated file: revise is refused, remove is allowed.
        const QString wrapper = "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{agent-1}{Why.\n}{One.\n}{Uno.\n}";
        put(generated, (wrapper + "Two.\n").toUtf8());
        auto revise = request(generated, false); revise.insert("proposal_id", "agent-1");
        QCOMPARE(Thoughts::authorProposal(root.path(), revise)["error_code"].toString(), "generated_source");
        auto remove = request(generated, false); remove["command"] = "remove-proposal"; remove.remove("replacement"); remove.insert("proposal_id", "agent-1");
        const auto removed = Thoughts::authorProposal(root.path(), remove);
        QVERIFY2(removed["accepted"].toBool(), qPrintable(QJsonDocument(removed).toJson()));
        QCOMPARE(get(generated), QByteArray("One.\nTwo.\n"));
        QVERIFY(Thoughts::isGeneratedAgentProposalSource("docs-x/tex/generated/a.tex", "One.\n"));
        QVERIFY(Thoughts::isGeneratedAgentProposalSource(".thoughts/projects/x/documents/tex/generated/a.tex", ""));
        QVERIFY(!Thoughts::isGeneratedAgentProposalSource("docs-x/tex/generatedish/a.tex", "One.\n"));
        QVERIFY(!Thoughts::isGeneratedAgentProposalSource("docs-x/generated/a.tex", "% Generated later\n"));
    }
    void metadataIsMandatoryAndValidated() {
        const QString id = "0190b000-0000-7000-8000-00000000000b";
        const QString reason = "Reason.\nPrepared in Codex conversation " + id + ".\n";
        // Since 2026-10-10 a wrapper without both metadata values is broken; the legacy sentence is just text.
        for (const QString &meta : QStringList{QString(), QStringLiteral("[created=2026-09-16T12:00:00Z]"), QStringLiteral("[conversation_id=") + id + QStringLiteral("]")}) {
            const auto legacy = Thoughts::parseLatexAgentProposals("\\agentProposedEdit" + meta + "{legacy}{" + reason + "}{Old.}{New.}");
            QVERIFY(legacy.proposals.isEmpty());
            QCOMPARE(legacy.issues.size(), 1); QCOMPARE(legacy.issues[0].code, "invalid_metadata"); QCOMPARE(legacy.issues[0].id, "legacy");
        }
        const auto full = Thoughts::parseLatexAgentProposals("\\agentProposedEdit[created=2026-09-16T12:00:00Z,conversation_id=" + id + "]{p}{" + reason + "}{Old.}{New.}");
        QCOMPARE(full.proposals.size(), 1); QCOMPARE(full.proposals[0].conversationId, id);
        QCOMPARE(Thoughts::proposalConversationId(id), id);
        QCOMPARE(Thoughts::proposalConversationId(id.toUpper()), id);
        QVERIFY(Thoughts::proposalConversationId("invalid").isEmpty());
        QVERIFY(Thoughts::proposalConversationId({}).isEmpty());
        const auto invalid = Thoughts::parseLatexAgentProposals("\\agentProposedEdit[created=2026-09-16T12:00:00Z,conversation_id=invalid]{p}{Why.}{Old.}{New.}");
        QVERIFY(invalid.proposals.isEmpty());
        QCOMPARE(invalid.issues.size(), 1); QCOMPARE(invalid.issues[0].code, "invalid_metadata"); QCOMPARE(invalid.issues[0].id, "p");
    }
    void createsRevisesAndReplays() {
        QTemporaryDir root; const auto path = root.filePath("note.tex");
        const QByteArray initial = "First.\nOld {text} with caf\xc3\xa9.\nLast.\n"; put(path, initial);
        auto req = request(path); req.insert("start_line", 2); req.insert("end_line", 3);
        req.insert("explanation", "Fix 50% & literal {braces} and \\commands.");
        const auto result = Thoughts::authorProposal(root.path(), req);
        QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(result["original_text"].toString(), QString::fromUtf8("Old {text} with caf\xc3\xa9.\n"));
        QCOMPARE(result["state"].toString(), "proposal_created");
        // Writers emit UTC with exactly three fractional digits (spec §3.2).
        QVERIFY(QRegularExpression("^\\d{4}-\\d{2}-\\d{2}T\\d{2}:\\d{2}:\\d{2}\\.\\d{3}Z$").match(result["created_at"].toString()).hasMatch());
        QVERIFY(Thoughts::isValidAgentProposalTimestamp(result["created_at"].toString()));
        const auto createdBytes = get(path);
        QVERIFY(createdBytes.contains(",conversation_id="));
        QVERIFY(!createdBytes.contains("Prepared in Codex"));
        QCOMPARE(result["conversation_id"], req["conversation_id"]);
        QVERIFY(createdBytes.startsWith("First.\n")); QVERIFY(createdBytes.endsWith("Last.\n"));
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["replayed"].toBool(), true);
        QCOMPARE(get(path), createdBytes);
        auto conflictingRetry = req; conflictingRetry.insert("replacement", "Different.");
        QCOMPARE(Thoughts::authorProposal(root.path(), conflictingRetry)["error_code"].toString(), "request_conflict");
        auto revise = request(path, false); revise.insert("proposal_id", result["proposal_id"]); revise.insert("replacement", "Revised text with $x$.\n");
        revise.insert("conversation_id", "0190b000-0000-7000-8000-00000000000b");
        const auto revised = Thoughts::authorProposal(root.path(), revise);
        QVERIFY2(!revised.contains("error_code"), qPrintable(QJsonDocument(revised).toJson()));
        QCOMPARE(revised["created_at"], result["created_at"]); QCOMPARE(revised["proposal_id"], result["proposal_id"]);
        QCOMPARE(revised["original_text"], result["original_text"]);
        QCOMPARE(revised["conversation_id"], req["conversation_id"]);
        QCOMPARE(revised["agent_conversation_id"], revise["conversation_id"]);
        QVERIFY(get(path).contains(req["conversation_id"].toString().toUtf8()));
        QVERIFY(!get(path).contains(revise["conversation_id"].toString().toUtf8()));
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {result["proposal_id"].toString()}, "reject").toUtf8(), initial);
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {result["proposal_id"].toString()}, "accept"), QString::fromUtf8("First.\nRevised text with $x$.\nLast.\n"));
        // Only the current state and a short-lived record of agent requests are kept.
        QHash<QString, QJsonObject> current; QVERIFY(Thoughts::ProposalHistory::latest(root.path(), &current));
        const auto state = current.value(result["proposal_id"].toString());
        QCOMPARE(state["kind"].toString(), "changed");
        QCOMPARE(state["agent_conversation_id"], revise["conversation_id"]);
        QCOMPARE(state["snapshot"].toObject()["conversation_id"], req["conversation_id"]);
        QCOMPARE(Thoughts::ProposalHistory::agentRequests(root.path()).size(), 2);
        QVERIFY(!QFileInfo::exists(Thoughts::ProposalHistory::filename(root.path())));
        const auto revisedBytes = get(path);
        QVERIFY(Thoughts::authorProposal(root.path(), revise)["replayed"].toBool()); QCOMPARE(get(path), revisedBytes);
    }
    void writesTheConversationLink() {
        // The MCP server passes the link (spec §3.6); revisions keep it, other commands refuse it.
        QTemporaryDir root; const auto tex = root.filePath("note.tex"), md = root.filePath("note.md");
        put(tex, "First.\nOld.\nLast.\n"); put(md, "First.\nOld.\nLast.\n");
        const QString link = "https://claude.ai/code/session_01ExampleSessionAbcdEfgh";
        for (const QString &path : {tex, md}) {
            auto req = request(path); req.insert("start_line", 2); req.insert("end_line", 3); req.insert("conversation_url", link);
            const auto created = Thoughts::authorProposal(root.path(), req);
            QVERIFY2(!created.contains("error_code"), qPrintable(QJsonDocument(created).toJson()));
            QCOMPARE(created["conversation_url"].toString(), link);
            auto parsed = Thoughts::parseAgentProposals(path, QString::fromUtf8(get(path)));
            QCOMPARE(parsed.proposals.size(), 1);
            QCOMPARE(parsed.proposals[0].conversationUrl, link);
            auto revise = request(path, false); revise.insert("proposal_id", created["proposal_id"]); revise.insert("replacement", "Revised.\n");
            QVERIFY(!Thoughts::authorProposal(root.path(), revise).contains("error_code"));
            parsed = Thoughts::parseAgentProposals(path, QString::fromUtf8(get(path)));
            QCOMPARE(parsed.proposals[0].conversationUrl, link);
            QCOMPARE(parsed.proposals[0].replacement, "Revised.\n");
            auto linkedRevision = request(path, false); linkedRevision.insert("proposal_id", created["proposal_id"]);
            linkedRevision.insert("replacement", "Again.\n"); linkedRevision.insert("conversation_url", link);
            QCOMPARE(Thoughts::authorProposal(root.path(), linkedRevision)["error_code"].toString(), "invalid_request");
        }
        for (const QJsonValue &bad : {QJsonValue("https://example.com/session_01"), QJsonValue("codex://threads/NOT-A-UUID"), QJsonValue(""), QJsonValue(7)}) {
            auto req = request(tex); req.insert("start_line", 1); req.insert("end_line", 2); req.insert("conversation_url", bad);
            QCOMPARE(Thoughts::authorProposal(root.path(), req)["error_code"].toString(), "invalid_request");
        }
    }
    void createsSeveralAgainstOneVersion() {
        for (const QString name : {"batch.tex", "batch.md"}) {
            QTemporaryDir root; const auto path = root.filePath(name);
            const QByteArray initial = "One.\n\nTwo.\n\nThree.\n\nFour.\n"; put(path, initial);
            auto proposal = [](int start, int end, const QString &replacement) {
                return QJsonObject{{"start_line", start}, {"end_line", end}, {"replacement", replacement}, {"explanation", QString("Why " + replacement.trimmed())}};
            };
            QJsonObject req {{"command", "create-proposals"}, {"source", path}, {"expected_source_sha256", Thoughts::ProposalHistory::digest(initial)},
                             {"conversation_id", "0190a000-0000-7000-8000-00000000000a"}, {"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                             // Out of order; lines all refer to the version above. 7 inserts at the end of the replaced line 5.
                             {"proposals", QJsonArray{proposal(7, 8, "4.\n"), proposal(1, 2, "1.\n"), proposal(5, 6, "3.\n"), proposal(6, 6, "Added.\n")}}};
            const auto result = Thoughts::authorProposal(root.path(), req);
            QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
            QCOMPARE(result["state"].toString(), "proposals_created");
            const auto proposals = result["proposals"].toArray();
            QCOMPARE(proposals.size(), 4);
            QCOMPARE(proposals[0].toObject()["original_text"].toString(), "Four.\n");
            QCOMPARE(proposals[1].toObject()["original_text"].toString(), "One.\n");
            QVERIFY(proposals[1].toObject()["line"].toInt() < proposals[2].toObject()["line"].toInt());
            QCOMPARE(result["source_sha256"].toString(), Thoughts::ProposalHistory::digest(get(path)));
            QStringList ids; for (const auto &value : proposals) ids << value.toObject()["proposal_id"].toString();
            QCOMPARE(QSet<QString>(ids.cbegin(), ids.cend()).size(), 4);
            QCOMPARE(decide(path, QString::fromUtf8(get(path)), ids, "accept"), "1.\n\nTwo.\n\n3.\nAdded.\n\n4.\n");
            QCOMPARE(decide(path, QString::fromUtf8(get(path)), ids, "disregard").toUtf8(), initial);
            QHash<QString, QJsonObject> current; QVERIFY(Thoughts::ProposalHistory::latest(root.path(), &current));
            for (const auto &id : ids) QCOMPARE(current.value(id)["kind"].toString(), "first_seen");
            const auto written = get(path);
            const auto replay = Thoughts::authorProposal(root.path(), req);
            QVERIFY(replay["replayed"].toBool()); QCOMPARE(replay["proposals"], result["proposals"]); QCOMPARE(get(path), written);

            // Invalid batches write nothing.
            const auto expected = Thoughts::ProposalHistory::digest(written);
            for (const auto &bad : {QJsonArray{proposal(1, 3, "A.\n"), proposal(2, 4, "B.\n")}, QJsonArray{proposal(4, 4, "A.\n"), proposal(4, 4, "B.\n")},
                                    QJsonArray{proposal(1, 2, "A.\n")}, QJsonArray{}, QJsonArray{QJsonObject{{"start_line", 2}, {"end_line", 3}, {"replacement", "x"}}}}) {
                auto invalid = req; invalid.insert("request_id", QUuid::createUuid().toString(QUuid::WithoutBraces));
                invalid.insert("expected_source_sha256", expected); invalid.insert("proposals", bad);
                QVERIFY(Thoughts::authorProposal(root.path(), invalid).contains("error_code"));
                QCOMPARE(get(path), written);
            }
            auto stray = req; stray.insert("request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)); stray.insert("replacement", "x");
            QCOMPARE(Thoughts::authorProposal(root.path(), stray)["error_code"].toString(), "invalid_request");
        }
    }
    void markdownCreatesRevisesAndResolves() {
        QTemporaryDir root; const auto path = root.filePath("proposal.md");
        const QByteArray initial = "# Introduction\n\nOld café {text}.\nNext sentence.\n\n# Methods\n";
        put(path, initial);
        auto req = request(path); req.insert("start_line", 3); req.insert("end_line", 5);
        req.insert("replacement", "New café {text}.\nAnother sentence.\n");
        const auto created = Thoughts::authorProposal(root.path(), req);
        QVERIFY2(!created.contains("error_code"), qPrintable(QJsonDocument(created).toJson()));
        QCOMPARE(created["original_text"].toString(), QString::fromUtf8("Old café {text}.\nNext sentence.\n"));
        const auto parsed = Thoughts::parseAgentProposals(path, QString::fromUtf8(get(path)));
        QVERIFY(parsed.issues.isEmpty());
        const auto &proposals = parsed.proposals;
        QCOMPARE(proposals.size(), 1);
        QCOMPARE(proposals[0].conversationId, req["conversation_id"].toString());
        QCOMPARE(proposals[0].replacement, req["replacement"].toString());
        QVERIFY(get(path).contains("New café {text}."));
        QVERIFY(get(path).contains("<!-- thoughts:agent-proposal:v2:start\n"));
        QVERIFY(!get(path).contains("replacement_length"));
        QVERIFY(get(path).contains("original_text:\n| Old café {text}.\n| Next sentence.\n| \n"));
        QVERIFY(!get(path).contains("eyJjb252ZXJzYXRpb25faWQi"));
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["replayed"].toBool(), true);
        auto revise = request(path, false); revise.insert("proposal_id", created["proposal_id"]);
        revise.insert("replacement", "Revised text without a final newline.");
        const auto revised = Thoughts::authorProposal(root.path(), revise);
        QVERIFY2(!revised.contains("error_code"), qPrintable(QJsonDocument(revised).toJson()));
        QCOMPARE(revised["created_at"], created["created_at"]);
        QCOMPARE(revised["original_text"], created["original_text"]);
        QCOMPARE(revised["replacement"].toString(), "Revised text without a final newline.\n");
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {created["proposal_id"].toString()}, "accept"), QString::fromUtf8("# Introduction\n\nRevised text without a final newline.\n\n# Methods\n"));
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {created["proposal_id"].toString()}, "disregard").toUtf8(), initial);
        auto remove = request(path, false); remove.insert("command", "remove-proposal"); remove.remove("replacement");
        remove.insert("proposal_id", created["proposal_id"]); remove.insert("expected_source_sha256", revised["source_sha256"]);
        const auto removed = Thoughts::authorProposal(root.path(), remove);
        QVERIFY2(!removed.contains("error_code"), qPrintable(QJsonDocument(removed).toJson()));
        QCOMPARE(get(path), initial);
    }
    void legacyMarkdownRevisesToReadable() {
        QTemporaryDir root; const auto path = root.filePath("legacy.md");
        const QJsonObject metadata {{"id", "agent-old"}, {"created_at", "2026-09-23T17:34:16.099Z"},
                                    {"conversation_id", "0190c000-0000-7000-8000-00000000000c"},
                                    {"explanation", "Align with the abstract."}, {"original_text", "Old text.\n"},
                                    {"replacement_length", 1387}};
        const auto encoded = QJsonDocument(metadata).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        put(path, "<!-- thoughts:agent-proposal:v1:start " + encoded + " -->\nNew text.\n<!-- thoughts:agent-proposal:v1:end agent-old -->\n");
        auto req = request(path, false); req.insert("proposal_id", "agent-old"); req.insert("replacement", "New text.\n");
        req.insert("explanation", "Align with the abstract.");
        const auto revised = Thoughts::authorProposal(root.path(), req);
        QVERIFY2(!revised.contains("error_code"), qPrintable(QJsonDocument(revised).toJson()));
        QCOMPARE(revised["created_at"].toString(), "2026-09-23T17:34:16.099Z");
        QCOMPARE(revised["conversation_id"].toString(), "0190c000-0000-7000-8000-00000000000c");
        QVERIFY(get(path).contains("<!-- thoughts:agent-proposal:v2:start\n"));
        QVERIFY(get(path).contains("original_text:\n| Old text.\n| \n"));
        QVERIFY(!get(path).contains("replacement_length"));
    }
    void markdownClosingMarkerDefinesReplacement() {
        Thoughts::AgentProposedEdit p;
        p.id = "agent-marker"; p.createdAt = "2026-09-23T17:34:16.099Z";
        p.conversationId = "0190c000-0000-7000-8000-00000000000c";
        p.explanation = "Fix the text."; p.original = "Original.\n";
        for (const QString &after : {QString(), QString::fromUtf8("Edited 😀 text.\r\n\n")}) {
            p.replacement = after;
            const QString block = Thoughts::formatMarkdownAgentProposal(p);
            for (const QString &legacy : {QString(), QString("replacement_length: 1387\n")}) {
                QString value = block;
                value.replace("explanation:\n", legacy + "explanation:\n");
                const auto parsed = Thoughts::parseAgentProposals("test.md", value + "Methods.\n");
                QVERIFY(parsed.issues.isEmpty());
                QCOMPARE(parsed.proposals.size(), 1); QCOMPARE(parsed.proposals[0].replacement, after);
                QCOMPARE(decide("test.md", value + "Methods.\n", {p.id}, "accept"), after + "Methods.\n");
            }
        }
        p.replacement = "Text.\n";
        const QString value = Thoughts::formatMarkdownAgentProposal(p);
        // Each break is one issue of its own (spec §6).
        const auto issues = [](const QString &source) {
            QStringList codes;
            for (const auto &issue : Thoughts::parseAgentProposals("test.md", source).issues) codes << issue.code;
            return codes;
        };
        QString broken = value; broken.replace("v2:end agent-marker", "v2:end other");
        QCOMPARE(issues(broken), QStringList({"unmatched_marker", "unmatched_marker"}));
        broken = value; broken.replace("Text.\n", "Text.");
        QCOMPARE(issues(broken), QStringList({"unmatched_marker"})); // the end marker no longer starts a line
        broken = value; broken.replace("Text.\n", "<!-- thoughts:agent-proposal:v1:start nested -->\n");
        QCOMPARE(issues(broken), QStringList({"nested_proposal"}));
    }
    void removesAndRestoresOriginal_data() {
        QTest::addColumn<QString>("before"); QTest::addColumn<QString>("after");
        QTest::newRow("replacement") << QString::fromUtf8("Original café {text}.\r\n") << QString("New text.\n");
        QTest::newRow("addition") << QString() << QString("Added text.\n");
        QTest::newRow("deletion") << QString("Original text.\n") << QString();
        QTest::newRow("empty-addition") << QString() << QString();
    }
    void removesAndRestoresOriginal() {
        QFETCH(QString, before); QFETCH(QString, after);
        QTemporaryDir root; const auto path = root.filePath("note.tex");
        const QByteArray neighbour = "\\agentProposedEdit[created=2026-09-12T12:13:36Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{other}{Why.}{Keep.}{Change.}";
        const QByteArray wrapper = (QString("\\agentProposedEdit [created=2026-09-12T12:13:36Z,conversation_id=0190b000-0000-7000-8000-00000000000b]\n{stable}{Why.}{") + before + "}{" + after + "}").toUtf8();
        const QByteArray initial = "First.\n" + wrapper + "Last.\n" + neighbour;
        put(path, initial);
        auto req = request(path, false); req.insert("command", "remove-proposal"); req.remove("replacement"); req.insert("proposal_id", "stable");
        const auto result = Thoughts::authorProposal(root.path(), req);
        QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(result["state"].toString(), "proposal_removed");
        QCOMPARE(result["original_text"].toString(), before);
        QCOMPARE(result["conversation_id"].toString(), "0190b000-0000-7000-8000-00000000000b");
        QCOMPARE(result["agent_conversation_id"], req["conversation_id"]);
        const QByteArray expected = "First.\n" + before.toUtf8() + "Last.\n" + neighbour;
        QCOMPARE(get(path), expected);
        // A withdrawn proposal is resolved: it leaves the current state.
        QHash<QString, QJsonObject> current; QVERIFY(Thoughts::ProposalHistory::latest(root.path(), &current));
        QVERIFY(!current.contains("stable"));
        QVERIFY(current.contains("other"));
        QVERIFY(Thoughts::authorProposal(root.path(), req)["replayed"].toBool());
        QCOMPARE(get(path), expected);
        auto conflict = req; conflict.insert("explanation", "Different reason.");
        QCOMPARE(Thoughts::authorProposal(root.path(), conflict)["error_code"].toString(), "request_conflict");
        put(path, initial);
        QVERIFY(Thoughts::observeAgentProposalSource(root.path(), path, initial));
        QVERIFY(Thoughts::ProposalHistory::latest(root.path(), &current));
        QCOMPARE(current.value("stable")["kind"].toString(), "first_seen");
        QCOMPARE(current.value("stable")["snapshot"].toObject()["before"].toString(), before);
    }
    void removalGuards() {
        QTemporaryDir root; const auto path = root.filePath("note.tex");
        const QByteArray initial = "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{legacy}{Why.}{Old.}{New.}";
        put(path, initial);
        auto req = request(path, false); req.insert("command", "remove-proposal"); req.remove("replacement"); req.insert("proposal_id", "legacy");
        auto invalid = req; invalid.insert("expected_source_sha256", QString(64, '0'));
        QCOMPARE(Thoughts::authorProposal(root.path(), invalid)["error_code"].toString(), "source_conflict");
        invalid = req; invalid.insert("proposal_id", "missing");
        QCOMPARE(Thoughts::authorProposal(root.path(), invalid)["error_code"].toString(), "unknown_id");
        invalid = req; invalid.insert("replacement", "Not allowed.");
        QCOMPARE(Thoughts::authorProposal(root.path(), invalid)["error_code"].toString(), "invalid_request");
        put(root.filePath("duplicate.tex"), initial);
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["error_code"].toString(), "duplicate_id");
        QVERIFY(QFile::remove(root.filePath("duplicate.tex")));
        QCOMPARE(get(path), initial);
        const auto removed = Thoughts::authorProposal(root.path(), req);
        QVERIFY2(!removed.contains("error_code"), qPrintable(QJsonDocument(removed).toJson()));
        QCOMPARE(get(path), QByteArray("Old."));
    }
    void preservesExistingProposalFormatting() {
        QTemporaryDir root; const auto path = root.filePath("note.tex");
        const QByteArray initial = "\\agentProposedEdit [created=2026-09-12T12:13:36.023Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]\n {stable}\n {Old explanation.}\n {Original % comment\n  text.}\n {Old replacement.}\n";
        put(path, initial); auto req = request(path, false); req.insert("proposal_id", "stable"); req.insert("replacement", "New replacement.");
        const auto result = Thoughts::authorProposal(root.path(), req); QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
        const auto after = get(path);
        QVERIFY(after.startsWith("\\agentProposedEdit [created=2026-09-12T12:13:36.023Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]\n {stable}\n {"));
        QVERIFY(after.contains("}\n {Original % comment\n  text.}\n {New replacement.}\n"));
    }
    void insertionAndDeletion() {
        QTemporaryDir root; const auto path = root.filePath("note.tex");
        put(path, "First.\r\nLast.\r\n");
        auto req = request(path); req.insert("start_line", 2); req.insert("end_line", 2);
        const auto inserted = Thoughts::authorProposal(root.path(), req); QVERIFY(!inserted.contains("error_code")); QCOMPARE(inserted["original_text"].toString(), "");
        put(path, "Delete me.\nKeep me.\n"); req = request(path); req.insert("start_line", 1); req.insert("end_line", 2); req.insert("replacement", "");
        const auto deleted = Thoughts::authorProposal(root.path(), req); QVERIFY(!deleted.contains("error_code")); QCOMPARE(deleted["original_text"].toString(), "Delete me.\n");
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {deleted["proposal_id"].toString()}, "accept"), "Keep me.\n");
    }
    void rejectsInvalidMutations_data() {
        QTest::addColumn<QString>("field"); QTest::addColumn<QJsonValue>("value"); QTest::addColumn<QString>("code");
        QTest::newRow("stale") << "expected_source_sha256" << QJsonValue(QString(64, '0')) << "source_conflict";
        QTest::newRow("unbalanced") << "replacement" << QJsonValue("Bad } text.") << "invalid_proposal";
        QTest::newRow("nested") << "replacement" << QJsonValue("\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{p}{Why.}{Old.}{New.}") << "invalid_proposal";
        QTest::newRow("trailing-comment") << "replacement" << QJsonValue("Text % swallowed closing brace") << "invalid_proposal";
        QTest::newRow("range") << "end_line" << QJsonValue(50) << "invalid_range";
        QTest::newRow("boolean-line") << "start_line" << QJsonValue(true) << "invalid_request";
        QTest::newRow("fractional-line") << "start_line" << QJsonValue(1.5) << "invalid_request";
        QTest::newRow("empty-explanation") << "explanation" << QJsonValue(" ") << "invalid_request";
        QTest::newRow("missing-conversation") << "conversation_id" << QJsonValue("") << "invalid_request";
        QTest::newRow("immutable-field") << "original_text" << QJsonValue("Changed original.") << "invalid_request";
    }
    void rejectsInvalidMutations() {
        QFETCH(QString, field); QFETCH(QJsonValue, value); QFETCH(QString, code);
        QTemporaryDir root; const auto path = root.filePath("note.tex"); put(path, "Old text.\n");
        auto req = request(path); req.insert("start_line", 1); req.insert("end_line", 2); req.insert(field, value);
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["error_code"].toString(), code); QCOMPARE(get(path), QByteArray("Old text.\n"));
    }
    void brokenProposalsKeepTheirSpecCodes() {
        // One broken wrapper does not fail the file: the others stay usable, and
        // commands on the broken one return its issue code (spec §6, §7).
        QTemporaryDir root; const auto path = root.filePath("note.tex");
        const QByteArray good = "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{good}{Why.}{Old.\n}{New.\n}";
        const QByteArray broken = "\\agentProposedEdit[created=2026-10-10T12:00:00+02:00,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{bad}{Why.}{Old.\n}{New.\n}";
        const QByteArray blank = "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{blank}{ }{Old.\n}{New.\n}";
        const QByteArray initial = "Intro.\n" + good + "\n" + broken + "\n" + blank + "\nTail.\n"; put(path, initial);
        const auto parsed = Thoughts::parseAgentProposals(path, QString::fromUtf8(initial));
        QCOMPARE(parsed.proposals.size(), 1);
        QCOMPARE(parsed.issues.size(), 2);
        QCOMPARE(parsed.issues[0].code, "invalid_metadata"); QCOMPARE(parsed.issues[0].id, "bad"); QCOMPARE(parsed.issues[0].line, 5);
        QCOMPARE(parsed.issues[1].code, "invalid_metadata"); QCOMPARE(parsed.issues[1].id, "blank");
        for (const QString command : {"revise-proposal", "remove-proposal"}) {
            auto req = request(path, false); req.insert("command", command); req.insert("proposal_id", "bad");
            if (command == QLatin1String("remove-proposal")) req.remove("replacement");
            const auto result = Thoughts::authorProposal(root.path(), req);
            QCOMPARE(result["error_code"].toString(), "invalid_metadata");
            QCOMPARE(result["proposal_id"].toString(), "bad");
            QCOMPARE(get(path), initial);
        }
        auto missing = request(path, false); missing.insert("proposal_id", "absent");
        QCOMPARE(Thoughts::authorProposal(root.path(), missing)["error_code"].toString(), "unknown_id");
        // A new proposal next to the broken ones, and a revision of the good one.
        auto create = request(path); create.insert("start_line", 11); create.insert("end_line", 12);
        const auto created = Thoughts::authorProposal(root.path(), create);
        QVERIFY2(!created.contains("error_code"), qPrintable(QJsonDocument(created).toJson()));
        auto revise = request(path, false); revise.insert("proposal_id", "good");
        const auto revised = Thoughts::authorProposal(root.path(), revise);
        QVERIFY2(!revised.contains("error_code"), qPrintable(QJsonDocument(revised).toJson()));
        // A range that touches a broken wrapper would nest it.
        auto overlap = request(path); overlap.insert("start_line", 5); overlap.insert("end_line", 6);
        QCOMPARE(Thoughts::authorProposal(root.path(), overlap)["error_code"].toString(), "proposal_overlap");
        // Decisions: the broken one is refused with its code, the good one applies.
        const QString current = QString::fromUtf8(get(path));
        QCOMPARE(decide(path, current, {"bad"}, "accept"), "refused:invalid_metadata");
        QCOMPARE(decide(path, current, {"good", "bad"}, "accept"), "refused:invalid_metadata");
        QVERIFY(decide(path, current, {"good"}, "accept").startsWith("Intro.\nNew text.\n\n\\agentProposedEdit[created=2026-10-10T12:00:00+02:00,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{bad}"));
        QString error, code;
        QVERIFY(!Thoughts::resolveLiveAgentProposedEdits(root.path(), path, Thoughts::ProposalHistory::digest(get(path)), {"bad"}, "accept", &error, nullptr, nullptr, &code));
        QCOMPARE(code, "invalid_metadata");
        QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(root.path(), path, Thoughts::ProposalHistory::digest(get(path)), {"good"}, "accept", &error, nullptr, nullptr, &code), qPrintable(error));
        QVERIFY(get(path).contains("{bad}"));
    }
    void writerRefusalsCarryReasons_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QString>("replacement"); QTest::addColumn<QString>("reason");
        QTest::newRow("tex-unbalanced") << "note.tex" << "Bad } text.\n" << "unbalanced_braces";
        QTest::newRow("tex-final-backslash") << "note.tex" << "Line break \\" << "unbalanced_braces";
        QTest::newRow("tex-trailing-comment") << "note.tex" << "Text % comment" << "unterminated_comment";
        QTest::newRow("tex-nested") << "note.tex" << "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{p}{Why.}{Old.}{New.}" << "nested_proposal";
        QTest::newRow("tex-agent-remove-is-text") << "note.tex" << "\\agentRemove{a}{b}{c}\n" << "";
        QTest::newRow("md-nested") << "note.md" << "<!-- thoughts:agent-proposal:v2:end x -->\n" << "nested_proposal";
        QTest::newRow("md-tex-wrapper-is-text") << "note.md" << "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{p}{Why.}{Old.}{New.}\n" << "";
        QTest::newRow("md-unbalanced-is-text") << "note.md" << "Bad } text % here\n" << "";
    }
    void writerRefusalsCarryReasons() {
        QFETCH(QString, name); QFETCH(QString, replacement); QFETCH(QString, reason);
        QTemporaryDir root; const auto path = root.filePath(name); put(path, "Old text.\n");
        auto req = request(path); req.insert("start_line", 1); req.insert("end_line", 2); req.insert("replacement", replacement);
        const auto result = Thoughts::authorProposal(root.path(), req);
        if (reason.isEmpty()) {
            QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
            return;
        }
        QCOMPARE(result["error_code"].toString(), "invalid_proposal");
        QCOMPARE(result["reason"].toString(), reason);
        QCOMPARE(get(path), QByteArray("Old text.\n"));
    }
    void rejectsOverlapDuplicatesAndOutsideSource() {
        QTemporaryDir root; QTemporaryDir outside; const auto path = root.filePath("note.tex");
        const QByteArray proposal = "\\agentProposedEdit[created=2026-09-12T12:13:36Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{stable}{Why.}{Old.}{New.}\n"; put(path, proposal);
        auto req = request(path); req.insert("start_line", 1); req.insert("end_line", 2);
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["error_code"].toString(), "proposal_overlap");
        put(root.filePath("other.tex"), proposal); req = request(path, false); req.insert("proposal_id", "stable");
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["error_code"].toString(), "duplicate_id"); QCOMPARE(get(path), proposal);
        const auto outer = outside.filePath("outside.tex"); put(outer, "Outside.\n"); req = request(outer); req.insert("start_line", 1); req.insert("end_line", 2);
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["error_code"].toString(), "invalid_source");
        QVERIFY(QFile::link(outer, root.filePath("link.tex"))); req.insert("source", root.filePath("link.tex"));
        QCOMPARE(Thoughts::authorProposal(root.path(), req)["error_code"].toString(), "invalid_source");
    }

    // 2026-11-10 (thoughts-server baff0ed, links by PDF): the build finds a
    // Library PDF by its filename in any Project, so a \PdfLink to another
    // Project's PDF resolves, and so does a \CrossPdfLink through any folder,
    // the note's own, one of no Project or one the PDF left.
    void pdfLinksFindLibraryPdfsByName() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        QVERIFY(QDir(root).mkpath("docs-concepts-a6fe5d/tex/source"));
        QVERIFY(QDir(root).mkpath("docs-aaa-111111/tex/source"));
        QVERIFY(QDir(root).mkpath("docs-benchmark-f9d64d/tex/source"));
        put(root + "/docs-concepts-a6fe5d/tex/source/grpo.tex", "Intro.\nOld claim.\nEnd.\n");
        put(root + "/docs-benchmark-f9d64d/tex/source/main.tex", "Main.\n");
        writeProject(root, "test:concepts", "concepts", "docs-concepts-a6fe5d", {"tex/source/grpo.tex"}, {});
        writeProject(root, "test:benchmark", "Benchmark", "docs-benchmark-f9d64d", {"tex/source/main.tex"},
                     {{"paper_abc.pdf", pdfWithAnnotations({"doc-anchor:intro"})}}, {}, {"docs-aaa-111111"});
        const QString grpo = root + "/docs-concepts-a6fe5d/tex/source/grpo.tex";
        const QByteArray before = get(grpo);
        for (const QString &link : {QStringLiteral("\\PdfLink{paper_abc}{document-start}{Chu}"), QStringLiteral("\\PdfLink{paper_abc}{intro}{Chu}"),
                                    QStringLiteral("\\CrossPdfLink{docs-benchmark-f9d64d/paper_abc}{intro}{Chu}"), QStringLiteral("\\CrossPdfLink{docs-aaa-111111/paper_abc}{document-start}{Chu}"),
                                    QStringLiteral("\\CrossPdfLink{docs-concepts-a6fe5d/paper_abc}{document-start}{Chu}"), QStringLiteral("\\CrossPdfLink{docs-moved-away/paper_abc}{intro}{Chu}")}) {
            const auto result = create(root, grpo, 2, 3, "See " + link + ".\n");
            QVERIFY2(result["accepted"].toBool(), qPrintable(link + ": " + QJsonDocument(result).toJson()));
            put(grpo, before);
        }
        // The anchor is looked up in the PDF the build finds.
        auto result = create(root, grpo, 2, 3, "See \\PdfLink{paper_abc}{paper-title}{Chu}.\n");
        QCOMPARE(result["error_code"].toString(), "invalid_link");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "anchor_not_found");
        QCOMPARE(result["links"].toArray().first().toObject()["suggestion"].toString(), "\\PdfLink{paper_abc}{document-start}{Chu}");
        QVERIFY2(result["message"].toString().contains("intro"), qPrintable(result["message"].toString()));
        QCOMPARE(create(root, grpo, 2, 3, "See \\CrossPdfLink{docs-moved-away/paper_abc}{paper-title}{Chu}.\n")["links"].toArray().first().toObject()["suggestion"].toString(),
                 "\\CrossPdfLink{docs-moved-away/paper_abc}{document-start}{Chu}");
        // The build matches the Library filename exactly.
        result = create(root, grpo, 2, 3, "See \\PdfLink{Paper_ABC}{document-start}{Chu}.\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "pdf_not_in_library");
        QCOMPARE(result["links"].toArray().first().toObject()["suggestion"].toString(), "\\PdfLink{paper_abc}{document-start}{Chu}");
        QVERIFY2(result["message"].toString().contains("Project Benchmark"), qPrintable(result["message"].toString()));
        // Another Project's PDF that this Library does not hold: a named anchor is left to the build.
        const QString paper = root + "/.thoughts/projects/" + projectHash("test:benchmark") + "/documents/paper_abc.pdf";
        QVERIFY(QFile::remove(paper));
        QVERIFY(create(root, grpo, 2, 3, "See \\PdfLink{paper_abc}{paper-title}{Chu}.\n")["accepted"].toBool());
    }
    // 2026-10-12: a \PdfLink to a PDF the build could not reach was accepted, and every Sync failed on it.
    void proposalLinksMustResolve() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        writeLinkLibrary(root);
        const QString grpo = root + "/docs-concepts/tex/source/grpo.tex";
        const QByteArray before = get(grpo);
        const auto accepted = [&](const QString &replacement) {
            const auto result = create(root, grpo, 2, 3, replacement);
            put(grpo, before);
            if (!result["accepted"].toBool()) qWarning("%s", QJsonDocument(result).toJson().constData());
            return result["accepted"].toBool();
        };
        // Another Project's PDF through \PdfLink (since 2026-11-10 the build finds it by
        // name): checked against that PDF, whose anchors are intro only.
        auto result = create(root, grpo, 2, 3, "See \\PdfLink{paper_abc}{paper-title}{Chu et al.}.\n");
        QCOMPARE(result["error_code"].toString(), "invalid_link");
        QCOMPARE(get(grpo), before);
        auto link = result["links"].toArray().first().toObject();
        QCOMPARE(link["reason"].toString(), "anchor_not_found");
        QCOMPARE(link["command"].toString(), "PdfLink");
        QCOMPARE(link["suggestion"].toString(), "\\PdfLink{paper_abc}{document-start}{Chu et al.}");
        QVERIFY(result["message"].toString().contains("paper-title"));
        QVERIFY(accepted("See \\PdfLink{paper_abc}{intro}{Chu}.\n"));
        QVERIFY(accepted("See \\PdfLink{paper_abc}{document-start}{Chu et al.}.\n"));
        // \CrossPdfLink keeps working, with document-start or an existing anchor.
        QVERIFY(accepted("See \\CrossPdfLink{docs-followup/paper_abc}{document-start}{Chu et al.}.\n"));
        QVERIFY(accepted("See \\CrossPdfLink{docs-followup/paper_abc}{intro}{Chu}.\n"));
        // A missing anchor, in this Project's PDF or another's.
        result = create(root, grpo, 2, 3, "See \\PdfLink{own}{nope}{x}.\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "anchor_not_found");
        QVERIFY(result["message"].toString().contains("hyp"));
        result = create(root, grpo, 2, 3, "See \\CrossPdfLink{docs-followup/paper_abc}{nope}{x}.\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "anchor_not_found");
        QCOMPARE(result["links"].toArray().first().toObject()["suggestion"].toString(), "\\CrossPdfLink{docs-followup/paper_abc}{document-start}{x}");
        QVERIFY(accepted("See \\PdfLink{own}{hyp}{x}.\n"));
        // The folder of a \CrossPdfLink only says where to look first: the note's own folder,
        // a folder of no Project, or one that does not hold the PDF still resolve by name.
        QVERIFY(accepted("\\CrossPdfLink{docs-concepts/paper_abc}{intro}{x}\n"));
        QVERIFY(accepted("\\CrossPdfLink{docs-concepts/own}{hyp}{x}\n"));
        QVERIFY(accepted("\\CrossPdfLink{docs-elsewhere/paper_abc}{document-start}{x}\n"));
        QVERIFY(accepted("\\CrossPdfLink{docs-followup/own}{document-start}{x}\n"));
        // The note's own Project comes first: its PDF's anchors decide.
        QCOMPARE(create(root, grpo, 2, 3, "\\CrossPdfLink{docs-followup/own}{intro}{x}\n")["links"].toArray().first().toObject()["reason"].toString(), "anchor_not_found");
        // Malformed targets and PDFs no Project selects are refused, with the \PdfLink to use.
        result = create(root, grpo, 2, 3, "\\PdfLink{docs-followup/paper_abc}{document-start}{x}\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "invalid_pdf_key");
        QCOMPARE(result["links"].toArray().first().toObject()["suggestion"].toString(), "\\PdfLink{paper_abc}{document-start}{x}");
        result = create(root, grpo, 2, 3, "\\CrossPdfLink{paper_abc.pdf}{intro}{x}\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "invalid_cross_target");
        QCOMPARE(result["links"].toArray().first().toObject()["suggestion"].toString(), "\\PdfLink{paper_abc}{intro}{x}");
        result = create(root, grpo, 2, 3, "\\PdfLink{unselected}{document-start}{x}\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "pdf_not_selected");
        QVERIFY2(result["message"].toString().contains("not selected for building"), qPrintable(result["message"].toString()));
        QCOMPARE(create(root, grpo, 2, 3, "\\CrossPdfLink{docs-followup/unselected}{document-start}{x}\n")["links"].toArray().first().toObject()["reason"].toString(), "pdf_not_selected");
        result = create(root, grpo, 2, 3, "\\PdfLink{nothing_like_it}{document-start}{x}\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "pdf_not_in_library");
        QVERIFY(result["message"].toString().contains("No Project in the Library"));
        result = create(root, grpo, 2, 3, "\\CrossPdfLink{docs-followup/paper_abc_v2}{document-start}{x}\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "pdf_not_in_library");
        QVERIFY2(result["message"].toString().contains("similar PDF keys: paper_abc"), qPrintable(result["message"].toString()));
        // Source links: target selection and labels.
        QVERIFY(create(root, grpo, 2, 3, "\\SourceLink{tex/source/other.tex}{known}{x}\n")["accepted"].toBool());
        put(grpo, before);
        QCOMPARE(create(root, grpo, 2, 3, "\\SourceLink{tex/source/other.tex}{unknown}{x}\n")["links"].toArray().first().toObject()["reason"].toString(), "label_not_found");
        QCOMPARE(create(root, grpo, 2, 3, "\\SourceLink{tex/source/main.tex}{known}{x}\n")["links"].toArray().first().toObject()["suggestion"].toString(),
                 "\\CrossSourceLink{docs-followup/tex/source/main.tex}{known}{x}");
        QCOMPARE(create(root, grpo, 2, 3, "\\CrossSourceLink{docs-concepts/tex/source/other.tex}{known}{x}\n")["links"].toArray().first().toObject()["suggestion"].toString(),
                 "\\SourceLink{tex/source/other.tex}{known}{x}");
        // A label the same proposal adds counts.
        QVERIFY(create(root, grpo, 2, 3, "\\SourceLabel{fresh}\\SourceLink{tex/source/grpo.tex}{fresh}{x}\n")["accepted"].toBool());
        put(grpo, before);
        // Links inside verbatim or comments are not links.
        QVERIFY(create(root, grpo, 2, 3, "% \\PdfLink{paper_abc}{x}{y}\n\\verb|\\PdfLink{paper_abc}{x}{y}|\n")["accepted"].toBool());
        put(grpo, before);
        QCOMPARE(get(grpo), before);
    }
    // 2026-10-26: \TexLink and \CrossTexLink are retired. A proposal that adds
    // one is refused with the \SourceLink / \CrossSourceLink that replaces it;
    // one the original text already has does not block the proposal.
    void retiredTexLinksAreRefusedWithTheirReplacement() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        writeLinkLibrary(root);
        const QString grpo = root + "/docs-concepts/tex/source/grpo.tex";
        const QByteArray before = get(grpo);
        // The one link problem of a refused proposal (otherwise the whole result, as its reason).
        const auto only = [&](const QString &replacement) {
            const auto result = create(root, grpo, 2, 3, replacement);
            const QJsonArray links = result["links"].toArray();
            if (result["error_code"].toString() != "invalid_link" || links.size() != 1)
                return QJsonObject{{"reason", QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact))}};
            return links.first().toObject();
        };
        // The short alias of tex/source/other.tex, whose Source label exists.
        auto link = only("See \\TexLink{docs-concepts/other.tex}{known}{the other note}.\n");
        QCOMPARE(link["reason"].toString(), "retired_link");
        QCOMPARE(link["command"].toString(), "TexLink");
        QCOMPARE(link["text"].toString(), "\\TexLink{docs-concepts/other.tex}{known}{the other note}");
        QCOMPARE(link["suggestion"].toString(), "\\SourceLink{tex/source/other.tex}{known}{the other note}");
        QVERIFY2(link["message"].toString().contains("\\TexLink is retired; use \\SourceLink{tex/source/other.tex}{known}{the other note}."),
                 qPrintable(link["message"].toString()));
        QCOMPARE(get(grpo), before);
        // Only a \label: the same command, and the target needs \SourceLabel.
        link = only("\\TexLink{docs-concepts/tex/source/other.tex}{sec:other}{y}\n");
        QCOMPARE(link["suggestion"].toString(), "\\SourceLink{tex/source/other.tex}{sec:other}{y}");
        QVERIFY2(link["message"].toString().contains("mark the target with \\SourceLabel{sec:other} in docs-concepts/tex/source/other.tex (it has only \\label{sec:other})"),
                 qPrintable(link["message"].toString()));
        // Another folder: \CrossSourceLink with the full path.
        link = only("\\CrossTexLink{docs-followup/main.tex}{intro}{z}\n");
        QCOMPARE(link["command"].toString(), "CrossTexLink");
        QCOMPARE(link["suggestion"].toString(), "\\CrossSourceLink{docs-followup/tex/source/main.tex}{intro}{z}");
        QVERIFY(link["message"].toString().contains("\\SourceLabel{intro}"));
        QVERIFY(!link["message"].toString().contains("\\label{intro}"));
        // A \TexLink to another folder becomes a Cross link; a \CrossTexLink to this one a plain one.
        QCOMPARE(only("\\TexLink{docs-followup/tex/source/main.tex}{intro}{z}\n")["suggestion"].toString(),
                 "\\CrossSourceLink{docs-followup/tex/source/main.tex}{intro}{z}");
        QCOMPARE(only("\\CrossTexLink{docs-concepts/other.tex}{known}{z}\n")["suggestion"].toString(), "\\SourceLink{tex/source/other.tex}{known}{z}");
        // An unknown target or an invalid Source label: refused, without a suggestion.
        link = only("\\TexLink{docs-nowhere/x.tex}{a}{b}\n");
        QCOMPARE(link["reason"].toString(), "retired_link");
        QVERIFY(!link.contains("suggestion"));
        QVERIFY(link["message"].toString().contains("\\CrossSourceLink{docs-<folder>/<path>}{label}{text}"));
        for (const QString &replacement : {QStringLiteral("\\TexLink{docs-concepts/other.tex}{bad label}{b}\n"), QStringLiteral("\\TexLink{docs-concepts/other.tex}{known}\n")}) {
            link = only(replacement);
            QCOMPARE(link["reason"].toString(), "retired_link");
            QVERIFY(!link.contains("suggestion"));
        }
        // Next to a working \SourceLink only the retired link is reported.
        QCOMPARE(only("\\SourceLink{tex/source/other.tex}{known}{x} \\TexLink{docs-concepts/other.tex}{sec:other}{y}\n")["command"].toString(), "TexLink");
        QCOMPARE(get(grpo), before);

        // An old \TexLink already in the source does not block rewording its line, nor revising that proposal.
        const QString other = root + "/docs-concepts/tex/source/other.tex";
        put(other, "\\SourceLabel{known}Other.\nSee \\TexLink{docs-concepts/grpo.tex}{document-start}{GRPO}.\n\\label{sec:other}\n");
        auto result = create(root, other, 2, 3, "Read \\TexLink{docs-concepts/grpo.tex}{document-start}{the GRPO note}.\n");
        QVERIFY2(result["accepted"].toBool(), qPrintable(QJsonDocument(result).toJson()));
        const QByteArray proposed = get(other);
        auto revise = request(other, false); revise.insert("proposal_id", result["proposal_id"].toString());
        revise["replacement"] = "Now read \\TexLink{docs-concepts/grpo.tex}{document-start}{GRPO}.\n";
        result = Thoughts::authorProposal(root, revise);
        QVERIFY2(result["accepted"].toBool(), qPrintable(QJsonDocument(result).toJson()));
        // A revision that adds another one is refused.
        revise["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        revise["expected_source_sha256"] = Thoughts::ProposalHistory::digest(get(other));
        revise["replacement"] = "Read \\TexLink{docs-concepts/grpo.tex}{document-start}{GRPO} and \\CrossTexLink{docs-followup/main.tex}{document-start}{main}.\n";
        const QByteArray revised = get(other);
        result = Thoughts::authorProposal(root, revise);
        QCOMPARE(result["error_code"].toString(), "invalid_link");
        QCOMPARE(result["links"].toArray().size(), 1);
        QCOMPARE(result["links"].toArray().first().toObject()["suggestion"].toString(), "\\CrossSourceLink{docs-followup/tex/source/main.tex}{document-start}{main}");
        QCOMPARE(get(other), revised);
        QVERIFY(proposed != revised);
    }
    // Only links a proposal introduces are checked; an old broken link is the source's own.
    void preexistingBrokenLinksDoNotBlockProposals() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        writeLinkLibrary(root);
        const QString grpo = root + "/docs-concepts/tex/source/grpo.tex";
        QVERIFY(create(root, grpo, 1, 2, "New intro.\n")["accepted"].toBool());
        const QString text = QString::fromUtf8(get(grpo));
        const int line = text.left(text.indexOf("See \\PdfLink{gone}")).count('\n') + 1;
        // Rewording the line keeps its old link: allowed.
        auto result = create(root, grpo, line, line + 1, "Read \\PdfLink{gone}{document-start}{that paper}.\n");
        QVERIFY2(result["accepted"].toBool(), qPrintable(QJsonDocument(result).toJson()));
    }
    // revise-proposal and create-proposals check their new text too.
    void reviseAndBatchCheckLinks() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        writeLinkLibrary(root);
        const QString grpo = root + "/docs-concepts/tex/source/grpo.tex";
        const auto created = create(root, grpo, 2, 3, "A good claim.\n");
        QVERIFY(created["accepted"].toBool());
        const QByteArray before = get(grpo);
        auto revise = request(grpo, false); revise.insert("proposal_id", created["proposal_id"].toString());
        revise["replacement"] = "A claim \\PdfLink{paper_abc}{nope}{Chu}.\n";
        auto result = Thoughts::authorProposal(root, revise);
        QCOMPARE(result["error_code"].toString(), "invalid_link");
        QCOMPARE(result["links"].toArray().first().toObject()["suggestion"].toString(), "\\PdfLink{paper_abc}{document-start}{Chu}");
        QCOMPARE(get(grpo), before);
        revise["request_id"] = QUuid::createUuid().toString(QUuid::WithoutBraces);
        revise["replacement"] = "A claim \\PdfLink{paper_abc}{document-start}{Chu}.\n";
        QVERIFY(Thoughts::authorProposal(root, revise)["accepted"].toBool());
        const QString other = root + "/docs-concepts/tex/source/other.tex";
        const QByteArray unchanged = get(other);
        auto batch = request(other); batch.remove("replacement"); batch.remove("explanation"); batch["command"] = "create-proposals";
        batch.insert("proposals", QJsonArray{QJsonObject{{"start_line", 1}, {"end_line", 1}, {"replacement", "Fine.\n"}, {"explanation", "Why."}},
                                             QJsonObject{{"start_line", 3}, {"end_line", 3}, {"replacement", "\\PdfLink{own}{missing}{x}\n"}, {"explanation", "Why."}}});
        result = Thoughts::authorProposal(root, batch);
        QCOMPARE(result["error_code"].toString(), "invalid_link");
        QCOMPARE(result["links"].toArray().first().toObject()["proposal_index"].toInt(), 1);
        QCOMPARE(get(other), unchanged);
    }
    // 2026-11-09: a thoughts-html: link must name an HTML page of the note's own folder (thoughts-server
    // protocol/html-pages.md 3): a file there, or one its page list names that Sync has not installed yet.
    void htmlPageLinksMustNameAPageOfTheFolder() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        writeLinkLibrary(root);
        const QString grpo = root + "/docs-concepts/tex/source/grpo.tex";
        const QString notes = root + "/docs-concepts/markdown/notes.md";
        const QByteArray before = get(grpo);
        const QString page = "docs-concepts/tex/support/html/irt-middle-slope.html";
        auto result = create(root, grpo, 2, 3, "See \\href{thoughts-html:" + page + "}{the curve}.\n");
        QCOMPARE(result["error_code"].toString(), "invalid_link");
        QCOMPARE(get(grpo), before);
        auto link = result["links"].toArray().first().toObject();
        QCOMPARE(link["reason"].toString(), "html_page_not_found");
        QCOMPARE(link["command"].toString(), "thoughts-html");
        QCOMPARE(link["text"].toString(), "thoughts-html:" + page);
        // A page of another folder: refused, with this folder's page of the same name once it has one.
        QVERIFY(QDir(root).mkpath("docs-concepts/tex/support/html"));
        put(root + "/" + page, "<!doctype html><p>curve");
        result = create(root, grpo, 2, 3, "\\href{thoughts-html:docs-followup/tex/support/html/irt-middle-slope.html}{x}\n");
        link = result["links"].toArray().first().toObject();
        QCOMPARE(link["reason"].toString(), "html_page_other_folder");
        QCOMPARE(link["suggestion"].toString(), "thoughts-html:" + page);
        // The page exists: accepted, in LaTeX and in Markdown.
        result = create(root, grpo, 2, 3, "See \\href{thoughts-html:" + page + "}{the curve}.\n");
        QVERIFY2(result["accepted"].toBool(), qPrintable(QJsonDocument(result).toJson()));
        QVERIFY(create(root, notes, 3, 4, "[Explore](thoughts-html:" + page + ")\n")["accepted"].toBool());
        // A page the folder's list names but Sync has not installed yet exists too.
        const QString listed = "docs-concepts/tex/support/html/imported.html";
        put(root + "/docs-concepts/tex/support/thoughts_html_attachments.txt",
            "{\"attachments\": [{\"bytes\": 3, \"kind\": \"html_attachment\", \"object\": \"" + QByteArray(64, 'a') + "\", \"path\": \"" + listed.toUtf8()
                + "\", \"sha256\": \"" + QByteArray(64, 'a') + "\"}], \"schema_version\": 1}\n");
        QVERIFY(create(root, grpo, 1, 2, "\\href{thoughts-html:" + listed + "}{imported}\n")["accepted"].toBool());
        // A link already in the source is not checked again.
        const QByteArray broken = "Intro.\n\\href{thoughts-html:docs-concepts/tex/support/html/gone.html}{gone}\nEnd.\n";
        put(grpo, broken);
        QVERIFY(create(root, grpo, 3, 4, "Changed.\n")["accepted"].toBool());
    }
    void markdownLinksMustResolve() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        writeLinkLibrary(root);
        const QString notes = root + "/docs-concepts/markdown/notes.md";
        auto result = create(root, notes, 3, 4, "See [the target](target.md#source-label:here) and [web](https://example.org) and `[code](nowhere.md)`.\n");
        QVERIFY2(result["accepted"].toBool(), qPrintable(QJsonDocument(result).toJson()));
        const QByteArray before = get(notes);
        result = create(root, notes, 1, 2, "# Notes [gone](missing.md)\n");
        QCOMPARE(result["links"].toArray().first().toObject()["reason"].toString(), "link_target_not_found");
        QCOMPARE(create(root, notes, 1, 2, "# Notes [t](target.md#source-label:nowhere)\n")["links"].toArray().first().toObject()["reason"].toString(), "label_not_found");
        QCOMPARE(create(root, notes, 1, 2, "![img](figure.png)\n")["links"].toArray().first().toObject()["reason"].toString(), "image_not_found");
        QCOMPARE(get(notes), before);
    }
    // 2026-10-19: relative Markdown links make the four LaTeX document links
    // (\SourceLink, \CrossSourceLink, \PdfLink, \CrossPdfLink) and are checked
    // as those commands, with problems and suggestions written in Markdown.
    // protocol/agent-proposals.md §12: a card linked to source markers moves as two
    // proposals in one write; either decision decides both, in both files.
    void movesALinkedCardAcrossSourcesAsOnePair() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("docs-a/tex/source"));
        QVERIFY(QDir(root.path()).mkpath("docs-b/tex/source"));
        const QString a = root.filePath("docs-a/tex/source/a.tex"), b = root.filePath("docs-b/tex/source/b.tex");
        const QByteArray originalA = marked("docs-a/tex/source/a.tex", "\\documentclass{article}\n\\begin{document}\nIntro.\n\nThe answer.\n\nOutro.\n\\end{document}\n", 5);
        QVERIFY2(originalA.contains("ReviewCardMaskStart"), originalA.constData());
        const QByteArray originalB = "\\documentclass{article}\n\\begin{document}\nFirst.\n\nSecond.\n\\end{document}\n";
        put(a, originalA); put(b, originalB);
        // An older server would decide one half alone: nothing is written.
        auto unsupported = movable("docs-a/tex/source/a.tex"); unsupported.movePairsSupported = false;
        QCOMPARE(Thoughts::authorCardMove(root.path(), moveRequest(b, 5), "mcp", unsupported)["error_code"].toString(), "server_unsupported");
        QCOMPARE(get(a), originalA); QCOMPARE(get(b), originalB);

        const auto req = moveRequest(b, 5);
        const auto moved = Thoughts::authorCardMove(root.path(), req, "mcp", movable("docs-a/tex/source/a.tex"));
        QVERIFY2(moved["accepted"].toBool(), qPrintable(QJsonDocument(moved).toJson()));
        QCOMPARE(moved["state"].toString(), "move_proposed");
        const auto proposals = moved["proposals"].toArray();
        const QString cut = proposals[0].toObject()["proposal_id"].toString(), paste = proposals[1].toObject()["proposal_id"].toString();
        QVERIFY(cut.endsWith(".cut")); QVERIFY(paste.endsWith(".paste"));
        QCOMPARE(Thoughts::AgentProposalMove::partner(cut), paste);
        QCOMPARE(Thoughts::AgentProposalMove::partner(paste), cut);
        QCOMPARE(moved["source_sha256"].toString(), Thoughts::ProposalHistory::digest(get(b)));
        QCOMPARE(moved["origin_source_sha256"].toString(), Thoughts::ProposalHistory::digest(get(a)));
        const QString passage = proposals[0].toObject()["original_text"].toString();
        QVERIFY(passage.startsWith("\\ReviewCardMaskStart{" + QString(Card) + "}{0}%\n"));
        QVERIFY(passage.endsWith("\\ReviewCardMaskEnd{" + QString(Card) + "}{0}%\n"));
        const auto parsedA = Thoughts::parseAgentProposals(a, QString::fromUtf8(get(a)));
        const auto parsedB = Thoughts::parseAgentProposals(b, QString::fromUtf8(get(b)));
        QVERIFY(parsedA.proposal(cut) && parsedB.proposal(paste));
        QCOMPARE(parsedA.proposal(cut)->original, passage);
        QCOMPARE(parsedA.proposal(cut)->replacement, QString());
        QCOMPARE(parsedB.proposal(paste)->original, QString());
        QCOMPARE(parsedB.proposal(paste)->replacement, passage);
        QVERIFY(parsedB.proposal(paste)->explanation.contains("docs-a/tex/source/a.tex"));
        // The explanations name the card's question, never the other half's ID (§10.2 vanish rule).
        QVERIFY(!get(b).contains(cut.toUtf8())); QVERIFY(!get(a).contains(paste.toUtf8()));
        QVERIFY(parsedA.proposal(cut)->explanation.contains("What is the answer?"));
        // A retry replays; the same ID with other arguments is refused.
        QVERIFY(Thoughts::authorCardMove(root.path(), req, "mcp", movable("docs-a/tex/source/a.tex"))["replayed"].toBool());
        auto conflict = req; conflict["explanation"] = "Something else.";
        QCOMPARE(Thoughts::authorCardMove(root.path(), conflict, "mcp", movable("docs-a/tex/source/a.tex"))["error_code"].toString(), "request_conflict");
        // A half cannot be revised.
        auto revise = request(b, false); revise.insert("proposal_id", paste);
        QCOMPARE(Thoughts::authorProposal(root.path(), revise)["error_code"].toString(), "move_proposal");
        const QByteArray pendingA = get(a), pendingB = get(b);

        // Accept the removal: the insertion is accepted with it, in its own file.
        QByteArray previous; QString after, error, code;
        QList<Thoughts::AgentProposalSourceChange> others;
        QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(root.path(), a, Thoughts::ProposalHistory::digest(pendingA), {cut}, "accept",
                                                         &error, &previous, &after, &code, {}, &others), qPrintable(error));
        QCOMPARE(others.size(), 1);
        QCOMPARE(others[0].path, QFileInfo(b).canonicalFilePath());
        QVERIFY(!get(a).contains(Card));
        QVERIFY(!get(a).contains("agentProposedEdit"));
        QVERIFY(!get(b).contains("agentProposedEdit"));
        const auto anchor = Thoughts::readReviewSourceAnchor("docs-b/tex/source/b.tex", get(b), Card);
        QVERIFY2(anchor.error.isEmpty(), qPrintable(anchor.error));
        QCOMPARE(get(b), QByteArray("\\documentclass{article}\n\\begin{document}\nFirst.\n\n") + passage.toUtf8() + "Second.\n\\end{document}\n");
        // Undo puts both halves back.
        QVERIFY2(Thoughts::restoreLiveAgentProposalSources(root.path(), a, after, previous, others, &error), qPrintable(error));
        QCOMPARE(get(a), pendingA); QCOMPARE(get(b), pendingB);

        // Reject the insertion: both files are as they were before the move.
        QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(root.path(), b, Thoughts::ProposalHistory::digest(pendingB), {paste}, "disregard",
                                                         &error, nullptr, nullptr, &code), qPrintable(error));
        QCOMPARE(get(a), originalA); QCOMPARE(get(b), originalB);
        QVERIFY(!QDir(Thoughts::ProposalHistory::directory(root.path())).entryList({"proposal_move_pending_*"}).size());

        // Withdrawing one half withdraws the move.
        const auto again = Thoughts::authorCardMove(root.path(), moveRequest(b, 3), "mcp", movable("docs-a/tex/source/a.tex"));
        QVERIFY2(again["accepted"].toBool(), qPrintable(QJsonDocument(again).toJson()));
        auto withdraw = request(a, false); withdraw.insert("command", "remove-proposal"); withdraw.remove("replacement");
        withdraw.insert("proposal_id", again["proposals"].toArray()[0].toObject()["proposal_id"]);
        const auto withdrawn = Thoughts::authorProposal(root.path(), withdraw, "mcp");
        QVERIFY2(withdrawn["accepted"].toBool(), qPrintable(QJsonDocument(withdrawn).toJson()));
        QCOMPARE(withdrawn["withdrawn_proposal_ids"].toArray().size(), 2);
        QCOMPARE(get(a), originalA); QCOMPARE(get(b), originalB);
    }
    // A lone half (its partner gone outside Thoughts) is never decided alone.
    void loneHalfIsNotDecided() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("docs-a"));
        const QString a = root.filePath("docs-a/a.tex"), b = root.filePath("docs-a/b.tex");
        put(a, marked("docs-a/a.tex", "Intro.\n\nThe answer.\n\nOutro.\n", 3));
        put(b, "First.\n\nSecond.\n");
        const auto moved = Thoughts::authorCardMove(root.path(), moveRequest(b, 3), "mcp", movable("docs-a/a.tex"));
        QVERIFY2(moved["accepted"].toBool(), qPrintable(QJsonDocument(moved).toJson()));
        const QString cut = moved["proposals"].toArray()[0].toObject()["proposal_id"].toString();
        const QByteArray pendingA = get(a), pendingB = get(b);
        put(b, "First.\n\nSecond.\n");
        QString error, code;
        for (const QString action : {"accept", "disregard"}) {
            QVERIFY(!Thoughts::resolveLiveAgentProposedEdits(root.path(), a, Thoughts::ProposalHistory::digest(pendingA), {cut}, action, &error, nullptr, nullptr, &code));
            QCOMPARE(code, "move_incomplete");
            QCOMPARE(get(a), pendingA);
        }
        // Pending in two files (a copy): refused as well.
        put(b, pendingB);
        put(root.filePath("docs-a/copy.tex"), pendingB);
        QVERIFY(!Thoughts::resolveLiveAgentProposedEdits(root.path(), a, Thoughts::ProposalHistory::digest(pendingA), {cut}, "accept", &error, nullptr, nullptr, &code));
        QCOMPARE(code, "move_ambiguous");
        QCOMPARE(get(a), pendingA);
        // A copy in a hidden folder (a staging snapshot) is not a source.
        QVERIFY(QFile::remove(root.filePath("docs-a/copy.tex")));
        QVERIFY(QDir(root.path()).mkpath(".stage/docs-a"));
        put(root.filePath(".stage/docs-a/b.tex"), pendingB);
        QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(root.path(), a, Thoughts::ProposalHistory::digest(pendingA), {cut}, "accept", &error, nullptr, nullptr, &code),
                 qPrintable(error));
        QVERIFY(get(b).contains(Card));
        QCOMPARE(get(root.filePath(".stage/docs-a/b.tex")), pendingB);
    }
    void movesWithinOneSourceAndInMarkdown_data() {
        QTest::addColumn<QString>("name"); QTest::addColumn<QByteArray>("text"); QTest::addColumn<int>("answer"); QTest::addColumn<int>("line");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("tex down") << "docs-a/a.tex" << QByteArray("Intro.\n\nThe answer.\n\nOutro.\n\nEnd.\n") << 3 << 7 << QByteArray("Intro.\n\n\nOutro.\n\n@End.\n");
        QTest::newRow("tex up") << "docs-a/a.tex" << QByteArray("Intro.\n\nOutro.\n\nThe answer.\n") << 5 << 1 << QByteArray("@Intro.\n\nOutro.\n\n");
        QTest::newRow("markdown") << "docs-a/a.md" << QByteArray("# Title\n\nIntro.\n\nThe answer.\n\nOutro.\n") << 5 << 3 << QByteArray("# Title\n\n@Intro.\n\n\nOutro.\n");
    }
    void movesWithinOneSourceAndInMarkdown() {
        QFETCH(QString, name); QFETCH(QByteArray, text); QFETCH(int, answer); QFETCH(int, line); QFETCH(QByteArray, expected);
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("docs-a"));
        const QString path = root.filePath(name);
        const QByteArray original = marked(name, text, answer);
        QVERIFY2(!original.startsWith("marking failed"), original.constData());
        put(path, original);
        const auto moved = Thoughts::authorCardMove(root.path(), moveRequest(path, line + (line > answer ? 2 : 0)), "mcp", movable(name));
        QVERIFY2(moved["accepted"].toBool(), qPrintable(QJsonDocument(moved).toJson()));
        const QString cut = moved["proposals"].toArray()[0].toObject()["proposal_id"].toString();
        const QString passage = moved["proposals"].toArray()[0].toObject()["original_text"].toString();
        const auto parsed = Thoughts::parseAgentProposals(path, QString::fromUtf8(get(path)));
        QCOMPARE(parsed.proposals.size(), 2);
        const QByteArray pending = get(path);
        QString error, code;
        QList<Thoughts::AgentProposalSourceChange> others;
        QVERIFY2(Thoughts::resolveLiveAgentProposedEdits(root.path(), path, Thoughts::ProposalHistory::digest(pending), {cut}, "accept",
                                                         &error, nullptr, nullptr, &code, {}, &others), qPrintable(error));
        QVERIFY(others.isEmpty());
        QCOMPARE(get(path), QByteArray(expected).replace("@", passage.toUtf8()));
        QVERIFY(Thoughts::readReviewSourceAnchor(name, get(path), Card).error.isEmpty());
    }
    void cardMoveGuards() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("docs-a"));
        const QString a = root.filePath("docs-a/a.tex"), md = root.filePath("docs-a/b.md"), b = root.filePath("docs-a/b.tex");
        const QString other = "7a2d3b1f-4c5e-4d6f-9a70-8293a4b5c6d7";
        QByteArray originalA = marked("docs-a/a.tex", "Intro.\n\nThe answer.\n\nOutro.\n", 3);
        put(a, originalA); put(md, "Text.\n"); put(b, "\\documentclass{article}\n\\begin{document}\nFirst.\n\\end{document}\n");
        const auto refusal = [&](const QJsonObject &req, const Thoughts::CardMoveContext &card = movable("docs-a/a.tex")) {
            return Thoughts::authorCardMove(root.path(), req, "mcp", card)["error_code"].toString();
        };
        QCOMPARE(refusal(moveRequest(md, 1)), "format_mismatch");
        QCOMPARE(refusal(moveRequest(a, 3)), "no_change");
        QCOMPARE(refusal(moveRequest(a, 4)), "invalid_range");
        QCOMPARE(refusal(moveRequest(b, 9)), "invalid_range");
        // The preamble is not rendered: the markers would not read back there.
        QCOMPARE(refusal(moveRequest(b, 2)), "invalid_destination");
        auto stale = moveRequest(b, 3); stale["expected_source_sha256"] = QString(64, '0');
        QCOMPARE(refusal(stale), "source_conflict");
        auto pdfOnly = movable("docs-a/a.tex"); pdfOnly.refusal = "pdf_only_card"; pdfOnly.refusalMessage = "Not linked.";
        QCOMPARE(refusal(moveRequest(b, 3), pdfOnly), "pdf_only_card");
        QCOMPARE(refusal(moveRequest(b, 3), movable("docs-a/b.tex")), "invalid_markers");
        // Another card inside the passage moves with it, but never one of its markers alone.
        const QString notes = root.filePath("docs-a/notes.md"), target = root.filePath("docs-a/target.md");
        const QByteArray inner = "<!-- thoughts:v1:start review:" + other.toUtf8() + ":answer:0 -->\nInner.\n<!-- thoughts:v1:end review:" + other.toUtf8() + ":answer:0 -->\n";
        const QByteArray outerStart = "<!-- thoughts:v1:start review:" + QByteArray(Card) + ":answer:0 -->\n";
        const QByteArray outerEnd = "<!-- thoughts:v1:end review:" + QByteArray(Card) + ":answer:0 -->\n";
        put(notes, "Intro.\n\n" + outerStart + "Outer.\n\n" + inner.left(inner.indexOf("Inner.")) + "Inner.\n" + outerEnd + inner.mid(inner.indexOf("Inner.") + 7) + "\nOutro.\n");
        put(target, "Target.\n");
        QCOMPARE(refusal(moveRequest(target, 2), movable("docs-a/notes.md")), "split_card");
        put(notes, "Intro.\n\n" + outerStart + "Outer.\n\n" + inner + outerEnd + "\nOutro.\n");
        const auto nested = Thoughts::authorCardMove(root.path(), moveRequest(target, 2), "mcp", movable("docs-a/notes.md"));
        QVERIFY2(nested["accepted"].toBool(), qPrintable(QJsonDocument(nested).toJson()));
        QCOMPARE(nested["also_moved_card_ids"].toArray(), QJsonArray{other});
        // Markers inside a pending proposal's text: the move waits for that proposal.
        const QString pending = root.filePath("docs-a/pending.md");
        put(pending, "Intro.\n\n<!-- thoughts:agent-proposal:v2:start\nid: p1\ncreated_at: 2026-10-10T12:00:00.000Z\n"
                     "conversation_id: 0b6c5b7e-1d2a-4c3b-9f8e-112233445566\nexplanation:\n| Why.\noriginal_text:\n| Old.\n| \n-->\n"
                     + outerStart + "New.\n" + outerEnd + "<!-- thoughts:agent-proposal:v2:end p1 -->\n");
        QCOMPARE(refusal(moveRequest(target, 1), movable("docs-a/pending.md")), "proposal_overlap");
        // A passage inside a pending proposal waits for it.
        const QByteArray wrapped = "\\agentProposedEdit[created=2026-10-10T12:00:00.000Z,conversation_id=0b6c5b7e-1d2a-4c3b-9f8e-112233445566]{p1}{Why.\n}{"
                                   + originalA + "}{" + originalA + "}";
        put(a, wrapped);
        QCOMPARE(refusal(moveRequest(b, 3)), "invalid_markers");
        put(a, originalA);
        QCOMPARE(get(b), QByteArray("\\documentclass{article}\n\\begin{document}\nFirst.\n\\end{document}\n"));
        QVERIFY(Thoughts::authorCardMove(root.path(), moveRequest(b, 3), "mcp", movable("docs-a/a.tex"))["accepted"].toBool());
    }
    // A move written to one file before a crash is completed from its journal.
    void interruptedMoveIsRolledForward() {
        QTemporaryDir root;
        QVERIFY(QDir(root.path()).mkpath("docs-a"));
        const QString a = QFileInfo(root.filePath("docs-a")).canonicalFilePath() + "/a.tex", b = QFileInfo(root.filePath("docs-a")).canonicalFilePath() + "/b.tex";
        put(a, "A after.\n"); put(b, "B before.\n");
        const auto journal = [&](const QByteArray &bBefore) {
            QVERIFY(QDir().mkpath(Thoughts::ProposalHistory::directory(root.path())));
            const QJsonArray sources{
                QJsonObject{{"source_path", a}, {"before_sha256", Thoughts::ProposalHistory::digest("A before.\n")},
                            {"after_sha256", Thoughts::ProposalHistory::digest("A after.\n")}, {"after", QString::fromLatin1(QByteArray("A after.\n").toBase64())}},
                QJsonObject{{"source_path", b}, {"before_sha256", Thoughts::ProposalHistory::digest(bBefore)},
                            {"after_sha256", Thoughts::ProposalHistory::digest("B after.\n")}, {"after", QString::fromLatin1(QByteArray("B after.\n").toBase64())}}};
            put(QDir(Thoughts::ProposalHistory::directory(root.path())).filePath("proposal_move_pending_1.json"),
                QJsonDocument(QJsonObject{{"sources", sources}, {"events", QJsonArray{}}}).toJson(QJsonDocument::Compact));
        };
        journal("B before.\n");
        QString error;
        QVERIFY2(Thoughts::ProposalHistory::recover(root.path(), &error), qPrintable(error));
        QCOMPARE(get(b), QByteArray("B after.\n"));
        QVERIFY(QDir(Thoughts::ProposalHistory::directory(root.path())).entryList({"proposal_move_pending_*"}).isEmpty());
        // Changed by someone else since: their text stays and the journal goes.
        put(a, "A after.\n"); put(b, "B edited by hand.\n");
        journal("B before.\n");
        QVERIFY2(Thoughts::ProposalHistory::recover(root.path(), &error), qPrintable(error));
        QCOMPARE(get(b), QByteArray("B edited by hand.\n"));
        QVERIFY(QDir(Thoughts::ProposalHistory::directory(root.path())).entryList({"proposal_move_pending_*"}).isEmpty());
    }
    void markdownDocumentLinksAreCheckedAsLatexLinks() {
        QTemporaryDir directory;
        const QString root = QFileInfo(directory.path()).canonicalFilePath();
        writeLinkLibrary(root);
        const QString notes = root + "/docs-concepts/markdown/notes.md";
        auto result = create(root, notes, 3, 4,
                             "[p](own.pdf#hyp) [p0](own.pdf) [x](../../docs-followup/paper_abc.pdf#intro) "
                             "[x0](../../docs-followup/paper_abc.pdf) [b](paper_abc.pdf#intro) [m](../../docs-moved/paper_abc.pdf) "
                             "[s](../tex/source/other.tex#source-label:known) "
                             "[t](target.md) [c](../../docs-followup/tex/source/main.tex) [self](notes.md)\n");
        QVERIFY2(result["accepted"].toBool(), qPrintable(QJsonDocument(result).toJson()));
        const QByteArray before = get(notes);
        const auto first = [&](const QString &text) { return create(root, notes, 1, 2, "# Notes " + text + "\n")["links"].toArray().first().toObject(); };
        auto problem = first("[p](paper_abc.pdf#nope)");
        QCOMPARE(problem["reason"].toString(), "anchor_not_found");
        QCOMPARE(problem["suggestion"].toString(), "[p](paper_abc.pdf)");
        QVERIFY2(problem["message"].toString().startsWith("[p](paper_abc.pdf#nope): "), qPrintable(problem["message"].toString()));
        problem = first("[n](paper_abcd.pdf)");
        QCOMPARE(problem["reason"].toString(), "pdf_not_in_library");
        QVERIFY2(problem["message"].toString().contains("similar PDF keys: paper_abc"), qPrintable(problem["message"].toString()));
        problem = first("[k](../../docs-followup/Paper_abc.pdf)");
        QCOMPARE(problem["reason"].toString(), "pdf_not_in_library");
        QCOMPARE(problem["suggestion"].toString(), "[k](paper_abc.pdf)");
        problem = first("[a](own.pdf#nope)");
        QCOMPARE(problem["reason"].toString(), "anchor_not_found");
        QCOMPARE(problem["suggestion"].toString(), "[a](own.pdf)");
        QCOMPARE(first("[u](unselected.pdf)")["reason"].toString(), "pdf_not_selected");
        QCOMPARE(first("[x](../../docs-followup/paper_abc.pdf#nope)")["suggestion"].toString(), "[x](../../docs-followup/paper_abc.pdf)");
        QCOMPARE(first("[c](../../docs-followup/tex/source/main.tex#source-label:none)")["reason"].toString(), "label_not_found");
        problem = first("[h](target.md#heading)");
        QCOMPARE(problem["reason"].toString(), "fragment_not_source_label");
        QCOMPARE(problem["suggestion"].toString(), "[h](target.md)");
        QCOMPARE(first("[o](../../docs-followup/data.csv)")["reason"].toString(), "invalid_markdown_link");
        QCOMPARE(first("[k](my%20paper.pdf)")["reason"].toString(), "invalid_markdown_link");
        QCOMPARE(first("[z](../../docs-nowhere/x.md)")["reason"].toString(), "unknown_project_folder");
        QCOMPARE(first("[self](notes.md#source-label:none)")["reason"].toString(), "label_not_found");
        QCOMPARE(first("[here](#source-label:none)")["reason"].toString(), "label_not_found");
        QCOMPARE(get(notes), before);
        QVERIFY(create(root, notes, 1, 2, "# Notes {#source-label:top}\n\n[here](#source-label:top) [self](notes.md#source-label:top)\n")["accepted"].toBool());
    }
    // ---------------------------------------------------------------- beside review card markers
    // A card marker line stays alone while a proposal is pending; accepted or rejected, every
    // proposal reads exactly as the plain whole-line wrapper did. The same cases are thoughts-cli's
    // (clients/thoughts-cli/tests/test_card_marker_lines.py in thoughts-server).
private:
    struct Edit { int start; int end; QString replacement; };
    static constexpr const char *MarkedCard = "3c9d2e41-7a5b-4c6d-8e9f-0a1b2c3d4e5f";
    static constexpr const char *OtherCard = "5e1f3a62-8b7c-4d8e-9f0a-1b2c3d4e5f60";
    static QString cardStart(const QString &card = MarkedCard, int segment = 0) { return QStringLiteral("\\ReviewCardMaskStart{%1}{%2}%").arg(card).arg(segment); }
    static QString cardEnd(const QString &card = MarkedCard, int segment = 0) { return QStringLiteral("\\ReviewCardMaskEnd{%1}{%2}%").arg(card).arg(segment); }
    // Lines:  1 Opening line.  2 start  3 First claim.  4 Second claim.  5 end  6 Closing line.
    static QString note() { return "Opening line.\n" + cardStart() + "\nFirst claim.\nSecond claim.\n" + cardEnd() + "\nClosing line.\n"; }
    // Two cards back to back (1-5, 6-8), then a commented line and a second segment's end (12).
    static QString notes() {
        return "Opening line.\n" + cardStart() + "\nFirst claim.\nSecond claim.\n" + cardEnd() + "\n" + cardStart(OtherCard) + "\nOther answer.\n"
            + cardEnd(OtherCard) + "\nPlain line.\nKeep this%\nLast claim.\n" + cardEnd(OtherCard, 1) + "\n";
    }
    static bool markerLinesAlone(const QString &text) {
        static const QRegularExpression marker(R"(\A[ \t]*\\ReviewCardMask(?:Start|End)\{[^{}\n]*\}\{[^{}\n]*\}%?[ \t]*\r?\z)");
        for (const QString &line : text.split('\n'))
            if (line.contains("\\ReviewCardMask") && !marker.match(line).hasMatch()) return false;
        return true;
    }
    static QJsonObject createEdits(const QString &root, const QString &path, const QList<Edit> &edits) {
        QJsonArray proposals;
        for (int i = 0; i < edits.size(); ++i)
            proposals.append(QJsonObject{{"start_line", edits[i].start}, {"end_line", edits[i].end}, {"replacement", edits[i].replacement},
                                         {"explanation", QStringLiteral("Edit %1.").arg(i + 1)}});
        if (edits.size() == 1) {
            auto req = request(path); req.insert("start_line", edits[0].start); req.insert("end_line", edits[0].end);
            req["replacement"] = edits[0].replacement; req["explanation"] = "Edit 1.";
            return Thoughts::authorProposal(root, req);
        }
        return Thoughts::authorProposal(root, QJsonObject{{"command", "create-proposals"}, {"source", path}, {"expected_source_sha256", Thoughts::ProposalHistory::digest(get(path))},
                                                          {"conversation_id", Conversation}, {"request_id", QUuid::createUuid().toString(QUuid::WithoutBraces)}, {"proposals", proposals}});
    }
    static QStringList createdIds(const QJsonObject &result) {
        if (!result.contains("proposals")) return {result["proposal_id"].toString()};
        QStringList ids;
        for (const auto &value : result["proposals"].toArray()) ids << value.toObject()["proposal_id"].toString();
        return ids;
    }
    // The source as the writer wrote it before 2026-11-13: each wrapper replaces its whole lines.
    static QString plainLayout(const QString &text, const QList<Edit> &edits, const QJsonObject &result) {
        const QStringList ids = createdIds(result);
        QVector<int> offsets {0};
        for (const QString &line : text.split('\n')) offsets.append(offsets.last() + int(line.size()) + 1);
        QList<int> order(edits.size()); std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](int a, int b) { return std::pair(edits[a].start, edits[a].end) < std::pair(edits[b].start, edits[b].end); });
        QString output; int cursor = 0;
        for (const int i : order) {
            const int low = offsets[edits[i].start - 1], high = offsets[edits[i].end - 1];
            Thoughts::AgentProposedEdit proposal;
            proposal.id = ids[i]; proposal.createdAt = result["created_at"].toString().isEmpty() ? result["proposals"].toArray()[0].toObject()["created_at"].toString() : result["created_at"].toString();
            proposal.conversationId = Conversation; proposal.explanation = QStringLiteral("Edit %1.").arg(i + 1);
            proposal.original = text.mid(low, high - low); proposal.replacement = edits[i].replacement;
            QString refusal;
            output += text.mid(cursor, low - cursor) + Thoughts::formatLatexAgentProposal(proposal, &refusal);
            cursor = high;
        }
        return output + text.mid(cursor);
    }
    // Every combination of decisions reads exactly as with the plain layout, and pending ones keep every marker alone.
    static void verifySameDecisions(const QString &path, const QString &text, const QList<Edit> &edits, const QJsonObject &result) {
        QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
        const QString updated = QString::fromUtf8(get(path));
        const QString before = plainLayout(text, edits, result);
        const QStringList ids = createdIds(result);
        QCOMPARE(Thoughts::parseAgentProposals(path, updated).proposals.size(), ids.size());
        int combinations = 1; for (int i = 0; i < ids.size(); ++i) combinations *= 3;
        for (int combination = 0; combination < combinations; ++combination) {
            QString now = updated, old = before; bool complete = true;
            for (int i = 0, code = combination; i < ids.size(); ++i, code /= 3) {
                if (code % 3 == 2) { complete = false; continue; }
                const QString action = code % 3 == 0 ? "accept" : "reject";
                now = decide(path, now, {ids[i]}, action); old = decide(path, old, {ids[i]}, action);
            }
            if (complete) QCOMPARE(now, old);
            QVERIFY2(markerLinesAlone(now), qPrintable(now));
        }
        QCOMPARE(decide(path, updated, ids, "reject"), text);
    }
private Q_SLOTS:
    void cardMarkerLinesLastLines() {
        QTemporaryDir root; const auto path = root.filePath("topic.tex");
        // The last line of a card, then its last two lines.
        for (const auto &edit : {Edit{4, 5, "Sharper second claim.\n"}, Edit{3, 5, "One merged claim.\nWith a second line.\n"}}) {
            put(path, note().toUtf8());
            const auto result = createEdits(root.path(), path, {edit});
            verifySameDecisions(path, note(), {edit}, result);
            const QString updated = QString::fromUtf8(get(path));
            const auto proposal = Thoughts::parseAgentProposals(path, updated).proposals.first();
            QCOMPARE(proposal.original + "\n", note().split('\n').mid(edit.start - 1, edit.end - edit.start).join('\n') + "\n");
            QCOMPARE(proposal.replacement + "\n", edit.replacement);
            QVERIFY(updated.contains(proposal.replacement + "}\n" + cardEnd() + "\nClosing line.\n"));
            // The plain layout put the closing brace on the marker's line.
            QVERIFY(!markerLinesAlone(plainLayout(note(), {edit}, result)));
            // The agent's view stays whole lines; the history keeps the wrapper's own texts.
            QCOMPARE(result["original_text"].toString(), proposal.original + "\n");
            QCOMPARE(result["replacement"].toString(), edit.replacement);
            QHash<QString, QJsonObject> current; QVERIFY(Thoughts::ProposalHistory::latest(root.path(), &current));
            QCOMPARE(current.value(proposal.id)["snapshot"].toObject()["before"].toString(), proposal.original);
            QCOMPARE(current.value(proposal.id)["snapshot"].toObject()["after"].toString(), proposal.replacement);
            // Every marker line is one that the Mac's card tools read as a marker.
            static const QRegularExpression strict(R"(^\\ReviewCardMask(Start|End)\{([^}]+)\}\{([0-9]+)\}%$)");
            for (const QString &line : updated.split('\n'))
                if (line.contains("ReviewCardMask")) QVERIFY2(strict.match(line).hasMatch(), qPrintable(line));
        }
    }
    void cardMarkerLinesDeletionAndInsertion() {
        QTemporaryDir root; const auto path = root.filePath("topic.tex");
        put(path, note().toUtf8());
        const auto deleted = createEdits(root.path(), path, {{4, 5, ""}});
        verifySameDecisions(path, note(), {{4, 5, ""}}, deleted);
        QVERIFY(get(path).startsWith(("Opening line.\n" + cardStart() + "\nFirst claim.\\agentProposedEdit[").toUtf8()));
        QVERIFY(get(path).endsWith(("}{\nSecond claim.}{}\n" + cardEnd() + "\nClosing line.\n").toUtf8()));
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {deleted["proposal_id"].toString()}, "accept"),
                 "Opening line.\n" + cardStart() + "\nFirst claim.\n" + cardEnd() + "\nClosing line.\n");
        QCOMPARE(deleted["original_text"].toString(), "Second claim.\n");
        QCOMPARE(deleted["line"].toInt(), 3);
        put(path, note().toUtf8());
        const auto inserted = createEdits(root.path(), path, {{5, 5, "Third claim.\n"}});
        verifySameDecisions(path, note(), {{5, 5, "Third claim.\n"}}, inserted);
        QVERIFY(get(path).endsWith(("Second claim.\\agentProposedEdit[created=" + inserted["created_at"].toString() + ",conversation_id=" + Conversation + "]{"
                                    + inserted["proposal_id"].toString() + "}{Edit 1.\n}{}{\nThird claim.}\n" + cardEnd() + "\nClosing line.\n").toUtf8()));
    }
    void cardMarkerLinesPlainLayoutWhereNoMarkerFollows() {
        QTemporaryDir root; const auto path = root.filePath("topic.tex");
        // Just after a start marker.
        put(path, note().toUtf8());
        auto result = createEdits(root.path(), path, {{3, 4, "Sharper first claim.\n"}});
        verifySameDecisions(path, note(), {{3, 4, "Sharper first claim.\n"}}, result);
        QCOMPARE(QString::fromUtf8(get(path)), plainLayout(note(), {{3, 4, "Sharper first claim.\n"}}, result));
        // Right before a start marker.
        const QString text = "Lead-in sentence.\n" + cardStart() + "\nThe answer.\n" + cardEnd() + "\n";
        put(path, text.toUtf8());
        result = createEdits(root.path(), path, {{1, 2, "A better lead-in.\n"}});
        verifySameDecisions(path, text, {{1, 2, "A better lead-in.\n"}}, result);
        // The card after the proposal stays readable on the Mac; the plain layout made it unavailable.
        QCOMPARE(Thoughts::readReviewSourceAnchor("topic.tex", get(path), MarkedCard).error, QString());
        QVERIFY(!Thoughts::readReviewSourceAnchor("topic.tex", plainLayout(text, {{1, 2, "A better lead-in.\n"}}, result).toUtf8(), MarkedCard).error.isEmpty());
        QVERIFY(QString::fromUtf8(get(path)).endsWith("{Lead-in sentence.}{A better lead-in.}\n" + cardStart() + "\nThe answer.\n" + cardEnd() + "\n"));
        // A range that starts on a marker keeps today's layout: the marker goes into the wrapper.
        put(path, note().toUtf8());
        result = createEdits(root.path(), path, {{2, 4, cardStart() + "\nNew first claim.\n"}});
        QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
        QCOMPARE(QString::fromUtf8(get(path)), plainLayout(note(), {{2, 4, cardStart() + "\nNew first claim.\n"}}, result));
        // Markdown proposals keep their own marker lines.
        const auto md = root.filePath("topic.md");
        const QByteArray markdown = "Intro.\n<!-- thoughts:v1:start review:card:answer:0 -->\nAnswer.\n<!-- thoughts:v1:end review:card:answer:0 -->\n";
        put(md, markdown);
        result = createEdits(root.path(), md, {{3, 4, "Better answer.\n"}});
        QVERIFY2(!result.contains("error_code"), qPrintable(QJsonDocument(result).toJson()));
        QVERIFY(get(md).endsWith(" -->\n<!-- thoughts:v1:end review:card:answer:0 -->\n"));
    }
    void cardMarkerLinesRefusals() {
        QTemporaryDir root; const auto path = root.filePath("topic.tex");
        const auto refusal = [&](const QString &text, const QList<Edit> &edits) {
            put(path, text.toUtf8());
            const auto result = createEdits(root.path(), path, edits);
            [&] { QCOMPARE(get(path), text.toUtf8()); }();
            return result;
        };
        // A card with a single line: its replacement is fine, deleting it would empty the passage.
        const QString single = "Before.\n" + cardStart() + "\nOnly line.\n" + cardEnd() + "\nAfter.\n";
        put(path, single.toUtf8());
        verifySameDecisions(path, single, {{3, 4, "Rewritten only line.\n"}}, createEdits(root.path(), path, {{3, 4, "Rewritten only line.\n"}}));
        auto refused = refusal(single, {{3, 4, ""}});
        QCOMPARE(refused["error_code"].toString(), "card_marker_line");
        QCOMPARE(refused["line"].toInt(), 4);
        QVERIFY(refused["message"].toString().contains("Keep at least one"));
        // The last line of the range ends in a comment: the closing brace cannot follow it.
        const QString commented = "Before.\n" + cardStart() + "\nFirst claim.\nSecond claim.%\n" + cardEnd() + "\n";
        refused = refusal(commented, {{4, 5, "Sharper.\n"}});
        QCOMPARE(refused["error_code"].toString(), "card_marker_line");
        QVERIFY(refused["message"].toString().contains("end_line 4"));
        QCOMPARE(refusal(commented, {{4, 5, ""}})["error_code"].toString(), "card_marker_line");
        QCOMPARE(refusal(commented, {{5, 5, "Inserted.\n"}})["error_code"].toString(), "card_marker_line");
        put(path, commented.toUtf8());
        verifySameDecisions(path, commented, {{3, 4, "Sharper first claim.\n"}}, createEdits(root.path(), path, {{3, 4, "Sharper first claim.\n"}}));
        QVERIFY(refusal(note(), {{4, 5, "Sharper.%\n"}})["message"].toString().contains("replacement's last line"));
        // A deletion after a line that ends in a comment or a backslash starts where they would hide it.
        const QString before = "Before.\n" + cardStart() + "\nFirst claim.%\nSecond claim.\n" + cardEnd() + "\n";
        refused = refusal(before, {{4, 5, ""}});
        QCOMPARE(refused["error_code"].toString(), "card_marker_line");
        QVERIFY(refused["message"].toString().contains("line 3"));
        put(path, before.toUtf8());
        verifySameDecisions(path, before, {{4, 5, "Sharper.\n"}}, createEdits(root.path(), path, {{4, 5, "Sharper.\n"}}));
        QCOMPARE(refusal("Before.\n" + cardStart() + "\nFirst claim \\\nSecond claim.\n" + cardEnd() + "\n", {{4, 5, ""}})["error_code"].toString(), "card_marker_line");
        // No final line break before the marker.
        QVERIFY(refusal(note(), {{4, 5, "No line break"}})["message"].toString().contains("End the replacement with a line break"));
        QCOMPARE(refusal(note(), {{5, 5, "No line break"}})["error_code"].toString(), "card_marker_line");
        // Between two markers.
        QVERIFY(refusal(notes(), {{6, 6, "Between the cards.\n"}})["message"].toString().contains("Insert the text before line 5 or after line 6"));
        QVERIFY(refusal(notes(), {{2, 6, "Replaced card.\n"}})["message"].toString().contains("Select only lines between card markers"));
    }
    void cardMarkerLinesSeveralProposals() {
        QTemporaryDir root; const auto path = root.filePath("topic.tex");
        const QList<Edit> edits {{11, 12, "Last claim, sharper.\n"}, {3, 4, "Sharper first claim.\n"}, {7, 8, "Other answer, rewritten.\n"},
                                 {4, 5, ""}, {9, 10, "Plain line, rewritten.\n"}};
        put(path, notes().toUtf8());
        const auto result = createEdits(root.path(), path, edits);
        verifySameDecisions(path, notes(), edits, result);
        QCOMPARE(result["proposals"].toArray()[3].toObject()["original_text"].toString(), "Second claim.\n");
        // The deletion of line 4 starts where the edit of line 3 now ends: that edit leaves out its line break too.
        QVERIFY(get(path).contains("{First claim.}{Sharper first claim.}\\agentProposedEdit"));
        QVERIFY(get(path).contains(("{\nSecond claim.}{}\n" + cardEnd()).toUtf8()));
        // Deleting a single-line card's line in a batch is refused like one deletion, and writes nothing.
        put(path, notes().toUtf8());
        auto refused = createEdits(root.path(), path, {{3, 4, "Edited.\n"}, {7, 8, ""}});
        QCOMPARE(refused["error_code"].toString(), "card_marker_line");
        QCOMPARE(refused["proposal_index"].toInt(), 1);
        QCOMPARE(get(path), notes().toUtf8());
        // Touching proposals before a marker.
        const QString longer = "Opening line.\n" + cardStart() + "\nFirst claim.\nSecond claim.\nThird claim.\n" + cardEnd() + "\n";
        for (const auto &pair : {QList<Edit>{{4, 5, ""}, {5, 6, ""}}, QList<Edit>{{4, 4, "Inserted.\n"}, {4, 5, ""}}, QList<Edit>{{3, 4, "Edited.\n"}, {6, 6, "Inserted.\n"}}}) {
            put(path, longer.toUtf8());
            verifySameDecisions(path, longer, pair, createEdits(root.path(), path, pair));
        }
        put(path, note().toUtf8());
        refused = createEdits(root.path(), path, {{3, 4, "Joined "}, {4, 5, ""}});
        QCOMPARE(refused["error_code"].toString(), "card_marker_line");
        QVERIFY(refused["message"].toString().contains("Proposals 1 and 2"));
        QCOMPARE(refused["proposal_index"].toInt(), 0);
        QCOMPARE(createEdits(root.path(), path, {{3, 4, ""}, {4, 5, ""}})["error_code"].toString(), "card_marker_line");
    }
    void cardMarkerLinesRevisions() {
        QTemporaryDir root; const auto path = root.filePath("topic.tex");
        const auto revise = [&](const QString &id, const QString &replacement) {
            auto req = request(path, false); req.insert("proposal_id", id); req.insert("replacement", replacement);
            return Thoughts::authorProposal(root.path(), req);
        };
        const QString head = "Opening line.\n" + cardStart() + "\nFirst claim.\n", tail = cardEnd() + "\nClosing line.\n";
        put(path, note().toUtf8());
        const QString id = createEdits(root.path(), path, {{4, 5, "Sharper.\n"}})["proposal_id"].toString();
        QVERIFY(!revise(id, "Sharper still.\n").contains("error_code"));
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {id}, "accept"), head + "Sharper still.\n" + tail);
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {id}, "reject"), note());
        QVERIFY(markerLinesAlone(QString::fromUtf8(get(path))));
        QHash<QString, QJsonObject> current; QVERIFY(Thoughts::ProposalHistory::latest(root.path(), &current));
        QCOMPARE(current.value(id)["snapshot"].toObject()["after"].toString(), "Sharper still.");
        QVERIFY(!revise(id, "As written.").contains("error_code"));
        QCOMPARE(decide(path, QString::fromUtf8(get(path)), {id}, "accept"), head + "As written.\n" + tail);
        const auto refused = revise(id, "");
        QCOMPARE(refused["error_code"].toString(), "card_marker_line");
        QVERIFY(refused["message"].toString().contains("remove_proposal"));
        // A deletion revised into a replacement, again, and back.
        put(path, note().toUtf8());
        const QString deletion = createEdits(root.path(), path, {{4, 5, ""}})["proposal_id"].toString();
        for (const QString &replacement : {QStringLiteral("Instead of deleting.\n"), QStringLiteral("Once more.\n"), QString()}) {
            QVERIFY(!revise(deletion, replacement).contains("error_code"));
            QCOMPARE(decide(path, QString::fromUtf8(get(path)), {deletion}, "accept"), head + replacement + tail);
            QCOMPARE(decide(path, QString::fromUtf8(get(path)), {deletion}, "reject"), note());
            QVERIFY(markerLinesAlone(QString::fromUtf8(get(path))));
        }
        // Other wrappers take the replacement as they always did.
        put(path, note().toUtf8());
        const QString elsewhere = createEdits(root.path(), path, {{3, 4, "Edited.\n"}})["proposal_id"].toString();
        QVERIFY(!revise(elsewhere, "Again.\n").contains("error_code"));
        QVERIFY(get(path).contains("{Again.\n}Second claim."));
    }
};
QTEST_GUILESS_MAIN(ProposalAuthoringTest)
#include "proposalauthoringtest.moc"
