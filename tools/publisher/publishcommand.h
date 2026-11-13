// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "argparse.h"

namespace Thoughts::Publisher
{

// The publish subcommand. Returns the exit status; raises Py::Exception (after
// reporting the failure event) like the Python publisher's main().
int runPublish(const ArgParse::Arguments &arguments);

}
