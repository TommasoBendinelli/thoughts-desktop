/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "../shell/agentproposalparser.h"

#include <QString>
#include <QVector>

namespace Okular
{
// Retain source offsets and line numbers, but never resolve a comment into a
// proposal's historical branch or a literal TeX example.
inline QString visibleCommentSource(const QString &source, bool hideProposalHistory = true)
{
    QString visible = source;
    // Wrappers exactly as the agent-proposal reader finds them (spec §4.2).
    const auto wrappers = hideProposalHistory ? Thoughts::latexAgentProposalHiddenRanges(source) : QVector<QPair<int, int>>();
    qsizetype nextWrapper = 0;
    const auto mask = [&](int first, int last) {
        for (int i = first; i < last; ++i) {
            if (source.at(i) != QLatin1Char('\n') && source.at(i) != QLatin1Char('\r')) {
                visible[i] = QLatin1Char(' ');
            }
        }
    };
    const auto space = [&](int at) {
        while (at < source.size()) {
            if (source.at(at).isSpace()) ++at;
            else if (source.at(at) == QLatin1Char('%')) {
                const int next = source.indexOf(QLatin1Char('\n'), at);
                at = next < 0 ? source.size() : next + 1;
            } else break;
        }
        return at;
    };
    const auto groupEnd = [&](int at) {
        at = space(at);
        if (at >= source.size() || source.at(at) != QLatin1Char('{')) return -1;
        int depth = 1;
        while (++at < source.size()) {
            const QChar c = source.at(at);
            if (c == QLatin1Char('\\')) ++at;
            else if (c == QLatin1Char('%')) {
                at = source.indexOf(QLatin1Char('\n'), at);
                if (at < 0) return -1;
            } else if (c == QLatin1Char('{')) ++depth;
            else if (c == QLatin1Char('}') && --depth == 0) return at + 1;
        }
        return -1;
    };
    for (int at = 0; at < source.size();) {
        if (source.at(at) == QLatin1Char('%')) {
            const int end = source.indexOf(QLatin1Char('\n'), at);
            const int next = end < 0 ? source.size() : end;
            mask(at, next); at = next; continue;
        }
        if (source.at(at) != QLatin1Char('\\')) { ++at; continue; }
        while (nextWrapper < wrappers.size() && wrappers.at(nextWrapper).first < at) ++nextWrapper;
        if (nextWrapper < wrappers.size() && wrappers.at(nextWrapper).first == at) {
            const int next = wrappers.at(nextWrapper++).second;
            mask(at, next); at = next; continue;
        }
        int end = at + 1;
        if (end < source.size() && source.at(end).isLetter()) {
            while (end < source.size() && (source.at(end).isLetter() || source.at(end) == QLatin1Char('@'))) ++end;
        } else { at = qMin(end + 1, int(source.size())); continue; }
        const QString command = source.mid(at + 1, end - at - 1);
        const bool definition = command == QLatin1String("def") || command == QLatin1String("gdef") || command == QLatin1String("edef") || command == QLatin1String("xdef");
        if (definition || command == QLatin1String("newcommand") || command == QLatin1String("renewcommand")
            || command == QLatin1String("providecommand") || command == QLatin1String("DeclareRobustCommand")) {
            int cursor = space(end);
            if (cursor < source.size() && source.at(cursor) == QLatin1Char('*')) cursor = space(cursor + 1);
            if (!definition && cursor < source.size() && source.at(cursor) == QLatin1Char('{')) cursor = groupEnd(cursor);
            else if (cursor < source.size() && source.at(cursor) == QLatin1Char('\\')) {
                ++cursor;
                while (cursor < source.size() && (source.at(cursor).isLetter() || source.at(cursor) == QLatin1Char('@'))) ++cursor;
            }
            if (cursor >= 0) {
                cursor = space(cursor);
                if (definition) {
                    while (cursor < source.size() && source.at(cursor) != QLatin1Char('{')) ++cursor;
                } else {
                    while (cursor < source.size() && source.at(cursor) == QLatin1Char('[')) {
                        const int close = source.indexOf(QLatin1Char(']'), cursor + 1);
                        if (close < 0) break;
                        cursor = space(close + 1);
                    }
                }
                const int next = groupEnd(cursor);
                if (next >= 0) { mask(at, next); at = next; continue; }
            }
        }
        if (command == QLatin1String("code") || command == QLatin1String("detokenize") || command == QLatin1String("url")) {
            const int next = groupEnd(end);
            if (next >= 0) { mask(at, next); at = next; continue; }
        } else if (command == QLatin1String("begin")) {
            const int start = space(end), next = groupEnd(end);
            if (next >= 0) {
                const QString environment = source.mid(start + 1, next - start - 2);
                if (environment == QLatin1String("DocCode") || environment == QLatin1String("Verbatim") || environment == QLatin1String("verbatim") || environment == QLatin1String("lstlisting") || environment == QLatin1String("minted")) {
                    const QString closing = QStringLiteral("\\end{") + environment + QLatin1Char('}');
                    const int stop = source.indexOf(closing, next);
                    const int limit = stop < 0 ? source.size() : stop + closing.size();
                    mask(at, limit); at = limit; continue;
                }
            }
        }
        at = end;
    }
    return visible;
}
}
