// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QPair>
#include <QString>

#include <optional>

// thoughts_server.sources._merge: Project configurations compare by meaning;
// everything else is a three-way text merge by git merge-file, as Fedora
// merges, so both sides agree on the result. The publisher compiles git's own
// merge code (xdiff/, git v2.56.0) instead of running an installed git.
namespace Thoughts::Publisher
{

// project_configuration.semantic_identity(path, content)
std::optional<QByteArray> semanticIdentity(const QString &path, const QByteArray &content);
// project_configuration.merge(path, base, current, incoming): a field- and
// document-wise three-way merge of a Project manifest, or nullopt on a clash.
std::optional<QByteArray> mergeManifest(const QString &path, const QByteArray &base, const QByteArray &current, const QByteArray &incoming);
// `git merge-file -p -L FEDORA_CURRENT -L COMMON_BASE -L PHONE_INCOMING
// current base incoming` with git's default configuration: (merged bytes, exit
// status), the status being the conflict count capped at 127. Raises
// SourceInfrastructureError with git's message where git exits with an error
// (binary input, out of memory).
QPair<QByteArray, int> mergeFile(const QByteArray &current, const QByteArray &base, const QByteArray &incoming);
// _merge(base, current, incoming, path=path): (merged bytes, conflict count).
// Raises SourceInfrastructureError when the merge fails (see mergeFile).
QPair<QByteArray, int> mergeSources(const QByteArray &base, const QByteArray &current, const QByteArray &incoming, const QString &path);

}
