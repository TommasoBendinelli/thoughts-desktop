// SPDX-License-Identifier: GPL-2.0-or-later
#include "part/noteattachment.h"
#include "part/thoughtsplatform.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

using namespace Thoughts::NoteAttachment;

class NoteAttachmentTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void names_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("safe");
        QTest::addColumn<bool>("safeOnWindows");
        const auto row = [](const char *title, const QString &name, bool safe, bool safeOnWindows) {
            QTest::newRow(title) << name << safe << safeOnWindows;
        };
        row("plain", QStringLiteral("report.pdf"), true, true);
        row("spaces and accents", QStringLiteral("Résumé (final) 2.pdf"), true, true);
        row("other scripts", QStringLiteral("報告書 отчёт.pdf"), true, true);
        row("emoji", QStringLiteral("plan \U0001F4C4.txt"), true, true);
        row("no extension", QStringLiteral("README"), true, true);
        row("hidden", QStringLiteral(".notes.txt"), true, true);
        row("empty", QString(), false, false);
        row("dot", QStringLiteral("."), false, false);
        row("dot dot", QStringLiteral(".."), false, false);
        row("slash", QStringLiteral("a/b.pdf"), false, false);
        row("parent", QStringLiteral("../b.pdf"), false, false);
        row("backslash", QStringLiteral("a\\b.pdf"), false, false);
        row("too long", QString(256, QLatin1Char('a')), false, false);
        row("longest", QString(251, QLatin1Char('a')) + QStringLiteral(".pdf"), true, true);
        // Control characters.
        row("newline", QStringLiteral("a\nb.pdf"), false, false);
        row("tab", QStringLiteral("a\tb.pdf"), false, false);
        row("null", QStringLiteral("a") + QChar(0) + QStringLiteral(".pdf"), false, false);
        row("escape", QStringLiteral("a\u001b[31m.pdf"), false, false);
        row("C1 control", QStringLiteral("a\u0085.pdf"), false, false);
        // Format characters (Cf).
        row("right-to-left override", QStringLiteral("report‮fdp.exe"), false, false);
        row("left-to-right override", QStringLiteral("report‭.pdf"), false, false);
        row("right-to-left embedding", QStringLiteral("report‫.pdf"), false, false);
        row("right-to-left isolate", QStringLiteral("report⁧.pdf"), false, false);
        row("pop directional isolate", QStringLiteral("report⁩.pdf"), false, false);
        row("right-to-left mark", QStringLiteral("report‏.pdf"), false, false);
        row("arabic letter mark", QStringLiteral("report؜.pdf"), false, false);
        row("zero width space", QStringLiteral("report​.pdf"), false, false);
        row("zero width joiner", QStringLiteral("report‍.pdf"), false, false);
        row("word joiner", QStringLiteral("report⁠.pdf"), false, false);
        row("soft hyphen", QStringLiteral("re­port.pdf"), false, false);
        row("byte order mark", QStringLiteral("﻿report.pdf"), false, false);
        row("tag characters", QStringLiteral("report\U000E0065.pdf"), false, false);
        row("line separator", QStringLiteral("report .pdf"), false, false);
        row("paragraph separator", QStringLiteral("report .pdf"), false, false);
        row("unpaired high surrogate", QStringLiteral("report") + QChar(0xD83D) + QStringLiteral(".pdf"), false, false);
        row("unpaired low surrogate", QStringLiteral("report") + QChar(0xDCC4) + QStringLiteral(".pdf"), false, false);
        // Names Windows would change or read as something else.
        row("colon", QStringLiteral("notes.txt:hidden.exe"), true, false);
        row("question mark", QStringLiteral("what?.pdf"), true, false);
        row("trailing dot", QStringLiteral("setup.exe."), true, false);
        row("trailing space", QStringLiteral("setup.pdf "), true, false);
        row("device", QStringLiteral("NUL.txt"), true, false);
        row("device without extension", QStringLiteral("com1"), true, false);
    }

    void names()
    {
        QFETCH(QString, name);
        QFETCH(bool, safe);
        QFETCH(bool, safeOnWindows);
        QCOMPARE(nameIsSafe(name, false), safe);
        QCOMPARE(nameIsSafe(name, true), safeOnWindows);
        // What is shown never contains what a name may not.
        QVERIFY(nameIsSafe(QStringLiteral("x") + displayName(name), false) || name.contains(QLatin1Char('/')) || name.contains(QLatin1Char('\\')) || name.size() > 254);
    }

    void shownNameCannotBeReordered()
    {
        QCOMPARE(displayName(QStringLiteral("report‮fdp.exe")), QStringLiteral("report�fdp.exe"));
        QCOMPARE(displayName(QStringLiteral("a\nb")), QStringLiteral("a�b"));
        QCOMPARE(displayName(QStringLiteral("plan \U0001F4C4.txt")), QStringLiteral("plan \U0001F4C4.txt"));
    }

    void types_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("opens");
        for (const char *suffix : {"pdf", "png", "jpg", "jpeg", "gif", "webp", "txt", "md", "tex", "bib", "csv", "json"}) {
            QTest::newRow(suffix) << QStringLiteral("file.") + QLatin1String(suffix) << true;
        }
        QTest::newRow("upper case") << QStringLiteral("SCAN.PDF") << true;
        QTest::newRow("mixed case") << QStringLiteral("photo.JpEg") << true;
        QTest::newRow("several dots") << QStringLiteral("archive.tar.pdf") << true;
        QTest::newRow("hidden text") << QStringLiteral(".txt") << true;
        for (const char *suffix : {"exe",  "app", "command", "sh",   "zsh",     "py",  "js",  "jar",  "bat",   "cmd",      "ps1",    "vbs",  "scr",  "lnk",  "url",         "webloc",
                                   "html", "htm", "svg",     "xml",  "dmg",     "pkg", "zip", "gz",   "iso",   "msi",      "docx",   "docm", "xlsx", "xlsm", "terminal",    "workflow",
                                   "scpt", "rtf", "pages",   "heic", "tiff",    "bmp", "ico", "yaml", "plist", "inetloc", "desktop", "dll",  "dylib", "so",  "applescript", "action",
                                   "tool", "mobileconfig", "ics", "vcf", "eml", "torrent", "pdf ", "pdf.", "p df"}) {
            QTest::newRow(suffix) << QStringLiteral("file.") + QLatin1String(suffix) << false;
        }
        QTest::newRow("no extension") << QStringLiteral("README") << false;
        QTest::newRow("ends with a dot") << QStringLiteral("report.") << false;
        QTest::newRow("allowed name, other type") << QStringLiteral("pdf") << false;
        QTest::newRow("double extension") << QStringLiteral("report.pdf.exe") << false;
        QTest::newRow("application bundle") << QStringLiteral("Report.pdf.app") << false;
        QTest::newRow("alternate data stream") << QStringLiteral("notes.exe:x.pdf") << true; // refused as a name on Windows
        QTest::newRow("empty") << QString() << false;
    }

    void types()
    {
        QFETCH(QString, name);
        QFETCH(bool, opens);
        QCOMPARE(actionFor(name) == Action::Open, opens);
    }

    void onlyTheTwelvePassiveTypesOpen()
    {
        QCOMPARE(passiveSuffixes(), QStringList({QStringLiteral("pdf"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("webp"),
                                                 QStringLiteral("txt"), QStringLiteral("md"), QStringLiteral("tex"), QStringLiteral("bib"), QStringLiteral("csv"), QStringLiteral("json")}));
        // The name Windows would open as a program is not a name there at all.
        QVERIFY(!nameIsSafe(QStringLiteral("notes.exe:x.pdf"), true));
    }

    void savedFilesAreMarkedAsDownloaded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString path = directory.filePath(QStringLiteral("attachment.zip"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("PK");
        file.close();
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
        QVERIFY(!Thoughts::Platform::isMarkedDownloaded(path));
        QVERIFY(Thoughts::Platform::markDownloaded(path));
        QVERIFY(Thoughts::Platform::isMarkedDownloaded(path));
        // Marking again keeps the mark.
        QVERIFY(Thoughts::Platform::markDownloaded(path));
        QVERIFY(Thoughts::Platform::isMarkedDownloaded(path));
        QVERIFY(!Thoughts::Platform::markDownloaded(directory.filePath(QStringLiteral("missing.zip"))));
#else
        QVERIFY(Thoughts::Platform::markDownloaded(path));
#endif
#ifdef Q_OS_MACOS
        // The attribute has the form browsers write: flags;time;agent;event.
        QProcess xattr;
        xattr.start(QStringLiteral("/usr/bin/xattr"), {QStringLiteral("-p"), QStringLiteral("com.apple.quarantine"), path});
        QVERIFY(xattr.waitForFinished(5000));
        const QList<QByteArray> fields = xattr.readAllStandardOutput().trimmed().split(';');
        QCOMPARE(fields.size(), 4);
        QCOMPARE(fields.at(0), QByteArray("0081"));
        bool ok = false;
        QVERIFY(fields.at(1).toLongLong(&ok, 16) > 0 && ok);
        QCOMPARE(fields.at(2), QByteArray("Thoughts"));
#endif
    }
};

QTEST_GUILESS_MAIN(NoteAttachmentTest)
#include "noteattachmenttest.moc"
