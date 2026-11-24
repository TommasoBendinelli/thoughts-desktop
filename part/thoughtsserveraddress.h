// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QString>

#include <functional>

// The address of the Thoughts server, which this computer's device token
// belongs to. THOUGHTS_SERVER_URL overrides it for one process; otherwise
// Thoughts uses the address saved in server.json in the Thoughts data folder
// (Server… in the app, or thoughtsctl set-server).
namespace Thoughts::ServerAddress
{

// The server a new installation starts with: THOUGHTS_DEFAULT_SERVER_URL at
// build time (https://thoughts.example.org unless the build names another).
QString defaultUrl();
// The address older versions of this build always used, which an
// installation that already synchronized keeps: THOUGHTS_LEGACY_SERVER_URL at
// build time, empty when the build has none.
QString legacyUrl();

// The canonical form of an address (lower-case host, no trailing slash), or
// an empty string with a reason in *error. Thoughts only accepts https URLs
// with a host and without user name, password, query or fragment.
QString normalized(const QString &address, QString *error = nullptr);

// Where the chosen address is saved.
QString storePath();
// The saved address, or an empty string. *present tells whether something
// was saved at all, even an address that is no longer valid.
QString saved(bool *present = nullptr, const QString &path = storePath());
// Saving another address on a computer that already synchronized records
// first that the tokens stored so far belong to the previous server (see
// the token functions below).
bool save(const QString &address, QString *error = nullptr, const QString &path = storePath());
bool save(const QString &address, QString *error, const QString &path, const std::function<bool()> &existingInstallation);

// The address a computer that never saved one starts with. Older versions
// always used the legacy address, so an installation that already
// synchronized keeps it until its owner switches; a new one, and every
// installation of a build without a legacy address, starts with the default.
QString firstRunUrl(bool existingInstallation);
QString firstRunUrl(bool existingInstallation, const QString &defaultAddress, const QString &legacyAddress);
// Whether this computer already synchronized with the server: it holds Sync
// state. Thoughts asks once, at startup, before the first Sync of this run
// could create that state.
bool existingInstallation();

// Whether THOUGHTS_SERVER_URL chooses the address for this process.
bool overriddenByEnvironment();
// The address this process uses: THOUGHTS_SERVER_URL, else the saved one.
// The first time nothing is saved, it saves firstRunUrl() so later runs and
// the Server dialog see the same address; a saved address always wins over
// the build's addresses. May be invalid or empty; callers validate it with
// normalized().
QString configured();
QString configured(const std::function<bool()> &existingInstallation, const QString &path);
// The same with other default and legacy addresses than the build's.
QString configured(const std::function<bool()> &existingInstallation, const QString &path, const QString &defaultAddress, const QString &legacyAddress);

// A device token belongs to the server that issued it. The secret store only
// knows "the token of this computer", so the origin (scheme, host and port)
// of the issuing server is recorded next to the address, in server.json, per
// secret-store account. A stored token is sent to an address of that origin
// only: when the address changes to another origin (Server… in the app,
// thoughtsctl set-server or THOUGHTS_SERVER_URL), the old token stays here
// and Thoughts asks for the new server's token. A token given for one
// process in THOUGHTS_TOKEN is not stored and goes where that process goes.

// The secret store's service for the server's tokens.
QString tokenService();
// The origin of an address, e.g. https://example.org or
// https://example.org:8443; empty for an address normalized() refuses.
QString origin(const QString &address);
// The origin recorded for the token stored for `account`, or an empty string
// if nothing is recorded yet.
QString tokenOrigin(const QString &account, const QString &path = storePath());
// Records that `token`, now stored for `account`, was issued by the server
// at `address`: what thoughtsctl store-secret and Server… do after storing a
// token the owner typed.
bool bindToken(const QString &account, const QByteArray &token, const QString &address, QString *error = nullptr, const QString &path = storePath());
// Whether `token`, read from the secret store for `account`, may be sent to
// `address`. *issuedFor receives the origin it is bound to.
// - The first time a token is seen (the first run after the update that
//   introduced this), it is bound to the address saved then: an existing
//   installation keeps working.
// - A token that differs from the one last seen was stored since, for the
//   server in use at that time: it is bound to the saved address.
// The record keeps only a short fingerprint of the token, to notice that it
// changed.
bool tokenMayBeSentTo(const QString &account, const QByteArray &token, const QString &address, QString *issuedFor = nullptr);
bool tokenMayBeSentTo(const QString &account, const QByteArray &token, const QString &address, QString *issuedFor, const std::function<bool()> &existingInstallation, const QString &path);
// The same with other default and legacy addresses than the build's.
bool tokenMayBeSentTo(const QString &account, const QByteArray &token, const QString &address, QString *issuedFor, const std::function<bool()> &existingInstallation, const QString &path,
                      const QString &defaultAddress, const QString &legacyAddress);
// The token stored for `account` if it may be sent to `address`, else an
// empty array; *issuedFor is then the origin of the server it belongs to
// (empty when no token is stored at all).
QByteArray storedToken(const QString &account, const QString &address, QString *issuedFor = nullptr);
// What to tell the owner when a stored token is not sent to `address`.
QString tokenWithheldMessage(const QString &account, const QString &issuedFor, const QString &address);

// Signing in or out, or storing a token by hand, changes the server or the tokens while Thoughts
// runs. Whatever keeps a token or a client for later compares connectionGeneration() with the
// value it read them at and reads them again when it moved. connectionChanged() moves it. It
// counts per binary: the app and its document part each move their own, see
// Part::reloadServerConnection.
quint64 connectionGeneration();
void connectionChanged();

}
