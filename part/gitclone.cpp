// SPDX-License-Identifier: GPL-2.0-or-later
#include "gitclone.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QHostInfo>
#include <QMessageAuthenticationCode>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QtEndian>

#include <git2.h>
#if __has_include(<git2/sys/errors.h>)
#include <git2/sys/errors.h> // git_error_set_str() since libgit2 1.8
#endif

namespace Thoughts {
namespace {

QByteArray readSshString(const QByteArray &data, qsizetype *offset)
{
    if (*offset < 0 || *offset + 4 > data.size()) {
        *offset = -1;
        return {};
    }
    const quint32 length = qFromBigEndian<quint32>(data.constData() + *offset);
    *offset += 4;
    if (length > quint64(data.size() - *offset)) {
        *offset = -1;
        return {};
    }
    const QByteArray value = data.mid(*offset, length);
    *offset += length;
    return value;
}

// OpenSSH's match_pattern(): '*' and '?' wildcards, case-insensitive host names.
bool wildcardMatch(const char *pattern, const char *text)
{
    for (;; ++pattern, ++text) {
        if (!*pattern) return !*text;
        if (*pattern == '*') {
            while (*pattern == '*') ++pattern;
            if (!*pattern) return true;
            for (; *text; ++text)
                if (wildcardMatch(pattern, text)) return true;
            return false;
        }
        if (!*text || (*pattern != '?' && *pattern != *text)) return false;
    }
}

bool hostPatternsMatch(const QByteArray &patterns, const QByteArray &name)
{
    bool matched = false;
    for (QByteArray pattern : patterns.split(',')) {
        const bool negated = pattern.startsWith('!');
        if (negated) pattern.remove(0, 1);
        bool hit = false;
        if (pattern.startsWith("|1|")) {
            const auto parts = pattern.split('|'); // "", "1", salt, hash
            if (parts.size() == 4) {
                const auto salt = QByteArray::fromBase64(parts[2]);
                hit = QMessageAuthenticationCode::hash(name, salt, QCryptographicHash::Sha1) == QByteArray::fromBase64(parts[3]);
            }
        } else {
            hit = wildcardMatch(pattern.toLower().constData(), name.constData());
        }
        if (hit && negated) return false;
        matched = matched || hit;
    }
    return matched;
}

QString localUserName()
{
    for (const char *variable : {"USER", "USERNAME", "LOGNAME"}) {
        const auto value = qEnvironmentVariable(variable);
        if (!value.isEmpty()) return value;
    }
    return QStringLiteral("git");
}

QString tr(const char *text)
{
    return QObject::tr(text);
}

struct CloneState {
    CloneState(const GitCloneRequest &request, GitClonePrompts *prompts, const std::atomic_bool *cancel, const std::function<void(const GitCloneProgress &)> &progress)
        : request(request), prompts(prompts), cancel(cancel), progress(progress)
    {
    }
    ~CloneState()
    {
        // Passphrases live only as long as the clone.
        for (auto it = passphrases.begin(); it != passphrases.end(); ++it) it->fill(' ');
    }
    const GitCloneRequest &request;
    GitClonePrompts *prompts;
    const std::atomic_bool *cancel;
    const std::function<void(const GitCloneProgress &)> &progress;
    GitCloneProgress current;
    QString sshDirectory, sshHost;
    int sshPort = 0;
    bool plainHttp = false;
    bool canceledInPrompt = false;
    QString problem; // replaces the generic description of a failure

    // SSH: the agent first, then the default key files.
    QStringList keys;
    bool agentTried = false;
    qsizetype nextKey = 0;
    QString offeredKey;
    QSet<QString> rejectedKeys, unusableKeys, retryPassphrase;
    QHash<QString, QByteArray> passphrases;
    QByteArray trustedHostKey;
    bool askedHttps = false;
    QString urlUser; // a user name without password, taken out of an https:// URL

    bool canceled() const { return canceledInPrompt || (cancel && cancel->load()); }
    void report()
    {
        if (progress) progress(current);
    }
};

int credentialsCallback(git_credential **out, const char *url, const char *usernameFromUrl, unsigned int allowed, void *payload)
{
    auto *state = static_cast<CloneState *>(payload);
    if (state->canceled()) return GIT_EUSER;
    const QString user = usernameFromUrl && *usernameFromUrl ? QString::fromUtf8(usernameFromUrl) : state->urlUser;
    if (state->sshPort) {
        if (allowed & GIT_CREDENTIAL_USERNAME)
            return git_credential_username_new(out, localUserName().toUtf8().constData());
        if (allowed & GIT_CREDENTIAL_SSH_KEY) {
            const QByteArray name = (user.isEmpty() ? localUserName() : user).toUtf8();
            // libgit2 asks again only after the last offer was refused.
            if (!state->offeredKey.isEmpty()) state->rejectedKeys.insert(state->offeredKey);
            state->offeredKey.clear();
            if (!state->agentTried) {
                state->agentTried = true;
                return git_credential_ssh_key_from_agent(out, name.constData());
            }
            while (state->nextKey < state->keys.size()) {
                const QString key = state->keys.at(state->nextKey++);
                if (state->rejectedKeys.contains(key) || state->unusableKeys.contains(key)) continue;
                QFile file(key);
                if (!file.open(QIODevice::ReadOnly)) continue;
                const bool encrypted = sshPrivateKeyEncrypted(file.read(1 << 20));
                if (encrypted && !state->passphrases.contains(key)) {
                    QString passphrase;
                    const bool retry = state->retryPassphrase.contains(key);
                    // Canceling the question skips this key, as ssh does.
                    if (!state->prompts || !state->prompts->keyPassphrase(QDir::toNativeSeparators(key), retry, &passphrase)) {
                        if (state->canceled()) return GIT_EUSER;
                        continue;
                    }
                    state->passphrases.insert(key, passphrase.toUtf8());
                    passphrase.fill(QLatin1Char(' '));
                }
                const QString publicKey = key + QStringLiteral(".pub");
                state->offeredKey = key;
                return git_credential_ssh_key_new(out, name.constData(),
                    QFileInfo::exists(publicKey) ? QFile::encodeName(publicKey).constData() : nullptr,
                    QFile::encodeName(key).constData(), encrypted ? state->passphrases.value(key).constData() : nullptr);
            }
            state->problem = tr("The SSH server did not accept any of your SSH keys. Thoughts tried the keys in your SSH agent "
                                "and the key files id_ed25519, id_ecdsa and id_rsa in %1.\n\n"
                                "Add your public key to your account on the Git server, or load the right key into your SSH agent.")
                                 .arg(QDir::toNativeSeparators(state->sshDirectory));
            if (!state->unusableKeys.isEmpty()) {
                QStringList names;
                for (const auto &key : std::as_const(state->unusableKeys)) names.append(QFileInfo(key).fileName());
                names.sort();
                state->problem += QLatin1String("\n\n") + tr("These keys could not be used (wrong passphrase or a key format Thoughts cannot read): %1.").arg(names.join(QLatin1String(", ")));
            }
            git_error_set_str(GIT_ERROR_SSH, "no SSH key was accepted");
            return GIT_EAUTH;
        }
        state->problem = tr("The SSH server accepts only password sign-in, which Thoughts does not support. Use an SSH key, or clone with an https:// URL.");
        git_error_set_str(GIT_ERROR_SSH, "the server offers no public key authentication");
        return GIT_EAUTH;
    }
    if (allowed & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
        if (state->plainHttp) {
            state->problem = tr("The server asks for a sign-in over an unencrypted http:// connection. Thoughts does not send passwords or tokens "
                                "without encryption; use the repository's https:// URL.");
            git_error_set_str(GIT_ERROR_HTTP, "credentials refused over plain http");
            return GIT_EAUTH;
        }
        if (state->askedHttps) {
            state->problem = tr("The server did not accept this user name and password or token, or the account has no access to this repository.");
            git_error_set_str(GIT_ERROR_HTTP, "authentication failed");
            return GIT_EAUTH;
        }
        state->askedHttps = true;
        if (!state->prompts) {
            state->problem = tr("The server asks for a sign-in to clone this repository.");
            git_error_set_str(GIT_ERROR_HTTP, "authentication required");
            return GIT_EAUTH;
        }
        QString name = user, secret;
        if (!state->prompts->httpsCredentials(QString::fromUtf8(url), &name, &secret)) {
            state->canceledInPrompt = true;
            return GIT_EUSER;
        }
        const int result = git_credential_userpass_plaintext_new(out, name.toUtf8().constData(), secret.toUtf8().constData());
        secret.fill(QLatin1Char(' '));
        return result;
    }
    state->problem = tr("The server asks for a kind of sign-in that Thoughts does not support.");
    git_error_set_str(GIT_ERROR_NET, "unsupported authentication method");
    return GIT_EAUTH;
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(64 << 20) : QByteArray();
}

QString globalKnownHostsFile()
{
#ifdef Q_OS_WIN
    const auto programData = qEnvironmentVariable("PROGRAMDATA");
    return programData.isEmpty() ? QString() : QDir(programData).filePath(QStringLiteral("ssh/ssh_known_hosts"));
#else
    return QStringLiteral("/etc/ssh/ssh_known_hosts");
#endif
}

bool appendKnownHost(const QString &sshDirectory, const QByteArray &line)
{
    if (!QFileInfo(sshDirectory).isDir()) {
        if (!QDir().mkpath(sshDirectory)) return false;
        QFile::setPermissions(sshDirectory, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    const QString path = QDir(sshDirectory).filePath(QStringLiteral("known_hosts"));
    const QByteArray existing = readFile(path);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) return false;
    if (!existing.isEmpty() && !existing.endsWith('\n') && file.write("\n") != 1) return false;
    return file.write(line) == line.size();
}

int certificateCallback(git_cert *certificate, int valid, const char *host, void *payload)
{
    auto *state = static_cast<CloneState *>(payload);
    if (state->canceled()) return GIT_EUSER;
    const QString hostName = QString::fromUtf8(host);
    if (certificate->cert_type != GIT_CERT_HOSTKEY_LIBSSH2) {
        if (valid) return 0;
        // Never accepted: an invalid TLS certificate can mean someone reads or changes the connection.
        state->problem = tr("The security certificate of %1 is not valid or not trusted, so Thoughts did not connect. "
                            "If the address is right, the network may be intercepting the connection.").arg(hostName);
        git_error_set_str(GIT_ERROR_SSL, "the server certificate is not valid");
        return GIT_ECERTIFICATE;
    }
    const auto *hostKey = reinterpret_cast<const git_cert_hostkey *>(certificate);
    if (!(hostKey->type & GIT_CERT_SSH_RAW) || !hostKey->hostkey || !hostKey->hostkey_len) {
        state->problem = tr("The SSH server %1 did not provide a host key that Thoughts can check.").arg(hostName);
        git_error_set_str(GIT_ERROR_SSH, "no raw host key");
        return GIT_ECERTIFICATE;
    }
    const QByteArray key(hostKey->hostkey, qsizetype(hostKey->hostkey_len));
    if (!state->trustedHostKey.isEmpty() && key == state->trustedHostKey) return 0;
    const int port = state->sshPort > 0 ? state->sshPort : 22;
    const auto userFile = QDir(state->sshDirectory).filePath(QStringLiteral("known_hosts"));
    const auto user = checkKnownHosts(readFile(userFile), hostName, port, key);
    const auto global = checkKnownHosts(readFile(globalKnownHostsFile()), hostName, port, key);
    const QString type = QString::fromLatin1(sshKeyType(key));
    const QString fingerprint = sshKeyFingerprint(key);
    if (user == KnownHost::Matches || global == KnownHost::Matches) return 0;
    if (user == KnownHost::Mismatch || global == KnownHost::Mismatch) {
        state->problem = tr("WARNING: the SSH host key of %1 has changed.\n\n"
                            "The server now presents the %2 key %3, which does not match the key in %4. "
                            "Someone could be intercepting the connection, or the server's key was replaced. "
                            "Thoughts did not connect. If you know the key changed, remove the old line for this host from known_hosts and try again.")
                             .arg(hostName, type, fingerprint, QDir::toNativeSeparators(userFile));
        git_error_set_str(GIT_ERROR_SSH, "host key mismatch");
        return GIT_ECERTIFICATE;
    }
    const bool otherTypes = user == KnownHost::OtherKeyTypesKnown || global == KnownHost::OtherKeyTypesKnown;
    if (!state->prompts) {
        state->problem = tr("The SSH host %1 is not in known_hosts. Its %2 key fingerprint is %3.").arg(hostName, type, fingerprint);
        git_error_set_str(GIT_ERROR_SSH, "unknown host key");
        return GIT_ECERTIFICATE;
    }
    if (!state->prompts->trustUnknownHost(hostName, type, fingerprint, otherTypes)) {
        state->canceledInPrompt = true;
        return GIT_EUSER;
    }
    state->trustedHostKey = key;
    appendKnownHost(state->sshDirectory, knownHostsLine(hostName, port, key));
    return 0;
}

int transferCallback(const git_indexer_progress *stats, void *payload)
{
    auto *state = static_cast<CloneState *>(payload);
    state->current.receivedObjects = stats->received_objects;
    state->current.indexedObjects = stats->indexed_objects;
    state->current.totalObjects = stats->total_objects;
    state->current.receivedBytes = stats->received_bytes;
    state->current.indexedDeltas = stats->indexed_deltas;
    state->current.totalDeltas = stats->total_deltas;
    state->report();
    return state->canceled() ? GIT_EUSER : 0;
}

int sidebandCallback(const char *, int, void *payload)
{
    return static_cast<CloneState *>(payload)->canceled() ? GIT_EUSER : 0;
}

void checkoutCallback(const char *, size_t completed, size_t total, void *payload)
{
    auto *state = static_cast<CloneState *>(payload);
    state->current.checkedOutFiles = completed;
    state->current.filesToCheckOut = total;
    state->report();
}

enum class SourceKind { Local, Ssh, Http, Other };

SourceKind sourceKind(const QString &source)
{
    QString host;
    int port = 0;
    if (sshEndpoint(source, &host, &port)) return SourceKind::Ssh;
    const auto scheme = source.section(QStringLiteral("://"), 0, 0).toLower();
    if (source.contains(QStringLiteral("://"))) {
        if (scheme == QLatin1String("http") || scheme == QLatin1String("https")) return SourceKind::Http;
        if (scheme == QLatin1String("file")) return SourceKind::Local;
        return SourceKind::Other;
    }
    return SourceKind::Local;
}

QString describeFailure(int code, const QString &detail, const CloneState &state)
{
    if (!state.problem.isEmpty()) return state.problem;
    const auto text = detail.toLower();
    const auto kind = sourceKind(state.request.source);
    const auto has = [&text](const char *needle) { return text.contains(QLatin1String(needle)); };
    if (!state.request.branch.isEmpty() && code == GIT_ENOTFOUND && has("refs/remotes/"))
        return tr("The branch “%1” does not exist in this repository.").arg(state.request.branch);
    if (kind == SourceKind::Local)
        return tr("No Git repository was found at this path.");
    if (kind == SourceKind::Other || has("unsupported url protocol") || has("malformed url") || has("invalid url"))
        return tr("This is not a repository address Thoughts can clone. Use an https:// URL, an SSH address such as git@github.com:owner/repository.git, or a local folder.");
    if (code == GIT_EAUTH || has("authentication") || has("401") || has("403"))
        return kind == SourceKind::Ssh ? tr("The SSH server did not accept the sign-in.")
                                       : tr("The server did not accept the sign-in, or the account has no access to this repository.");
    if (code == GIT_ECERTIFICATE || has("certificate"))
        return tr("The server's security certificate is not valid or not trusted, so Thoughts did not connect.");
    if (has("resolve") || has("name or service") || has("nodename") || has("server name or address") || has("no such host"))
        return tr("The server could not be found. Check the address and your network connection.");
    if (code == GIT_TIMEOUT || has("timed out") || has("timeout"))
        return tr("The server did not answer in time. Check your network connection and try again.");
    if (has("failed to connect") || has("connection refused") || has("unreachable") || has("could not connect") || has("cannot connect"))
        return tr("Could not connect to the server. Check the address and your network connection.");
    if (code == GIT_ENOTFOUND || has("404") || has("not found") || has("does not exist") || has("does not appear to be a git repository"))
        return tr("No Git repository was found at this address. Check the URL; private repositories also need a sign-in.");
    // WinHTTP reports most network failures as "failed to send request: <Windows
    // message>", and the message can be empty (Wine) or translated: ask the resolver.
    if (has("failed to send request") || has("failed to connect") || has("failed to receive")) {
        const QString host = kind == SourceKind::Ssh ? state.sshHost : QUrl(state.request.source.trimmed()).host();
        if (!host.isEmpty()) {
            if (QHostInfo::fromName(host).error() != QHostInfo::NoError)
                return tr("The server %1 could not be found. Check the address and your network connection.").arg(host);
            return tr("Could not connect to %1. Check the address and your network connection.").arg(host);
        }
    }
    return tr("The repository could not be cloned.");
}

}

QString gitLibraryVersion()
{
    int major = 0, minor = 0, revision = 0;
    git_libgit2_version(&major, &minor, &revision);
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(revision);
}

QByteArray sshKeyType(const QByteArray &hostKey)
{
    qsizetype offset = 0;
    return readSshString(hostKey, &offset);
}

QString sshKeyFingerprint(const QByteArray &hostKey)
{
    return QStringLiteral("SHA256:")
        + QString::fromLatin1(QCryptographicHash::hash(hostKey, QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals));
}

QByteArray knownHostsLine(const QString &host, int port, const QByteArray &hostKey)
{
    const QByteArray name = port > 0 && port != 22 ? '[' + host.toLower().toUtf8() + "]:" + QByteArray::number(port) : host.toLower().toUtf8();
    return name + ' ' + sshKeyType(hostKey) + ' ' + hostKey.toBase64() + '\n';
}

KnownHost checkKnownHosts(const QByteArray &knownHosts, const QString &host, int port, const QByteArray &hostKey)
{
    const QByteArray name = port > 0 && port != 22 ? '[' + host.toLower().toUtf8() + "]:" + QByteArray::number(port) : host.toLower().toUtf8();
    const QByteArray type = sshKeyType(hostKey);
    bool matches = false, mismatch = false, otherTypes = false;
    for (const auto &rawLine : knownHosts.split('\n')) {
        const QByteArray line = rawLine.simplified();
        if (line.isEmpty() || line.startsWith('#')) continue;
        auto fields = line.split(' ');
        QByteArray marker;
        if (fields.first().startsWith('@')) marker = fields.takeFirst();
        if (fields.size() < 3 || marker == "@cert-authority" || !hostPatternsMatch(fields[0], name)) continue;
        const QByteArray key = QByteArray::fromBase64(fields[2]);
        if (marker == "@revoked") {
            if (key == hostKey) return KnownHost::Mismatch;
            continue;
        }
        if (!marker.isEmpty()) continue;
        if (key == hostKey) matches = true;
        else if (fields[1] == type) mismatch = true;
        else otherTypes = true;
    }
    if (matches) return KnownHost::Matches;
    if (mismatch) return KnownHost::Mismatch;
    return otherTypes ? KnownHost::OtherKeyTypesKnown : KnownHost::Unknown;
}

bool sshPrivateKeyEncrypted(const QByteArray &privateKey)
{
    static const QByteArray begin("-----BEGIN OPENSSH PRIVATE KEY-----"), end("-----END OPENSSH PRIVATE KEY-----");
    const auto start = privateKey.indexOf(begin);
    if (start >= 0) {
        const auto stop = privateKey.indexOf(end, start);
        if (stop < 0) return false;
        QByteArray body = privateKey.mid(start + begin.size(), stop - start - begin.size());
        body.replace('\r', "").replace('\n', "").replace(' ', "");
        const QByteArray data = QByteArray::fromBase64(body);
        static const QByteArray magic("openssh-key-v1", 15); // including its NUL
        if (!data.startsWith(magic)) return false;
        qsizetype offset = magic.size();
        const QByteArray cipher = readSshString(data, &offset);
        return offset > 0 && cipher != "none";
    }
    return privateKey.contains("-----BEGIN ENCRYPTED PRIVATE KEY-----") || privateKey.contains("Proc-Type: 4,ENCRYPTED");
}

// https://git@host/repo: libgit2 first signs in as that user with an empty password, and
// a server that refuses it without offering a sign-in again (Overleaf) ends the clone
// before Thoughts can ask for the password or token. Without the user name in the URL
// the server asks normally, and the question starts from that name.
QString withoutPasswordlessUser(const QString &source, QString *user)
{
    const auto schemeEnd = source.indexOf(QStringLiteral("://"));
    if (schemeEnd < 0) return source;
    const auto scheme = source.left(schemeEnd).toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http")) return source;
    const auto authorityStart = schemeEnd + 3;
    auto authorityEnd = source.indexOf(QLatin1Char('/'), authorityStart);
    if (authorityEnd < 0) authorityEnd = source.size();
    const auto at = source.lastIndexOf(QLatin1Char('@'), authorityEnd - 1);
    if (at < authorityStart) return source;
    const QString userInfo = source.mid(authorityStart, at - authorityStart);
    if (userInfo.isEmpty() || userInfo.contains(QLatin1Char(':'))) return source;
    *user = QUrl::fromPercentEncoding(userInfo.toUtf8());
    return source.left(authorityStart) + source.mid(at + 1);
}

bool sshEndpoint(const QString &source, QString *host, int *port)
{
    const QString trimmed = source.trimmed();
    const auto schemeEnd = trimmed.indexOf(QStringLiteral("://"));
    if (schemeEnd >= 0) {
        const auto scheme = trimmed.left(schemeEnd).toLower();
        if (scheme != QLatin1String("ssh") && scheme != QLatin1String("ssh+git") && scheme != QLatin1String("git+ssh")) return false;
        const QUrl url(QStringLiteral("ssh") + trimmed.mid(schemeEnd));
        if (!url.isValid() || url.host().isEmpty()) return false;
        *host = url.host();
        *port = url.port(22);
        return true;
    }
    // scp-style [user@]host:path, as git reads it: a colon before any slash, and
    // not a Windows drive letter.
    const auto colon = trimmed.indexOf(QLatin1Char(':'));
    const auto slash = trimmed.indexOf(QLatin1Char('/'));
    if (colon <= 0 || (slash >= 0 && slash < colon) || QDir::isAbsolutePath(trimmed)) return false;
    if (colon == 1 && trimmed.at(0).isLetter()) return false;
    QString name = trimmed.left(colon).section(QLatin1Char('@'), -1);
    if (name.startsWith(QLatin1Char('[')) && name.endsWith(QLatin1Char(']'))) name = name.mid(1, name.size() - 2);
    if (name.isEmpty()) return false;
    *host = name;
    *port = 22;
    return true;
}

GitCloneOutcome gitClone(const GitCloneRequest &request, GitClonePrompts *prompts, const std::atomic_bool *cancel,
                         const std::function<void(const GitCloneProgress &)> &progress)
{
    git_libgit2_init();
    struct Shutdown {
        ~Shutdown() { git_libgit2_shutdown(); }
    } shutdown;
#if LIBGIT2_VER_MAJOR > 1 || (LIBGIT2_VER_MAJOR == 1 && LIBGIT2_VER_MINOR >= 7)
    // No hang on a server that stops answering (WinHTTP has its own limits).
    git_libgit2_opts(GIT_OPT_SET_SERVER_CONNECT_TIMEOUT, 30000);
    git_libgit2_opts(GIT_OPT_SET_SERVER_TIMEOUT, 120000);
#endif

    CloneState state(request, prompts, cancel, progress);
    state.sshDirectory = request.sshDirectory.isEmpty() ? QDir::home().filePath(QStringLiteral(".ssh")) : request.sshDirectory;
    for (const auto *name : {"id_ed25519", "id_ecdsa", "id_rsa"}) {
        const auto path = QDir(state.sshDirectory).filePath(QString::fromLatin1(name));
        if (QFileInfo(path).isFile()) state.keys.append(path);
    }
    sshEndpoint(request.source, &state.sshHost, &state.sshPort);
    state.plainHttp = request.source.trimmed().startsWith(QLatin1String("http://"), Qt::CaseInsensitive);
    const auto removeDestination = [&request] { QDir(request.destination).removeRecursively(); };

    const QByteArray originalSource = request.source.trimmed().toUtf8();
    const QByteArray source = withoutPasswordlessUser(request.source.trimmed(), &state.urlUser).toUtf8();
    const QByteArray destination = QFile::encodeName(request.destination);
    const QByteArray branch = request.branch.trimmed().toUtf8();
    for (int attempt = 0;; ++attempt) {
        if (state.canceled()) {
            removeDestination();
            return {GitCloneOutcome::Canceled, {}, {}};
        }
        git_clone_options options;
        git_clone_options_init(&options, GIT_CLONE_OPTIONS_VERSION);
        options.checkout_branch = branch.isEmpty() ? nullptr : branch.constData();
        options.checkout_opts.progress_cb = checkoutCallback;
        options.checkout_opts.progress_payload = &state;
        options.fetch_opts.proxy_opts.type = GIT_PROXY_AUTO;
        auto &callbacks = options.fetch_opts.callbacks;
        callbacks.credentials = credentialsCallback;
        callbacks.certificate_check = certificateCallback;
        callbacks.transfer_progress = transferCallback;
        callbacks.sideband_progress = sidebandCallback;
        callbacks.payload = &state;

        state.nextKey = 0;
        state.offeredKey.clear();
        git_repository *repository = nullptr;
        const int code = git_clone(&repository, source.constData(), destination.constData(), &options);
        const git_error *error = code ? git_error_last() : nullptr;
        const QString detail = error && error->message ? QString::fromUtf8(error->message).trimmed() : QString();
        // The checkout keeps the URL as given.
        if (code == 0 && source != originalSource) git_remote_set_url(repository, "origin", originalSource.constData());
        git_repository_free(repository);
        if (state.canceled()) {
            removeDestination();
            return {GitCloneOutcome::Canceled, {}, {}};
        }
        if (code == 0) return {GitCloneOutcome::Cloned, {}, {}};
        removeDestination();
        // libssh2 fails the whole connection when it cannot read a key file
        // (wrong passphrase, unsupported format); try again without it.
        const QString key = state.offeredKey;
        if (!key.isEmpty() && code != GIT_EAUTH && attempt < 6
            && (detail.contains(QLatin1String("private key"), Qt::CaseInsensitive) || detail.contains(QLatin1String("passphrase"), Qt::CaseInsensitive))) {
            const bool hadPassphrase = state.passphrases.contains(key);
            if (hadPassphrase) {
                state.passphrases[key].fill(' ');
                state.passphrases.remove(key);
            }
            if (hadPassphrase && !state.retryPassphrase.contains(key)) state.retryPassphrase.insert(key);
            else state.unusableKeys.insert(key);
            state.agentTried = true;
            continue;
        }
        return {GitCloneOutcome::Failed, describeFailure(code, detail, state), detail};
    }
}

}
