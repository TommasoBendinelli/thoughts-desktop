// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "argparse.h"
#include "pyjson.h"

namespace Thoughts::Publisher
{

class SourcePublisher;

// The installer's run against `publisher` through Fedora's source frontier
// `through`: its report (without writing the report file).
Py::Json reconcileAuthoringSources(SourcePublisher &publisher, qint64 through);

// python -m thoughts_server.source_reconciliation: returns the exit status.
int runReconcile(const ArgParse::Arguments &arguments);

}
