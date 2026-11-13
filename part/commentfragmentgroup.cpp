/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#include "commentfragmentgroup.h"

#include "commentfragmentids.h"
#include "core/annotations.h"
#include "core/document.h"
#include "core/page.h"

#include <QSet>

#include <algorithm>

namespace Thoughts
{

namespace
{

CommentFragmentGroup standaloneGroup(Okular::Annotation *annotation, int pageNumber)
{
    return {
        annotation ? annotation->uniqueName() : QString(),
        {
            {
                pageNumber,
                0,
                annotation,
            },
        },
    };
}

}

CommentFragmentGroup commentFragmentGroup(const QList<Okular::Page *> &pages, Okular::Annotation *annotation, int pageNumber)
{
    if (!annotation || annotation->uniqueName().isEmpty()) {
        return standaloneGroup(annotation, pageNumber);
    }

    const QString logicalId = logicalCommentId(annotation->uniqueName());
    CommentFragmentGroup group;
    group.logicalId = logicalId;
    int rootCount = 0;

    for (const Okular::Page *page : pages) {
        if (!page) {
            continue;
        }
        for (Okular::Annotation *candidate : page->annotations()) {
            if (!candidate) {
                continue;
            }
            if (candidate->uniqueName() == logicalId) {
                ++rootCount;
                group.fragments.append({
                    page->number(),
                    0,
                    candidate,
                });
                continue;
            }
            const std::optional<CommentFragmentId> parsed = parseCommentFragmentId(candidate->uniqueName());
            if (parsed && parsed->logicalId == logicalId) {
                group.fragments.append({
                    page->number(),
                    parsed->index,
                    candidate,
                });
            }
        }
    }

    if (rootCount != 1) {
        return standaloneGroup(annotation, pageNumber);
    }

    std::sort(group.fragments.begin(), group.fragments.end(), [](const CommentFragment &left, const CommentFragment &right) {
        if (left.index != right.index) {
            return left.index < right.index;
        }
        return left.page < right.page;
    });

    const CommentFragment root = group.root();
    if (!root.annotation || root.index != 0 || root.annotation->subType() != Okular::Annotation::AHighlight) {
        return standaloneGroup(annotation, pageNumber);
    }
    for (qsizetype position = 0; position < group.fragments.size(); ++position) {
        const CommentFragment &fragment = group.fragments.at(position);
        if (!fragment.annotation
            || fragment.index != position
            || fragment.page != root.page + position
            || fragment.annotation->subType() != root.annotation->subType()
            || (position > 0
                && !fragment.annotation->contents().isEmpty()
                && fragment.annotation->contents() != root.annotation->contents())) {
            return standaloneGroup(annotation, pageNumber);
        }
    }
    return group;
}

CommentFragmentGroup commentFragmentGroup(Okular::Document *document, Okular::Annotation *annotation, int pageNumber)
{
    if (!document) {
        return standaloneGroup(annotation, pageNumber);
    }

    QList<Okular::Page *> pages;
    pages.reserve(document->pages());
    for (uint page = 0; page < document->pages(); ++page) {
        pages.append(const_cast<Okular::Page *>(document->page(page)));
    }
    return commentFragmentGroup(pages, annotation, pageNumber);
}

CommentFragment commentThreadRoot(Okular::Document *document, Okular::Annotation *annotation, int pageNumber)
{
    if (!document || !annotation) {
        return {pageNumber, 0, annotation};
    }

    Okular::Annotation *root = annotation;
    QSet<Okular::Annotation *> visited;
    while (root->previousAnnotation()) {
        if (visited.contains(root) || visited.contains(root->previousAnnotation())) {
            return {pageNumber, 0, annotation};
        }
        visited.insert(root);
        root = root->previousAnnotation();
    }

    for (uint candidatePage = 0; candidatePage < document->pages(); ++candidatePage) {
        const Okular::Page *page = document->page(candidatePage);
        if (page && page->annotations().contains(root)) {
            return {static_cast<int>(candidatePage), 0, root};
        }
    }
    return {pageNumber, 0, annotation};
}

bool isRecognizedCommentContinuation(const QList<Okular::Page *> &pages, const Okular::Annotation *annotation, int pageNumber)
{
    const std::optional<CommentFragmentId> parsed =
        annotation ? parseCommentFragmentId(annotation->uniqueName()) : std::nullopt;
    if (!parsed) {
        return false;
    }

    if (pageNumber < 0) {
        return false;
    }
    const CommentFragmentGroup group =
        commentFragmentGroup(pages, const_cast<Okular::Annotation *>(annotation), pageNumber);
    return group.logicalId == parsed->logicalId && group.isGrouped();
}

}
