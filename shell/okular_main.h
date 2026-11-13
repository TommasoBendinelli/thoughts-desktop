/*
    SPDX-FileCopyrightText: 2002 Wilco Greven <greven@kde.org>
    SPDX-FileCopyrightText: 2003 Christophe Devriese <Christophe.Devriese@student.kuleuven.ac.be>
    SPDX-FileCopyrightText: 2003 Laurent Montel <montel@kde.org>
    SPDX-FileCopyrightText: 2003-2007 Albert Astals Cid <aacid@kde.org>
    SPDX-FileCopyrightText: 2004 Andy Goossens <andygoossens@telenet.be>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _OKULAR_MAIN_H_
#define _OKULAR_MAIN_H_
#include <QJsonObject>
#include <QObject>
#include <QStringList>

class QEvent;
class QUrl;

namespace Okular
{
enum Status { Error, AttachedOtherProcess, Success };

class FileOpenEventHandler : public QObject
{
public:
    explicit FileOpenEventHandler(QObject *parent = nullptr)
        : QObject(parent)
    {
    }

protected:
    bool eventFilter(QObject *object, QEvent *event) override;
};

QUrl resolvedExternalOpenUrl(const QUrl &url, const QString &docsRoot);
// The card a thoughts://review-card/<card id> link names, or empty for any other link.
QString reviewCardIdFromUrl(const QUrl &url);
// thoughts://agent-notes?document=…|project=…&note=…: the open-agent-notes request it asks for
// ({"document"|"project", "note_id"}); empty for anything else.
QJsonObject agentNotesRequestFromUrl(const QUrl &url);
Status main(const QStringList &paths, const QString &serializedOptions);

}

#endif

/* kate: replace-tabs on; indent-width 4; */
