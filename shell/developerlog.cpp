// SPDX-License-Identifier: GPL-2.0-or-later
#include "developerlog.h"

#include <QCoreApplication>
#include <QApplication>
#include <QAbstractItemView>
#include <QAction>
#include <QActionEvent>
#include <QInputEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QWheelEvent>
#include <QPointer>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QSettings>
#include <QProcess>
#include <QSaveFile>
#include <functional>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>
#include <QUuid>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace Thoughts
{
namespace
{
std::mutex registryMutex;
DeveloperLog *active = nullptr;
QtMessageHandler previousHandler = nullptr;
qint64 monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
void messageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    // Free-form Qt messages can contain document text, URLs or tokens. Keep only
    // diagnostic location and severity, including in verbose mode.
    const QString severity = type == QtDebugMsg ? QStringLiteral("debug") : type == QtInfoMsg ? QStringLiteral("info") : type == QtWarningMsg ? QStringLiteral("warning") : type == QtCriticalMsg ? QStringLiteral("critical") : QStringLiteral("fatal");
    DeveloperLog::record(QStringLiteral("qt.message"), {{QStringLiteral("severity"), severity},
        {QStringLiteral("category"), QString::fromLatin1(context.category ? context.category : "default").left(128)},
        {QStringLiteral("source"), QFileInfo(QString::fromUtf8(context.file ? context.file : "")).fileName()},
        {QStringLiteral("line"), context.line}}, type == QtDebugMsg || type == QtInfoMsg);
    if (previousHandler) {
        previousHandler(type, context, message);
    }
}
}

struct DeveloperLog::Private
{
    Options options;
    QString filePath;
    QString session = QUuid::createUuid().toString(QUuid::WithoutBraces);
    QByteArray identitySalt = QUuid::createUuid().toByteArray();
    std::atomic<bool> verbose {false};
    mutable std::atomic<quint64> sequence {0};
    quint64 inputSequence = 0;
    std::atomic<bool> available {false};
    std::atomic<qint64> heartbeat {monotonicMs()};
    std::atomic<qint64> lastGap {0};
    std::mutex mutex;
    std::condition_variable condition;
    std::deque<QByteArray> queue;
    QJsonArray performanceEntries;
    quint64 dropped = 0;
    bool stopping = false;
    std::thread worker;
    QTimer timer;
    std::unique_ptr<QProcess> sampler;
    QString captureId;
    QString capturePath;
    qint64 captureStarted = 0;
    qint64 lastCaptureStarted = 0;
    static constexpr qint64 reportBytes = 2 * 1024 * 1024;
    static constexpr int reportCount = 5;
    using Write = std::function<void(const QByteArray &)>;

    void startCapture(const QString &stallId, const Write &write)
    {
        const auto skipped = [&](const QString &reason) {
            write(entry(QStringLiteral("ui.stack_capture.skipped"), {{QStringLiteral("stall_id"), stallId}, {QStringLiteral("reason"), reason}}));
        };
        if (!options.captureStacks) { skipped(QStringLiteral("unsupported_or_disabled")); return; }
        if (sampler) { skipped(QStringLiteral("capture_in_progress")); return; }
        const qint64 now = monotonicMs();
        if (lastCaptureStarted && now - lastCaptureStarted < options.captureCooldownMs) { skipped(QStringLiteral("cooldown")); return; }
        if (!available) { skipped(QStringLiteral("log_storage_unavailable")); return; }
        QDir folder(options.directory);
        // Reports are separate from rotating event logs: five reports, 2 MiB
        // each, plus at most one bounded capture in progress.
        auto files = folder.entryInfoList({QStringLiteral("freeze-*.txt")}, QDir::Files, QDir::Time | QDir::Reversed);
        while (files.size() >= reportCount) { QFile::remove(files.takeFirst().absoluteFilePath()); }
        for (const auto &pending : folder.entryList({QStringLiteral("freeze-*.pending")}, QDir::Files)) { folder.remove(pending); }
        captureId = stallId;
        capturePath = folder.filePath(QStringLiteral("freeze-%1-%2.txt").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz")), stallId));
        QFile pending(capturePath + QStringLiteral(".pending"));
        if (!pending.open(QIODevice::WriteOnly)) { skipped(QStringLiteral("report_file_unavailable")); return; }
        pending.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        pending.close();
        sampler = std::make_unique<QProcess>();
        sampler->setProcessChannelMode(QProcess::MergedChannels);
        sampler->setProgram(options.samplerProgram);
        sampler->setArguments({QString::number(QCoreApplication::applicationPid()), QStringLiteral("1"), QStringLiteral("1"), QStringLiteral("-file"), pending.fileName()});
        captureStarted = lastCaptureStarted = now;
        write(entry(QStringLiteral("ui.stack_capture.started"), {{QStringLiteral("stall_id"), stallId}, {QStringLiteral("report"), QFileInfo(capturePath).fileName()}}));
        sampler->start();
        if (!sampler->waitForStarted(1000)) {
            write(entry(QStringLiteral("ui.stack_capture.failed"), {{QStringLiteral("stall_id"), stallId}, {QStringLiteral("reason"), QStringLiteral("sampler_failed_to_start")}}));
            sampler.reset();
            QFile::remove(pending.fileName());
        }
    }

    void pollCapture(const Write &write, bool stopping = false)
    {
        if (!sampler) { return; }
        sampler->waitForFinished(0); // Pump process completion without waiting on the GUI.
        const QString pendingPath = capturePath + QStringLiteral(".pending");
        QString failure;
        if (stopping) { failure = QStringLiteral("app_shutdown"); }
        else if (QFileInfo(pendingPath).size() > reportBytes) { failure = QStringLiteral("report_size_limit"); }
        else if (monotonicMs() - captureStarted > options.captureTimeoutMs) { failure = QStringLiteral("sampler_timeout"); }
        if (!failure.isEmpty() && sampler->state() != QProcess::NotRunning) {
            sampler->kill();
            sampler->waitForFinished(1000);
        }
        if (sampler->state() != QProcess::NotRunning) { return; }
        const int exitCode = sampler->exitCode();
        if (failure.isEmpty() && (sampler->exitStatus() != QProcess::NormalExit || exitCode != 0)) { failure = QStringLiteral("sampler_failed"); }
        QFile input(pendingPath);
        QByteArray report;
        if (failure.isEmpty() && input.open(QIODevice::ReadOnly)) { report = input.read(reportBytes); input.close(); }
        // Keep code locations, symbols, image UUIDs and addresses. Never dump
        // process memory or arguments; redact the home directory in code paths.
        report.replace(QDir::homePath().toUtf8(), QByteArray("~"));
        if (failure.isEmpty() && !report.contains("Call graph:")) { failure = QStringLiteral("invalid_stack_report"); }
        if (failure.isEmpty()) {
            QSaveFile output(capturePath);
            if (!output.open(QIODevice::WriteOnly)) { failure = QStringLiteral("report_write_failed"); }
            else {
                output.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                const QByteArray header = QByteArray("Thoughts freeze ID: ") + captureId.toUtf8() + "\n";
                report = header + report.left(reportBytes - header.size());
                if (output.write(report) != report.size() || !output.commit()) { failure = QStringLiteral("report_write_failed"); }
            }
        }
        QJsonObject metadata {{QStringLiteral("stall_id"), captureId}, {QStringLiteral("capture_duration_ms"), monotonicMs() - captureStarted}};
        if (failure.isEmpty()) {
            metadata.insert(QStringLiteral("report"), QFileInfo(capturePath).fileName());
            write(entry(QStringLiteral("ui.stack_capture.finished"), metadata));
        } else {
            metadata.insert(QStringLiteral("reason"), failure);
            metadata.insert(QStringLiteral("exit_code"), exitCode);
            write(entry(QStringLiteral("ui.stack_capture.failed"), metadata));
        }
        QFile::remove(pendingPath);
        sampler.reset();
    }

    QByteArray entry(const QString &event, QJsonObject metadata = {}) const
    {
        QJsonObject object {{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
            {QStringLiteral("session"), session}, {QStringLiteral("pid"), QCoreApplication::applicationPid()},
            {QStringLiteral("sequence"), qint64(++sequence)},
            {QStringLiteral("event"), event}, {QStringLiteral("data"), metadata}};
        return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
    }
    void enqueue(QByteArray line)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (queue.size() >= 1024 || line.size() > qMin<qint64>(8192, options.fileBytes)) {
            ++dropped;
            return;
        }
        queue.push_back(std::move(line));
        condition.notify_one();
    }
    void run()
    {
        QDir folder(options.directory);
        const bool ready = folder.mkpath(QStringLiteral("."));
        if (ready) {
            QFile::setPermissions(options.directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        }
        // One writer across app instances; secondary command-forwarding launches
        // must never rotate or damage the running application's logs.
        QLockFile writerLock(folder.filePath(QStringLiteral("writer.lock")));
        writerLock.setStaleLockTime(0);
        const bool owns = ready && writerLock.tryLock(0);
        QFile file(filePath);
        auto openFile = [&] {
            if (!owns) { return false; }
            if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) { return false; }
            file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            return true;
        };
        auto prune = [&] {
            auto files = folder.entryInfoList({QStringLiteral("developer-*.jsonl")}, QDir::Files, QDir::Time | QDir::Reversed);
            while (files.size() >= options.fileCount) {
                const QFileInfo oldest = files.takeFirst();
                if (oldest.absoluteFilePath() != filePath) { QFile::remove(oldest.absoluteFilePath()); }
            }
        };
        if (owns) { prune(); }
        available = openFile();
        auto write = [&](const QByteArray &line) {
            if (!file.isOpen()) { available = openFile(); }
            if (!file.isOpen()) { return; }
            if (file.size() + line.size() > options.fileBytes && file.size() > 0) {
                file.close();
                prune();
                const QString archive = folder.filePath(QStringLiteral("developer-%1-%2.jsonl").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMddTHHmmsszzz")), QUuid::createUuid().toString(QUuid::Id128).left(8)));
                if (!QFile::rename(filePath, archive)) { available = false; return; }
                available = openFile();
                if (!file.isOpen()) { return; }
            }
            available = file.write(line) == line.size() && file.flush();
        };
        bool stalled = false;
        QString stallId;
        qint64 lastPoll = monotonicMs();
        while (true) {
            std::deque<QByteArray> batch;
            quint64 lost = 0;
            bool stop = false;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait_for(lock, std::chrono::milliseconds(options.heartbeatMs), [&] { return stopping || !queue.empty(); });
                batch.swap(queue);
                lost = dropped;
                dropped = 0;
                stop = stopping;
            }
            const qint64 now = monotonicMs();
            const qint64 gap = now - heartbeat.load();
            // A suspended machine or process is not evidence of a UI freeze.
            if (now - lastPoll > qMax(5000, options.stallMs * 2)) {
                write(entry(QStringLiteral("watchdog.scheduling_gap"), {{QStringLiteral("duration_ms"), now - lastPoll}}));
                heartbeat = now;
                stalled = false;
            } else if (gap >= options.stallMs && !stalled) {
                stalled = true;
                stallId = QUuid::createUuid().toString(QUuid::Id128);
                write(entry(QStringLiteral("ui.stalled"), {{QStringLiteral("unresponsive_ms"), gap}, {QStringLiteral("stall_id"), stallId}}));
                if (!stop) { startCapture(stallId, write); }
            } else if (gap < options.stallMs && stalled) {
                stalled = false;
                write(entry(QStringLiteral("ui.recovered"), {{QStringLiteral("duration_ms"), lastGap.load()}, {QStringLiteral("stall_id"), stallId}}));
            }
            lastPoll = now;
            for (const auto &line : batch) { write(line); }
            if (lost) { write(entry(QStringLiteral("logger.dropped"), {{QStringLiteral("count"), qint64(lost)}})); }
            pollCapture(write, stop);
            if (stop) { break; }
        }
        available = false;
    }
};

DeveloperLog::Options DeveloperLog::defaultOptions()
{
    Options options;
    options.directory = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("developer-logs"));
    return options;
}
DeveloperLog::DeveloperLog(const Options &options, QObject *parent) : QObject(parent), d(std::make_unique<Private>())
{
    d->options = options;
    d->options.fileBytes = qMax<qint64>(1024, options.fileBytes);
    d->options.fileCount = qMax(2, options.fileCount);
    d->options.heartbeatMs = qMax(10, options.heartbeatMs);
    d->options.stallMs = qMax(d->options.heartbeatMs * 3, options.stallMs);
    d->filePath = QDir(options.directory).filePath(QStringLiteral("developer-current.jsonl"));
    d->verbose = QSettings().value(QStringLiteral("DeveloperLogs/verbose"), false).toBool();
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        Q_ASSERT(!active);
        active = this;
        previousHandler = qInstallMessageHandler(messageHandler);
    }
    d->worker = std::thread([this] { d->run(); });
    connect(&d->timer, &QTimer::timeout, this, [this] {
        const qint64 now = monotonicMs();
        const qint64 gap = now - d->heartbeat.exchange(now);
        if (gap >= d->options.stallMs) { d->lastGap = gap; }
    });
    d->timer.start(d->options.heartbeatMs);
    QCoreApplication::instance()->installEventFilter(this);
    record(QStringLiteral("app.started"), {{QStringLiteral("version"), QCoreApplication::applicationVersion()}, {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
        {QStringLiteral("os"), QSysInfo::prettyProductName()}, {QStringLiteral("architecture"), QSysInfo::currentCpuArchitecture()}});
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, [this] {
        d->timer.stop();
        record(QStringLiteral("app.quitting"));
    });
}
DeveloperLog::~DeveloperLog()
{
    d->timer.stop();
    QCoreApplication::instance()->removeEventFilter(this);
    record(QStringLiteral("app.stopped"));
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        qInstallMessageHandler(previousHandler);
        active = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(d->mutex);
        d->stopping = true;
    }
    d->condition.notify_one();
    d->worker.join();
}
void DeveloperLog::record(const QString &event, const QJsonObject &metadata, bool verboseOnly)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    if (active && (!verboseOnly || active->d->verbose)) {
        if (event == QLatin1String("app.started") || event.startsWith(QLatin1String("library."))
            || event.startsWith(QLatin1String("comments.refresh")) || event.startsWith(QLatin1String("source_tracking."))) {
            QJsonObject performanceEntry {{QStringLiteral("time"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                                          {QStringLiteral("event"), event}, {QStringLiteral("data"), metadata}};
            active->d->performanceEntries.append(performanceEntry);
            if (active->d->performanceEntries.size() > 256) active->d->performanceEntries.removeAt(128);
        }
        active->d->enqueue(active->d->entry(event, metadata));
    }
}
QJsonArray DeveloperLog::performanceEvents()
{
    std::lock_guard<std::mutex> lock(registryMutex);
    return active ? active->d->performanceEntries : QJsonArray();
}
QString DeveloperLog::directory()
{
    std::lock_guard<std::mutex> lock(registryMutex);
    return active ? active->d->options.directory : defaultOptions().directory;
}
QString DeveloperLog::currentFile() { return QDir(directory()).filePath(QStringLiteral("developer-current.jsonl")); }
bool DeveloperLog::available()
{
    std::lock_guard<std::mutex> lock(registryMutex);
    return active && active->d->available;
}
bool DeveloperLog::verbose()
{
    std::lock_guard<std::mutex> lock(registryMutex);
    return active && active->d->verbose;
}
void DeveloperLog::setVerbose(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(registryMutex);
        if (active) { active->d->verbose = enabled; }
    }
    QSettings().setValue(QStringLiteral("DeveloperLogs/verbose"), enabled);
    record(QStringLiteral("logger.verbosity"), {{QStringLiteral("verbose"), enabled}});
}
QString DeveloperLog::documentId(const QString &identity)
{
    std::lock_guard<std::mutex> lock(registryMutex);
    if (!active) { return {}; }
    const QByteArray saltedIdentity = active->d->identitySalt + identity.toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(saltedIdentity, QCryptographicHash::Sha256).toHex().left(16));
}
QString DeveloperLog::diagnostics()
{
    QFile file(currentFile());
    if (!file.open(QIODevice::ReadOnly)) { return QStringLiteral("Developer log is unavailable."); }
    if (file.size() > 128 * 1024) {
        file.seek(file.size() - 128 * 1024);
        file.readLine();
    }
    return QString::fromUtf8(file.read(128 * 1024));
}
QString DeveloperLog::latestFreezeReport()
{
    const auto files = QDir(directory()).entryInfoList({QStringLiteral("freeze-*.txt")}, QDir::Files, QDir::Time);
    return files.isEmpty() ? QString() : files.first().absoluteFilePath();
}
QString DeveloperLog::diagnosticsWithFreezeReport()
{
    QString result = diagnostics();
    const QString path = latestFreezeReport();
    QFile report(path);
    if (!path.isEmpty() && report.open(QIODevice::ReadOnly)) {
        result += QStringLiteral("\n--- Latest freeze report: ") + QFileInfo(path).fileName() + QStringLiteral(" ---\n");
        result += QString::fromUtf8(report.read(Private::reportBytes));
    }
    return result;
}
namespace
{
QString controlName(const QObject *object)
{
    if (!object) { return {}; }
    // Only these fixed, developer-owned names are safe to include literally.
    static const QStringList names {QStringLiteral("thoughtsOverviewTabs"), QStringLiteral("commentsOverviewTable"),
        QStringLiteral("commentsOverviewDocuments"), QStringLiteral("commentsOverviewDetails"), QStringLiteral("commentsOverviewContentSplitter")};
    for (auto *parent = object; parent; parent = parent->parent()) {
        if (names.contains(parent->objectName())) { return parent->objectName(); }
    }
    return QString::fromLatin1(object->metaObject()->className());
}
QJsonObject inputSnapshot(QWidget *widget)
{
    if (!widget) { return {{QStringLiteral("target_destroyed"), true}}; }
    QJsonObject state {{QStringLiteral("control"), controlName(widget)}, {QStringLiteral("visible"), widget->isVisible()},
        {QStringLiteral("enabled"), widget->isEnabled()}, {QStringLiteral("focus"), controlName(QApplication::focusWidget())},
        {QStringLiteral("active_window"), controlName(QApplication::activeWindow())}};
    for (QWidget *parent = widget; parent; parent = parent->parentWidget()) {
        if (auto *view = qobject_cast<QAbstractItemView *>(parent)) {
            state.insert(QStringLiteral("current_row"), view->currentIndex().row());
            state.insert(QStringLiteral("rows"), view->model() ? view->model()->rowCount() : 0);
            if (view->selectionModel()) {
                const auto rows = view->selectionModel()->selectedRows();
                state.insert(QStringLiteral("selected_count"), rows.size());
                QJsonArray selected;
                for (const auto &row : rows.mid(0, 30)) { selected.append(row.row()); }
                state.insert(QStringLiteral("selected_rows"), selected);
            }
            break;
        }
    }
    return state;
}
}
bool DeveloperLog::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::ActionAdded) {
        auto *action = static_cast<QActionEvent *>(event)->action();
        if (!action->property("thoughtsDiagnosticConnected").toBool()) {
            action->setProperty("thoughtsDiagnosticConnected", true);
            connect(action, &QAction::triggered, this, [action](bool checked) {
                record(QStringLiteral("ui.action.triggered"), {{QStringLiteral("action_id"), documentId(action->objectName())},
                    {QStringLiteral("checked"), checked}});
            });
        }
    }
    auto *widget = qobject_cast<QWidget *>(watched);
    if (event->type() == QEvent::ApplicationStateChange) {
        record(QStringLiteral("app.state_changed"), {{QStringLiteral("state"), int(QApplication::applicationState())}});
    } else if (widget && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut
        || event->type() == QEvent::WindowActivate || event->type() == QEvent::WindowDeactivate
        || ((event->type() == QEvent::Show || event->type() == QEvent::Hide || event->type() == QEvent::Close) && widget->isWindow()))) {
        auto metadata = inputSnapshot(widget);
        metadata.insert(QStringLiteral("event_type"), int(event->type()));
        record(QStringLiteral("ui.state_changed"), metadata);
    }
    QString kind;
    switch (event->type()) {
    case QEvent::MouseButtonPress: kind = QStringLiteral("pointer.press"); break;
    case QEvent::MouseButtonRelease: kind = QStringLiteral("pointer.release"); break;
    case QEvent::MouseButtonDblClick: kind = QStringLiteral("pointer.double_click"); break;
    case QEvent::KeyPress: kind = QStringLiteral("key.press"); break;
    case QEvent::KeyRelease: kind = QStringLiteral("key.release"); break;
    case QEvent::Wheel: kind = QStringLiteral("wheel"); break;
    case QEvent::ContextMenu: kind = QStringLiteral("context_menu"); break;
    default: break;
    }
    if (widget && !kind.isEmpty()) {
        const auto actionId = qint64(++d->inputSequence);
        auto metadata = inputSnapshot(widget);
        metadata.insert(QStringLiteral("input_id"), actionId);
        metadata.insert(QStringLiteral("kind"), kind);
        metadata.insert(QStringLiteral("spontaneous"), event->spontaneous());
        if (auto *input = dynamic_cast<QInputEvent *>(event)) {
            metadata.insert(QStringLiteral("modifiers"), int(input->modifiers()));
        }
        if (auto *mouse = dynamic_cast<QMouseEvent *>(event)) {
            metadata.insert(QStringLiteral("button"), int(mouse->button()));
            metadata.insert(QStringLiteral("buttons"), int(mouse->buttons()));
            metadata.insert(QStringLiteral("x"), mouse->position().x());
            metadata.insert(QStringLiteral("y"), mouse->position().y());
        }
        if (auto *key = dynamic_cast<QKeyEvent *>(event)) {
            // Text entry is never logged, including key codes that reveal text.
            const bool navigation = key->key() >= Qt::Key_Escape && key->key() <= Qt::Key_PageDown;
            metadata.insert(QStringLiteral("key_kind"), navigation ? QStringLiteral("navigation") : QStringLiteral("redacted"));
            if (navigation) { metadata.insert(QStringLiteral("navigation_key"), key->key()); }
            metadata.insert(QStringLiteral("repeat"), key->isAutoRepeat());
        }
        record(QStringLiteral("ui.input.before"), metadata);
        const QPointer<QWidget> target(widget);
        // This is the next event-loop turn, not a claim that asynchronous work finished.
        QTimer::singleShot(0, this, [target, actionId, kind] {
            auto after = inputSnapshot(target.data());
            after.insert(QStringLiteral("input_id"), actionId);
            after.insert(QStringLiteral("kind"), kind);
            record(QStringLiteral("ui.input.after_dispatch"), after);
        });
    }
    return QObject::eventFilter(watched, event);
}
DeveloperActivity::DeveloperActivity(const QString &name, const QJsonObject &metadata) : m_name(name), m_metadata(metadata)
{
    m_elapsed.start();
    DeveloperLog::record(m_name + QStringLiteral(".started"), m_metadata);
}
DeveloperActivity::~DeveloperActivity()
{
    m_metadata.insert(QStringLiteral("duration_ms"), m_elapsed.elapsed());
    DeveloperLog::record(m_name + QStringLiteral(".finished"), m_metadata);
}
}
