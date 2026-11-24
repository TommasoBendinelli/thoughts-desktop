// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "thoughtignore.h"
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <limits>

namespace Thoughts {

// Local, disposable metadata cache. Never reads source/PDF contents; only the
// .ThoughtIgnore policy is read. Directory membership is reused until its
// filesystem identity changes, while known files are stat'ed for size changes.
class LibrarySourceInventory {
    QString m_root;
    QJsonObject m_entries;

    static QString identity(const QFileInfo &file) {
        return QStringLiteral("%1:%2:%3:%4")
            .arg(file.lastModified().toMSecsSinceEpoch()).arg(file.metadataChangeTime().toMSecsSinceEpoch())
            .arg(file.birthTime().toMSecsSinceEpoch()).arg(file.size());
    }
    QString cachePath() const { return QDir(m_root).filePath(QStringLiteral(".thoughts/library-source-inventory.json")); }

public:
    int directoriesEnumerated = 0;
    int filesChecked = 0;

    explicit LibrarySourceInventory(const QString &root) : m_root(QDir(root).absolutePath()) {
        // Read again only when it changed (fileidentitycache.h).
        const auto saved = cachedJsonFile(cachePath()).object();
        if (saved.value(QStringLiteral("version")).toInt() == 1 && saved.value(QStringLiteral("root")).toString() == m_root)
            m_entries = saved.value(QStringLiteral("folders")).toObject();
    }

    QHash<QString, QJsonObject> cached() const {
        QHash<QString, QJsonObject> result;
        for (auto it = m_entries.begin(); it != m_entries.end(); ++it) {
            auto location = it.value().toObject().value(QStringLiteral("location")).toObject();
            if (location.value(QStringLiteral("id")).toString() != it.key()) continue;
            if (location.value(QStringLiteral("size_state")).toString() != QLatin1String("unavailable"))
                location.insert(QStringLiteral("size_state"), QStringLiteral("cached"));
            result.insert(it.key(), location);
        }
        return result;
    }

    QJsonObject refresh(const QString &owner) {
        const auto previous = m_entries.value(owner).toObject();
        auto location = previous.value(QStringLiteral("location")).toObject();
        location.insert(QStringLiteral("id"), owner);
        location.insert(QStringLiteral("name"), owner);
        const auto fail = [&](const QString &error) {
            location.insert(QStringLiteral("size_state"), QStringLiteral("unavailable"));
            location.insert(QStringLiteral("size_error"), error);
            // Retry directory membership after any incomplete observation;
            // never turn a partial scan into a smaller successful total.
            m_entries.insert(owner, QJsonObject{{QStringLiteral("location"), location}});
            return location;
        };
        if (owner.isEmpty() || QDir::isAbsolutePath(owner) || QDir::cleanPath(owner) != owner || owner.split(QLatin1Char('/')).contains(QStringLiteral("..")))
            return fail(QStringLiteral("Invalid source folder."));
        const QDir root(QDir(m_root).filePath(owner));
        const QFileInfo info(root.absolutePath());
        if (!info.isDir() || info.isSymLink() || !info.isReadable()) return fail(QStringLiteral("Source folder is missing or unreadable."));
        const ThoughtIgnore ignore(root.absolutePath());
        if (!ignore.error().isEmpty()) return fail(ignore.error());
        const QString ignoreIdentity = identity(QFileInfo(root.filePath(QStringLiteral(".ThoughtIgnore"))));
        const bool samePolicy = previous.value(QStringLiteral("ignore_identity")).toString() == ignoreIdentity;
        const auto oldDirectories = samePolicy ? previous.value(QStringLiteral("directories")).toObject() : QJsonObject();
        QJsonObject directories;
        QJsonArray files;
        QString error;
        qint64 bytes = 0;
        const auto visit = [&](const auto &self, const QString &relative) -> void {
            if (!error.isEmpty()) return;
            const QDir directory(root.filePath(relative));
            const QFileInfo before(directory.absolutePath());
            if (!before.isDir() || before.isSymLink() || !before.isReadable()) {
                error = QStringLiteral("A source directory is missing or unreadable."); return;
            }
            const QString stamp = identity(before);
            const auto old = oldDirectories.value(relative).toObject();
            QJsonArray names;
            if (old.value(QStringLiteral("identity")).toString() == stamp && old.value(QStringLiteral("names")).isArray()) {
                names = old.value(QStringLiteral("names")).toArray();
            } else {
                ++directoriesEnumerated;
                for (const auto &name : directory.entryList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name)) names.append(name);
            }
            for (const auto &value : names) {
                const QString name = value.toString();
                if (name.isEmpty() || name.contains(QLatin1Char('/')) || name == QLatin1String(".") || name == QLatin1String("..")) continue;
                const QString path = relative.isEmpty() ? name : relative + QLatin1Char('/') + name;
                const QFileInfo file(root.filePath(path));
                if (file.isSymLink()) continue;
                if (!file.exists()) { error = QStringLiteral("Source files changed during the check."); return; }
                if (ignore.excludes(path, file.isDir())) continue;
                if (file.isDir()) { self(self, path); continue; }
                if (!file.isFile()) continue;
                ++filesChecked;
                const qint64 size = file.size();
                if (size < 0 || bytes > std::numeric_limits<qint64>::max() - size) { error = QStringLiteral("Source size is unavailable."); return; }
                bytes += size;
                files.append(QJsonObject{{QStringLiteral("id"), QString(owner + QLatin1Char('/') + path)},
                    {QStringLiteral("name"), name}, {QStringLiteral("relative_path"), path}, {QStringLiteral("size_bytes"), size}});
            }
            if (stamp != identity(QFileInfo(directory.absolutePath()))) { error = QStringLiteral("Source files changed during the check."); return; }
            directories.insert(relative, QJsonObject{{QStringLiteral("identity"), stamp}, {QStringLiteral("names"), names}});
        };
        visit(visit, QString());
        if (ignoreIdentity != identity(QFileInfo(root.filePath(QStringLiteral(".ThoughtIgnore"))))) error = QStringLiteral("Ignore rules changed during the check.");
        if (!error.isEmpty()) return fail(error);
        location.insert(QStringLiteral("files"), files);
        location.insert(QStringLiteral("size_bytes"), bytes);
        location.insert(QStringLiteral("size_state"), QStringLiteral("current"));
        location.remove(QStringLiteral("size_error"));
        m_entries.insert(owner, QJsonObject{{QStringLiteral("location"), location},
            {QStringLiteral("ignore_identity"), ignoreIdentity}, {QStringLiteral("directories"), directories}});
        return location;
    }

    void save(const QSet<QString> &owners) {
        for (auto it = m_entries.begin(); it != m_entries.end();) {
            if (!owners.contains(it.key())) it = m_entries.erase(it); else ++it;
        }
        const auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("version"), 1}, {QStringLiteral("root"), m_root},
            {QStringLiteral("folders"), m_entries}}).toJson(QJsonDocument::Compact);
        if (const auto previous = cachedFileBytes(cachePath()); previous && *previous == bytes) return;
        QDir().mkpath(QFileInfo(cachePath()).absolutePath());
        QSaveFile file(cachePath());
        if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
    }
};
}
