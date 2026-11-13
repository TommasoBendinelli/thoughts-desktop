// SPDX-License-Identifier: GPL-2.0-or-later
// Sync's three-way text merge: git's merge code compiled into the publisher
// (tools/publisher/xdiff). The expected bytes are what `git merge-file -p
// -L FEDORA_CURRENT -L COMMON_BASE -L PHONE_INCOMING current base incoming`
// prints, as Fedora runs it; tools/publisher/tests/merge_differential.py
// compares thousands more cases with git itself.
#include "../tools/publisher/pyerror.h"
#include "../tools/publisher/sourcemerge.h"

#include <QTest>

using Thoughts::Publisher::mergeSources;

class SourceMergeTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void merges_data()
    {
        QTest::addColumn<QByteArray>("current");
        QTest::addColumn<QByteArray>("base");
        QTest::addColumn<QByteArray>("incoming");
        QTest::addColumn<QByteArray>("merged");
        QTest::addColumn<int>("conflicts");
        QTest::newRow("clean") << QByteArray("one\nTWO\nthree\nfour\nfive\n") << QByteArray("one\ntwo\nthree\nfour\nfive\n")
                               << QByteArray("one\ntwo\nthree\nFOUR\nfive\n") << QByteArray("one\nTWO\nthree\nFOUR\nfive\n") << 0;
        QTest::newRow("only current changed") << QByteArray("a\nB\n") << QByteArray("a\nb\n") << QByteArray("a\nb\n") << QByteArray("a\nB\n") << 0;
        QTest::newRow("only incoming changed") << QByteArray("a\nb\n") << QByteArray("a\nb\n") << QByteArray("a\nB\n") << QByteArray("a\nB\n") << 0;
        QTest::newRow("same change") << QByteArray("a\nB\nc\n") << QByteArray("a\nb\nc\n") << QByteArray("a\nB\nc\n") << QByteArray("a\nB\nc\n") << 0;
        // git's default style shows no base section; the labels are the server's.
        QTest::newRow("conflict") << QByteArray("one\nTWO MAC\nthree\n") << QByteArray("one\ntwo\nthree\n") << QByteArray("one\nTWO PHONE\nthree\n")
                                  << QByteArray("one\n<<<<<<< FEDORA_CURRENT\nTWO MAC\n=======\nTWO PHONE\n>>>>>>> PHONE_INCOMING\nthree\n") << 1;
        QTest::newRow("two conflicts") << QByteArray("a\nB1\nk1\nk2\nk3\nk4\nD1\ne\n") << QByteArray("a\nb\nk1\nk2\nk3\nk4\nd\ne\n")
                                       << QByteArray("a\nB2\nk1\nk2\nk3\nk4\nD2\ne\n")
                                       << QByteArray("a\n<<<<<<< FEDORA_CURRENT\nB1\n=======\nB2\n>>>>>>> PHONE_INCOMING\nk1\nk2\nk3\nk4\n"
                                                     "<<<<<<< FEDORA_CURRENT\nD1\n=======\nD2\n>>>>>>> PHONE_INCOMING\ne\n")
                                       << 2;
        // XDL_MERGE_ZEALOUS_ALNUM joins conflicts separated only by lines without letters or digits.
        QTest::newRow("joined conflicts") << QByteArray("a\nB1\n}\n}\n}\n}\nD1\ne\n") << QByteArray("a\nb\n}\n}\n}\n}\nd\ne\n")
                                          << QByteArray("a\nB2\n}\n}\n}\n}\nD2\ne\n")
                                          << QByteArray("a\n<<<<<<< FEDORA_CURRENT\nB1\n}\n}\n}\n}\nD1\n=======\nB2\n}\n}\n}\n}\nD2\n>>>>>>> PHONE_INCOMING\ne\n")
                                          << 1;
        // git's ctype ignores the locale: byte 0xE9 is no letter, although macOS's
        // isalnum() says it is in a UTF-8 locale.
        QTest::newRow("latin-1 byte is no letter")
            << QByteArray("a\nB1\n\xe9\n\xe9\n\xe9\n\xe9\nD1\ne\n") << QByteArray("a\nb\n\xe9\n\xe9\n\xe9\n\xe9\nd\ne\n")
            << QByteArray("a\nB2\n\xe9\n\xe9\n\xe9\n\xe9\nD2\ne\n")
            << QByteArray("a\n<<<<<<< FEDORA_CURRENT\nB1\n\xe9\n\xe9\n\xe9\n\xe9\nD1\n=======\nB2\n\xe9\n\xe9\n\xe9\n\xe9\nD2\n>>>>>>> PHONE_INCOMING\ne\n") << 1;
        QTest::newRow("crlf markers") << QByteArray("a\r\nB1\r\nc\r\n") << QByteArray("a\r\nb\r\nc\r\n") << QByteArray("a\r\nB2\r\nc\r\n")
                                      << QByteArray("a\r\n<<<<<<< FEDORA_CURRENT\r\nB1\r\n=======\r\nB2\r\n>>>>>>> PHONE_INCOMING\r\nc\r\n") << 1;
        QTest::newRow("no final newline") << QByteArray("a\nB1") << QByteArray("a\nb") << QByteArray("a\nB2")
                                          << QByteArray("a\n<<<<<<< FEDORA_CURRENT\nB1\n=======\nB2\n>>>>>>> PHONE_INCOMING\n") << 1;
        QTest::newRow("both add to empty") << QByteArray("x\n") << QByteArray() << QByteArray("y\n")
                                           << QByteArray("<<<<<<< FEDORA_CURRENT\nx\n=======\ny\n>>>>>>> PHONE_INCOMING\n") << 1;
    }

    void merges()
    {
        QFETCH(QByteArray, current);
        QFETCH(QByteArray, base);
        QFETCH(QByteArray, incoming);
        QFETCH(QByteArray, merged);
        QFETCH(int, conflicts);
        const QPair<QByteArray, int> result = mergeSources(base, current, incoming, QStringLiteral("docs-a/manual.tex"));
        QCOMPARE(result.first, merged);
        QCOMPARE(result.second, conflicts);
        QVERIFY(!result.first.contains("COMMON_BASE"));
    }

    void conflictCountIsCappedAt127()
    {
        QByteArray base, current, incoming;
        for (int index = 0; index < 200; ++index) {
            base += "line " + QByteArray::number(index) + "\nkeep 1\nkeep 2\nkeep 3\nkeep 4\n";
            current += "current " + QByteArray::number(index) + "\nkeep 1\nkeep 2\nkeep 3\nkeep 4\n";
            incoming += "incoming " + QByteArray::number(index) + "\nkeep 1\nkeep 2\nkeep 3\nkeep 4\n";
        }
        const QPair<QByteArray, int> result = mergeSources(base, current, incoming, QStringLiteral("a.md"));
        QCOMPARE(result.first.count("<<<<<<< FEDORA_CURRENT\n"), 200);
        QCOMPARE(result.second, 127);
    }

    void binaryIsRefused_data()
    {
        QTest::addColumn<QByteArray>("current");
        QTest::addColumn<QByteArray>("base");
        QTest::addColumn<QByteArray>("incoming");
        QTest::addColumn<QString>("message");
        QTest::newRow("current") << QByteArray("a\0\n", 3) << QByteArray("a\n") << QByteArray("b\n") << QStringLiteral("error: Cannot merge binary files: current");
        QTest::newRow("base") << QByteArray("a\n") << QByteArray("a\0\n", 3) << QByteArray("b\n") << QStringLiteral("error: Cannot merge binary files: base");
        QTest::newRow("incoming") << QByteArray("a\n") << QByteArray("c\n") << QByteArray("b\0\n", 3)
                                  << QStringLiteral("error: Cannot merge binary files: incoming");
    }

    void binaryIsRefused()
    {
        QFETCH(QByteArray, current);
        QFETCH(QByteArray, base);
        QFETCH(QByteArray, incoming);
        QFETCH(QString, message);
        try {
            mergeSources(base, current, incoming, QStringLiteral("docs-a/manual.tex"));
            QFAIL("binary input merged");
        } catch (const Thoughts::Py::Exception &error) {
            QCOMPARE(error.type, Thoughts::Py::sourceInfrastructureErrorType);
            QCOMPARE(error.message, message);
        }
    }

    // A NUL after git's first 8000 bytes does not make a file binary.
    void lateNulIsText()
    {
        const QByteArray base = QByteArray(8000, 'x') + QByteArray("\0\nend\n", 6);
        const QPair<QByteArray, int> result = mergeSources(base, base + "current\n", base, QStringLiteral("a.md"));
        QCOMPARE(result.first, base + "current\n");
        QCOMPARE(result.second, 0);
        const QPair<QByteArray, int> both = mergeSources(base, "top\n" + base, base + "bottom\n", QStringLiteral("a.md"));
        QCOMPARE(both.first, "top\n" + base + "bottom\n");
        QCOMPARE(both.second, 0);
    }
};

QTEST_GUILESS_MAIN(SourceMergeTest)
#include "sourcemergetest.moc"
