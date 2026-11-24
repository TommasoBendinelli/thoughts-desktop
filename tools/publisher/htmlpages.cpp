// SPDX-License-Identifier: GPL-2.0-or-later
#include "htmlpages.h"

#include "pyerror.h"
#include "pyio.h"
#include "sha256.h"
#include "sourceapi.h"
#include "sourceinventory.h"
#include "sourceowner.h"
#include "thoughtignore.h"

#include <QDateTime>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <optional>

namespace Thoughts::Publisher
{
namespace
{
using Py::Json;

const QString pageDirectory = QStringLiteral("tex/support/html");
const QString attachmentManifest = QStringLiteral("tex/support/thoughts_html_attachments.txt");
constexpr qint64 maximumPageBytes = 100000000;

// A connected docs-* folder or a Project's default folder (.thoughts/projects/<hex>/default).
bool isFolder(const QString &name)
{
    return SourceOwner::isOwner(name);
}

bool isPageName(const QString &name)
{
    static const QRegularExpression page(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]{0,122}\\.html$"));
    return page.match(name).hasMatch();
}

Json stringArray(QStringList values)
{
    std::sort(values.begin(), values.end(), Py::codePointLess);
    Json array = Json::array();
    for (const QString &value : std::as_const(values)) {
        array.append(Json(value));
    }
    return array;
}

Py::Path statePathOf(const Py::Path &publisherState)
{
    return publisherState.parent() / QStringLiteral("html-pages.json");
}

// path -> the version this computer and Fedora last agreed on.
QMap<QString, QString> readAgreed(const Py::Path &path)
{
    QMap<QString, QString> agreed;
    if (!Py::isFile(path)) {
        return agreed;
    }
    try {
        const Json value = Py::loadsBytes(Py::readBytes(path));
        for (const Json::Member &member : value.value(QStringLiteral("pages")).members()) {
            if (member.second.isString()) {
                agreed.insert(member.first, member.second.toString());
            }
        }
    } catch (const Py::Exception &) {
        // An unreadable record agrees on nothing: differences are kept as conflicts, never lost.
    }
    return agreed;
}

void writeAgreed(const Py::Path &path, const QMap<QString, QString> &agreed)
{
    Json pages = Json::object();
    for (auto it = agreed.cbegin(); it != agreed.cend(); ++it) {
        pages.insert(it.key(), Json(it.value()));
    }
    Py::makeDirectories(path.parent());
    Py::atomicWrite(path, Py::dumpsFile(Json::object({{QStringLiteral("schema_version"), Json(1)}, {QStringLiteral("pages"), pages}}), true));
}

struct LocalPage {
    QString sha256;
    QByteArray content;
};

// The folder's page files: regular files with a portable name, inside the folder, not ignored.
QMap<QString, LocalPage> localPages(const Py::Path &docsRoot, const QString &folder, QStringList *errors)
{
    QMap<QString, LocalPage> pages;
    const Py::Path project = docsRoot / folder;
    const Py::Path directory = project / pageDirectory;
    if (!Py::isDir(directory) || !isRealProjectPath(project, directory)) {
        return pages;
    }
    const ThoughtIgnore ignored(project);
    const std::optional<QList<Py::DirectoryEntry>> entries = Py::scanDirectory(directory);
    for (const Py::DirectoryEntry &entry : entries.value_or(QList<Py::DirectoryEntry>())) {
        const Py::Path file = directory / entry.name;
        if (entry.isDirectory || !isPageName(entry.name) || Py::isSymlink(file) || !Py::isFile(file)
            || ignored.excludes(pageDirectory + QLatin1Char('/') + entry.name)) {
            continue;
        }
        const QString path = folder + QLatin1Char('/') + pageDirectory + QLatin1Char('/') + entry.name;
        const Py::FileStatus status = Py::status(file);
        if (status.size < 1 || status.size > maximumPageBytes) {
            errors->append(path + QStringLiteral(": an HTML page must be between 1 byte and 100 MB"));
            continue;
        }
        QByteArray content;
        try {
            content = Py::readBytes(file);
        } catch (const Py::Exception &error) {
            errors->append(path + QStringLiteral(": ") + error.line());
            continue;
        }
        if (!Py::decodeUtf8(content)) {
            errors->append(path + QStringLiteral(": an HTML page must be UTF-8 text"));
            continue;
        }
        pages.insert(path, {sha256Hex(content), content});
    }
    return pages;
}

QString conflictStamp()
{
    return QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
}

struct Change {
    QString path;
    std::optional<QString> sha256;
    std::optional<QString> base;
};

Json optionalJson(const std::optional<QString> &value)
{
    return value ? Json(*value) : Json(nullptr);
}

QString apiErrorCode(const Py::Exception &error)
{
    try {
        return Py::loadsBytes(error.message.toUtf8()).value(QStringLiteral("error_code")).toString();
    } catch (const Py::Exception &) {
        return {};
    }
}
}

bool isHtmlPagePath(const QString &path)
{
    const QString owner = SourceOwner::of(path);
    if (owner.isEmpty()) {
        return false;
    }
    const QStringList parts = path.mid(owner.size() + 1).split(QLatin1Char('/'));
    return parts.size() == 4 && parts.mid(0, 3).join(QLatin1Char('/')) == pageDirectory && isPageName(parts.at(3));
}

QStringList htmlPageFolders(const QStringList &paths)
{
    QSet<QString> folders;
    for (const QString &path : paths) {
        const QString owner = SourceOwner::of(path);
        if (isFolder(owner)) {
            folders.insert(owner);
        }
    }
    QStringList sorted(folders.cbegin(), folders.cend());
    std::sort(sorted.begin(), sorted.end(), Py::codePointLess);
    return sorted;
}

QStringList listedHtmlPages(const Py::Path &docsRoot, const QString &folder)
{
    QStringList listed;
    const Py::Path manifest = docsRoot / folder / attachmentManifest;
    if (!isFolder(folder) || !Py::isFile(manifest)) {
        return listed;
    }
    try {
        const Json value = Py::loadsBytes(Py::readBytes(manifest));
        for (const Json &entry : value.value(QStringLiteral("attachments")).items()) {
            const QString path = entry.value(QStringLiteral("path")).toString();
            if (path.startsWith(folder + QLatin1Char('/'))) {
                listed.append(path);
            }
        }
    } catch (const Py::Exception &) {
    }
    return listed;
}

Json HtmlPageSync::toJson() const
{
    return Json::object({{QStringLiteral("supported"), Json(supported)},
                         {QStringLiteral("installed"), stringArray(installed)},
                         {QStringLiteral("uploaded"), stringArray(uploaded)},
                         {QStringLiteral("removed"), stringArray(removed)},
                         {QStringLiteral("conflicts"), stringArray(conflicts)},
                         {QStringLiteral("errors"), stringArray(errors)}});
}

HtmlPageSync syncHtmlPages(SourceApi &api, const Py::Path &docsRoot, const Py::Path &statePath, const QStringList &folders, const QString &stamp)
{
    HtmlPageSync report;
    if (folders.isEmpty()) {
        return report;
    }
    // Fedora's pages of every folder: path -> object.
    QMap<QString, QString> remote;
    try {
        const Json listing = Py::loadsBytes(api.request("GET", QStringLiteral("/v1/html-pages")).body);
        for (const Json &page : listing.value(QStringLiteral("pages")).items()) {
            const QString path = page.value(QStringLiteral("path")).toString();
            const QString sha256 = page.value(QStringLiteral("sha256")).toString();
            if (isHtmlPagePath(path) && sha256.size() == 64) {
                remote.insert(path, sha256);
            }
        }
    } catch (const Py::Exception &error) {
        if (error.type == Py::apiErrorType && (error.status == 404 || error.status == 405)) {
            report.supported = false; // A server without html_pages_v1.
        } else {
            report.errors.append(QStringLiteral("HTML pages: ") + error.line());
        }
        return report;
    }
    const Py::Path agreedPath = statePathOf(statePath);
    QMap<QString, QString> agreed = readAgreed(agreedPath);
    const QMap<QString, QString> recorded = agreed;
    const QString conflictTime = stamp.isEmpty() ? conflictStamp() : stamp;
    for (const QString &folder : folders) {
        if (!isFolder(folder) || !Py::isDir(docsRoot / folder)) {
            continue;
        }
        const QMap<QString, LocalPage> local = localPages(docsRoot, folder, &report.errors);
        const bool directoryExists = Py::isDir(docsRoot / folder / pageDirectory);
        QSet<QString> paths;
        for (auto it = local.cbegin(); it != local.cend(); ++it) {
            paths.insert(it.key());
        }
        for (auto it = remote.cbegin(); it != remote.cend(); ++it) {
            if (it.key().startsWith(folder + QLatin1Char('/'))) {
                paths.insert(it.key());
            }
        }
        QStringList ordered(paths.cbegin(), paths.cend());
        std::sort(ordered.begin(), ordered.end(), Py::codePointLess);
        // A page gone on both sides is forgotten.
        for (const QString &known : agreed.keys()) {
            if (known.startsWith(folder + QLatin1Char('/')) && !paths.contains(known)) {
                agreed.remove(known);
            }
        }
        QList<Change> changes;
        for (const QString &path : std::as_const(ordered)) {
            const std::optional<QString> mine = local.contains(path) ? std::optional<QString>(local.value(path).sha256) : std::nullopt;
            const std::optional<QString> theirs = remote.contains(path) ? std::optional<QString>(remote.value(path)) : std::nullopt;
            const std::optional<QString> both = agreed.contains(path) ? std::optional<QString>(agreed.value(path)) : std::nullopt;
            if (mine == theirs) {
                if (mine) {
                    agreed.insert(path, *mine);
                } else {
                    agreed.remove(path);
                }
                continue;
            }
            const auto install = [&](bool keepLocal) {
                try {
                    // The page is written inside the folder itself: never through a
                    // symbolic link (the folder, tex, support, html or the page's own
                    // name), which could lead anywhere on this computer.
                    if (!isRealProjectPath(docsRoot / folder, docsRoot / path)) {
                        report.errors.append(path + QStringLiteral(": not installed, because its place in the folder is reached through a symbolic link"));
                        return;
                    }
                    const QByteArray content = api.download(*theirs);
                    if (keepLocal) {
                        const Py::Path kept = docsRoot / QStringLiteral(".thoughts/html-page-conflicts") / conflictTime / path;
                        Py::makeDirectories(kept.parent());
                        Py::atomicWrite(kept, local.value(path).content);
                        report.conflicts.append(path);
                    }
                    const Py::Path file = docsRoot / path;
                    Py::makeDirectories(file.parent());
                    Py::atomicWrite(file, content);
                    agreed.insert(path, *theirs);
                    report.installed.append(path);
                } catch (const Py::Exception &error) {
                    report.errors.append(path + QStringLiteral(": ") + error.line());
                }
            };
            if (!mine) {
                if (both == theirs && directoryExists) {
                    changes.append({path, std::nullopt, theirs}); // Deleted here.
                } else {
                    install(false);
                }
            } else if (!theirs) {
                changes.append({path, mine, std::nullopt}); // New here (or re-adds one Fedora lost).
            } else if (both == mine) {
                install(false); // Changed on Fedora.
            } else if (both == theirs) {
                changes.append({path, mine, theirs}); // Changed here.
            } else {
                install(true); // Both changed: Fedora's page wins, the local one is kept.
            }
        }
        if (changes.isEmpty()) {
            continue;
        }
        Json list = Json::array();
        try {
            for (const Change &change : std::as_const(changes)) {
                if (change.sha256) {
                    api.uploadObject(local.value(change.path).content, QStringLiteral("text/html; charset=utf-8"));
                }
                list.append(Json::object({{QStringLiteral("path"), Json(change.path)},
                                          {QStringLiteral("sha256"), optionalJson(change.sha256)},
                                          {QStringLiteral("base_sha256"), optionalJson(change.base)}}));
            }
            api.request("POST", QStringLiteral("/v1/html-pages"),
                        Json::object({{QStringLiteral("operation_id"), Json(QUuid::createUuid().toString(QUuid::WithoutBraces))},
                                      {QStringLiteral("folder"), Json(folder)},
                                      {QStringLiteral("changes"), list}}));
        } catch (const Py::Exception &error) {
            const QString code = apiErrorCode(error);
            report.errors.append(folder + QStringLiteral(": ") + (code.isEmpty() ? error.line() : code + QStringLiteral(": ") + error.line()));
            continue;
        }
        for (const Change &change : std::as_const(changes)) {
            if (change.sha256) {
                agreed.insert(change.path, *change.sha256);
                report.uploaded.append(change.path);
            } else {
                agreed.remove(change.path);
                report.removed.append(change.path);
            }
        }
    }
    try {
        if (agreed != recorded) {
            writeAgreed(agreedPath, agreed);
        }
    } catch (const Py::Exception &error) {
        report.errors.append(QStringLiteral("HTML pages: ") + error.line());
    }
    return report;
}

}
