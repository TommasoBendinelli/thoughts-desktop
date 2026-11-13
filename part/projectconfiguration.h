// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSet>
#include <QMap>
#include <QUuid>
#include "sourceowner.h"
#include "verifiedartifactcache.h"
#include <atomic>

namespace Thoughts {
// Schema 5 is authoritative. Per-folder manifests below are ephemeral compiler views.
class ProjectConfiguration {
public:
    // The Project's manifest: in its default folder as found on disk
    // (default/ before documents/), or where a new one goes.
    static QString path(const QString &root, const QString &id) {
        return QDir(root).filePath(DefaultFolderLocation::ofProject(root, id) + QStringLiteral("/thoughts-project.json"));
    }
    // The default folder's path in a schema 5 configuration, or empty.
    static QString defaultFolderPath(const QJsonObject &value) {
        for (const auto &raw : value.value(QStringLiteral("source_folders")).toArray())
            if (raw.toObject().value(QStringLiteral("folder_id")) == value.value(QStringLiteral("default_folder_id")))
                return raw.toObject().value(QStringLiteral("path")).toString();
        return {};
    }
    // Rename spec rule 3: a manifest lives inside the default folder it declares.
    static bool storedInItsDefaultFolder(const QString &root, const QString &filename, const QJsonObject &value) {
        const auto folder = defaultFolderPath(value);
        return !folder.isEmpty() && QDir::cleanPath(QDir(root).filePath(folder + QStringLiteral("/thoughts-project.json"))) == QDir::cleanPath(filename);
    }
    // Shared by all readers in this process, including successive Syncs.
    // Inode and nanosecond ctime detect atomic replacements and same-size edits.
    static quint64 diskReadCount() { return reads().load(); }
    static QJsonObject read(const QString &filename, QString *error = nullptr) {
        struct Entry { QString identity; QJsonObject value; QString error; };
        static QMutex mutex;
        static QCache<QString, Entry> cache(512);
        const QString path = QFileInfo(filename).absoluteFilePath();
        QMutexLocker lock(&mutex);
        const QString identity = artifactFileIdentity(path);
        if (!QFileInfo(path).isSymLink() && !identity.isEmpty()) {
            if (const auto *entry = cache.object(path); entry && entry->identity == identity) {
                if (error && !entry->error.isEmpty()) *error = entry->error;
                return entry->value;
            }
        }
        QString failure;
        ++reads();
        const auto value = readUncached(path, &failure);
        if (artifactFileIdentity(path) != identity) {
            cache.remove(path);
            if (error) *error = QStringLiteral("The Project settings changed while they were being read.");
            return {};
        }
        if (!identity.isEmpty() && !QFileInfo(path).isSymLink()) cache.insert(path, new Entry{identity, value, failure});
        else cache.remove(path);
        if (error && !failure.isEmpty()) *error = failure;
        return value;
    }
    // Metadata only: permits rejecting obsolete worker results without parsing
    // manifests on the GUI thread, including project additions and removals.
    static QMap<QString, QString> snapshotIdentity(const QString &root) {
        QMap<QString, QString> result;
        const QDir projects(QDir(root).filePath(QStringLiteral(".thoughts/projects")));
        for (const auto &directory : projects.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            for (const auto segment : {SourceOwner::DefaultSegment, SourceOwner::LegacyDefaultSegment}) {
                const auto path = projects.filePath(directory + QLatin1Char('/') + segment + QStringLiteral("/thoughts-project.json"));
                result.insert(path, (QFileInfo(path).isSymLink() ? QStringLiteral("symlink:") : QString()) + artifactFileIdentity(path));
            }
        }
        return result;
    }
private:
    static std::atomic<quint64> &reads() { static std::atomic<quint64> count{0}; return count; }
    static QJsonObject readUncached(const QString &filename, QString *error = nullptr) {
        QFile file(filename);
        if (!file.exists()) return {};
        if (QFileInfo(filename).isSymLink() || !file.open(QIODevice::ReadOnly)) {
            if (error) *error = QStringLiteral("Cannot read the Project configuration.");
            return {};
        }
        QJsonParseError parse;
        const auto value = QJsonDocument::fromJson(file.readAll(), &parse).object();
        if (parse.error != QJsonParseError::NoError || value.value(QStringLiteral("schema_version")).toInt() != 5
            || value.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-project")
            || !value.value(QStringLiteral("documents")).isArray() || !value.value(QStringLiteral("source_folders")).isArray()) {
            if (error) *error = QStringLiteral("Invalid Project configuration.");
            return {};
        }
        QSet<QString> folders, identifiers, locations, documentLocations;
        bool valid = true;
        for (const auto &raw : value.value(QStringLiteral("source_folders")).toArray()) {
            const auto folder = raw.toObject(); const auto id = folder.value(QStringLiteral("folder_id")).toString();
            const auto location = folder.value(QStringLiteral("path")).toString();
            valid &= folder.size() == 3 && !QUuid(id).isNull() && !folders.contains(id) && safeRelative(location)
                && !locations.contains(location) && !QUuid(folder.value(QStringLiteral("document_id")).toString()).isNull();
            // Either spelling of this Project's default folder (rename spec rule 3).
            if (id == value.value(QStringLiteral("default_folder_id")).toString())
                valid &= SourceOwner::isDefaultFolder(location)
                    && location.mid(SourceOwner::ProjectsPrefix.size(), SourceOwner::HashLength) == SourceOwner::projectHash(value.value(QStringLiteral("id")).toString());
            folders.insert(id); locations.insert(location);
        }
        valid &= folders.contains(value.value(QStringLiteral("default_folder_id")).toString());
        for (const auto &raw : value.value(QStringLiteral("documents")).toArray()) {
            const auto document = raw.toObject(); const auto id = document.value(QStringLiteral("document_id")).toString();
            const auto source = document.value(QStringLiteral("source")).toObject();
            const auto format = document.value(QStringLiteral("format")).toString();
            valid &= document.size() == 4 && !QUuid(id).isNull() && !identifiers.contains(id)
                && document.value(QStringLiteral("included")).isBool() && source.size() == 2
                && folders.contains(source.value(QStringLiteral("folder_id")).toString())
                && safeRelative(source.value(QStringLiteral("path")).toString())
                && (format == QLatin1String("latex") || format == QLatin1String("markdown") || format == QLatin1String("pdf"));
            const auto sourcePath = source.value(QStringLiteral("path")).toString();
            const auto sourceKey = source.value(QStringLiteral("folder_id")).toString() + QLatin1Char('/') + sourcePath;
            valid &= !documentLocations.contains(sourceKey);
            if (format == QLatin1String("pdf"))
                valid &= source.value(QStringLiteral("folder_id")) == value.value(QStringLiteral("default_folder_id"))
                    && QFileInfo(sourcePath).fileName() == sourcePath && sourcePath.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
            identifiers.insert(id); documentLocations.insert(sourceKey);
        }
        if (!valid) { if (error) *error = QStringLiteral("Invalid Project document or source-folder identity."); return {}; }
        return value;
    }
public:
    static bool safeRelative(const QString &value) {
        return !value.isEmpty() && value != QLatin1String(".") && !QDir::isAbsolutePath(value)
            && QDir::cleanPath(value) == value && !value.split(QLatin1Char('/')).contains(QStringLiteral(".."))
            && !value.contains(QLatin1Char('\\')) && !value.contains(QLatin1Char('\n'));
    }
    static QJsonObject find(const QString &root, const QString &owner, QString *filename = nullptr, QString *error = nullptr) {
        QJsonObject result;
        for (const auto &candidate : DefaultFolderLocation::manifestPaths(root)) {
            const auto value = read(candidate, error);
            if (value.isEmpty()) return {};
            if (!storedInItsDefaultFolder(root, candidate, value)
                || !QFileInfo(candidate).canonicalFilePath().startsWith(QFileInfo(root).canonicalFilePath() + QLatin1Char('/'))) {
                if (error) *error = QStringLiteral("The Project configuration belongs to a different folder.");
                return {};
            }
            for (const auto &folder : value.value(QStringLiteral("source_folders")).toArray()) {
                if (folder.toObject().value(QStringLiteral("path")).toString() != owner) continue;
                if (!result.isEmpty()) { if (error) *error = QStringLiteral("The source folder belongs to multiple Projects."); return {}; }
                result = value;
                if (filename) *filename = candidate;
            }
        }
        return result;
    }
    static QJsonObject projection(const QJsonObject &value, const QString &owner) {
        QJsonObject folder;
        for (const auto &raw : value.value(QStringLiteral("source_folders")).toArray())
            if (raw.toObject().value(QStringLiteral("path")).toString() == owner) folder = raw.toObject();
        if (folder.isEmpty()) return {};
        QJsonArray documents; QJsonObject rendering;
        const auto defaultDirectory = QStringLiteral("../") + defaultFolderPath(value) + QLatin1Char('/');
        for (const auto &raw : value.value(QStringLiteral("documents")).toArray()) {
            auto document = raw.toObject(); const auto source = document.value(QStringLiteral("source")).toObject();
            const bool pdf = document.value(QStringLiteral("format")).toString() == QLatin1String("pdf");
            if (!pdf && source.value(QStringLiteral("folder_id")) != folder.value(QStringLiteral("folder_id"))) continue;
            document.insert(QStringLiteral("source"), QString((pdf ? defaultDirectory : QString()) + source.value(QStringLiteral("path")).toString()));
            documents.append(document);
            const auto id = document.value(QStringLiteral("document_id")).toString();
            const auto options = value.value(QStringLiteral("rendering")).toObject().value(id);
            if (options.isObject()) rendering.insert(id, options);
        }
        return {{QStringLiteral("schema_version"), 4}, {QStringLiteral("document_id"), folder.value(QStringLiteral("document_id"))},
                {QStringLiteral("documents"), documents}, {QStringLiteral("rendering"), rendering}};
    }
    // Source documents (LaTeX, Markdown) that a Project declares but that no
    // Project selects: unchecked in Manage documents. Fedora keeps their last
    // PDF, comments and review cards but neither builds nor lists them.
    static QSet<QString> unselectedSourceDocumentIds(const QList<QJsonObject> &configurations) {
        QSet<QString> selected, unselected;
        for (const auto &configuration : configurations) {
            for (const auto &raw : configuration.value(QStringLiteral("documents")).toArray()) {
                const auto document = raw.toObject();
                const auto format = document.value(QStringLiteral("format")).toString();
                const auto id = document.value(QStringLiteral("document_id")).toString();
                if (id.isEmpty() || (format != QLatin1String("latex") && format != QLatin1String("markdown"))) continue;
                (document.value(QStringLiteral("included")).toBool() ? selected : unselected).insert(id);
            }
        }
        return unselected.subtract(selected);
    }
    // Documents of every format that some Project declares unpublished and no
    // Project publishes: their review cards are kept but not offered for review
    // (protocol/project-document-publication.md §4).
    static QSet<QString> unpublishedDocumentIds(const QList<QJsonObject> &configurations) {
        QSet<QString> published, unpublished;
        for (const auto &configuration : configurations) {
            for (const auto &raw : configuration.value(QStringLiteral("documents")).toArray()) {
                const auto document = raw.toObject();
                const auto id = document.value(QStringLiteral("document_id")).toString();
                if (!id.isEmpty()) (document.value(QStringLiteral("included")).toBool() ? published : unpublished).insert(id);
            }
        }
        return unpublished.subtract(published);
    }
    // The same, read from this computer's Project manifests; empty when one cannot be read.
    static QSet<QString> unpublishedDocumentIds(const QString &root) {
        QList<QJsonObject> configurations;
        for (const auto &filename : DefaultFolderLocation::manifestPaths(root)) {
            QString error;
            const auto configuration = read(filename, &error);
            if (!error.isEmpty()) return {};
            configurations.append(configuration);
        }
        return unpublishedDocumentIds(configurations);
    }
    static QJsonObject empty(const QString &root, const QString &id, const QString &name) {
        const QUuid urlNamespace(QStringLiteral("6ba7b811-9dad-11d1-80b4-00c04fd430c8"));
        const auto folderId = QUuid::createUuidV5(urlNamespace, QString(QStringLiteral("thoughts:default-folder:") + id)).toString(QUuid::WithoutBraces);
        const auto aggregate = QUuid::createUuidV5(urlNamespace, QString(QStringLiteral("thoughts:default-document:") + id)).toString(QUuid::WithoutBraces);
        return {{QStringLiteral("schema_version"), 5}, {QStringLiteral("kind"), QStringLiteral("thoughts-project")},
            {QStringLiteral("id"), id}, {QStringLiteral("name"), name}, {QStringLiteral("default_folder_id"), folderId},
            {QStringLiteral("source_folders"), QJsonArray{QJsonObject{{QStringLiteral("folder_id"), folderId},
                {QStringLiteral("path"), QDir(root).relativeFilePath(QFileInfo(path(root, id)).absolutePath())}, {QStringLiteral("document_id"), aggregate}}}},
            {QStringLiteral("documents"), QJsonArray{}}, {QStringLiteral("rendering"), QJsonObject{}}, {QStringLiteral("document_names"), QJsonObject{}}};
    }
    static QString documentAlias(const QJsonObject &value, const QJsonObject &document) {
        const auto source = document.value(QStringLiteral("source")).toObject();
        if (document.value(QStringLiteral("format")).toString() == QLatin1String("pdf"))
            return QStringLiteral("third_party/") + source.value(QStringLiteral("path")).toString();
        for (const auto &folder : value.value(QStringLiteral("source_folders")).toArray())
            if (folder.toObject().value(QStringLiteral("folder_id")) == source.value(QStringLiteral("folder_id")))
                return folder.toObject().value(QStringLiteral("path")).toString() + QLatin1Char('/') + source.value(QStringLiteral("path")).toString();
        return {};
    }
    static QJsonObject organize(const QJsonObject &value, QJsonObject project) {
        QSet<QString> owners;
        for (const auto &raw : value.value(QStringLiteral("source_folders")).toArray()) {
            const auto folder = raw.toObject();
            if (folder.value(QStringLiteral("folder_id")) != value.value(QStringLiteral("default_folder_id")))
                owners.insert(folder.value(QStringLiteral("path")).toString());
        }
        QHash<QString, QJsonObject> labels;
        for (const auto &raw : project.value(QStringLiteral("source_folders")).toArray())
            labels.insert(raw.toObject().value(QStringLiteral("project")).toString(), raw.toObject());
        QJsonArray connected;
        for (const auto &raw : value.value(QStringLiteral("source_folders")).toArray()) {
            const auto owner = raw.toObject().value(QStringLiteral("path")).toString();
            if (!owners.contains(owner)) continue;
            auto folder = labels.value(owner);
            folder.insert(QStringLiteral("project"), owner);
            if (!folder.contains(QStringLiteral("folder"))) folder.insert(QStringLiteral("folder"), owner);
            connected.append(folder);
        }
        project.remove(QStringLiteral("source_project"));
        project.insert(QStringLiteral("source_folders"), connected);
        QJsonArray ownedSources;
        for (const auto &raw : project.value(QStringLiteral("sources")).toArray()) {
            const auto source = raw.toObject();
            const auto id = source.value(QStringLiteral("id")).toString();
            for (const auto &owner : owners) if (id == owner || id.startsWith(owner + QLatin1Char('/'))) {
                ownedSources.append(source); break;
            }
        }
        project.insert(QStringLiteral("sources"), ownedSources);
        QJsonArray documents, unpublished;
        // A server that can publish and unpublish lists `unpublished_documents`; only
        // then does an unchecked declaration stay in the Project as an unpublished
        // document (protocol/project-document-publication.md).
        const bool publication = project.contains(QStringLiteral("unpublished_documents"));
        auto existing = project.value(QStringLiteral("documents")).toArray();
        for (const auto &raw : project.value(QStringLiteral("unpublished_documents")).toArray()) existing.append(raw);
        for (const auto &raw : value.value(QStringLiteral("documents")).toArray()) {
            const auto declaration = raw.toObject();
            const bool included = declaration.value(QStringLiteral("included")).toBool();
            if (!included && !publication) continue;
            const auto id = declaration.value(QStringLiteral("document_id"));
            const auto alias = documentAlias(value, declaration);
            QJsonObject document;
            for (const auto &candidate : existing) {
                const auto d = candidate.toObject();
                if (d.value(QStringLiteral("document_id")) == id || d.value(QStringLiteral("id")).toString() == alias
                    || d.value(QStringLiteral("source_file")).toString() == alias
                    || (declaration.value(QStringLiteral("format")).toString() == QLatin1String("pdf")
                        && d.value(QStringLiteral("filename")).toString() == declaration.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString())) {
                    document = d; break;
                }
            }
            if (document.isEmpty()) {
                document = {{QStringLiteral("id"), alias}, {QStringLiteral("pending_build"), included}, {QStringLiteral("start_page"), 0}};
                if (declaration.value(QStringLiteral("format")).toString() == QLatin1String("pdf")) {
                    document.insert(QStringLiteral("filename"), declaration.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")));
                    document.insert(QStringLiteral("imported"), true);
                } else {
                    document.insert(QStringLiteral("source_file"), alias);
                    document.insert(QStringLiteral("source_project"), SourceOwner::of(alias));
                }
            }
            document.insert(QStringLiteral("document_id"), id);
            const auto name = value.value(QStringLiteral("document_names")).toObject().value(id.toString());
            if (name.isString()) document.insert(QStringLiteral("name"), name);
            // A document not built yet has no catalogue entry to name it: use its
            // file name, as Manage documents does, rather than an empty row.
            else if (document.value(QStringLiteral("name")).toString().trimmed().isEmpty())
                document.insert(QStringLiteral("name"), QFileInfo(declaration.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString()).completeBaseName());
            if (publication) {
                document.insert(QStringLiteral("included"), included);
                document.insert(QStringLiteral("published"), included);
            }
            if (!included) document.remove(QStringLiteral("pending_build"));
            (included ? documents : unpublished).append(document);
        }
        project.insert(QStringLiteral("documents"), documents);
        if (publication) project.insert(QStringLiteral("unpublished_documents"), unpublished);
        project.insert(QStringLiteral("name"), value.value(QStringLiteral("name")));
        QString defaultDirectory;
        for (const auto &folder : value.value(QStringLiteral("source_folders")).toArray())
            if (folder.toObject().value(QStringLiteral("folder_id")) == value.value(QStringLiteral("default_folder_id")))
                defaultDirectory = folder.toObject().value(QStringLiteral("path")).toString();
        QJsonArray files{QJsonObject{{QStringLiteral("id"), QString(defaultDirectory + QStringLiteral("/thoughts-project.json"))},
            {QStringLiteral("name"), QStringLiteral("thoughts-project.json")}, {QStringLiteral("relative_path"), QStringLiteral("thoughts-project.json")}}};
        for (const auto &raw : value.value(QStringLiteral("documents")).toArray()) {
            const auto d = raw.toObject();
            const auto source = d.value(QStringLiteral("source")).toObject();
            // Imported PDFs and the Markdown and LaTeX documents declared in the default folder.
            if (d.value(QStringLiteral("format")).toString() != QLatin1String("pdf") && source.value(QStringLiteral("folder_id")) != value.value(QStringLiteral("default_folder_id"))) continue;
            const auto filename = source.value(QStringLiteral("path")).toString();
            files.append(QJsonObject{{QStringLiteral("id"), QString(defaultDirectory + QLatin1Char('/') + filename)},
                {QStringLiteral("name"), QFileInfo(filename).fileName()}, {QStringLiteral("relative_path"), filename}});
        }
        auto sources = project.value(QStringLiteral("sources")).toArray();
        for (int i = sources.size() - 1; i >= 0; --i)
            if (sources[i].toObject().value(QStringLiteral("id")).toString() == defaultDirectory) sources.removeAt(i);
        sources.prepend(QJsonObject{{QStringLiteral("id"), defaultDirectory}, {QStringLiteral("name"), QStringLiteral("Default source folder")},
            {QStringLiteral("kind"), QStringLiteral("local")}, {QStringLiteral("files"), files}});
        project.insert(QStringLiteral("sources"), sources);
        return project;
    }
    static QString change(const QString &root, const QJsonObject &project, const QString &action, const QJsonObject &values) {
        // A source folder's display name lives in the Library record, not in the manifest.
        if (action == QLatin1String("rename_source")) return {};
        const auto filename = path(root, project.value(QStringLiteral("id")).toString());
        if (!QFileInfo::exists(filename)) return QStringLiteral("The current Project manifest is missing. Sync before editing this Project.");
        QString error; auto value = read(filename, &error);
        if (!error.isEmpty()) return error;
        auto documents = value.value(QStringLiteral("documents")).toArray();
        auto names = value.value(QStringLiteral("document_names")).toObject();
        if (action == QLatin1String("rename")) value.insert(QStringLiteral("name"), values.value(QStringLiteral("name")));
        if (action == QLatin1String("order")) {
            const auto order = values.value(QStringLiteral("documents")).toArray(); QJsonArray ordered;
            for (const auto &identifier : order) for (const auto &raw : documents) {
                const auto d = raw.toObject();
                if (identifier == d.value(QStringLiteral("document_id")) || identifier.toString() == documentAlias(value, d)
                    || (d.value(QStringLiteral("format")).toString() == QLatin1String("pdf") && identifier == d.value(QStringLiteral("source")).toObject().value(QStringLiteral("path"))))
                    ordered.append(d);
            }
            int index = 0;
            for (int i = 0; i < documents.size(); ++i)
                if (documents[i].toObject().value(QStringLiteral("included")).toBool() && index < ordered.size()) documents[i] = ordered[index++];
        }
        if (action == QLatin1String("rename_document") || action == QLatin1String("remove_document")) {
            const auto identifier = values.value(QStringLiteral("document_id"));
            for (int i = documents.size() - 1; i >= 0; --i) {
                const auto d = documents[i].toObject();
                if (identifier != d.value(QStringLiteral("document_id")) && identifier.toString() != documentAlias(value, d)
                    && !(d.value(QStringLiteral("format")).toString() == QLatin1String("pdf") && identifier == d.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")))) continue;
                if (action == QLatin1String("remove_document")) documents.removeAt(i);
                else names.insert(d.value(QStringLiteral("document_id")).toString(), values.value(QStringLiteral("name")));
            }
        }
        if (action == QLatin1String("import")) {
            const auto id = values.value(QStringLiteral("document_id")).toString();
            documents.append(QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("format"), QStringLiteral("pdf")}, {QStringLiteral("included"), true},
                {QStringLiteral("source"), QJsonObject{{QStringLiteral("folder_id"), value.value(QStringLiteral("default_folder_id"))}, {QStringLiteral("path"), values.value(QStringLiteral("filename"))}}}});
            auto rendering = value.value(QStringLiteral("rendering")).toObject();
            rendering.insert(id, QJsonObject{{QStringLiteral("sha256"), values.value(QStringLiteral("sha256"))}});
            value.insert(QStringLiteral("rendering"), rendering); names.insert(id, values.value(QStringLiteral("name")));
        }
        if (action == QLatin1String("remove_source")) {
            auto folders = value.value(QStringLiteral("source_folders")).toArray(); QSet<QString> removed;
            for (int i = folders.size() - 1; i >= 0; --i)
                if (folders[i].toObject().value(QStringLiteral("path")) == values.value(QStringLiteral("source_project"))) {
                    removed.insert(folders[i].toObject().value(QStringLiteral("folder_id")).toString()); folders.removeAt(i);
                }
            for (int i = documents.size() - 1; i >= 0; --i)
                if (removed.contains(documents[i].toObject().value(QStringLiteral("source")).toObject().value(QStringLiteral("folder_id")).toString())) documents.removeAt(i);
            value.insert(QStringLiteral("source_folders"), folders);
        }
        QSet<QString> retained;
        for (const auto &raw : documents) retained.insert(raw.toObject().value(QStringLiteral("document_id")).toString());
        auto rendering = value.value(QStringLiteral("rendering")).toObject();
        for (const auto &id : rendering.keys()) if (!retained.contains(id)) rendering.remove(id);
        for (const auto &id : names.keys()) if (!retained.contains(id)) names.remove(id);
        value.insert(QStringLiteral("rendering"), rendering); value.insert(QStringLiteral("document_names"), names);
        value.insert(QStringLiteral("documents"), documents);
        return save(filename, value);
    }
    static QString save(const QString &filename, const QJsonObject &value) {
        // Rewriting an unchanged file in Qt's layout would look like a local edit to Sync.
        QFile existing(filename);
        if (!QFileInfo(filename).isSymLink() && existing.open(QIODevice::ReadOnly)) {
            const auto current = QJsonDocument::fromJson(existing.readAll());
            if (current.isObject() && current.object() == value) return {};
        }
        return saveBytes(filename, QJsonDocument(value).toJson());
    }
    static QString saveBytes(const QString &filename, const QByteArray &bytes) {
        QSaveFile file(filename);
        if (QFileInfo(filename).isSymLink() || !QDir().mkpath(QFileInfo(filename).absolutePath())
            || !file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
            return QStringLiteral("Cannot save the Project configuration.");
        return {};
    }
    static QString replaceProjection(const QString &filename, QJsonObject value, const QString &owner, const QJsonObject &incoming) {
        QJsonObject folder;
        for (const auto &raw : value.value(QStringLiteral("source_folders")).toArray())
            if (raw.toObject().value(QStringLiteral("path")).toString() == owner) folder = raw.toObject();
        if (folder.isEmpty() || folder.value(QStringLiteral("document_id")) != incoming.value(QStringLiteral("document_id")))
            return QStringLiteral("The source-folder identity changed.");
        QJsonArray replacements;
        for (const auto &raw : incoming.value(QStringLiteral("documents")).toArray()) {
            auto document = raw.toObject(); const bool pdf = document.value(QStringLiteral("format")).toString() == QLatin1String("pdf");
            const auto source = document.value(QStringLiteral("source")).toString();
            if (pdf) {
                const auto expected = QStringLiteral("../") + defaultFolderPath(value) + QLatin1Char('/') + QFileInfo(source).fileName();
                // Either spelling of the same default folder.
                if (SourceOwner::rewriteLegacyDefaultFolder(source) != SourceOwner::rewriteLegacyDefaultFolder(expected))
                    return QStringLiteral("The PDF belongs to a different Project.");
            }
            document.insert(QStringLiteral("source"), QJsonObject{
                {QStringLiteral("folder_id"), pdf ? value.value(QStringLiteral("default_folder_id")) : folder.value(QStringLiteral("folder_id"))},
                {QStringLiteral("path"), pdf ? QFileInfo(source).fileName() : source}});
            replacements.append(document);
        }
        QJsonArray documents; QSet<QString> removed; int index = 0;
        for (const auto &raw : value.value(QStringLiteral("documents")).toArray()) {
            const auto old = raw.toObject();
            if (old.value(QStringLiteral("source")).toObject().value(QStringLiteral("folder_id")) == folder.value(QStringLiteral("folder_id"))
                || old.value(QStringLiteral("format")).toString() == QLatin1String("pdf")) {
                removed.insert(old.value(QStringLiteral("document_id")).toString());
                if (index < replacements.size()) documents.append(replacements[index++]);
            } else documents.append(old);
        }
        while (index < replacements.size()) documents.append(replacements[index++]);
        auto rendering = value.value(QStringLiteral("rendering")).toObject();
        auto names = value.value(QStringLiteral("document_names")).toObject();
        for (const auto &id : removed) { rendering.remove(id); names.remove(id); }
        const auto options = incoming.value(QStringLiteral("rendering")).toObject();
        for (auto it = options.begin(); it != options.end(); ++it) rendering.insert(it.key(), it.value());
        const auto titles = incoming.value(QStringLiteral("document_names")).toObject();
        for (auto it = titles.begin(); it != titles.end(); ++it) names.insert(it.key(), it.value());
        value.insert(QStringLiteral("documents"), documents); value.insert(QStringLiteral("rendering"), rendering);
        value.insert(QStringLiteral("document_names"), names);
        return save(filename, value);
    }
};
}
