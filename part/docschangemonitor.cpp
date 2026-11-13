// SPDX-License-Identifier: GPL-2.0-or-later
#include "docschangemonitor.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <algorithm>

#if defined(Q_OS_MACOS)
#include <CoreServices/CoreServices.h>
#elif defined(Q_OS_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <thread>
#include <vector>
#else
#include <QFileSystemWatcher>
#endif

namespace Thoughts
{

namespace
{
constexpr QLatin1StringView changeNotifierName("thoughts-desktop-sync-change-notifier");

// Found by object name so the application and the part plugin, which each
// link their own copy of this class, share one announcement channel.
class DesktopSyncChangeNotifier : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;
    Q_INVOKABLE void announce(const QString &databasePath)
    {
        Q_EMIT changed(databasePath);
    }

Q_SIGNALS:
    void changed(const QString &databasePath);
};

QObject *changeNotifier()
{
    QCoreApplication *application = QCoreApplication::instance();
    if (!application) {
        return nullptr;
    }
    if (QObject *existing = application->findChild<QObject *>(changeNotifierName, Qt::FindDirectChildrenOnly)) {
        return existing;
    }
    auto *notifier = new DesktopSyncChangeNotifier(application);
    notifier->setObjectName(changeNotifierName);
    return notifier;
}

// Short enough to feel immediate, long enough to fold an editor's atomic
// save (temporary file, rename, attribute update) into one batch.
constexpr int flushDelayMs = 60;
}

#if defined(Q_OS_MACOS)
struct DocsChangeMonitor::Platform {
    FSEventStreamRef stream = nullptr;
    dispatch_queue_t queue = nullptr;

    ~Platform()
    {
        if (stream) {
            FSEventStreamStop(stream);
            FSEventStreamInvalidate(stream);
            FSEventStreamRelease(stream);
        }
        if (queue) {
            // No callback is scheduled after invalidation; wait for one that
            // may still be running so it cannot outlive the monitor.
            dispatch_sync_f(queue, nullptr, [](void *) {});
            dispatch_release(queue);
        }
    }

    static void callback(ConstFSEventStreamRef, void *info, size_t count, void *eventPaths, const FSEventStreamEventFlags flags[], const FSEventStreamEventId[])
    {
        auto *monitor = static_cast<DocsChangeMonitor *>(info);
        const char *const *paths = static_cast<const char *const *>(eventPaths);
        QStringList changed;
        QStringList entries;
        changed.reserve(static_cast<qsizetype>(count));
        bool rescan = false;
        constexpr FSEventStreamEventFlags entryFlags = kFSEventStreamEventFlagItemCreated | kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed;
        constexpr FSEventStreamEventFlags dropped = kFSEventStreamEventFlagMustScanSubDirs | kFSEventStreamEventFlagUserDropped
            | kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagRootChanged | kFSEventStreamEventFlagMount | kFSEventStreamEventFlagUnmount;
        for (size_t index = 0; index < count; ++index) {
            if (flags[index] & kFSEventStreamEventFlagHistoryDone) {
                continue;
            }
            if (flags[index] & dropped) {
                rescan = true;
            }
            changed.append(QString::fromUtf8(paths[index]));
            if (flags[index] & entryFlags) {
                entries.append(changed.constLast());
            }
        }
        // The stream is stopped synchronously before the monitor is destroyed,
        // and a queued functor is dropped once its context object is gone.
        QMetaObject::invokeMethod(monitor, [monitor, changed, entries, rescan] { monitor->receive(changed, entries, rescan); }, Qt::QueuedConnection);
    }
};
#elif defined(Q_OS_WIN)
struct DocsChangeMonitor::Platform {
    HANDLE directory = INVALID_HANDLE_VALUE;
    HANDLE stopEvent = nullptr;
    HANDLE ioEvent = nullptr;
    std::thread reader;

    ~Platform()
    {
        // Join before the monitor goes away so no event is posted after it;
        // a queued functor is dropped once its context object is gone.
        if (stopEvent) {
            SetEvent(stopEvent);
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (directory != INVALID_HANDLE_VALUE) {
            CloseHandle(directory);
        }
        if (ioEvent) {
            CloseHandle(ioEvent);
        }
        if (stopEvent) {
            CloseHandle(stopEvent);
        }
    }

    DWORD openError = 0;

    bool open(const QString &root)
    {
        directory = CreateFileW(QDir::toNativeSeparators(root).toStdWString().c_str(), FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (directory == INVALID_HANDLE_VALUE) {
            openError = GetLastError();
            return false;
        }
        stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ioEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        openError = GetLastError();
        return stopEvent && ioEvent;
    }

    // Reads the whole tree's changes, like an FSEvents stream: every changed
    // path, with creations, removals and renames also reported as entries.
    void run(DocsChangeMonitor *monitor, const QString &root)
    {
        constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE
            | FILE_NOTIFY_CHANGE_CREATION;
        // 64 KiB is the most a network share accepts; DWORD elements keep the
        // buffer aligned as ReadDirectoryChangesW requires.
        std::vector<DWORD> buffer(16 * 1024);
        const DWORD bufferBytes = DWORD(buffer.size() * sizeof(DWORD));
        for (;;) {
            OVERLAPPED overlapped {};
            overlapped.hEvent = ioEvent;
            ResetEvent(ioEvent);
            if (!ReadDirectoryChangesW(directory, buffer.data(), bufferBytes, TRUE, filter, nullptr, &overlapped, nullptr)) {
                post(monitor, {}, {}, true);
                return;
            }
            const HANDLE handles[] = {stopEvent, ioEvent};
            const DWORD woken = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
            DWORD bytes = 0;
            if (woken != WAIT_OBJECT_0 + 1) {
                CancelIoEx(directory, &overlapped);
                GetOverlappedResult(directory, &overlapped, &bytes, TRUE);
                return;
            }
            if (!GetOverlappedResult(directory, &overlapped, &bytes, FALSE)) {
                post(monitor, {}, {}, true);
                if (GetLastError() == ERROR_ACCESS_DENIED) {
                    // The watched folder itself was removed.
                    return;
                }
                continue;
            }
            if (bytes == 0) {
                // The kernel buffer overflowed and individual changes were lost.
                post(monitor, {}, {}, true);
                continue;
            }
            QStringList changed;
            QStringList entries;
            const auto *cursor = reinterpret_cast<const char *>(buffer.data());
            for (;;) {
                const auto *information = reinterpret_cast<const FILE_NOTIFY_INFORMATION *>(cursor);
                const QString relative = QString::fromWCharArray(information->FileName, int(information->FileNameLength / sizeof(WCHAR)));
                const QString path = QDir::cleanPath(root + QLatin1Char('/') + QDir::fromNativeSeparators(relative));
                changed.append(path);
                if (information->Action != FILE_ACTION_MODIFIED) {
                    entries.append(path);
                }
                if (information->NextEntryOffset == 0) {
                    break;
                }
                cursor += information->NextEntryOffset;
            }
            post(monitor, changed, entries, false);
        }
    }

    static void post(DocsChangeMonitor *monitor, const QStringList &changed, const QStringList &entries, bool rescan)
    {
        QMetaObject::invokeMethod(monitor, [monitor, changed, entries, rescan] { monitor->receive(changed, entries, rescan); }, Qt::QueuedConnection);
    }
};
#else
struct DocsChangeMonitor::Platform {
    // Without a recursive native stream, report changes to the root itself
    // and let consumers re-check what they own.
    QFileSystemWatcher *watcher = nullptr;
};
#endif

DocsChangeMonitor *DocsChangeMonitor::forRoot(const QString &rootPath)
{
    static QMutex mutex;
    static QHash<QString, QPointer<DocsChangeMonitor>> monitors;
    const QString root = QDir::cleanPath(QDir(rootPath).absolutePath());
    QMutexLocker locker(&mutex);
    if (DocsChangeMonitor *existing = monitors.value(root)) {
        return existing;
    }
    // Consumers are UI-thread objects; the monitor delivers on that thread.
    Q_ASSERT(!QCoreApplication::instance() || QThread::currentThread() == QCoreApplication::instance()->thread());
    auto *monitor = new DocsChangeMonitor(root, QCoreApplication::instance());
    monitors.insert(root, monitor);
    QMetaObject::invokeMethod(monitor, &DocsChangeMonitor::start, Qt::QueuedConnection);
    return monitor;
}

DocsChangeMonitor::DocsChangeMonitor(QString rootPath, QObject *parent)
    : QObject(parent)
    , m_rootPath(std::move(rootPath))
    , m_flushTimer(new QTimer(this))
{
    m_flushTimer->setSingleShot(true);
    m_flushTimer->setInterval(flushDelayMs);
    connect(m_flushTimer, &QTimer::timeout, this, &DocsChangeMonitor::flush);
}

DocsChangeMonitor::~DocsChangeMonitor() = default;

QString DocsChangeMonitor::rootPath() const
{
    return m_rootPath;
}

bool DocsChangeMonitor::active() const
{
    return m_platform != nullptr;
}

bool DocsChangeMonitor::isWithin(const QString &path, const QString &directory)
{
    if (directory.isEmpty() || !path.startsWith(directory)) {
        return false;
    }
    return path.size() == directory.size() || path.at(directory.size()) == QLatin1Char('/') || directory.endsWith(QLatin1Char('/'));
}

void DocsChangeMonitor::start()
{
    if (m_platform) {
        return;
    }
    const QString canonical = QFileInfo(m_rootPath).canonicalFilePath();
    m_canonicalRootPath = canonical.isEmpty() ? m_rootPath : canonical;
#if defined(Q_OS_MACOS)
    auto platform = std::make_unique<Platform>();
    const QByteArray encodedRoot = m_rootPath.toUtf8();
    CFStringRef rootString = CFStringCreateWithCString(kCFAllocatorDefault, encodedRoot.constData(), kCFStringEncodingUTF8);
    CFArrayRef roots = CFArrayCreate(kCFAllocatorDefault, reinterpret_cast<const void **>(&rootString), 1, &kCFTypeArrayCallBacks);
    FSEventStreamContext context {0, this, nullptr, nullptr, nullptr};
    platform->stream = FSEventStreamCreate(kCFAllocatorDefault,
                                           &Platform::callback,
                                           &context,
                                           roots,
                                           kFSEventStreamEventIdSinceNow,
                                           0.02,
                                           kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer | kFSEventStreamCreateFlagWatchRoot);
    CFRelease(roots);
    CFRelease(rootString);
    if (!platform->stream) {
        qWarning() << "Could not create a filesystem event stream for" << m_rootPath;
        return;
    }
    platform->queue = dispatch_queue_create("org.tbend.thoughts.docs-change-monitor", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(platform->stream, platform->queue);
    if (!FSEventStreamStart(platform->stream)) {
        qWarning() << "Could not start the filesystem event stream for" << m_rootPath;
        return;
    }
    m_platform = std::move(platform);
#elif defined(Q_OS_WIN)
    auto platform = std::make_unique<Platform>();
    if (!platform->open(m_rootPath)) {
        // FSEvents can watch a folder that does not exist yet, such as the
        // data folder before the first Sync; here, try again until it does.
        if (platform->openError == ERROR_FILE_NOT_FOUND || platform->openError == ERROR_PATH_NOT_FOUND) {
            m_waitingForRoot = true;
            QTimer::singleShot(1000, this, &DocsChangeMonitor::start);
        } else {
            qWarning() << "Could not watch" << m_rootPath << "for changes; Windows error" << platform->openError;
        }
        return;
    }
    platform->reader = std::thread([raw = platform.get(), this, root = m_rootPath] { raw->run(this, root); });
    m_platform = std::move(platform);
    if (std::exchange(m_waitingForRoot, false)) {
        // Whatever appeared before the watch began was not reported.
        receive({}, {}, true);
    }
#else
    m_platform = std::make_unique<Platform>();
    m_platform->watcher = new QFileSystemWatcher(this);
    m_platform->watcher->addPath(m_rootPath);
    connect(m_platform->watcher, &QFileSystemWatcher::directoryChanged, this, [this](const QString &path) { receive({path}, {path}, true); });
#endif
}

void DocsChangeMonitor::injectChanges(const QStringList &absolutePaths, bool rescan, bool entriesChanged)
{
    receive(absolutePaths, entriesChanged ? absolutePaths : QStringList(), rescan);
}

void DocsChangeMonitor::receive(const QStringList &paths, const QStringList &entryPaths, bool rescan)
{
    const auto local = [this](const QString &reported) {
        QString path = QDir::cleanPath(reported);
        // FSEvents reports resolved paths; consumers compare against the
        // configured root.
        if (!m_canonicalRootPath.isEmpty() && m_canonicalRootPath != m_rootPath && isWithin(path, m_canonicalRootPath)) {
            path = m_rootPath + path.mid(m_canonicalRootPath.size());
        }
        return isWithin(path, m_rootPath) ? path : QString();
    };
    for (const QString &reported : paths) {
        const QString path = local(reported);
        if (!path.isEmpty()) {
            m_pendingPaths.insert(path);
        }
    }
    for (const QString &reported : entryPaths) {
        const QString path = local(reported);
        if (!path.isEmpty()) {
            m_pendingEntryPaths.insert(path);
        }
    }
    m_pendingRescan = m_pendingRescan || rescan;
    if ((!m_pendingPaths.isEmpty() || m_pendingRescan) && !m_flushTimer->isActive()) {
        m_flushTimer->start();
    }
}

void DocsChangeMonitor::flush()
{
    QStringList paths(m_pendingPaths.cbegin(), m_pendingPaths.cend());
    QStringList entries(m_pendingEntryPaths.cbegin(), m_pendingEntryPaths.cend());
    m_pendingPaths.clear();
    m_pendingEntryPaths.clear();
    const bool rescan = std::exchange(m_pendingRescan, false);
    std::sort(paths.begin(), paths.end());
    std::sort(entries.begin(), entries.end());
    if (!paths.isEmpty()) {
        Q_EMIT pathsChanged(paths, entries);
    }
    if (rescan) {
        Q_EMIT rescanRequired();
    }
}

void notifyDesktopSyncStoreChanged(const QString &databasePath)
{
    QCoreApplication *application = QCoreApplication::instance();
    if (!application) {
        return;
    }
    QMetaObject::invokeMethod(application, [databasePath] {
        if (QObject *notifier = changeNotifier()) {
            QMetaObject::invokeMethod(notifier, "announce", Qt::DirectConnection, Q_ARG(QString, databasePath));
        }
    }, Qt::QueuedConnection);
}

QMetaObject::Connection connectDesktopSyncStoreChanges(QObject *receiver, const char *slot)
{
    QObject *notifier = changeNotifier();
    return notifier ? QObject::connect(notifier, SIGNAL(changed(QString)), receiver, slot) : QMetaObject::Connection();
}

}

#include "docschangemonitor.moc"
