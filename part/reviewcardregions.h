/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_REVIEWCARDREGIONS_H
#define THOUGHTS_REVIEWCARDREGIONS_H

#include "reviewplacement.h"

namespace Thoughts
{
inline QJsonArray effectiveReviewCardRegions(const QJsonObject &primary, int schemaVersion)
{
    const QJsonObject edited = primary.value(QStringLiteral("answer_region_override")).toObject();
    const QJsonArray custom = edited.value(QStringLiteral("regions")).toArray();
    bool valid = !custom.isEmpty() && !primary.value(QStringLiteral("pdf_sha256")).toString().isEmpty()
        && edited.value(QStringLiteral("pdf_sha256")) == primary.value(QStringLiteral("pdf_sha256"));
    for (const auto &value : custom) {
        const auto region = value.toObject();
        valid = valid && region.value(QStringLiteral("page")).toInt() >= 1
            && validNormalizedReviewCoordinates(region.value(QStringLiteral("rect")).toArray(), 4);
    }
    if (valid) return custom;
    if (primary.contains(QStringLiteral("regions"))) return primary.value(QStringLiteral("regions")).toArray();
    if (schemaVersion == 3) return {QJsonObject{{QStringLiteral("page"), primary.value(QStringLiteral("page"))},
                                               {QStringLiteral("rect"), primary.value(QStringLiteral("rect"))}}};
    return {};
}
}
#endif
