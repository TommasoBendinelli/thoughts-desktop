/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "../shell/agentproposalparser.h"

#include <QStringList>
#include <QStringView>
#include <QVector>

namespace Okular
{
// A navigation-only projection: hidden proposal arguments become spaces, so
// physical source lines and offsets remain valid. Literal examples (comments,
// verbatim, definitions) stay visible and are not interpreted as proposals.
// Do not use this for comment anchoring.
inline QStringList sourceNavigationLines(const QStringList &lines)
{
    bool hasProposal = false;
    for (const QString &line : lines) {
        if (line.contains(QLatin1String("\\agentProposedEdit"))) {
            hasProposal = true;
            break;
        }
    }
    if (!hasProposal) {
        return lines;
    }
    const QString source = lines.join(QLatin1Char('\n'));
    struct Range { qsizetype first; qsizetype last; };
    QVector<Range> hidden;
    // Wrappers exactly as the agent-proposal reader finds them (spec §4.2).
    bool incomplete = false;
    for (const auto &[first, last] : Thoughts::latexAgentProposalHiddenRanges(source, &incomplete)) hidden.append({first, last});
    if (incomplete) return {}; // Incomplete source: never expose an old branch.
    if (hidden.isEmpty()) return lines;
    QStringList visible = lines; // Implicit sharing; only masked lines detach.
    qsizetype lineIndex = 0;
    qsizetype lineStart = 0;
    for (const Range &range : hidden) {
        while (lineIndex < visible.size() && lineStart + visible.at(lineIndex).size() < range.first) {
            lineStart += visible.at(lineIndex).size() + 1;
            ++lineIndex;
        }
        while (lineIndex < visible.size() && lineStart < range.last) {
            const qsizetype lineEnd = lineStart + visible.at(lineIndex).size();
            const qsizetype first = qMax(range.first, lineStart) - lineStart;
            const qsizetype last = qMin(range.last, lineEnd) - lineStart;
            if (first < last) {
                QString &line = visible[lineIndex];
                for (qsizetype i = first; i < last; ++i) {
                    if (line.at(i) != QLatin1Char('\r')) line[i] = QLatin1Char(' ');
                }
            }
            if (lineEnd >= range.last) break;
            lineStart = lineEnd + 1;
            ++lineIndex;
        }
    }
    return visible;
}
}
