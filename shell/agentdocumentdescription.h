// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "../part/projectconfiguration.h"
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QPair>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>

namespace Thoughts
{
// The Library project and document whose PDF (or attachment) is @p filename; empty if none.
inline QPair<QJsonObject, QJsonObject> agentProjectDocument(const QJsonArray &projects, const QString &filename)
{
    for (const auto &projectValue : projects) {
        const auto candidate = projectValue.toObject();
        for (const auto &documentValue : candidate.value(QStringLiteral("documents")).toArray()) {
            const auto entry = documentValue.toObject();
            if (entry.value(QStringLiteral("filename")).toString() == filename
                || entry.value(QStringLiteral("attachment_filename")).toString() == filename)
                return {candidate, entry};
        }
    }
    return {};
}

// "LaTeX" or "Markdown" for a source path, else empty.
inline QString agentSourceFormat(const QString &source)
{
    const QString suffix = QFileInfo(source).suffix().toLower();
    return suffix == QLatin1String("tex") ? QStringLiteral("LaTeX")
        : (suffix == QLatin1String("md") || suffix == QLatin1String("markdown")) ? QStringLiteral("Markdown") : QString();
}

// The document's placeholders for the review card prompts (agentprompts.h): document_name,
// project, source, source_name, source_format, pdf, project_file, document_id. The same
// values the lines of agentDocumentDescription below show.
inline QHash<QString, QString> agentDocumentFields(const QString &docsRoot, const QJsonArray &projects, const QString &pdfPath, const QString &documentName)
{
    const QString filename = QFileInfo(pdfPath).fileName();
    const auto [project, document] = agentProjectDocument(projects, filename);
    QHash<QString, QString> fields;
    fields.insert(QStringLiteral("document_name"), documentName.isEmpty() ? filename : documentName);
    fields.insert(QStringLiteral("project"), project.value(QStringLiteral("name")).toString().trimmed());
    const QString source = document.value(QStringLiteral("source_file")).toString();
    if (!source.isEmpty()) {
        fields.insert(QStringLiteral("source"), QDir(docsRoot).filePath(source));
        fields.insert(QStringLiteral("source_name"), QFileInfo(source).fileName());
        const QString format = agentSourceFormat(source);
        // Another kind of source still names its format, so its Source line stays.
        fields.insert(QStringLiteral("source_format"), format.isEmpty() ? QFileInfo(source).suffix() : format);
    }
    fields.insert(QStringLiteral("pdf"), pdfPath);
    const QString projectId = project.value(QStringLiteral("id")).toString();
    if (!projectId.isEmpty()) {
        const QString configuration = ProjectConfiguration::path(docsRoot, projectId);
        if (QFileInfo::exists(configuration)) fields.insert(QStringLiteral("project_file"), configuration);
    }
    fields.insert(QStringLiteral("document_id"), document.value(QStringLiteral("document_id")).toString());
    return fields;
}

// "Copy for agent": what a coding agent needs to find the document a reader
// tab shows, as plain text to paste into Codex or Claude.
inline QString agentDocumentDescription(const QString &docsRoot, const QJsonArray &projects, const QString &pdfPath, const QString &documentName,
                                        const QString &selectedText)
{
    const QString filename = QFileInfo(pdfPath).fileName();
    const auto [project, document] = agentProjectDocument(projects, filename);

    QStringList lines {QStringLiteral("Thoughts document: \"%1\"").arg(documentName.isEmpty() ? filename : documentName)};
    const QString projectName = project.value(QStringLiteral("name")).toString().trimmed();
    if (!projectName.isEmpty()) lines.append(QStringLiteral("Project: %1").arg(projectName));
    const QString source = document.value(QStringLiteral("source_file")).toString();
    if (!source.isEmpty()) {
        const QString format = agentSourceFormat(source);
        lines.append(QStringLiteral("Source: %1%2").arg(QDir(docsRoot).filePath(source), format.isEmpty() ? QString() : QStringLiteral("  (%1)").arg(format)));
        // Proposals let the user accept or reject each change in Thoughts.
        lines.append(QStringLiteral("To change the text, create proposals with the Thoughts create_proposals tool (several edits at once) or "
                                    "`thoughtsctl create-proposals`; don't edit the source directly unless asked, and never edit the PDF."));
    }
    // The page on screen is left out: the user asked for the text without it (12 Nov).
    lines.append(QStringLiteral("Built PDF: %1").arg(pdfPath));
    const QString projectId = project.value(QStringLiteral("id")).toString();
    if (!projectId.isEmpty()) {
        const QString configuration = ProjectConfiguration::path(docsRoot, projectId);
        if (QFileInfo::exists(configuration)) lines.append(QStringLiteral("Project file: %1").arg(configuration));
    }
    const QString documentId = document.value(QStringLiteral("document_id")).toString();
    if (!documentId.isEmpty()) lines.append(QStringLiteral("Document ID: %1").arg(documentId));
    QString selection = selectedText;
    selection.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    selection = selection.trimmed();
    if (!selection.isEmpty()) lines.append(QStringLiteral("Selected text: \"%1\"").arg(selection));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

// "Copy for agent" on an HTML page's tab, after its document's description: the page (its title,
// its source in the Library, the part of it on screen) and the text selected in it. A live page
// (thoughts-server protocol/html-live-pages.md, @p liveAddress) says so and gives its address: what
// it shows comes from there, and its source is only the stub that names it.
inline QString agentHtmlPageDescription(const QString &docsRoot, const QString &path, const QString &title, const QString &hash, const QString &selectedText,
                                        const QString &liveAddress = QString())
{
    QStringList lines{QStringLiteral("HTML page: \"%1\"").arg(title.isEmpty() ? QFileInfo(path).fileName() : title)};
    if (!liveAddress.isEmpty()) {
        lines.append(QStringLiteral("Live page: it shows %1 as that address serves it now (loaded from the network on this device), not HTML kept in the Library.")
                         .arg(liveAddress));
    }
    const QString source = QDir(docsRoot).filePath(path);
    lines.append(QStringLiteral("Page source: %1%2").arg(QFileInfo::exists(source) ? source : path,
                                                         liveAddress.isEmpty() ? QString() : QStringLiteral("  (the live page's stub; a snapshot or add_html_page with replace makes it an embedded page)")));
    if (!hash.isEmpty() && hash != QLatin1String("#")) lines.append(QStringLiteral("Part of the page on screen: %1").arg(hash));
    QString selection = selectedText;
    selection.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
    selection = selection.trimmed();
    if (!selection.isEmpty()) lines.append(QStringLiteral("Selected text: \"%1\"").arg(selection));
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}
}
