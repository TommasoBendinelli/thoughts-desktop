// SPDX-License-Identifier: GPL-2.0-or-later
#include "developerlogdialog.h"
#include "developerlog.h"
#include <KLocalizedString>
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QLabel>
#include <QFontDatabase>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QTabWidget>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>

namespace Thoughts
{
void showDeveloperLogs(QWidget *parent, std::function<QVariantMap()> sourceTracking)
{
    if (auto *existing = parent->findChild<QDialog *>(QStringLiteral("developerLogsDialog"))) {
        existing->show(); existing->raise(); existing->activateWindow(); return;
    }
    auto *dialog = new QDialog(parent);
    dialog->setObjectName(QStringLiteral("developerLogsDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(i18n("Developer logs"));
    dialog->resize(850, 560);
    auto *layout = new QVBoxLayout(dialog);
    auto *description = new QLabel(i18n("Local user actions, selection changes, PDF opening and UI stalls of 3 seconds or longer. Automatic stack reports on macOS (at most one per 30 seconds). Up to 20 MiB of event logs and 10 MiB of stack reports.\nDocument contents, credentials and raw Qt message text are excluded; Qt warnings retain their severity and code location."), dialog);
    description->setWordWrap(true);
    layout->addWidget(description);
    auto *location = new QLabel(DeveloperLog::directory(), dialog);
    location->setTextInteractionFlags(Qt::TextSelectableByMouse);
    location->setWordWrap(true);
    layout->addWidget(location);
    auto *status = new QLabel(dialog);
    status->setObjectName(QStringLiteral("developerLogsStatus"));
    layout->addWidget(status);
    auto *verbose = new QCheckBox(i18n("Verbose logging (additional Qt debug locations)"), dialog);
    verbose->setChecked(DeveloperLog::verbose());
    QObject::connect(verbose, &QCheckBox::toggled, dialog, [](bool value) { DeveloperLog::setVerbose(value); });
    layout->addWidget(verbose);
    auto *tabs = new QTabWidget(dialog);
    tabs->setObjectName(QStringLiteral("developerLogsTabs"));
    auto *performance = new QPlainTextEdit(tabs);
    performance->setObjectName(QStringLiteral("developerPerformanceText"));
    performance->setReadOnly(true);
    performance->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    performance->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs->addTab(performance, i18n("Performance"));
    auto *text = new QPlainTextEdit(dialog);
    text->setObjectName(QStringLiteral("developerLogsText"));
    text->setReadOnly(true);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    tabs->addTab(text, i18n("Events"));
    layout->addWidget(tabs, 1);
    auto *freezeReport = new QPushButton(i18n("Open latest freeze report"), dialog);
    freezeReport->setObjectName(QStringLiteral("developerLogsOpenFreezeReport"));
    QObject::connect(freezeReport, &QPushButton::clicked, dialog, [dialog] {
        QFile file(DeveloperLog::latestFreezeReport());
        if (!file.open(QIODevice::ReadOnly)) { return; }
        auto *reportDialog = new QDialog(dialog);
        reportDialog->setAttribute(Qt::WA_DeleteOnClose);
        reportDialog->setObjectName(QStringLiteral("developerFreezeReportDialog"));
        reportDialog->setWindowTitle(i18n("Freeze stack report"));
        reportDialog->resize(1000, 700);
        auto *layout = new QVBoxLayout(reportDialog);
        auto *report = new QPlainTextEdit(QString::fromUtf8(file.read(2 * 1024 * 1024)), reportDialog);
        report->setReadOnly(true);
        report->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        report->setLineWrapMode(QPlainTextEdit::NoWrap);
        layout->addWidget(report);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, reportDialog);
        QObject::connect(buttons, &QDialogButtonBox::rejected, reportDialog, &QDialog::close);
        layout->addWidget(buttons);
        reportDialog->show();
    });
    const auto refresh = [text, performance, status, freezeReport, sourceTracking] {
        freezeReport->setEnabled(!DeveloperLog::latestFreezeReport().isEmpty());
        status->setText(DeveloperLog::available() ? i18n("Recording locally · showing the latest 128 KiB") : i18n("Logging unavailable: check folder permissions, free disk space, or another running Thoughts instance."));
        const QJsonArray measured = DeveloperLog::performanceEvents();
        QList<QPair<qint64, QString>> stages;
        QStringList timeline;
        for (const QJsonValue &value : measured) {
            const QJsonObject item = value.toObject();
            const QString event = item.value(QStringLiteral("event")).toString();
            const QJsonObject data = item.value(QStringLiteral("data")).toObject();
            const QString time = item.value(QStringLiteral("time")).toString().mid(11, 12);
            const QString duration = data.contains(QStringLiteral("duration_ms"))
                ? i18n("%1 ms", data.value(QStringLiteral("duration_ms")).toInteger()) : QStringLiteral("—");
            QString label = event;
            if (event == QLatin1String("app.started")) label = i18n("App started");
            else if (event == QLatin1String("library.overview.shown")) label = i18n("Library window shown");
            else if (event == QLatin1String("library.reload.requested")) label = i18n("Library load requested");
            else if (event == QLatin1String("library.reload.queued")) label = i18n("Library load queued");
            else if (event == QLatin1String("library.catalog_changed")) label = i18n("Library inventory changed");
            else if (event == QLatin1String("library.reload.worker_finished")) label = i18n("Library data ready");
            else if (event == QLatin1String("library.load.started")) label = i18n("Reading Library data");
            else if (event == QLatin1String("library.load.finished")) label = i18n("Library data read");
            else if (event.startsWith(QLatin1String("library.catalog_signature"))) label = i18n("Checking Library files");
            else if (event.startsWith(QLatin1String("library.catalog_poll"))) label = i18n("Checking Library inventory");
            else if (event == QLatin1String("library.tree.deferred")) label = i18n("Library tree waits until shown");
            else if (event == QLatin1String("library.tree.stages")) label = i18n("Library tree build stages");
            else if (event.startsWith(QLatin1String("library.tree"))) label = i18n("Building Library tree");
            else if (event.startsWith(QLatin1String("comments.refresh"))) label = i18n("Updating visible comments");
            QString details;
            if (data.contains(QStringLiteral("stage"))) label = i18n("Library: %1", data.value(QStringLiteral("stage")).toString());
            if (data.contains(QStringLiteral("reason"))) details += i18n(" · reason: %1", data.value(QStringLiteral("reason")).toString());
            if (data.contains(QStringLiteral("full_discovery")) && data.value(QStringLiteral("full_discovery")).toBool()) details += i18n(" · discover sources");
            if (data.contains(QStringLiteral("already_running")) && data.value(QStringLiteral("already_running")).toBool()) details += i18n(" · deferred");
            if (data.contains(QStringLiteral("count"))) details += i18n(" · %1 items", data.value(QStringLiteral("count")).toInt());
            if (data.contains(QStringLiteral("load_id"))) details += i18n(" · load %1", data.value(QStringLiteral("load_id")).toInteger());
            if (data.contains(QStringLiteral("documents"))) details += i18n(" · %1 documents", data.value(QStringLiteral("documents")).toInt());
            if (data.contains(QStringLiteral("sources"))) details += i18n(" · %1 sources", data.value(QStringLiteral("sources")).toInt());
            if (data.contains(QStringLiteral("artifacts"))) details += i18n(" · %1 bundles", data.value(QStringLiteral("artifacts")).toInt());
            const QString line = QStringLiteral("%1  %2  %3%4").arg(time, duration.rightJustified(9), label,
                details.isEmpty() ? QString() : QStringLiteral("  ") + details);
            timeline.append(line);
            if (data.contains(QStringLiteral("duration_ms"))) stages.append({data.value(QStringLiteral("duration_ms")).toInteger(), line});
        }
        std::sort(stages.begin(), stages.end(), [](const auto &left, const auto &right) { return left.first > right.first; });
        QStringList slowest;
        for (const auto &stage : stages.mid(0, 10)) slowest.append(stage.second);
        QString performanceContent = i18n("Current launch · elapsed time for each measured step. A blank duration means the step started or was queued.\n");
        const QVariantMap tracking = sourceTracking ? sourceTracking() : QVariantMap();
        if (!tracking.isEmpty()) {
            const auto measuredMs = [&tracking](const QString &key) {
                const qint64 value = tracking.value(key, -1).toLongLong();
                return value < 0 ? QStringLiteral("—") : i18n("%1 ms", value);
            };
            performanceContent += QLatin1Char('\n') + i18n("Background source check") + QLatin1Char('\n')
                + i18n("%1 saved paths · %2 eligible sources · watching %3 files and %4 folders",
                    tracking.value(QStringLiteral("tracked_paths")).toInt(), tracking.value(QStringLiteral("background_sources")).toInt(),
                    tracking.value(QStringLiteral("watched_files")).toInt(), tracking.value(QStringLiteral("watched_directories")).toInt())
                + QLatin1Char('\n')
                + i18n("Opening: inventory read %1 · setup %2 · file scan %3 · database %4",
                    measuredMs(QStringLiteral("inventory_read_ms")), measuredMs(QStringLiteral("first_setup_ms")),
                    measuredMs(QStringLiteral("first_scan_ms")), measuredMs(QStringLiteral("first_store_ms")))
                + QLatin1Char('\n')
                + i18n("Latest check: scan %1 · database %2 · completion %3 · %4 files hashed",
                    measuredMs(QStringLiteral("scan_ms")), measuredMs(QStringLiteral("store_ms")),
                    measuredMs(QStringLiteral("finish_ms")), tracking.value(QStringLiteral("hashed_paths")).toInt())
                + QLatin1Char('\n') + (tracking.value(QStringLiteral("running")).toBool() ? i18n("Checking now") : i18n("Check idle"))
                + QLatin1Char('\n');
        }
        performanceContent += QLatin1Char('\n') + i18n("Slowest measured steps") + QLatin1Char('\n')
            + (slowest.isEmpty() ? i18n("No measurements yet.") : slowest.join(QLatin1Char('\n')))
            + QStringLiteral("\n\n") + i18n("Timeline") + QLatin1Char('\n') + timeline.join(QLatin1Char('\n'));
        if (performance->toPlainText() != performanceContent) performance->setPlainText(performanceContent);
        QString content;
        const auto lines = DeveloperLog::diagnostics().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            const auto entry = QJsonDocument::fromJson(line.toUtf8()).object();
            if (entry.isEmpty()) { content += line + QLatin1Char('\n'); continue; }
            content += entry.value(QStringLiteral("time")).toString() + QStringLiteral("  ")
                + entry.value(QStringLiteral("event")).toString() + QStringLiteral("  ")
                + QString::fromUtf8(QJsonDocument(entry.value(QStringLiteral("data")).toObject()).toJson(QJsonDocument::Compact)) + QLatin1Char('\n');
        }
        if (text->toPlainText() == content) { return; }
        auto *scroll = text->verticalScrollBar();
        const bool atEnd = scroll->value() == scroll->maximum();
        const int oldPosition = scroll->value();
        text->setPlainText(content);
        scroll->setValue(atEnd ? scroll->maximum() : oldPosition);
    };
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto *folder = buttons->addButton(i18n("Open folder"), QDialogButtonBox::ActionRole);
    buttons->addButton(freezeReport, QDialogButtonBox::ActionRole);
    auto *copy = buttons->addButton(i18n("Copy diagnostics"), QDialogButtonBox::ActionRole);
    QObject::connect(folder, &QPushButton::clicked, dialog, [status] {
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(DeveloperLog::directory()))) { status->setText(i18n("Could not open the folder. You can copy its path above.")); }
    });
    QObject::connect(copy, &QPushButton::clicked, dialog, [] { QApplication::clipboard()->setText(DeveloperLog::diagnosticsWithFreezeReport()); });
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);
    auto *timer = new QTimer(dialog);
    timer->setInterval(1000);
    QObject::connect(timer, &QTimer::timeout, dialog, refresh);
    timer->start();
    refresh();
    DeveloperLog::record(QStringLiteral("developer_logs.opened"));
    dialog->show();
}
}
