/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <QByteArray>
#include <QString>

namespace Thoughts
{

struct DocumentStyleResourceStatus {
    QString kind;
    QString relativePath;
    QString absolutePath;
    QString localSha256;
    QString publishedSha256;
    QString publishedObjectSha256;
    QString fedoraSha256;
    QString frozenSnapshotPath;
    qint64 publishedSizeBytes = -1;
    bool liveAvailable = false;
    bool publishedIdentityAvailable = false;
    bool fedoraIdentityAvailable = false;
    bool frozenSnapshotAvailable = false;

    bool localMatchesPublished() const;
    bool localMatchesFedora() const;
};

struct DocumentStyleInformation {
    DocumentStyleResourceStatus sharedStyle;
    DocumentStyleResourceStatus projectOverride;
    bool publishedResourcesAvailable = false;
    // False for a Project's default folder, which has no shared project style.
    bool projectStyleApplicable = true;
};

DocumentStyleInformation inspectDocumentStyleInformation(const QString &docsRoot, const QString &pdfFilename, const QString &pdfSha256, const QString &project, const QString &stateRoot = {});

bool readFrozenDocumentStyleSnapshot(const DocumentStyleResourceStatus &resource, QByteArray *contents, QString *error);
QByteArray projectStyleOverrideTemplate(const QString &project);
bool createProjectStyleOverride(const QString &docsRoot, const QString &project, QString *absolutePath, QString *error);

}
