// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QByteArray>
#include <QString>

#include "okularpart_export.h"

namespace Thoughts
{
// Comments on a PDF outside the Library are kept on this computer, never in the PDF: one file
// per PDF content (its SHA-256) in the folder outside-pdf-comments of the application data,
// holding Okular::Document::localAnnotationsXml() plus the PDF's path and hash. They come back
// when the same bytes are opened again from anywhere (moved, renamed, a second download), and
// when the file at the same path changed (a newer version of the paper). They stay on this
// computer: nothing of them is synchronized.
namespace OutsidePdfComments
{
// The folder that keeps them.
OKULARPART_EXPORT QString directory();

// The file for the PDF content @p sha256 (lower-case hex).
OKULARPART_EXPORT QString fileFor(const QString &sha256);

// The comments file to restore for the PDF at @p pdfPath with content @p sha256: the one of that
// content, else the newest one last saved for that path. Empty when there is none.
OKULARPART_EXPORT QString find(const QString &pdfPath, const QString &sha256);

// The PDF path a comments file was last saved for.
OKULARPART_EXPORT QString recordedPath(const QString &commentsFile);

// Writes @p xml for the PDF content @p sha256 at @p pdfPath, replacing the file at once. An
// empty @p xml (no comments any more) writes an empty list when the file exists, and nothing
// otherwise.
OKULARPART_EXPORT bool save(const QString &pdfPath, const QString &sha256, const QByteArray &xml, QString *error = nullptr);
}
}
