/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef OKULAR_COMMENTFRAGMENTGROUP_H
#define OKULAR_COMMENTFRAGMENTGROUP_H

#include <QList>
#include <QString>

namespace Okular
{
class Annotation;
class Document;
class Page;
}

namespace Thoughts
{

struct CommentFragment {
    int page = -1;
    int index = 0;
    Okular::Annotation *annotation = nullptr;
};

struct CommentFragmentGroup {
    QString logicalId;
    QList<CommentFragment> fragments;

    bool isGrouped() const
    {
        return fragments.size() > 1;
    }

    CommentFragment root() const
    {
        return fragments.isEmpty() ? CommentFragment {} : fragments.constFirst();
    }
};

CommentFragmentGroup commentFragmentGroup(Okular::Document *document, Okular::Annotation *annotation, int pageNumber);
CommentFragmentGroup commentFragmentGroup(const QList<Okular::Page *> &pages, Okular::Annotation *annotation, int pageNumber);
CommentFragment commentThreadRoot(Okular::Document *document, Okular::Annotation *annotation, int pageNumber);
bool isRecognizedCommentContinuation(const QList<Okular::Page *> &pages, const Okular::Annotation *annotation, int pageNumber);

}

#endif
