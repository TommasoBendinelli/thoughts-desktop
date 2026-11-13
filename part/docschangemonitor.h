// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <memory>

class QTimer;

namespace Thoughts
{

// One recursive filesystem event stream per root directory, shared by every
// component in this binary image that needs to notice changes beneath it.
// Events are hints: consumers filter the batched paths they care about and
// re-read those paths from disk. When the operating system reports that
// events were dropped, rescanRequired() asks consumers to re-check everything.
//
// Note: this lives in a static library linked into both the application and
// the part plugin, so each image owns its own instance per root.
class DocsChangeMonitor : public QObject
{
    Q_OBJECT

public:
    static DocsChangeMonitor *forRoot(const QString &rootPath);
    ~DocsChangeMonitor() override;

    QString rootPath() const;
    bool active() const;
    // Feeds paths as if the operating system had reported them; by default
    // as entries that were created, removed or renamed.
    void injectChanges(const QStringList &absolutePaths, bool rescan = false, bool entriesChanged = true);

    // Whether path equals directory or lies beneath it. Both must be clean
    // absolute paths.
    static bool isWithin(const QString &path, const QString &directory);

Q_SIGNALS:
    // Absolute, clean paths below rootPath(), deduplicated and sorted.
    // entryPaths is the subset whose directory entry was created, removed or
    // renamed (as opposed to contents or metadata modified in place).
    void pathsChanged(const QStringList &absolutePaths, const QStringList &entryPaths);
    void rescanRequired();

private:
    explicit DocsChangeMonitor(QString rootPath, QObject *parent = nullptr);
    void start();
    void receive(const QStringList &paths, const QStringList &entryPaths, bool rescan);
    void flush();

    struct Platform;
    QString m_rootPath;
    QString m_canonicalRootPath;
    QSet<QString> m_pendingPaths;
    QSet<QString> m_pendingEntryPaths;
    bool m_pendingRescan = false;
    QTimer *m_flushTimer = nullptr;
    std::unique_ptr<Platform> m_platform;
    // Windows cannot watch a folder before it exists; start() retries.
    bool m_waitingForRoot = false;
};

// Announces that this process committed a change to a desktop
// synchronization database. Safe from any thread; delivered on the
// application thread to every connected receiver in either binary image.
void notifyDesktopSyncStoreChanged(const QString &databasePath);
// Connects a receiver's slot, e.g. SLOT(handleChange(QString)), to those
// announcements. Call on the application thread.
QMetaObject::Connection connectDesktopSyncStoreChanges(QObject *receiver, const char *slot);

}
