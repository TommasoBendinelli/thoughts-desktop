/*
    SPDX-FileCopyrightText: 2026 Tommaso Bendinelli

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#pragma once

#include <KLocalizedString>

#include <QString>

// The Sync status vocabulary shared by Thoughts on the phone, Thoughts Desktop
// and the Thoughts Manual. Every surface that names an overall Sync state uses
// exactly these words; syncstatuslabelstest pins them to the phone's
// SyncStatusLabels. Progress phases ("Building PDFs", "Waiting for a
// connection") and failure explanations may appear as details only.
namespace Thoughts::SyncStatusLabels
{

inline QString syncing()
{
    return i18n("Syncing…");
}

inline QString upToDate()
{
    return i18n("Up to date");
}

inline QString changesPending()
{
    return i18n("Changes pending");
}

// The changes reached the server, but at least one PDF could not be built.
inline QString pdfBuildFailed()
{
    return i18n("PDF build failed");
}

// The changes could not be propagated. The detail names the failed step.
inline QString syncFailed()
{
    return i18n("Sync failed");
}

inline QString showLogs()
{
    return i18n("Show logs");
}

// A source file changed here and on the server in overlapping places. This
// outranks every other failure: nothing else can sync until it is merged.
inline QString mergeNeeded()
{
    return i18n("Merge needed");
}

// The only action offered with mergeNeeded(); logs stay in Sync history.
inline QString showMerge()
{
    return i18n("Show merge");
}

inline QString sourceFilesNeedMerge(int files)
{
    return i18np("1 source file needs a merge", "%1 source files need a merge", files);
}

// Detail lines under pdfBuildFailed(), when the changes themselves arrived.
inline QString changesSynced()
{
    return i18n("Your changes are synced.");
}

inline QString changesSyncedWithFailedPdfs(int failedPdfs)
{
    return i18np("Your changes are synced · 1 PDF failed to build.", "Your changes are synced · %1 PDFs failed to build.", failedPdfs);
}

inline QString changesSyncedWithFailedProjects(int failedProjects)
{
    return i18np("Your changes are synced · 1 project failed to build.", "Your changes are synced · %1 projects failed to build.", failedProjects);
}

}
