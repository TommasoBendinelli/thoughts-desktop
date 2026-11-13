// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Clones a Git repository with libgit2, so Import sources › From Git repository
// needs no installed Git. gitClone() blocks and runs on a worker thread; questions
// for the user (sign-in, key passphrase, unknown SSH host) go through GitClonePrompts.
#include <QByteArray>
#include <QString>
#include <atomic>
#include <functional>

namespace Thoughts {

class GitClonePrompts
{
public:
    virtual ~GitClonePrompts() = default;
    // HTTPS sign-in, asked at most once per clone. *username may hold the URL's
    // user name. False cancels the clone.
    virtual bool httpsCredentials(const QString &url, QString *username, QString *password) = 0;
    // Passphrase of an encrypted SSH key; retry is true after a wrong one.
    virtual bool keyPassphrase(const QString &keyPath, bool retry, QString *passphrase) = 0;
    // An SSH host not in known_hosts. otherKeyTypesKnown: known_hosts has keys of
    // other types for it. True trusts the key and appends it to known_hosts.
    virtual bool trustUnknownHost(const QString &host, const QString &keyType, const QString &fingerprint, bool otherKeyTypesKnown) = 0;
};

struct GitCloneProgress {
    quint64 receivedObjects = 0, indexedObjects = 0, totalObjects = 0, receivedBytes = 0;
    quint64 indexedDeltas = 0, totalDeltas = 0;
    quint64 checkedOutFiles = 0, filesToCheckOut = 0;
};

struct GitCloneOutcome {
    enum Status { Cloned, Canceled, Failed };
    Status status = Failed;
    QString message; // for Failed: what went wrong, for the user
    QString detail;  // for Failed: libgit2's own message
};

struct GitCloneRequest {
    QString source;      // URL, scp-style address or local path
    QString branch;      // empty: the remote's default branch
    QString destination; // must not exist or be empty; removed again unless Cloned
    QString sshDirectory; // empty: ~/.ssh (known_hosts and key files)
};

// prompts may be null (every question is answered "no"). cancel is polled during
// the transfer; progress is called from the cloning thread.
GitCloneOutcome gitClone(const GitCloneRequest &request, GitClonePrompts *prompts, const std::atomic_bool *cancel,
                         const std::function<void(const GitCloneProgress &)> &progress = {});

// libgit2's version, e.g. "1.7.1".
QString gitLibraryVersion();

// known_hosts as OpenSSH reads it: plain and hashed host names, [host]:port,
// wildcards, negation, @revoked (a revoked key counts as a mismatch).
enum class KnownHost { Matches, Unknown, OtherKeyTypesKnown, Mismatch };
KnownHost checkKnownHosts(const QByteArray &knownHosts, const QString &host, int port, const QByteArray &hostKey);
QByteArray knownHostsLine(const QString &host, int port, const QByteArray &hostKey);
// "ssh-ed25519" etc., read from the key blob itself.
QByteArray sshKeyType(const QByteArray &hostKey);
// "SHA256:…" as ssh prints it.
QString sshKeyFingerprint(const QByteArray &hostKey);
// Whether an OpenSSH or PEM private key file needs a passphrase.
bool sshPrivateKeyEncrypted(const QByteArray &privateKey);
// Host and port an SSH URL or scp-style address connects to; port 0 if not SSH.
bool sshEndpoint(const QString &source, QString *host, int *port);

}
