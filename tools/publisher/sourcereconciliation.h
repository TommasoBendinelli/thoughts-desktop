// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "argparse.h"

namespace Thoughts::Publisher
{

// python -m thoughts_server.source_reconciliation: returns the exit status.
int runReconcile(const ArgParse::Arguments &arguments);

}
