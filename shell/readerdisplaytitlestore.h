// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "readerdisplaytitle.h"
#include "../part/libraryprojectstore.h"
#include "../part/docschangemonitor.h"
#include <QFutureWatcher>
#include <QTimer>
#include <QSet>
#include <QtConcurrentRun>
#include <algorithm>
#include <functional>

namespace Thoughts {
/** Observe durable title metadata, including atomic manifest replacements, without reloading PDFs. */
class ReaderDisplayTitleStore : public QObject {
    QString m_root;
    QTimer m_timer;
    QFutureWatcher<QJsonObject> m_load;
    std::function<void()> m_changed;
    bool m_dirty = false;
    bool m_loaded = false;
    QJsonArray m_projects, m_publications;

    void load() {
        if (m_load.isRunning()) { m_dirty = true; return; }
        m_dirty = false;
        m_load.setFuture(QtConcurrent::run([root = m_root] {
            LibraryProjectStore store(root);
            const auto projects = store.projects(false);
            QFile file(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
            const auto publications = file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("pdfs")).toArray() : QJsonArray{};
            return QJsonObject{{QStringLiteral("projects"), projects}, {QStringLiteral("publications"), publications},
                               {QStringLiteral("error"), store.error()}};
        }));
    }
    // Title metadata: the Library project list, per-project manifests, and the
    // published Library index (current_pdfs is swapped to a new stage).
    bool affectsTitles(const QString &path) const {
        const QString root = QDir::cleanPath(m_root);
        if (path == QDir::cleanPath(LibraryProjectStore(m_root).path()) || path == root + QStringLiteral("/current_pdfs")) return true;
        const QString projects = root + QStringLiteral("/.thoughts/projects");
        if (DocsChangeMonitor::isWithin(path, projects)) return true;
        const QFileInfo info(path);
        return info.fileName() == QLatin1String(".thoughts-library-index.json") && QFileInfo(info.absolutePath()).absolutePath() == root;
    }
public:
    ReaderDisplayTitleStore(const QString &root, QObject *parent, std::function<void()> changed)
        : QObject(parent), m_root(root), m_changed(std::move(changed)) {
        m_timer.setSingleShot(true);
        m_timer.setInterval(100);
        connect(&m_timer, &QTimer::timeout, this, [this] { load(); });
        DocsChangeMonitor *monitor = DocsChangeMonitor::forRoot(m_root);
        connect(monitor, &DocsChangeMonitor::pathsChanged, this, [this](const QStringList &paths) {
            if (std::any_of(paths.cbegin(), paths.cend(), [this](const QString &path) { return affectsTitles(path); })) refresh();
        });
        connect(monitor, &DocsChangeMonitor::rescanRequired, this, [this] { refresh(); });
        connect(&m_load, &QFutureWatcher<QJsonObject>::finished, this, [this] {
            const auto result = m_load.result();
            auto projects = m_projects;
            if (result.value(QStringLiteral("error")).toString().isEmpty()) projects = result.value(QStringLiteral("projects")).toArray();
            const auto publications = result.value(QStringLiteral("publications")).toArray();
            const bool changed = !m_loaded || projects != m_projects || publications != m_publications;
            m_loaded = true;
            m_projects = projects;
            m_publications = publications;
            if (changed) m_changed();
            if (m_dirty) refresh();
        });
        refresh();
    }
    void refresh() { m_timer.start(); }
    QString title(const QUrl &url, const QString &documentId, const QString &projectId, const QString &fallback) const {
        return readerDisplayTitle(m_projects, m_publications, m_root, url, documentId, projectId, fallback);
    }
};
}
