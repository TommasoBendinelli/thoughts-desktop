// SPDX-License-Identifier: GPL-2.0-or-later
// On screen the Mac app calls the machine its server runs on "the server",
// never by its name. This test reads the sources of shell/ and part/ and the
// .ui, .rc, .desktop and .plist files there, and fails on a text that names
// Fedora. A lowercase "fedora" is a key or protocol value (fedora_moves,
// keep=fedora), and log lines and identifiers are not on screen, so those
// pass.
#include <QDirIterator>
#include <QFile>
#include <QRegularExpression>
#include <QSet>
#include <QTest>

#include <algorithm>

namespace
{
struct Literal {
    qsizetype position = 0;
    QStringView text;
};

bool namesFedora(QStringView text)
{
    return text.contains(u"Fedora") || text.contains(u"FEDORA");
}

int lineAt(QStringView source, qsizetype position)
{
    return int(source.left(position).count(u'\n')) + 1;
}

// A literal in the statement of a qDebug/qInfo/qWarning/... reaches only the log.
bool inLogStatement(QStringView source, qsizetype position)
{
    qsizetype start = position;
    while (start > 0 && source.at(start - 1) != u';' && source.at(start - 1) != u'{' && source.at(start - 1) != u'}') {
        --start;
    }
    static const QRegularExpression log(QStringLiteral("\\bq(C?Debug|C?Info|C?Warning|C?Critical|Fatal)\\b"));
    return log.matchView(source.mid(start, position - start)).hasMatch();
}

// String literals of a C++ source, outside comments and character literals.
QList<Literal> literals(QStringView source)
{
    QList<Literal> result;
    const qsizetype n = source.size();
    qsizetype i = 0;
    while (i < n) {
        const QChar c = source.at(i);
        const QStringView two = source.mid(i, 2);
        if (two == u"//") {
            const qsizetype end = source.indexOf(u'\n', i);
            i = end < 0 ? n : end;
            continue;
        }
        if (two == u"/*") {
            const qsizetype end = source.indexOf(u"*/", i + 2);
            i = end < 0 ? n : end + 2;
            continue;
        }
        if (two == u"R\"" && (i == 0 || !(source.at(i - 1).isLetterOrNumber() || source.at(i - 1) == u'_'))) {
            const qsizetype open = source.indexOf(u'(', i + 2);
            if (open > 0) {
                const QString delimiter = QLatin1Char(')') + source.mid(i + 2, open - i - 2).toString() + QLatin1Char('"');
                const qsizetype end = source.indexOf(delimiter, open + 1);
                if (end > 0) {
                    result.append({i, source.mid(open + 1, end - open - 1)});
                    i = end + delimiter.size();
                    continue;
                }
            }
        }
        if (c == u'"' || c == u'\'') {
            qsizetype j = i + 1;
            while (j < n && source.at(j) != c) {
                j += source.at(j) == u'\\' ? 2 : 1;
            }
            if (c == u'"') {
                result.append({i, source.mid(i + 1, std::min(j, n) - i - 1)});
            }
            i = j + 1;
            continue;
        }
        ++i;
    }
    return result;
}

// Literals that name Fedora without showing it: they read what the server or
// the source publisher writes, or what earlier builds saved.
const QSet<QString> &allowed()
{
    static const QSet<QString> texts {
        // A server journal message the Sync log rewords (shell/documentsyncpresentation.cpp).
        QStringLiteral(R"(^Reusing (?:Fedora|server|the server's) build ([A-Za-z0-9-]+)\\.?$)"),
        // Source publisher progress, hidden as internal activity.
        QStringLiteral("Checking the current Fedora source frontier"),
        // Sync journals saved by earlier builds.
        QStringLiteral("Fedora accepted the conditional PDF update."),
    };
    return texts;
}

QStringList cppProblems(const QString &relativePath, const QString &source)
{
    // An object, method or type name (thoughtsOverviewFedoraStorage, FedoraRepository), not a
    // word on its own: "Fedora" can be the last piece of a text split over several literals.
    static const QRegularExpression identifier(QStringLiteral("^(?=.*(?:[a-z][A-Z]|[_:]))[A-Za-z_][A-Za-z0-9_:]*$"));
    QStringList problems;
    for (const Literal &literal : literals(source)) {
        if (!namesFedora(literal.text) || identifier.matchView(literal.text).hasMatch() || allowed().contains(literal.text.toString())
            || inLogStatement(source, literal.position)) {
            continue;
        }
        problems.append(QStringLiteral("%1:%2: %3").arg(relativePath).arg(lineAt(source, literal.position)).arg(literal.text.left(160)));
    }
    return problems;
}

QStringList markupProblems(const QString &relativePath, const QString &contents)
{
    QRegularExpression pattern;
    if (relativePath.endsWith(QLatin1String(".ui")) || relativePath.endsWith(QLatin1String(".plist.in"))) {
        pattern = QRegularExpression(QStringLiteral("<string[^>]*>(.*?)</string>"), QRegularExpression::DotMatchesEverythingOption);
    } else if (relativePath.endsWith(QLatin1String(".rc"))) {
        pattern = QRegularExpression(QStringLiteral("<(text|tooltip|whatsthis)[^>]*>(.*?)</\\1>"), QRegularExpression::DotMatchesEverythingOption);
    } else {
        pattern = QRegularExpression(QStringLiteral("^(Name|GenericName|Comment|Keywords)(?:\\[[^\\]]*\\])?=(.*)$"), QRegularExpression::MultilineOption);
    }
    const int group = relativePath.endsWith(QLatin1String(".ui")) || relativePath.endsWith(QLatin1String(".plist.in")) ? 1 : 2;
    QStringList problems;
    for (auto it = pattern.globalMatch(contents); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        if (match.captured(group).contains(QLatin1String("fedora"), Qt::CaseInsensitive)) {
            problems.append(QStringLiteral("%1:%2: %3").arg(relativePath).arg(lineAt(contents, match.capturedStart(group))).arg(match.captured(group).left(160)));
        }
    }
    return problems;
}
}

class ServerWordingTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void scannerFindsOnScreenTexts();
    void shellAndPartNeverShowFedora();
};

void ServerWordingTest::scannerFindsOnScreenTexts()
{
    const QString source = QStringLiteral(R"cpp(
        // A comment about Fedora is fine.
        label->setText(i18n("Sync with "
                            "Fedora"));
        button->setToolTip(i18nc("@info", "Keep Fedora's version"));
        error = QStringLiteral("Fedora returned an invalid sync state.");
        script = QStringLiteral(R"(print -r -- "Fedora")");
        qWarning() << "Could not persist Fedora work" << path;
        QMetaObject::invokeMethod(part, "fedoraManualSyncState");
        widget->setObjectName(QStringLiteral("thoughtsOverviewFedoraDetailsDialog"));
        const QString key = QStringLiteral("fedora_moves");
        const QChar quote = '"';
        summary->setText(i18n("Fedora is healthy."));
        filter = QStringLiteral("Checking the current Fedora source frontier");
    )cpp");
    const QStringList problems = cppProblems(QStringLiteral("example.cpp"), source);
    QVERIFY2(problems.size() == 5, qPrintable(problems.join(QLatin1Char('\n'))));
    QCOMPARE(problems.at(0), QStringLiteral("example.cpp:4: Fedora"));
    QVERIFY(problems.at(1).startsWith(QLatin1String("example.cpp:5: Keep Fedora's version")));
    QVERIFY(problems.at(2).startsWith(QLatin1String("example.cpp:6: Fedora returned")));
    QVERIFY(problems.at(3).startsWith(QLatin1String("example.cpp:7: print -r")));
    QVERIFY(problems.at(4).startsWith(QLatin1String("example.cpp:13: Fedora is healthy.")));

    QCOMPARE(markupProblems(QStringLiteral("dialog.ui"), QStringLiteral("<property name=\"text\">\n <string>Fedora storage</string>\n</property>")).size(), 1);
    QCOMPARE(markupProblems(QStringLiteral("part.rc"), QStringLiteral("<Action name=\"update_from_fedora\"/>\n<text>Sync</text>")).size(), 0);
    QCOMPARE(markupProblems(QStringLiteral("part.rc"), QStringLiteral("<Menu><text>Fedora</text></Menu>")).size(), 1);
    QCOMPARE(markupProblems(QStringLiteral("app.desktop"), QStringLiteral("Exec=fedora\nComment[de]=Mit Fedora abgleichen\nName=Thoughts\n")).size(), 1);
}

void ServerWordingTest::shellAndPartNeverShowFedora()
{
    static const QStringList cppSuffixes {QStringLiteral(".cpp"), QStringLiteral(".h"), QStringLiteral(".mm")};
    static const QStringList markupSuffixes {QStringLiteral(".ui"), QStringLiteral(".rc"), QStringLiteral(".desktop"), QStringLiteral(".plist.in")};
    const auto endsWithAny = [](const QString &path, const QStringList &suffixes) {
        return std::any_of(suffixes.cbegin(), suffixes.cend(), [&path](const QString &suffix) {
            return path.endsWith(suffix);
        });
    };
    QStringList problems;
    int cppFiles = 0;
    int markupFiles = 0;
    for (const QString &directory : {QStringLiteral("shell"), QStringLiteral("part")}) {
        const QString root = QStringLiteral(KDESRCDIR "../") + directory;
        QVERIFY2(QFileInfo(root).isDir(), qPrintable(root));
        for (QDirIterator it(root, QDir::Files, QDirIterator::Subdirectories); it.hasNext();) {
            const QString path = it.next();
            const bool cpp = endsWithAny(path, cppSuffixes);
            if (!cpp && !endsWithAny(path, markupSuffixes)) {
                continue;
            }
            QFile file(path);
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(path));
            const QString contents = QString::fromUtf8(file.readAll());
            const QString relativePath = directory + path.mid(root.size());
            if (cpp) {
                ++cppFiles;
                problems += cppProblems(relativePath, contents);
            } else {
                ++markupFiles;
                problems += markupProblems(relativePath, contents);
            }
        }
    }
    // The sources were found: an empty scan would pass for the wrong reason.
    QVERIFY2(cppFiles > 100, qPrintable(QString::number(cppFiles)));
    QVERIFY2(markupFiles >= 4, qPrintable(QString::number(markupFiles)));
    QVERIFY2(problems.isEmpty(), qPrintable(QStringLiteral("On-screen text names Fedora; say \"the server\":\n") + problems.join(QLatin1Char('\n'))));
}

QTEST_GUILESS_MAIN(ServerWordingTest)
#include "serverwordingtest.moc"
