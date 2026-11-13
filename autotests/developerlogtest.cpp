// SPDX-License-Identifier: GPL-2.0-or-later
#include "../shell/developerlog.h"
#include "../shell/developerlogdialog.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTreeView>
#include <QStandardItemModel>
#include <QLineEdit>
#include <thread>

using Thoughts::DeveloperLog;
Q_NEVER_INLINE void deliberateDeveloperLogStall()
{
    QThread::msleep(3000);
    QThread::yieldCurrentThread(); // Keep this identifiable frame in the sample.
}

class DeveloperLogTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase()
    {
        QCoreApplication::setOrganizationName(QStringLiteral("ThoughtsDeveloperLogTest"));
        QCoreApplication::setApplicationName(QStringLiteral("ThoughtsDeveloperLogTest"));
        QSettings::setDefaultFormat(QSettings::IniFormat);
        QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    }
    void capturesActivityWithoutPrivateMessageText()
    {
        QTemporaryDir dir;
        DeveloperLog::Options options; options.directory = dir.path();
        {
            DeveloperLog logger(options);
            QTRY_VERIFY(DeveloperLog::available());
            DeveloperLog::setVerbose(true);
            QMessageLogger("/private/path/test.cpp", 123, "test", "thoughts.test").warning("Bearer secret-token and document body text");
            const QString id = DeveloperLog::documentId(QStringLiteral("/private/my-paper.pdf"));
            QCOMPARE(id.size(), 16);
            QCOMPARE(id, DeveloperLog::documentId(QStringLiteral("/private/my-paper.pdf")));
            { Thoughts::DeveloperActivity activity(QStringLiteral("document.open"), {{QStringLiteral("document_id"), id}}); }
            QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("document.open.finished")));
            const QString log = DeveloperLog::diagnostics();
            QVERIFY(log.contains(QStringLiteral("qt.message")));
            QVERIFY(log.contains(QStringLiteral("test.cpp")));
            QVERIFY(!log.contains(QStringLiteral("secret-token")));
            QVERIFY(!log.contains(QStringLiteral("document body")));
            QVERIFY(!log.contains(QStringLiteral("/private/")));
            DeveloperLog::setVerbose(false);
        }
        QFile file(QDir(dir.path()).filePath(QStringLiteral("developer-current.jsonl")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("app.stopped"));
    }
    void followsPointerSelectionAndRedactsTyping()
    {
        QTemporaryDir dir;
        DeveloperLog::Options options; options.directory = dir.path(); options.captureStacks = false;
        DeveloperLog logger(options);
        QTRY_VERIFY(DeveloperLog::available());
        DeveloperLog::setVerbose(false); // Reproduction tracing must work by default.
        QTreeView table;
        table.setObjectName(QStringLiteral("commentsOverviewTable"));
        table.setWindowTitle(QStringLiteral("private document title"));
        table.setSelectionBehavior(QAbstractItemView::SelectRows);
        QStandardItemModel model(3, 1);
        model.setData(model.index(0, 0), QStringLiteral("private comment body"));
        table.setModel(&model);
        table.show();
        QTest::qWait(30);
        const QPoint point = table.visualRect(model.index(0, 0)).center();
        QTest::mouseClick(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QTest::mouseDClick(table.viewport(), Qt::LeftButton, Qt::NoModifier, point);
        QLineEdit editor;
        editor.setObjectName(QStringLiteral("private object name"));
        editor.show();
        QTest::keyClicks(&editor, QStringLiteral("secret-token"));
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("ui.input.after_dispatch")));
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("key.release")));
        const QString log = DeveloperLog::diagnostics();
        QVERIFY(log.contains(QStringLiteral("pointer.press")));
        QVERIFY(log.contains(QStringLiteral("pointer.release")));
        QVERIFY(log.contains(QStringLiteral("pointer.double_click")));
        QVERIFY(log.contains(QStringLiteral("commentsOverviewTable")));
        QVERIFY(!log.contains(QStringLiteral("private document title")));
        QVERIFY(!log.contains(QStringLiteral("private comment body")));
        QVERIFY(!log.contains(QStringLiteral("private object name")));
        QVERIFY(!log.contains(QStringLiteral("secret-token")));
        QSet<qint64> before;
        bool selectedAfter = false;
        for (const QString &line : log.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
            const auto entry = QJsonDocument::fromJson(line.toUtf8()).object();
            const auto data = entry.value(QStringLiteral("data")).toObject();
            QVERIFY(entry.value(QStringLiteral("sequence")).toInteger() > 0);
            if (entry.value(QStringLiteral("event")) == QLatin1String("ui.input.before")) {
                before.insert(data.value(QStringLiteral("input_id")).toInteger());
            } else if (entry.value(QStringLiteral("event")) == QLatin1String("ui.input.after_dispatch")) {
                QVERIFY(before.contains(data.value(QStringLiteral("input_id")).toInteger()));
                selectedAfter |= data.value(QStringLiteral("selected_count")).toInt() == 1;
            }
        }
        QVERIFY(selectedAfter);
    }
    void detectsBlockedMainThreadAndRecovery()
    {
        QTemporaryDir dir;
        DeveloperLog::Options options; options.directory = dir.path(); options.heartbeatMs = 20; options.stallMs = 120; options.captureStacks = false;
        DeveloperLog logger(options);
        QTRY_VERIFY(DeveloperLog::available());
        QTest::qWait(80);
        // Deliberately block this real GUI thread while the independent logger runs.
        QTest::qSleep(350);
        QVERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("ui.stalled")));
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("ui.recovered")));
    }
    void capturesActualBlockedFunction()
    {
#ifndef Q_OS_MACOS
        QSKIP("Native stack sampling is implemented for macOS.");
#else
        QTemporaryDir dir;
        for (int i = 0; i < 6; ++i) {
            QFile old(dir.filePath(QStringLiteral("freeze-old-%1.txt").arg(i)));
            QVERIFY(old.open(QIODevice::WriteOnly)); old.write("old report");
        }
        DeveloperLog::Options options; options.directory = dir.path(); options.heartbeatMs = 20; options.stallMs = 120;
        options.captureStacks = true;
        DeveloperLog logger(options);
        QTRY_VERIFY(DeveloperLog::available());
        QTest::qWait(80);
        deliberateDeveloperLogStall();
        QTRY_VERIFY_WITH_TIMEOUT(DeveloperLog::diagnostics().contains(QStringLiteral("ui.stack_capture.finished")), 15000);
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("ui.recovered")));
        const QString path = DeveloperLog::latestFreezeReport();
        QFile report(path); QVERIFY(report.open(QIODevice::ReadOnly));
        const QByteArray contents = report.readAll();
        QVERIFY(contents.contains("deliberateDeveloperLogStall"));
        QVERIFY(contents.contains("com.apple.main-thread"));
        QVERIFY(contents.contains("Thoughts freeze ID:"));
        QVERIFY(!contents.contains(QDir::homePath().toUtf8()));
        QVERIFY(contents.size() <= 2 * 1024 * 1024);
        QVERIFY(!(report.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("freeze-*.txt")}, QDir::Files).size() <= 5);
        QVERIFY(DeveloperLog::diagnosticsWithFreezeReport().contains(QStringLiteral("deliberateDeveloperLogStall")));
        const QString exported = qEnvironmentVariable("THOUGHTS_STACK_TEST_REPORT");
        if (!exported.isEmpty()) { QFile::remove(exported); QVERIFY(QFile::copy(path, exported)); }
        QTest::qWait(100);
        QTest::qSleep(300);
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("cooldown")));
        QCOMPARE(DeveloperLog::diagnostics().count(QStringLiteral("ui.stack_capture.started")), 1);
        QWidget parent;
        Thoughts::showDeveloperLogs(&parent);
        auto *dialog = parent.findChild<QDialog *>(QStringLiteral("developerLogsDialog"));
        QVERIFY(dialog);
        auto *open = dialog->findChild<QPushButton *>(QStringLiteral("developerLogsOpenFreezeReport"));
        QVERIFY(open); QVERIFY(open->isEnabled()); open->click();
        auto *reportDialog = dialog->findChild<QDialog *>(QStringLiteral("developerFreezeReportDialog"));
        QVERIFY(reportDialog); QVERIFY(reportDialog->isVisible());
        QVERIFY(reportDialog->findChild<QPlainTextEdit *>()->toPlainText().contains(QStringLiteral("deliberateDeveloperLogStall")));
        for (auto *button : dialog->findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Copy diagnostics")) { button->click(); }
        }
        QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("deliberateDeveloperLogStall")));
        reportDialog->close(); dialog->close();
#endif
    }
    void captureFailureIsVisibleAndWatchdogRecovers()
    {
        QTemporaryDir dir;
        DeveloperLog::Options options; options.directory = dir.path(); options.heartbeatMs = 20; options.stallMs = 120;
        options.captureStacks = true; options.samplerProgram = dir.filePath(QStringLiteral("nonexistent-sampler"));
        DeveloperLog logger(options);
        QTRY_VERIFY(DeveloperLog::available());
        QTest::qWait(80); QTest::qSleep(350);
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("sampler_failed_to_start")));
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("ui.recovered")));
        QVERIFY(DeveloperLog::latestFreezeReport().isEmpty());
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.pending")}, QDir::Files).isEmpty());
    }
    void samplerTimeoutDoesNotBlockRecovery()
    {
#ifndef Q_OS_UNIX
        QSKIP("The fake sampler uses a Unix shell.");
#else
        QTemporaryDir dir;
        QFile helper(dir.filePath(QStringLiteral("sampler")));
        QVERIFY(helper.open(QIODevice::WriteOnly));
        helper.write("#!/bin/sh\nexec /bin/sleep 20\n");
        helper.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        helper.close();
        DeveloperLog::Options options; options.directory = dir.path(); options.heartbeatMs = 20; options.stallMs = 120;
        options.captureStacks = true; options.samplerProgram = helper.fileName(); options.captureTimeoutMs = 250;
        DeveloperLog logger(options);
        QTRY_VERIFY(DeveloperLog::available());
        QTest::qWait(80); QTest::qSleep(650);
        QTRY_VERIFY_WITH_TIMEOUT(DeveloperLog::diagnostics().contains(QStringLiteral("sampler_timeout")), 2000);
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("ui.recovered")));
        QVERIFY(DeveloperLog::latestFreezeReport().isEmpty());
        QVERIFY(QDir(dir.path()).entryList({QStringLiteral("*.pending")}, QDir::Files).isEmpty());
#endif
    }
    void boundsFilesWithConcurrentWriters()
    {
        QTemporaryDir dir;
        DeveloperLog::Options options; options.directory = dir.path(); options.fileBytes = 2048; options.fileCount = 3;
        {
            DeveloperLog logger(options);
            QTRY_VERIFY(DeveloperLog::available());
            std::vector<std::thread> writers;
            for (int n = 0; n < 4; ++n) {
                writers.emplace_back([n] {
                    for (int i = 0; i < 300; ++i) { DeveloperLog::record(QStringLiteral("test.concurrent"), {{QStringLiteral("worker"), n}, {QStringLiteral("index"), i}}); }
                });
            }
            for (auto &writer : writers) { writer.join(); }
        }
        const auto files = QDir(dir.path()).entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files);
        QVERIFY(files.size() <= 3);
        QVERIFY(files.size() >= 2);
        for (const auto &info : files) {
            QVERIFY(info.size() <= 2048);
#ifndef Q_OS_WIN
            // Windows files have no group or other permission bits.
            QVERIFY(!(info.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther)));
#endif
            QFile file(info.absoluteFilePath()); QVERIFY(file.open(QIODevice::ReadOnly));
            while (!file.atEnd()) { QVERIFY(QJsonDocument::fromJson(file.readLine()).isObject()); }
        }
    }
    void unavailableStorageDoesNotBlockTheApp()
    {
        QTemporaryDir dir;
        QFile blocker(dir.filePath(QStringLiteral("file"))); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.close();
        DeveloperLog::Options options; options.directory = blocker.fileName() + QStringLiteral("/logs");
        DeveloperLog logger(options);
        QTest::qWait(100);
        QVERIFY(!DeveloperLog::available());
        QElapsedTimer elapsed; elapsed.start();
        for (int i = 0; i < 2000; ++i) { DeveloperLog::record(QStringLiteral("test.disk_unavailable")); }
        QVERIFY(elapsed.elapsed() < 2000);
    }
    void viewerShowsAndCopiesDiagnostics()
    {
        QTemporaryDir dir;
        DeveloperLog::Options options; options.directory = dir.path();
        DeveloperLog logger(options);
        QTRY_VERIFY(DeveloperLog::available());
        DeveloperLog::record(QStringLiteral("test.viewer"));
        DeveloperLog::record(QStringLiteral("library.load.stage"), {{QStringLiteral("stage"), QStringLiteral("source bundles")},
            {QStringLiteral("duration_ms"), 42}, {QStringLiteral("count"), 20}});
        QTRY_VERIFY(DeveloperLog::diagnostics().contains(QStringLiteral("test.viewer")));
        QWidget parent;
        Thoughts::showDeveloperLogs(&parent, [] {
            return QVariantMap {{QStringLiteral("tracked_paths"), 4000}, {QStringLiteral("background_sources"), 300},
                {QStringLiteral("watched_files"), 300}, {QStringLiteral("watched_directories"), 80},
                {QStringLiteral("inventory_read_ms"), 21}, {QStringLiteral("first_scan_ms"), 35}};
        });
        auto *dialog = parent.findChild<QDialog *>(QStringLiteral("developerLogsDialog"));
        QVERIFY(dialog); QVERIFY(dialog->isVisible());
        auto *text = dialog->findChild<QPlainTextEdit *>(QStringLiteral("developerLogsText")); QVERIFY(text); QVERIFY(text->isReadOnly());
        QVERIFY(text->toPlainText().contains(QStringLiteral("test.viewer")));
        auto *performance = dialog->findChild<QPlainTextEdit *>(QStringLiteral("developerPerformanceText"));
        QVERIFY(performance); QVERIFY(performance->isReadOnly());
        QVERIFY(performance->toPlainText().contains(QStringLiteral("source bundles")));
        QVERIFY(performance->toPlainText().contains(QStringLiteral("42 ms")));
        QVERIFY(performance->toPlainText().contains(QStringLiteral("saved paths")));
        QVERIFY(performance->toPlainText().contains(QStringLiteral("21 ms")));
        auto *verbose = dialog->findChild<QCheckBox *>(); QVERIFY(verbose);
        verbose->setChecked(true); QVERIFY(DeveloperLog::verbose());
        verbose->setChecked(false); QVERIFY(!DeveloperLog::verbose());
        QPushButton *copy = nullptr;
        for (auto *button : dialog->findChildren<QPushButton *>()) { if (button->text() == QStringLiteral("Copy diagnostics")) { copy = button; } }
        QVERIFY(copy); copy->click();
        QVERIFY(QApplication::clipboard()->text().contains(QStringLiteral("test.viewer")));
        Thoughts::showDeveloperLogs(&parent);
        QCOMPARE(parent.findChildren<QDialog *>(QStringLiteral("developerLogsDialog")).size(), 1);
        QTest::qWait(100);
        const QString screenshot = qEnvironmentVariable("THOUGHTS_DEVELOPER_LOG_TEST_SCREENSHOT");
        if (!screenshot.isEmpty()) { QVERIFY(dialog->grab().save(screenshot)); }
        dialog->close();
    }
private:
    QTemporaryDir settings;
};
QTEST_MAIN(DeveloperLogTest)
#include "developerlogtest.moc"
