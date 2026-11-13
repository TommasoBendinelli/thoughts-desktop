// SPDX-License-Identifier: GPL-2.0-or-later
#include "readaloudtext.h"

#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <utility>

namespace Thoughts::ReadAloud
{

namespace
{

bool isHyphen(QChar c)
{
    return c == QLatin1Char('-') || c == QChar(0x2010) || c == QChar(0x2011);
}

bool isClosing(QChar c)
{
    static const QString closing = QStringLiteral("\"')]}»”’");
    return closing.contains(c);
}

// Abbreviations after which a full stop does not end a sentence.
bool isAbbreviation(const QString &word)
{
    static const QSet<QString> abbreviations = {
        QStringLiteral("e.g"), QStringLiteral("i.e"), QStringLiteral("cf"), QStringLiteral("vs"), QStringLiteral("etc"),
        QStringLiteral("al"), QStringLiteral("fig"), QStringLiteral("figs"), QStringLiteral("eq"), QStringLiteral("eqs"),
        QStringLiteral("sec"), QStringLiteral("secs"), QStringLiteral("ch"), QStringLiteral("no"), QStringLiteral("nos"),
        QStringLiteral("vol"), QStringLiteral("pp"), QStringLiteral("p"), QStringLiteral("ref"), QStringLiteral("refs"),
        QStringLiteral("dr"), QStringLiteral("mr"), QStringLiteral("mrs"), QStringLiteral("ms"), QStringLiteral("prof"),
        QStringLiteral("st"), QStringLiteral("approx"), QStringLiteral("resp"), QStringLiteral("viz"), QStringLiteral("thm"),
        QStringLiteral("def"), QStringLiteral("prop"), QStringLiteral("lem"), QStringLiteral("cor"), QStringLiteral("tab"),
        QStringLiteral("app"), QStringLiteral("z.b"), QStringLiteral("u.a"), QStringLiteral("bzw"), QStringLiteral("ca"),
    };
    QString key = word.toLower();
    while (!key.isEmpty() && !key.front().isLetter()) {
        key.remove(0, 1);
    }
    if (key.size() == 1 && key.front().isLetter()) {
        // An initial: "J. Smith", "A. B. Name".
        return true;
    }
    return abbreviations.contains(key);
}

// Sentences of prepared text; each keeps its trailing punctuation, none keeps
// surrounding spaces.
QStringList sentences(const QString &text)
{
    QStringList result;
    qsizetype start = 0;
    const qsizetype n = text.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = text.at(i);
        if (c != QLatin1Char('.') && c != QLatin1Char('!') && c != QLatin1Char('?') && c != QChar(0x2026)) {
            continue;
        }
        qsizetype end = i + 1;
        while (end < n && (text.at(end) == QLatin1Char('.') || text.at(end) == QLatin1Char('!') || text.at(end) == QLatin1Char('?') || isClosing(text.at(end)))) {
            ++end;
        }
        if (end < n && text.at(end) != QLatin1Char(' ')) {
            // "3.5", "e.g.,", "file.txt": not the end of a sentence.
            i = end - 1;
            continue;
        }
        if (c == QLatin1Char('.') && end == i + 1) {
            qsizetype wordStart = i;
            while (wordStart > start && text.at(wordStart - 1) != QLatin1Char(' ')) {
                --wordStart;
            }
            if (isAbbreviation(text.mid(wordStart, i - wordStart))) {
                continue;
            }
        }
        if (end < n) {
            // The next sentence should not start with a lower-case letter.
            qsizetype next = end;
            while (next < n && text.at(next) == QLatin1Char(' ')) {
                ++next;
            }
            if (next < n && text.at(next).isLower()) {
                i = end - 1;
                continue;
            }
        }
        result.append(text.mid(start, end - start).trimmed());
        start = end;
        i = end - 1;
    }
    if (start < n) {
        const QString rest = text.mid(start).trimmed();
        if (!rest.isEmpty()) {
            result.append(rest);
        }
    }
    result.removeAll(QString());
    return result;
}

// A piece of a sentence; spaceBefore tells whether a space separated it from
// the previous piece (not when one long token had to be cut).
struct Piece {
    QString text;
    bool spaceBefore = true;
};

// Splits one piece at the last suitable break at or before limit: after a
// clause mark, else at a space, else (one long token) at the limit itself.
QList<Piece> splitLong(const QString &piece, int limit)
{
    QList<Piece> parts;
    QString rest = piece;
    bool spaceBefore = true;
    while (rest.size() > limit) {
        qsizetype cut = -1;
        for (qsizetype i = limit; i > limit / 3; --i) {
            const QChar c = rest.at(i - 1);
            if ((c == QLatin1Char(';') || c == QLatin1Char(':') || c == QLatin1Char(',') || c == QChar(0x2014) || c == QChar(0x2013)) && i < rest.size()
                && rest.at(i) == QLatin1Char(' ')) {
                cut = i;
                break;
            }
        }
        if (cut < 0) {
            const qsizetype space = rest.lastIndexOf(QLatin1Char(' '), limit);
            cut = space > 0 ? space : limit;
        }
        parts.append({rest.left(cut).trimmed(), spaceBefore});
        spaceBefore = rest.at(cut) == QLatin1Char(' ');
        rest = rest.mid(cut).trimmed();
    }
    if (!rest.isEmpty()) {
        parts.append({rest, spaceBefore});
    }
    return parts;
}

}

QString prepareText(const QString &text)
{
    QString value = text;
    value.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    value.replace(QLatin1Char('\r'), QLatin1Char('\n'));
    value.replace(QChar(0x2028), QLatin1Char('\n'));
    value.replace(QChar(0x2029), QLatin1Char('\n'));
    // A soft hyphen at a line end is a break inside a word.
    static const QRegularExpression softHyphenBreak(QStringLiteral("\\x{00AD}[ \\t]*\\n[ \\t]*"));
    value.replace(softHyphenBreak, QString());
    value.remove(QChar(0x00AD));
    // Ligatures (ﬁ, ﬂ) and other compatibility forms PDFs often contain.
    value = value.normalized(QString::NormalizationForm_KC);

    // "exam-\nple" -> "example" when the next line starts lower-case.
    QString joined;
    joined.reserve(value.size());
    const qsizetype n = value.size();
    for (qsizetype i = 0; i < n; ++i) {
        const QChar c = value.at(i);
        if (isHyphen(c) && i > 0 && value.at(i - 1).isLetter()) {
            qsizetype j = i + 1;
            while (j < n && (value.at(j) == QLatin1Char(' ') || value.at(j) == QLatin1Char('\t'))) {
                ++j;
            }
            if (j < n && value.at(j) == QLatin1Char('\n')) {
                qsizetype k = j + 1;
                while (k < n && value.at(k).isSpace()) {
                    ++k;
                }
                if (k < n && value.at(k).isLower()) {
                    // A word broken across lines: "exam-\nple" -> "example".
                    i = k - 1;
                    continue;
                }
                if (k < n && (value.at(k).isLetterOrNumber())) {
                    // A hyphenated compound broken at its hyphen: "COVID-\n19" -> "COVID-19".
                    joined.append(c);
                    i = k - 1;
                    continue;
                }
            }
        }
        joined.append(c);
    }

    QString collapsed;
    collapsed.reserve(joined.size());
    bool space = false;
    for (const QChar c : std::as_const(joined)) {
        if (c.isSpace() || c.category() == QChar::Other_Control) {
            space = !collapsed.isEmpty();
            continue;
        }
        if (space) {
            collapsed.append(QLatin1Char(' '));
            space = false;
        }
        collapsed.append(c);
    }
    return collapsed;
}

QStringList chunkText(const QString &preparedText, const ChunkLimits &limits)
{
    // Limits only grow from chunk to chunk, so a piece split for one chunk
    // also fits any later one.
    const auto limitFor = [&limits](qsizetype index) {
        return index == 0 ? limits.first : index == 1 ? std::max(limits.first, limits.second) : std::max({limits.first, limits.second, limits.later});
    };
    QStringList chunks;
    QString current;
    for (const QString &sentence : sentences(preparedText)) {
        // A sentence too long for the chunk it would start is split in clauses.
        const int sentenceLimit = limitFor(chunks.size() + (current.isEmpty() ? 0 : 1));
        const QList<Piece> parts = sentence.size() > sentenceLimit ? splitLong(sentence, sentenceLimit) : QList<Piece>{{sentence, true}};
        for (const Piece &part : parts) {
            if (!current.isEmpty()) {
                const bool firstLongEnough = chunks.isEmpty() && current.size() >= limits.firstMinimum;
                if (firstLongEnough || current.size() + 1 + part.text.size() > limitFor(chunks.size())) {
                    chunks.append(std::exchange(current, QString()));
                }
            }
            if (current.isEmpty()) {
                current = part.text;
            } else {
                current += part.spaceBefore ? QLatin1Char(' ') + part.text : part.text;
            }
        }
    }
    if (!current.isEmpty()) {
        chunks.append(current);
    }
    return chunks;
}

}
