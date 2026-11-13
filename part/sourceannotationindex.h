/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include "core/okularcore_export.h"

namespace Thoughts {
// Locations only: annotation bodies, decisions, and PDF geometry live elsewhere.
QJsonObject scanAnnotationLocations(const QString &path, const QByteArray &source, const QJsonArray &comments);
QString commentAnnotationType(const QString &subtype);
// Version of the per-file index written by DocsCommentSynchronizer::sourceCommentFileIndex.
// Raising it rescans every source file and re-derives every cached comment location.
inline constexpr int annotationParserVersion = 4;
QJsonObject annotationSourceLocation(const QJsonObject &index, const QString &type, const QString &id, const QString &project, const QString &sourceFile = {});
QJsonObject annotationSourceLocation(const QString &root, const QString &type, const QString &id, const QString &project, const QString &sourceFile = {});
// A comment record's location: its own markup type first, then any other comment marker
// with its ID. A Markdown marker carries no type, and a TeX marker may differ from the
// PDF annotation subtype (a point marker for a highlight); the ID is what links them.
QJsonObject commentSourceLocation(const QJsonObject &index, const QString &subtype, const QString &id, const QString &project, const QString &sourceFile = {});
// The shared core owns process state so the shell and loaded PDF part see the same revision.
OKULARCORE_EXPORT QJsonObject sourceLocationIndex(const QString &root);
OKULARCORE_EXPORT void installSourceLocationIndex(const QString &root, const QJsonObject &index, const QJsonObject &scannedFiles = {});
OKULARCORE_EXPORT QJsonArray reviewSourceEvidence(const QString &root, const QString &id, const QString &project, const QString &sourceFile = {});
OKULARCORE_EXPORT void invalidateSourceLocationIndex(const QString &root);
// Forgets the loaded index and its evidence (the default-folder rename
// changed every path in them); the next scan rebuilds both.
OKULARCORE_EXPORT void discardSourceLocationIndex(const QString &root);
}
