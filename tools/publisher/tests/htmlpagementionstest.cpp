// SPDX-License-Identifier: GPL-2.0-or-later
#include "../buildinputs.h"

#include <QElapsedTimer>
#include <QTest>

using Thoughts::Publisher::htmlPageMentions;

class HtmlPageMentionsTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void findsPageNames();
    void longRunsTakeLinearTime();
};

void HtmlPageMentionsTest::findsPageNames()
{
    using Mentions = QList<QPair<bool, QString>>;
    QCOMPARE(htmlPageMentions(QStringLiteral(R"(<a href="thoughts-html:docs-notes/tex/support/html/a.html#slide=2">A</a>)")),
             (Mentions{{true, QStringLiteral("docs-notes/tex/support/html/a.html")}}));
    QCOMPARE(htmlPageMentions(QStringLiteral("See pages/b.HTM and c.html, not d.htmlx or e.html_ or .html.")),
             (Mentions{{false, QStringLiteral("pages/b.HTM")}, {false, QStringLiteral("c.html")}}));
    // The longest mention of a run, as the regular expression gave.
    QCOMPARE(htmlPageMentions(QStringLiteral("x.html,y.html end")), (Mentions{{false, QStringLiteral("x.html,y.html")}}));
    QCOMPARE(htmlPageMentions(QStringLiteral("THOUGHTS-HTML:p.htm")), (Mentions{{true, QStringLiteral("p.htm")}}));
    QVERIFY(htmlPageMentions(QStringLiteral("thoughts-html:.html")).isEmpty());
}

void HtmlPageMentionsTest::longRunsTakeLinearTime()
{
    // A picture embedded as base64 is one run of 756,000 characters with no space or quote:
    // the regular expression this replaces took about 15 minutes over it (27 Nov 2026).
    QString text = QStringLiteral(R"(<img src="data:image/svg+xml;base64,)");
    text += QString(800000, QLatin1Char('A'));
    text += QStringLiteral(R"("><a href="thoughts-html:docs-notes/tex/support/html/next.html">next</a>)");
    QElapsedTimer timer;
    timer.start();
    const auto mentions = htmlPageMentions(text);
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(mentions.size(), 1);
    QCOMPARE(mentions.first().second, QStringLiteral("docs-notes/tex/support/html/next.html"));

    // Many short runs: each is searched on its own, never back to the start of the text.
    QString words;
    for (int i = 0; i < 300000; ++i) {
        words += QStringLiteral("word ");
    }
    words += QStringLiteral("last.html");
    timer.restart();
    const auto found = htmlPageMentions(words);
    QVERIFY2(timer.elapsed() < 2000, qPrintable(QString::number(timer.elapsed())));
    QCOMPARE(found.size(), 1);
    QCOMPARE(found.first().second, QStringLiteral("last.html"));
}

QTEST_GUILESS_MAIN(HtmlPageMentionsTest)
#include "htmlpagementionstest.moc"
