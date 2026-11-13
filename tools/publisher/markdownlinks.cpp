// SPDX-License-Identifier: GPL-2.0-or-later
// A port of thoughts_server.markdown_links (classify, markdown_form,
// link_spans); keep the two in step.
#include "markdownlinks.h"

#include "pyurl.h"
#include "sourceowner.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

namespace Thoughts::Publisher::MarkdownLinks
{
namespace
{

const QString sourceLabelPrefix = QStringLiteral("source-label:");
const QString documentStart = QStringLiteral("document-start");
const QString hrefSafe = QStringLiteral("/~!$&'()*+,;=:@-._");

bool pdfIdentifier(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.-]*$"));
    return pattern.match(value).hasMatch();
}

bool sourceLabel(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9_.:-]*$"));
    return pattern.match(value).hasMatch();
}

bool sourceSuffix(const QString &path)
{
    const QString lower = path.toLower();
    return lower.endsWith(QLatin1String(".md")) || lower.endsWith(QLatin1String(".markdown")) || lower.endsWith(QLatin1String(".tex"));
}

QString lastSegment(const QString &path)
{
    return path.section(QLatin1Char('/'), -1);
}

DocumentLink invalid(const QString &problem)
{
    DocumentLink link;
    link.problem = problem;
    return link;
}

DocumentLink sourceAnchor(const QString &kind, const QString &target, const std::optional<QString> &fragment)
{
    DocumentLink link{kind, target, {}, {}, false, {}};
    if (!fragment || fragment->isEmpty()) return link;
    if (fragment->startsWith(sourceLabelPrefix)) {
        const QString label = fragment->mid(sourceLabelPrefix.size());
        if (!sourceLabel(label)) return invalid(QStringLiteral("invalid Source label '%1'").arg(label));
        link.anchor = label;
        return link;
    }
    link.ignoredFragment = *fragment;
    link.hasIgnoredFragment = true;
    return link;
}

DocumentLink pdfAnchor(const QString &kind, const QString &target, const QString &key, const std::optional<QString> &fragment)
{
    if (!pdfIdentifier(key))
        return invalid(QStringLiteral("invalid PDF key '%1'; a Library PDF filename uses letters, digits, dots, underscores or hyphens").arg(key));
    const QString anchor = fragment && !fragment->isEmpty() ? *fragment : documentStart;
    if (!pdfIdentifier(anchor)) return invalid(QStringLiteral("invalid PDF anchor '%1'; use document-start or a doc-anchor: name").arg(anchor));
    return DocumentLink{kind, target, anchor, {}, false, {}};
}

// posixpath.relpath(target, start) for docs-root-relative, normalized paths.
QString relativePath(const QString &target, const QString &start)
{
    const QStringList to = target.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    const QStringList from = start.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    qsizetype common = 0;
    while (common < to.size() && common < from.size() && to.at(common) == from.at(common)) ++common;
    QStringList parts;
    for (qsizetype index = common; index < from.size(); ++index) parts.append(QStringLiteral(".."));
    parts.append(to.mid(common));
    return parts.isEmpty() ? QStringLiteral(".") : parts.join(QLatin1Char('/'));
}

QString relativeHref(const QString &target, const QString &folder, const QString &source)
{
    return relativePath(target, Py::posixDirname(folder + QLatin1Char('/') + source));
}

}

QString resolvePath(const QString &folder, const QString &source, const QString &path)
{
    const QString directory = Py::posixDirname(folder + QLatin1Char('/') + source);
    const QString joined = path.isEmpty() ? folder + QLatin1Char('/') + source : Py::posixNormpath(Py::posixJoin(directory, path));
    if (joined == QLatin1String("..") || joined.startsWith(QLatin1String("../")) || joined.startsWith(QLatin1Char('/'))) return {};
    return joined;
}

std::optional<DocumentLink> classify(const QString &href, const QString &folder, const QString &source)
{
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]*:"));
    if (href.isEmpty() || href.startsWith(QLatin1Char('#')) || href.startsWith(QLatin1Char('/')) || scheme.match(href).hasMatch()) return std::nullopt;
    const Py::SplitResult parts = Py::urlsplit(href);
    if (!parts.scheme.isEmpty() || !parts.netloc.isEmpty()) return std::nullopt;
    const QString path = Py::unquote(parts.path);
    const std::optional<QString> fragment = href.contains(QLatin1Char('#')) ? std::optional<QString>(Py::unquote(parts.fragment)) : std::nullopt;
    const bool query = !parts.query.isEmpty();
    if (path.isEmpty()) return std::nullopt;
    const bool pdf = path.toLower().endsWith(QLatin1String(".pdf"));
    if (!path.contains(QLatin1Char('/')) && pdf) {
        if (query) return invalid(QStringLiteral("a PDF link takes no query"));
        return pdfAnchor(QStringLiteral("PdfLink"), path.chopped(4), path.chopped(4), fragment);
    }
    const QString resolved = resolvePath(folder, source, path);
    if (resolved.isEmpty()) return invalid(QStringLiteral("the path leaves the Library"));
    if (resolved.startsWith(folder + QLatin1Char('/'))) {
        if (!sourceSuffix(path)) return std::nullopt;
        if (query) return invalid(QStringLiteral("a link to a document takes no query"));
        return sourceAnchor(QStringLiteral("SourceLink"), resolved.mid(folder.size() + 1), fragment);
    }
    const QString owner = SourceOwner::of(resolved);
    if (owner.isEmpty() || resolved == owner)
        return invalid(QStringLiteral("a link that leaves this folder must name a document of another source folder or an imported PDF in the root of a docs-* folder"));
    if (query) return invalid(QStringLiteral("a link to a document takes no query"));
    const QString relative = resolved.mid(owner.size() + 1);
    if (pdf && !relative.contains(QLatin1Char('/')) && SourceOwner::isConnectedFolder(owner)) {
        const QString key = relative.chopped(4);
        return pdfAnchor(QStringLiteral("CrossPdfLink"), owner + QLatin1Char('/') + key, key, fragment);
    }
    if (sourceSuffix(path)) return sourceAnchor(QStringLiteral("CrossSourceLink"), resolved, fragment);
    return invalid(QStringLiteral("a link that leaves this folder must name a source document (.md, .markdown, .tex) or an imported PDF in the root of a docs-* folder"));
}

QString markdownForm(const QString &kind, const QString &target, const QString &anchor, const QString &folder, const QString &source)
{
    QString path;
    QString fragment;
    if (kind == QLatin1String("PdfLink")) {
        path = target + QStringLiteral(".pdf");
        fragment = anchor;
    } else if (kind == QLatin1String("CrossPdfLink")) {
        path = relativeHref(target.section(QLatin1Char('/'), 0, 0), folder, source) + QLatin1Char('/') + target.section(QLatin1Char('/'), 1) + QStringLiteral(".pdf");
        fragment = anchor;
    } else {
        path = relativeHref(kind == QLatin1String("SourceLink") ? folder + QLatin1Char('/') + target : target, folder, source);
        fragment = anchor.isEmpty() || anchor == documentStart ? QString() : sourceLabelPrefix + anchor;
    }
    if (fragment == documentStart) fragment.clear();
    return Py::quote(path, hrefSafe) + (fragment.isEmpty() ? QString() : QLatin1Char('#') + fragment);
}

QList<LinkSpan> linkSpans(const QString &markdown)
{
    QString visible = markdown;
    // Blank code so positions (and line numbers) stay put.
    static const QRegularExpression fenced(QStringLiteral("(^|\\n) {0,3}(`{3,}|~{3,})[^\\n]*\\n.*?(\\n {0,3}\\2[`~]*[ \\t]*(?=\\n|$)|$)"),
                                           QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression inlineCode(QStringLiteral("(`+)(?:(?!\\1).)+?\\1"), QRegularExpression::DotMatchesEverythingOption);
    for (const QRegularExpression *pattern : {&fenced, &inlineCode}) {
        auto it = pattern->globalMatch(visible);
        QList<QPair<qsizetype, qsizetype>> ranges;
        while (it.hasNext()) {
            const auto match = it.next();
            ranges.append({match.capturedStart(), match.capturedEnd()});
        }
        for (const auto &[start, end] : std::as_const(ranges))
            for (qsizetype index = start; index < end; ++index)
                if (visible.at(index) != QLatin1Char('\n')) visible[index] = QLatin1Char(' ');
    }
    static const QRegularExpression inlineLink(QStringLiteral("(!?)\\[(?:[^\\[\\]\\n]|\\[[^\\[\\]\\n]*\\])*\\]\\(\\s*"
                                                              "(<[^<>\\n]*>|(?:[^\\s()<>\\\\]|\\\\.|\\((?:[^\\s()<>\\\\]|\\\\.)*\\))+)"
                                                              "(?:\\s+(?:\"[^\"\\n]*\"|'[^'\\n]*'|\\([^()\\n]*\\)))?\\s*\\)"));
    static const QRegularExpression reference(QStringLiteral("^ {0,3}\\[[^\\]\\n]+\\]:[ \\t]*(<[^<>\\n]*>|\\S+)"), QRegularExpression::MultilineOption);
    QList<LinkSpan> spans;
    const auto collect = [&](const QRegularExpression &pattern, int imageGroup, int group) {
        auto it = pattern.globalMatch(visible);
        while (it.hasNext()) {
            const auto match = it.next();
            LinkSpan span;
            span.start = match.capturedStart(group);
            span.end = match.capturedEnd(group);
            span.href = markdown.mid(span.start, span.end - span.start);
            if (span.href.startsWith(QLatin1Char('<')) && span.href.endsWith(QLatin1Char('>'))) {
                ++span.start;
                --span.end;
                span.href = span.href.mid(1, span.href.size() - 2);
            }
            span.image = imageGroup > 0 && !match.captured(imageGroup).isEmpty();
            span.line = 1 + int(QStringView(markdown).left(match.capturedStart()).count(QLatin1Char('\n')));
            span.text = markdown.mid(match.capturedStart(), match.capturedLength());
            spans.append(span);
        }
    };
    collect(inlineLink, 1, 2);
    collect(reference, 0, 1);
    std::sort(spans.begin(), spans.end(), [](const LinkSpan &a, const LinkSpan &b) { return a.start < b.start; });
    return spans;
}

QStringList pdfLinkKeys(const QString &markdown)
{
    QStringList keys;
    for (const LinkSpan &span : linkSpans(markdown)) {
        if (span.image) continue;
        // The bare-filename form does not depend on where the document is.
        const auto link = classify(span.href, QStringLiteral("docs-x"), QStringLiteral("x.md"));
        if (link && link->kind == QLatin1String("PdfLink") && !keys.contains(link->target)) keys.append(link->target);
    }
    return keys;
}

}
