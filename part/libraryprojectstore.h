#include "documentidentity.h"
#include "fileidentitycache.h"
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "markdownfolderimport.h"
#include "thoughtignore.h"
#include "documentbuildselection.h"
#include "sourcefoldernames.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QUrl>
#include <QUuid>

namespace Thoughts {
class LibraryProjectStore {
    QString m_path;
    mutable QString m_error;
    QString root() const { return QFileInfo(QFileInfo(m_path).absolutePath()).absolutePath(); }
    // The Project files this computer wrote itself and nobody has edited
    // since: the bytes of its own Library changes and of Fedora revisions it
    // installed. By path relative to the docs root, the SHA-256 of those bytes.
    static constexpr QLatin1StringView generatedKey{"generated_manifests"};
    static QString sha256Hex(const QByteArray &bytes) {
        return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
    }
    // The manifest "Add project" writes for this name, unchanged since
    // (Thoughts before 2026-10-12 kept no record of the files it wrote).
    bool createdHere(const QString &id, const QString &name, const QByteArray &bytes) const {
        const auto document = QJsonDocument::fromJson(bytes);
        return document.isObject() && QJsonDocument(document.object()).toJson() == bytes
            && document.object() == ProjectConfiguration::empty(root(), id, name);
    }
    void recordGenerated(QJsonObject &state, const QString &id) const {
        QFile file(ProjectConfiguration::path(root(), id));
        if (QFileInfo(file.fileName()).isSymLink() || !file.open(QIODevice::ReadOnly)) return;
        auto generated = state.value(generatedKey).toObject();
        generated.insert(QDir(root()).relativeFilePath(file.fileName()), sha256Hex(file.readAll()));
        state.insert(generatedKey, generated);
    }
    // busy: Projects with queued Library changes, which Fedora has not applied yet.
    QString installSourceManifests(const QJsonArray &projects, const QSet<QString> &busy, QJsonObject &generated) const {
        for (const auto &value : projects) {
            const auto project = value.toObject(); QSet<QString> allowed;
            const auto configuration = project.value(QStringLiteral("project_configuration")).toObject();
            for (const auto &entry : configuration.value(QStringLiteral("source_folders")).toArray())
                allowed.insert(entry.toObject().value(QStringLiteral("path")).toString());
            for (const auto &folder : project.value(QStringLiteral("source_folders")).toArray()) allowed.insert(folder.toObject().value(QStringLiteral("project")).toString());
            for (const auto &entry : project.value(QStringLiteral("source_manifests")).toArray()) {
                const auto item = entry.toObject(); const auto relative = item.value(QStringLiteral("path")).toString();
                const auto owner = relative.section(QLatin1Char('/'), 0, 0);
                // Fedora names the Project's default folder in either spelling.
                // Sync renames a legacy folder before it gets here (rename spec,
                // client local migration); a mismatch left now needs attention.
                const auto hash = SourceOwner::projectHash(project.value(QStringLiteral("id")).toString());
                if (SourceOwner::isManifestPath(relative) && relative.mid(SourceOwner::ProjectsPrefix.size(), SourceOwner::HashLength) == hash) {
                    const auto local = DefaultFolderLocation::existing(root(), hash);
                    if (!local.isEmpty() && local + QStringLiteral("/thoughts-project.json") != relative)
                        return QStringLiteral("The Project's default folder on this computer (%1) differs from the server's (%2).").arg(local, relative);
                }
                const auto central = QDir(root()).filePath(SourceOwner::isManifestPath(relative) && relative.mid(SourceOwner::ProjectsPrefix.size(), SourceOwner::HashLength) == hash
                    ? relative : ProjectConfiguration::path(root(), project.value(QStringLiteral("id")).toString()));
                if (QDir(root()).filePath(relative) == central) {
                    const auto bytes = QByteArray::fromBase64(item.value(QStringLiteral("content")).toString().toLatin1());
                    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != item.value(QStringLiteral("sha256")).toString().toLatin1())
                        return QStringLiteral("The Project configuration failed verification.");
                    if (!QJsonDocument::fromJson(bytes).isObject()) return QStringLiteral("The Project configuration failed verification.");
                    const auto id = project.value(QStringLiteral("id")).toString();
                    if (QFileInfo(central).isSymLink()) continue;
                    if (QFileInfo::exists(central)) {
                        QFile existing(central);
                        if (!existing.open(QIODevice::ReadOnly)) return QStringLiteral("The Project configuration could not be read.");
                        const auto local = existing.readAll();
                        existing.close();
                        if (local == bytes) { generated.insert(relative, sha256Hex(bytes)); continue; }
                        // Replace only this computer's own copy of an earlier
                        // revision, once Fedora has applied every queued change
                        // to the Project. Anything else is an edit: Sync merges
                        // it with its base or reports the conflict.
                        if (busy.contains(id) || (generated.value(relative).toString() != sha256Hex(local) && !createdHere(id, project.value(QStringLiteral("name")).toString(), local))) continue;
                    }
                    // Keep Fedora's exact bytes so Sync sees the file as unchanged.
                    const auto error = ProjectConfiguration::saveBytes(central, bytes);
                    if (!error.isEmpty()) return error;
                    generated.insert(relative, sha256Hex(bytes));
                    continue;
                }
                if (!allowed.contains(owner) || !QRegularExpression(QStringLiteral("^docs-[A-Za-z0-9_.-]+/thoughts-project\\.json$")).match(relative).hasMatch())
                    return QStringLiteral("The project manifest path is invalid.");
                const auto decoded = QByteArray::fromBase64Encoding(item.value(QStringLiteral("content")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
                if (!decoded || QCryptographicHash::hash(decoded.decoded, QCryptographicHash::Sha256).toHex() != item.value(QStringLiteral("sha256")).toString().toLatin1()
                    || !QJsonDocument::fromJson(decoded.decoded).isObject()) return QStringLiteral("The project manifest failed verification.");
                const auto directory = QDir(root()).filePath(owner);
                if (!QFileInfo(directory).isDir()) continue;
                const auto path = QDir(root()).filePath(relative);
                if (QFileInfo(directory).isSymLink() || QFileInfo(path).isSymLink()
                    || !QFileInfo(directory).canonicalFilePath().startsWith(QFileInfo(root()).canonicalFilePath() + QLatin1Char('/')))
                    return QStringLiteral("The project manifest is outside its source folder.");
                QFile marker(QDir(directory).filePath(QStringLiteral(".thoughts-import.json")));
                QJsonObject receipt;
                if (marker.open(QIODevice::ReadOnly)) receipt = QJsonDocument::fromJson(marker.readAll()).object();
                QFile existing(path);
                if (existing.exists()) {
                    if (!existing.open(QIODevice::ReadOnly)) return QStringLiteral("The project manifest could not be read.");
                    const auto bytes = existing.readAll();
                    if (bytes == decoded.decoded) continue;
                    // Only replace our untouched provisional manifest. User edits use normal source Sync.
                    if (QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() != receipt.value(QStringLiteral("manifest_sha256")).toString().toLatin1()) continue;
                    // Windows cannot replace a file that is still open.
                    existing.close();
                }
                QSaveFile output(path);
                if (!output.open(QIODevice::WriteOnly) || output.write(decoded.decoded) != decoded.decoded.size() || !output.commit())
                    return QStringLiteral("The project manifest could not be installed.");
            }
        }
        return {};
    }
public:
    // The folder an Import sources operation creates: the docs-<slug>-<hex>
    // name it recorded when it was queued (source folder names spec, rule N),
    // or docs-import-<32 hex> for an operation queued before that.
    static QString sourceProject(const QJsonObject &op) {
        const auto id = op.value(QStringLiteral("operation_id")).toString();
        if (QUuid(id).isNull()) return {};
        const auto named = op.value(QStringLiteral("source_project")).toString();
        if (SourceFolderNames::isStrictFolderName(named)) return named;
        return legacyImportFolder(id);
    }
    static QString legacyImportFolder(const QString &operationId) {
        return SourceFolderNames::ImportPrefix + QString(operationId).remove(QLatin1Char('-')).toLower();
    }
private:
    // Every folder name this computer knows, lowercased: entries of the docs
    // root, folders of Fedora's catalogue and of queued operations.
    QSet<QString> knownFolderNames(const QJsonObject &state, const QString &exceptOperation = {}) const {
        QSet<QString> names;
        for (const auto &entry : QDir(root()).entryList(QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot)) names.insert(entry.toLower());
        for (const auto &value : state.value(QStringLiteral("projects")).toArray()) {
            const auto project = value.toObject();
            for (const auto &folder : project.value(QStringLiteral("source_folders")).toArray()) names.insert(folder.toObject().value(QStringLiteral("project")).toString().toLower());
            for (const auto &folder : project.value(QStringLiteral("project_configuration")).toObject().value(QStringLiteral("source_folders")).toArray())
                names.insert(folder.toObject().value(QStringLiteral("path")).toString().toLower());
        }
        for (const auto &value : state.value(QStringLiteral("source_folder_renames")).toArray()) {
            names.insert(value.toObject().value(QStringLiteral("old")).toString().toLower());
            names.insert(value.toObject().value(QStringLiteral("new")).toString().toLower());
        }
        for (const auto &value : state.value(QStringLiteral("pending")).toArray()) {
            const auto op = value.toObject();
            if (op.value(QStringLiteral("action")).toString() != QLatin1String("import_folder") || op.value(QStringLiteral("operation_id")).toString() == exceptOperation) continue;
            names.insert(sourceProject(op).toLower());
        }
        names.remove(QString());
        return names;
    }
    // Rule P with one pair over the queued operations that name a folder.
    static QJsonArray renameInPending(const QJsonArray &pending, const QString &from, const QString &to) {
        const SourceFolderRenames pair{{QString(), from, to}};
        return SourceFolderNames::rewriteJson(pending, pair);
    }
    // Rename this computer's copy of a queued import (it carries the
    // operation's receipt) and every queued reference to it.
    QString moveImportFolder(QJsonObject &state, const QString &operationId, const QString &from, const QString &to) const {
        const auto source = QDir(root()).filePath(from), target = QDir(root()).filePath(to);
        if (QFileInfo::exists(source) || QFileInfo(source).isSymLink()) {
            QFile marker(QDir(source).filePath(QStringLiteral(".thoughts-import.json")));
            if (QFileInfo(source).isSymLink() || !marker.open(QIODevice::ReadOnly)
                || QJsonDocument::fromJson(marker.readAll()).object().value(QStringLiteral("operation_id")).toString() != operationId)
                return QStringLiteral("The imported folder %1 is not this import's copy; it has been preserved.").arg(source);
            marker.close();
            if (QFileInfo::exists(target) || QFileInfo(target).isSymLink()) return QStringLiteral("The source folder already exists and has been preserved: ") + target;
            // Windows cannot rename a folder while a file in it is open.
            if (!QDir().rename(source, target)) return QStringLiteral("The imported folder could not be renamed to %1. Close its files and Sync again.").arg(to);
        }
        state.insert(QStringLiteral("pending"), renameInPending(state.value(QStringLiteral("pending")).toArray(), from, to));
        return {};
    }
    QString materializeSources(const QJsonObject &op) const {
        const auto project = sourceProject(op);
        if (project.isEmpty()) return QStringLiteral("The folder import has an invalid identity.");
        const auto destination = QDir(root()).filePath(project);
        const auto markerPath = QDir(destination).filePath(QStringLiteral(".thoughts-import.json"));
        if (QFileInfo::exists(destination)) {
            QFile marker(markerPath);
            if (!QFileInfo(destination).isSymLink() && marker.open(QIODevice::ReadOnly)
                && QJsonDocument::fromJson(marker.readAll()).object().value(QStringLiteral("operation_id")) == op.value(QStringLiteral("operation_id"))) return {};
            return QStringLiteral("The source folder already exists and has been preserved: ") + destination;
        }
        const auto copy = MarkdownFolderImport::savedBundle(op.value(QStringLiteral("local_path")).toString(), op.value(QStringLiteral("sha256")).toString());
        if (!copy.error.isEmpty()) return copy.error;
        QTemporaryDir temporary(QDir(root()).filePath(QStringLiteral(".thoughts-import-XXXXXX")));
        if (!temporary.isValid()) return QStringLiteral("Cannot create the source folder.");
        const auto checkout = op.value(QStringLiteral("git_checkout")).toString();
        if (!checkout.isEmpty()) {
            const auto staging = QFileInfo(QDir(root()).filePath(QStringLiteral(".thoughts/git-imports"))).canonicalFilePath();
            const auto canonical = QFileInfo(checkout).canonicalFilePath();
            if (staging.isEmpty() || !canonical.startsWith(staging + QLatin1Char('/')) || QFileInfo(checkout).isSymLink()
                || !QFileInfo(QDir(checkout).filePath(QStringLiteral(".git"))).isDir())
                return QStringLiteral("The Git checkout is unavailable or outside the import store.");
            const bool projectLayout = copy.latex && QDir(checkout).exists(QStringLiteral("tex/source"));
            if (projectLayout) {
                // The import needs its own manifest and receipt. Preserve repository
                // versions in Git metadata before generating these local files.
                for (const auto &name : {QStringLiteral("thoughts-project.json"), QStringLiteral(".thoughts-import.json")}) {
                    const auto original = QDir(checkout).filePath(name);
                    if (QFileInfo(original).isSymLink()) return QStringLiteral("The repository contains a symlink at an import metadata path.");
                    if (!QFileInfo::exists(original)) continue;
                    const auto backup = QDir(checkout).filePath(QStringLiteral(".git/thoughts-import-originals/") + name);
                    if (!QDir().mkpath(QFileInfo(backup).absolutePath()) || !QFile::copy(original, backup))
                        return QStringLiteral("Cannot preserve the repository's original import metadata.");
                }
            }
            const auto target = projectLayout ? temporary.path() : QDir(temporary.path()).filePath(copy.repositoryLayout ? QStringLiteral("tex/source") : copy.latex ? QStringLiteral("tex") : QStringLiteral("markdown/source"));
            if (projectLayout) QDir().rmdir(temporary.path());
            else if (!QDir().mkpath(QFileInfo(target).absolutePath())) return QStringLiteral("Cannot create the Git source folder.");
            if (!QDir().rename(checkout, target)) return QStringLiteral("Cannot move the Git checkout into Sources.");
            QDir().rmdir(QFileInfo(checkout).absolutePath());
        }
        for (auto it = copy.files.cbegin(); checkout.isEmpty() && it != copy.files.cend(); ++it) {
            const auto path = QDir(temporary.path()).filePath(copy.sourcePath(it.key()));
            QFile file(path);
            if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) || file.write(it.value()) != it.value().size() || !file.flush()) return QStringLiteral("Cannot create source: ") + it.key();
            QDateTime modified = QFileInfo(op.value(QStringLiteral("local_path")).toString()).lastModified();
            for (const auto &value : op.value(QStringLiteral("documents")).toArray()) if (value.toObject().value(QStringLiteral("relative_path")).toString() == it.key()) {
                const auto recorded = QDateTime::fromString(value.toObject().value(QStringLiteral("modified_at")).toString(), Qt::ISODateWithMs);
                if (recorded.isValid()) modified = recorded;
            }
            file.setFileTime(modified, QFileDevice::FileModificationTime);
        }
        const QString ignorePath = QDir(temporary.path()).filePath(QStringLiteral(".ThoughtIgnore"));
        const QFileInfo ignoreInfo(ignorePath);
        if (ignoreInfo.isSymLink()) return QStringLiteral("The source project has a symlink at .ThoughtIgnore.");
        if (op.value(QStringLiteral("thought_ignore_present")).toBool() || !ignoreInfo.exists()) {
            const QByteArray encoded = op.value(QStringLiteral("thought_ignore")).toString().toLatin1();
            const auto decoded = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
            if (!decoded || decoded.decoded.size() > MarkdownFolderImport::DocumentLimit)
                return QStringLiteral("The saved .ThoughtIgnore rules are invalid.");
            const QByteArray content = op.value(QStringLiteral("thought_ignore_present")).toBool() ? decoded.decoded : ThoughtIgnore::defaultRules();
            QFile ignore(ignorePath);
            if (!ignore.open(QIODevice::WriteOnly) || ignore.write(content) != content.size() || !ignore.flush())
                return QStringLiteral("Cannot create .ThoughtIgnore in the source project.");
        }
        const ThoughtIgnore validatedIgnore(temporary.path());
        if (!validatedIgnore.error().isEmpty()) return validatedIgnore.error();
        QJsonArray documents;
        for (const auto &v : copy.projectDocuments(project)) { auto d = v.toObject(); d.insert(QStringLiteral("document_id"), op.value(QStringLiteral("source_document_ids")).toObject().value(d.value(QStringLiteral("source")).toString())); d.insert(QStringLiteral("included"), checkout.isEmpty()); d.remove(QStringLiteral("id")); d.remove(QStringLiteral("title")); documents.append(d); }
        const auto manifest = QJsonDocument(DocumentBuildSelection::uniformStorage(QJsonObject{{QStringLiteral("schema_version"), 4}, {QStringLiteral("document_id"), op.value(QStringLiteral("document_id"))}, {QStringLiteral("documents"), documents}})).toJson();
        QFile manifestFile(QDir(temporary.path()).filePath(QStringLiteral("thoughts-project.json")));
        if (!manifestFile.open(QIODevice::WriteOnly) || manifestFile.write(manifest) != manifest.size() || !manifestFile.flush()) return QStringLiteral("Cannot create the source project manifest.");
        QFile marker(QDir(temporary.path()).filePath(QStringLiteral(".thoughts-import.json")));
        const auto receipt = QJsonDocument(QJsonObject{{QStringLiteral("operation_id"), op.value(QStringLiteral("operation_id"))}, {QStringLiteral("original_sha256"), op.value(QStringLiteral("sha256"))},
            {QStringLiteral("manifest_sha256"), QString::fromLatin1(QCryptographicHash::hash(manifest, QCryptographicHash::Sha256).toHex())}}).toJson();
        if (!marker.open(QIODevice::WriteOnly) || marker.write(receipt) != receipt.size()) return QStringLiteral("Cannot record the source copy.");
        marker.close();
        // Windows cannot rename a folder while a file in it is open.
        manifestFile.close();
        if (!QDir().rename(temporary.path(), destination)) return QStringLiteral("Cannot activate the source folder.");
        return {};
    }
    QJsonObject localSourceLocation(const QJsonObject &folder) const {
        const QString owner = folder.value(QStringLiteral("project")).toString();
        if (!QRegularExpression(QStringLiteral("^docs-[A-Za-z0-9_.-]+$")).match(owner).hasMatch()) return {};
        const QDir directory(QDir(root()).filePath(owner));
        const QFileInfo rootInfo(directory.absolutePath());
        if (!rootInfo.isDir() || rootInfo.isSymLink()) return {};
        const ThoughtIgnore ignored(directory.absolutePath());
        if (!ignored.error().isEmpty()) { m_error = ignored.error(); return {}; }
        QJsonArray files;
        const auto scan = [&](const auto &self, const QDir &current) -> void {
            for (const QFileInfo &file : current.entryInfoList(QDir::Files | QDir::Dirs | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name)) {
                const QString relative = directory.relativeFilePath(file.absoluteFilePath());
                if (file.isSymLink() || ignored.excludes(relative, file.isDir())) continue;
                if (file.isDir()) { self(self, QDir(file.absoluteFilePath())); continue; }
                files.append(QJsonObject{{QStringLiteral("id"), QString(owner + QLatin1Char('/') + relative)},
                    {QStringLiteral("name"), file.fileName()}, {QStringLiteral("relative_path"), relative},
                    {QStringLiteral("size_bytes"), file.size()}});
            }
        };
        scan(scan, directory);
        QString name = folder.value(QStringLiteral("folder")).toString();
        if (name.isEmpty()) name = owner;
        return {{QStringLiteral("id"), owner}, {QStringLiteral("name"), name}, {QStringLiteral("files"), files},
            {QStringLiteral("pending_import"), folder.value(QStringLiteral("pending_import"))}, {QStringLiteral("import_operation_id"), folder.value(QStringLiteral("import_operation_id"))},
            {QStringLiteral("build_error"), folder.value(QStringLiteral("build_error"))}};
    }
    QJsonObject read() const {
        m_error.clear();
        if (!QFile::exists(m_path)) return {{QStringLiteral("projects"), QJsonArray{}}, {QStringLiteral("pending"), QJsonArray{}}};
        // Read and parsed again only when it changed (about 1 MB, read by every Library load and change check).
        const CachedJsonFile file = cachedJsonFile(m_path);
        if (!file.opened) { m_error = QStringLiteral("The project catalogue could not be read."); return {}; }
        if (file.error != QJsonParseError::NoError || !file.document.isObject()) { m_error = QStringLiteral("The project catalogue is damaged; it has been preserved."); return {}; }
        return file.document.object();
    }
    QString write(const QJsonObject &value) const {
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        QSaveFile file(m_path);
        const auto data = QJsonDocument(value).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) return QStringLiteral("The project change could not be saved.");
        return {};
    }
public:
    explicit LibraryProjectStore(const QString &root) : m_path(QDir(root).filePath(QStringLiteral(".thoughts/library-projects.json"))) {}
    QString error() const { return m_error; }
    QString path() const { return m_path; }
    // Fedora's source folder renames as last received, [{folder_id, old, new}].
    static SourceFolderRenames sourceFolderRenames(const QString &root) {
        QFile file(QDir(root).filePath(QStringLiteral(".thoughts/library-projects.json")));
        if (!file.open(QIODevice::ReadOnly)) return {};
        return SourceFolderNames::fromJson(QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("source_folder_renames")).toArray());
    }
    // The name of the project whose documents include this Library PDF, as the
    // Library tree shows it; empty when no project lists it.
    static QString projectNameForPdf(const QJsonArray &projects, const QString &pdfFilename) {
        if (pdfFilename.isEmpty()) return {};
        for (const auto &projectValue : projects) {
            const auto project = projectValue.toObject();
            for (const auto &documentValue : project.value(QStringLiteral("documents")).toArray()) {
                const auto document = documentValue.toObject();
                if (document.value(QStringLiteral("filename")).toString() == pdfFilename
                    || document.value(QStringLiteral("attachment_filename")).toString() == pdfFilename)
                    return project.value(QStringLiteral("name")).toString().trimmed();
            }
        }
        return {};
    }
    // Where this computer's link imports came from, by Library PDF filename.
    // This stays on this computer; the synced catalogue has no field for it.
    static QString sourceLinksPath(const QString &root) {
        return QDir(root).filePath(QStringLiteral(".thoughts/document-links.json"));
    }
    static QUrl acceptedSourceLink(const QString &text) {
        const QUrl link(text, QUrl::StrictMode);
        return link.isValid() && (link.scheme() == QLatin1String("https") || link.scheme() == QLatin1String("http")) ? link : QUrl();
    }
    // The link a PDF was imported from: this computer's own link import
    // first, else the URL Fedora downloaded it from (POST
    // /v1/library/url-imports keeps it as metadata.source_url of the PDF's
    // /v1/library entry), as the Library generation this computer installed
    // last lists it (current_pdfs/.thoughts-generation.json).
    static QUrl sourceLinkForPdf(const QString &root, const QString &pdfFilename) {
        if (pdfFilename.isEmpty()) return {};
        QFile file(sourceLinksPath(root));
        if (file.open(QIODevice::ReadOnly)) {
            const QUrl link = acceptedSourceLink(QJsonDocument::fromJson(file.readAll()).object().value(pdfFilename).toString());
            if (!link.isEmpty()) return link;
        }
        QFile generation(QDir(root).filePath(QStringLiteral("current_pdfs/.thoughts-generation.json")));
        if (!generation.open(QIODevice::ReadOnly)) return {};
        for (const auto &value : QJsonDocument::fromJson(generation.readAll()).object().value(QStringLiteral("publications")).toArray()) {
            const auto publication = value.toObject();
            if (publication.value(QStringLiteral("filename")).toString() == pdfFilename)
                return acceptedSourceLink(publication.value(QStringLiteral("metadata")).toObject().value(QStringLiteral("source_url")).toString());
        }
        return {};
    }
    static bool recordSourceLink(const QString &root, const QString &pdfFilename, const QUrl &link) {
        if (pdfFilename.isEmpty() || !link.isValid()) return false;
        const QString path = sourceLinksPath(root);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QLockFile lock(path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return false;
        QJsonObject links;
        QFile existing(path);
        if (existing.open(QIODevice::ReadOnly)) links = QJsonDocument::fromJson(existing.readAll()).object();
        existing.close();
        links.insert(pdfFilename, link.toString());
        QSaveFile file(path);
        const auto data = QJsonDocument(links).toJson();
        return file.open(QIODevice::WriteOnly) && file.write(data) == data.size() && file.commit();
    }
    QString projectManifestPath(const QString &id) const {
        return ProjectConfiguration::path(root(), id);
    }
    static bool ownsSourceFolder(const QJsonObject &project, const QString &owner) {
        const auto configuration = project.value(QStringLiteral("project_configuration")).toObject();
        for (const auto &entry : configuration.value(QStringLiteral("source_folders")).toArray()) {
            const auto folder = entry.toObject();
            if (folder.value(QStringLiteral("folder_id")) != configuration.value(QStringLiteral("default_folder_id"))
                && folder.value(QStringLiteral("path")).toString() == owner) return true;
        }
        return false;
    }
    static QJsonObject normalizedOperation(QJsonObject operation) {
        if (operation.value(QStringLiteral("action")).toString() == QLatin1String("import_folder") && operation.value(QStringLiteral("name")).toString().trimmed().isEmpty())
            operation.insert(QStringLiteral("name"), QStringLiteral("Imported folder"));
        return operation;
    }
    QJsonArray pending() const {
        QJsonArray result;
        for (const auto &value : read().value(QStringLiteral("pending")).toArray()) result.append(normalizedOperation(value.toObject()));
        return result;
    }
    QJsonArray projects(bool includeSourceInventory = true, const QHash<QString, QJsonObject> *cachedInventory = nullptr) const {
        const auto state = read();
        if (!m_error.isEmpty()) return {};
        QHash<QString, QJsonObject> cached;
        for (const auto &raw : state.value(QStringLiteral("projects")).toArray()) {
            const auto project = raw.toObject();
            cached.insert(project.value(QStringLiteral("id")).toString(), project);
        }
        QJsonArray projects;
        for (const auto &filename : DefaultFolderLocation::manifestPaths(root())) {
            const auto configuration = ProjectConfiguration::read(filename, &m_error);
            if (!m_error.isEmpty()) return {};
            const auto id = configuration.value(QStringLiteral("id")).toString();
            if (!ProjectConfiguration::storedInItsDefaultFolder(root(), filename, configuration)
                || QFileInfo(filename).absolutePath().section(QLatin1Char('/'), -2, -2) != SourceOwner::projectHash(id)) {
                m_error = QStringLiteral("The Project manifest belongs to a different Project."); return {};
            }
            auto project = cached.value(id, QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("revision"), 0},
                {QStringLiteral("target"), QString()}, {QStringLiteral("documents"), QJsonArray{}}, {QStringLiteral("sources"), QJsonArray{}}});
            project.insert(QStringLiteral("project_configuration"), configuration);
            projects.append(ProjectConfiguration::organize(configuration, project));
        }
        for (const auto &value : state.value(QStringLiteral("pending")).toArray()) {
            const auto op = normalizedOperation(value.toObject());
            const auto id = op.value(QStringLiteral("project_id"));
            const auto action = op.value(QStringLiteral("action")).toString();
            if (action == QLatin1String("move_source")) {
                const QString owner = op.value(QStringLiteral("source_project")).toString();
                QJsonObject folder;
                QJsonArray movedDocuments, movedSources;
                for (int i = 0; i < projects.size(); ++i) {
                    auto source = projects[i].toObject();
                    if (source.value(QStringLiteral("id")) != id) continue;
                    auto folders = source.value(QStringLiteral("source_folders")).toArray();
                    for (int j = folders.size() - 1; j >= 0; --j)
                        if (folders[j].toObject().value(QStringLiteral("project")).toString() == owner) {
                            folder = folders[j].toObject(); folders.removeAt(j);
                        }
                    source.insert(QStringLiteral("source_folders"), folders);
                    auto documents = source.value(QStringLiteral("documents")).toArray();
                    for (int j = documents.size() - 1; j >= 0; --j) {
                        const auto document = documents[j].toObject();
                        if (document.value(QStringLiteral("source_project")).toString() == owner
                            || document.value(QStringLiteral("id")).toString().startsWith(owner + QLatin1Char('/'))) {
                            movedDocuments.prepend(document); documents.removeAt(j);
                        }
                    }
                    source.insert(QStringLiteral("documents"), documents);
                    auto sources = source.value(QStringLiteral("sources")).toArray();
                    for (int j = sources.size() - 1; j >= 0; --j) {
                        const QString path = sources[j].toObject().value(QStringLiteral("id")).toString();
                        if (path == owner || path.startsWith(owner + QLatin1Char('/'))) {
                            movedSources.prepend(sources[j]); sources.removeAt(j);
                        }
                    }
                    source.insert(QStringLiteral("sources"), sources);
                    source.insert(QStringLiteral("revision"), source.value(QStringLiteral("revision")).toInt() + 1);
                    projects[i] = source;
                }
                for (int i = 0; i < projects.size() && !folder.isEmpty(); ++i) {
                    auto destination = projects[i].toObject();
                    if (destination.value(QStringLiteral("id")) != op.value(QStringLiteral("destination_project_id"))) continue;
                    auto folders = destination.value(QStringLiteral("source_folders")).toArray(); folders.append(folder);
                    destination.insert(QStringLiteral("source_folders"), folders);
                    auto documents = destination.value(QStringLiteral("documents")).toArray();
                    for (const auto &document : movedDocuments) documents.append(document);
                    destination.insert(QStringLiteral("documents"), documents);
                    auto sources = destination.value(QStringLiteral("sources")).toArray();
                    for (const auto &entry : movedSources) sources.append(entry);
                    destination.insert(QStringLiteral("sources"), sources);
                    destination.insert(QStringLiteral("revision"), destination.value(QStringLiteral("revision")).toInt() + 1);
                    projects[i] = destination;
                }
                continue;
            }
            if (action == QLatin1String("move_source_document")) {
                // Shown in its destination until Fedora applies the move.
                QJsonObject moved;
                const QString documentId = op.value(QStringLiteral("document_id")).toString();
                for (int i = 0; i < projects.size(); ++i) {
                    auto project = projects[i].toObject();
                    if (project.value(QStringLiteral("id")) != id) continue;
                    auto documents = project.value(QStringLiteral("documents")).toArray();
                    for (int j = documents.size() - 1; j >= 0; --j) {
                        if (documents[j].toObject().value(QStringLiteral("document_id")).toString() == documentId) {
                            moved = documents[j].toObject(); documents.removeAt(j);
                        }
                    }
                    project.insert(QStringLiteral("documents"), documents);
                    project.insert(QStringLiteral("revision"), project.value(QStringLiteral("revision")).toInt() + 1);
                    projects[i] = project;
                }
                for (int i = 0; i < projects.size() && !moved.isEmpty(); ++i) {
                    auto project = projects[i].toObject();
                    if (project.value(QStringLiteral("id")) != op.value(QStringLiteral("destination_project_id"))) continue;
                    moved.insert(QStringLiteral("pending_move"), true);
                    auto documents = project.value(QStringLiteral("documents")).toArray(); documents.append(moved);
                    project.insert(QStringLiteral("documents"), documents);
                    project.insert(QStringLiteral("revision"), project.value(QStringLiteral("revision")).toInt() + 1);
                    projects[i] = project;
                }
                continue;
            }
            if (action == QLatin1String("move_document")) {
                QJsonObject moved;
                for (int i = 0; i < projects.size(); ++i) {
                    auto project = projects[i].toObject();
                    if (project.value(QStringLiteral("id")) != id) continue;
                    auto documents = project.value(QStringLiteral("documents")).toArray();
                    for (int j = documents.size() - 1; j >= 0; --j) {
                        if (documents[j].toObject().value(QStringLiteral("id")) == op.value(QStringLiteral("document_id"))) {
                            moved = documents[j].toObject(); documents.removeAt(j);
                        }
                    }
                    auto removed = project.value(QStringLiteral("removed_documents")).toArray();
                    removed.append(op.value(QStringLiteral("document_id")));
                    project.insert(QStringLiteral("removed_documents"), removed);
                    project.insert(QStringLiteral("documents"), documents);
                    project.insert(QStringLiteral("revision"), project.value(QStringLiteral("revision")).toInt() + 1);
                    projects[i] = project;
                }
                for (int i = 0; i < projects.size() && !moved.isEmpty(); ++i) {
                    auto project = projects[i].toObject();
                    if (project.value(QStringLiteral("id")) != op.value(QStringLiteral("destination_project_id"))) continue;
                    const auto filename = moved.value(QStringLiteral("attachment_filename")).toString(moved.value(QStringLiteral("filename")).toString());
                    moved.insert(QStringLiteral("id"), filename);
                    moved.insert(QStringLiteral("pending_move"), true);
                    moved.insert(QStringLiteral("imported"), true);
                    for (const auto &field : {"source_project", "folder", "relative_path", "group"}) moved.remove(QString::fromLatin1(field));
                    QJsonArray removed;
                    for (const auto &value : project.value(QStringLiteral("removed_documents")).toArray())
                        if (value != moved.value(QStringLiteral("id"))) removed.append(value);
                    project.insert(QStringLiteral("removed_documents"), removed);
                    auto documents = project.value(QStringLiteral("documents")).toArray(); documents.append(moved);
                    project.insert(QStringLiteral("documents"), documents);
                    project.insert(QStringLiteral("revision"), project.value(QStringLiteral("revision")).toInt() + 1);
                    projects[i] = project;
                }
                continue;
            }
            for (int i = 0; i < projects.size(); ++i) {
                auto project = projects[i].toObject();
                if (project.value(QStringLiteral("id")) != id) continue;
                if (action == QLatin1String("remove")) { projects.removeAt(i); break; }
                if (action == QLatin1String("rename")) project.insert(QStringLiteral("name"), op.value(QStringLiteral("name")));
                // Knowledge cards or Project cards (review-card-kinds.md §2.3), shown at once.
                if (action == QLatin1String("set_review_kind")) project.insert(QStringLiteral("review_kind"), op.value(QStringLiteral("review_kind")));
                auto documents = project.value(QStringLiteral("documents")).toArray();
                if (action == QLatin1String("rename_source")) {
                    const QString owner = op.value(QStringLiteral("source_project")).toString();
                    auto folders = project.value(QStringLiteral("source_folders")).toArray();
                    bool labelled = false;
                    for (int j = 0; j < folders.size(); ++j) {
                        auto folder = folders[j].toObject();
                        if (folder.value(QStringLiteral("project")).toString() != owner) continue;
                        folder.insert(QStringLiteral("folder"), op.value(QStringLiteral("name"))); folders[j] = folder; labelled = true;
                    }
                    if (!labelled) folders.append(QJsonObject{{QStringLiteral("project"), owner}, {QStringLiteral("folder"), op.value(QStringLiteral("name"))}});
                    project.insert(QStringLiteral("source_folders"), folders);
                    auto sources = project.value(QStringLiteral("sources")).toArray();
                    for (int j = 0; j < sources.size(); ++j) {
                        auto location = sources[j].toObject();
                        if (location.value(QStringLiteral("id")).toString() != owner) continue;
                        location.insert(QStringLiteral("name"), op.value(QStringLiteral("name"))); sources[j] = location;
                    }
                    project.insert(QStringLiteral("sources"), sources);
                }
                if (action == QLatin1String("remove_source")) {
                    const QString owner = op.value(QStringLiteral("source_project")).toString();
                    const QString prefix = owner + QLatin1Char('/');
                    auto folders = project.value(QStringLiteral("source_folders")).toArray();
                    for (int j = folders.size() - 1; j >= 0; --j)
                        if (folders[j].toObject().value(QStringLiteral("project")).toString() == owner) folders.removeAt(j);
                    project.insert(QStringLiteral("source_folders"), folders);
                    auto sources = project.value(QStringLiteral("sources")).toArray();
                    for (int j = sources.size() - 1; j >= 0; --j)
                        if (sources[j].toObject().value(QStringLiteral("id")).toString() == owner || sources[j].toObject().value(QStringLiteral("id")).toString().startsWith(prefix)) sources.removeAt(j);
                    project.insert(QStringLiteral("sources"), sources);
                    for (int j = documents.size() - 1; j >= 0; --j) {
                        const auto doc = documents[j].toObject();
                        if (doc.value(QStringLiteral("source_project")).toString() == owner || doc.value(QStringLiteral("id")).toString().startsWith(prefix)) documents.removeAt(j);
                    }
                }
                if (action == QLatin1String("import_folder")) {
                    const QString sourceProject = LibraryProjectStore::sourceProject(op);
                    const auto error = materializeSources(op);
                    if (!error.isEmpty()) m_error = error;
                    auto folders = project.value(QStringLiteral("source_folders")).toArray();
                    folders.append(QJsonObject{{QStringLiteral("project"), sourceProject}, {QStringLiteral("folder"), op.value(QStringLiteral("name"))},
                        {QStringLiteral("pending_import"), true}, {QStringLiteral("import_operation_id"), op.value(QStringLiteral("operation_id"))}, {QStringLiteral("import_error"), error}});
                    project.insert(QStringLiteral("source_folders"), folders);
                }
                if (action == QLatin1String("import")) {
                    auto doc = op;
                    doc.insert(QStringLiteral("id"), op.value(QStringLiteral("filename")).toString());
                    doc.insert(QStringLiteral("start_page"), 1); doc.insert(QStringLiteral("imported"), true);
                    bool present = false;
                    for (int j = 0; j < documents.size(); ++j) {
                        if (documents[j].toObject().value(QStringLiteral("document_id")) == doc.value(QStringLiteral("document_id"))) {
                            documents[j] = doc; present = true; break;
                        }
                    }
                    if (!present) documents.append(doc);
                }
                if (action == QLatin1String("rename_document")) for (int j = 0; j < documents.size(); ++j) {
                    auto document = documents[j].toObject();
                    if (document.value(QStringLiteral("id")) == op.value(QStringLiteral("document_id"))) {
                        document.insert(QStringLiteral("name"), op.value(QStringLiteral("name")));
                        documents[j] = document;
                    }
                }
                if (action == QLatin1String("remove_document")) {
                    // As Fedora does: a removed source keeps its identity for the next
                    // declaration. The configuration no longer declares it; the catalogue does.
                    for (const auto &raw : cached.value(id.toString()).value(QStringLiteral("documents")).toArray()) {
                        const auto doc = raw.toObject();
                        if (doc.value(QStringLiteral("id")) != op.value(QStringLiteral("document_id")) || doc.value(QStringLiteral("imported")).toBool()
                            || doc.value(QStringLiteral("source_project")).toString().isEmpty() || !DocumentIdentityStore::validId(doc.value(QStringLiteral("document_id")).toString())) continue;
                        auto removed = project.value(QStringLiteral("removed_identities")).toObject();
                        removed.insert(doc.value(QStringLiteral("id")).toString(), doc.value(QStringLiteral("document_id")));
                        project.insert(QStringLiteral("removed_identities"), removed);
                    }
                    for (int j = documents.size() - 1; j >= 0; --j)
                        if (documents[j].toObject().value(QStringLiteral("id")) == op.value(QStringLiteral("document_id"))) documents.removeAt(j);
                }
                if (action == QLatin1String("set_document_published")) {
                    // As Fedora does: the document moves between `documents` and
                    // `unpublished_documents` and keeps everything else
                    // (protocol/project-document-publication.md).
                    const bool published = op.value(QStringLiteral("published")).toBool();
                    const QJsonValue key = op.value(QStringLiteral("document_id"));
                    auto unpublished = project.value(QStringLiteral("unpublished_documents")).toArray();
                    QJsonArray &from = published ? unpublished : documents;
                    QJsonArray &to = published ? documents : unpublished;
                    for (int j = from.size() - 1; j >= 0; --j) {
                        auto document = from[j].toObject();
                        if (document.value(QStringLiteral("id")) != key && document.value(QStringLiteral("document_id")) != key) continue;
                        document.insert(QStringLiteral("published"), published);
                        document.insert(QStringLiteral("included"), published);
                        from.removeAt(j);
                        to.append(document);
                    }
                    project.insert(QStringLiteral("unpublished_documents"), unpublished);
                }
                if (action == QLatin1String("order")) {
                    QJsonArray ordered;
                    for (const auto &key : op.value(QStringLiteral("documents")).toArray()) for (const auto &doc : documents)
                        if (doc.toObject().value(QStringLiteral("id")) == key) ordered.append(doc);
                    documents = ordered;
                }
                project.insert(QStringLiteral("documents"), documents);
                project.insert(QStringLiteral("revision"), project.value(QStringLiteral("revision")).toInt() + 1);
                projects[i] = project;
                break;
            }
        }
        if (!includeSourceInventory) return projects;
        if (cachedInventory) {
            // The UI consumes one metadata snapshot; never enumerate source
            // folders or read ignore rules while constructing the tree.
            for (int i = 0; i < projects.size(); ++i) {
                auto project = projects[i].toObject();
                QMap<QString, QString> folders;
                const auto configuration = project.value(QStringLiteral("project_configuration")).toObject();
                for (const auto &entry : configuration.value(QStringLiteral("source_folders")).toArray()) {
                    const auto folder = entry.toObject();
                    const auto owner = folder.value(QStringLiteral("path")).toString();
                    folders.insert(owner, folder.value(QStringLiteral("folder_id")) == configuration.value(QStringLiteral("default_folder_id"))
                        ? QStringLiteral("Default source folder") : owner);
                }
                for (const auto &entry : project.value(QStringLiteral("source_folders")).toArray()) {
                    const auto folder = entry.toObject();
                    folders.insert(folder.value(QStringLiteral("project")).toString(), folder.value(QStringLiteral("folder")).toString());
                }
                QJsonArray sources;
                for (auto folder = folders.cbegin(); folder != folders.cend(); ++folder) {
                    if (folder.key().isEmpty()) continue;
                    auto location = cachedInventory->value(folder.key());
                    if (location.isEmpty()) {
                        location.insert(QStringLiteral("size_state"), QStringLiteral("checking"));
                        // Keep recorded source rows visible during the first scan.
                        QJsonArray files;
                        QSet<QString> seen;
                        for (const auto &entry : project.value(QStringLiteral("sources")).toArray()) {
                            const auto source = entry.toObject();
                            const auto id = source.value(QStringLiteral("id")).toString();
                            if (id != folder.key() && !id.startsWith(folder.key() + QLatin1Char('/'))) continue;
                            for (const auto &raw : source.value(QStringLiteral("files")).toArray()) {
                                auto file = raw.toObject();
                                const auto path = file.value(QStringLiteral("id")).toString();
                                if (!path.startsWith(folder.key() + QLatin1Char('/')) || seen.contains(path)) continue;
                                file.insert(QStringLiteral("relative_path"), path.mid(folder.key().size() + 1));
                                files.append(file);
                                seen.insert(path);
                            }
                        }
                        location.insert(QStringLiteral("files"), files);
                    }
                    location.insert(QStringLiteral("id"), folder.key());
                    location.insert(QStringLiteral("name"), folder.value().isEmpty() ? folder.key() : folder.value());
                    for (const auto &entry : project.value(QStringLiteral("sources")).toArray()) {
                        const auto saved = entry.toObject();
                        if (saved.value(QStringLiteral("id")).toString() == folder.key())
                            for (const auto &field : {"pending_import", "import_operation_id", "build_error"})
                                if (saved.contains(QLatin1String(field))) location.insert(QLatin1String(field), saved.value(QLatin1String(field)));
                    }
                    sources.append(location);
                }
                project.insert(QStringLiteral("sources"), sources);
                projects[i] = project;
            }
            return projects;
        }
        for (int i = 0; i < projects.size(); ++i) {
            auto project = projects[i].toObject(); auto sources = project.value(QStringLiteral("sources")).toArray();
            QMap<QString, QJsonObject> folders;
            const auto configuration = project.value(QStringLiteral("project_configuration")).toObject();
            for (const auto &entry : configuration.value(QStringLiteral("source_folders")).toArray()) {
                const auto folder = entry.toObject();
                if (folder.value(QStringLiteral("folder_id")) == configuration.value(QStringLiteral("default_folder_id"))) continue;
                const auto owner = folder.value(QStringLiteral("path")).toString();
                folders.insert(owner, {{QStringLiteral("project"), owner}, {QStringLiteral("folder"), owner}});
            }
            for (const auto &value : project.value(QStringLiteral("source_folders")).toArray()) {
                const auto folder = value.toObject();
                folders.insert(folder.value(QStringLiteral("project")).toString(), folder);
            }
            for (auto folder = folders.cbegin(); folder != folders.cend(); ++folder) {
                const QString owner = folder.key();
                if (!QRegularExpression(QStringLiteral("^docs-[A-Za-z0-9_.-]+$")).match(owner).hasMatch()) continue;
                auto local = cachedInventory ? cachedInventory->value(owner) : localSourceLocation(folder.value());
                if (local.isEmpty()) {
                    // Keep the saved inventory usable while a background scan runs,
                    // including when the connected folder is temporarily unavailable.
                    QJsonArray files;
                    QSet<QString> known;
                    const ThoughtIgnore ignored(QDir(root()).filePath(owner));
                    for (const auto &entry : sources) {
                        const auto location = entry.toObject();
                        const QString id = location.value(QStringLiteral("id")).toString();
                        if (id != owner && !id.startsWith(owner + QLatin1Char('/'))) continue;
                        for (const auto &value : location.value(QStringLiteral("files")).toArray()) {
                            auto file = value.toObject();
                            const QString path = file.value(QStringLiteral("id")).toString();
                            if (!path.startsWith(owner + QLatin1Char('/')) || known.contains(path)) continue;
                            const QString relative = path.mid(owner.size() + 1);
                            if (ignored.excludes(relative)) continue;
                            file.insert(QStringLiteral("relative_path"), relative);
                            files.append(file); known.insert(path);
                        }
                    }
                    const QString ignoreId = owner + QStringLiteral("/.ThoughtIgnore");
                    const QFileInfo ignoreFile(QDir(root()).filePath(ignoreId));
                    if (!known.contains(ignoreId) && ignoreFile.isFile() && !ignoreFile.isSymLink() && !ignored.excludes(QStringLiteral(".ThoughtIgnore")))
                        files.append(QJsonObject{{QStringLiteral("id"), ignoreId}, {QStringLiteral("name"), QStringLiteral(".ThoughtIgnore")},
                            {QStringLiteral("relative_path"), QStringLiteral(".ThoughtIgnore")}, {QStringLiteral("size_bytes"), ignoreFile.size()}});
                    local.insert(QStringLiteral("id"), owner);
                    local.insert(QStringLiteral("files"), files);
                }
                QString name = folder.value().value(QStringLiteral("folder")).toString();
                local.insert(QStringLiteral("name"), name.isEmpty() ? owner : name);
                for (const auto &field : {"pending_import", "import_operation_id", "build_error"})
                    local.insert(QString::fromLatin1(field), folder.value().value(QString::fromLatin1(field)));
                // Replace all legacy subfolder groups with the connected root.
                for (int j = sources.size() - 1; j >= 0; --j) {
                    const QString id = sources[j].toObject().value(QStringLiteral("id")).toString();
                    if (id == owner || id.startsWith(owner + QLatin1Char('/'))) sources.removeAt(j);
                }
                sources.append(local);
            }
            project.insert(QStringLiteral("sources"), sources);
            // The central configuration already established selection and global order.
            // Preserve pending operation overlays instead of reapplying per-folder order.
            projects[i] = project;
        }
        return projects;
    }
    QString prepareFolderUpload(const QString &operationId) const {
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The source import is busy.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        auto pending = state.value(QStringLiteral("pending")).toArray();
        for (int i = 0; i < pending.size(); ++i) {
            auto op = normalizedOperation(pending[i].toObject());
            if (op.value(QStringLiteral("operation_id")).toString() != operationId) continue;
            if (op.value(QStringLiteral("upload_frozen")).toBool()) return {};
            if (!SourceFolderNames::isStrictFolderName(op.value(QStringLiteral("source_project")).toString())) {
                // Queued before folders were named by rule N: name it now, so
                // Fedora and this computer use one name.
                const auto taken = knownFolderNames(state, operationId);
                const auto name = SourceFolderNames::importFolderName(op.value(QStringLiteral("name")).toString(), operationId,
                    [&taken](const QString &candidate) { return taken.contains(candidate); });
                if (name.isEmpty()) return QStringLiteral("No free name was found for the imported folder.");
                const auto error = moveImportFolder(state, operationId, legacyImportFolder(operationId), name);
                if (!error.isEmpty()) return error;
                pending = state.value(QStringLiteral("pending")).toArray();
                op = normalizedOperation(pending[i].toObject());
                op.insert(QStringLiteral("source_project"), name);
            }
            const auto error = materializeSources(op); if (!error.isEmpty()) return error;
            const auto ownerRoot = QDir(root()).filePath(sourceProject(op));
            const auto directory = QDir(ownerRoot).exists(QStringLiteral("tex/source")) ? QDir(ownerRoot).filePath(QStringLiteral("tex")) : QDir(ownerRoot).filePath(QStringLiteral("markdown/source"));
            if (QFileInfo(directory).isSymLink() || !QFileInfo(directory).canonicalFilePath().startsWith(QDir(root()).canonicalPath() + QLatin1Char('/') + sourceProject(op) + QLatin1Char('/')))
                return QStringLiteral("The imported source folder has moved outside its project.");
            const bool nativeRepository = QDir(ownerRoot).exists(QStringLiteral("tex/source/.git"));
            auto copy = MarkdownFolderImport::scan(nativeRepository ? QDir(ownerRoot).filePath(QStringLiteral("tex/source")) : directory);
            if (!op.value(QStringLiteral("git_checkout")).toString().isEmpty() && copy.error == QLatin1String("This folder contains no Markdown documents.")) copy.error.clear();
            if (!copy.error.isEmpty()) return copy.error;
            const ThoughtIgnore ignored(ownerRoot);
            if (!ignored.error().isEmpty()) return ignored.error();
            for (auto it = copy.files.begin(); it != copy.files.end();) {
                if (!ignored.excludes(copy.sourcePath(it.key()))) { ++it; continue; }
                copy.sizes.remove(it.key());
                copy.modifiedTimes.remove(it.key());
                it = copy.files.erase(it);
            }
            for (auto it = copy.capturedImages.begin(); it != copy.capturedImages.end();) {
                if (!ignored.excludes(copy.sourcePath(it.key()))) { ++it; continue; }
                copy.sizes.remove(it.key());
                copy.modifiedTimes.remove(it.key());
                it = copy.capturedImages.erase(it);
            }
            for (auto it = copy.documents.begin(); it != copy.documents.end();) {
                if (ignored.excludes(copy.sourcePath(*it))) it = copy.documents.erase(it);
                else ++it;
            }
            QFile manifestFile(QDir(ownerRoot).filePath(QStringLiteral("thoughts-project.json")));
            if (!manifestFile.open(QIODevice::ReadOnly)) return QStringLiteral("Cannot read the registered source documents.");
            const auto manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
            QStringList order;
            for (const auto &entry : manifest.value(QStringLiteral("documents")).toArray()) {
                const auto document = entry.toObject();
                const auto source = document.value(QStringLiteral("source")).toString();
                const auto path = copy.latex ? source : source.mid(QStringLiteral("markdown/source/").size());
                if (document.value(QStringLiteral("included")).toBool() && copy.documents.contains(path)) order.append(path);
            }
            if (!copy.latex && op.value(QStringLiteral("git_checkout")).toString().isEmpty()) for (const auto &path : copy.documents) if (!order.contains(path)) order.append(path);
            for (const auto &path : order) { const auto issues = copy.issues(path, order); if (!issues.isEmpty()) return issues.join(QLatin1Char('\n')); }
            QStringList inventory = order;
            for (const auto &path : copy.documents) if (!inventory.contains(path)) inventory.append(path);
            const auto rawBundle = copy.bundle(inventory, false);
            if (rawBundle.isEmpty()) return QStringLiteral("The edited source folder exceeds the import limits or could not be copied.");
            auto bundle = QJsonDocument::fromJson(rawBundle).object();
            QFile ignoreFile(QDir(ownerRoot).filePath(QStringLiteral(".ThoughtIgnore")));
            if (!ignoreFile.open(QIODevice::ReadOnly) || ignoreFile.size() > MarkdownFolderImport::DocumentLimit)
                return QStringLiteral("The source project's .ThoughtIgnore file could not be uploaded.");
            const QByteArray ignoreContent = ignoreFile.readAll();
            auto fileEntries = bundle.value(QStringLiteral("files")).toArray();
            fileEntries.append(QJsonObject{{QStringLiteral("path"), QStringLiteral(".ThoughtIgnore")},
                {QStringLiteral("content"), QString::fromLatin1(ignoreContent.toBase64())}});
            bundle.insert(QStringLiteral("files"), fileEntries);
            QJsonArray included;
            for (const auto &path : order) included.append(path);
            bundle.insert(QStringLiteral("included_documents"), included);
            const auto content = QJsonDocument(bundle).toJson(QJsonDocument::Compact);
            if (content.isEmpty()) return QStringLiteral("The edited source folder exceeds the import limits or could not be copied.");
            const auto sha = QString::fromLatin1(QCryptographicHash::hash(content, QCryptographicHash::Sha256).toHex());
            const auto path = QDir(root()).filePath(QStringLiteral(".thoughts/folder-imports/") + sha + QStringLiteral(".json"));
            QSaveFile file(path);
            if (!QDir().mkpath(QFileInfo(path).absolutePath()) || !file.open(QIODevice::WriteOnly) || file.write(content) != content.size() || !file.commit()) return QStringLiteral("Cannot save the source upload.");
            auto sourceIds = op.value(QStringLiteral("source_document_ids")).toObject();
            for (const auto &value : manifest.value(QStringLiteral("documents")).toArray()) {
                const auto entry = value.toObject();
                if (entry.contains(QStringLiteral("source"))) sourceIds.insert(entry.value(QStringLiteral("source")).toString(), entry.value(QStringLiteral("document_id")));
            }
            for (const auto &path : inventory) if (sourceIds.value(copy.sourcePath(path)).toString().isEmpty())
                sourceIds.insert(copy.sourcePath(path), QUuid::createUuid().toString(QUuid::WithoutBraces));
            op.insert(QStringLiteral("source_document_ids"), sourceIds);
            op.insert(QStringLiteral("local_path"), path); op.insert(QStringLiteral("sha256"), sha); op.insert(QStringLiteral("upload_frozen"), true);
            pending[i] = op; state.insert(QStringLiteral("pending"), pending); return write(state);
        }
        return QStringLiteral("The source import is no longer pending.");
    }
    QString releaseRejectedFolderUpload(const QString &operationId) const {
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The source import is busy.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        auto pending = state.value(QStringLiteral("pending")).toArray();
        for (int i = 0; i < pending.size(); ++i) {
            auto op = pending[i].toObject();
            if (op.value(QStringLiteral("operation_id")).toString() != operationId) continue;
            op.remove(QStringLiteral("upload_frozen")); pending[i] = op;
            state.insert(QStringLiteral("pending"), pending); return write(state);
        }
        return {};
    }
    QString enqueue(const QString &action, const QJsonObject &project, QJsonObject values = {}) const {
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("Another project change is being saved. Try again.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        if (action == QLatin1String("move_document") || action == QLatin1String("move_source") || action == QLatin1String("move_source_document")) {
            const auto currentProjects = projects();
            QJsonObject source, destination;
            for (const auto &value : currentProjects) {
                const auto p = value.toObject();
                if (p.value(QStringLiteral("id")) == project.value(QStringLiteral("id"))) source = p;
                if (p.value(QStringLiteral("id")) == values.value(QStringLiteral("destination_project_id"))) destination = p;
            }
            if (source.isEmpty() || destination.isEmpty() || source.value(QStringLiteral("id")) == destination.value(QStringLiteral("id"))
                || source.value(QStringLiteral("revision")).toInt() != project.value(QStringLiteral("revision")).toInt()
                || destination.value(QStringLiteral("revision")).toInt() != values.value(QStringLiteral("destination_revision")).toInt())
                return QStringLiteral("The projects changed. Close this dialog and choose the destination again.");
        }
        if (action == QLatin1String("import")) {
            QString error;
            const auto id = DocumentIdentityStore(root()).registerImport(values.value(QStringLiteral("filename")).toString(),
                values.value(QStringLiteral("sha256")).toString(), values.value(QStringLiteral("local_path")).toString(), &error);
            if (id.isEmpty()) return error;
            values.insert(QStringLiteral("document_id"), id);
        }
        if (action == QLatin1String("include_source"))
            values.insert(QStringLiteral("new_document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (action == QLatin1String("import_folder")) {
            values.insert(QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            const auto copy = MarkdownFolderImport::savedBundle(values.value(QStringLiteral("local_path")).toString(), values.value(QStringLiteral("sha256")).toString());
            if (!copy.error.isEmpty()) return copy.error;
            QJsonObject sourceIds;
            for (const auto &path : copy.documents) sourceIds.insert(copy.sourcePath(path), QUuid::createUuid().toString(QUuid::WithoutBraces));
            values.insert(QStringLiteral("source_document_ids"), sourceIds);
        }
        values.insert(QStringLiteral("action"), action);
        values.insert(QStringLiteral("operation_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
        if (action == QLatin1String("import_folder")) {
            // Rule N: display name = import name, hex = the operation id's; Fedora
            // creates the folder under this name or refuses with source_folder_exists.
            const auto taken = knownFolderNames(state);
            const auto name = SourceFolderNames::importFolderName(normalizedOperation(values).value(QStringLiteral("name")).toString(),
                values.value(QStringLiteral("operation_id")).toString(), [&taken](const QString &candidate) { return taken.contains(candidate); });
            if (name.isEmpty()) return QStringLiteral("No free name was found for the imported folder.");
            values.insert(QStringLiteral("source_project"), name);
        }
        values.insert(QStringLiteral("project_id"), project.value(QStringLiteral("id")));
        values.insert(QStringLiteral("base_revision"), project.value(QStringLiteral("revision")).toInt());
        if (action == QLatin1String("create")) {
            const auto id = project.value(QStringLiteral("id")).toString();
            const auto error = ProjectConfiguration::save(ProjectConfiguration::path(root(), id), ProjectConfiguration::empty(root(), id, values.value(QStringLiteral("name")).toString()));
            if (!error.isEmpty()) return error;
        }
        if (action == QLatin1String("import_folder") && !values.value(QStringLiteral("git_checkout")).toString().isEmpty()) {
            const auto error = materializeSources(values);
            if (!error.isEmpty()) return error;
        }
        const auto configurationError = ProjectConfiguration::change(root(), project, action, values);
        if (!configurationError.isEmpty()) return configurationError;
        recordGenerated(state, project.value(QStringLiteral("id")).toString());
        auto pending = state.value(QStringLiteral("pending")).toArray(); pending.append(values); state.insert(QStringLiteral("pending"), pending);
        return write(state);
    }
    // Copies a chosen or downloaded PDF into the Project's documents folder and
    // queues its import. File and link imports share this.
    QString importPdf(const QJsonObject &project, const QString &path, const QString &name, const QUrl &link = {}) const {
        const auto sha256 = [](const QString &filename) {
            QFile file(filename); QCryptographicHash hash(QCryptographicHash::Sha256);
            return file.open(QIODevice::ReadOnly) && hash.addData(&file) ? QString::fromLatin1(hash.result().toHex()) : QString();
        };
        const QString hash = sha256(path);
        if (hash.isEmpty()) return QObject::tr("The PDF could not be read.");
        QString safe = name;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
        if (safe.isEmpty() || !safe.at(0).isLetterOrNumber()) safe.prepend(QStringLiteral("document_"));
        const QString projectKey = QString::fromLatin1(QCryptographicHash::hash(project.value(QStringLiteral("id")).toString().toUtf8(), QCryptographicHash::Sha256).toHex());
        const QString filename = safe + QLatin1Char('_') + hash.left(12) + QLatin1Char('_') + projectKey.left(8) + QStringLiteral(".pdf");
        // The Project's default folder, or a new one in the Library's spelling (rename spec rule 5).
        const QString directory = QDir(root()).filePath(DefaultFolderLocation::of(root(), projectKey));
        const QString staged = QDir(directory).filePath(filename);
        const bool existed = QFileInfo::exists(staged);
        if (!QDir().mkpath(directory) || (!existed && !QFile::copy(path, staged))) return QObject::tr("The PDF could not be copied into the Library.");
        if (sha256(staged) != hash) return QObject::tr("The staged PDF failed verification.");
        const QString error = enqueue(QStringLiteral("import"), project, {{QStringLiteral("filename"), filename}, {QStringLiteral("sha256"), hash},
            {QStringLiteral("name"), name}, {QStringLiteral("local_path"), staged}, {QStringLiteral("size_bytes"), QFileInfo(staged).size()}});
        if (!error.isEmpty() && !existed) QFile::remove(staged);
        if (error.isEmpty() && !link.isEmpty()) recordSourceLink(root(), filename, link);
        return error;
    }
    // A Project removed on any device leaves its configuration here, and that
    // configuration still connects its docs-* folders: Sync goes on publishing
    // a Project Fedora no longer has, so the source inventory never matches
    // the Library, and the reconciliation that would install Fedora's removal
    // of the file runs only once it does (10 November 2026). Remove this
    // computer's own unedited copy as the catalogue drops the Project; its
    // folders and their files stay where they are.
    QString retireRemovedConfigurations(const QJsonArray &before, const QJsonArray &after, const QSet<QString> &busy, QJsonObject &generated) const {
        QSet<QString> current;
        for (const auto &value : after) current.insert(value.toObject().value(QStringLiteral("id")).toString());
        for (const auto &value : before) {
            const auto project = value.toObject();
            const auto id = project.value(QStringLiteral("id")).toString();
            if (id.isEmpty() || current.contains(id) || busy.contains(id)) continue;
            const auto local = DefaultFolderLocation::existing(root(), SourceOwner::projectHash(id));
            const auto relative = local.isEmpty() ? QDir(root()).relativeFilePath(ProjectConfiguration::path(root(), id)) : local + QStringLiteral("/thoughts-project.json");
            const QFileInfo info(QDir(root()).filePath(relative));
            if (!info.isFile() || info.isSymLink()) continue;
            QFile file(info.filePath());
            if (!file.open(QIODevice::ReadOnly)) return QStringLiteral("The configuration of a removed Project could not be read.");
            const auto bytes = file.readAll();
            file.close();
            // An edit is the user's: normal source Sync reports it.
            if (generated.value(relative).toString() != sha256Hex(bytes) && !createdHere(id, project.value(QStringLiteral("name")).toString(), bytes)) continue;
            if (!QFile::remove(info.filePath())) return QStringLiteral("The configuration of a removed Project could not be removed.");
            generated.remove(relative);
        }
        return {};
    }
    QString receive(const QJsonObject &snapshot, const QString &acknowledged = {}) const {
        QDir().mkpath(QFileInfo(m_path).absolutePath());
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The project catalogue is busy.");
        if (!snapshot.value(QStringLiteral("projects")).isArray()) return QStringLiteral("The server returned an invalid project catalogue.");
        auto state = read(); if (!m_error.isEmpty()) return m_error; QJsonArray retained; QSet<QString> busy;
        for (const auto &op : state.value(QStringLiteral("pending")).toArray()) {
            if (op.toObject().value(QStringLiteral("operation_id")).toString() == acknowledged) continue;
            retained.append(op);
            busy.insert(op.toObject().value(QStringLiteral("project_id")).toString());
            busy.insert(op.toObject().value(QStringLiteral("destination_project_id")).toString());
        }
        auto generated = state.value(generatedKey).toObject();
        const auto manifestError = installSourceManifests(snapshot.value(QStringLiteral("projects")).toArray(), busy, generated);
        if (!manifestError.isEmpty()) return manifestError;
        const auto retireError = retireRemovedConfigurations(state.value(QStringLiteral("projects")).toArray(), snapshot.value(QStringLiteral("projects")).toArray(), busy, generated);
        if (!retireError.isEmpty()) return retireError;
        state.insert(generatedKey, generated);
        // Fedora's folder renames (source folder names spec): kept for the
        // names a new import may not take and for PDF names of renamed folders.
        if (snapshot.value(QStringLiteral("source_folder_renames")).isArray())
            state.insert(QStringLiteral("source_folder_renames"), SourceFolderNames::toJson(SourceFolderNames::fromJson(snapshot.value(QStringLiteral("source_folder_renames")).toArray())));
        state.insert(QStringLiteral("pending"), retained); state.insert(QStringLiteral("projects"), snapshot.value(QStringLiteral("projects"))); state.insert(QStringLiteral("downloaded"), true); return write(state);
    }
    // Replace a queued operation's local bookkeeping (same operation identity).
    QString updatePending(const QJsonObject &operation) const {
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The project catalogue is busy.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        auto pending = state.value(QStringLiteral("pending")).toArray();
        for (int i = 0; i < pending.size(); ++i) {
            if (pending[i].toObject().value(QStringLiteral("operation_id")) != operation.value(QStringLiteral("operation_id"))) continue;
            pending[i] = operation; state.insert(QStringLiteral("pending"), pending); return write(state);
        }
        return QStringLiteral("The project change is no longer pending.");
    }
    // Drop a queued operation Fedora refused for good (a reviewed move whose plan no longer holds).
    QString dropPending(const QString &operationId) const {
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The project catalogue is busy.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        QJsonArray retained;
        for (const auto &value : state.value(QStringLiteral("pending")).toArray())
            if (value.toObject().value(QStringLiteral("operation_id")).toString() != operationId) retained.append(value);
        state.insert(QStringLiteral("pending"), retained); return write(state);
    }
    // Fedora refused the import's folder name (409 source_folder_exists): take
    // the next longer hex (rule N) and rename this computer's copy. The next
    // Sync sends the import again under the new name.
    QString renameRefusedImportFolder(const QString &operationId, const QString &refused, QString *renamed = nullptr) const {
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The source import is busy.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        for (const auto &value : state.value(QStringLiteral("pending")).toArray()) {
            const auto op = normalizedOperation(value.toObject());
            if (op.value(QStringLiteral("operation_id")).toString() != operationId || op.value(QStringLiteral("action")).toString() != QLatin1String("import_folder")) continue;
            const auto current = sourceProject(op);
            auto taken = knownFolderNames(state, operationId);
            taken.insert(current.toLower());
            if (!refused.isEmpty()) taken.insert(refused.toLower());
            const auto name = SourceFolderNames::importFolderName(op.value(QStringLiteral("name")).toString(), operationId,
                [&taken](const QString &candidate) { return taken.contains(candidate); });
            if (name.isEmpty()) return QStringLiteral("No free name was found for the imported folder.");
            auto pending = state.value(QStringLiteral("pending")).toArray();
            for (int i = 0; i < pending.size(); ++i) {
                auto entry = pending[i].toObject();
                if (entry.value(QStringLiteral("operation_id")).toString() != operationId) continue;
                entry.insert(QStringLiteral("source_project"), current);
                entry.remove(QStringLiteral("upload_frozen"));
                pending[i] = entry;
            }
            state.insert(QStringLiteral("pending"), pending);
            const auto error = moveImportFolder(state, operationId, current, name);
            if (!error.isEmpty()) return error;
            if (renamed) *renamed = name;
            return write(state);
        }
        return QStringLiteral("The source import is no longer pending.");
    }
    // Fedora created the import under another name than this computer sent
    // (a server from before the naming change keeps docs-import-<32 hex>):
    // follow Fedora's name before installing its catalogue.
    QString adoptImportFolderName(const QJsonObject &snapshot, const QJsonObject &operation) const {
        if (operation.value(QStringLiteral("action")).toString() != QLatin1String("import_folder")) return {};
        const auto operationId = operation.value(QStringLiteral("operation_id")).toString();
        const auto sent = sourceProject(operation), legacy = legacyImportFolder(operationId);
        if (sent.isEmpty() || sent == legacy) return {};
        bool declaresSent = false, declaresLegacy = false;
        for (const auto &value : snapshot.value(QStringLiteral("projects")).toArray()) {
            const auto project = value.toObject();
            QStringList folders;
            for (const auto &folder : project.value(QStringLiteral("source_folders")).toArray()) folders.append(folder.toObject().value(QStringLiteral("project")).toString());
            for (const auto &folder : project.value(QStringLiteral("project_configuration")).toObject().value(QStringLiteral("source_folders")).toArray())
                folders.append(folder.toObject().value(QStringLiteral("path")).toString());
            declaresSent = declaresSent || folders.contains(sent);
            declaresLegacy = declaresLegacy || folders.contains(legacy);
        }
        if (declaresSent || !declaresLegacy) return {};
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The project catalogue is busy.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        const auto error = moveImportFolder(state, operationId, sent, legacy);
        if (!error.isEmpty()) return error;
        return write(state);
    }
    QString cancelFolderImport(const QString &operationId) const {
        QLockFile lock(m_path + QStringLiteral(".lock"));
        if (!lock.tryLock(1000)) return QStringLiteral("The project catalogue is busy.");
        auto state = read(); if (!m_error.isEmpty()) return m_error;
        const auto pending = state.value(QStringLiteral("pending")).toArray();
        QString projectId, prefix; QJsonArray retained; int removed = 0;
        for (const auto &value : pending) {
            auto op = value.toObject();
            if (op.value(QStringLiteral("operation_id")).toString() == operationId && op.value(QStringLiteral("action")).toString() == QLatin1String("import_folder")) {
                projectId = op.value(QStringLiteral("project_id")).toString();
                // The import's own folder, whichever way it is named (never a name prefix).
                prefix = sourceProject(op) + QLatin1Char('/');
                ++removed; continue;
            }
            if (!projectId.isEmpty() && op.value(QStringLiteral("project_id")).toString() == projectId) {
                if (op.value(QStringLiteral("action")).toString() == QLatin1String("remove_document") && op.value(QStringLiteral("document_id")).toString().startsWith(prefix)) { ++removed; continue; }
                if (op.value(QStringLiteral("action")).toString() == QLatin1String("order")) {
                    QJsonArray order;
                    for (const auto &id : op.value(QStringLiteral("documents")).toArray()) if (!id.toString().startsWith(prefix)) order.append(id);
                    op.insert(QStringLiteral("documents"), order);
                }
                op.insert(QStringLiteral("base_revision"), op.value(QStringLiteral("base_revision")).toInt() - removed);
            }
            retained.append(op);
        }
        if (!removed) return QStringLiteral("This import is no longer pending. Refresh the Library.");
        state.insert(QStringLiteral("pending"), retained); return write(state);
    }
};
}
