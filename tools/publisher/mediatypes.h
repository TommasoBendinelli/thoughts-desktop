// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

namespace Thoughts::Publisher
{
// thoughts_server.media_types.guess_media_type: Python's
// mimetypes.guess_type(name)[0] over the frozen macOS table, or
// application/octet-stream.
QString guessMediaType(const QString &name);
// mimetypes.guess_type(name)[0], empty when unknown.
QString guessType(const QString &name);
}
