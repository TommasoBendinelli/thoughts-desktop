// SPDX-License-Identifier: GPL-2.0-or-later
#include "statelock.h"

#include "pyerror.h"

#include <QElapsedTimer>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace Thoughts::Publisher
{

Py::Path withSuffix(const Py::Path &path, const QString &suffix)
{
    return path.parent() / (path.stem() + suffix);
}

SourceStateLock::SourceStateLock(const Py::Path &state, double timeoutSeconds)
{
    const Py::Path lockPath = withSuffix(state, QStringLiteral(".lock"));
    Py::makeDirectories(lockPath.parent());
    QElapsedTimer clock;
    clock.start();
#ifdef Q_OS_WIN
    // open("a"): create if missing, share like the C runtime does.
    HANDLE handle = CreateFileW(lockPath.str().toStdWString().c_str(), FILE_APPEND_DATA | GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        Py::raiseOsError(EACCES, lockPath.str());
    }
    m_handle = handle;
    for (;;) {
        // msvcrt.locking(fd, LK_NBLCK, 1) at offset 0.
        if (LockFile(handle, 0, 0, 1, 0)) {
            break;
        }
        if (clock.elapsed() >= qint64(timeoutSeconds * 1000)) {
            CloseHandle(handle);
            m_handle = nullptr;
            Py::raise(QStringLiteral("TimeoutError"), QStringLiteral("another source-state writer is active"));
        }
        QThread::msleep(50);
    }
#else
    m_descriptor = ::open(Py::encodeFileName(lockPath.str()).constData(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0666);
    if (m_descriptor < 0) {
        Py::raiseOsError(errno, lockPath.str());
    }
    for (;;) {
        // fcntl.lockf(fd, LOCK_EX | LOCK_NB): a whole-file POSIX record lock.
        struct flock lock {};
        lock.l_type = F_WRLCK;
        lock.l_whence = SEEK_SET;
        if (::fcntl(m_descriptor, F_SETLK, &lock) == 0) {
            break;
        }
        if (errno != EACCES && errno != EAGAIN) {
            const int error = errno;
            ::close(m_descriptor);
            m_descriptor = -1;
            Py::raiseOsError(error, {});
        }
        if (clock.elapsed() >= qint64(timeoutSeconds * 1000)) {
            ::close(m_descriptor);
            m_descriptor = -1;
            Py::raise(QStringLiteral("TimeoutError"), QStringLiteral("another source-state writer is active"));
        }
        QThread::msleep(50);
    }
#endif
}

SourceStateLock::~SourceStateLock()
{
#ifdef Q_OS_WIN
    if (m_handle) {
        UnlockFile(static_cast<HANDLE>(m_handle), 0, 0, 1, 0);
        CloseHandle(static_cast<HANDLE>(m_handle));
    }
#else
    if (m_descriptor >= 0) {
        struct flock lock {};
        lock.l_type = F_UNLCK;
        lock.l_whence = SEEK_SET;
        ::fcntl(m_descriptor, F_SETLK, &lock);
        ::close(m_descriptor);
    }
#endif
}

}
