// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyfs.h"

#include <QString>

// The local-source progress protocol (--desktop-progress-jsonl): a stderr line
// before each potentially blocking access, so the desktop can tell a slow
// folder from a stalled one and name it.
namespace Thoughts::Publisher::Progress
{

void setActive(bool active);
bool isActive();
// operation is scan_directory, inspect_file or read_file. Paths outside
// docsRoot (or docsRoot itself) are not reported.
void report(const char *operation, const Py::Path &docsRoot, const Py::Path &path);

}
