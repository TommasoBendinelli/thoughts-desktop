/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include <algorithm>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFile>
#include <QTest>

#include "../part/agentproposaloverlayutils.h"

namespace
{

QJsonObject fragment(const QString &filename, int page, double left, double top, double right, double bottom)
{
    return {
        {QStringLiteral("pdf_filename"), filename},
        {QStringLiteral("page"), page},
        {QStringLiteral("left"), left},
        {QStringLiteral("top"), top},
        {QStringLiteral("right"), right},
        {QStringLiteral("bottom"), bottom},
    };
}

}

class AgentProposalOverlayUtilsTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void preservesProseFragments();
    void joinsWordsOnTheSameRenderedLine();
    void keepsDistantFragmentsSeparate();
    void compactsStructuralFragmentsByPage();
    void mergesDenseProseByLine();
    void rejectsFragmentsForAnotherPdfOrInvalidPage();
    void mergesPublishedScreenshotGeometry();
    void preservesIdentityAndPageScale();
    void readsDuplicateFlag();
    void keepsDuplicatesApartWhenMerging();
    void anchorsMarkerOnOwnOccurrence();
};

void AgentProposalOverlayUtilsTest::preservesProseFragments()
{
    const QJsonArray fragments {
        fragment(QStringLiteral("paper.pdf"), 1, 72.0, 100.0, 280.0, 112.0),
        fragment(QStringLiteral("paper.pdf"), 1, 72.0, 114.0, 310.0, 126.0),
    };
    const auto overlays = Thoughts::agentProposalOverlays(QStringLiteral("proposal-1"),
                                                          QStringLiteral("after"),
                                                          QStringLiteral("Two ordinary lines of prose."),
                                                          fragments,
                                                          QStringLiteral("paper.pdf"),
                                                          2);

    QCOMPARE(overlays.size(), 2);
    QCOMPARE(static_cast<int>(overlays.at(0).style), static_cast<int>(Thoughts::AgentProposalOverlayStyle::Fragment));
    QCOMPARE(overlays.at(0).rect, QRectF(72.0, 100.0, 208.0, 12.0));
    QCOMPARE(overlays.at(1).rect, QRectF(72.0, 114.0, 238.0, 12.0));
}

void AgentProposalOverlayUtilsTest::joinsWordsOnTheSameRenderedLine()
{
    const QJsonArray fragments {
        fragment(QStringLiteral("paper.pdf"), 1, 72.0, 100.0, 112.0, 112.0),
        fragment(QStringLiteral("paper.pdf"), 1, 117.0, 100.0, 160.0, 112.0),
        fragment(QStringLiteral("paper.pdf"), 1, 166.0, 100.0, 230.0, 112.0),
    };
    const auto overlays = Thoughts::agentProposalOverlays(QStringLiteral("proposal-words"),
                                                          QStringLiteral("after"),
                                                          QStringLiteral("Three adjacent words."),
                                                          fragments,
                                                          QStringLiteral("paper.pdf"),
                                                          1);

    QCOMPARE(overlays.size(), 1);
    QCOMPARE(static_cast<int>(overlays.constFirst().style), static_cast<int>(Thoughts::AgentProposalOverlayStyle::Fragment));
    QCOMPARE(overlays.constFirst().rect, QRectF(72.0, 100.0, 158.0, 12.0));
}

void AgentProposalOverlayUtilsTest::keepsDistantFragmentsSeparate()
{
    const QJsonArray fragments {
        fragment(QStringLiteral("paper.pdf"), 1, 72.0, 100.0, 112.0, 112.0),
        fragment(QStringLiteral("paper.pdf"), 1, 190.0, 100.0, 230.0, 112.0),
    };
    const auto overlays = Thoughts::agentProposalOverlays(QStringLiteral("proposal-columns"),
                                                          QStringLiteral("after"),
                                                          QStringLiteral("Separate columns."),
                                                          fragments,
                                                          QStringLiteral("paper.pdf"),
                                                          1);

    QCOMPARE(overlays.size(), 2);
    QCOMPARE(overlays.at(0).rect, QRectF(72.0, 100.0, 40.0, 12.0));
    QCOMPARE(overlays.at(1).rect, QRectF(190.0, 100.0, 40.0, 12.0));
}

void AgentProposalOverlayUtilsTest::compactsStructuralFragmentsByPage()
{
    const QJsonArray fragments {
        fragment(QStringLiteral("paper.pdf"), 1, 72.0, 100.0, 280.0, 112.0),
        fragment(QStringLiteral("paper.pdf"), 1, 120.0, 300.0, 500.0, 340.0),
        fragment(QStringLiteral("paper.pdf"), 2, 80.0, 90.0, 420.0, 180.0),
    };
    const auto overlays = Thoughts::agentProposalOverlays(QStringLiteral("proposal-figure"),
                                                          QStringLiteral("after"),
                                                          QStringLiteral("\\begin{figure}[H] \\includegraphics{plot.pdf} \\end{figure}"),
                                                          fragments,
                                                          QStringLiteral("paper.pdf"),
                                                          2);

    QCOMPARE(overlays.size(), 2);
    QCOMPARE(static_cast<int>(overlays.at(0).style), static_cast<int>(Thoughts::AgentProposalOverlayStyle::Region));
    QCOMPARE(overlays.at(0).page, 0);
    QCOMPARE(overlays.at(0).rect, QRectF(72.0, 100.0, 428.0, 240.0));
    QCOMPARE(overlays.at(1).page, 1);
    QCOMPARE(overlays.at(1).rect, QRectF(80.0, 90.0, 340.0, 90.0));
}

void AgentProposalOverlayUtilsTest::mergesDenseProseByLine()
{
    QJsonArray fragments;
    for (int index = 0; index < 64; ++index) {
        const int line = index / 32;
        const int word = index % 32;
        const double left = 72.0 + word * 9.0;
        fragments.append(fragment(QStringLiteral("paper.pdf"), 1, left, 100.0 + line * 14.0, left + 7.0, 112.0 + line * 14.0));
    }
    const auto overlays = Thoughts::agentProposalOverlays(QStringLiteral("proposal-dense"),
                                                          QStringLiteral("after"),
                                                          QStringLiteral("A very long ordinary proposal."),
                                                          fragments,
                                                          QStringLiteral("paper.pdf"),
                                                          1);

    QCOMPARE(overlays.size(), 2);
    QCOMPARE(static_cast<int>(overlays.at(0).style), static_cast<int>(Thoughts::AgentProposalOverlayStyle::Fragment));
    QCOMPARE(overlays.at(0).rect, QRectF(72.0, 100.0, 286.0, 12.0));
    QCOMPARE(overlays.at(1).rect, QRectF(72.0, 114.0, 286.0, 12.0));
}

void AgentProposalOverlayUtilsTest::rejectsFragmentsForAnotherPdfOrInvalidPage()
{
    const QJsonArray fragments {
        fragment(QStringLiteral("other.pdf"), 1, 72.0, 100.0, 280.0, 112.0),
        fragment(QStringLiteral("paper.pdf"), 3, 72.0, 100.0, 280.0, 112.0),
    };
    const auto overlays = Thoughts::agentProposalOverlays(QStringLiteral("proposal-1"),
                                                          QStringLiteral("after"),
                                                          QStringLiteral("Text"),
                                                          fragments,
                                                          QStringLiteral("paper.pdf"),
                                                          2);

    QVERIFY(overlays.isEmpty());
}

void AgentProposalOverlayUtilsTest::mergesPublishedScreenshotGeometry()
{
    // Word geometry from the two reported screenshots, including mixed fonts,
    // a wrapped link, and a table whose columns must stay independent.
    QFile fixture(QStringLiteral(KDESRCDIR "data/proposal-highlight-lines.json"));
    QVERIFY(fixture.open(QIODevice::ReadOnly));
    const auto cases = QJsonDocument::fromJson(fixture.readAll()).array();
    for (const auto &value : cases) {
        const auto testCase = value.toObject();
        QList<Thoughts::AgentProposalOverlay> fragments;
        for (const auto &entry : testCase.value(QStringLiteral("regions")).toArray()) {
            const auto region = entry.toObject();
            const auto rect = region.value(QStringLiteral("rect")).toArray();
            fragments.append({testCase.value(QStringLiteral("case")).toString(), QStringLiteral("after"), region.value(QStringLiteral("page")).toInt(),
                              QRectF(rect[0].toDouble(), rect[1].toDouble(), rect[2].toDouble() - rect[0].toDouble(), rect[3].toDouble() - rect[1].toDouble()),
                              Thoughts::AgentProposalOverlayStyle::Fragment, true});
        }
        const auto merged = Thoughts::mergeAgentProposalOverlayLines(fragments, QList<QSizeF>(5, QSizeF(612, 792)));
        const bool table = testCase.value(QStringLiteral("case")).toString() == QLatin1String("paragraph-and-table");
        QCOMPARE(merged.size(), table ? 51 : 2);
        // Every word remains covered exactly once, without cross-line rectangles.
        for (const auto &fragment : fragments) {
            const QPointF center(fragment.rect.center().x() * 612, fragment.rect.center().y() * 792);
            int covering = 0;
            for (const auto &overlay : merged) {
                QVERIFY(!overlay.normalized);
                QVERIFY(overlay.rect.height() < 14);
                covering += overlay.rect.contains(center);
            }
            QCOMPARE(covering, 1);
        }
        // Reordering compiler locations must not change the rendered runs.
        std::reverse(fragments.begin(), fragments.end());
        const auto reversed = Thoughts::mergeAgentProposalOverlayLines(fragments, QList<QSizeF>(5, QSizeF(612, 792)));
        QCOMPARE(reversed.size(), merged.size());
        for (qsizetype index = 0; index < merged.size(); ++index) {
            QCOMPARE(reversed[index].rect, merged[index].rect);
        }
    }
}

void AgentProposalOverlayUtilsTest::preservesIdentityAndPageScale()
{
    using Thoughts::AgentProposalOverlayStyle;
    // On this wide page a normalized horizontal gap of .01 is 20 points,
    // exceeding the 10-point line height even though .01 < normalized height.
    const QList<Thoughts::AgentProposalOverlay> fragments {
        {QStringLiteral("one"), QStringLiteral("after"), 0, QRectF(.1, .2, .1, .02), AgentProposalOverlayStyle::Fragment, true},
        {QStringLiteral("one"), QStringLiteral("after"), 0, QRectF(.21, .2, .1, .02), AgentProposalOverlayStyle::Fragment, true},
        {QStringLiteral("one"), QStringLiteral("before"), 0, QRectF(.1, .2, .1, .02), AgentProposalOverlayStyle::Fragment, true},
        {QStringLiteral("two"), QStringLiteral("after"), 0, QRectF(.1, .2, .1, .02), AgentProposalOverlayStyle::Fragment, true},
        {QStringLiteral("one"), QStringLiteral("after"), 1, QRectF(.1, .2, .1, .02), AgentProposalOverlayStyle::Fragment, true},
        {QStringLiteral("one"), QStringLiteral("after"), 0, QRectF(.2, .2, .008, .008), AgentProposalOverlayStyle::Region, true},
    };
    const auto merged = Thoughts::mergeAgentProposalOverlayLines(fragments, {QSizeF(2000, 500), QSizeF(500, 2000)});
    QCOMPARE(merged.size(), fragments.size());
    QCOMPARE(merged[0].style, AgentProposalOverlayStyle::Region);
    QVERIFY(merged[0].normalized);
    QCOMPARE(merged[0].rect, fragments.last().rect);
}

void AgentProposalOverlayUtilsTest::readsDuplicateFlag()
{
    // The server marks LaTeX-made copies (contents entries, running heads);
    // older data has no key and stays a primary occurrence.
    QJsonObject copy = fragment(QStringLiteral("paper.pdf"), 1, 72.0, 100.0, 280.0, 112.0);
    copy.insert(QStringLiteral("duplicate"), true);
    QJsonObject explicitPrimary = fragment(QStringLiteral("paper.pdf"), 2, 72.0, 100.0, 280.0, 112.0);
    explicitPrimary.insert(QStringLiteral("duplicate"), false);
    const QJsonArray fragments {
        copy,
        explicitPrimary,
        fragment(QStringLiteral("paper.pdf"), 3, 72.0, 100.0, 280.0, 112.0),
    };
    const auto overlays = Thoughts::agentProposalOverlays(QStringLiteral("proposal-heading"),
                                                          QStringLiteral("after"),
                                                          QStringLiteral("\\section{Renamed heading}"),
                                                          fragments,
                                                          QStringLiteral("paper.pdf"),
                                                          3);

    QCOMPARE(overlays.size(), 3);
    for (const auto &overlay : overlays) {
        QCOMPARE(overlay.duplicate, overlay.page == 0);
    }

    const auto structural = Thoughts::agentProposalOverlays(QStringLiteral("proposal-figure"),
                                                            QStringLiteral("after"),
                                                            QStringLiteral("\\begin{figure}[H] \\includegraphics{plot.pdf} \\end{figure}"),
                                                            QJsonArray {copy, fragment(QStringLiteral("paper.pdf"), 1, 120.0, 300.0, 500.0, 340.0)},
                                                            QStringLiteral("paper.pdf"),
                                                            3);
    // A list-of-figures copy does not grow the figure's own region.
    QCOMPARE(structural.size(), 2);
    for (const auto &overlay : structural) {
        QCOMPARE(static_cast<int>(overlay.style), static_cast<int>(Thoughts::AgentProposalOverlayStyle::Region));
        QCOMPARE(overlay.rect, overlay.duplicate ? QRectF(72.0, 100.0, 208.0, 12.0) : QRectF(120.0, 300.0, 380.0, 40.0));
    }
    QVERIFY(structural.at(0).duplicate != structural.at(1).duplicate);
}

void AgentProposalOverlayUtilsTest::keepsDuplicatesApartWhenMerging()
{
    using Thoughts::AgentProposalOverlayStyle;
    // A running head and the heading itself on one line must not merge into
    // one fragment, or the pencil could anchor on the copy.
    const QList<Thoughts::AgentProposalOverlay> fragments {
        {QStringLiteral("one"), QStringLiteral("after"), 0, QRectF(72, 100, 100, 12), AgentProposalOverlayStyle::Fragment, false, true},
        {QStringLiteral("one"), QStringLiteral("after"), 0, QRectF(174, 100, 100, 12), AgentProposalOverlayStyle::Fragment, false, false},
    };
    const auto merged = Thoughts::mergeAgentProposalOverlayLines(fragments, {QSizeF(612, 792)});
    QCOMPARE(merged.size(), 2);
    QVERIFY(merged.at(0).duplicate != merged.at(1).duplicate);
    for (const auto &overlay : merged) {
        QCOMPARE(overlay.rect, overlay.duplicate ? fragments.at(0).rect : fragments.at(1).rect);
    }
}

void AgentProposalOverlayUtilsTest::anchorsMarkerOnOwnOccurrence()
{
    using Thoughts::AgentProposalOverlayStyle;
    const QString after = QStringLiteral("after");
    const auto overlay = [&after](const QString &id, int page, double top, bool duplicate, bool normalized = true) {
        return Thoughts::AgentProposalOverlay {id, after, page, QRectF(normalized ? .1 : 61.2, top, .3, .02), AgentProposalOverlayStyle::Fragment, normalized, duplicate};
    };
    const QList<Thoughts::AgentProposalOverlay> overlays {
        // A heading copied into the contents (page 1) and a running head above it.
        overlay(QStringLiteral("heading"), 2, .40, false),
        overlay(QStringLiteral("heading"), 0, .15, true),
        overlay(QStringLiteral("heading"), 2, .04, true),
        overlay(QStringLiteral("heading"), 3, .10, false),
        // Only copies: the earliest copy keeps today's anchor.
        overlay(QStringLiteral("copies"), 1, .60, true),
        overlay(QStringLiteral("copies"), 0, .60, true),
        // Data without the flag: the earliest fragment, in PDF points or normalized.
        overlay(QStringLiteral("legacy"), 1, .20, false),
        overlay(QStringLiteral("legacy"), 1, 79.2, false, false),
        // A page whose size is unknown never anchors.
        overlay(QStringLiteral("unsized"), 4, .10, false),
        overlay(QStringLiteral("unsized"), 5, .10, false),
    };
    QList<QSizeF> pageSizes(6, QSizeF(612, 792));
    pageSizes[4] = QSizeF();
    const auto anchors = Thoughts::agentProposalMarkerAnchors(overlays, pageSizes);
    QCOMPARE(anchors.size(), 4);
    QCOMPARE(anchors.value(QStringLiteral("heading")), 0);
    QCOMPARE(anchors.value(QStringLiteral("copies")), 5);
    QCOMPARE(anchors.value(QStringLiteral("legacy")), 7);
    QCOMPARE(anchors.value(QStringLiteral("unsized")), 9);
}

QTEST_GUILESS_MAIN(AgentProposalOverlayUtilsTest)

#include "agentproposaloverlayutilstest.moc"
