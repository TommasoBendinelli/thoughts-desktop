// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/buildterminallauncher.h"
#include "../shell/powershellquote.h"

#include <QTest>

namespace
{
// How PowerShell reads a single-quoted string literal that starts at
// `start`: any of its five single-quote characters ends the string unless
// another one follows, and then the second one is the text. Returns the
// string's value and where the literal ends, or -1 if it never ends.
struct Literal {
    QString value;
    qsizetype end = -1;
};

Literal readPowerShellLiteral(const QString &script, qsizetype start)
{
    Literal literal;
    if (start >= script.size() || !Thoughts::isPowerShellSingleQuote(script.at(start))) {
        return literal;
    }
    for (qsizetype index = start + 1; index < script.size(); ++index) {
        const QChar character = script.at(index);
        if (Thoughts::isPowerShellSingleQuote(character)) {
            if (index + 1 < script.size() && Thoughts::isPowerShellSingleQuote(script.at(index + 1))) {
                literal.value += script.at(++index);
                continue;
            }
            literal.end = index + 1;
            return literal;
        }
        literal.value += character;
    }
    return literal;
}

QStringList hostileNames()
{
    const QString payload = QStringLiteral("; Start-Process calc; ");
    QStringList names {
        QStringLiteral("plain.pdf"),
        QStringLiteral("it's.pdf"),
        QStringLiteral("''.pdf"),
        QStringLiteral("$(Start-Process calc).pdf"),
        QStringLiteral("`n\"double\" $env:USERPROFILE.pdf"),
        QStringLiteral("%1 %2 %3 %4.pdf"),
    };
    for (const char16_t quote : {u'\u0027', u'\u2018', u'\u2019', u'\u201A', u'\u201B'}) {
        const QChar character(quote);
        names << QStringLiteral("a") + character + payload + character + QStringLiteral(".pdf");
        names << QString(character) + QStringLiteral(".pdf");
        names << QStringLiteral("end") + character;
        // A different quote character right after it must not pair with it.
        names << QStringLiteral("mixed") + character + QChar(u'\u2019') + payload + QChar(u'\u0027') + character + QStringLiteral(".pdf");
    }
    return names;
}
}

class PowerShellQuoteTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void everySingleQuoteCharacterIsKnown()
    {
        for (const char16_t quote : {u'\u0027', u'\u2018', u'\u2019', u'\u201A', u'\u201B'}) {
            QVERIFY(Thoughts::isPowerShellSingleQuote(QChar(quote)));
        }
        for (const char16_t other : {u'"', u'`', u'\u201C', u'\u201D', u'\u2032', u'a'}) {
            QVERIFY(!Thoughts::isPowerShellSingleQuote(QChar(other)));
        }
    }

    void quotedTextIsOneLiteral()
    {
        for (const QString &name : hostileNames()) {
            const QString quoted = Thoughts::powerShellQuote(name);
            const Literal literal = readPowerShellLiteral(quoted, 0);
            // The literal ends exactly where the quoted text ends, so nothing
            // of the name is left over for PowerShell to run.
            QCOMPARE(literal.end, quoted.size());
            QCOMPARE(literal.value, name);
        }
        QCOMPARE(Thoughts::powerShellQuote(QStringLiteral("it's")), QStringLiteral("'it''s'"));
        QCOMPARE(Thoughts::powerShellQuote(QStringLiteral("a\u2019b")), QStringLiteral("'a\u2019\u2019b'"));
        QCOMPARE(Thoughts::powerShellQuote(QString()), QStringLiteral("''"));
    }

    void buildScriptKeepsAFileNameInsideItsLiteral()
    {
        const QString watcher = QStringLiteral("C:/Program Files/Thoughts/bin/thoughts-watch-build.exe");
        const QString server = QStringLiteral("https://example.org");
        for (const QString &name : hostileNames()) {
            const QString script = Thoughts::BuildTerminalLauncher::windowsBuildTerminalScript(QStringLiteral("C:/Users/reader/AppData/Local/Thoughts/build-output/b1"), QStringLiteral("b1"), name, watcher, server);
            // The watcher's command line: every argument is one literal.
            const qsizetype call = script.indexOf(QStringLiteral("        & '"));
            QVERIFY2(call >= 0, qPrintable(name));
            qsizetype position = call + 10;
            QStringList arguments;
            while (position < script.size() && script.at(position) != QLatin1Char('\n')) {
                if (script.at(position) == QLatin1Char(' ')) {
                    ++position;
                    continue;
                }
                if (script.at(position) == QLatin1Char('-')) {
                    const qsizetype space = script.indexOf(QLatin1Char(' '), position);
                    arguments << script.mid(position, space - position);
                    position = space;
                    continue;
                }
                const Literal literal = readPowerShellLiteral(script, position);
                QVERIFY2(literal.end > position, qPrintable(name));
                arguments << literal.value;
                position = literal.end;
            }
            QCOMPARE(arguments, QStringList({watcher, QStringLiteral("--server"), server, QStringLiteral("--build-id"), QStringLiteral("b1"), QStringLiteral("--target"), name}));

            // The window title holds the name too.
            const qsizetype title = script.indexOf(QStringLiteral("$Title = "));
            QVERIFY(title >= 0);
            const Literal titleLiteral = readPowerShellLiteral(script, title + 9);
            QCOMPARE(titleLiteral.value, QStringLiteral("Thoughts build output - ") + name);
            QCOMPARE(script.at(titleLiteral.end), QLatin1Char('\n'));
        }
    }
};

QTEST_GUILESS_MAIN(PowerShellQuoteTest)
#include "powershellquotetest.moc"
