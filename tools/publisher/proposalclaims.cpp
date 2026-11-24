// SPDX-License-Identifier: GPL-2.0-or-later
// Decision claims (protocol/agent-proposals.md §10.4.2) on a source
// publication: Thoughts records a claim for every proposal the user decided
// or an agent withdrew, next to the pending path in desktop-sync.sqlite. The
// publication that uploads the path carries the claim; the server's answer
// expires it. A claim whose wrapper is back in the uploaded text (a local
// Undo) is dropped instead. The Python publisher has no claims; without
// stored claims this adds no request and no state.

#include "proposalclaims.h"

#include "../../shell/proposaldecisions.h"
#include "managedsource.h"
#include "pyio.h"
#include "pyops.h"
#include "sourceinventory.h"
#include "sourcepublisher.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QSet>

#include <functional>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

const QString pendingKey = QStringLiteral("pending_publication_decisions");

Json toJson(const QJsonObject &object)
{
    return Py::loadsText(QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact)));
}

// Whether the server reads claims; nothing when it cannot be asked.
std::optional<bool> serverReadsClaims(SourceApi &api)
{
    try {
        const QJsonObject health = QJsonDocument::fromJson(api.request("GET", QStringLiteral("/v1/health")).body).object();
        return health.value(QStringLiteral("capabilities")).toArray().contains(QJsonValue(QLatin1String(ProposalDecisions::Capability)));
    } catch (const Py::Exception &) {
        return std::nullopt;
    }
}

// What decides whether a stored claim can still travel with an upload.
struct ClaimApplicability {
    QSet<QString> manifestPaths; // the managed source manifest
    QSet<QString> owners; // folders whose authored sources Sync manages
    QSet<QString> recorded; // paths with a recorded Fedora base: a deletion can be published
    QSet<QString> present, deleted; // what this publication read and deletes
};

// Owners of the manifest's Projects and of the Library's Project configurations.
QSet<QString> claimOwners(const Py::Path &root, const Json &manifest)
{
    QSet<QString> owners = sourceOwners(root);
    for (const Json &project : Py::iterate(Py::get(manifest, QStringLiteral("projects"), Json::array()))) {
        const Json owner = Py::get(project, QStringLiteral("project"));
        if (owner.isString()) owners.insert(owner.toString());
    }
    return owners;
}

// Claims no upload can ever carry: their file is not (or no longer) a source
// Sync manages (tex/generated/, .ThoughtIgnore, outside a Project's folders),
// or it is gone locally with no deletion to publish. They are forgotten, so
// they do not wait forever. Returns the claims that remain.
QList<QJsonObject> dropInapplicableClaims(const Py::Path &root, const QString &database, const QList<QJsonObject> &stored, const ClaimApplicability &applicability,
                                          const std::function<void(const QString &)> &report)
{
    QHash<QString, QString> reasons; // path -> why its claims are dropped
    QStringList dropped;
    QList<QJsonObject> kept;
    for (const QJsonObject &claim : stored) {
        const QString path = claim.value(QStringLiteral("source_file")).toString();
        if (!reasons.contains(path)) {
            QString reason;
            try {
                if (!applicability.manifestPaths.contains(path) && !isManagedSourcePath(root, applicability.owners, path)) {
                    reason = QStringLiteral("Sync does not manage this file");
                } else if (!applicability.present.contains(path) && !applicability.deleted.contains(path) && !applicability.recorded.contains(path)
                           && !Py::isFile(root / path)) {
                    reason = QStringLiteral("the file no longer exists and no deletion is pending");
                }
            } catch (const Py::Exception &) {
                reason.clear(); // undecidable now: keep the claim
            }
            reasons.insert(path, reason);
        }
        if (reasons.value(path).isEmpty()) {
            kept.append(claim);
        } else {
            dropped.append(claim.value(QStringLiteral("decision_id")).toString());
        }
    }
    if (dropped.isEmpty()) {
        return stored;
    }
    QString error;
    if (!ProposalDecisions::forgetClaims(database, dropped, &error)) {
        report(QStringLiteral("Proposal decisions that no upload can carry could not be dropped: ") + error);
        return stored;
    }
    QMap<QString, int> counts;
    for (const QJsonObject &claim : stored) {
        const QString path = claim.value(QStringLiteral("source_file")).toString();
        if (!reasons.value(path).isEmpty()) counts[path] += 1;
    }
    for (auto it = counts.cbegin(); it != counts.cend(); ++it) {
        report(QStringLiteral("Dropped %1 proposal decision(s) for %2: %3.").arg(it.value()).arg(it.key(), reasons.value(it.key())));
    }
    return kept;
}

QList<QJsonObject> dropInapplicableClaims(SourcePublisher &publisher, const QList<QJsonObject> &stored, const SourcePublisher::LocalSources &local,
                                          const QList<Json> &mutations)
{
    ClaimApplicability applicability;
    for (const auto &[path, content] : local) {
        if (content) applicability.present.insert(path);
    }
    for (const Json &mutation : mutations) {
        if (Py::subscript(mutation, QStringLiteral("action")) == Json(QStringLiteral("delete")))
            applicability.deleted.insert(Py::subscript(mutation, QStringLiteral("path")).toString());
    }
    for (const auto &[path, base] : Py::get(publisher.state, QStringLiteral("bases"), Json::object()).members()) {
        applicability.recorded.insert(path);
    }
    try {
        for (const QString &path : publisher.paths()) applicability.manifestPaths.insert(path);
        applicability.owners = claimOwners(publisher.root, Py::loadsText(Py::readText(publisher.manifestPath)));
    } catch (const Py::Exception &failure) {
        publisher.report(QStringLiteral("Could not check which proposal decisions still apply; they wait for the next Sync: ") + failure.message);
        return stored;
    }
    return dropInapplicableClaims(publisher.root, proposalClaimDatabase(publisher), stored, applicability, [&publisher](const QString &message) {
        publisher.report(message);
    });
}
}

QString proposalClaimDatabase(const SourcePublisher &publisher)
{
    return (publisher.statePath.parent() / QStringLiteral("desktop-sync.sqlite")).str();
}

std::optional<Json> prepareProposalClaims(SourcePublisher &publisher, const SourcePublisher::LocalSources &local, const QList<Json> &mutations)
{
    const QString database = proposalClaimDatabase(publisher);
    QString error;
    QList<QJsonObject> stored = ProposalDecisions::storedClaims(database, &error);
    if (!error.isEmpty()) {
        publisher.report(QStringLiteral("Proposal decisions are unavailable and wait for the next Sync: ") + error);
        return std::nullopt;
    }
    if (stored.isEmpty()) {
        return std::nullopt;
    }
    stored = dropInapplicableClaims(publisher, stored, local, mutations);
    if (stored.isEmpty()) {
        return std::nullopt;
    }
    QHash<QString, std::optional<QString>> examined;
    for (const auto &[path, content] : local) {
        if (content) {
            examined.insert(path, QString::fromUtf8(*content));
        }
    }
    QHash<QString, std::optional<QString>> uploads;
    for (const Json &mutation : mutations) {
        const QString path = Py::subscript(mutation, QStringLiteral("path")).toString();
        uploads.insert(path, Py::subscript(mutation, QStringLiteral("action")) == Json(QStringLiteral("delete")) ? std::nullopt : examined.value(path));
    }
    // Unchanged paths cannot carry a claim, but a wrapper back in them still
    // means the decision was undone.
    const ProposalDecisions::ClaimSelection unchanged = ProposalDecisions::selectClaimsForUpload(stored, examined);
    const ProposalDecisions::ClaimSelection selection = ProposalDecisions::selectClaimsForUpload(stored, uploads);
    QStringList candidates = selection.dropped;
    for (const QString &id : unchanged.dropped) {
        if (!candidates.contains(id)) candidates.append(id);
    }
    // A decision made while this Sync read its sources is newer than the text it
    // uploads: the claim waits unless the wrapper is still in the file now.
    QHash<QString, QSet<QString>> presentNow;
    QStringList dropped;
    for (const QJsonObject &claim : stored) {
        if (!candidates.contains(claim.value(QStringLiteral("decision_id")).toString())) continue;
        const QString path = claim.value(QStringLiteral("source_file")).toString();
        if (!presentNow.contains(path)) {
            const Py::Path file = publisher.root / path;
            QSet<QString> ids;
            try {
                if (Py::isFile(file)) ids = parseAgentProposals(path, QString::fromUtf8(Py::readBytes(file))).ids();
            } catch (const Py::Exception &) {
                ids.clear(); // unreadable now: keep the claim
            }
            presentNow.insert(path, ids);
        }
        if (presentNow.value(path).contains(claim.value(QStringLiteral("proposal_id")).toString())) dropped.append(claim.value(QStringLiteral("decision_id")).toString());
    }
    if (!dropped.isEmpty()) {
        if (ProposalDecisions::forgetClaims(database, dropped, &error)) {
            publisher.report(QStringLiteral("Dropped %1 proposal decision(s) undone before Sync.").arg(dropped.size()));
        } else {
            publisher.report(QStringLiteral("Undone proposal decisions could not be dropped: ") + error);
        }
    }
    if (selection.sent.isEmpty() || mutations.isEmpty()) {
        return std::nullopt;
    }
    const std::optional<bool> capable = serverReadsClaims(publisher.api);
    if (!capable) {
        // A later upload of the file carries them; the server then matches them late.
        publisher.report(QStringLiteral("Could not ask the server about proposal decisions; %1 decision(s) wait for the next Sync.").arg(selection.sent.size()));
        return std::nullopt;
    }
    Json ids = Json::array();
    for (const QString &id : ProposalDecisions::decisionIds(selection.sent)) {
        ids.append(Json(id));
    }
    publisher.state.insert(pendingKey, ids);
    // An older server ignores claims and classifies exact decisions itself;
    // they expire with its answer all the same.
    if (!*capable) {
        publisher.report(QStringLiteral("The server does not read proposal decisions; %1 decision(s) expire with this publication.").arg(selection.sent.size()));
        return std::nullopt;
    }
    Json claims = Json::array();
    for (const QJsonObject &claim : selection.sent) {
        claims.append(toJson(claim));
    }
    publisher.report(QStringLiteral("Recording %1 proposal decision(s) with this publication.").arg(selection.sent.size()));
    return claims;
}

void expireProposalClaims(SourcePublisher &publisher, const Json &result)
{
    if (!publisher.state.contains(pendingKey)) {
        return;
    }
    QStringList ids;
    for (const Json &id : Py::iterate(publisher.state.value(pendingKey))) {
        if (id.isString()) ids.append(id.toString());
    }
    qint64 matched = 0, other = 0;
    if (result.isObject()) {
        for (const Json &answer : Py::iterate(Py::get(result, QStringLiteral("proposal_decisions"), Json::array()))) {
            const Json outcome = Py::get(answer, QStringLiteral("outcome"));
            (outcome == Json(QStringLiteral("matched")) || outcome == Json(QStringLiteral("matched_late")) ? matched : other) += 1;
        }
    }
    QString error;
    if (!ProposalDecisions::forgetClaims(proposalClaimDatabase(publisher), ids, &error)) {
        publisher.report(QStringLiteral("Answered proposal decisions could not be removed: ") + error);
    } else if (matched || other) {
        publisher.report(QStringLiteral("The server recorded %1 proposal decision(s); %2 needed no change.").arg(matched).arg(other));
    }
    publisher.state.remove(pendingKey);
}

std::optional<Json> applyProposalClaims(SourcePublisher &publisher, const QString &path, const QByteArray &content)
{
    QString error;
    const QList<QJsonObject> claims = ProposalDecisions::claimsForConflictResolution(proposalClaimDatabase(publisher), path, QString::fromUtf8(content), &error);
    if (!error.isEmpty()) {
        publisher.report(QStringLiteral("Proposal decisions for %1 are unavailable and wait for the next Sync: %2").arg(path, error));
        return std::nullopt;
    }
    if (claims.isEmpty()) {
        return std::nullopt;
    }
    const std::optional<bool> capable = serverReadsClaims(publisher.api);
    if (!capable || !*capable) {
        // A later upload of the file carries them; the server then matches them late.
        return std::nullopt;
    }
    Json result = Json::array();
    for (const QJsonObject &claim : claims) {
        result.append(toJson(claim));
    }
    publisher.report(QStringLiteral("Recording %1 proposal decision(s) with the source change %2.").arg(claims.size()).arg(path));
    return result;
}

void expireApplyProposalClaims(SourcePublisher &publisher, const Json &mutation, const Json &result)
{
    QStringList ids;
    for (const Json &claim : Py::iterate(Py::get(mutation, QStringLiteral("proposal_decisions"), Json::array()))) {
        const Json id = Py::get(claim, QStringLiteral("decision_id"));
        if (id.isString()) ids.append(id.toString());
    }
    if (ids.isEmpty() || Py::get(result, QStringLiteral("outcome")) == Json(QStringLiteral("conflict"))) {
        return;
    }
    qint64 matched = 0, other = 0;
    for (const Json &answer : Py::iterate(Py::get(result, QStringLiteral("proposal_decisions"), Json::array()))) {
        const Json outcome = Py::get(answer, QStringLiteral("outcome"));
        (outcome == Json(QStringLiteral("matched")) || outcome == Json(QStringLiteral("matched_late")) ? matched : other) += 1;
    }
    QString error;
    if (!ProposalDecisions::forgetClaims(proposalClaimDatabase(publisher), ids, &error)) {
        publisher.report(QStringLiteral("Answered proposal decisions could not be removed: ") + error);
    } else if (matched || other) {
        publisher.report(QStringLiteral("The server recorded %1 proposal decision(s); %2 needed no change.").arg(matched).arg(other));
    }
}

std::optional<QByteArray> withClaimedDecisions(const SourcePublisher &publisher, const QString &path, const QByteArray &text)
{
    QString error;
    const QList<QJsonObject> claims = ProposalDecisions::storedClaims(proposalClaimDatabase(publisher), &error, {path});
    if (!error.isEmpty() || claims.isEmpty()) {
        return std::nullopt;
    }
    QString source = QString::fromUtf8(text);
    if (source.toUtf8() != text) {
        return std::nullopt;
    }
    QHash<QString, QString> wrappers; // proposal ID -> SHA-256 of its wrapper in `text`
    for (const AgentProposedEdit &proposal : parseAgentProposals(path, source).proposals) {
        wrappers.insert(proposal.id, ProposalDecisions::wrapperSha256(source, proposal));
    }
    // A claim stands for its decision only on the wrapper it was made on.
    QStringList accepted, rejected;
    for (const QJsonObject &claim : claims) {
        const QString id = claim.value(QStringLiteral("proposal_id")).toString();
        const auto wrapper = wrappers.constFind(id);
        if (wrapper == wrappers.cend() || wrapper.value() != claim.value(QStringLiteral("wrapper_sha256")).toString()) {
            continue;
        }
        accepted.removeAll(id);
        rejected.removeAll(id);
        const QString action = claim.value(QStringLiteral("action")).toString();
        if (action == QLatin1String("accept")) {
            accepted.append(id);
        } else if (action == QLatin1String("reject") || action == QLatin1String("withdraw")) {
            rejected.append(id);
        }
        // A partial decision also edited the text, in a way no claim records.
    }
    if (accepted.isEmpty() && rejected.isEmpty()) {
        return std::nullopt;
    }
    for (const auto &[ids, action] : {std::pair {accepted, QStringLiteral("accept")}, std::pair {rejected, QStringLiteral("reject")}}) {
        if (ids.isEmpty()) {
            continue;
        }
        QString decided;
        if (!decideAgentProposals(path, source, ids, action, &decided, nullptr, nullptr)) {
            return std::nullopt;
        }
        source = decided;
    }
    return source.toUtf8();
}

int dropInapplicableProposalClaims(const QString &docsRoot, const QString &manifestPath, const QString &sourceStatePath, const QString &database,
                                   const std::function<void(const QString &)> &report)
{
    QString error;
    const QList<QJsonObject> stored = ProposalDecisions::storedClaims(database, &error);
    if (!error.isEmpty()) {
        report(QStringLiteral("Proposal decisions are unavailable and wait for the next Sync: ") + error);
        return -1;
    }
    if (stored.isEmpty()) {
        return 0;
    }
    // Only the recorded bases of the state matter: no publication runs, so
    // nothing is read or deleted, and a file without a base has no deletion to publish.
    ClaimApplicability applicability;
    Py::Path root;
    try {
        root = Py::Path(docsRoot);
        const Json manifest = Py::loadsText(Py::readText(Py::Path(manifestPath)));
        if (!manifest.isObject()) {
            Py::raiseValueError(QStringLiteral("source manifest has an unsupported schema"));
        }
        for (const Json &path : manifestPayloadPaths(manifest)) {
            if (path.isString()) applicability.manifestPaths.insert(path.toString());
        }
        applicability.owners = claimOwners(root, manifest);
        const Py::Path statePath(sourceStatePath);
        if (Py::isFile(statePath)) {
            for (const auto &[path, base] : Py::get(Py::loadsText(Py::readText(statePath)), QStringLiteral("bases"), Json::object()).members()) {
                applicability.recorded.insert(path);
            }
        }
    } catch (const Py::Exception &failure) {
        report(QStringLiteral("Could not check which proposal decisions still apply; they wait for the next Sync: ") + failure.message);
        return -1;
    }
    return int(stored.size() - dropInapplicableClaims(root, database, stored, applicability, report).size());
}

}
