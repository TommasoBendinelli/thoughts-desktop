// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QStringList>

// The files a stylesheet loads, as the Python publisher finds them with
// tinycss2.parse_stylesheet(text, skip_comments=True, skip_whitespace=True):
// url() tokens, url("...") functions and @import strings anywhere in the
// rules. A rule tinycss2 drops (e.g. one cut off by the end of the file)
// contributes nothing.
namespace Thoughts::Publisher
{

QStringList stylesheetReferences(const QString &css);

}
