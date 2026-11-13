// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "pyfs.h"

// thoughts_server.source_lock.source_state_lock: the one authoring-state
// writer across the publisher, the editable-source installer and the Overleaf
// bridge. It must be the same lock those (Python) writers take: a POSIX
// record lock (fcntl F_SETLK) on <state>.lock, or a lock on byte 0 on Windows.
namespace Thoughts::Publisher
{

class SourceStateLock
{
public:
    // Waits up to timeoutSeconds, then raises
    // TimeoutError("another source-state writer is active").
    explicit SourceStateLock(const Py::Path &state, double timeoutSeconds = 30);
    ~SourceStateLock();
    SourceStateLock(const SourceStateLock &) = delete;
    SourceStateLock &operator=(const SourceStateLock &) = delete;

private:
    int m_descriptor = -1;
    void *m_handle = nullptr;
};

// Path.with_suffix(suffix)
Py::Path withSuffix(const Py::Path &path, const QString &suffix);

}
