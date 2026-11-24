// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <KLocalizedString>

#include <QList>
#include <QString>

// What the Overlay tab's Copy Request puts on the clipboard: a request to paste to an agent with
// the Thoughts tools (the Claude app with the Thoughts connector, an Agent Study session), which
// makes a reading overlay of the open document (reading-overlays.md). Four kinds; the request is in
// English for the agent, the labels are the user's. The phone has the same kinds, keys and texts
// (ui/reader/ReadingOverlayRequests.kt): change both together.
namespace Thoughts::ReadingOverlayRequests
{
enum class Kind { MentalModel, Document, FirstPass, Question };

inline QList<Kind> kinds()
{
    return {Kind::MentalModel, Kind::Document, Kind::FirstPass, Kind::Question};
}

// The kind as it is remembered (the phone keeps the same keys).
inline QString key(Kind kind)
{
    switch (kind) {
    case Kind::MentalModel:
        return QStringLiteral("mental_model");
    case Kind::Document:
        return QStringLiteral("document");
    case Kind::FirstPass:
        return QStringLiteral("first_pass");
    case Kind::Question:
        return QStringLiteral("question");
    }
    return QString();
}

inline Kind fromKey(const QString &value)
{
    for (const Kind kind : kinds()) {
        if (key(kind) == value) return kind;
    }
    return Kind::MentalModel;
}

inline QString label(Kind kind)
{
    switch (kind) {
    case Kind::MentalModel:
        return i18nc("@item:inlistbox reading overlay request", "From What I Know");
    case Kind::Document:
        return i18nc("@item:inlistbox reading overlay request", "From the Document Alone");
    case Kind::FirstPass:
        return i18nc("@item:inlistbox reading overlay request", "Quick First Pass");
    case Kind::Question:
        return i18nc("@item:inlistbox reading overlay request", "To Answer a Question");
    }
    return QString();
}

inline QString description(Kind kind)
{
    switch (kind) {
    case Kind::MentalModel:
        return i18n("Read, skim or skip by what you already know: your Mental Model and review cards");
    case Kind::Document:
        return i18n("The paper's main ideas, supporting detail and what a first reading can skip, whatever you know");
    case Kind::FirstPass:
        return i18n("Only the few passages that give the gist, for about ten minutes");
    case Kind::Question:
        return i18n("The passages that answer a question you write into the request");
    }
    return QString();
}

// The request for @p kind on the document @p name (its Library PDF @p filename, its Library
// document ID @p documentId; either may be empty, but not both).
inline QString text(Kind kind, const QString &name, const QString &filename, const QString &documentId)
{
    QStringList identity;
    if (!filename.isEmpty()) identity.append(QStringLiteral("Library PDF %1").arg(filename));
    if (!documentId.isEmpty()) identity.append(QStringLiteral("document ID %1").arg(documentId));
    const QString shown = name.trimmed().isEmpty() ? filename : name.trimmed();
    QString request = QStringLiteral("Make a reading overlay in Thoughts (add_reading_overlay) for \"%1\"").arg(shown);
    if (!identity.isEmpty()) request += QStringLiteral(" (%1)").arg(identity.join(QStringLiteral(", ")));
    request += QStringLiteral(".\n");
    switch (kind) {
    case Kind::MentalModel:
        request += QStringLiteral(
            "Base it on what I already know: read my Mental Model (mental_model) and call knowledge_profile for its topics. Mark what I should "
            "read carefully, what I can skim, and what I can skip because I know it already, each with a short comment on why (for a skip, "
            "what shows I know it). Add note marks where a term or step is new to me, explaining it in a line.\n");
        break;
    case Kind::Document:
        request += QStringLiteral(
            "Base it on the document alone, not on what I know: mark the passages that carry its main ideas and results as read, the "
            "supporting detail as skim, and what a first reading can skip as skip, each with a short comment on what it contributes.\n");
        break;
    case Kind::FirstPass:
        request += QStringLiteral(
            "It is for a quick first pass of about ten minutes: mark only the few passages that give the gist as read, and the rest of each "
            "section as skim or skip, each with a one-line comment. Take what I already know into account (mental_model, knowledge_profile).\n");
        break;
    case Kind::Question:
        request += QStringLiteral(
            "It is to answer this question: [write your question here]\n"
            "Mark the passages that answer it as read and those that help as skim, with comments saying how each passage answers it; leave "
            "the rest unmarked.\n");
        break;
    }
    request += QStringLiteral("Mark sections and paragraphs, not single sentences, and quote the PDF's text exactly. ");
    request += kind == Kind::Question
        ? QStringLiteral("When it is done, send me a note (send_note, attached to this document) with the answer, linking the overlay with its open_url.\n")
        : QStringLiteral("When it is done, send me a short note (send_note, attached to this document) that links the overlay with its open_url.\n");
    return request;
}
}
