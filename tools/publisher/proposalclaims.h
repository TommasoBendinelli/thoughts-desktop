// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyjson.h"
#include "sourcepublisher.h"

#include <optional>

namespace Thoughts::Publisher
{

// desktop-sync.sqlite next to the publisher state, where Thoughts records claims.
QString proposalClaimDatabase(const SourcePublisher &publisher);

// Before a new publication is sent: drops undone claims and records the IDs
// of the claims of its mutations in the state, so a retry expires them too.
// Returns the publication's proposal_decisions member when the server reads
// claims and there are some.
std::optional<Py::Json> prepareProposalClaims(SourcePublisher &publisher, const SourcePublisher::LocalSources &local, const QList<Py::Json> &mutations);

// After the server answered the publication (`result`): forgets its claims.
void expireProposalClaims(SourcePublisher &publisher, const Py::Json &result);

// The claims a source sent alone to /v1/sources/apply carries: the stored
// claims on `path` that `content` shows decided (claims undone in it are
// forgotten). Without them the server's merge saw the user's decisions as
// wrappers removed outside Thoughts. nullopt when there are none or the
// server does not read claims.
std::optional<Py::Json> applyProposalClaims(SourcePublisher &publisher, const QString &path, const QByteArray &content);

// After the server answered such an apply (`mutation`, `result`): forgets the
// claims it carried, unless the server preserved a conflict, whose
// resolution carries them (§10.8).
void expireApplyProposalClaims(SourcePublisher &publisher, const Py::Json &mutation, const Py::Json &result);

// `text`, a Fedora revision of `path`, with the stored claims made on its own
// wrappers decided as they say (accepted, or rejected or withdrawn): the
// source Thoughts wrote when the user decided those proposals on that
// revision. nullopt when no stored claim names a wrapper of `text`, or the
// claims cannot be read or applied.
std::optional<QByteArray> withClaimedDecisions(const SourcePublisher &publisher, const QString &path, const QByteArray &text);

}
