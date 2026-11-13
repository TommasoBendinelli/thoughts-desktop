// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Moving one source-linked document (.tex/.md) into another Project's source
// folder. Fedora plans and applies the move (thoughts_server
// source_document_moves): it moves the authored sources, rewrites links and
// both Project manifests, and keeps the document's identity and PDF filename,
// so annotations, review cards and history stay attached.
//
// Build inputs (figures, generated fragments) are this computer's: Fedora only
// knows the snapshots it publishes. They are copied into the destination
// before the operation is sent, and moved originals are removed only after
// Fedora accepted it; a rejected move removes the copies again.

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QScopeGuard>
#include <QString>
#include <QStringList>

namespace Thoughts::SourceDocumentMove
{

inline constexpr QLatin1StringView action{"move_source_document"};
inline constexpr QLatin1StringView planAction{"plan_move_source_document"};

// Keys only this computer uses; they never reach Fedora, so a retried
// operation keeps the digest Fedora's receipt recorded.
inline const QStringList &localKeys()
{
    static const QStringList keys{QStringLiteral("local_created"), QStringLiteral("desktop_transfers"), QStringLiteral("fedora_moves"),
                                  QStringLiteral("document_name"), QStringLiteral("destination_name"), QStringLiteral("source_folder"),
                                  QStringLiteral("destination_folder")};
    return keys;
}

inline QJsonObject serverPayload(QJsonObject operation)
{
    for (const QString &key : localKeys()) {
        operation.remove(key);
    }
    return operation;
}

inline QString fileSha256(const QString &path)
{
    QFile file(path);
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

inline bool safeRelative(const QString &path)
{
    const QString clean = QDir::cleanPath(path);
    return !path.isEmpty() && clean == path && !QDir::isAbsolutePath(path) && !path.startsWith(QLatin1String("../")) && path != QLatin1String("..");
}

// A project folder: one plain name directly in the Library, not a hidden one.
inline bool safeFolder(const QString &folder)
{
    return !folder.isEmpty() && folder != QLatin1String(".") && folder != QLatin1String("..") && !folder.startsWith(QLatin1Char('.')) && !folder.contains(QLatin1Char('/'))
        && !folder.contains(QLatin1Char('\\'));
}

// The file a Library-relative `path` of a move names, or an empty string if
// the move may not touch it. The plan comes from the server, and a move
// copies and deletes files on this computer, so every path is bound to one of
// the move's two project folders: by its text (inside `folder`, no ".."), and
// by where it really leads. The folder must be a real folder of the Library
// and everything below it lie inside the folder, with symbolic links
// resolved; what does not exist yet is created below a place that was checked.
inline QString boundPath(const QString &root, const QString &folder, const QString &path)
{
    if (!safeFolder(folder) || !safeRelative(path) || !path.startsWith(folder + QLatin1Char('/'))) {
        return {};
    }
    const QString library = QFileInfo(root).canonicalFilePath();
    if (library.isEmpty()) {
        return {};
    }
    const auto inside = [](const QString &canonical, const QString &directory) {
        return canonical == directory || canonical.startsWith(directory + QLatin1Char('/'));
    };
    QString allowed = library;
    QString current = root;
    const QStringList parts = path.split(QLatin1Char('/'));
    for (qsizetype index = 0; index < parts.size(); ++index) {
        current += QLatin1Char('/') + parts.at(index);
        const QFileInfo info(current);
        if (!info.exists() && !info.isSymLink()) {
            break;
        }
        const QString canonical = info.canonicalFilePath();
        // Empty: a link that leads nowhere, which a write would follow. The
        // project folder itself is a real folder of the Library, not a link.
        if (canonical.isEmpty() || !inside(canonical, allowed) || (index == 0 && (canonical == library || info.isSymLink()))) {
            return {};
        }
        if (index == 0) {
            allowed = canonical;
        }
    }
    return QDir(root).filePath(path);
}

// The two project folders of a queued move: recorded when it was reviewed,
// else (a move queued by an earlier version) the folders its paths name.
struct Folders {
    QString source;
    QString destination;
};

inline Folders folders(const QJsonObject &operation)
{
    Folders bound{operation.value(QStringLiteral("source_folder")).toString(), operation.value(QStringLiteral("destination_folder")).toString()};
    if (bound.source.isEmpty()) {
        bound.source = operation.value(QStringLiteral("fedora_moves")).toArray().first().toString().section(QLatin1Char('/'), 0, 0);
    }
    if (bound.destination.isEmpty()) {
        bound.destination = operation.value(QStringLiteral("desktop_transfers")).toArray().first().toObject().value(QStringLiteral("destination")).toString().section(QLatin1Char('/'), 0, 0);
    }
    return bound;
}

// Whether a reviewed plan stays within the two projects the reader chose:
// its source folder is the document's, its destination folder is one of the
// destination project's or a plain name that is no other project's folder
// (`foreignFolders`; the server may give a project its first folder), and
// every file it names lies in those folders. *error says what not.
inline bool planBound(const QJsonObject &plan, const QString &documentFolder, const QStringList &destinationFolders, const QStringList &foreignFolders, QString *error = nullptr)
{
    const auto fail = [error](const QString &reason) {
        if (error) {
            *error = reason;
        }
        return false;
    };
    const QString origin = plan.value(QStringLiteral("source")).toObject().value(QStringLiteral("folder")).toString();
    const QString target = plan.value(QStringLiteral("destination")).toObject().value(QStringLiteral("folder")).toString();
    if (!safeFolder(origin) || (!documentFolder.isEmpty() && origin != documentFolder)) {
        return fail(QStringLiteral("The plan moves files of another folder than the document's (%1).").arg(origin.left(120)));
    }
    if (!safeFolder(target) || (!destinationFolders.contains(target) && foreignFolders.contains(target, Qt::CaseInsensitive))) {
        return fail(QStringLiteral("The plan moves files into another folder than the chosen project's (%1).").arg(target.left(120)));
    }
    QStringList relative{plan.value(QStringLiteral("source")).toObject().value(QStringLiteral("path")).toString(),
                         plan.value(QStringLiteral("destination")).toObject().value(QStringLiteral("path")).toString()};
    for (const QJsonValue &value : plan.value(QStringLiteral("files")).toArray()) {
        relative << value.toObject().value(QStringLiteral("source")).toString() << value.toObject().value(QStringLiteral("destination")).toString();
    }
    for (const QString &path : std::as_const(relative)) {
        if (!safeRelative(path)) {
            return fail(QStringLiteral("The plan names a file outside the two project folders (%1).").arg(path.left(120)));
        }
    }
    return true;
}

inline QJsonObject planRequest(const QJsonObject &project, const QString &documentId, const QString &destinationId, const QString &destinationPath,
                               bool carryPreamble)
{
    QJsonObject request{{QStringLiteral("action"), QString(planAction)},
                        {QStringLiteral("project_id"), project.value(QStringLiteral("id"))},
                        {QStringLiteral("document_id"), documentId},
                        {QStringLiteral("destination_project_id"), destinationId},
                        {QStringLiteral("carry_preamble"), carryPreamble}};
    if (!destinationPath.isEmpty()) {
        request.insert(QStringLiteral("destination_path"), destinationPath);
    }
    return request;
}

// The queued operation for a reviewed plan.
inline QJsonObject operationValues(const QJsonObject &plan, bool carryPreamble)
{
    const QJsonObject source = plan.value(QStringLiteral("source")).toObject();
    const QJsonObject destination = plan.value(QStringLiteral("destination")).toObject();
    const QString origin = source.value(QStringLiteral("folder")).toString();
    const QString target = destination.value(QStringLiteral("folder")).toString();
    QJsonArray transfers;
    QJsonArray fedoraMoves;
    fedoraMoves.append(QString(origin + QLatin1Char('/') + source.value(QStringLiteral("path")).toString()));
    for (const QJsonValue &value : plan.value(QStringLiteral("files")).toArray()) {
        const QJsonObject file = value.toObject();
        const QString kind = file.value(QStringLiteral("action")).toString();
        const QString from = QString(origin + QLatin1Char('/') + file.value(QStringLiteral("source")).toString());
        if (file.value(QStringLiteral("transfer")).toString() == QLatin1String("desktop")) {
            if (kind == QLatin1String("move") || kind == QLatin1String("copy") || kind == QLatin1String("present-move")) {
                transfers.append(QJsonObject{{QStringLiteral("source"), from},
                                             {QStringLiteral("destination"), QString(target + QLatin1Char('/') + file.value(QStringLiteral("destination")).toString())},
                                             {QStringLiteral("action"), kind == QLatin1String("copy") ? QStringLiteral("copy") : QStringLiteral("move")},
                                             {QStringLiteral("sha256"), file.value(QStringLiteral("sha256"))}});
            }
        } else if (kind == QLatin1String("move") || kind == QLatin1String("present-move")) {
            fedoraMoves.append(from);
        }
    }
    return QJsonObject{{QStringLiteral("document_id"), plan.value(QStringLiteral("document_id"))},
                       {QStringLiteral("destination_project_id"), destination.value(QStringLiteral("project_id"))},
                       {QStringLiteral("destination_revision"), destination.value(QStringLiteral("revision")).toInt()},
                       {QStringLiteral("destination_path"), destination.value(QStringLiteral("path"))},
                       {QStringLiteral("carry_preamble"), carryPreamble},
                       {QStringLiteral("plan_digest"), plan.value(QStringLiteral("plan_digest"))},
                       {QStringLiteral("document_name"), plan.value(QStringLiteral("name"))},
                       {QStringLiteral("destination_name"), destination.value(QStringLiteral("project_name"))},
                       {QStringLiteral("source_folder"), origin},
                       {QStringLiteral("destination_folder"), target},
                       {QStringLiteral("desktop_transfers"), transfers},
                       {QStringLiteral("fedora_moves"), fedoraMoves}};
}

// Copies this computer's build inputs into the destination. Idempotent: a
// destination that already holds the reviewed bytes is left alone.
inline QString stageTransfers(const QString &root, QJsonObject &operation)
{
    QJsonArray created = operation.value(QStringLiteral("local_created")).toArray();
    const auto known = [&created](const QString &path) {
        for (const QJsonValue &value : std::as_const(created))
            if (value.toString() == path) return true;
        return false;
    };
    // Every copy made so far is recorded, also when a later one fails, so a
    // rollback removes exactly what this move created.
    const auto record = qScopeGuard([&operation, &created] { operation.insert(QStringLiteral("local_created"), created); });
    const Folders bound = folders(operation);
    for (const QJsonValue &value : operation.value(QStringLiteral("desktop_transfers")).toArray()) {
        const QJsonObject transfer = value.toObject();
        const QString from = transfer.value(QStringLiteral("source")).toString();
        const QString to = transfer.value(QStringLiteral("destination")).toString();
        const QString digest = transfer.value(QStringLiteral("sha256")).toString();
        // Read only inside the document's folder, written only inside the
        // destination project's, also where symbolic links lead.
        const QString source = boundPath(root, bound.source, from);
        const QString target = boundPath(root, bound.destination, to);
        if (source.isEmpty() || target.isEmpty()) {
            return QStringLiteral("The move names a file outside its two project folders: %1.").arg((source.isEmpty() ? from : to).left(200));
        }
        if (QFileInfo::exists(target)) {
            if (fileSha256(target) == digest) continue;
            return QStringLiteral("%1 already exists with different content.").arg(to);
        }
        if (fileSha256(source) != digest) {
            return QStringLiteral("%1 changed after the move was reviewed.").arg(from);
        }
        if (!QDir().mkpath(QFileInfo(target).absolutePath()) || boundPath(root, bound.destination, to).isEmpty()) {
            return QStringLiteral("The folder for %1 could not be created.").arg(to);
        }
        QFile input(source);
        QSaveFile output(target);
        if (!input.open(QIODevice::ReadOnly) || !output.open(QIODevice::WriteOnly) || output.write(input.readAll()) < 0 || !output.commit()) {
            return QStringLiteral("%1 could not be copied.").arg(from);
        }
        if (fileSha256(target) != digest) {
            QFile::remove(target);
            return QStringLiteral("The copy of %1 failed verification.").arg(from);
        }
        if (!known(to)) created.append(to);
    }
    return {};
}

// After Fedora accepted the move: remove moved originals that still hold the
// reviewed bytes. A changed original is kept.
inline QStringList finishTransfers(const QString &root, const QJsonObject &operation)
{
    QStringList kept;
    const Folders bound = folders(operation);
    for (const QJsonValue &value : operation.value(QStringLiteral("desktop_transfers")).toArray()) {
        const QJsonObject transfer = value.toObject();
        if (transfer.value(QStringLiteral("action")).toString() != QLatin1String("move")) continue;
        const QString from = transfer.value(QStringLiteral("source")).toString();
        // Nothing outside the document's folder is ever removed.
        const QString source = boundPath(root, bound.source, from);
        if (source.isEmpty() || !QFileInfo::exists(source)) continue;
        if (fileSha256(source) != transfer.value(QStringLiteral("sha256")).toString() || !QFile::remove(source)) kept.append(from);
    }
    return kept;
}

// After Fedora refused the move: remove only the copies this move created.
inline void rollbackTransfers(const QString &root, const QJsonObject &operation)
{
    QHash<QString, QString> digests;
    for (const QJsonValue &value : operation.value(QStringLiteral("desktop_transfers")).toArray()) {
        const QJsonObject transfer = value.toObject();
        digests.insert(transfer.value(QStringLiteral("destination")).toString(), transfer.value(QStringLiteral("sha256")).toString());
    }
    const Folders bound = folders(operation);
    for (const QJsonValue &value : operation.value(QStringLiteral("local_created")).toArray()) {
        const QString path = value.toString();
        const QString target = boundPath(root, bound.destination, path);
        if (!target.isEmpty() && digests.contains(path) && fileSha256(target) == digests.value(path)) QFile::remove(target);
    }
}

// Sources the move deletes at their old path that this computer edited but
// has not published: the move waits for them, or they would conflict with
// Fedora's deletion.
inline QStringList unpublishedEdits(const QString &root, const QString &sourcePublicationState, const QJsonObject &operation)
{
    QFile file(sourcePublicationState);
    QJsonObject bases;
    if (file.open(QIODevice::ReadOnly)) bases = QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("bases")).toObject();
    QStringList result;
    const Folders bound = folders(operation);
    for (const QJsonValue &value : operation.value(QStringLiteral("fedora_moves")).toArray()) {
        const QString path = value.toString();
        const QString local = boundPath(root, bound.source, path);
        if (local.isEmpty() || !QFileInfo::exists(local)) continue;
        const QString base = bases.value(path).toObject().value(QStringLiteral("sha256")).toString();
        if (base.isEmpty() || fileSha256(local) != base) result.append(path);
    }
    return result;
}

// Scripts of the origin folder that name a generated input (scripts/ is not
// synced, so only this computer can tell). Matches the input's folder-relative
// path, its directory, or its file name.
inline QStringList generatorScripts(const QString &root, const QString &folder, const QString &relative)
{
    QStringList result;
    const QString directory = QDir(root).filePath(folder + QStringLiteral("/scripts"));
    if (!QFileInfo(directory).isDir()) return result;
    const QString parent = QFileInfo(relative).path();
    const QString name = QFileInfo(relative).fileName();
    const QString stem = QFileInfo(relative).completeBaseName();
    QDirIterator it(directory, {QStringLiteral("*.py"), QStringLiteral("*.sh"), QStringLiteral("Makefile"), QStringLiteral("*.mk"), QStringLiteral("*.R"), QStringLiteral("*.jl")},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext() && result.size() < 5) {
        const QString path = it.next();
        QFile file(path);
        if (file.size() > 2 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) continue;
        const QString text = QString::fromUtf8(file.readAll());
        if (text.contains(relative) || (parent != QLatin1String(".") && text.contains(parent)) || text.contains(name) || (stem.size() > 3 && text.contains(QLatin1Char('"') + stem + QLatin1Char('"'))))
            result << QDir(QDir(root).filePath(folder)).relativeFilePath(path);
    }
    result.sort();
    return result;
}

inline QString escaped(const QString &value)
{
    return value.toHtmlEscaped();
}

// What the dialog shows for a plan.
inline QString describe(const QJsonObject &plan, const QHash<QString, QStringList> &generators = {})
{
    const QJsonObject source = plan.value(QStringLiteral("source")).toObject();
    const QJsonObject destination = plan.value(QStringLiteral("destination")).toObject();
    QString html;
    html += QStringLiteral("<p><b>%1</b> moves from <b>%2</b> (<code>%3/%4</code>) to <b>%5</b> (<code>%6/%7</code>). "
                           "It keeps its identity and PDF (<code>%8</code>), so its review cards, comments, proposals and history stay attached.</p>")
                .arg(escaped(plan.value(QStringLiteral("name")).toString()), escaped(source.value(QStringLiteral("project_name")).toString()),
                     escaped(source.value(QStringLiteral("folder")).toString()), escaped(source.value(QStringLiteral("path")).toString()),
                     escaped(destination.value(QStringLiteral("project_name")).toString()), escaped(destination.value(QStringLiteral("folder")).toString()),
                     escaped(destination.value(QStringLiteral("path")).toString()), escaped(plan.value(QStringLiteral("filename")).toString()));
    const QJsonObject markers = plan.value(QStringLiteral("markers")).toObject();
    QStringList attached;
    if (markers.value(QStringLiteral("review_cards")).toInt()) attached << QStringLiteral("%1 review card markers").arg(markers.value(QStringLiteral("review_cards")).toInt());
    if (markers.value(QStringLiteral("comments")).toInt()) attached << QStringLiteral("%1 comment markers").arg(markers.value(QStringLiteral("comments")).toInt());
    if (markers.value(QStringLiteral("proposals")).toInt()) attached << QStringLiteral("%1 pending proposals").arg(markers.value(QStringLiteral("proposals")).toInt());
    if (!attached.isEmpty()) html += QStringLiteral("<p>The source carries %1.</p>").arg(attached.join(QStringLiteral(", ")));
    if (destination.value(QStringLiteral("collision")).toBool()) {
        html += QStringLiteral("<p style='color:#b00020'><b>%1/%2 already exists.</b> Choose another file name, for example <code>%3</code>.</p>")
                    .arg(escaped(destination.value(QStringLiteral("folder")).toString()), escaped(destination.value(QStringLiteral("path")).toString()),
                         escaped(destination.value(QStringLiteral("suggested_path")).toString()));
    }
    QString files;
    bool generated = false;
    for (const QJsonValue &value : plan.value(QStringLiteral("files")).toArray()) {
        const QJsonObject file = value.toObject();
        const QString kind = file.value(QStringLiteral("action")).toString();
        const QString verb = kind == QLatin1String("move") ? QStringLiteral("Move")
            : kind == QLatin1String("copy") ? QStringLiteral("Copy")
            : kind == QLatin1String("present-move") ? QStringLiteral("Remove (identical copy already there)")
            : QStringLiteral("Keep (identical copy already there)");
        generated = generated || file.value(QStringLiteral("generated")).toBool();
        const QStringList scripts = generators.value(file.value(QStringLiteral("source")).toString());
        files += QStringLiteral("<li>%1 <code>%2</code>%3 — %4%5</li>")
                     .arg(verb, escaped(file.value(QStringLiteral("source")).toString()),
                          file.value(QStringLiteral("destination")) != file.value(QStringLiteral("source"))
                              ? QStringLiteral(" → <code>%1</code>").arg(escaped(file.value(QStringLiteral("destination")).toString()))
                              : QString(),
                          escaped(file.value(QStringLiteral("reason")).toString()),
                          scripts.isEmpty() ? QString() : QStringLiteral(" (generated by <code>%1</code>)").arg(escaped(scripts.join(QStringLiteral(", ")))));
    }
    html += files.isEmpty() ? QStringLiteral("<p>The document uses no other files of its folder.</p>")
                            : QStringLiteral("<p>Files it uses:</p><ul>%1</ul>").arg(files);
    if (generated) {
        html += QStringLiteral("<p>Generated inputs are copied as fixed files. Their generator scripts stay in %1; "
                               "regenerating there no longer updates this document.</p>")
                    .arg(escaped(source.value(QStringLiteral("project_name")).toString()));
    }
    const QJsonObject preamble = plan.value(QStringLiteral("preamble")).toObject();
    if (preamble.value(QStringLiteral("applies")).toBool()) {
        QStringList missing;
        for (const QJsonValue &value : preamble.value(QStringLiteral("missing")).toArray())
            missing << QStringLiteral("<code>%1</code>").arg(escaped(value.toObject().value(QStringLiteral("name")).toString()));
        for (const QJsonValue &value : preamble.value(QStringLiteral("packages")).toArray())
            if (value.toObject().value(QStringLiteral("required")).toBool())
                missing << QStringLiteral("package <code>%1</code>").arg(escaped(value.toObject().value(QStringLiteral("name")).toString()));
        if (missing.isEmpty()) {
            html += QStringLiteral("<p>The destination's preamble provides everything this document uses.</p>");
        } else if (preamble.value(QStringLiteral("declined")).toBool()) {
            html += QStringLiteral("<p style='color:#b00020'>The destination's preamble lacks %1. Without carrying them the document will not build.</p>")
                        .arg(missing.join(QStringLiteral(", ")));
        } else {
            html += QStringLiteral("<p>The destination's preamble lacks %1. They are added to the top of the document, "
                                   "each only where the destination does not define it.</p>").arg(missing.join(QStringLiteral(", ")));
        }
        QStringList different;
        for (const QJsonValue &value : preamble.value(QStringLiteral("different")).toArray())
            different << QStringLiteral("<code>%1</code>").arg(escaped(value.toObject().value(QStringLiteral("name")).toString()));
        if (!different.isEmpty())
            html += QStringLiteral("<p>Defined differently in the destination, so they may look different: %1.</p>").arg(different.join(QStringLiteral(", ")));
    }
    const QJsonObject bibliography = plan.value(QStringLiteral("bibliography")).toObject();
    QStringList keys;
    for (const QJsonValue &value : bibliography.value(QStringLiteral("missing")).toArray())
        keys << QStringLiteral("<code>%1</code>").arg(escaped(value.toObject().value(QStringLiteral("key")).toString()));
    if (!keys.isEmpty() && !bibliography.value(QStringLiteral("target")).toString().isEmpty())
        html += QStringLiteral("<p>Adds %1 to the destination's <code>%2</code>.</p>").arg(keys.join(QStringLiteral(", ")), escaped(bibliography.value(QStringLiteral("target")).toString()));
    const QJsonObject edits = plan.value(QStringLiteral("link_edits")).toObject();
    const QString moved = source.value(QStringLiteral("folder")).toString() + QLatin1Char('/') + source.value(QStringLiteral("path")).toString();
    QStringList others;
    int own = 0;
    for (auto it = edits.begin(); it != edits.end(); ++it) {
        if (it.key() == moved) own = it.value().toArray().size();
        else others << QStringLiteral("<code>%1</code>").arg(escaped(it.key()));
    }
    if (own) html += QStringLiteral("<p>%1 links in the document are rewritten so they keep working across Projects.</p>").arg(own);
    if (!others.isEmpty()) html += QStringLiteral("<p>Links to it are updated in %1.</p>").arg(others.join(QStringLiteral(", ")));
    for (const QJsonValue &value : plan.value(QStringLiteral("warnings")).toArray())
        html += QStringLiteral("<p style='color:#8a5300'>%1</p>").arg(escaped(value.toString()));
    html += QStringLiteral("<p>The move happens at the next Sync. The server then rebuilds the document in its new Project.</p>");
    return html;
}

}
