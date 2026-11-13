// SPDX-License-Identifier: GPL-2.0-or-later
#include "projectdocuments.h"

#include "pyerror.h"
#include "pystr.h"
#include "sourceowner.h"
#include "thoughtignore.h"

#include <QCryptographicHash>
#include <QSet>

#include <algorithm>

namespace Thoughts::Publisher
{
namespace
{
const QString layoutInvalid = QStringLiteral("project_layout_invalid: ");

// PurePosixPath(path).parent.as_posix()
QString posixParent(const QString &path)
{
    QStringList segments;
    for (const QString &segment : path.split(QLatin1Char('/'), Qt::SkipEmptyParts)) {
        if (segment != QLatin1String(".")) {
            segments.append(segment);
        }
    }
    const QString root = path.startsWith(QLatin1Char('/')) ? QStringLiteral("/") : QString();
    if (!segments.isEmpty()) {
        segments.removeLast();
    }
    const QString result = root + segments.join(QLatin1Char('/'));
    return result.isEmpty() ? QStringLiteral(".") : result;
}

// _valid_output(filename)
QString validOutput(const QString &filename)
{
    if (Py::isSafePdfFilename(filename)) {
        return filename;
    }
    QString base = filename;
    if (base.endsWith(QLatin1String(".pdf"))) {
        base.chop(4);
    }
    // re.sub(r"[^A-Za-z0-9_.-]+", "_", ...), then re.sub(r"_{2,}", "_", ...)
    QString replaced;
    bool inRun = false;
    for (const QChar character : base) {
        if (Py::isAsciiWordChar(character)) {
            replaced += character;
            inRun = false;
        } else if (!inRun) {
            replaced += QLatin1Char('_');
            inRun = true;
        }
    }
    QString collapsed;
    for (const QChar character : replaced) {
        if (character == QLatin1Char('_') && collapsed.endsWith(QLatin1Char('_'))) {
            continue;
        }
        collapsed += character;
    }
    const QString stem = Py::strip(collapsed, QStringLiteral("_.-"));
    return (stem.isEmpty() ? QStringLiteral("document") : stem) + QStringLiteral(".pdf");
}

QString withoutDocsPrefix(const QString &owner)
{
    return owner.startsWith(QLatin1String("docs-")) ? owner.mid(5) : owner;
}

// safe_project_file(project, value, "", require_exists=...)
Py::Path safeProjectFile(const Py::Path &project, const QString &value, bool requireExists)
{
    for (const QChar character : value) {
        if (character.unicode() < 32) {
            Py::raiseRenderError(layoutInvalid + QStringLiteral("invalid document path"));
        }
    }
    bool normal = !value.startsWith(QLatin1Char('/')) && !value.contains(QLatin1Char('\\')) && !value.isEmpty();
    for (const QString &segment : value.split(QLatin1Char('/'))) {
        if (segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..")) {
            normal = false;
        }
    }
    if (!normal) {
        // PurePosixPath("") is ".", so an empty value fails as_posix() == value too.
        Py::raiseRenderError(layoutInvalid + QStringLiteral("invalid document path ") + Py::repr(value));
    }
    const Py::Path actual = project / value;
    if ((requireExists && !Py::isFile(actual)) || !Py::resolve(actual).isRelativeTo(Py::resolve(project))) {
        Py::raiseRenderError(layoutInvalid + QStringLiteral("document input unavailable: ") + value);
    }
    return actual;
}

bool isWrappedType(const QString &type)
{
    // except (KeyError, TypeError, ValueError, UnicodeDecodeError), with
    // ValueError's subclasses.
    static const QSet<QString> wrapped{QStringLiteral("KeyError"),           QStringLiteral("TypeError"),
                                       QStringLiteral("ValueError"),         QStringLiteral("UnicodeDecodeError"),
                                       QStringLiteral("UnicodeEncodeError"), Py::jsonDecodeErrorType};
    return wrapped.contains(type);
}
}

QString componentOutput(const QString &project, const QString &source)
{
    QString stem = Py::posixStem(source);
    if (posixParent(source) != QLatin1String("tex/source") || Py::lower(Py::posixSuffix(source)) != QLatin1String(".tex")) {
        stem += QLatin1Char('_') + QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
    }
    return validOutput(withoutDocsPrefix(project) + QLatin1Char('_') + stem + QStringLiteral(".pdf"));
}

QString documentOutput(const QString &owner, const Json &entry)
{
    const QString source = entry.value(QStringLiteral("source")).toString();
    if (entry.contains(QStringLiteral("output"))) {
        return entry.value(QStringLiteral("output")).toString();
    }
    if (SourceOwner::isDefaultFolder(owner)) {
        // Named after the full source id, so two Projects never collide (spec Rule 2).
        return SourceOwner::defaultFolderOutput(owner + QLatin1Char('/') + source);
    }
    if (entry.value(QStringLiteral("format")).toString() == QLatin1String("markdown") && source.startsWith(QLatin1String("markdown/source/"))) {
        return validOutput(withoutDocsPrefix(owner) + QLatin1Char('_') + Py::posixStem(source) + QStringLiteral(".pdf"));
    }
    return componentOutput(owner, source);
}

QString latexWorkingDirectory(const QString &owner, const QString &source, const Json &options)
{
    if (options.contains(QStringLiteral("working_directory"))) {
        return options.value(QStringLiteral("working_directory")).toString();
    }
    // A default folder's LaTeX document compiles from its own directory (spec Rule 3).
    return SourceOwner::isDefaultFolder(owner) ? posixParent(source) : QStringLiteral(".");
}

QList<ProjectDocument> projectDocuments(const Py::Path &project, bool allowMissingInputs)
{
    const auto [root, name] = folderOwner(project);
    const std::optional<Configuration> configuration = findConfiguration(root, name);
    if (!configuration) {
        Py::raiseValueError(QStringLiteral("Missing current Project manifest for ") + name);
    }
    const Json value = projection(configuration->value, name);
    const Py::Path resolvedProject = Py::resolve(project);
    if (!Py::resolve(project / QStringLiteral("thoughts-project.json")).isRelativeTo(resolvedProject)) {
        Py::raiseRenderError(layoutInvalid + QStringLiteral("project manifest escapes the project"));
    }
    QList<ProjectDocument> result;
    try {
        const Json rendering = value.value(QStringLiteral("rendering"));
        QSet<QString> seenSources;
        QSet<QString> seenIdentities;
        QSet<QString> seenOutputs;
        for (const Json &original : value.value(QStringLiteral("documents")).items()) {
            const QString format = original.value(QStringLiteral("format")).toString();
            if ((format != QLatin1String("latex") && format != QLatin1String("markdown")) || !original.value(QStringLiteral("included")).toBool()) {
                continue;
            }
            // uniform_manifest.runtime: build options join the entry.
            Json entry = original;
            const QString documentId = entry.value(QStringLiteral("document_id")).toString();
            for (const auto &option : rendering.value(documentId).members()) {
                entry.insert(option.first, option.second);
            }
            const QString source = entry.value(QStringLiteral("source")).toString();
            safeProjectFile(project, source, !allowMissingInputs);
            const QString identity = name + QLatin1Char('/') + source;
            const QString output = documentOutput(name, entry);
            bool validOutputName = Py::Path(output).name() == output && !output.contains(QLatin1Char('\\')) && output.endsWith(QLatin1String(".pdf"))
                && output != withoutDocsPrefix(name) + QStringLiteral("_combined.pdf");
            for (const QChar character : output) {
                if (character.unicode() < 32) {
                    validOutputName = false;
                }
            }
            if (!validOutputName) {
                Py::raiseValueError(QStringLiteral("invalid component output name"));
            }
            std::optional<QString> stylesheet;
            if (entry.contains(QStringLiteral("stylesheet"))) {
                stylesheet = entry.value(QStringLiteral("stylesheet")).toString();
                if (format != QLatin1String("markdown")) {
                    Py::raiseValueError(QStringLiteral("stylesheet is only supported for Markdown"));
                }
                if (safeProjectFile(project, *stylesheet, !allowMissingInputs).suffix() != QLatin1String(".css")) {
                    Py::raiseValueError(QStringLiteral("Markdown stylesheet must be CSS"));
                }
            }
            const QList<std::tuple<const char *, QString, QSet<QString> *>> keys{{"source", source, &seenSources},
                                                                                  {"identity", identity, &seenIdentities},
                                                                                  {"output", output, &seenOutputs}};
            for (const auto &[kind, key, seen] : keys) {
                const QString folded = Py::casefold(key);
                if (seen->contains(folded)) {
                    Py::raiseValueError(QStringLiteral("duplicate document %1: %2").arg(QLatin1String(kind), key));
                }
                seen->insert(folded);
            }
            const QString directory = format == QLatin1String("latex") ? latexWorkingDirectory(name, source, entry)
                : (entry.contains(QStringLiteral("working_directory")) ? entry.value(QStringLiteral("working_directory")).toString() : QStringLiteral("."));
            const Py::Path actualDirectory = project / directory;
            if (!Py::resolve(actualDirectory).isRelativeTo(resolvedProject) || (!allowMissingInputs && !Py::isDir(actualDirectory))) {
                Py::raiseValueError(QStringLiteral("compiler working directory escapes its source folder or is unavailable"));
            }
            result.append({source, format, identity, output, stylesheet, directory, documentId});
        }
    } catch (const Py::Exception &error) {
        if (!isWrappedType(error.type)) {
            throw;
        }
        Py::raiseRenderError(layoutInvalid + QStringLiteral("thoughts-project.json: ") + error.message);
    }
    return result;
}

const QSet<QString> latexAssetSuffixes{QStringLiteral(".png"), QStringLiteral(".jpg"), QStringLiteral(".jpeg"), QStringLiteral(".pdf"),
                                       QStringLiteral(".eps"), QStringLiteral(".svg"), QStringLiteral(".csv"),  QStringLiteral(".dat")};

namespace
{
const QSet<QString> latexFolderExcludedParts{QStringLiteral("__pycache__"), QStringLiteral("build"), QStringLiteral("current_pdfs"),
                                             QStringLiteral("node_modules"), QStringLiteral("pdf"),  QStringLiteral("tmp"),
                                             QStringLiteral("venv")};

bool pathWithin(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root + QLatin1Char('/'));
}
}

bool latexFolderPath(const QString &relative)
{
    if (relative.isEmpty() || relative.contains(QLatin1Char('\\'))) {
        return false;
    }
    const QStringList parts = Py::Path(relative).segments();
    if (parts.isEmpty()) {
        return false;
    }
    for (qsizetype index = 0; index < parts.size(); ++index) {
        const QString &part = parts.at(index);
        if (part.isEmpty() || part.startsWith(QLatin1Char('.')) || (index + 1 < parts.size() && latexFolderExcludedParts.contains(part))) {
            return false;
        }
    }
    return true;
}

bool latexFolderSources(const Py::Path &project)
{
    const auto [root, owner] = folderOwner(project);
    const std::optional<Configuration> configuration = findConfiguration(root, owner);
    if (!configuration) {
        return false;
    }
    const Json value = projection(configuration->value, owner);
    for (const Json &document : value.value(QStringLiteral("documents")).items()) {
        if (document.value(QStringLiteral("format")).toString() != QLatin1String("latex")) {
            continue;
        }
        const Json options = value.value(QStringLiteral("rendering")).value(document.value(QStringLiteral("document_id")).toString());
        const QString source = document.value(QStringLiteral("source")).toString();
        const QString directory = latexWorkingDirectory(owner, source, options);
        const QString nested = directory == QLatin1String(".") ? QStringLiteral("tex") : directory + QStringLiteral("/tex");
        bool covered = false;
        for (const QString &tree : {QStringLiteral("tex"), nested}) {
            for (const QString &root : {QStringLiteral("source"), QStringLiteral("fragments"), QStringLiteral("support")}) {
                covered = covered || pathWithin(source, tree + QLatin1Char('/') + root);
            }
        }
        if (!covered) {
            return true;
        }
    }
    return false;
}

QList<Py::Path> latexFolderFiles(const Py::Path &project, const QSet<QString> &suffixes)
{
    const ThoughtIgnore ignored(project);
    QList<Py::Path> files;
    Py::walk(project, [&](Py::WalkStep &step) {
        QStringList kept;
        for (const QString &name : std::as_const(step.subdirectories)) {
            const Py::Path child = step.directory / name;
            if (!Py::isSymlink(child) && !name.startsWith(QLatin1Char('.')) && !latexFolderExcludedParts.contains(name)
                && !ignored.excludes(*child.relativeTo(project), true)) {
                kept.append(name);
            }
        }
        std::sort(kept.begin(), kept.end(), Py::codePointLess);
        step.subdirectories = kept;
        QStringList names = step.files;
        std::sort(names.begin(), names.end(), Py::codePointLess);
        for (const QString &name : std::as_const(names)) {
            const Py::Path path = step.directory / name;
            const QString relative = *path.relativeTo(project);
            if (suffixes.contains(Py::casefold(path.suffix())) && latexFolderPath(relative) && Py::isFile(path) && !Py::isSymlink(path)
                && !ignored.excludes(relative)) {
                files.append(path);
            }
        }
    });
    return files;
}

QList<Py::Path> markdownInputFiles(const Py::Path &project)
{
    static const QSet<QString> suffixes{QStringLiteral(".md"),  QStringLiteral(".markdown"), QStringLiteral(".png"),
                                        QStringLiteral(".jpg"), QStringLiteral(".jpeg"),     QStringLiteral(".gif"),
                                        QStringLiteral(".webp"), QStringLiteral(".svg"),     QStringLiteral(".css"),
                                        QStringLiteral(".txt")};
    const ThoughtIgnore ignored(project);
    QList<Py::Path> files;
    Py::walk(project, [&](Py::WalkStep &step) {
        QStringList kept;
        for (const QString &name : std::as_const(step.subdirectories)) {
            const Py::Path child = step.directory / name;
            if (!Py::isSymlink(child) && !ignored.excludes(*child.relativeTo(project), true)) {
                kept.append(name);
            }
        }
        std::sort(kept.begin(), kept.end(), Py::codePointLess);
        step.subdirectories = kept;
        QStringList names = step.files;
        std::sort(names.begin(), names.end(), Py::codePointLess);
        for (const QString &name : std::as_const(names)) {
            const Py::Path path = step.directory / name;
            if (suffixes.contains(Py::lower(path.suffix())) && Py::isFile(path) && !Py::isSymlink(path)
                && !ignored.excludes(*path.relativeTo(project))) {
                files.append(path);
            }
        }
    });
    return files;
}

}
