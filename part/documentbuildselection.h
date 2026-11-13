// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QDirIterator>
#include <QMap>
#include <algorithm>
#include <QFile>
#include <QSet>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QCryptographicHash>
#include <QUuid>
#include "markdownfolderimport.h"
#include "documentidentity.h"
#include "projectconfiguration.h"
#include "sourceowner.h"
#include "thoughtignore.h"

namespace Thoughts {
// Selection is source configuration, synchronized with the other build inputs.
class DocumentBuildSelection {
public:
    static QString managedPdfSource(const QString &root, const QString &owner, const QString &filename) {
        const auto configuration = ProjectConfiguration::find(root, owner);
        const auto projectKey = configuration.value(QStringLiteral("id")).toString();
        if (projectKey.isEmpty()) return {};
        const auto folder = ProjectConfiguration::defaultFolderPath(configuration);
        return QStringLiteral("../") + (folder.isEmpty() ? DefaultFolderLocation::ofProject(root, projectKey) : folder) + QLatin1Char('/') + filename;
    }
    static QJsonObject uniformStorage(QJsonObject manifest) {
        manifest.insert(QStringLiteral("schema_version"), 4);
        auto rendering = manifest.value(QStringLiteral("rendering")).toObject();
        QJsonArray documents;
        QSet<QString> identifiers;
        for (const auto &value : manifest.value(QStringLiteral("documents")).toArray()) {
            auto entry = value.toObject();
            const auto id = entry.value(QStringLiteral("document_id")).toString();
            identifiers.insert(id);
            auto options = rendering.value(id).toObject();
            for (const auto &key : {QStringLiteral("output"), QStringLiteral("stylesheet"), QStringLiteral("working_directory"), QStringLiteral("sha256")})
                if (entry.contains(key)) options.insert(key, entry.value(key));
            if (!options.isEmpty()) rendering.insert(id, options);
            documents.append(QJsonObject{{QStringLiteral("document_id"), id}, {QStringLiteral("source"), entry.value(QStringLiteral("source"))},
                {QStringLiteral("format"), entry.value(QStringLiteral("format"))}, {QStringLiteral("included"), entry.value(QStringLiteral("included"))}});
        }
        for (const auto &key : rendering.keys()) if (!identifiers.contains(key)) rendering.remove(key);
        manifest.insert(QStringLiteral("documents"), documents);
        if (rendering.isEmpty()) manifest.remove(QStringLiteral("rendering"));
        else manifest.insert(QStringLiteral("rendering"), rendering);
        return manifest;
    }
    static QJsonObject readUniform(QJsonObject manifest, const QString &owner, QString *error) {
        const auto fail = [&]() { if (error) *error = QObject::tr("Invalid uniform document manifest."); return QJsonObject{}; };
        for (const auto &key : manifest.keys())
            if (key != QLatin1String("schema_version") && key != QLatin1String("document_id") && key != QLatin1String("documents") && key != QLatin1String("rendering")) return fail();
        if (manifest.contains(QStringLiteral("rendering")) && !manifest.value(QStringLiteral("rendering")).isObject()) return fail();
        const auto aggregate = QUuid(manifest.value(QStringLiteral("document_id")).toString());
        // A default folder's aggregate identity is derived from its Project (version 5).
        const bool aggregateVersion = aggregate.version() == QUuid::Random || (SourceOwner::isDefaultFolder(owner) && aggregate.version() == QUuid::Sha1);
        if (aggregate.isNull() || !aggregateVersion || !manifest.value(QStringLiteral("documents")).isArray()) return fail();
        QSet<QString> ids{aggregate.toString(QUuid::WithoutBraces)}, paths;
        QJsonArray documents;
        const auto rendering = manifest.value(QStringLiteral("rendering")).toObject();
        for (const auto &value : manifest.value(QStringLiteral("documents")).toArray()) {
            auto entry = value.toObject();
            const auto id = entry.value(QStringLiteral("document_id")).toString();
            const auto uuid = QUuid(id);
            const auto format = entry.value(QStringLiteral("format")).toString();
            const auto source = entry.value(QStringLiteral("source")).toString();
            if (entry.size() != 4 || !entry.contains(QStringLiteral("document_id")) || !entry.contains(QStringLiteral("source"))
                || !entry.contains(QStringLiteral("format")) || !entry.value(QStringLiteral("included")).isBool()
                || uuid.isNull() || uuid.version() != QUuid::Random || uuid.toString(QUuid::WithoutBraces) != id || ids.contains(id)
                || source.isEmpty() || QDir::isAbsolutePath(source) || source.contains(QLatin1Char('\\')) || QDir::cleanPath(source) != source) return fail();
            ids.insert(id);
            QString alias;
            if (format == QLatin1String("pdf")) {
                if (!QRegularExpression(QStringLiteral("^\\.\\./\\.thoughts/projects/[a-f0-9]{64}/(default|documents)/[A-Za-z0-9][A-Za-z0-9_.-]*\\.pdf$")).match(source).hasMatch()) return fail();
                const auto filename = QFileInfo(source).fileName();
                alias = QStringLiteral("third_party/") + filename;
                entry.insert(QStringLiteral("filename"), filename);
            } else {
                const auto suffix = QFileInfo(source).suffix().toLower();
                // A LaTeX entry point may live anywhere in its folder, as the publisher accepts.
                const bool validSource = format == QLatin1String("latex") ? suffix == QLatin1String("tex")
                    : format == QLatin1String("markdown") && (suffix == QLatin1String("md") || suffix == QLatin1String("markdown"));
                if (!validSource || source.split(QLatin1Char('/')).contains(QStringLiteral(".."))) return fail();
                alias = owner + QLatin1Char('/') + source;
            }
            if (paths.contains(source.toCaseFolded())) return fail();
            paths.insert(source.toCaseFolded());
            entry.insert(QStringLiteral("id"), alias);
            if (rendering.contains(id) && !rendering.value(id).isObject()) return fail();
            const auto options = rendering.value(id).toObject();
            if (format == QLatin1String("pdf") && (options.size() != 1 || !QRegularExpression(QStringLiteral("^[a-f0-9]{64}$")).match(options.value(QStringLiteral("sha256")).toString()).hasMatch())) return fail();
            if (format != QLatin1String("pdf") && options.contains(QStringLiteral("sha256"))) return fail();
            if (options.contains(QStringLiteral("working_directory"))) {
                const auto directory = options.value(QStringLiteral("working_directory")).toString();
                if (format != QLatin1String("latex") || directory.isEmpty() || QDir::isAbsolutePath(directory) || directory.contains(QLatin1Char('\\'))
                    || QDir::cleanPath(directory) != directory || directory.split(QLatin1Char('/')).contains(QStringLiteral(".."))) return fail();
            }
            for (auto it = options.begin(); it != options.end(); ++it) {
                if (it.key() != QLatin1String("output") && it.key() != QLatin1String("stylesheet") && it.key() != QLatin1String("working_directory") && it.key() != QLatin1String("sha256")) return fail();
                if (!it.value().isString() || it.value().toString().isEmpty()) return fail();
                entry.insert(it.key(), it.value());
            }
            documents.append(entry);
        }
        for (const auto &key : rendering.keys()) if (!ids.contains(key) || key == aggregate.toString(QUuid::WithoutBraces)) return fail();
        manifest.insert(QStringLiteral("documents"), documents);
        return manifest;
    }
    static QStringList owners(const QJsonObject &project) {
        QStringList result;
        const auto add = [&](const QString &name) {
            if (SourceOwner::isConnectedFolder(name) && !result.contains(name)) result.append(name);
        };
        add(project.value(QStringLiteral("source_project")).toString());
        for (const auto &value : project.value(QStringLiteral("source_folders")).toArray()) add(value.toObject().value(QStringLiteral("project")).toString());
        // The Project's default folder declares Markdown and LaTeX documents too.
        const auto defaultOwner = defaultFolder(project.value(QStringLiteral("project_configuration")).toObject());
        if (!defaultOwner.isEmpty() && !result.contains(defaultOwner)) result.append(defaultOwner);
        return result;
    }
    // The default folder's path in a schema 5 Project configuration.
    static QString defaultFolder(const QJsonObject &configuration) {
        for (const auto &value : configuration.value(QStringLiteral("source_folders")).toArray()) {
            const auto folder = value.toObject();
            if (folder.value(QStringLiteral("folder_id")) == configuration.value(QStringLiteral("default_folder_id"))
                && SourceOwner::isDefaultFolder(folder.value(QStringLiteral("path")).toString())) return folder.value(QStringLiteral("path")).toString();
        }
        return {};
    }
    // The docs root and owner of a folder's thoughts-project.json: a default
    // folder's owner spans four segments below the root, never its dirName().
    static QPair<QString, QString> folderLocation(const QString &manifestPath) {
        const auto directory = QFileInfo(manifestPath).absolutePath();
        const auto root = SourceOwner::defaultFolderRoot(directory);
        if (!root.isEmpty()) return {root, directory.mid(root.size() + 1)};
        return {QFileInfo(directory).absolutePath(), QFileInfo(directory).fileName()};
    }
    static QString optionsPath(const QString &root, const QString &owner) {
        return QDir(root).filePath(owner + QStringLiteral("/tex/support/thoughts_document_options.txt"));
    }
    static QJsonObject rawRead(const QString &path, QString *error = nullptr) {
        return folderManifest(path, true, error);
    }
    // A folder's per-folder view; imported PDFs join every folder's view
    // unless left out.
    static QJsonObject folderManifest(const QString &path, bool withPdfs, QString *error = nullptr) {
        if (!path.endsWith(QLatin1String("/thoughts-project.json"))) {
            if (error) *error = QObject::tr("Unsupported Project settings path.");
            return {};
        }
        const auto [root, owner] = folderLocation(path);
        const auto configuration = ProjectConfiguration::find(root, owner, nullptr, error);
        if (error && !error->isEmpty()) return {};
        if (configuration.isEmpty()) {
            if (error) *error = QObject::tr("The current Project manifest is missing.");
            return {};
        }
        auto projection = ProjectConfiguration::projection(configuration, owner);
        if (!withPdfs) {
            QJsonArray documents;
            auto rendering = projection.value(QStringLiteral("rendering")).toObject();
            for (const auto &value : projection.value(QStringLiteral("documents")).toArray()) {
                if (value.toObject().value(QStringLiteral("format")).toString() != QLatin1String("pdf")) documents.append(value);
                else rendering.remove(value.toObject().value(QStringLiteral("document_id")).toString());
            }
            projection.insert(QStringLiteral("documents"), documents);
            projection.insert(QStringLiteral("rendering"), rendering);
        }
        auto manifest = readUniform(projection, owner, error);
        // Titles live in the Project configuration's document names.
        const auto names = configuration.value(QStringLiteral("document_names")).toObject();
        auto documents = manifest.value(QStringLiteral("documents")).toArray();
        for (int i = 0; i < documents.size(); ++i) {
            auto entry = documents[i].toObject();
            const auto name = names.value(entry.value(QStringLiteral("document_id")).toString());
            if (name.isString()) { entry.insert(QStringLiteral("title"), name); documents[i] = entry; }
        }
        if (!manifest.isEmpty()) manifest.insert(QStringLiteral("documents"), documents);
        return manifest;
    }
    static QString manifestPath(const QString &root, const QString &owner) {
        return QDir(root).filePath(owner + QStringLiteral("/thoughts-project.json"));
    }
    static QString settingsProject(const QString &path) {
        for (const auto &name : {QStringLiteral("thoughts_document_options.txt"), QStringLiteral("thoughts_document_order.txt"), QStringLiteral("thoughts_project_attachments.txt")})
            if (path.endsWith(QStringLiteral("/tex/support/") + name)) return path.left(path.size() - name.size() - 13);
        return {};
    }
    static QJsonObject projection(const QJsonObject &manifest, const QString &path) {
        QJsonArray selected, excluded, order, attachments;
        QJsonObject titles;
        for (const auto &value : manifest.value(QStringLiteral("documents")).toArray()) {
            const auto d = value.toObject(); const auto id = d.value(QStringLiteral("id"));
            order.append(id);
            if (!d.value(QStringLiteral("included")).toBool()) excluded.append(id);
            else if (d.value(QStringLiteral("format")) == QLatin1String("latex") || d.value(QStringLiteral("format")) == QLatin1String("markdown")) selected.append(id);
            if (d.contains(QStringLiteral("title"))) titles.insert(id.toString(), d.value(QStringLiteral("title")));
            if (d.value(QStringLiteral("format")) == QLatin1String("pdf")) attachments.append(QJsonObject{
                {QStringLiteral("filename"), d.value(QStringLiteral("filename"))}, {QStringLiteral("sha256"), d.value(QStringLiteral("sha256"))},
                {QStringLiteral("title"), d.value(QStringLiteral("title")).toString(QFileInfo(d.value(QStringLiteral("filename")).toString()).completeBaseName())}, {QStringLiteral("placement"), QStringLiteral("append")},
                {QStringLiteral("source"), d.value(QStringLiteral("source"))}, {QStringLiteral("document_id"), d.value(QStringLiteral("document_id"))}});
        }
        if (path.endsWith(QLatin1String("thoughts_document_order.txt"))) return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("documents"), order}};
        if (path.endsWith(QLatin1String("thoughts_project_attachments.txt"))) return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("attachments"), attachments}};
        return {{QStringLiteral("schema_version"), 1}, {QStringLiteral("selected_documents"), selected}, {QStringLiteral("excluded_documents"), excluded}, {QStringLiteral("titles"), titles}};
    }
    static QJsonObject read(const QString &path, QString *error = nullptr) {
        const auto project = settingsProject(path);
        if (!project.isEmpty()) {
            const auto manifest = rawRead(QDir(project).filePath(QStringLiteral("thoughts-project.json")), error);
            if (manifest.isEmpty()) return {};
            return projection(manifest, path);
        }
        return rawRead(path, error);
    }
    static QJsonObject reordered(QJsonObject manifest, const QJsonArray &order) {
        QJsonArray sorted;
        const auto documents = manifest.value(QStringLiteral("documents")).toArray();
        for (const auto &id : order) {
            for (const auto &doc : documents) if (doc.toObject().value(QStringLiteral("id")) == id) { sorted.append(doc); break; }
        }
        for (const auto &doc : documents) if (!order.contains(doc.toObject().value(QStringLiteral("id")))) sorted.append(doc);
        manifest.insert(QStringLiteral("documents"), sorted);
        return manifest;
    }
    static QJsonObject effectiveManifest(const QString &root, const QString &owner, QString *error = nullptr) {
        return rawRead(manifestPath(root, owner), error);
    }
    static QJsonObject updateOptions(QJsonObject manifest, const QJsonObject &options) {
        auto docs = manifest.value(QStringLiteral("documents")).toArray();
        const auto excluded = options.value(QStringLiteral("excluded_documents")).toArray();
        const auto titles = options.value(QStringLiteral("titles")).toObject();
        for (int i = 0; i < docs.size(); ++i) {
            auto d = docs[i].toObject(); const auto id = d.value(QStringLiteral("id"));
            bool included = d.value(QStringLiteral("included")).toBool();
            if (options.contains(QStringLiteral("selected_documents")) && (d.value(QStringLiteral("format")) == QLatin1String("latex") || d.value(QStringLiteral("format")) == QLatin1String("markdown"))) included = options.value(QStringLiteral("selected_documents")).toArray().contains(id);
            if (excluded.contains(id)) included = false;
            d.insert(QStringLiteral("included"), included);
            if (titles.contains(id.toString())) d.insert(QStringLiteral("title"), titles.value(id.toString()));
            docs[i] = d;
        }
        manifest.insert(QStringLiteral("documents"), docs); return manifest;
    }
    static QString write(const QString &path, const QJsonObject &value) {
        const auto project = settingsProject(path);
        if (project.isEmpty()) return rawWrite(path, value);
        QString error;
        const auto [projectRoot, projectOwner] = folderLocation(QDir(project).filePath(QStringLiteral("thoughts-project.json")));
        auto manifest = effectiveManifest(projectRoot, projectOwner, &error);
        if (!error.isEmpty() || manifest.isEmpty()) return error.isEmpty() ? QObject::tr("Cannot read the project manifest.") : error;
        if (path.endsWith(QLatin1String("thoughts_document_options.txt"))) manifest = updateOptions(manifest, value);
        else if (path.endsWith(QLatin1String("thoughts_document_order.txt"))) manifest = reordered(manifest, value.value(QStringLiteral("documents")).toArray());
        else {
            QJsonArray docs; QMap<QString, QJsonObject> replacements;
            for (const auto &v : value.value(QStringLiteral("attachments")).toArray()) {
                auto d = v.toObject(); d.remove(QStringLiteral("placement"));
                const QString id = QStringLiteral("third_party/") + d.value(QStringLiteral("filename")).toString();
                if (manifest.value(QStringLiteral("schema_version")).toInt() == 4) {
                    const QString root = projectRoot;
                    const auto filename = d.value(QStringLiteral("filename")).toString();
                    const auto source = managedPdfSource(root, projectOwner, filename);
                    const auto destination = QDir::cleanPath(QDir(project).filePath(source));
                    if (!QFileInfo::exists(destination)) {
                        const QStringList candidates{QDir(root).filePath(QStringLiteral(".thoughts/project-imports/") + filename),
                            QDir(root).filePath(QStringLiteral("third_party/") + filename), QDir(root).filePath(QStringLiteral("current_pdfs/") + filename)};
                        for (const auto &candidate : candidates) {
                            QFile file(candidate);
                            if (!file.open(QIODevice::ReadOnly)) continue;
                            QCryptographicHash hash(QCryptographicHash::Sha256); hash.addData(&file);
                            if (QString::fromLatin1(hash.result().toHex()) != d.value(QStringLiteral("sha256")).toString()) continue;
                            if (!QDir().mkpath(QFileInfo(destination).absolutePath()) || !QFile::copy(candidate, destination)) return QObject::tr("Cannot save the Project's PDF input.");
                            break;
                        }
                    }
                    if (!QFileInfo::exists(destination)) return QObject::tr("The Project's PDF input is unavailable.");
                    QFile input(destination);
                    if (!input.open(QIODevice::ReadOnly)) return QObject::tr("Cannot read the Project's PDF input.");
                    QCryptographicHash digest(QCryptographicHash::Sha256); digest.addData(&input);
                    if (QString::fromLatin1(digest.result().toHex()) != d.value(QStringLiteral("sha256")).toString()) return QObject::tr("The Project's PDF input has changed.");
                    auto identity = DocumentIdentityStore(root).byFilename(filename).value(QStringLiteral("document_id")).toString();
                    if (identity.isEmpty()) identity = DocumentIdentityStore(root).registerImport(filename, d.value(QStringLiteral("sha256")).toString(), destination, &error);
                    if (identity.isEmpty()) return error;
                    d.insert(QStringLiteral("source"), source);
                    d.insert(QStringLiteral("document_id"), identity);
                } else { d.remove(QStringLiteral("source")); d.remove(QStringLiteral("document_id")); }
                d.insert(QStringLiteral("id"), id); d.insert(QStringLiteral("format"), QStringLiteral("pdf")); d.insert(QStringLiteral("included"), true); replacements.insert(id, d);
            }
            for (const auto &v : manifest.value(QStringLiteral("documents")).toArray()) {
                const auto d = v.toObject(); const auto id = d.value(QStringLiteral("id")).toString();
                if (replacements.contains(id)) { auto replacement = replacements.take(id); replacement.insert(QStringLiteral("included"), d.value(QStringLiteral("included"))); docs.append(replacement); }
                else if (d.value(QStringLiteral("format")) != QLatin1String("pdf")) docs.append(d);
            }
            for (const auto &d : replacements) docs.append(d);
            manifest.insert(QStringLiteral("documents"), docs);
        }
        return rawWrite(QDir(project).filePath(QStringLiteral("thoughts-project.json")), manifest);
    }
    static QJsonArray candidates(const QString &root, const QString &owner, QString *error = nullptr) {
        const QDir project(QDir(root).filePath(owner));
        if (!QFileInfo(project.path()).isDir() || QFileInfo(project.path()).isSymLink()) {
            if (error) *error = QObject::tr("Source folder is unavailable: %1. Sync to download it first.").arg(owner);
            return {};
        }
        QJsonArray result;
        const QString manifestPath = project.filePath(QStringLiteral("thoughts-project.json"));
        // A default folder offers its Markdown and LaTeX documents; its PDFs are declared on import.
        const auto manifest = folderManifest(manifestPath, !SourceOwner::isDefaultFolder(owner), error);
        const bool explicitManifest = manifest.value(QStringLiteral("schema_version")).toInt() >= 2 || QFileInfo::exists(manifestPath);
        if (manifest.isEmpty()) return {};
        QSet<QString> known;
        // A default folder's documents build from their own directory into
        // a Library PDF named after their full source id (spec Rules 2, 3).
        const bool inDefaultFolder = SourceOwner::isDefaultFolder(owner);
        const auto append = [&](QJsonObject entry) {
            const auto source = entry.value(QStringLiteral("source")).toString();
            if (known.contains(source)) return;
            const auto path = project.filePath(source);
            if (!QFileInfo(path).isFile() || QFileInfo(path).isSymLink() || !QFileInfo(path).canonicalFilePath().startsWith(project.canonicalPath() + QLatin1Char('/'))) return;
            known.insert(source);
            entry.insert(QStringLiteral("id"), entry.value(QStringLiteral("id")).toString(owner + QLatin1Char('/') + source));
            entry.insert(QStringLiteral("owner"), owner);
            result.append(entry);
        };
        for (const auto &value : manifest.value(QStringLiteral("documents")).toArray()) {
            auto entry = value.toObject(); entry.insert(QStringLiteral("default_selected"), entry.value(QStringLiteral("included")).toBool(true)); append(entry);
        }
        if (explicitManifest) {
            const ThoughtIgnore ignored(project.path());
            if (!ignored.error().isEmpty()) {
                if (error) *error = ignored.error();
                return {};
            }
            QStringList directories{project.path()};
            while (!directories.isEmpty()) {
                const QDir directory(directories.takeLast());
                for (const auto &file : directory.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name)) {
                    const auto relative = project.relativeFilePath(file.filePath());
                    if (file.isSymLink() || ignored.excludes(relative, file.isDir()) || (inDefaultFolder && file.fileName().startsWith(QLatin1Char('.')))) continue;
                    if (file.isDir()) { directories.append(file.filePath()); continue; }
                    const auto suffix = file.suffix().toLower();
                    if (suffix == QLatin1String("tex") && (inDefaultFolder || QFileInfo(relative).path() != QLatin1String("tex/source")) && MarkdownFolderImport::latexFolderPath(relative)) {
                        // An entry point anywhere else builds from its own directory.
                        QFile source(file.filePath());
                        if (!source.open(QIODevice::ReadOnly) || !MarkdownFolderImport::standaloneLatex(source.read(MarkdownFolderImport::DocumentLimit))) continue;
                        QJsonObject entry{{QStringLiteral("source"), relative}, {QStringLiteral("format"), QStringLiteral("latex")}, {QStringLiteral("default_selected"), false}};
                        if (QFileInfo(relative).path() != QLatin1String(".")) entry.insert(QStringLiteral("working_directory"), QFileInfo(relative).path());
                        if (inDefaultFolder) entry.insert(QStringLiteral("output"), SourceOwner::defaultFolderOutput(owner + QLatin1Char('/') + relative));
                        append(entry);
                        continue;
                    }
                    if (suffix != QLatin1String("md") && suffix != QLatin1String("markdown")) continue;
                    const auto name = relative.startsWith(QLatin1String("markdown/source/")) ? relative.mid(QStringLiteral("markdown/source/").size()) : QStringLiteral("folder:") + relative;
                    append({{QStringLiteral("source"), relative}, {QStringLiteral("format"), QStringLiteral("markdown")},
                        {QStringLiteral("output"), inDefaultFolder ? SourceOwner::defaultFolderOutput(owner + QLatin1Char('/') + relative)
                            : QString(QStringLiteral("document_") + QString::fromLatin1(QCryptographicHash::hash(name.toUtf8(), QCryptographicHash::Sha256).toHex().left(20)) + QStringLiteral(".pdf"))}});
                }
            }
        }
        if (!inDefaultFolder) {
            const QDir sources(project.filePath(QStringLiteral("tex/source")));
            bool repositoryLayout = sources.exists(QStringLiteral(".git"));
            for (const auto &value : manifest.value(QStringLiteral("documents")).toArray())
                repositoryLayout = repositoryLayout || value.toObject().value(QStringLiteral("working_directory")) == QLatin1String("tex/source");
            for (const auto &name : sources.entryList({QStringLiteral("*.tex")}, QDir::Files, QDir::Name)) {
                if (name.startsWith(QLatin1String("appendix_")) || name == QLatin1String("documentation_index.tex") || name == QLatin1String("docs_combined_compact.tex")) continue;
                if (repositoryLayout) {
                    QFile file(sources.filePath(name));
                    if (!file.open(QIODevice::ReadOnly) || !MarkdownFolderImport::standaloneLatex(file.read(MarkdownFolderImport::DocumentLimit))) continue;
                }
                QJsonObject entry{{QStringLiteral("source"), QString(QStringLiteral("tex/source/") + name)}, {QStringLiteral("format"), QStringLiteral("latex")}, {QStringLiteral("default_selected"), !explicitManifest}};
                if (repositoryLayout) entry.insert(QStringLiteral("working_directory"), QStringLiteral("tex/source"));
                append(entry);
            }
        }
        return result;
    }
    static bool selected(const QJsonObject &options, const QJsonObject &entry) {
        const auto id = entry.value(QStringLiteral("id"));
        return !options.value(QStringLiteral("excluded_documents")).toArray().contains(id)
            && (options.contains(QStringLiteral("selected_documents")) ? options.value(QStringLiteral("selected_documents")).toArray().contains(id) : entry.value(QStringLiteral("default_selected")).toBool());
    }
    static QString rawWrite(const QString &path, QJsonObject value) {
        if (value.value(QStringLiteral("schema_version")).toInt() == 5) return ProjectConfiguration::save(path, value);
        if (!path.endsWith(QLatin1String("/thoughts-project.json"))) return QObject::tr("Unsupported Project settings path.");
        const auto [root, owner] = folderLocation(path);
        QString configurationPath, error;
        const auto configuration = ProjectConfiguration::find(root, owner, &configurationPath, &error);
        if (!error.isEmpty()) return error;
        if (configuration.isEmpty()) return QObject::tr("The current Project manifest is missing.");
        QJsonObject titles;
        for (const auto &raw : value.value(QStringLiteral("documents")).toArray()) {
            const auto entry = raw.toObject();
            if (entry.value(QStringLiteral("title")).isString() && !entry.value(QStringLiteral("document_id")).toString().isEmpty())
                titles.insert(entry.value(QStringLiteral("document_id")).toString(), entry.value(QStringLiteral("title")));
        }
        auto incoming = uniformStorage(value);
        incoming.insert(QStringLiteral("document_names"), titles);
        return ProjectConfiguration::replaceProjection(configurationPath, configuration, owner, incoming);
    }
    static QString prepareSelection(const QString &root, const QString &owner, const QJsonArray &entries, const QJsonArray &selectedIds, QMap<QString, QJsonObject> &changes) {
        QString error;
        auto options = read(optionsPath(root, owner), &error);
        if (!error.isEmpty()) return error;
        if (selectedIds.isEmpty()) return QObject::tr("Keep at least one document selected in %1.").arg(owner);
        const auto available = candidates(root, owner, &error);
        if (!error.isEmpty()) return error;
        QSet<QString> known;
        for (const auto &entry : available) known.insert(entry.toObject().value(QStringLiteral("id")).toString());
        for (const auto &id : selectedIds) if (!known.contains(id.toString())) return QObject::tr("The source list changed. Reopen the checklist.");
        auto manifest = effectiveManifest(root, owner, &error);
        if (!error.isEmpty()) return error;
        auto documents = manifest.value(QStringLiteral("documents")).toArray();
        QSet<QString> registered;
        for (int i = 0; i < documents.size(); ++i) {
            auto entry = documents[i].toObject();
            if (entry.contains(QStringLiteral("source"))) entry.insert(QStringLiteral("included"), selectedIds.contains(entry.value(QStringLiteral("id"))));
            registered.insert(entry.value(QStringLiteral("id")).toString());
            documents[i] = entry;
        }
        for (auto value : entries) {
            auto entry = value.toObject();
            if (registered.contains(entry.value(QStringLiteral("id")).toString())) continue;
            entry.remove(QStringLiteral("owner")); entry.remove(QStringLiteral("default_selected"));
            // A newly declared source needs its permanent identity before it reaches the Project configuration.
            if (entry.value(QStringLiteral("document_id")).toString().isEmpty())
                entry.insert(QStringLiteral("document_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
            entry.insert(QStringLiteral("included"), selectedIds.contains(entry.value(QStringLiteral("id"))));
            documents.append(entry);
        }
        manifest.insert(QStringLiteral("documents"), documents);
        changes.insert(manifestPath(root, owner), manifest);
        return {};
    }
    static QString save(const QString &root, const QString &owner, const QJsonArray &entries, const QJsonArray &selectedIds) {
        QMap<QString, QJsonObject> changes;
        const auto error = prepareSelection(root, owner, entries, selectedIds, changes);
        if (!error.isEmpty()) return error;
        return write(manifestPath(root, owner), changes.value(manifestPath(root, owner)));
    }
    static QJsonObject project(const QString &root, QJsonObject project) {
        auto documents = project.value(QStringLiteral("documents")).toArray();
        QSet<QString> pending;
        for (const auto &v : project.value(QStringLiteral("source_folders")).toArray()) if (v.toObject().value(QStringLiteral("pending_import")).toBool()) pending.insert(v.toObject().value(QStringLiteral("project")).toString());
        for (const auto &owner : owners(project)) {
            if (pending.contains(owner)) continue;
            const auto options = read(optionsPath(root, owner));
            if (!options.contains(QStringLiteral("selected_documents"))) continue;
            const auto entries = candidates(root, owner);
            QSet<QString> wanted;
            for (const auto &entry : entries) if (selected(options, entry.toObject())) wanted.insert(entry.toObject().value(QStringLiteral("id")).toString());
            QSet<QString> shown;
            for (int i = documents.size() - 1; i >= 0; --i) {
                const auto doc = documents[i].toObject(); const auto id = doc.value(QStringLiteral("id")).toString();
                if (id.startsWith(owner + QLatin1Char('/')) && !doc.value(QStringLiteral("imported")).toBool() && !wanted.contains(id)) documents.removeAt(i);
                else shown.insert(id);
            }
            for (const auto &value : entries) {
                const auto entry = value.toObject(); const auto id = entry.value(QStringLiteral("id")).toString();
                if (!wanted.contains(id) || shown.contains(id)) continue;
                documents.append(QJsonObject{{QStringLiteral("id"), id}, {QStringLiteral("source_file"), QString(owner + QLatin1Char('/') + entry.value(QStringLiteral("source")).toString())},
                    {QStringLiteral("source_project"), owner}, {QStringLiteral("name"), entry.value(QStringLiteral("title")).toString(QFileInfo(entry.value(QStringLiteral("source")).toString()).completeBaseName())},
                    {QStringLiteral("start_page"), 0}, {QStringLiteral("size_bytes"), -1}, {QStringLiteral("pending_build"), true}});
            }
        }
        for (const auto &owner : owners(project)) {
            const auto order = read(QDir(root).filePath(owner + QStringLiteral("/tex/support/thoughts_document_order.txt"))).value(QStringLiteral("documents")).toArray();
            if (order.isEmpty()) continue;
            QJsonArray sorted;
            for (const auto &id : order) for (const auto &doc : documents) if (doc.toObject().value(QStringLiteral("id")) == id) sorted.append(doc);
            for (const auto &doc : documents) if (!order.contains(doc.toObject().value(QStringLiteral("id")))) sorted.append(doc);
            documents = sorted;
            break;
        }
        project.insert(QStringLiteral("documents"), documents);
        return project;
    }
};
}
