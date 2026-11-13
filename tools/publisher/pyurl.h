// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

// urllib.parse and posixpath as the Python publisher uses them to follow a
// stylesheet's url() references.
namespace Thoughts::Py
{

struct SplitResult {
    QString scheme;
    QString netloc;
    QString path;
    QString query;
    QString fragment;
};

// urllib.parse.urlsplit(url); raises ValueError for malformed bracketed hosts.
SplitResult urlsplit(const QString &url);
// urllib.parse.unquote(text): %XX as UTF-8, invalid sequences become U+FFFD.
QString unquote(const QString &text);
// urllib.parse.quote(text, safe=safe): UTF-8, uppercase hex.
QString quote(const QString &text, const QString &safe = QStringLiteral("/"));

// posixpath.normpath / join / dirname
QString posixNormpath(const QString &path);
QString posixJoin(const QString &first, const QString &second);
QString posixDirname(const QString &path);

// bytes.decode("utf-8", "replace")
QString decodeUtf8Replacing(const QByteArray &bytes);

}
