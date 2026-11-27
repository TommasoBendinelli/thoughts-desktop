// SPDX-License-Identifier: GPL-2.0-or-later
#include "updatecheck.h"

#include "../part/thoughtsplatform.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>
#include <KConfigGroup>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <functional>
#include <memory>
#include <optional>

#ifndef THOUGHTS_RELEASE_VERSION
#define THOUGHTS_RELEASE_VERSION ""
#endif
#ifndef THOUGHTS_UPDATE_REPOSITORY
#define THOUGHTS_UPDATE_REPOSITORY ""
#endif

namespace Thoughts::Updates
{
namespace
{
constexpr char tokenService[] = "thoughts-github";
constexpr char tokenAccount[] = "releases";
#ifdef Q_OS_MACOS
// Releases for the Mac carry the self-contained disk image (tools/macos/release.py).
constexpr char tagPrefix[] = "mac-";
constexpr char assetPattern[] = "^Thoughts-[A-Za-z0-9._-]+\\.dmg$";
#else
constexpr char tagPrefix[] = "windows-";
constexpr char assetPattern[] = "^Thoughts-[A-Za-z0-9._-]+-setup\\.exe$";
#endif
constexpr qint64 backgroundInterval = 20 * 60 * 60;

struct Release {
    QString version;
    QString notes;
    QString pageUrl;
    QUrl assetUrl;
    QString assetName;
    qint64 assetSize = 0;
    QByteArray sha256; // hex, from the asset's "digest" when GitHub gives one
};

QString repository()
{
    return QString::fromLatin1(THOUGHTS_UPDATE_REPOSITORY);
}

bool isSha256(const QByteArray &digest)
{
    static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
    return hex.match(QString::fromLatin1(digest)).hasMatch();
}

QByteArray token()
{
    return Platform::readSecret(QString::fromLatin1(tokenService), QString::fromLatin1(tokenAccount)).trimmed();
}

QNetworkAccessManager *network()
{
    static QNetworkAccessManager *manager = new QNetworkAccessManager(qApp);
    return manager;
}

QNetworkRequest apiRequest(const QUrl &url, const QByteArray &accept)
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", accept);
    // A public repository answers without a token; one is sent only when the user stored it.
    const QByteArray secret = token();
    if (!secret.isEmpty()) {
        request.setRawHeader("Authorization", "Bearer " + secret);
    }
    // The token is GitHub's API's: a redirect to another origin is not followed with it.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Thoughts/%1").arg(currentVersion()));
    request.setTransferTimeout(30000);
    return request;
}

// The newest windows-* (mac-*) release with a setup (disk image) asset, from GET /releases.
std::optional<Release> newestRelease(const QJsonArray &releases)
{
    std::optional<Release> best;
    static const QRegularExpression setupName(QString::fromLatin1(assetPattern), QRegularExpression::CaseInsensitiveOption);
    for (const QJsonValue &value : releases) {
        const QJsonObject release = value.toObject();
        const QString tag = release.value(QLatin1String("tag_name")).toString();
        if (release.value(QLatin1String("draft")).toBool() || !tag.startsWith(QLatin1String(tagPrefix))) {
            continue;
        }
        Release candidate;
        candidate.version = tag.mid(int(qstrlen(tagPrefix)));
        candidate.notes = release.value(QLatin1String("body")).toString();
        candidate.pageUrl = release.value(QLatin1String("html_url")).toString();
        for (const QJsonValue &assetValue : release.value(QLatin1String("assets")).toArray()) {
            const QJsonObject asset = assetValue.toObject();
            const QString name = asset.value(QLatin1String("name")).toString();
            if (!setupName.match(name).hasMatch()) {
                continue;
            }
            candidate.assetName = name;
            candidate.assetUrl = QUrl(asset.value(QLatin1String("url")).toString());
            candidate.assetSize = asset.value(QLatin1String("size")).toInteger();
            const QString digest = asset.value(QLatin1String("digest")).toString();
            if (digest.startsWith(QLatin1String("sha256:"))) {
                candidate.sha256 = digest.mid(7).toLatin1().toLower();
            }
        }
        if (candidate.assetUrl.isValid() && (!best || compareVersions(candidate.version, best->version) > 0)) {
            best = candidate;
        }
    }
    return best;
}

// Calls done(release, error, refused); release is empty when there is nothing newer.
// refused: GitHub did not show the releases to this request, as it does for a private
// repository without a token that may read it.
void fetchUpdate(QObject *context, std::function<void(std::optional<Release>, QString, bool)> done)
{
    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(repository()));
    QNetworkReply *reply = network()->get(apiRequest(url, "application/vnd.github+json"));
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 || status == 403 || status == 404) {
            done({},
                 token().isEmpty() ? i18n("GitHub did not show the releases of %2 (HTTP %1).", status, repository())
                                   : i18n("GitHub refused the token (HTTP %1). It needs read access to the contents of %2.", status, repository()),
                 true);
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done({}, i18n("Could not reach GitHub: %1", reply->errorString()), false);
            return;
        }
        const std::optional<Release> release = newestRelease(QJsonDocument::fromJson(reply->readAll()).array());
        if (!release || compareVersions(release->version, currentVersion()) <= 0) {
            done({}, {}, false);
            return;
        }
        done(release, {}, false);
    });
}

bool askForToken(QWidget *parent, const QString &problem)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(i18n("Updates from GitHub"));
    auto *layout = new QVBoxLayout(&dialog);
    auto *text = new QLabel(&dialog);
    text->setWordWrap(true);
    text->setTextFormat(Qt::RichText);
    text->setOpenExternalLinks(true);
    QString html;
    if (!problem.isEmpty()) {
        html += QStringLiteral("<p><b>%1</b></p>").arg(problem.toHtmlEscaped());
    }
    html += i18n(
        "<p>New versions of Thoughts are published as releases of the GitHub repository <b>%1</b>. A public repository needs no token. "
        "If that repository is private, Thoughts needs a GitHub token that can read it.</p>"
        "<p>Create one at <a href=\"https://github.com/settings/personal-access-tokens/new\">github.com › Settings › Developer settings › "
        "Fine-grained tokens</a>: choose the repository owner, <i>Only select repositories</i> › %1, and under "
        "<i>Repository permissions</i> set <i>Contents</i> to <i>Read-only</i>.</p>",
        repository());
#ifdef Q_OS_MACOS
    html += i18n("<p>The token is stored in your login Keychain as thoughts-github/releases.</p>");
#else
    html += i18n("<p>The token is stored in Windows Credential Manager as thoughts-github/releases.</p>");
#endif
    text->setText(html);
    layout->addWidget(text);
    auto *form = new QFormLayout;
    auto *field = new QLineEdit(&dialog);
    field->setEchoMode(QLineEdit::Password);
    field->setPlaceholderText(QStringLiteral("github_pat_…"));
    form->addRow(i18n("Token:"), field);
    layout->addLayout(form);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.resize(520, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted || field->text().trimmed().isEmpty()) {
        return false;
    }
    QString error;
    if (!Platform::writeSecret(QString::fromLatin1(tokenService), QString::fromLatin1(tokenAccount), field->text().trimmed().toUtf8(), &error)) {
        KMessageBox::error(parent, i18n("Could not store the token: %1", error), i18n("Updates from GitHub"));
        return false;
    }
    return true;
}

#ifdef Q_OS_MACOS
// The bundle this process runs from: …/Thoughts.app/Contents/MacOS/thoughts.
QString runningBundle()
{
    QDir directory(QCoreApplication::applicationDirPath());
    if (!directory.cdUp() || !directory.cdUp()) {
        return {};
    }
    const QString bundle = directory.canonicalPath();
    return bundle.endsWith(QLatin1String(".app")) ? bundle : QString();
}
#endif

// Closes every window (each may ask to save or refuse while busy), then starts the
// installer, which waits for Thoughts to exit, installs over it and starts it again.
// On macOS the installer is Contents/Resources/install-update.sh, run from a
// temporary copy: it replaces this bundle with the one in the disk image only if
// that one is signed like this one, and keeps this one on any failure.
void installAndRestart(QWidget *parent, const QString &installer)
{
#ifdef Q_OS_MACOS
    const QString bundle = runningBundle();
    const QString helper = QDir(bundle).filePath(QStringLiteral("Contents/Resources/install-update.sh"));
    if (bundle.isEmpty() || !QFileInfo::exists(helper)) {
        KMessageBox::information(parent, i18n("This copy of Thoughts cannot update itself. Quit Thoughts, open %1 and drag Thoughts onto Applications.", installer), i18n("Update Thoughts"));
        return;
    }
    const QString folder = QFileInfo(bundle).absolutePath();
    // A managed Mac may refuse this account /Applications but leave it the app it owns:
    // the installer then replaces the app's contents in place (install-update.sh).
    const bool ownsBundle = QFileInfo(bundle).isWritable() && QFileInfo(QDir(bundle).filePath(QStringLiteral("Contents"))).isWritable();
    if (!QFileInfo(folder).isWritable() && !ownsBundle) {
        KMessageBox::information(parent,
                                 i18n("Thoughts cannot replace itself in %1, because this account may not change that folder. Quit Thoughts, open %2 and drag Thoughts onto Applications.",
                                      folder,
                                      installer),
                                 i18n("Update Thoughts"));
        return;
    }
    // A copy outside the bundle, so replacing the bundle cannot change the running script.
    const QString script = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("thoughts-install-update.sh"));
    QFile::remove(script);
    if (!QFile::copy(helper, script)) {
        KMessageBox::error(parent, i18n("Could not prepare the update in %1.", script), i18n("Update Thoughts"));
        return;
    }
#endif
    QApplication::closeAllWindows();
    for (QWidget *window : QApplication::topLevelWidgets()) {
        if (window->isVisible() && window->isWindow() && !window->inherits("QMenu")) {
            KMessageBox::information(parent, i18n("Thoughts is still open, so the update was not installed. It is saved at %1.", installer), i18n("Update Thoughts"));
            return;
        }
    }
#ifdef Q_OS_MACOS
    const QStringList arguments {script, QString::number(QCoreApplication::applicationPid()), installer, bundle};
    if (!QProcess::startDetached(QStringLiteral("/bin/sh"), arguments)) {
        KMessageBox::error(nullptr, i18n("Could not start the update. It is saved at %1.", installer), i18n("Update Thoughts"));
        return;
    }
#else
    if (!QProcess::startDetached(installer, {QStringLiteral("/S"), QStringLiteral("/RELAUNCH")})) {
        KMessageBox::error(nullptr, i18n("Could not start the installer %1.", installer), i18n("Update Thoughts"));
        return;
    }
#endif
    QCoreApplication::quit();
}

KConfigGroup state();

// Fetches @p release's installer (following only allowed redirects), checks its digest and
// saves it (saveInstaller). @p progress hears the percentage; @p started gets the reply (to
// cancel it); @p done gets the saved path, or an error (empty for a cancel).
void fetchInstaller(const Release &release, const std::function<void(int)> &progress, const std::function<void(QNetworkReply *)> &started,
                    const std::function<void(const QString &path, const QString &error)> &done)
{
    auto fetchFrom = std::make_shared<std::function<void(const QUrl &, bool, int)>>();
    *fetchFrom = [=](const QUrl &url, bool authorized, int redirects) {
        // The asset URL answers with a redirect to a signed download link, which
        // must be fetched without the token.
        QNetworkRequest request = authorized ? apiRequest(url, "application/octet-stream") : QNetworkRequest(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(120000);
        QNetworkReply *reply = network()->get(request);
        if (started) {
            started(reply);
        }
        QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [progress, release](qint64 received, qint64 total) {
            const qint64 size = total > 0 ? total : release.assetSize;
            if (progress && size > 0) {
                progress(int(received * 100 / size));
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400) {
                const QUrl next = url.resolved(reply->header(QNetworkRequest::LocationHeader).toUrl());
                bool sendToken = false;
                if (redirects >= 5 || !redirectAllowed(url, next, &sendToken)) {
                    done({}, i18n("The download was redirected to an address Thoughts does not follow. Nothing was installed."));
                    return;
                }
                (*fetchFrom)(next, authorized && sendToken, redirects + 1);
                return;
            }
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                done({}, {});
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                done({}, i18n("The download failed: %1", reply->errorString()));
                return;
            }
            const QByteArray bytes = reply->readAll();
            if (!installerMatches(release.sha256, bytes)) {
                done({}, i18n("The downloaded installer does not match the release's checksum. Nothing was installed."));
                return;
            }
            const QString target = saveInstaller(bytes, release.assetName);
            if (target.isEmpty()) {
                done({}, i18n("Could not save the installer in the temporary folder."));
                return;
            }
            rememberReadyInstaller(target, release.version, release.sha256);
            done(target, {});
        });
    };
    (*fetchFrom)(release.assetUrl, true, 0);
}

// The installer at @p path is downloaded: install now, or later. It is checked again just
// before it runs.
void askToInstall(QWidget *parent, const Release &release, const QString &path, bool inBackground)
{
    QString text = i18n("Thoughts %1 is downloaded. Install it now? Thoughts closes, updates and opens again.", release.version);
    if (inBackground) {
        QString notes = release.notes.trimmed();
        if (notes.size() > 1200) {
            notes = notes.left(1200) + QStringLiteral("…");
        }
        text = notes.isEmpty() ? i18n("Thoughts %1 is downloaded and ready to install. You have %2. Install it now? Thoughts closes, updates and opens again.", release.version, currentVersion())
                               : i18n("Thoughts %1 is downloaded and ready to install. You have %2. Install it now? Thoughts closes, updates and opens again.\n\n%3", release.version,
                                      currentVersion(), notes);
    }
    const auto answer = KMessageBox::questionTwoActions(parent, text, i18n("Update Thoughts"), KGuiItem(i18n("Install and Restart")), KGuiItem(i18n("Later")));
    if (answer != KMessageBox::PrimaryAction) {
        return;
    }
    if (!installerFileMatches(path, release.sha256)) {
        KMessageBox::error(parent, i18n("The downloaded installer changed since it was checked. Nothing was installed; Check for Updates downloads it again."), i18n("Update Thoughts"));
        state().deleteEntry("ReadyPath");
        return;
    }
    installAndRestart(parent, path);
}

void download(QWidget *parent, const Release &release)
{
    // The installer is not signed: its SHA-256 in the release is all that
    // ties the download to what was published. Without one nothing is run.
    if (!isSha256(release.sha256)) {
        KMessageBox::error(parent, i18n("The release of Thoughts %1 gives no SHA-256 checksum for its installer, so it is not downloaded. Install it from the release page.", release.version),
                           i18n("Update Thoughts"));
        return;
    }
    // Downloaded in the background already: only the install is left.
    const QString ready = readyInstaller(release.version, release.sha256);
    if (!ready.isEmpty()) {
        askToInstall(parent, release, ready, false);
        return;
    }
    auto *progress = new QProgressDialog(i18n("Downloading Thoughts %1…", release.version), i18n("Cancel"), 0, 100, parent);
    progress->setWindowTitle(i18n("Update Thoughts"));
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    QPointer<QProgressDialog> guard(progress);
    QPointer<QWidget> owner(parent);
    fetchInstaller(
        release,
        [guard](int percent) {
            if (guard) {
                guard->setValue(percent);
            }
        },
        [guard](QNetworkReply *reply) {
            if (guard) {
                QObject::connect(guard, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
            }
        },
        [guard, owner, release](const QString &path, const QString &error) {
            if (guard) {
                guard->close();
            }
            if (path.isEmpty()) {
                if (!error.isEmpty()) {
                    KMessageBox::error(owner, error, i18n("Update Thoughts"));
                }
                return;
            }
            askToInstall(owner, release, path, false);
        });
}

void offer(QWidget *parent, const Release &release)
{
    QString notes = release.notes.trimmed();
    if (notes.size() > 1200) {
        notes = notes.left(1200) + QStringLiteral("…");
    }
    const QString text = notes.isEmpty() ? i18n("Thoughts %1 is available. You have %2.", release.version, currentVersion())
                                         : i18n("Thoughts %1 is available. You have %2.\n\n%3", release.version, currentVersion(), notes);
    const auto answer =
        KMessageBox::questionTwoActions(parent, text, i18n("Update Thoughts"), KGuiItem(i18n("Download and Install")), KGuiItem(i18n("Later")));
    if (answer == KMessageBox::PrimaryAction) {
        download(parent, release);
    }
}

KConfigGroup state()
{
    return KSharedConfig::openConfig()->group(QStringLiteral("Updates"));
}
}

bool available()
{
    return !currentVersion().isEmpty() && !repository().isEmpty();
}

QString currentVersion()
{
    return QString::fromLatin1(THOUGHTS_RELEASE_VERSION);
}

bool installerMatches(const QByteArray &expectedSha256, const QByteArray &bytes)
{
    return isSha256(expectedSha256) && QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex() == expectedSha256;
}

QString installerFileName(const QString &assetName)
{
    // A Mac release's asset is a disk image, a Windows release's a setup program.
    const bool image = assetName.endsWith(QLatin1String(".dmg"), Qt::CaseInsensitive);
    static const QRegularExpression plain(QStringLiteral("^Thoughts-[A-Za-z0-9._-]{1,80}(-setup\\.exe|\\.dmg)$"), QRegularExpression::CaseInsensitiveOption);
    if (plain.match(assetName).hasMatch() && !assetName.contains(QLatin1String(".."))) {
        return assetName;
    }
    return image ? QStringLiteral("Thoughts.dmg") : QStringLiteral("Thoughts-setup.exe");
}

QString saveInstaller(const QByteArray &bytes, const QString &assetName)
{
    // A new folder each time: its name cannot be guessed, it is created
    // exclusively, and it belongs to this user.
    QTemporaryDir directory(QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation)).filePath(QStringLiteral("Thoughts-update-XXXXXX")));
    if (!directory.isValid()) {
        return {};
    }
    // The installer stays until it has run ("Later" keeps it for the next start).
    directory.setAutoRemove(false);
    const QString target = directory.filePath(installerFileName(assetName));
    QFile file(target);
    if (!file.open(QIODevice::WriteOnly | QIODevice::NewOnly) || file.write(bytes) != bytes.size() || !file.flush()) {
        file.close();
        QDir(directory.path()).removeRecursively();
        return {};
    }
    file.close();
    return target;
}

bool redirectAllowed(const QUrl &from, const QUrl &next, bool *sendToken)
{
    if (sendToken) {
        *sendToken = false;
    }
    if (!next.isValid() || next.scheme() != QLatin1String("https") || next.host().isEmpty() || !next.userInfo().isEmpty()) {
        return false;
    }
    if (sendToken) {
        *sendToken = from.scheme() == next.scheme() && from.host().compare(next.host(), Qt::CaseInsensitive) == 0 && from.port(443) == next.port(443);
    }
    return true;
}

int compareVersions(const QString &a, const QString &b)
{
    const QStringList left = a.split(QLatin1Char('.'));
    const QStringList right = b.split(QLatin1Char('.'));
    for (int i = 0; i < std::max(left.size(), right.size()); ++i) {
        const qlonglong x = left.value(i).toLongLong();
        const qlonglong y = right.value(i).toLongLong();
        if (x != y) {
            return x < y ? -1 : 1;
        }
    }
    return 0;
}

void checkInteractively(QWidget *parent)
{
    if (!available()) {
        return;
    }
    QPointer<QWidget> guard(parent);
    fetchUpdate(parent, [guard](std::optional<Release> release, const QString &error, bool refused) {
        QWidget *parent = guard.data();
        if (refused) {
            // A private repository: ask for a token that may read it, then check again.
            if (askForToken(parent, error)) {
                checkInteractively(parent);
            }
            return;
        }
        if (!error.isEmpty()) {
            KMessageBox::error(parent, error, i18n("Update Thoughts"));
            return;
        }
        state().writeEntry("LastCheck", QDateTime::currentSecsSinceEpoch());
        if (!release) {
            KMessageBox::information(parent, i18n("Thoughts %1 is the newest version.", currentVersion()), i18n("Update Thoughts"));
            return;
        }
        offer(parent, *release);
    });
}

void checkInBackground(QWidget *parent)
{
    if (!available() || qEnvironmentVariableIsSet("THOUGHTS_NO_UPDATE_CHECK")) {
        return;
    }
    // The installer of the version now running is not needed any more.
    forgetInstalledInstaller();
    const qint64 last = state().readEntry("LastCheck", qint64(0));
    if (QDateTime::currentSecsSinceEpoch() - last < backgroundInterval) {
        return;
    }
    QPointer<QWidget> guard(parent);
    QTimer::singleShot(30000, parent, [guard] {
        fetchUpdate(guard, [guard](std::optional<Release> release, const QString &error, bool) {
            if (!error.isEmpty() || !guard) {
                return;
            }
            state().writeEntry("LastCheck", QDateTime::currentSecsSinceEpoch());
            if (!release) {
                return;
            }
            // Downloaded at once, quietly: asked only when it is ready, so the update is only the
            // install. A download that fails is offered as before.
            const Release found = *release;
            const QString ready = isSha256(found.sha256) ? readyInstaller(found.version, found.sha256) : QString();
            if (!ready.isEmpty()) {
                askToInstall(guard, found, ready, true);
                return;
            }
            if (!isSha256(found.sha256)) {
                offer(guard, found);
                return;
            }
            fetchInstaller(found, {}, {}, [guard, found](const QString &path, const QString &) {
                if (!guard) {
                    return;
                }
                if (path.isEmpty()) {
                    offer(guard, found);
                    return;
                }
                askToInstall(guard, found, path, true);
            });
        });
    });
}

bool installerFileMatches(const QString &path, const QByteArray &sha256)
{
    QFile file(path);
    if (!isSha256(sha256) || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return false;
    }
    return hash.result().toHex() == sha256;
}

QString readyInstaller(const QString &version, const QByteArray &sha256)
{
    KConfigGroup group = state();
    const QString path = group.readEntry("ReadyPath", QString());
    if (path.isEmpty()) {
        return {};
    }
    if (group.readEntry("ReadyVersion", QString()) != version || group.readEntry("ReadySha256", QByteArray()) != sha256) {
        return {};
    }
    if (!installerFileMatches(path, sha256)) {
        // Changed or gone: never run, and not kept.
        if (QFileInfo(path).dir().dirName().startsWith(QStringLiteral("Thoughts-update-"))) {
            QDir(QFileInfo(path).absolutePath()).removeRecursively();
        }
        group.deleteEntry("ReadyPath");
        group.sync();
        return {};
    }
    return path;
}

void rememberReadyInstaller(const QString &path, const QString &version, const QByteArray &sha256)
{
    KConfigGroup group = state();
    // One remembered installer: an older one goes.
    const QString previous = group.readEntry("ReadyPath", QString());
    if (!previous.isEmpty() && previous != path && QFileInfo(previous).dir().dirName().startsWith(QStringLiteral("Thoughts-update-"))) {
        QDir(QFileInfo(previous).absolutePath()).removeRecursively();
    }
    group.writeEntry("ReadyPath", path);
    group.writeEntry("ReadyVersion", version);
    group.writeEntry("ReadySha256", sha256);
    group.sync();
}

void forgetInstalledInstaller()
{
    KConfigGroup group = state();
    const QString path = group.readEntry("ReadyPath", QString());
    if (path.isEmpty() || compareVersions(group.readEntry("ReadyVersion", QString()), currentVersion()) > 0) {
        return;
    }
    if (QFileInfo(path).dir().dirName().startsWith(QStringLiteral("Thoughts-update-"))) {
        QDir(QFileInfo(path).absolutePath()).removeRecursively();
    }
    group.deleteEntry("ReadyPath");
    group.deleteEntry("ReadyVersion");
    group.deleteEntry("ReadySha256");
    group.sync();
}
}
