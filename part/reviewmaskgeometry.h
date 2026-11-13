/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_REVIEWMASKGEOMETRY_H
#define THOUGHTS_REVIEWMASKGEOMETRY_H

#include "core/area.h"
#include "../gui/textlinegeometry.h"
#include <QList>
#include <QMap>
#include <algorithm>
#include <numeric>

namespace Thoughts
{
// A table's columns line up: two rows split at horizontally overlapping gaps.
// A paragraph's run-in heading or an inline formula also splits a row, but not
// in line with another row.
inline bool alignedReviewMaskColumns(const QList<QList<Okular::NormalizedRect>> &rows)
{
    struct Gap {
        double left, right;
        int row;
    };
    QList<Gap> gaps;
    for (int r = 0; r < rows.size(); ++r) {
        QList<Okular::NormalizedRect> row = rows[r];
        std::sort(row.begin(), row.end(), [](const auto &a, const auto &b) { return a.left < b.left; });
        double right = row.first().right;
        for (int i = 1; i < row.size(); ++i) {
            if (row[i].left > right) gaps.append({right, row[i].left, r});
            right = std::max(right, row[i].right);
        }
    }
    for (int i = 0; i < gaps.size(); ++i) {
        for (int j = 0; j < i; ++j) {
            if (gaps[i].row != gaps[j].row && std::max(gaps[i].left, gaps[j].left) < std::min(gaps[i].right, gaps[j].right)) return true;
        }
    }
    return false;
}

// One selection over several paragraphs is one answer: join its blocks across
// the blank space between them, as long as their enclosing rectangle hides no
// page text outside the answer. A gap taller than three lines (a figure, a
// display) or text beside the gap keeps them apart.
inline QList<Okular::NormalizedRect> joinReviewMaskBlocksAcrossBlankGaps(QList<Okular::NormalizedRect> blocks, const QList<Okular::NormalizedRect> &fragments,
                                                                        const QList<Okular::NormalizedRect> &pageWords)
{
    if (pageWords.isEmpty() || blocks.size() < 2 || fragments.isEmpty()) return blocks;
    QList<double> heights;
    for (const auto &f : fragments) heights.append(f.bottom - f.top);
    std::sort(heights.begin(), heights.end());
    const double lineHeight = heights[heights.size() / 2];
    const auto hidesOtherText = [&](double left, double top, double right, double bottom) {
        const double dx = 0.005, dy = 0.25 * lineHeight;
        for (const auto &w : pageWords) {
            const double x = (w.left + w.right) / 2, y = (w.top + w.bottom) / 2;
            if (x <= left || x >= right || y <= top || y >= bottom) continue;
            const bool answer = std::any_of(fragments.begin(), fragments.end(), [&](const auto &f) {
                return x >= f.left - dx && x <= f.right + dx && y >= f.top - dy && y <= f.bottom + dy;
            });
            if (!answer) return true;
        }
        return false;
    };
    for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < blocks.size() && !changed; ++i) {
            for (int j = i + 1; j < blocks.size() && !changed; ++j) {
                const auto &a = blocks[i], &b = blocks[j];
                const double xGap = std::max(a.left, b.left) - std::min(a.right, b.right);
                const double yGap = std::max(a.top, b.top) - std::min(a.bottom, b.bottom);
                if (xGap > 0.0 || yGap > 3 * lineHeight) continue;
                const Okular::NormalizedRect joined(std::min(a.left, b.left), std::min(a.top, b.top), std::max(a.right, b.right), std::max(a.bottom, b.bottom));
                if (hidesOtherText(joined.left, joined.top, joined.right, joined.bottom)) continue;
                blocks[i] = joined;
                blocks.removeAt(j);
                changed = true;
            }
        }
    }
    return blocks;
}

// Hidden answers additionally cover neighbouring lines after shared word grouping.
// pageWords, every word on the page when known, lets a table answer grow over
// its rules and empty space up to, never over, the surrounding text.
inline QList<Okular::NormalizedRect> continuousReviewMaskBlocks(const QList<Okular::NormalizedRect> &words, double pageHeightOverWidth,
                                                               const QList<Okular::NormalizedRect> &pageWords = {})
{
    QList<QRectF> rects;
    for (const auto &r : words) rects.append(QRectF(r.left, r.top, r.right - r.left, r.bottom - r.top));
    QList<Okular::NormalizedRect> fragments;
    for (const auto &r : normalizedTextLineRects(std::move(rects), QSizeF(1.0, pageHeightOverWidth))) {
        fragments.append(Okular::NormalizedRect(r.left(), r.top(), r.right(), r.bottom()));
    }
    // A table answer (several rows, some split into columns) is hidden as one
    // block: covering only its words leaves the rules, column gaps and empty
    // cells readable. Pad it so rules just outside the text are covered too.
    QList<Okular::NormalizedRect> blocks;
    {
        QList<int> cluster(fragments.size());
        std::iota(cluster.begin(), cluster.end(), 0);
        const auto top = [&cluster](int i) {
            while (cluster[i] != i) {
                cluster[i] = cluster[cluster[i]];
                i = cluster[i];
            }
            return i;
        };
        for (int i = 0; i < fragments.size(); ++i) {
            for (int j = 0; j < i; ++j) {
                const auto &a = fragments[i], &b = fragments[j];
                const double height = std::min(a.bottom - a.top, b.bottom - b.top);
                const double yGap = std::max(a.top, b.top) - std::min(a.bottom, b.bottom);
                if (yGap <= 1.2 * height) cluster[top(i)] = top(j);
            }
        }
        QMap<int, QList<int>> members;
        for (int i = 0; i < fragments.size(); ++i) members[top(i)].append(i);
        QList<Okular::NormalizedRect> remaining;
        for (const QList<int> &group : std::as_const(members)) {
            // Rows are printed lines: fragments that overlap vertically, such as
            // the stacked parts of an inline fraction, belong to one row.
            QList<QPair<double, double>> bands;
            QList<QList<Okular::NormalizedRect>> rows;
            for (int index : group) {
                bands.append({fragments[index].top, fragments[index].bottom});
                rows.append({fragments[index]});
            }
            for (bool merged = true; merged;) {
                merged = false;
                for (int a = 0; a < bands.size() && !merged; ++a) {
                    for (int b = a + 1; b < bands.size() && !merged; ++b) {
                        if (bands[a].first < bands[b].second && bands[b].first < bands[a].second) {
                            bands[a] = {std::min(bands[a].first, bands[b].first), std::max(bands[a].second, bands[b].second)};
                            rows[a] += rows.takeAt(b);
                            bands.removeAt(b);
                            merged = true;
                        }
                    }
                }
            }
            if (bands.size() < 2 || !alignedReviewMaskColumns(rows)) {
                for (int index : group) remaining.append(fragments[index]);
                continue;
            }
            Okular::NormalizedRect table = fragments[group.first()];
            double lineHeight = 1.0;
            for (int index : group) {
                const auto &f = fragments[index];
                table.left = std::min(table.left, f.left);
                table.top = std::min(table.top, f.top);
                table.right = std::max(table.right, f.right);
                table.bottom = std::max(table.bottom, f.bottom);
                lineHeight = std::min(lineHeight, f.bottom - f.top);
            }
            double left = table.left - 0.012, top = table.top - 0.6 * lineHeight;
            double right = table.right + 0.012, bottom = table.bottom + 0.6 * lineHeight;
            if (!pageWords.isEmpty()) {
                // Rules sit just outside the text column; stop short of any
                // other text, and never more than three lines away.
                double columnLeft = table.left, columnRight = table.right;
                for (const auto &w : pageWords) {
                    columnLeft = std::min(columnLeft, w.left);
                    columnRight = std::max(columnRight, w.right);
                }
                const double gap = 0.3 * lineHeight;
                left = columnLeft - 0.015;
                right = columnRight + 0.015;
                top = table.top - 3 * lineHeight;
                bottom = table.bottom + 3 * lineHeight;
                for (const auto &w : pageWords) {
                    const double x = (w.left + w.right) / 2, y = (w.top + w.bottom) / 2;
                    if (x >= table.left && x <= table.right && y >= table.top && y <= table.bottom) continue; // the answer itself
                    const bool besideRows = w.top < table.bottom && w.bottom > table.top;
                    const bool withinColumns = w.left < table.right && w.right > table.left;
                    if (withinColumns && w.bottom <= table.top) top = std::max(top, w.bottom + gap);
                    if (withinColumns && w.top >= table.bottom) bottom = std::min(bottom, w.top - gap);
                    if (besideRows && w.right <= table.left) left = std::max(left, w.right + 0.005);
                    if (besideRows && w.left >= table.right) right = std::min(right, w.left - 0.005);
                }
                left = std::min(left, table.left);
                top = std::min(top, table.top);
                right = std::max(right, table.right);
                bottom = std::max(bottom, table.bottom);
            }
            blocks.append(Okular::NormalizedRect(std::max(0.0, left), std::max(0.0, top), std::min(1.0, right), std::min(1.0, bottom)));
        }
        fragments = remaining;
    }
    QList<int> parents(fragments.size());
    std::iota(parents.begin(), parents.end(), 0);
    const auto root = [&parents](int i) {
        while (parents[i] != i) {
            parents[i] = parents[parents[i]];
            i = parents[i];
        }
        return i;
    };
    for (int i = 0; i < fragments.size(); ++i) {
        const auto &a = fragments[i];
        for (int j = 0; j < i; ++j) {
            const auto &b = fragments[j];
            // A tall visual region must not bridge unrelated text.
            const double height = std::min(a.bottom - a.top, b.bottom - b.top);
            const double xGap = std::max(a.left, b.left) - std::min(a.right, b.right);
            const double yGap = std::max(a.top, b.top) - std::min(a.bottom, b.bottom);
            const bool neighbours = xGap <= 0.0 && yGap <= 0.8 * height;
            if (neighbours) parents[root(i)] = root(j);
        }
    }
    // Establish connectivity from the original fragments, not growing bounds.
    QMap<int, Okular::NormalizedRect> bounds;
    for (int i = 0; i < fragments.size(); ++i) {
        const auto &rect = fragments[i];
        const int key = root(i);
        auto block = bounds.find(key);
        if (block == bounds.end()) bounds.insert(key, rect);
        else {
            block->left = std::min(block->left, rect.left);
            block->top = std::min(block->top, rect.top);
            block->right = std::max(block->right, rect.right);
            block->bottom = std::max(block->bottom, rect.bottom);
        }
    }
    const QList<Okular::NormalizedRect> joined = joinReviewMaskBlocksAcrossBlankGaps(bounds.values(), fragments, pageWords);
    // Drop blocks inside another block, such as the parts of an inline fraction within its paragraph.
    QList<Okular::NormalizedRect> result = blocks + joined, kept;
    for (int i = 0; i < result.size(); ++i) {
        const auto &a = result[i];
        bool contained = false;
        for (int j = 0; j < result.size() && !contained; ++j) {
            const auto &b = result[j];
            const bool inside = j != i && a.left >= b.left && a.top >= b.top && a.right <= b.right && a.bottom <= b.bottom;
            const bool same = a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
            // Of two identical blocks, keep the first.
            contained = inside && (j < i || !same);
        }
        if (!contained) kept.append(a);
    }
    return kept;
}
}
#endif
