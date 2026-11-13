// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

// Text preparation for Read Aloud: what the reader's selection becomes before
// it is sent to the cloud voice or the system voice.
namespace Thoughts::ReadAloud
{

// PDF text as one flowing passage: line breaks become spaces, a word hyphenated
// at the end of a line is joined again ("exam-\nple" -> "example") when the next
// line starts with a lower-case letter, ligatures and soft hyphens are undone,
// and runs of whitespace collapse to one space.
QString prepareText(const QString &text);

struct ChunkLimits {
    // The first chunk is short so that audio starts quickly.
    int first = 400;
    // The second one is still moderate, so it is ready before the first ends.
    int second = 800;
    // All later ones.
    int later = 1200;
    // The first chunk takes further sentences until it is at least this long.
    int firstMinimum = 80;
};

// Prepared text split into chunks for synthesis, at sentence boundaries where
// possible, else at clause boundaries, else between words; a single "word"
// longer than a chunk (a long URL) is the only thing ever cut. The chunks
// joined with single spaces give the text back, except where such a word was
// cut.
QStringList chunkText(const QString &preparedText, const ChunkLimits &limits = {});

}
