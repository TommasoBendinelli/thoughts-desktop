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

}
