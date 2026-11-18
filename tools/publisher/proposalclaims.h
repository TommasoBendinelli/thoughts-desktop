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

// `text`, a Fedora revision of `path`, with the stored claims made on its own
// wrappers decided as they say (accepted, or rejected or withdrawn): the
// source Thoughts wrote when the user decided those proposals on that
// revision. nullopt when no stored claim names a wrapper of `text`, or the
// claims cannot be read or applied.
std::optional<QByteArray> withClaimedDecisions(const SourcePublisher &publisher, const QString &path, const QByteArray &text);

}
