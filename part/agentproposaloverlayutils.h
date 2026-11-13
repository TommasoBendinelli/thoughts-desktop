/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef THOUGHTS_AGENTPROPOSALOVERLAYUTILS_H
#define THOUGHTS_AGENTPROPOSALOVERLAYUTILS_H

#include <QJsonArray>
#include <QList>
#include <QMap>
#include <QRectF>
#include <QSizeF>
#include <QString>

namespace Thoughts
{

enum class AgentProposalOverlayStyle {
    Fragment,
    Region,
};

struct AgentProposalOverlay {
    QString id;
    QString role;
    int page = -1;
    QRectF rect;
    AgentProposalOverlayStyle style = AgentProposalOverlayStyle::Fragment;
    bool normalized = false;
    // LaTeX-made copy of the change (contents entry, running head), as marked by the server.
    bool duplicate = false;
};

// Coalesce all verified text locations in PDF points, across source-format boundaries.
// Proposal IDs, roles, pages, duplicate copies, and structural/point markers remain independent.
QList<AgentProposalOverlay> mergeAgentProposalOverlayLines(QList<AgentProposalOverlay> overlays, const QList<QSizeF> &pageSizes);

// Index in overlays of each proposal's single pencil anchor: its earliest
// overlay (page, then top, then left) that is not a LaTeX-made copy, or its
// earliest copy when it has nothing else. Overlays on pages without a size in
// pageSizes (PDF points, for overlays that are not normalized) are ignored.
QMap<QString, qsizetype> agentProposalMarkerAnchors(const QList<AgentProposalOverlay> &overlays, const QList<QSizeF> &pageSizes);

QList<AgentProposalOverlay> agentProposalOverlays(const QString &id,
                                                  const QString &role,
                                                  const QString &preview,
                                                  const QJsonArray &fragments,
                                                  const QString &pdfFilename,
                                                  int pageCount);

}

#endif
