// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <QCryptographicHash>
#include <QDateTime>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QSet>
#include <algorithm>
#include <QRegularExpression>
#include <QUrl>
#include <QTemporaryDir>
#include <memory>

namespace Thoughts {
// Capture bytes once: the preview and the durable upload always refer to the
// same copy, even when another application edits the Downloads folder.
struct MarkdownFolderImport {
    QMap<QString, QByteArray> files;
    QMap<QString, qint64> sizes;
    QMap<QString, QDateTime> modifiedTimes;
    QMap<QString, QString> fileIssues;
    QMap<QString, QString> capturedImages;
    std::shared_ptr<QTemporaryDir> imageSnapshot;
    QStringList documents;
    static constexpr qint64 DocumentLimit = 4 * 1024 * 1024;
    static constexpr qint64 ImageLimit = 10 * 1024 * 1024;
    static constexpr qint64 SelectionLimit = 100 * 1024 * 1024;
    static constexpr int FileLimit = 1000;
    bool latex = false;
    bool repositoryLayout = false;
    QString error;
    qint64 bytes = 0;
    static QString folderName(const QString &directory) {
        const auto name = QFileInfo(QDir::cleanPath(directory)).fileName().left(200);
        return name.isEmpty() ? QStringLiteral("Imported folder") : name;
    }
    static bool markdown(const QString &path) {
        const auto suffix = QFileInfo(path).suffix().toLower();
        return suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
    }
    static bool html(const QString &path) {
        const auto suffix = QFileInfo(path).suffix().toLower();
        return suffix == QLatin1String("html") || suffix == QLatin1String("htm");
    }
    static bool image(const QString &path) {
        return QStringList{QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("gif"), QStringLiteral("webp"), QStringLiteral("svg")}.contains(QFileInfo(path).suffix().toLower());
    }
    static bool latexFile(const QString &path) {
        const auto parts = path.split(QLatin1Char('/'));
        if (parts.size() < 3 || parts[0] != QLatin1String("tex")
            || !QStringList{QStringLiteral("source"), QStringLiteral("fragments"), QStringLiteral("support"), QStringLiteral("assets"), QStringLiteral("generated")}.contains(parts[1])) return false;
        if (html(path)) return true;
        if (parts[1] != QLatin1String("assets") && parts[1] != QLatin1String("generated") && parts[1] != QLatin1String("source"))
            return QStringList{QStringLiteral("tex"), QStringLiteral("sty"), QStringLiteral("cls"), QStringLiteral("bib"), QStringLiteral("bst"), QStringLiteral("txt")}.contains(QFileInfo(path).suffix().toLower());
        return QStringList{QStringLiteral("tex"), QStringLiteral("sty"), QStringLiteral("cls"), QStringLiteral("bib"), QStringLiteral("bst"), QStringLiteral("txt"), QStringLiteral("png"), QStringLiteral("jpg"), QStringLiteral("jpeg"), QStringLiteral("pdf"), QStringLiteral("eps"), QStringLiteral("svg"), QStringLiteral("csv"), QStringLiteral("dat")}.contains(QFileInfo(path).suffix().toLower());
    }
    // Where the publisher looks for an entry point's inputs: no hidden or generated part.
    static bool latexFolderPath(const QString &relative) {
        static const QStringList excluded{QStringLiteral("__pycache__"), QStringLiteral("build"), QStringLiteral("current_pdfs"),
            QStringLiteral("node_modules"), QStringLiteral("pdf"), QStringLiteral("tmp"), QStringLiteral("venv")};
        const auto parts = relative.split(QLatin1Char('/'));
        for (qsizetype index = 0; index < parts.size(); ++index) {
            const auto &part = parts.at(index);
            if (part.isEmpty() || part.startsWith(QLatin1Char('.')) || (index + 1 < parts.size() && excluded.contains(part))) return false;
        }
        return true;
    }
    static bool standaloneLatex(const QByteArray &bytes) {
        QString text = QString::fromUtf8(bytes);
        text.remove(QRegularExpression(QStringLiteral("(?m)(?<!\\\\)%[^\\n]*")));
        return text.contains(QRegularExpression(QStringLiteral("\\\\documentclass\\s*(?:\\[[^\\]]*\\])?\\s*\\{")))
            && text.contains(QRegularExpression(QStringLiteral("\\\\begin\\s*\\{document\\}")));
    }
    // Fedora accepts only [A-Za-z0-9][A-Za-z0-9_.-]*.pdf; a source named
    // "My Paper.tex" must not make the whole Project publication fail.
    static QString libraryPdfFilename(const QString &stem) {
        static const QRegularExpression valid(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
        if (valid.match(stem).hasMatch()) return stem + QStringLiteral(".pdf");
        QString safe = stem;
        safe.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]+")), QStringLiteral("_"));
        safe.replace(QRegularExpression(QStringLiteral("_{2,}")), QStringLiteral("_"));
        while (!safe.isEmpty() && QStringLiteral("_.-").contains(safe.front())) safe.remove(0, 1);
        while (!safe.isEmpty() && QStringLiteral("_.-").contains(safe.back())) safe.chop(1);
        return (safe.isEmpty() ? QStringLiteral("document") : safe) + QStringLiteral(".pdf");
    }
    QString sourcePath(const QString &path) const { return latex ? path : QStringLiteral("markdown/source/") + path; }
    QJsonArray projectDocuments(const QString &project) const {
        QJsonArray result;
        for (const auto &path : documents) {
            QJsonObject entry{{QStringLiteral("format"), latex ? QStringLiteral("latex") : QStringLiteral("markdown")},
                {QStringLiteral("source"), sourcePath(path)}, {QStringLiteral("title"), title(path)}};
            // A nested entry point is named like Fedora's component_output, so two main.tex files differ.
            const bool nested = latex && QFileInfo(path).path() != QLatin1String("tex/source");
            const auto stem = nested ? QFileInfo(path).completeBaseName() + QLatin1Char('_')
                    + QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex().left(12))
                : QFileInfo(path).completeBaseName();
            entry.insert(QStringLiteral("output"), latex
                ? libraryPdfFilename(project.mid(5) + QLatin1Char('_') + stem)
                : QString(QStringLiteral("document_") + QString::fromLatin1(QCryptographicHash::hash(path.toUtf8(), QCryptographicHash::Sha256).toHex().left(20)) + QStringLiteral(".pdf")));
            if (repositoryLayout || nested) entry.insert(QStringLiteral("working_directory"), QFileInfo(path).path());
            result.append(entry);
        }
        return result;
    }
    static MarkdownFolderImport scanLatex(const QString &directory) {
        MarkdownFolderImport result;
        result.latex = true;
        const QDir root(QDir(directory).exists(QStringLiteral("tex/source")) ? QDir(directory).filePath(QStringLiteral("tex")) : directory);
        QSet<QString> folded;
        QDirIterator it(root.path(), QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const auto absolute = it.next();
            const QString path = QStringLiteral("tex/") + root.relativeFilePath(absolute);
            if (!latexFile(path)) continue;
            const QFileInfo info(absolute);
            if (info.isSymLink() || !info.canonicalFilePath().startsWith(root.canonicalPath() + QLatin1Char('/'))
                || path.contains(QLatin1Char('\\')) || path.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")))
                || path.size() > 700 || folded.contains(path.toCaseFolded())) {
                result.error = QStringLiteral("The folder contains an unsafe or duplicate path: ") + path; return result;
            }
            folded.insert(path.toCaseFolded());
            result.sizes[path] = info.size(); result.modifiedTimes[path] = info.lastModified().toUTC(); result.bytes += info.size();
            if (result.bytes > SelectionLimit || result.sizes.size() > FileLimit || info.size() > (html(path) ? 100000000 : ImageLimit)) {
                result.error = QStringLiteral("LaTeX folder exceeds the import limits (100 MiB total, 100 MB per HTML file, 10 MiB per other file, 1000 files)."); return result;
            }
            QFile file(absolute);
            if (!file.open(QIODevice::ReadOnly)) { result.error = QStringLiteral("Cannot read ") + path; return result; }
            const auto content = file.read((html(path) ? 100000000 : ImageLimit) + 1);
            if (file.error() != QFileDevice::NoError || content.size() != info.size()) { result.error = QStringLiteral("The folder changed while being copied: ") + path; return result; }
            result.files[path] = content;
            if (QFileInfo(path).path() == QLatin1String("tex/source") && path.endsWith(QLatin1String(".tex"))) result.documents.append(path);
            else if (path.startsWith(QLatin1String("tex/source/")) && path.endsWith(QLatin1String(".tex"))
                     && latexFolderPath(path) && standaloneLatex(content.left(DocumentLimit))) result.documents.append(path);
        }
        result.documents.sort();
        if (result.documents.isEmpty()) result.error = QStringLiteral("Choose a LaTeX project containing tex/source/*.tex, or its tex folder.");
        return result;
    }
    static MarkdownFolderImport scan(const QString &directory) {
        if (QDir(directory).exists(QStringLiteral("tex/source")) || !QDir(QDir(directory).filePath(QStringLiteral("source"))).entryList({QStringLiteral("*.tex")}, QDir::Files).isEmpty()) return scanLatex(directory);
        // Entry points may sit anywhere in the repository (paper/main.tex).
        QStringList entrypoints;
        QDirIterator candidates(directory, {QStringLiteral("*.tex")}, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (candidates.hasNext()) {
            const auto absolute = candidates.next();
            const auto name = QDir(directory).relativeFilePath(absolute);
            if (QFileInfo(absolute).isSymLink() || !latexFolderPath(name)) continue;
            QFile source(absolute);
            if (source.open(QIODevice::ReadOnly) && standaloneLatex(source.read(DocumentLimit))) entrypoints.append(name);
        }
        entrypoints.sort();
        if (!entrypoints.isEmpty()) {
            MarkdownFolderImport result;
            result.latex = true;
            result.repositoryLayout = true;
            QDirIterator files(directory, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
            while (files.hasNext()) {
                const auto absolute = files.next();
                const auto relative = QDir(directory).relativeFilePath(absolute);
                const auto path = QStringLiteral("tex/source/") + relative;
                if (!latexFile(path)) continue;
                const QFileInfo info(absolute);
                if (info.isSymLink() || !info.canonicalFilePath().startsWith(QFileInfo(directory).canonicalFilePath() + QLatin1Char('/'))) {
                    result.error = QStringLiteral("Unsafe repository source: ") + relative; return result;
                }
                result.bytes += info.size();
                if (info.size() > ImageLimit || result.bytes > SelectionLimit || result.files.size() >= FileLimit) {
                    result.error = QStringLiteral("Repository build inputs exceed the import limits."); return result;
                }
                QFile file(absolute);
                if (!file.open(QIODevice::ReadOnly)) { result.error = QStringLiteral("Cannot read ") + relative; return result; }
                result.files[path] = file.readAll(); result.sizes[path] = info.size(); result.modifiedTimes[path] = info.lastModified();
            }
            for (const auto &name : entrypoints) result.documents.append(QStringLiteral("tex/source/") + name);
            return result;
        }
        MarkdownFolderImport result;
        const QDir root(directory);
        result.imageSnapshot = std::make_shared<QTemporaryDir>();
        if (!result.imageSnapshot->isValid()) { result.error = QStringLiteral("Cannot create the folder preview copy."); return result; }
        QSet<QString> folded;
        QDirIterator it(directory, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString absolute = it.next();
            const QString path = root.relativeFilePath(absolute);
            if (!markdown(path) && !image(path) && !html(path)) continue;
            QFileInfo info(absolute);
            if (info.isSymLink() || !info.canonicalFilePath().startsWith(root.canonicalPath() + QLatin1Char('/'))
                || path.contains(QLatin1Char('\\')) || path.contains(QRegularExpression(QStringLiteral("[\\x00-\\x1f]")))
                || path.size() > 700 || folded.contains(path.toCaseFolded())) {
                result.error = QStringLiteral("The folder contains an unsafe or duplicate path: ") + path; return result;
            }
            folded.insert(path.toCaseFolded());
            result.sizes.insert(path, info.size());
            result.modifiedTimes.insert(path, info.lastModified().toUTC());
            result.bytes += info.size();
            if (markdown(path)) result.documents.append(path);
            // Oversized files stay visible so their documents can be deselected.
            if (info.size() > (markdown(path) ? DocumentLimit : html(path) ? 100000000 : ImageLimit)) {
                result.fileIssues.insert(path, (markdown(path) ? QStringLiteral("Document exceeds the 4 MiB limit: ")
                    : html(path) ? QStringLiteral("HTML exceeds the 100 MB limit: ") : QStringLiteral("Image exceeds the 10 MiB limit: ")) + path);
                continue;
            }
            QFile file(absolute);
            if (!file.open(QIODevice::ReadOnly)) { result.fileIssues.insert(path, QStringLiteral("Cannot read ") + path); continue; }
            const QByteArray content = file.read((markdown(path) ? DocumentLimit : html(path) ? 100000000 : ImageLimit) + 1);
            if (file.error() != QFileDevice::NoError || content.size() != info.size()) { result.error = QStringLiteral("The folder changed while being copied. Please reopen it: ") + path; return result; }
            if (markdown(path)) result.files.insert(path, content);
            else {
                // Keep image copies on disk; an initially oversized selection
                // must not consume its entire size in application memory.
                const auto captured = result.imageSnapshot->filePath(QString::number(result.capturedImages.size()));
                QFile out(captured);
                if (!out.open(QIODevice::WriteOnly) || out.write(content) != content.size() || !out.flush()) { result.error = QStringLiteral("Cannot copy image: ") + path; return result; }
                result.capturedImages.insert(path, captured);
            }
        }
        std::sort(result.documents.begin(), result.documents.end(), [](const QString &a, const QString &b) {
            const auto key = [](const QString &p) { return QFileInfo(p).fileName().compare(QStringLiteral("README.md"), Qt::CaseInsensitive) == 0 ? QFileInfo(p).path() + QStringLiteral("/\x01") : p; };
            return key(a) < key(b);
        });
        if (result.documents.isEmpty()) result.error = QStringLiteral("This folder contains no Markdown documents.");
        return result;
    }
    static MarkdownFolderImport savedBundle(const QString &path, const QString &sha256) {
        MarkdownFolderImport result;
        QFile file(path);
        if (path.isEmpty() || !file.open(QIODevice::ReadOnly) || file.size() > 2 * SelectionLimit) { result.error = QStringLiteral("The saved folder copy is unavailable."); return result; }
        const auto raw = file.readAll();
        if (file.error() != QFileDevice::NoError || sha256.size() != 64 || QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex() != sha256.toLatin1()) {
            result.error = QStringLiteral("The saved folder copy failed verification."); return result;
        }
        const auto bundle = QJsonDocument::fromJson(raw).object();
        if (bundle.value(QStringLiteral("schema_version")).toInt() != 1) { result.error = QStringLiteral("Unsupported saved folder copy."); return result; }
        result.latex = bundle.value(QStringLiteral("format")).toString() == QLatin1String("latex");
        result.repositoryLayout = bundle.value(QStringLiteral("repository_layout")).toBool();
        for (const auto &entry : bundle.value(QStringLiteral("files")).toArray()) {
            const auto item = entry.toObject(); const auto name = item.value(QStringLiteral("path")).toString();
            const auto decoded = QByteArray::fromBase64Encoding(item.value(QStringLiteral("content")).toString().toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
            if (name.isEmpty() || QDir::isAbsolutePath(name) || QDir::cleanPath(name) != name || name.startsWith(QLatin1String("../")) || name.contains(QLatin1Char('\\')) || (result.latex ? !latexFile(name) : (!markdown(name) && !image(name) && !html(name))) || !decoded || result.files.contains(name)) {
                result.error = QStringLiteral("The saved folder contains an invalid file."); return result;
            }
            result.files.insert(name, decoded.decoded); result.sizes.insert(name, decoded.decoded.size()); result.bytes += decoded.decoded.size();
            if (result.bytes > SelectionLimit || result.files.size() > FileLimit) { result.error = QStringLiteral("The saved folder exceeds the import limits."); return result; }
        }
        for (const auto &entry : bundle.value(QStringLiteral("documents")).toArray()) {
            const auto name = entry.toString();
            if ((result.latex ? !(name.startsWith(QLatin1String("tex/source/")) && name.endsWith(QLatin1String(".tex"))) : !markdown(name)) || !result.files.contains(name)) { result.error = QStringLiteral("A saved document is unavailable."); return result; }
            result.documents.append(name);
        }
        return result;
    }
    QDateTime selectionModified(const QStringList &selected) const {
        QDateTime latest;
        for (const auto &path : includedFiles(selected)) if (modifiedTimes.value(path).isValid() && (!latest.isValid() || modifiedTimes.value(path) > latest)) latest = modifiedTimes.value(path);
        return latest;
    }
    QString title(const QString &path) const {
        if (latex) return QFileInfo(path).completeBaseName();
        const auto match = QRegularExpression(QStringLiteral("^#\\s+(.+?)\\s*#*\\s*$"), QRegularExpression::MultilineOption).match(QString::fromUtf8(files.value(path)));
        return (match.hasMatch() ? match.captured(1).trimmed() : QFileInfo(path).completeBaseName()).left(200);
    }
    struct Reference { bool image; QString url; };
    QList<Reference> references(const QString &path) const {
        QList<Reference> result;
        QString text = QString::fromUtf8(files.value(path));
        // Ignore fenced and inline code in the preview. Fedora still performs
        // the authoritative validation using the complete Markdown syntax tree.
        text.remove(QRegularExpression(QStringLiteral("(?ms)^ {0,3}(`{3,}|~{3,})[^\\n]*\\n.*?^ {0,3}\\1[ \\t]*$")));
        text.remove(QRegularExpression(QStringLiteral("(`+)[^`]*\\1")));
        const auto label = [](QString value) { return value.simplified().toCaseFolded(); };
        QMap<QString, QString> definitions;
        const QRegularExpression definition(QStringLiteral("(?m)^ {0,3}\\[([^\\]\\n]+)\\]:[ \\t]*(?:<([^>\\n]+)>|([^\\s]+))[^\\n]*$"));
        auto defs = definition.globalMatch(text);
        while (defs.hasNext()) { const auto m = defs.next(); definitions.insert(label(m.captured(1)), m.captured(2).isEmpty() ? m.captured(3) : m.captured(2)); }
        text.remove(definition);
        // Angle-bracket destinations and balanced parentheses in file names.
        const QRegularExpression inlineLink(QStringLiteral("(!?)\\[[^\\]\\n]*\\]\\(\\s*(?:<([^>\\n]+)>|((?:\\\\.|[^\\s()\\\\]|\\((?:\\\\.|[^()\\\\])*\\))*))(?:\\s+[^)]*)?\\)"));
        auto matches = inlineLink.globalMatch(text);
        while (matches.hasNext()) { const auto m = matches.next(); result.append({!m.captured(1).isEmpty(), m.captured(2).isEmpty() ? m.captured(3) : m.captured(2)}); }
        text.remove(inlineLink);
        const QRegularExpression referenceLink(QStringLiteral("(!?)\\[([^\\]\\n]+)\\](?:\\[([^\\]\\n]*)\\])?"));
        matches = referenceLink.globalMatch(text);
        while (matches.hasNext()) {
            const auto m = matches.next();
            const auto key = label(m.captured(3).isEmpty() ? m.captured(2) : m.captured(3));
            if (definitions.contains(key)) result.append({!m.captured(1).isEmpty(), definitions.value(key)});
        }
        return result;
    }
    QString target(const QString &path, QString link) const {
        link.replace(QRegularExpression(QString::fromLatin1(R"(\\([[:punct:]]))")), QStringLiteral("\\1"));
        const QUrl url(link);
        if (!url.scheme().isEmpty() || !url.host().isEmpty() || url.path().isEmpty()) return {};
        QString value = QDir::cleanPath(QFileInfo(path).path() + QLatin1Char('/') + url.path(QUrl::FullyDecoded));
        if (!sizes.contains(value)) for (const auto &name : {QStringLiteral("README.md"), QStringLiteral("readme.md"), QStringLiteral("index.md")}) {
            const auto candidate = value == QLatin1String(".") ? name : value + QLatin1Char('/') + name;
            if (sizes.contains(candidate)) return candidate;
        }
        return value;
    }
    QSet<QString> includedFiles(const QStringList &selected) const {
        if (latex) { const auto paths = sizes.keys(); return QSet<QString>(paths.begin(), paths.end()); }
        QSet<QString> included(selected.begin(), selected.end());
        if (!selected.isEmpty()) for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) if (html(it.key())) included.insert(it.key());
        for (const auto &path : selected) for (const auto &reference : references(path)) {
            const auto resolved = target(path, reference.url);
            if (resolved.isEmpty()) continue;
            if (sizes.contains(resolved)) { if (image(resolved)) included.insert(resolved); }
            else if (!reference.image) {
                const auto prefix = resolved == QLatin1String(".") ? QString() : resolved + QLatin1Char('/');
                for (auto it = sizes.cbegin(); it != sizes.cend(); ++it) if (it.key().startsWith(prefix) && image(it.key())) included.insert(it.key());
            }
        }
        return included;
    }
    qint64 selectionBytes(const QStringList &selected) const {
        qint64 total = 0;
        for (const auto &path : includedFiles(selected)) total += sizes.value(path);
        return total;
    }
    QStringList selectionIssues(const QStringList &selected) const {
        QStringList result;
        const auto included = includedFiles(selected);
        if (selectionBytes(selected) > SelectionLimit) result.append(QStringLiteral("Selection exceeds 100 MiB. Uncheck documents or folders to continue."));
        if (included.size() > FileLimit) result.append(QStringLiteral("Selection exceeds 1000 files, including images. Uncheck documents or folders to continue."));
        return result;
    }
    QStringList issues(const QString &path, const QStringList &selected) const {
        QStringList result;
        if (latex) return result;
        for (const auto &file : includedFiles({path})) if (fileIssues.contains(file)) result.append(fileIssues.value(file));
        for (const auto &reference : references(path)) {
            const auto resolved = target(path, reference.url);
            if (resolved.isEmpty()) {
                if (reference.image) result.append(QStringLiteral("Image is outside this folder: ") + reference.url);
                continue;
            }
            const auto prefix = resolved == QLatin1String(".") ? QString() : resolved + QLatin1Char('/');
            const bool directory = std::any_of(sizes.keyBegin(), sizes.keyEnd(), [&](const QString &p) { return p.startsWith(prefix); });
            if (!sizes.contains(resolved) && !directory) result.append(QStringLiteral("Missing file: ") + resolved);
            else if (markdown(resolved) && !selected.contains(resolved)) result.append(QStringLiteral("Linked document is not selected: ") + resolved);
        }
        result.removeDuplicates(); return result;
    }
    QByteArray bundle(const QStringList &selected, bool validateLinks = true) const {
        if (!selectionIssues(selected).isEmpty()) return {};
        if (validateLinks) for (const auto &path : selected) if (!issues(path, selected).isEmpty()) return {};
        QJsonArray entries, order;
        for (const auto &path : selected) order.append(path);
        auto included = includedFiles(selected).values(); std::sort(included.begin(), included.end());
        for (const auto &path : included) {
            QByteArray content = files.value(path);
            if ((image(path) || html(path)) && capturedImages.contains(path)) {
                QFile file(capturedImages.value(path));
                if (!file.open(QIODevice::ReadOnly)) return {};
                content = file.readAll();
                if (file.error() != QFileDevice::NoError) return {};
            }
            if (content.size() != sizes.value(path, -1)) return {};
            entries.append(QJsonObject{{QStringLiteral("path"), path}, {QStringLiteral("content"), QString::fromLatin1(content.toBase64())}});
        }
        return QJsonDocument(QJsonObject{{QStringLiteral("schema_version"), 1}, {QStringLiteral("repository_layout"), repositoryLayout}, {QStringLiteral("format"), latex ? QStringLiteral("latex") : QStringLiteral("markdown")}, {QStringLiteral("documents"), order}, {QStringLiteral("files"), entries}}).toJson(QJsonDocument::Compact);
    }
};
}
