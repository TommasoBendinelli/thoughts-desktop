/* SPDX-License-Identifier: GPL-2.0-or-later */
// Proposal link checks. Each rule restates the Fedora renderer's resolver
// (thoughts_server.renderer_tools.doc_links: pdf_link_pdf, pdf_link_target,
// library_pdfs, _library_pdf_link, target_for_selector, normalize_selector) over the
// same Project configuration code the source publisher freezes with
// (tools/publisher), so a link the tools accept is a link the build resolves.
#include "proposallinks.h"
#include "thoughtspagelinkurl.h"

#include "../part/sourceowner.h"
#include "../tools/publisher/buildinputs.h"
#include "../tools/publisher/htmlpages.h"
#include "../tools/publisher/markdownlinks.h"
#include "../tools/publisher/projectconfig.h"
#include "../tools/publisher/projectdocuments.h"
#include "../tools/publisher/pyerror.h"
#include "../tools/publisher/pystr.h"
#include "../tools/publisher/texcommands.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <poppler-qt6.h>

#include <functional>
#include <optional>

namespace {
using Thoughts::ProposalLinkIssue;
using Thoughts::SourceOwner;
namespace Publisher = Thoughts::Publisher;
namespace Py = Thoughts::Py;

const QString documentStart = QStringLiteral("document-start");
const QString sourceLabelPrefix = QStringLiteral("source-label:");

bool pdfIdentifier(const QString &value)
{
    // PDF_LINK_IDENTIFIER_PATTERN
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    return pattern.match(value).hasMatch();
}

// doc_links.PDF_LINK_ANCHOR_PATTERN: anchors may also carry ':' and '*' (section*.10, cite.doe:2020).
bool pdfAnchor(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.:*-]*$"));
    return pattern.match(value).hasMatch();
}

bool sourceLabel(const QString &value)
{
    // SOURCE_LABEL_PATTERN
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.:-]*$"));
    return pattern.match(value).hasMatch();
}

QString command(const QString &name, const QString &target, const QString &anchor, const QString &text)
{
    return QStringLiteral("\\%1{%2}{%3}{%4}").arg(name, target, anchor, text);
}

QString stem(const QString &filename)
{
    return filename.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive) ? filename.chopped(4) : filename;
}

// One PDF of some Project, as its configuration declares it.
struct LibraryPdf {
    QString project;           // Project name
    QString configuration;     // thoughts-project.json path
    QStringList folders;       // its source folders
    QString path;              // relative to the docs root
    QString filename;
    bool included = false;
    QString documentId;        // its Library document
};

// One source document of some Project.
struct LibrarySource {
    QString project;
    QString folder;   // owner
    QString source;   // relative to the folder
    bool included = false;
    QString documentId; // the Library document built from it
};

class Checker
{
public:
    Checker(const QString &root, const QString &owner, const QString &sourceRelative, const QString &updatedSource, const QStringList &serverOrigins = {})
        : m_root(root)
        , m_owner(owner)
        , m_sourceRelative(sourceRelative)
        , m_updatedSource(updatedSource)
        , m_serverOrigins(serverOrigins)
    {
        const QString suffix = QFileInfo(sourceRelative).suffix().toLower();
        m_markdown = suffix == QLatin1String("md") || suffix == QLatin1String("markdown");
    }

    std::optional<ProposalLinkIssue> check(const Publisher::LinkCommand &link) const
    {
        if (link.name == QLatin1String("TexLink") || link.name == QLatin1String("CrossTexLink")) return retiredLink(link);
        if (link.arguments.size() < 3)
            return issue(QStringLiteral("malformed_link"),
                         QStringLiteral("\\%1 takes three braced arguments: \\%1{%2}{%3}{visibleText}.")
                             .arg(link.name, link.name.endsWith(QLatin1String("PdfLink")) ? QStringLiteral("target") : QStringLiteral("source"),
                                  link.name.endsWith(QLatin1String("PdfLink")) ? QStringLiteral("anchor") : QStringLiteral("label")));
        const QString target = link.arguments.at(0);
        const QString anchor = link.arguments.at(1);
        const QString text = link.arguments.at(2);
        if (link.name == QLatin1String("PdfLink")) return pdfLink(target, anchor, text);
        if (link.name == QLatin1String("CrossPdfLink")) return crossPdfLink(target, anchor, text);
        // doc_links._source_link_payload: the label document-start opens the target's start, as
        // for \PdfLink; any other label is a Source label the target defines.
        const QString label = anchor == documentStart ? documentStart : sourceLabelPrefix + anchor;
        if (link.name == QLatin1String("SourceLink")) return sourceLink(link.name, m_owner + QLatin1Char('/') + target, label, anchor, text, false);
        return sourceLink(link.name, target, label, anchor, text, true);
    }

    std::optional<ProposalLinkIssue> markdownLink(const QString &target, bool image, const QString &text) const;
    std::optional<ProposalLinkIssue> serverPageLink(const QString &address, const QString &text) const;

private:
    // A link as this document's language writes it: the LaTeX command, or in
    // a Markdown source the relative Markdown link that means it.
    QString form(const QString &name, const QString &target, const QString &anchor, const QString &text) const
    {
        if (!m_markdown) return command(name, target, anchor, text);
        const QString label = anchor == documentStart && name.endsWith(QLatin1String("SourceLink")) ? QString() : anchor;
        return QStringLiteral("[%1](%2)").arg(text, Publisher::MarkdownLinks::markdownForm(name, target, label, m_owner, m_sourceRelative.mid(m_owner.size() + 1)));
    }

    static ProposalLinkIssue issue(const QString &reason, const QString &message, const QString &suggestion = {})
    {
        ProposalLinkIssue result;
        result.reason = reason;
        result.message = message;
        result.suggestion = suggestion;
        return result;
    }

    QString projectName(const QString &owner) const
    {
        try {
            if (const auto configuration = Publisher::findConfiguration(Py::Path(m_root), owner))
                return configuration->value.value(QStringLiteral("name")).toString();
        } catch (const Py::Exception &) {
        }
        return {};
    }

    // Every PDF and source document the Project configurations declare.
    void loadCatalog() const
    {
        if (m_catalogLoaded) return;
        m_catalogLoaded = true;
        QList<Py::Path> paths;
        try {
            paths = Publisher::configurationPaths(Py::Path(m_root));
        } catch (const Py::Exception &) {
            return;
        }
        for (const Py::Path &path : std::as_const(paths)) {
            Py::Json value;
            try {
                value = Publisher::readConfiguration(path);
            } catch (const Py::Exception &) {
                continue;
            }
            const QString name = value.value(QStringLiteral("name")).toString();
            QHash<QString, QString> folders;
            QStringList paths;
            for (const Py::Json &folder : value.value(QStringLiteral("source_folders")).items()) {
                const QString folderPath = folder.value(QStringLiteral("path")).toString();
                folders.insert(folder.value(QStringLiteral("folder_id")).toString(), folderPath);
                paths.append(folderPath);
            }
            for (const Py::Json &document : value.value(QStringLiteral("documents")).items()) {
                const Py::Json &source = document.value(QStringLiteral("source"));
                const QString folder = folders.value(source.value(QStringLiteral("folder_id")).toString());
                const QString relative = source.value(QStringLiteral("path")).toString();
                if (folder.isEmpty() || relative.isEmpty()) continue;
                const QString format = document.value(QStringLiteral("format")).toString();
                const bool included = document.value(QStringLiteral("included")).toBool();
                const QString documentId = document.value(QStringLiteral("document_id")).toString();
                if (format == QLatin1String("pdf")) {
                    m_pdfs.append({name, path.str(), paths, folder + QLatin1Char('/') + relative, relative.section(QLatin1Char('/'), -1), included, documentId});
                } else if (format == QLatin1String("latex") || format == QLatin1String("markdown")) {
                    m_sources.append({name, folder, relative, included, documentId});
                }
            }
        }
    }

    QList<LibraryPdf> pdfsNamed(const QString &filename) const
    {
        loadCatalog();
        QList<LibraryPdf> found;
        const QString folded = Py::casefold(filename);
        for (const LibraryPdf &pdf : m_pdfs)
            if (Py::casefold(pdf.filename) == folded) found.append(pdf);
        // Selected copies first: those are the ones a link can reach.
        std::stable_sort(found.begin(), found.end(), [](const LibraryPdf &a, const LibraryPdf &b) { return a.included && !b.included; });
        return found;
    }

    // library_pdfs(docs_root, filename): the PDFs named exactly filename that
    // some Project selects. The build finds a Library PDF by this name alone,
    // in whichever Project holds it.
    QList<LibraryPdf> selectedPdfsNamed(const QString &filename) const
    {
        QList<LibraryPdf> found;
        for (const LibraryPdf &pdf : pdfsNamed(filename))
            if (pdf.included && pdf.filename == filename) found.append(pdf);
        return found;
    }

    QStringList similarPdfKeys(const QString &key) const
    {
        loadCatalog();
        QStringList similar;
        const QString folded = Py::casefold(key);
        for (const LibraryPdf &pdf : m_pdfs) {
            const QString candidate = stem(pdf.filename);
            const QString foldedCandidate = Py::casefold(candidate);
            if (folded.size() >= 4 && (foldedCandidate.contains(folded) || folded.contains(foldedCandidate)) && !similar.contains(candidate))
                similar.append(candidate);
            if (similar.size() == 3) break;
        }
        return similar;
    }

    // The anchors of a PDF: the doc-anchor: contents of its non-link
    // annotations, counted (pdf_link_target needs exactly one).
    std::optional<QHash<QString, int>> pdfAnchors(const QString &path) const
    {
        if (const auto cached = m_anchors.constFind(path); cached != m_anchors.cend()) return *cached;
        std::optional<QHash<QString, int>> anchors;
        if (std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path); document && !document->isLocked()) {
            anchors.emplace();
            for (int index = 0; index < document->numPages(); ++index) {
                const std::unique_ptr<Poppler::Page> page = document->page(index);
                if (!page) continue;
                for (const auto &annotation : page->annotations()) {
                    if (annotation->subType() == Poppler::Annotation::ALink) continue;
                    const QString contents = annotation->contents().trimmed();
                    if (contents.startsWith(QLatin1String("doc-anchor:"))) (*anchors)[contents.mid(11)] += 1;
                }
            }
        }
        m_anchors.insert(path, anchors);
        return anchors;
    }

    // The page count and named destinations of a PDF (doc_links.pdf_anchor_index), or -1 pages when unreadable.
    int pdfPageCount(const QString &path) const
    {
        if (const auto cached = m_pageCounts.constFind(path); cached != m_pageCounts.cend()) return *cached;
        const std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
        const int pages = document && !document->isLocked() ? document->numPages() : -1;
        m_pageCounts.insert(path, pages);
        return pages;
    }

    bool pdfHasDestination(const QString &path, const QString &name) const
    {
        const QString key = path + QChar(0) + name;
        if (const auto cached = m_destinations.constFind(key); cached != m_destinations.cend()) return *cached;
        const std::unique_ptr<Poppler::Document> document = Poppler::Document::load(path);
        bool found = false;
        if (document && !document->isLocked()) {
            const std::unique_ptr<Poppler::LinkDestination> destination = document->linkDestination(name);
            found = destination && destination->pageNumber() >= 1 && destination->pageNumber() <= document->numPages();
        }
        m_destinations.insert(key, found);
        return found;
    }

    // pdf_link_target (doc_links.resolve_pdf_anchor): document-start; exactly one doc-anchor:
    // annotation (it wins over a destination of the same name); page.N of the file's pages;
    // a named destination of the PDF (equation.1, figure.caption.3, subsection.2.1, ...).
    std::optional<QString> anchorProblem(const QString &absolutePdf, const QString &anchor) const
    {
        if (anchor == documentStart) return std::nullopt;
        const auto anchors = pdfAnchors(absolutePdf);
        if (!anchors) return std::nullopt; // Unreadable here; the build decides.
        const int count = anchors->value(anchor);
        if (count == 1) return std::nullopt;
        const QString filename = QFileInfo(absolutePdf).fileName();
        if (count > 1) return QStringLiteral("The anchor %1 occurs %2 times in %3; a PDF link needs a unique doc-anchor: annotation.").arg(anchor).arg(count).arg(filename);
        static const QRegularExpression page(QStringLiteral("^page\\.([1-9][0-9]{0,6})$"));
        if (const QRegularExpressionMatch match = page.match(anchor); match.hasMatch()) {
            const int pages = pdfPageCount(absolutePdf);
            if (pages < 0 || match.captured(1).toInt() <= pages) return std::nullopt;
            return QStringLiteral("%1 has %2 page(s); use page.1 to page.%2.").arg(filename).arg(pages);
        }
        if (pdfHasDestination(absolutePdf, anchor)) return std::nullopt;
        QStringList known = anchors->keys();
        std::sort(known.begin(), known.end());
        return QStringLiteral("%1 has no doc-anchor:%2 annotation, page or named destination %2; use document-start, page.N%3, or a name the "
                              "pdf_anchors tool lists (equation.1, figure.caption.3, subsection.2.1, ...).")
            .arg(filename, anchor, known.isEmpty() ? QString() : QStringLiteral(", one of its doc-anchors (") + known.mid(0, 10).join(QStringLiteral(", ")) + QLatin1Char(')'));
    }

    QString absolute(const QString &relative) const { return QDir(m_root).filePath(relative); }

    // pdf_link_pdf(docs_root, ..., key, source_project=owner): the Project's
    // selected attachment, or nothing when it does not select key.pdf.
    std::optional<QString> attachedPdf(const QString &owner, const QString &key, bool *checked) const
    {
        *checked = true;
        QList<Publisher::ProjectAttachment> attachments;
        try {
            attachments = Publisher::projectAttachments(Py::Path(m_root) / owner);
        } catch (const Py::Exception &) {
            *checked = false; // An unreadable manifest fails the build on its own.
            return std::nullopt;
        }
        for (const auto &attachment : std::as_const(attachments)) {
            if (attachment.filename != key + QStringLiteral(".pdf")) continue;
            return attachment.source.isEmpty() ? absolute(QStringLiteral("third_party/") + attachment.filename)
                                               : QDir::cleanPath(QDir(absolute(owner)).filePath(attachment.source));
        }
        return std::nullopt;
    }

    // Where key.pdf is, for a link that does not reach it as written: the
    // \PdfLink that does. The build finds a Library PDF by its filename in
    // whichever Project selects it, so no folder is needed.
    QString pdfElsewhere(const QString &key, const QString &anchor, const QString &text, QString *suggestion) const
    {
        const QString sourceProject = projectName(m_owner);
        for (const LibraryPdf &pdf : pdfsNamed(key + QStringLiteral(".pdf"))) {
            const bool here = pdf.project == sourceProject;
            if (!pdf.included)
                return here ? QStringLiteral(" %1 is in this Project but not selected for building; select it in the Project, or link a selected PDF.").arg(pdf.filename)
                            : QStringLiteral(" %1 belongs to Project %2 but is not selected there, so no link can reach it.").arg(pdf.filename, pdf.project);
            QString targetAnchor = anchor;
            QString anchorNote;
            if (anchor != documentStart && anchorProblem(absolute(pdf.path), anchor)) {
                targetAnchor = documentStart;
                anchorNote = QStringLiteral("; that PDF has no doc-anchor:%1 annotation, so the link opens its first page").arg(anchor);
            }
            *suggestion = form(QStringLiteral("PdfLink"), stem(pdf.filename), targetAnchor, text);
            return here ? QStringLiteral(" It is a PDF of this Project; link to it with %1%2.").arg(*suggestion, anchorNote)
                        : QStringLiteral(" It is a PDF of Project %1; link to it with %2%3.").arg(pdf.project, *suggestion, anchorNote);
        }
        const QStringList similar = similarPdfKeys(key);
        if (!similar.isEmpty()) return QStringLiteral(" No Project has %1.pdf; similar PDF keys: %2.").arg(key, similar.join(QStringLiteral(", ")));
        return QStringLiteral(" No Project in the Library has a PDF named %1.pdf.").arg(key);
    }

    // doc_links._library_pdf_link: \PdfLink{key} and \CrossPdfLink{folder/key}
    // reach the PDF named key.pdf of the note's own Project, else of the
    // Project of the folder a \CrossPdfLink names, else of any Project that
    // selects it. So a \PdfLink to another Project's PDF resolves, and so does
    // a \CrossPdfLink naming the note's own folder, a folder of no Project, or
    // one that no longer holds the PDF (it moved). Only a PDF no Project
    // selects leaves the link inactive. Another Project's PDF opens by its
    // filename: only a named anchor needs its file, and one this Library does
    // not hold is left to the build. anchorFix is the link with document-start.
    std::optional<ProposalLinkIssue> libraryPdfLink(const QString &link, const QString &key, const QString &anchor, const QString &text,
                                                    const QString &namedFolder, const QString &anchorFix) const
    {
        bool checked = false;
        if (const auto own = attachedPdf(m_owner, key, &checked)) {
            if (!QFileInfo(*own).isFile())
                return issue(QStringLiteral("pdf_unavailable"), QStringLiteral("%1: the Project selects %2.pdf, but its file is missing (%3).").arg(link, key, QDir(m_root).relativeFilePath(*own)));
            if (const auto problem = anchorProblem(*own, anchor)) return issue(QStringLiteral("anchor_not_found"), link + QStringLiteral(": ") + *problem, anchorFix);
            return std::nullopt;
        }
        if (!checked) return std::nullopt;
        const QString filename = key + QStringLiteral(".pdf");
        const QList<LibraryPdf> holders = selectedPdfsNamed(filename);
        if (holders.isEmpty()) {
            QString suggestion;
            const QString where = pdfElsewhere(key, anchor, text, &suggestion);
            bool held = false;
            for (const LibraryPdf &pdf : pdfsNamed(filename)) held = held || pdf.filename == filename;
            return issue(held ? QStringLiteral("pdf_not_selected") : QStringLiteral("pdf_not_in_library"),
                         QStringLiteral("%1: %2 is not a selected PDF of any Project, so the link would stay inactive.%3").arg(link, filename, where), suggestion);
        }
        const LibraryPdf *holder = &holders.first();
        for (const LibraryPdf &candidate : holders) {
            if (!namedFolder.isEmpty() && candidate.folders.contains(namedFolder)) {
                holder = &candidate;
                break;
            }
        }
        const QString file = absolute(holder->path);
        if (anchor == documentStart || !QFileInfo(file).isFile()) return std::nullopt;
        if (const auto problem = anchorProblem(file, anchor)) return issue(QStringLiteral("anchor_not_found"), link + QStringLiteral(": ") + *problem, anchorFix);
        return std::nullopt;
    }

    std::optional<ProposalLinkIssue> pdfLink(const QString &key, const QString &anchor, const QString &text) const
    {
        const QString link = form(QStringLiteral("PdfLink"), key, anchor, text);
        if (!pdfIdentifier(key)) {
            // A path or filename, e.g. docs-Other/paper or paper.pdf.
            const QString bare = stem(key.section(QLatin1Char('/'), -1));
            QString suggestion;
            const QString where = pdfIdentifier(bare) ? pdfElsewhere(bare, pdfAnchor(anchor) ? anchor : documentStart, text, &suggestion) : QString();
            return issue(QStringLiteral("invalid_pdf_key"),
                         QStringLiteral("%1 does not name a PDF: \\PdfLink takes a Library PDF by its filename without .pdf (letters, digits, dots, underscores, hyphens), with no folder.%2").arg(link, where),
                         suggestion);
        }
        if (!pdfAnchor(anchor))
            return issue(QStringLiteral("invalid_anchor"), QStringLiteral("%1 has an invalid anchor; use document-start, page.N, a named destination of the PDF or the name of a doc-anchor: annotation (letters, digits, dots, colons, asterisks, underscores, hyphens).").arg(link));
        return libraryPdfLink(link, key, anchor, text, QString(), form(QStringLiteral("PdfLink"), key, documentStart, text));
    }

    std::optional<ProposalLinkIssue> crossPdfLink(const QString &selector, const QString &anchor, const QString &text) const
    {
        const QString link = form(QStringLiteral("CrossPdfLink"), selector, anchor, text);
        const QStringList parts = selector.split(QLatin1Char('/'));
        // parse_cross_pdf_link_uri: docs-<folder>/<key>
        if (parts.size() != 2 || !SourceOwner::isConnectedFolder(parts.first()) || !pdfIdentifier(parts.last())) {
            const QString bare = stem(selector.section(QLatin1Char('/'), -1));
            QString suggestion;
            const QString where = pdfIdentifier(bare) ? pdfElsewhere(bare, pdfAnchor(anchor) ? anchor : documentStart, text, &suggestion) : QString();
            return issue(QStringLiteral("invalid_cross_target"),
                         QStringLiteral("%1 does not name a PDF: \\CrossPdfLink takes docs-<folder>/<pdfKey>, a docs-* folder and the PDF filename without .pdf; "
                                        "\\PdfLink{pdfKey}{anchor}{text} is simpler and finds the PDF in any Project.%2")
                             .arg(link, where),
                         suggestion);
        }
        if (!pdfAnchor(anchor))
            return issue(QStringLiteral("invalid_anchor"), QStringLiteral("%1 has an invalid anchor; use document-start, page.N, a named destination of the PDF or the name of a doc-anchor: annotation.").arg(link));
        // The folder only says where to look first; the PDF is found by its name.
        return libraryPdfLink(link, parts.last(), anchor, text, parts.first(), form(QStringLiteral("CrossPdfLink"), selector, documentStart, text));
    }

    // normalize_selector: <owner>/<path ending in .tex, .md or .markdown>.
    static std::optional<QString> normalizeSelector(const QString &selector)
    {
        const QString normalized = selector.trimmed().replace(QLatin1Char('\\'), QLatin1Char('/'));
        const QString relative = SourceOwner::relative(normalized);
        if (relative.isEmpty()) return std::nullopt;
        for (const QString &part : normalized.split(QLatin1Char('/')))
            if (part.isEmpty() || part == QLatin1String(".") || part == QLatin1String("..")) return std::nullopt;
        const QString suffix = QFileInfo(relative.section(QLatin1Char('/'), -1)).suffix().toLower();
        if (suffix != QLatin1String("tex") && suffix != QLatin1String("md") && suffix != QLatin1String("markdown")) return std::nullopt;
        return normalized;
    }

    // source_token (target_for_selector): tex/source/<name> is also reachable as <name>.
    static QString sourceToken(const QString &source)
    {
        return source.section(QLatin1Char('/'), 0, -2) == QLatin1String("tex/source") ? source.section(QLatin1Char('/'), -1) : source;
    }

    // Whether the Source label name is defined in the folder's sources (or in
    // the text this request writes): \SourceLabel{name} in LaTeX,
    // {#source-label:name} in Markdown.
    bool labelDefined(const QString &folder, const QString &name) const
    {
        const QRegularExpression tex(QStringLiteral("\\\\SourceLabel\\s*\\{\\s*%1\\s*\\}").arg(QRegularExpression::escape(name)));
        // A Markdown heading or span attribute {#source-label:name}, not a link that names it.
        const QRegularExpression markdown(QStringLiteral("\\{\\s*#%1(?=[\\s}])").arg(QRegularExpression::escape(sourceLabelPrefix + name)));
        const auto matches = [&](const QString &path, const QString &content) {
            const QString suffix = QFileInfo(path).suffix().toLower();
            return (suffix == QLatin1String("tex") ? tex : markdown).match(content).hasMatch();
        };
        if (m_sourceRelative.startsWith(folder + QLatin1Char('/')) && matches(m_sourceRelative, m_updatedSource)) return true;
        QDirIterator files(absolute(folder), {QStringLiteral("*.tex"), QStringLiteral("*.md"), QStringLiteral("*.markdown")}, QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const QString path = files.next();
            const QString relative = QDir(m_root).relativeFilePath(path);
            if (relative == m_sourceRelative || relative.contains(QLatin1String("/build/")) || relative.contains(QLatin1String("/."))) continue;
            QFile file(path);
            if (file.size() > 20 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) continue;
            if (matches(path, QString::fromUtf8(file.readAll()))) return true;
        }
        return false;
    }

    std::optional<ProposalLinkIssue> sourceLink(const QString &name, const QString &selector, const QString &label, const QString &writtenLabel,
                                                const QString &text, bool cross) const
    {
        const QString writtenTarget = cross ? selector : selector.mid(m_owner.size() + 1);
        const QString link = form(name, writtenTarget, writtenLabel, text);
        const auto normalized = normalizeSelector(selector);
        if (!normalized)
            return issue(QStringLiteral("invalid_source_target"),
                         !cross ? QStringLiteral("%1 does not name a source document: give its path in this folder, ending in .tex, .md or .markdown.").arg(link)
                                : QStringLiteral("%1 does not name a source document: give docs-<folder>/<path ending in .tex, .md or .markdown>.").arg(link));
        // A Markdown link without a fragment opens the document start.
        if (!(m_markdown && writtenLabel.isEmpty() && label == documentStart) && !sourceLabel(writtenLabel))
            return issue(QStringLiteral("invalid_label"), QStringLiteral("%1 has an invalid label.").arg(link));
        const QString owner = SourceOwner::of(*normalized);
        const QString relative = SourceOwner::relative(*normalized);
        if (cross && owner == m_owner) {
            const QString suggestion = form(QStringLiteral("SourceLink"), relative, writtenLabel, text);
            return issue(QStringLiteral("cross_link_same_project"), QStringLiteral("%1 names this document's own folder; within a folder use %2.").arg(link, suggestion), suggestion);
        }
        std::optional<Publisher::Configuration> configuration;
        try {
            configuration = Publisher::findConfiguration(Py::Path(m_root), owner);
        } catch (const Py::Exception &) {
            return std::nullopt;
        }
        if (!configuration)
            return issue(QStringLiteral("unknown_project_folder"), QStringLiteral("%1: %2 is not a folder of any Project, so the link would stay inactive.").arg(link, owner));
        QList<Publisher::ProjectDocument> documents;
        try {
            documents = Publisher::projectDocuments(Py::Path(m_root) / owner, true);
        } catch (const Py::Exception &) {
            return std::nullopt;
        }
        QList<Publisher::ProjectDocument> matches;
        for (const auto &document : std::as_const(documents))
            if (document.source == relative) matches.append(document);
        if (matches.isEmpty())
            for (const auto &document : std::as_const(documents))
                if (sourceToken(document.source) == relative) matches.append(document);
        if (matches.size() > 1)
            return issue(QStringLiteral("ambiguous_source_target"), QStringLiteral("%1: %2 matches more than one selected document; give its full path in the folder.").arg(link, relative));
        if (matches.isEmpty()) {
            // The same document in another folder: a Cross link reaches it.
            loadCatalog();
            QString suggestion;
            for (const LibrarySource &candidate : m_sources) {
                if (candidate.folder == owner || !candidate.included || !SourceOwner::isConnectedFolder(candidate.folder)
                    || (candidate.source != relative && sourceToken(candidate.source) != relative))
                    continue;
                const bool crossNeeded = candidate.folder != m_owner;
                suggestion = form(crossNeeded ? QStringLiteral("CrossSourceLink") : QStringLiteral("SourceLink"),
                                  crossNeeded ? candidate.folder + QLatin1Char('/') + candidate.source : candidate.source, writtenLabel, text);
                break;
            }
            return issue(QStringLiteral("source_not_selected"),
                         QStringLiteral("%1: %2 is not a selected source document of %3, so the link would stay inactive.%4")
                             .arg(link, relative, owner, suggestion.isEmpty() ? QString() : QStringLiteral(" It is in another folder; use ") + suggestion + QLatin1Char('.')),
                         suggestion);
        }
        if (!QFileInfo(absolute(owner + QLatin1Char('/') + matches.first().source)).isFile())
            return issue(QStringLiteral("source_unavailable"), QStringLiteral("%1: the source %2 does not exist.").arg(link, owner + QLatin1Char('/') + matches.first().source));
        if (label != documentStart && !labelDefined(owner, label.mid(sourceLabelPrefix.size()))) {
            const QString what = QStringLiteral("a \\SourceLabel{%1} (Markdown: {#source-label:%1})").arg(label.mid(sourceLabelPrefix.size()));
            return issue(QStringLiteral("label_not_found"),
                         QStringLiteral("%1: no source of %2 defines %3; add the label at the destination first.").arg(link, owner, what));
        }
        return std::nullopt;
    }

    // The selected source document (<owner>/<path>) a retired link's
    // <owner>/<path> selector named: its exact path or the tex/source/<name>
    // alias, as target_for_selector resolved it.
    std::optional<QString> retiredTarget(const QString &selector) const
    {
        const auto normalized = normalizeSelector(selector);
        if (!normalized) return std::nullopt;
        const QString owner = SourceOwner::of(*normalized);
        const QString relative = SourceOwner::relative(*normalized);
        QList<Publisher::ProjectDocument> documents;
        try {
            if (!Publisher::findConfiguration(Py::Path(m_root), owner)) return std::nullopt;
            documents = Publisher::projectDocuments(Py::Path(m_root) / owner, true);
        } catch (const Py::Exception &) {
            return std::nullopt;
        }
        QStringList matches;
        for (const auto &document : std::as_const(documents))
            if (document.source == relative) matches.append(document.source);
        if (matches.isEmpty())
            for (const auto &document : std::as_const(documents))
                if (sourceToken(document.source) == relative) matches.append(document.source);
        if (matches.size() != 1) return std::nullopt;
        return owner + QLatin1Char('/') + matches.first();
    }

    // Whether the source at path (relative to the docs root) has \label{name}.
    bool hasTexLabel(const QString &path, const QString &name) const
    {
        QString content = m_updatedSource;
        if (path != m_sourceRelative) {
            QFile file(absolute(path));
            if (!file.open(QIODevice::ReadOnly)) return false;
            content = QString::fromUtf8(file.readAll());
        }
        return QRegularExpression(QStringLiteral("\\\\label\\s*\\{\\s*%1\\s*\\}").arg(QRegularExpression::escape(name))).match(content).hasMatch();
    }

    // \TexLink{<owner>/<path>}{label}{text} and \CrossTexLink are retired: the
    // build leaves them inactive and reports them. The same link is now
    // \SourceLink{<path in this folder>}{label}{text} or
    // \CrossSourceLink{docs-<folder>/<path>}{label}{text}, whose destination
    // defines \SourceLabel{label}.
    std::optional<ProposalLinkIssue> retiredLink(const Publisher::LinkCommand &link) const
    {
        const QString selector = link.arguments.value(0);
        const QString label = link.arguments.value(1);
        const QString text = link.arguments.value(2);
        const QString retired = QStringLiteral("%1 would stay inactive: \\%2 is retired").arg(link.text, link.name);
        const QString general = QStringLiteral("%1; link a source document of this folder with \\SourceLink{<path in the folder>}{label}{text} "
                                               "or one of another folder with \\CrossSourceLink{docs-<folder>/<path>}{label}{text}, "
                                               "and mark the destination with \\SourceLabel{label}.")
                                    .arg(retired);
        const std::optional<QString> target = link.arguments.size() < 3 ? std::optional<QString>() : retiredTarget(selector);
        if (!target) {
            const QString why = link.arguments.size() < 3 ? QString()
                                                           : QStringLiteral(" %1 is not a selected source document of any Project.").arg(selector);
            return issue(QStringLiteral("retired_link"), general + why);
        }
        const QString owner = SourceOwner::of(*target);
        const QString relative = SourceOwner::relative(*target);
        if (!sourceLabel(label))
            return issue(QStringLiteral("retired_link"),
                         general + QStringLiteral(" %1 is no valid Source label (letters, digits, dots, underscores, colons, hyphens, starting with a letter or digit); "
                                                  "give the destination in %2 a \\SourceLabel with a valid name and link to that.")
                                       .arg(label, *target));
        const QString suggestion = owner == m_owner ? command(QStringLiteral("SourceLink"), relative, label, text)
                                                    : command(QStringLiteral("CrossSourceLink"), *target, label, text);
        if (label == documentStart || labelDefined(owner, label)) return issue(QStringLiteral("retired_link"), QStringLiteral("%1; use %2.").arg(retired, suggestion), suggestion);
        const QString texLabel = hasTexLabel(*target, label) ? QStringLiteral(" (it has only \\label{%1})").arg(label) : QString();
        return issue(QStringLiteral("retired_link"),
                     QStringLiteral("%1; use %2 and mark the target with \\SourceLabel{%3} in %4%5: the Source label must exist before the link resolves.")
                         .arg(retired, suggestion, label, *target, texLabel),
                     suggestion);
    }

    // The Source labels a source document defines (the text this request writes for its own).
    QStringList definedLabels(const QString &path) const
    {
        QString content = m_updatedSource;
        if (path != m_sourceRelative) {
            QFile file(absolute(path));
            if (file.size() > 20 * 1024 * 1024 || !file.open(QIODevice::ReadOnly)) return {};
            content = QString::fromUtf8(file.readAll());
        }
        const QString suffix = QFileInfo(path).suffix().toLower();
        static const QRegularExpression tex(QStringLiteral("\\\\SourceLabel\\s*\\{\\s*([A-Za-z0-9][A-Za-z0-9_.:-]*)\\s*\\}"));
        static const QRegularExpression markdown(QStringLiteral("\\{\\s*#source-label:([A-Za-z0-9][A-Za-z0-9_.:-]*)(?=[\\s}])"));
        QStringList labels;
        for (auto it = (suffix == QLatin1String("tex") ? tex : markdown).globalMatch(content); it.hasNext();) {
            const QString label = it.next().captured(1);
            if (!labels.contains(label)) labels.append(label);
        }
        return labels;
    }

    QString m_root;
    QString m_owner;
    QString m_sourceRelative;
    QString m_updatedSource;
    QStringList m_serverOrigins;
    bool m_markdown = false;
    mutable bool m_catalogLoaded = false;
    mutable QList<LibraryPdf> m_pdfs;
    mutable QList<LibrarySource> m_sources;
    mutable QHash<QString, std::optional<QHash<QString, int>>> m_anchors;
    mutable QHash<QString, int> m_pageCounts;
    mutable QHash<QString, bool> m_destinations;
};

// A relative Markdown link. A link to another document means a LaTeX link
// command (Publisher::MarkdownLinks) and is checked as that command, with the
// problem and suggestion written as Markdown links. Any other relative link is
// resolved against the source's own path in the folder: the renderer leaves a
// dead link when nothing is there, and an image that is missing fails the
// document.
std::optional<ProposalLinkIssue> Checker::markdownLink(const QString &target, bool image, const QString &text) const
{
    const QString link = QStringLiteral("(%1)").arg(target);
    const QString folderRelative = m_sourceRelative.mid(m_owner.size() + 1);
    if (!image) {
        if (const auto document = Publisher::MarkdownLinks::classify(target, m_owner, folderRelative)) {
            if (!document->problem.isEmpty())
                return issue(QStringLiteral("invalid_markdown_link"),
                             QStringLiteral("The Markdown link %1 cannot link a document: %2. Within this folder give the document's relative path "
                                            "(other.md#source-label:name); for another source folder a relative path into it "
                                            "(../../docs-other/notes/y.md#source-label:name); for an imported PDF of any Project its bare "
                                            "Library filename (paper.pdf#anchor).")
                                 .arg(link, document->problem));
            std::optional<ProposalLinkIssue> problem;
            if (document->kind == QLatin1String("PdfLink")) {
                problem = pdfLink(document->target, document->anchor, text);
            } else if (document->kind == QLatin1String("CrossPdfLink")) {
                problem = crossPdfLink(document->target, document->anchor, text);
            } else {
                const bool cross = document->kind == QLatin1String("CrossSourceLink");
                const QString selector = cross ? document->target : m_owner + QLatin1Char('/') + document->target;
                const QString label = document->anchor.isEmpty() ? documentStart : sourceLabelPrefix + document->anchor;
                // The document itself, as this request writes it.
                if (!cross && !QFileInfo::exists(absolute(selector))) {
                    problem = issue(QStringLiteral("link_target_not_found"),
                                    QStringLiteral("The Markdown link %1 points to %2, which does not exist.").arg(link, selector));
                } else if (selector == m_sourceRelative) {
                    static const QString attribute = QStringLiteral("\\{\\s*#%1(?=[\\s}])");
                    if (!document->anchor.isEmpty() && !QRegularExpression(attribute.arg(QRegularExpression::escape(label))).match(m_updatedSource).hasMatch())
                        problem = issue(QStringLiteral("label_not_found"),
                                        QStringLiteral("The Markdown link %1: this document defines no {#%2}; add the Source label first.").arg(link, label));
                } else {
                    problem = sourceLink(document->kind, selector, label, document->anchor, text, cross);
                }
            }
            if (problem || !document->hasIgnoredFragment) return problem;
            const QString start = QStringLiteral("[%1](%2)").arg(text, Publisher::MarkdownLinks::markdownForm(document->kind, document->target, {}, m_owner, folderRelative));
            return issue(QStringLiteral("fragment_not_source_label"),
                         QStringLiteral("The Markdown link %1: #%2 is not a Source label, so the link would open the start of the document. Mark the "
                                        "heading with {#source-label:name} and link #source-label:name, or drop the fragment.")
                             .arg(link, document->ignoredFragment),
                         start);
        }
    }
    QString path = target.section(QLatin1Char('#'), 0, 0);
    const QString fragment = target.contains(QLatin1Char('#')) ? QUrl::fromPercentEncoding(target.section(QLatin1Char('#'), 1).toUtf8()) : QString();
    path = QUrl::fromPercentEncoding(path.toUtf8());
    const QString sourceDirectory = m_sourceRelative.section(QLatin1Char('/'), 0, -2);
    QString resolved = m_sourceRelative;
    if (!path.isEmpty()) {
        resolved = path.startsWith(QLatin1Char('/')) ? m_owner + path : QDir::cleanPath(sourceDirectory + QLatin1Char('/') + path);
        if (!QString(resolved + QLatin1Char('/')).startsWith(m_owner + QLatin1Char('/')) || resolved.startsWith(QLatin1String("../")))
            return issue(QStringLiteral("link_outside_project"), QStringLiteral("The Markdown %1 %2 leaves this document's folder; use a path inside %3.")
                                                                      .arg(image ? QStringLiteral("image") : QStringLiteral("link"), link, m_owner));
        if (!QFileInfo::exists(absolute(resolved)))
            return issue(image ? QStringLiteral("image_not_found") : QStringLiteral("link_target_not_found"),
                         QStringLiteral("The Markdown %1 %2 points to %3, which does not exist.").arg(image ? QStringLiteral("image") : QStringLiteral("link"), link, resolved));
    }
    if (fragment.startsWith(sourceLabelPrefix)) {
        QString content;
        if (resolved == m_sourceRelative) {
            content = m_updatedSource;
        } else if (QFile file(absolute(resolved)); file.open(QIODevice::ReadOnly)) {
            content = QString::fromUtf8(file.readAll());
        }
        if (!QRegularExpression(QStringLiteral("\\{\\s*#%1(?=[\\s}])").arg(QRegularExpression::escape(fragment))).match(content).hasMatch())
            return issue(QStringLiteral("label_not_found"),
                         QStringLiteral("The Markdown link %1: %2 defines no {#%3}; add the Source label at the destination first.").arg(link, resolved, fragment));
    }
    return std::nullopt;
}

// A link to the Thoughts server's "Open in Thoughts" page of a Library document
// (\href{https://<server>/open/document/<id>?page=N...}{text}, or the Markdown link to it, on
// the server this computer uses: Thoughts::isServerDocumentPageUrl). The build keeps it a web
// link, which opens a browser instead of the document; a Thoughts link opens the document in the
// app and on the phone, and the build checks it. The suggestion is the Thoughts link to the same
// document: \PdfLink{<pdfKey>}{page.N}{text} for an imported PDF (N the page the server page
// opens), for a note the \SourceLink or \CrossSourceLink of its source at document-start (in
// Markdown its relative path without a fragment): its start, since a page of a note has no
// link. A place in the note needs a Source label there, which the message says to propose.
std::optional<ProposalLinkIssue> Checker::serverPageLink(const QString &address, const QString &text) const
{
    const QUrl url(address, QUrl::TolerantMode);
    if (!Thoughts::isServerDocumentPageUrl(url, m_serverOrigins)) return std::nullopt;
    const QString reason = QStringLiteral("server_page_link");
    const QString written = m_markdown ? QStringLiteral("[%1](%2)").arg(text, address) : QStringLiteral("\\href{%1}{%2}").arg(address, text);
    const QString general = m_markdown
        ? QStringLiteral("%1 links the Thoughts server's \"Open in Thoughts\" page, which opens in a browser instead of Thoughts. A link to a Library "
                         "document must be a Thoughts link: an imported PDF by its bare Library filename ([text](pdfKey.pdf#page.N)), a note by its "
                         "relative path ([text](other.md) in this folder, [text](../../docs-<folder>/path.tex) in another, with "
                         "#source-label:name for a place in it).")
              .arg(written)
        : QStringLiteral("%1 links the Thoughts server's \"Open in Thoughts\" page, which opens in a browser instead of Thoughts. A link to a Library "
                         "document must be a Thoughts link: an imported PDF is linked with \\PdfLink{pdfKey}{page.N}{text} (pdfKey is its Library "
                         "filename without .pdf), a note with \\SourceLink{path}{label}{text} in this folder or "
                         "\\CrossSourceLink{docs-<folder>/path}{label}{text}, where label is document-start for its start or a \\SourceLabel{label} "
                         "the note defines.")
              .arg(written);
    const auto link = Thoughts::serverPageLink(url, m_serverOrigins);
    if (!link) return issue(reason, general + QStringLiteral(" This address names no document."));
    loadCatalog();
    for (const LibraryPdf &pdf : m_pdfs) {
        if (pdf.documentId != link->documentId) continue;
        if (!pdf.included)
            return issue(reason, general + QStringLiteral(" It is the PDF %1 of Project %2, which is not selected for building there, so no link can reach it.")
                                               .arg(pdf.filename, pdf.project));
        QString anchor = QStringLiteral("page.%1").arg(link->targetPage());
        QString note;
        if (const auto problem = anchorProblem(absolute(pdf.path), anchor)) {
            anchor = documentStart;
            note = QStringLiteral(" (%1 The link opens its start.)").arg(*problem);
        }
        const QString suggestion = form(QStringLiteral("PdfLink"), stem(pdf.filename), anchor, text);
        return issue(reason, general + QStringLiteral(" It is the PDF %1 of Project %2: use %3.%4").arg(pdf.filename, pdf.project, suggestion, note), suggestion);
    }
    for (const LibrarySource &source : m_sources) {
        if (source.documentId != link->documentId) continue;
        const QString path = source.folder + QLatin1Char('/') + source.source;
        if (!source.included)
            return issue(reason, general + QStringLiteral(" It is the note %1 of Project %2, which is not selected for building there, so no link can reach it.")
                                               .arg(path, source.project));
        const bool here = source.folder == m_owner;
        const QString name = here ? QStringLiteral("SourceLink") : QStringLiteral("CrossSourceLink");
        const QString target = here ? source.source : path;
        const QString labelForm = source.source.endsWith(QLatin1String(".tex"), Qt::CaseInsensitive) ? QStringLiteral("\\SourceLabel{name}")
                                                                                                      : QStringLiteral("{#source-label:name} on a heading");
        const QString suggestion = form(name, target, documentStart, text);
        QString message = general + QStringLiteral(" It is the note %1 of Project %2: use %3 for its start. For a specific place in it, the note needs a Source "
                                                   "label there (%4; propose it if the note has none) and the link %5.")
                                        .arg(path, source.project, suggestion, labelForm,
                                             m_markdown ? QStringLiteral("adds #source-label:name") : QStringLiteral("names that label instead of document-start"));
        if (const QStringList labels = definedLabels(path); !labels.isEmpty())
            message += QStringLiteral(" Its Source labels: %1.").arg(labels.mid(0, 10).join(QStringLiteral(", ")));
        return issue(reason, message, suggestion);
    }
    return issue(reason, general + QStringLiteral(" No Project of this Library has the document %1; link its Library PDF or source instead.").arg(link->documentId));
}

// The address an \href names, as hyperref reads it: TeX's escaped specials are the characters.
QString hrefAddress(const QString &argument)
{
    static const QRegularExpression escaped(QStringLiteral("\\\\([#%&_~$])"));
    return QString(argument).replace(escaped, QStringLiteral("\\1")).trimmed();
}

// The link text of a Markdown link as written ([text](target)).
QString markdownLinkText(const QString &link)
{
    static const QRegularExpression label(QStringLiteral("^!?\\[(.*)\\][(:]"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::InvertedGreedinessOption);
    const auto match = label.match(link);
    return match.hasMatch() ? match.captured(1) : QString();
}

struct MarkdownLink {
    QString target;
    bool image = false;
    int line = 0;
    QString text;    // the link as written
    QString visible; // its link text
};

// Inline Markdown links, images and reference definitions outside code, with relative targets.
QList<MarkdownLink> markdownLinks(const QString &markdown)
{
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    static const QRegularExpression label(QStringLiteral("^!?\\[(.*)\\][(:]"), QRegularExpression::DotMatchesEverythingOption | QRegularExpression::InvertedGreedinessOption);
    QList<MarkdownLink> links;
    for (const auto &span : Publisher::MarkdownLinks::linkSpans(markdown)) {
        const QString &target = span.href;
        if (target.isEmpty() || target.startsWith(QLatin1String("//")) || scheme.match(target).hasMatch()) continue;
        if (target.startsWith(QLatin1Char('#')) && !target.startsWith(QStringLiteral("#") + sourceLabelPrefix)) continue;
        const auto visible = label.match(span.text);
        links.append({target, span.image, span.line, span.text, visible.hasMatch() ? visible.captured(1) : QString()});
    }
    return links;
}
}

namespace {
// thoughts-html: links the replacement adds must name an HTML page of the note's own folder: a file
// there, or a page its list names that Sync has not installed yet (thoughts-server
// protocol/html-pages.md 3). A PDF carries only the pages of its folder.
QList<ProposalLinkIssue> htmlPageLinkIssues(const QString &root, const QString &owner, const QString &original, const QString &replacement)
{
    static const QRegularExpression link(QStringLiteral("thoughts-html:([^\\s{}()<>\\[\\]\"'\\\\]+)"));
    QSet<QString> before;
    for (auto it = link.globalMatch(original); it.hasNext();) before.insert(QUrl::fromPercentEncoding(it.next().captured(1).toUtf8()));
    // A docs-* folder and a Project's default folder both have pages (thoughts-server protocol/html-pages.md 1).
    const bool connected = SourceOwner::isOwner(owner);
    const QStringList listed = connected ? Publisher::listedHtmlPages(Py::Path(root), owner) : QStringList();
    const auto exists = [&](const QString &path) {
        return Publisher::isHtmlSourcePath(path, owner)
            && (QFileInfo(QDir(root).filePath(path)).isFile() || listed.contains(path));
    };
    QList<ProposalLinkIssue> issues;
    QSet<QString> seen;
    for (auto it = link.globalMatch(replacement); it.hasNext();) {
        const QRegularExpressionMatch match = it.next();
        const QString target = QUrl::fromPercentEncoding(match.captured(1).toUtf8());
        if (before.contains(target) || seen.contains(target)) continue;
        seen.insert(target);
        if (connected && exists(target)) continue;
        ProposalLinkIssue issue;
        issue.command = QStringLiteral("thoughts-html");
        issue.text = QStringLiteral("thoughts-html:") + target;
        issue.line = int(replacement.left(match.capturedStart()).count(QLatin1Char('\n'))) + 1;
        const QString own = owner + QStringLiteral("/tex/support/html/") + target.section(QLatin1Char('/'), -1);
        if (connected && own != target && exists(own)) issue.suggestion = QStringLiteral("thoughts-html:") + own;
        if (!connected) {
            issue.reason = QStringLiteral("html_page_other_folder");
            issue.message = QStringLiteral("%1: only notes of a Project's source folder can link HTML pages, and only pages of their own folder.").arg(issue.text);
        } else if (SourceOwner::of(target) != owner) {
            issue.reason = QStringLiteral("html_page_other_folder");
            issue.message = QStringLiteral("%1 names a page of another folder; a note's PDF carries only the pages of its own folder (%2). "
                                           "Put the page in %2/tex/support/html/ first.").arg(issue.text, owner);
        } else {
            issue.reason = QStringLiteral("html_page_not_found");
            issue.message = QStringLiteral("%1: %2 has no such HTML page. Save the page as %2/tex/support/html/<name>.html first, then link it.")
                                .arg(issue.text, owner);
        }
        issues.append(issue);
    }
    return issues;
}
}

QJsonObject Thoughts::ProposalLinkIssue::toJson() const
{
    QJsonObject result {{QStringLiteral("reason"), reason}, {QStringLiteral("command"), command}, {QStringLiteral("text"), text},
                        {QStringLiteral("line"), line}, {QStringLiteral("message"), message}};
    if (!suggestion.isEmpty()) result.insert(QStringLiteral("suggestion"), suggestion);
    return result;
}

QList<Thoughts::ProposalLinkIssue> Thoughts::introducedLinkIssues(const QString &docsRoot, const QString &sourcePath, const QString &original,
                                                                  const QString &replacement, const QString &updatedSource)
{
    const QString root = QFileInfo(docsRoot).canonicalFilePath();
    const QString relative = QDir(root).relativeFilePath(sourcePath);
    const QString owner = SourceOwner::of(relative);
    if (root.isEmpty() || owner.isEmpty()) return {};
    try {
        if (!Publisher::findConfiguration(Py::Path(root), owner)) return {};
    } catch (const Py::Exception &) {
        return {};
    }
    const Checker checker(root, owner, relative, updatedSource, Thoughts::ownServerOrigins());
    QList<ProposalLinkIssue> issues = htmlPageLinkIssues(root, owner, original, replacement);
    const QString suffix = QFileInfo(sourcePath).suffix().toLower();
    if (suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) {
        QSet<QString> existing;
        for (const MarkdownLink &link : markdownLinks(original)) existing.insert(link.target);
        for (const MarkdownLink &link : markdownLinks(replacement)) {
            if (existing.contains(link.target)) continue;
            if (auto problem = checker.markdownLink(link.target, link.image, link.visible)) {
                problem->command = QStringLiteral("markdown_link");
                problem->text = link.text;
                problem->line = link.line;
                issues.append(*problem);
            }
        }
        // Web links to the Thoughts server's own document pages.
        QSet<QString> existingWeb;
        for (const auto &span : Publisher::MarkdownLinks::linkSpans(original)) existingWeb.insert(span.href);
        for (const auto &span : Publisher::MarkdownLinks::linkSpans(replacement)) {
            if (span.image || existingWeb.contains(span.href)) continue;
            if (auto problem = checker.serverPageLink(span.href, markdownLinkText(span.text))) {
                problem->command = QStringLiteral("markdown_link");
                problem->text = span.text;
                problem->line = span.line;
                issues.append(*problem);
            }
        }
        return issues;
    }
    // The link's target and anchor identify it; its visible text may change.
    const auto identity = [](const Publisher::LinkCommand &link) { return (QStringList{link.name} + link.arguments.mid(0, 2)).join(QChar(0x1f)); };
    QSet<QString> existing;
    for (const auto &link : Publisher::activeLinkCommands(original)) existing.insert(identity(link));
    for (const auto &link : Publisher::activeLinkCommands(replacement)) {
        if (link.arguments.size() >= 2 && existing.contains(identity(link))) continue;
        if (auto problem = checker.check(link)) {
            problem->command = link.name;
            problem->text = link.text;
            problem->line = link.line;
            issues.append(*problem);
        }
    }
    // \href to the Thoughts server's own document pages, by the address alone.
    const QStringList href{QStringLiteral("href")};
    QSet<QString> existingAddresses;
    for (const auto &link : Publisher::activeCommands(original, href, 2))
        if (!link.arguments.isEmpty()) existingAddresses.insert(hrefAddress(link.arguments.first()));
    for (const auto &link : Publisher::activeCommands(replacement, href, 2)) {
        if (link.arguments.isEmpty()) continue;
        const QString address = hrefAddress(link.arguments.first());
        if (existingAddresses.contains(address)) continue;
        if (auto problem = checker.serverPageLink(address, link.arguments.value(1))) {
            problem->command = link.name;
            problem->text = link.text;
            problem->line = link.line;
            issues.append(*problem);
        }
    }
    return issues;
}
