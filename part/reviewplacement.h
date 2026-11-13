/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef THOUGHTS_REVIEWPLACEMENT_H
#define THOUGHTS_REVIEWPLACEMENT_H

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QString>
#include <cmath>

namespace Thoughts
{
inline bool hasCompilerPlacementEvidence(const QJsonObject &proof, const QString &reviewKey, const QString &kind)
{
    const int version = proof.value(QStringLiteral("resolver_version")).toInt();
    if ((version != 3 && version != 4) || !proof.value(QStringLiteral("verified")).toBool()
        || proof.value(QStringLiteral("candidate_count")).toInt() != 1 || proof.value(QStringLiteral("occurrence")).toInt() < 1) {
        return false;
    }
    const bool point = kind == QLatin1String("exact_point");
    if (!point && kind != QLatin1String("exact_text")) {
        return false;
    }
    if (version == 4) {
        static const QRegularExpression sha(QStringLiteral("^[0-9a-f]{64}$"));
        static const QRegularExpression fragment(QStringLiteral("^thoughts-md-fragment-[1-9][0-9]*$"));
        const QJsonArray markers = proof.value(QStringLiteral("marker_ids")).toArray();
        const QString method = proof.value(QStringLiteral("method")).toString();
        const QString prefix = reviewKey + QLatin1Char(':');
        static const QRegularExpression segment(QStringLiteral("^[A-Za-z0-9_-]+$"));
        return proof.value(QStringLiteral("source_format")).toString() == QLatin1String("markdown")
            && reviewKey.startsWith(QLatin1String("comment:"))
            && sha.match(proof.value(QStringLiteral("renderer_evidence_sha256")).toString()).hasMatch()
            && fragment.match(proof.value(QStringLiteral("fragment_id")).toString()).hasMatch()
            && markers.size() == 1 && markers.at(0).toString().startsWith(prefix)
            && segment.match(markers.at(0).toString().mid(prefix.size())).hasMatch()
            && (point ? method == QLatin1String("markdown_source_boundary")
                      : method == QLatin1String("markdown_text_fragment") || method == QLatin1String("markdown_svg_object") || method == QLatin1String("markdown_image_object"));
    }
    if (proof.value(QStringLiteral("source_format")).toString(QStringLiteral("latex")) != QLatin1String("latex")) return false;
    if (proof.value(QStringLiteral("method")).toString() != (point ? QLatin1String("tex_shipout_point") : QLatin1String("tex_shipout_text"))
        && (point || proof.value(QStringLiteral("method")).toString() != QLatin1String("tex_shipout_box"))) {
        return false;
    }
    const QJsonArray markers = proof.value(QStringLiteral("marker_ids")).toArray();
    if (markers.size() != (point ? 1 : 2)) {
        return false;
    }
    const QString prefix = QStringLiteral("thoughts-shipout:%1:").arg(reviewKey);
    const QString suffix = QStringLiteral(":%1").arg(proof.value(QStringLiteral("occurrence")).toInt());
    QString firstStem;
    for (qsizetype index = 0; index < markers.size(); ++index) {
        const QString marker = markers.at(index).toString();
        if (!marker.startsWith(prefix) || !marker.endsWith(suffix)) {
            return false;
        }
        QString role = marker.mid(prefix.size(), marker.size() - prefix.size() - suffix.size());
        const QString edge = point ? QStringLiteral("point") : index == 0 ? QStringLiteral("start") : QStringLiteral("end");
        QString stem;
        if (role != edge) {
            if (point || !role.endsWith(QLatin1Char('-') + edge)) {
                return false;
            }
            stem = role.left(role.size() - edge.size() - 1);
            static const QRegularExpression segment(QStringLiteral("^span-[0-9]+$"));
            if (!segment.match(stem).hasMatch()) {
                return false;
            }
        }
        if (index == 0) {
            firstStem = stem;
        } else if (firstStem != stem) {
            return false;
        }
    }
    return true;
}

inline bool validNormalizedReviewCoordinates(const QJsonArray &values, int size)
{
    if (values.size() != size) {
        return false;
    }
    for (const auto &value : values) {
        const double number = value.toDouble(-1.0);
        if (!value.isDouble() || !std::isfinite(number) || number < 0.0 || number > 1.0) {
            return false;
        }
    }
    return size != 4 || (values.at(0).toDouble() < values.at(2).toDouble() && values.at(1).toDouble() < values.at(3).toDouble());
}

inline bool validCompilerReviewLocation(const QJsonObject &location, const QString &reviewKey, const QString &filename, const QString &pdfSha256, int pageCount, const QJsonObject &correspondence)
{
    const QJsonObject proof = location.value(QStringLiteral("proof")).toObject();
    const QString kind = location.value(QStringLiteral("placement_kind")).toString();
    const int page = location.value(QStringLiteral("page")).toInt();
    const QString bundle = location.value(QStringLiteral("source_bundle_sha256")).toString();
    const QString build = location.value(QStringLiteral("build_sha256")).toString();
    static const QRegularExpression sha256(QStringLiteral("^[0-9a-f]{64}$"));
    if (location.value(QStringLiteral("pdf_filename")).toString() != filename || location.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256
        || !sha256.match(pdfSha256).hasMatch() || page < 1 || page > pageCount || location.value(QStringLiteral("page_index")).toInt(-1) != page - 1
        || proof.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256 || !sha256.match(bundle).hasMatch() || !sha256.match(build).hasMatch()
        || proof.value(QStringLiteral("source_bundle_sha256")).toString() != bundle || proof.value(QStringLiteral("build_sha256")).toString() != build
        || !hasCompilerPlacementEvidence(proof, reviewKey, kind)) {
        return false;
    }
    if (correspondence.value(QStringLiteral("review_key")).toString() != reviewKey
        || correspondence.value(QStringLiteral("source_bundle_sha256")).toString() != bundle
        || correspondence.value(QStringLiteral("state")).toString() != kind) return false;
    bool sourceMatches = false;
    const QString sourceHash = proof.value(QStringLiteral("source_sha256")).toString();
    if (!sha256.match(sourceHash).hasMatch()) return false;
    for (const QJsonValue &targetValue : correspondence.value(QStringLiteral("source_targets")).toArray()) {
        const QJsonObject target = targetValue.toObject();
        sourceMatches = sourceMatches || (target.value(QStringLiteral("source_sha256")).toString() == sourceHash
            && target.value(QStringLiteral("role")) == location.value(QStringLiteral("role")));
    }
    if (!sourceMatches) return false;
    const QJsonArray point = location.value(QStringLiteral("point")).toArray();
    const QJsonArray regions = location.value(QStringLiteral("regions")).toArray();
    if (kind == QLatin1String("exact_point")) {
        return regions.isEmpty() && validNormalizedReviewCoordinates(point, 2);
    }
    if (!point.isEmpty() || regions.isEmpty()) {
        return false;
    }
    for (const auto &region : regions) {
        if (!validNormalizedReviewCoordinates(region.toArray(), 4)) {
            return false;
        }
    }
    return true;
}
inline bool validCompilerReviewCorrespondence(const QJsonObject &correspondence, const QString &filename, const QString &pdfSha256, int pageCount)
{
    const QJsonArray locations = correspondence.value(QStringLiteral("locations")).toArray();
    if (locations.isEmpty()) return false;
    for (const QJsonValue &value : locations) {
        if (!validCompilerReviewLocation(value.toObject(), correspondence.value(QStringLiteral("review_key")).toString(), filename, pdfSha256, pageCount, correspondence)) return false;
    }
    return true;
}

}
#endif
