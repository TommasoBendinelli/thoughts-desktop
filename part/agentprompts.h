// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

// The review card ⋮ menu's "Copy for agent" text: a template the user can edit in Settings >
// Agent prompts (saved by agentpromptsettings.h), with {placeholders} for the card and its
// document. The phone has the same default, placeholders and rules (AgentPromptTemplates.kt).
// "Copy for physical studying" is gone: the phone's study mode prompt does that job.
namespace Thoughts::AgentPrompts
{
enum class Kind { Agent };

inline QString defaultTemplate(Kind kind)
{
    if (kind == Kind::Agent)
        return QStringLiteral(
            "Thoughts document: \"{document_name}\"\n"
            "Project: {project}\n"
            "Source: {source}  ({source_format})\n"
            "To change {source_name}, create proposals with the Thoughts create_proposals tool (several edits at once) or "
            "`thoughtsctl create-proposals`; don't edit the source directly unless asked, and never edit the PDF.\n"
            "Built PDF: {pdf}  ({pdf_pages} pages)\n"
            "Project file: {project_file}\n"
            "Document ID: {document_id}\n"
            "Review card: {card_id}\n"
            "Question: \"{question}\"\n"
            "Note: \"{note}\"\n"
            "Answer pages: {answer_pages}\n"
            "Answer source: {answer_source}\n"
            "{answer_feedback}\n"
            "{current_review}\n");
    return {};
}

struct Placeholder {
    QString name;
    QString description;
};

// Every placeholder, in the order Settings lists them.
inline QList<Placeholder> placeholders()
{
    return {
        {QStringLiteral("document_name"), QStringLiteral("the document's name, as its reader tab shows it")},
        {QStringLiteral("project"), QStringLiteral("the project's name")},
        {QStringLiteral("source"), QStringLiteral("the document's source file (full path)")},
        {QStringLiteral("source_name"), QStringLiteral("the source file's name, e.g. grpo.tex")},
        {QStringLiteral("source_format"), QStringLiteral("LaTeX or Markdown")},
        {QStringLiteral("pdf"), QStringLiteral("the built PDF (full path)")},
        {QStringLiteral("pdf_pages"), QStringLiteral("the built PDF's page count")},
        {QStringLiteral("project_file"), QStringLiteral("the project's thoughts-project.json")},
        {QStringLiteral("document_id"), QStringLiteral("the document's ID")},
        {QStringLiteral("card_id"), QStringLiteral("the review card's ID")},
        {QStringLiteral("question"), QStringLiteral("the card's question")},
        {QStringLiteral("note"), QStringLiteral("the card's note")},
        {QStringLiteral("answer_pages"), QStringLiteral("the answer's pages, e.g. 6 or 6–7")},
        {QStringLiteral("answer_source"), QStringLiteral("the answer's source file and lines (between the card's markers)")},
        {QStringLiteral("answer_text"), QStringLiteral("the answer itself: those source lines, else the PDF text (not in the defaults)")},
        {QStringLiteral("answer_feedback"), QStringLiteral("the passages of the answer marked Liked or Disliked during reviews, with their comments")},
        {QStringLiteral("current_review"), QStringLiteral("the answer being given now (before step, tags, interval, notes), not saved yet")},
    };
}

// The template with each {placeholder} replaced by its value. A line is left out when one of
// its placeholders has no value (e.g. "Note:" for a card without a note); a missing value is
// empty. Braces around anything else, e.g. {x} or LaTeX, are copied as written. The result ends
// with exactly one newline; values may span several lines.
inline QString render(const QString &templateText, const QHash<QString, QString> &values)
{
    QStringList known;
    for (const auto &placeholder : placeholders()) known.append(placeholder.name);
    static const QRegularExpression pattern(QStringLiteral("\\{([a-z_]+)\\}"));
    QStringList lines;
    QString normalized = templateText;
    normalized.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    for (const QString &line : normalized.split(QLatin1Char('\n'))) {
        QString rendered;
        qsizetype from = 0;
        bool empty = false;
        auto matches = pattern.globalMatch(line);
        while (matches.hasNext()) {
            const auto match = matches.next();
            if (!known.contains(match.captured(1))) continue;
            QString value = values.value(match.captured(1));
            while (!value.isEmpty() && value.back().isSpace()) value.chop(1);
            if (value.trimmed().isEmpty()) {
                empty = true;
                break;
            }
            rendered += line.mid(from, match.capturedStart() - from) + value;
            from = match.capturedEnd();
        }
        if (empty) continue;
        lines.append(rendered + line.mid(from));
    }
    QString text = lines.join(QLatin1Char('\n'));
    while (!text.isEmpty() && text.back().isSpace()) text.chop(1);
    return text.isEmpty() ? QString() : text + QLatin1Char('\n');
}

// Whether the template needs the answer text, which is slower to read (the PDF fallback).
inline bool usesPlaceholder(const QString &templateText, const QString &name)
{
    return templateText.contains(QLatin1Char('{') + name + QLatin1Char('}'));
}

// The card's "Copy for agent" text: @p fields are the card's and document's placeholders.
inline QString renderForCard(const QHash<QString, QString> &fields, const QString &agentTemplate)
{
    QString agent = render(agentTemplate, fields);
    // The answer feedback follows the card lines even in a template saved before it existed.
    const QString feedback = fields.value(QStringLiteral("answer_feedback")).trimmed();
    if (!feedback.isEmpty() && !usesPlaceholder(agentTemplate, QStringLiteral("answer_feedback"))) agent += feedback + QLatin1Char('\n');
    // So does the answer being given now.
    const QString review = fields.value(QStringLiteral("current_review")).trimmed();
    if (!review.isEmpty() && !usesPlaceholder(agentTemplate, QStringLiteral("current_review"))) agent += review + QLatin1Char('\n');
    return agent;
}
}
