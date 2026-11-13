/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../gui/textlinepath.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

class TextLineGeometryTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void selectedCommentScreenshot();
    void rotatedAndReversedQuads();
    void distantAndInvalidRegions();
};

void TextLineGeometryTest::selectedCommentScreenshot()
{
    QFile fixture(QStringLiteral(KDESRCDIR "data/comment-highlight-lines.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    QList<QPolygonF> quads;
    for (const auto &value : QJsonDocument::fromJson(fixture.readAll()).array()) {
        const auto r = value.toArray();
        const QRectF rect(r[0].toDouble() * 595.276, r[1].toDouble() * 841.89,
                          (r[2].toDouble() - r[0].toDouble()) * 595.276, (r[3].toDouble() - r[1].toDouble()) * 841.89);
        quads.append({rect.topLeft(), rect.topRight(), rect.bottomRight(), rect.bottomLeft()});
    }
    QCOMPARE(quads.size(), 17);
    const auto path = Thoughts::textLinePath(quads);
    QCOMPARE(path.toSubpathPolygons().size(), 2);
    for (const auto &quad : quads) QVERIFY(path.contains(quad.boundingRect().center()));
}

void TextLineGeometryTest::rotatedAndReversedQuads()
{
    const QList<QPolygonF> quads {
        {{10, 20}, {30, 20}, {30, 30}, {10, 30}},
        {{34, 30}, {55, 30}, {55, 20}, {34, 20}}, // Opposite winding.
        {{10, 35}, {30, 35}, {30, 45}, {10, 45}},
    };
    for (const qreal angle : {0., 90., 180., 270., 31.}) {
        QTransform transform;
        transform.rotate(angle);
        QList<QPolygonF> rotated;
        for (const auto &quad : quads) rotated.append(transform.map(quad));
        rotated.append(rotated.first()); // Duplicate geometry must not compound opacity.
        const auto path = Thoughts::textLinePath(rotated);
        QCOMPARE(path.toSubpathPolygons().size(), 2);
        QVERIFY(path.contains(transform.map(QPointF(32, 25))));
        QVERIFY(!path.contains(transform.map(QPointF(20, 33))));
    }
    const QPolygonF skewed{{10, 10}, {30, 12}, {28, 22}, {8, 20}};
    const auto unchanged = Thoughts::textLineQuads({skewed});
    QCOMPARE(unchanged.size(), 1);
    QCOMPARE(unchanged.first(), skewed);
}

void TextLineGeometryTest::distantAndInvalidRegions()
{
    const QList<QRectF> rects {QRectF(10, 20, 20, 10), QRectF(34, 20, 20, 10), QRectF(100, 20, 20, 10),
                               QRectF(10, 33, 30, 10), QRectF(200, -75, 100, 200), QRectF()};
    const auto merged = Thoughts::mergeTextLineRects(rects);
    QCOMPARE(merged.size(), 4);
    QVERIFY(merged.contains(QRectF(10, 20, 44, 10)));
    QVERIFY(merged.contains(QRectF(10, 33, 30, 10)));
    QVERIFY(merged.contains(QRectF(200, -75, 100, 200)));
    const QList<QRectF> normalized {QRectF(.1, .2, .1, .02), QRectF(.21, .2, .1, .02)};
    QCOMPARE(Thoughts::normalizedTextLineRects(normalized, QSizeF(2000, 500)).size(), 2);
    QCOMPARE(Thoughts::normalizedTextLineRects(normalized, QSizeF(500, 2000)).size(), 1);
}

QTEST_GUILESS_MAIN(TextLineGeometryTest)
#include "textlinegeometrytest.moc"
