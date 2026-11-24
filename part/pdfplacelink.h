// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// A place in a Library PDF, as agent notes link it (server protocol/agent-notes.md, "Links to a
// place in a PDF", capability agent_note_link_targets_v1) and the phone's reader writes its links
// between PDFs:
//   thoughts-pdf-link:<Library filename>.pdf?page=<0-based>&x=<pt>&y=<pt>&rect=<x0>,<y0>,<x1>,<y1>
// x, y and rect are PDF points of that page, origin at the bottom left of its crop box; rect is the
// passage to highlight. Qt Core only: the Shell reads the link, the Part shows the place.

#include <QPointF>
#include <QRectF>
#include <QRegularExpression>
#include <QSizeF>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <optional>

namespace Thoughts
{
struct PdfPlaceLink {
    QString filename;
    int page = 0; // 0-based
    // The point to bring near the top of the view (PDF points). Only y is needed: without x it
    // is the crop box's left edge.
    std::optional<QPointF> point;
    // The passage (PDF points, left <= right, bottom <= top as PDF y grows upwards: QRectF's
    // top() is the lower edge here).
    std::optional<QRectF> rect;
};

// The link, or nothing when it is no thoughts-pdf-link to a plain Library filename and a page.
// x, y and rect are optional: one that is malformed (not a finite number, a rect without four
// numbers or with no area) is left out, so the link still opens its page; unknown parameters are
// ignored, and the first of repeated ones counts.
inline std::optional<PdfPlaceLink> parsePdfPlaceLink(const QString &href)
{
    static const QRegularExpression form(QStringLiteral(R"(^thoughts-pdf-link:([A-Za-z0-9][A-Za-z0-9_.-]*\.pdf)\?page=([0-9]{1,6})((?:&[^&#]*)*)$)"));
    static const QRegularExpression decimal(QStringLiteral(R"(^-?(?:[0-9]{1,9}(?:\.[0-9]*)?|\.[0-9]+)$)"));
    const QRegularExpressionMatch match = form.match(href);
    if (!match.hasMatch()) {
        return std::nullopt;
    }
    PdfPlaceLink link;
    link.filename = match.captured(1);
    link.page = match.captured(2).toInt();
    std::optional<QString> x;
    std::optional<QString> y;
    std::optional<QString> rect;
    for (const QString &parameter : match.captured(3).split(QLatin1Char('&'), Qt::SkipEmptyParts)) {
        const qsizetype separator = parameter.indexOf(QLatin1Char('='));
        if (separator <= 0) {
            continue;
        }
        const QString key = parameter.left(separator);
        const QString value = QUrl::fromPercentEncoding(parameter.mid(separator + 1).toUtf8()).trimmed();
        std::optional<QString> *slot = key == QLatin1String("x") ? &x : key == QLatin1String("y") ? &y : key == QLatin1String("rect") ? &rect : nullptr;
        if (slot && !*slot) {
            *slot = value;
        }
    }
    const auto number = [](const QString &value) -> std::optional<double> {
        if (!decimal.match(value).hasMatch()) {
            return std::nullopt;
        }
        bool ok = false;
        const double parsed = value.toDouble(&ok);
        return ok && std::isfinite(parsed) ? std::optional<double>(parsed) : std::nullopt;
    };
    if (const std::optional<double> top = y ? number(*y) : std::nullopt) {
        const std::optional<double> left = x ? number(*x) : std::nullopt;
        link.point = QPointF(left.value_or(0.0), *top);
    }
    if (rect) {
        const QStringList values = rect->split(QLatin1Char(','));
        if (values.size() == 4) {
            const auto x0 = number(values.at(0).trimmed());
            const auto y0 = number(values.at(1).trimmed());
            const auto x1 = number(values.at(2).trimmed());
            const auto y1 = number(values.at(3).trimmed());
            if (x0 && y0 && x1 && y1 && *x0 != *x1 && *y0 != *y1) {
                link.rect = QRectF(QPointF(std::min(*x0, *x1), std::min(*y0, *y1)), QPointF(std::max(*x0, *x1), std::max(*y0, *y1)));
            }
        }
    }
    return link;
}

// @p points (PDF points of a page, origin at the bottom left of its crop box, in the page's own
// user space: before its /Rotate) as a normalized point of the page as the reader shows it (top
// left origin, 0..1), clamped to the page. @p cropSize is the crop box as the PDF writes it
// (width along user-space x, also for a page with /Rotate 90); @p quarterTurns the clockwise
// quarter turns the page is shown with: its /Rotate and the reader's rotation together
// (Okular::Page::totalOrientation()).
inline QPointF normalizedPdfPoint(const QPointF &points, const QSizeF &cropSize, int quarterTurns)
{
    if (cropSize.width() <= 0 || cropSize.height() <= 0) {
        return {};
    }
    const double x = std::clamp(points.x() / cropSize.width(), 0.0, 1.0);
    const double y = std::clamp(1.0 - points.y() / cropSize.height(), 0.0, 1.0);
    switch (((quarterTurns % 4) + 4) % 4) {
    case 1:
        return {1.0 - y, x};
    case 2:
        return {1.0 - x, 1.0 - y};
    case 3:
        return {y, 1.0 - x};
    default:
        return {x, y};
    }
}

// @p points (a passage in PDF points, as parsePdfPlaceLink gives it) as a normalized rect of the
// page as the reader shows it, like normalizedPdfPoint.
inline QRectF normalizedPdfRect(const QRectF &points, const QSizeF &cropSize, int quarterTurns)
{
    const QPointF first = normalizedPdfPoint(points.topLeft(), cropSize, quarterTurns);
    const QPointF second = normalizedPdfPoint(points.bottomRight(), cropSize, quarterTurns);
    return QRectF(QPointF(std::min(first.x(), second.x()), std::min(first.y(), second.y())), QPointF(std::max(first.x(), second.x()), std::max(first.y(), second.y())));
}
}
