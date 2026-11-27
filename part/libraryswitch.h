// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>
#include <QStringList>

#include <optional>

// The Library on this computer belongs to one server and one account. Signing in to
// another server (or as another person) must not reuse it: its Sync checkpoint, its
// sources and its PDFs are the other Library's, and a Sync would either stop at once
// ("sources on the server kept changing", 26 Nov 2026) or mix the two Libraries.
// Sign In therefore schedules a switch; the next start sets the Library folder and the
// Library's own state aside, keeps them on this computer, and begins with an empty
// Library that the new server fills.
namespace Thoughts::LibrarySwitch
{
struct Holder {
    QString server;   // canonical address (ServerAddress::normalized)
    QString username; // empty when unknown (installations from before this record)
};

// The record of whose Library this computer holds, inside the Library's own state
// (it is set aside with it).
QString originPath();
std::optional<Holder> origin();
bool recordOrigin(const Holder &holder, QString *error = nullptr);

// Whether this computer holds a Library that was synchronized at least once.
bool holdsLibrary();

// Whether signing in as @p to must set aside the Library held for @p held: another
// server, or (when both names are known) another account on the same server.
bool differs(const Holder &held, const Holder &to);

// Where a scheduled switch waits for the next start.
QString pendingPath();
bool pending();
std::optional<Holder> pendingTarget();
bool schedule(const Holder &from, const Holder &to, QString *error = nullptr);

struct Result {
    bool ran = false;      // a switch was pending
    bool succeeded = false;
    QString keptIn;        // the folder that holds the previous Library's state
    QString keptLibrary;   // where the previous Library folder now is
    QString error;
};
// At start, before anything opens the Library: carries out a scheduled switch. On
// failure nothing stays moved and the switch stays scheduled (Sync refuses meanwhile).
Result runPending();

// What is set aside, relative to the folders they live in (for tests and the message).
QStringList stateEntries();
}
