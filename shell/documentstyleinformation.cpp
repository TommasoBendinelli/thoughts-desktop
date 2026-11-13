/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "documentstyleinformation.h"
#include "../part/sourceowner.h"
#include "../part/thoughtsplatform.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>

#include <optional>

namespace
{

constexpr qint64 maximumStyleBytes = 4 * 1024 * 1024;

bool validSha256(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{64}$"));
    return pattern.match(value).hasMatch();
}

bool validProject(const QString &project)
{
    static const QRegularExpression pattern(QStringLiteral("^docs-[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    return pattern.match(project).hasMatch();
}

bool pathInside(const QString &path, const QString &directory)
{
    const QString cleanPath = QDir::cleanPath(path);
    const QString cleanDirectory = QDir::cleanPath(directory);
    return cleanPath == cleanDirectory || cleanPath.startsWith(cleanDirectory + QLatin1Char('/'));
}

QByteArray readBoundedFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > maximumStyleBytes) {
        return {};
    }
    return file.readAll();
}

QString fileSha256(const QString &path)
{
    const QByteArray contents = readBoundedFile(path);
    if (contents.isEmpty() && QFileInfo(path).size() != 0) {
        return {};
    }
    return QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
}

QJsonObject readJsonObject(const QString &path, qint64 maximumBytes = 8 * 1024 * 1024)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 0 || file.size() > maximumBytes) {
        return {};
    }
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    return error.error == QJsonParseError::NoError && document.isObject() ? document.object() : QJsonObject();
}

struct PublishedResource {
    QString kind;
    QString path;
    QString sha256;
    QString objectSha256;
    qint64 bytes = -1;

    bool operator==(const PublishedResource &) const = default;
};

std::optional<PublishedResource> parseResource(const QJsonObject &resource, bool artifactShape)
{
    PublishedResource parsed;
    parsed.kind = resource.value(QStringLiteral("kind")).toString();
    parsed.path = QDir::cleanPath(resource.value(QStringLiteral("path")).toString());
    parsed.sha256 = resource.value(QStringLiteral("sha256")).toString();
    parsed.objectSha256 = resource.value(artifactShape ? QStringLiteral("object") : QStringLiteral("object_sha256")).toString();
    parsed.bytes = resource.value(artifactShape ? QStringLiteral("bytes") : QStringLiteral("size_bytes")).toInteger(-1);
    if ((parsed.kind != QLatin1String("project_style") && parsed.kind != QLatin1String("project_style_override")) || parsed.path.isEmpty() || QDir::isAbsolutePath(parsed.path) || parsed.path == QLatin1String("..") ||
        parsed.path.startsWith(QLatin1String("../")) || !validSha256(parsed.sha256) || parsed.objectSha256 != parsed.sha256 || parsed.bytes < 0 || parsed.bytes > maximumStyleBytes) {
        return std::nullopt;
    }
    return parsed;
}

QHash<QString, PublishedResource> parseResources(const QJsonArray &resources, bool artifactShape, bool *valid)
{
    if (valid) {
        *valid = false;
    }
    QHash<QString, PublishedResource> parsed;
    for (const QJsonValue &value : resources) {
        if (value.toObject().value(QStringLiteral("kind")).toString() == QLatin1String("html_attachment")) continue;
        const std::optional<PublishedResource> resource = parseResource(value.toObject(), artifactShape);
        if (!resource || parsed.contains(resource->kind)) {
            return {};
        }
        parsed.insert(resource->kind, *resource);
    }
    if (valid) {
        *valid = true;
    }
    return parsed;
}

QJsonObject activeLibraryEntry(const QString &docsRoot, const QString &pdfFilename, const QString &pdfSha256)
{
    const QJsonObject index = readJsonObject(QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-library-index.json")));
    for (const QJsonValue &value : index.value(QStringLiteral("pdfs")).toArray()) {
        const QJsonObject entry = value.toObject();
        if (entry.value(QStringLiteral("filename")).toString() == pdfFilename && entry.value(QStringLiteral("pdf_sha256")).toString() == pdfSha256) {
            return entry;
        }
    }
    return {};
}

QDir sourceBundleRoot(const QString &docsRoot, const QString &stateRoot)
{
    QString effectiveStateRoot = stateRoot;
    if (effectiveStateRoot.isEmpty()) {
        effectiveStateRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server"));
    }
    QDir root(QDir(effectiveStateRoot).filePath(QStringLiteral("source-bundle")));
    if (!QFileInfo::exists(root.filePath(QStringLiteral("documents")))) {
        root = QDir(QDir(docsRoot).filePath(QStringLiteral(".runtime/target/source-bundle")));
    }
    return root;
}

QHash<QString, PublishedResource> artifactResources(const QString &docsRoot, const QString &stateRoot, const QString &pdfFilename, const QString &pdfSha256, QDir *bundleRoot, bool *resourcesAvailable, bool *resourcesValid)
{
    if (resourcesAvailable) {
        *resourcesAvailable = false;
    }
    if (resourcesValid) {
        *resourcesValid = false;
    }
    const QDir root = sourceBundleRoot(docsRoot, stateRoot);
    if (bundleRoot) {
        *bundleRoot = root;
    }
    const QString activePointer = QDir(docsRoot).filePath(QStringLiteral("current_pdfs/.thoughts-source-artifacts/%1.json").arg(pdfFilename));
    const QJsonObject pointer = readJsonObject(QFileInfo::exists(activePointer) ? activePointer : root.filePath(QStringLiteral("documents/%1.json").arg(pdfFilename)));
    const QString artifactSha256 = pointer.value(QStringLiteral("artifact_sha256")).toString();
    if (pointer.value(QStringLiteral("filename")).toString() != pdfFilename || pointer.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256 || !validSha256(artifactSha256)) {
        return {};
    }
    const QString manifestPath = root.filePath(QStringLiteral("objects/%1").arg(artifactSha256));
    const QByteArray contents = readBoundedFile(manifestPath);
    if (QString::fromLatin1(QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex()) != artifactSha256) {
        return {};
    }
    const QJsonObject manifest = QJsonDocument::fromJson(contents).object();
    const QJsonObject pdf = manifest.value(QStringLiteral("pdf")).toObject();
    if (manifest.value(QStringLiteral("kind")).toString() != QLatin1String("thoughts-document-source-artifact") || manifest.value(QStringLiteral("filename")).toString() != pdfFilename ||
        pdf.value(QStringLiteral("filename")).toString() != pdfFilename || pdf.value(QStringLiteral("pdf_sha256")).toString() != pdfSha256) {
        return {};
    }
    if (resourcesAvailable) {
        *resourcesAvailable = manifest.value(QStringLiteral("resources")).isArray();
    }
    return parseResources(manifest.value(QStringLiteral("resources")).toArray(), true, resourcesValid);
}

QString acceptedFedoraSha256(const QString &stateRoot, const QString &path)
{
    QString effectiveStateRoot = stateRoot;
    if (effectiveStateRoot.isEmpty()) {
        effectiveStateRoot = QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server"));
    }
    const QJsonObject sourceState = readJsonObject(QDir(effectiveStateRoot).filePath(QStringLiteral("source-publication.json")));
    const QString sha256 = sourceState.value(QStringLiteral("bases")).toObject().value(path).toObject().value(QStringLiteral("sha256")).toString();
    return validSha256(sha256) ? sha256 : QString();
}

void populateStatus(Thoughts::DocumentStyleResourceStatus *status, const QString &docsRoot, const QString &stateRoot, const QDir &bundleRoot, const PublishedResource *published)
{
    const QDir root(docsRoot);
    status->absolutePath = root.absoluteFilePath(status->relativePath);
    const QFileInfo live(status->absolutePath);
    const QString canonicalRoot = QFileInfo(docsRoot).canonicalFilePath();
    const QString canonicalLive = live.exists() ? live.canonicalFilePath() : QFileInfo(live.absolutePath()).canonicalFilePath();
    status->liveAvailable = live.isFile() && !live.isSymLink() && !canonicalRoot.isEmpty() && !canonicalLive.isEmpty() && pathInside(canonicalLive, canonicalRoot);
    if (status->liveAvailable) {
        status->localSha256 = fileSha256(status->absolutePath);
    }

    if (published) {
        status->relativePath = published->path;
        status->absolutePath = root.absoluteFilePath(status->relativePath);
        status->publishedSha256 = published->sha256;
        status->publishedObjectSha256 = published->objectSha256;
        status->publishedSizeBytes = published->bytes;
        status->publishedIdentityAvailable = true;
        const QString frozen = bundleRoot.filePath(QStringLiteral("objects/%1").arg(published->objectSha256));
        const QFileInfo frozenInfo(frozen);
        if (frozenInfo.isFile() && frozenInfo.size() == published->bytes && fileSha256(frozen) == published->sha256) {
            status->frozenSnapshotPath = frozen;
            status->frozenSnapshotAvailable = true;
        }

        const QFileInfo updatedLive(status->absolutePath);
        const QString updatedCanonical = updatedLive.exists() ? updatedLive.canonicalFilePath() : QFileInfo(updatedLive.absolutePath()).canonicalFilePath();
        status->liveAvailable = updatedLive.isFile() && !updatedLive.isSymLink() && !canonicalRoot.isEmpty() && !updatedCanonical.isEmpty() && pathInside(updatedCanonical, canonicalRoot);
        status->localSha256 = status->liveAvailable ? fileSha256(status->absolutePath) : QString();
    }
    status->fedoraSha256 = acceptedFedoraSha256(stateRoot, status->relativePath);
    status->fedoraIdentityAvailable = validSha256(status->fedoraSha256);
}

}

namespace Thoughts
{

bool DocumentStyleResourceStatus::localMatchesPublished() const
{
    return liveAvailable && publishedIdentityAvailable && validSha256(localSha256) && localSha256 == publishedSha256;
}

bool DocumentStyleResourceStatus::localMatchesFedora() const
{
    return liveAvailable && fedoraIdentityAvailable && validSha256(localSha256) && localSha256 == fedoraSha256;
}

DocumentStyleInformation inspectDocumentStyleInformation(const QString &docsRoot, const QString &pdfFilename, const QString &pdfSha256, const QString &project, const QString &stateRoot)
{
    DocumentStyleInformation result;
    if (SourceOwner::isDefaultFolder(project)) {
        result.projectStyleApplicable = false;
        return result;
    }
    result.sharedStyle.kind = QStringLiteral("project_style");
    result.sharedStyle.relativePath = validProject(project) ? project + QStringLiteral("/tex/support/manual_style.sty") : QString();
    result.projectOverride.kind = QStringLiteral("project_style_override");
    if (validProject(project)) {
        result.projectOverride.relativePath = project + QStringLiteral("/tex/support/manual_style_override.sty");
    }

    QHash<QString, PublishedResource> libraryResources;
    const QJsonObject entry = activeLibraryEntry(docsRoot, pdfFilename, pdfSha256);
    const bool libraryResourcesAvailable = entry.value(QStringLiteral("source_resources")).isArray();
    bool libraryResourcesValid = false;
    if (libraryResourcesAvailable) {
        libraryResources = parseResources(entry.value(QStringLiteral("source_resources")).toArray(), false, &libraryResourcesValid);
    }
    QDir bundleRoot;
    bool artifactResourcesAvailable = false;
    bool artifactResourcesValid = false;
    const QHash<QString, PublishedResource> artifactResourceMap = artifactResources(docsRoot, stateRoot, pdfFilename, pdfSha256, &bundleRoot, &artifactResourcesAvailable, &artifactResourcesValid);
    result.publishedResourcesAvailable = libraryResourcesAvailable || artifactResourcesAvailable;
    QHash<QString, PublishedResource> resources;
    if ((libraryResourcesAvailable && !libraryResourcesValid) || (artifactResourcesAvailable && !artifactResourcesValid) || (libraryResourcesAvailable && artifactResourcesAvailable && libraryResources != artifactResourceMap)) {
        resources.clear();
    } else if (artifactResourcesAvailable) {
        resources = artifactResourceMap;
    } else if (libraryResourcesAvailable) {
        resources = libraryResources;
    }

    const PublishedResource shared = resources.value(QStringLiteral("project_style"));
    const QString expectedStyle = project + QStringLiteral("/tex/support/manual_style.sty");
    if (shared.kind == QLatin1String("project_style") && shared.path == expectedStyle) {
        result.sharedStyle.kind = shared.kind;
        result.sharedStyle.relativePath = expectedStyle;
    }
    const PublishedResource overrideResource = resources.value(QStringLiteral("project_style_override"));
    const PublishedResource *sharedPointer = (shared.kind == QLatin1String("project_style") && shared.path == expectedStyle) ? &shared : nullptr;
    const QString expectedOverride = validProject(project) ? project + QStringLiteral("/tex/support/manual_style_override.sty") : QString();
    const PublishedResource *overridePointer = overrideResource.kind == QLatin1String("project_style_override") && overrideResource.path == expectedOverride ? &overrideResource : nullptr;
    populateStatus(&result.sharedStyle, docsRoot, stateRoot, bundleRoot, sharedPointer);
    if (!result.projectOverride.relativePath.isEmpty()) {
        populateStatus(&result.projectOverride, docsRoot, stateRoot, bundleRoot, overridePointer);
    }
    return result;
}

bool readFrozenDocumentStyleSnapshot(const DocumentStyleResourceStatus &resource, QByteArray *contents, QString *error)
{
    if (contents) {
        contents->clear();
    }
    if (error) {
        error->clear();
    }
    if (!resource.frozenSnapshotAvailable || !resource.publishedIdentityAvailable || resource.frozenSnapshotPath.isEmpty() || !validSha256(resource.publishedSha256) || resource.publishedSizeBytes < 0 ||
        resource.publishedSizeBytes > maximumStyleBytes) {
        if (error) {
            *error = QStringLiteral("The verified published style snapshot is unavailable.");
        }
        return false;
    }
    QFile file(resource.frozenSnapshotPath);
    if (!file.open(QIODevice::ReadOnly) || file.size() != resource.publishedSizeBytes) {
        if (error) {
            *error = QStringLiteral("The published style snapshot could not be opened.");
        }
        return false;
    }
    const QByteArray value = file.readAll();
    if (QString::fromLatin1(QCryptographicHash::hash(value, QCryptographicHash::Sha256).toHex()) != resource.publishedSha256) {
        if (error) {
            *error = QStringLiteral("The published style snapshot failed SHA-256 verification.");
        }
        return false;
    }
    if (contents) {
        *contents = value;
    }
    return true;
}

QByteArray projectStyleOverrideTemplate(const QString &project)
{
    if (!validProject(project)) {
        return {};
    }
    const QByteArray date = QDate::currentDate().toString(QStringLiteral("yyyy/MM/dd")).toLatin1();
    return QByteArrayLiteral("\\NeedsTeXFormat{LaTeX2e}\n") + QByteArrayLiteral("\\ProvidesPackage{manual_style_override}[") + date + QByteArrayLiteral(" Project-specific manual style additions]\n\n") +
        QByteArrayLiteral("% Add only project-specific packages and conventions here.\n") + QByteArrayLiteral("% Project document conventions belong in manual_style.sty.\n");
}

bool createProjectStyleOverride(const QString &docsRoot, const QString &project, QString *absolutePath, QString *error)
{
    if (absolutePath) {
        absolutePath->clear();
    }
    if (error) {
        error->clear();
    }
    const QFileInfo rootInfo(docsRoot);
    const QString canonicalRoot = rootInfo.canonicalFilePath();
    const QString projectPath = QDir(docsRoot).filePath(project);
    const QFileInfo projectInfo(projectPath);
    const QString canonicalProject = projectInfo.canonicalFilePath();
    if (!validProject(project) || !rootInfo.isDir() || canonicalRoot.isEmpty() || !projectInfo.isDir() || projectInfo.isSymLink() || canonicalProject.isEmpty() || !pathInside(canonicalProject, canonicalRoot)) {
        if (error) {
            *error = QStringLiteral("The document project is not a safe local docs-* directory.");
        }
        return false;
    }

    const QString supportPath = QDir(projectPath).filePath(QStringLiteral("tex/support"));
    if (!QDir().mkpath(supportPath)) {
        if (error) {
            *error = QStringLiteral("The project style directory could not be created.");
        }
        return false;
    }
    const QFileInfo supportInfo(supportPath);
    const QString canonicalSupport = supportInfo.canonicalFilePath();
    if (!supportInfo.isDir() || supportInfo.isSymLink() || canonicalSupport.isEmpty() || !pathInside(canonicalSupport, canonicalProject)) {
        if (error) {
            *error = QStringLiteral("The project style directory resolves outside the document project.");
        }
        return false;
    }

    const QString destination = QDir(supportPath).filePath(QStringLiteral("manual_style_override.sty"));
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        if (error) {
            *error = QFileInfo::exists(destination) ? QStringLiteral("This project already has a style override.") : QStringLiteral("The project style override could not be created: %1").arg(file.errorString());
        }
        return false;
    }
    const QByteArray contents = projectStyleOverrideTemplate(project);
    const bool written = !contents.isEmpty() && file.write(contents) == contents.size() && file.flush();
    file.close();
    if (!written) {
        QFile::remove(destination);
        if (error) {
            *error = QStringLiteral("The project style override could not be written completely.");
        }
        return false;
    }
    QFile::setPermissions(destination, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
    if (absolutePath) {
        *absolutePath = destination;
    }
    return true;
}

}
