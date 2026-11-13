/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "agentproposaloverlayutils.h"

#include "../gui/textlinegeometry.h"
#include <map>
#include <tuple>
#include <utility>
#include <QJsonObject>
#include <QMap>
#include <QStringList>

namespace
{

bool isStructuralProposalPreview(const QString &preview)
{
    const QStringList structuralCommands {
        QStringLiteral("\\includegraphics"),
        QStringLiteral("\\includepdf"),
        QStringLiteral("\\begin{figure"),
        QStringLiteral("\\begin{table"),
        QStringLiteral("\\begin{longtable"),
        QStringLiteral("\\begin{tikzpicture"),
        QStringLiteral("\\resizebox"),
    };
    for (const QString &command : structuralCommands) {
        if (preview.contains(command)) {
            return true;
        }
    }
    return preview.contains(QLatin1String("\\input{"))
        && (preview.contains(QLatin1String("figures/")) || preview.contains(QLatin1String("tables/")));
}


}

QList<Thoughts::AgentProposalOverlay> Thoughts::mergeAgentProposalOverlayLines(QList<AgentProposalOverlay> overlays, const QList<QSizeF> &pageSizes)
{
    std::map<std::tuple<QString, QString, int, bool>, QList<AgentProposalOverlay>> groups;
    QList<AgentProposalOverlay> merged;
    for (AgentProposalOverlay overlay : overlays) {
        if (overlay.style != AgentProposalOverlayStyle::Fragment) {
            merged.append(overlay);
            continue;
        }
        if (overlay.normalized) {
            const QSizeF size = pageSizes.value(overlay.page);
            if (size.isEmpty()) {
                merged.append(overlay);
                continue;
            }
            // Normalized x and y have different scales on non-square pages.
            overlay.rect = QRectF(overlay.rect.x() * size.width(), overlay.rect.y() * size.height(),
                                  overlay.rect.width() * size.width(), overlay.rect.height() * size.height());
            overlay.normalized = false;
        }
        groups[{overlay.id, overlay.role, overlay.page, overlay.duplicate}].append(overlay);
    }
    for (auto &[key, fragments] : groups) {
        QList<QRectF> rects;
        for (const auto &fragment : fragments) rects.append(fragment.rect);
        for (const QRectF &rect : mergeTextLineRects(std::move(rects))) {
            auto overlay = fragments.constFirst();
            overlay.rect = rect;
            merged.append(overlay);
        }
    }
    return merged;
}

QMap<QString, qsizetype> Thoughts::agentProposalMarkerAnchors(const QList<AgentProposalOverlay> &overlays, const QList<QSizeF> &pageSizes)
{
    const auto normalizedStart = [&pageSizes](const AgentProposalOverlay &overlay) {
        const QSizeF size = pageSizes.at(overlay.page);
        return overlay.normalized ? overlay.rect.topLeft() : QPointF(overlay.rect.left() / size.width(), overlay.rect.top() / size.height());
    };
    const auto earlier = [&normalizedStart](const AgentProposalOverlay &overlay, const AgentProposalOverlay &previous) {
        if (overlay.page != previous.page) {
            return overlay.page < previous.page;
        }
        const QPointF start = normalizedStart(overlay);
        const QPointF previousStart = normalizedStart(previous);
        return start.y() < previousStart.y() || (start.y() == previousStart.y() && start.x() < previousStart.x());
    };
    QMap<QString, qsizetype> first;
    QMap<QString, qsizetype> firstPrimary;
    const auto keepEarliest = [&overlays, &earlier](QMap<QString, qsizetype> &earliest, qsizetype index) {
        const auto previous = earliest.constFind(overlays.at(index).id);
        if (previous == earliest.cend() || earlier(overlays.at(index), overlays.at(*previous))) {
            earliest.insert(overlays.at(index).id, index);
        }
    };
    for (qsizetype index = 0; index < overlays.size(); ++index) {
        const AgentProposalOverlay &overlay = overlays.at(index);
        if (overlay.page < 0 || overlay.page >= pageSizes.size() || pageSizes.at(overlay.page).isEmpty()) {
            continue;
        }
        keepEarliest(first, index);
        if (!overlay.duplicate) {
            keepEarliest(firstPrimary, index);
        }
    }
    first.insert(firstPrimary);
    return first;
}

QList<Thoughts::AgentProposalOverlay> Thoughts::agentProposalOverlays(const QString &id,
                                                                     const QString &role,
                                                                     const QString &preview,
                                                                     const QJsonArray &fragments,
                                                                     const QString &pdfFilename,
                                                                     int pageCount)
{
    QList<AgentProposalOverlay> overlays;
    for (const QJsonValue &value : fragments) {
        const QJsonObject fragment = value.toObject();
        if (fragment.value(QStringLiteral("pdf_filename")).toString().compare(pdfFilename, Qt::CaseInsensitive) != 0) {
            continue;
        }
        const int page = fragment.value(QStringLiteral("page")).toInt() - 1;
        const QJsonValue leftValue = fragment.value(QStringLiteral("left"));
        const QJsonValue topValue = fragment.value(QStringLiteral("top"));
        const QJsonValue rightValue = fragment.value(QStringLiteral("right"));
        const QJsonValue bottomValue = fragment.value(QStringLiteral("bottom"));
        if (page < 0 || page >= pageCount || !leftValue.isDouble() || !topValue.isDouble() || !rightValue.isDouble() || !bottomValue.isDouble()) {
            continue;
        }
        const double left = leftValue.toDouble();
        const double top = topValue.toDouble();
        const QRectF rect(left, top, rightValue.toDouble() - left, bottomValue.toDouble() - top);
        if (!id.isEmpty() && rect.isValid()) {
            overlays.append({id, role, page, rect, AgentProposalOverlayStyle::Fragment, false, fragment.value(QStringLiteral("duplicate")).toBool()});
        }
    }

    if (overlays.isEmpty()) {
        return overlays;
    }

    if (!isStructuralProposalPreview(preview)) {
        return mergeAgentProposalOverlayLines(std::move(overlays), {});
    }

    QMap<std::pair<int, bool>, QRectF> pageRegions;
    for (const AgentProposalOverlay &overlay : std::as_const(overlays)) {
        auto found = pageRegions.find({overlay.page, overlay.duplicate});
        if (found == pageRegions.end()) {
            pageRegions.insert({overlay.page, overlay.duplicate}, overlay.rect);
        } else {
            *found = found->united(overlay.rect);
        }
    }

    overlays.clear();
    for (auto it = pageRegions.cbegin(); it != pageRegions.cend(); ++it) {
        overlays.append({id, role, it.key().first, it.value(), AgentProposalOverlayStyle::Region, false, it.key().second});
    }
    return overlays;
}
