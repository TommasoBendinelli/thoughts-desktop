// SPDX-License-Identifier: GPL-2.0-or-later
#include "updatecheck.h"

#include "../part/thoughtsplatform.h"

#include <KLocalizedString>
#include <KMessageBox>
#include <KSharedConfig>
#include <KConfigGroup>
#include <QApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
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
constexpr char tagPrefix[] = "windows-";
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
    request.setRawHeader("Authorization", "Bearer " + token());
    // The token is GitHub's API's: a redirect to another origin is not followed with it.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::SameOriginRedirectPolicy);
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Thoughts/%1").arg(currentVersion()));
    request.setTransferTimeout(30000);
    return request;
}

// The newest windows-* release with a setup asset, from GET /releases.
std::optional<Release> newestRelease(const QJsonArray &releases)
{
    std::optional<Release> best;
    static const QRegularExpression setupName(QStringLiteral("^Thoughts-[A-Za-z0-9._-]+-setup\\.exe$"), QRegularExpression::CaseInsensitiveOption);
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

// Calls done(release, error); release is empty when there is nothing newer.
void fetchUpdate(QObject *context, std::function<void(std::optional<Release>, QString)> done)
{
    const QUrl url(QStringLiteral("https://api.github.com/repos/%1/releases?per_page=30").arg(repository()));
    QNetworkReply *reply = network()->get(apiRequest(url, "application/vnd.github+json"));
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, done] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 401 || status == 403 || status == 404) {
            done({}, i18n("GitHub refused the token (HTTP %1). It needs read access to the contents of %2.", status, repository()));
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            done({}, i18n("Could not reach GitHub: %1", reply->errorString()));
            return;
        }
        const std::optional<Release> release = newestRelease(QJsonDocument::fromJson(reply->readAll()).array());
        if (!release || compareVersions(release->version, currentVersion()) <= 0) {
            done({}, {});
            return;
        }
        done(release, {});
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
        "<p>New versions of Thoughts for Windows are published as releases of the private GitHub repository <b>%1</b>. "
        "To check for them, Thoughts needs a GitHub token that can read that repository.</p>"
        "<p>Create one at <a href=\"https://github.com/settings/personal-access-tokens/new\">github.com › Settings › Developer settings › "
        "Fine-grained tokens</a>: choose the repository owner, <i>Only select repositories</i> › %1, and under "
        "<i>Repository permissions</i> set <i>Contents</i> to <i>Read-only</i>.</p>"
        "<p>The token is stored in Windows Credential Manager as thoughts-github/releases.</p>",
        repository());
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

// Closes every window (each may ask to save or refuse while busy), then starts the
// installer, which waits for Thoughts to exit, installs over it and starts it again.
void installAndRestart(QWidget *parent, const QString &installer)
{
    QApplication::closeAllWindows();
    for (QWidget *window : QApplication::topLevelWidgets()) {
        if (window->isVisible() && window->isWindow() && !window->inherits("QMenu")) {
            KMessageBox::information(parent, i18n("Thoughts is still open, so the update was not installed. It is saved at %1.", installer), i18n("Update Thoughts"));
            return;
        }
    }
    if (!QProcess::startDetached(installer, {QStringLiteral("/S"), QStringLiteral("/RELAUNCH")})) {
        KMessageBox::error(nullptr, i18n("Could not start the installer %1.", installer), i18n("Update Thoughts"));
        return;
    }
    QCoreApplication::quit();
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
    auto *progress = new QProgressDialog(i18n("Downloading Thoughts %1…", release.version), i18n("Cancel"), 0, 100, parent);
    progress->setWindowTitle(i18n("Update Thoughts"));
    progress->setAttribute(Qt::WA_DeleteOnClose);
    progress->setMinimumDuration(0);
    progress->setValue(0);
    QPointer<QProgressDialog> guard(progress);

    auto fail = [parent, guard](const QString &message) {
        if (guard) {
            guard->close();
        }
        KMessageBox::error(parent, message, i18n("Update Thoughts"));
    };
    auto fetchFrom = std::make_shared<std::function<void(const QUrl &, bool, int)>>();
    *fetchFrom = [=](const QUrl &url, bool authorized, int redirects) {
        // The asset URL answers with a redirect to a signed download link, which
        // must be fetched without the token.
        QNetworkRequest request = authorized ? apiRequest(url, "application/octet-stream") : QNetworkRequest(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
        request.setTransferTimeout(120000);
        QNetworkReply *reply = network()->get(request);
        if (guard) {
            QObject::connect(guard, &QProgressDialog::canceled, reply, &QNetworkReply::abort);
        }
        QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [guard, release](qint64 received, qint64 total) {
            const qint64 size = total > 0 ? total : release.assetSize;
            if (guard && size > 0) {
                guard->setValue(int(received * 100 / size));
            }
        });
        QObject::connect(reply, &QNetworkReply::finished, reply, [=] {
            reply->deleteLater();
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status >= 300 && status < 400) {
                const QUrl next = url.resolved(reply->header(QNetworkRequest::LocationHeader).toUrl());
                bool sendToken = false;
                if (redirects >= 5 || !redirectAllowed(url, next, &sendToken)) {
                    fail(i18n("The download was redirected to an address Thoughts does not follow. Nothing was installed."));
                    return;
                }
                (*fetchFrom)(next, authorized && sendToken, redirects + 1);
                return;
            }
            if (reply->error() == QNetworkReply::OperationCanceledError) {
                return;
            }
            if (reply->error() != QNetworkReply::NoError) {
                fail(i18n("The download failed: %1", reply->errorString()));
                return;
            }
            const QByteArray bytes = reply->readAll();
            if (!installerMatches(release.sha256, bytes)) {
                fail(i18n("The downloaded installer does not match the release's checksum. Nothing was installed."));
                return;
            }
            const QString target = saveInstaller(bytes, release.assetName);
            if (target.isEmpty()) {
                fail(i18n("Could not save the installer in the temporary folder."));
                return;
            }
            if (guard) {
                guard->close();
            }
            const auto answer = KMessageBox::questionTwoActions(parent,
                                                                i18n("Thoughts %1 is downloaded. Install it now? Thoughts closes, updates and opens again.", release.version),
                                                                i18n("Update Thoughts"),
                                                                KGuiItem(i18n("Install and Restart")),
                                                                KGuiItem(i18n("Later")));
            if (answer == KMessageBox::PrimaryAction) {
                installAndRestart(parent, target);
            }
        });
    };
    (*fetchFrom)(release.assetUrl, true, 0);
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
    static const QRegularExpression plain(QStringLiteral("^Thoughts-[A-Za-z0-9._-]{1,80}-setup\\.exe$"), QRegularExpression::CaseInsensitiveOption);
    return plain.match(assetName).hasMatch() && !assetName.contains(QLatin1String("..")) ? assetName : QStringLiteral("Thoughts-setup.exe");
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
    if (token().isEmpty() && !askForToken(parent, {})) {
        return;
    }
    QPointer<QWidget> guard(parent);
    fetchUpdate(parent, [guard](std::optional<Release> release, const QString &error) {
        QWidget *parent = guard.data();
        if (!error.isEmpty()) {
            if (askForToken(parent, error)) {
                checkInteractively(parent);
            }
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
    if (!available() || token().isEmpty() || qEnvironmentVariableIsSet("THOUGHTS_NO_UPDATE_CHECK")) {
        return;
    }
    const qint64 last = state().readEntry("LastCheck", qint64(0));
    if (QDateTime::currentSecsSinceEpoch() - last < backgroundInterval) {
        return;
    }
    QPointer<QWidget> guard(parent);
    QTimer::singleShot(30000, parent, [guard] {
        fetchUpdate(guard, [guard](std::optional<Release> release, const QString &error) {
            if (!error.isEmpty() || !guard) {
                return;
            }
            state().writeEntry("LastCheck", QDateTime::currentSecsSinceEpoch());
            if (release) {
                offer(guard, *release);
            }
        });
    });
}
}
