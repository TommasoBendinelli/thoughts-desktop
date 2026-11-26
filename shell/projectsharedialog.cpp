// SPDX-License-Identifier: GPL-2.0-or-later
#include "projectsharedialog.h"
#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"

#include <KLocalizedString>
#include <algorithm>
#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QRegularExpression>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace Thoughts
{
namespace ProjectShares
{
namespace
{
QString text(const QJsonObject &object, const QString &key)
{
    const QJsonValue value = object.value(key);
    return value.isString() ? value.toString() : QString();
}

QDateTime moment(const QJsonObject &object, const QString &key)
{
    return QDateTime::fromString(text(object, key), Qt::ISODateWithMs);
}

// "24 Nov", or "24 Nov 2025" for another year, in local time.
QString shortDate(const QDateTime &when, const QLocale &locale)
{
    const QDate date = when.toLocalTime().date();
    return locale.toString(date, date.year() == QDate::currentDate().year() ? QStringLiteral("d MMM") : QStringLiteral("d MMM yyyy"));
}
}

Listing listing(const QJsonObject &answer)
{
    Listing result;
    result.name = text(answer, QStringLiteral("name"));
    result.sharedWithYouBy = text(answer, QStringLiteral("shared_with_you_by"));
    for (const QJsonValue &value : answer.value(QStringLiteral("shares")).toArray()) {
        const QJsonObject share = value.toObject();
        if (!text(share, QStringLiteral("link_id")).isEmpty()) {
            result.shares.append({text(share, QStringLiteral("link_id")), text(share, QStringLiteral("username"))});
        }
    }
    for (const QJsonValue &value : answer.value(QStringLiteral("invitations")).toArray()) {
        const QJsonObject invitation = value.toObject();
        if (!text(invitation, QStringLiteral("invitation_id")).isEmpty()) {
            result.invitations.append({text(invitation, QStringLiteral("invitation_id")), text(invitation, QStringLiteral("email")),
                                       moment(invitation, QStringLiteral("created_at"))});
        }
    }
    result.spareLibraries = answer.value(QStringLiteral("spare_libraries")).toInt(0);
    for (const QJsonValue &value : answer.value(QStringLiteral("people")).toArray()) {
        const QJsonObject person = value.toObject();
        if (!text(person, QStringLiteral("username")).isEmpty()) {
            result.people.append({text(person, QStringLiteral("username")), text(person, QStringLiteral("email"))});
        }
    }
    return result;
}

QString invitationLabel(const Invitation &invitation, const QList<Person> &people, const QLocale &locale)
{
    QString label = invitation.createdAt.isValid() ? i18n("%1 · invited %2, not accepted yet", invitation.email, shortDate(invitation.createdAt, locale))
                                                   : i18n("%1 · invited, not accepted yet", invitation.email);
    for (const Person &person : people) {
        if (!person.email.isEmpty() && person.email.compare(invitation.email, Qt::CaseInsensitive) == 0) {
            label += i18n(" · already has the account %1: share with it instead", person.username);
            break;
        }
    }
    return label;
}

bool LinkState::live() const
{
    return status == QLatin1String("seeding") || status == QLatin1String("active");
}

bool LinkState::ended() const
{
    return status == QLatin1String("ended") || status == QLatin1String("frozen");
}

QHash<QString, LinkState> linkStates(const QJsonObject &answer)
{
    QHash<QString, LinkState> result;
    for (const QJsonValue &value : answer.value(QStringLiteral("links")).toArray()) {
        const QJsonObject link = value.toObject();
        LinkState state{text(link, QStringLiteral("link_id")), text(link, QStringLiteral("project_id")), text(link, QStringLiteral("role")),
                        text(link, QStringLiteral("status")), text(link, QStringLiteral("peer_username")), text(link, QStringLiteral("end_reason")),
                        moment(link, QStringLiteral("created_at")), moment(link, QStringLiteral("ended_at"))};
        if (state.projectId.isEmpty() || (!state.live() && !state.ended())) {
            continue;
        }
        const auto known = result.constFind(state.projectId);
        const bool better = known == result.constEnd()
            || (state.live() && (!known->live() || state.createdAt > known->createdAt))
            || (!state.live() && !known->live() && state.endedAt > known->endedAt);
        if (better) {
            result.insert(state.projectId, state);
        }
    }
    return result;
}

QString linkSentence(const LinkState &state, const QLocale &locale)
{
    const bool owner = state.role == QLatin1String("owner");
    if (state.live()) {
        return owner ? i18n("Shared with %1.", state.peer) : i18n("%1 shared this project with you.", state.peer);
    }
    if (!state.ended()) {
        return QString();
    }
    const QString date = shortDate(state.endedAt, locale);
    if (owner) {
        const QString what = state.endReason == QLatin1String("leave") ? i18n("%1 left this project on %2.", state.peer, date)
                                                                       : i18n("Sharing with %1 ended on %2.", state.peer, date);
        return what + QLatin1Char(' ') + i18n("Changes either of you make now stay on your own side.");
    }
    const QString what = state.endReason == QLatin1String("leave")                 ? i18n("You left this project on %1.", date)
        : state.endReason == QLatin1String("owner_removed_project")                 ? i18n("%1 removed this project on %2.", state.peer, date)
                                                                                      : i18n("%1 stopped sharing this project on %2.", state.peer, date);
    return what + QLatin1Char(' ') + i18n("This is your own copy now: your changes stay here and no longer reach %1.", state.peer);
}

void send(QNetworkAccessManager *network, const AppSignIn::Hooks &hooks, const QByteArray &method, const QString &path,
          const QJsonObject *body, QObject *context, std::function<void(int status, const QJsonObject &answer)> done)
{
    const QString configured = ServerAddress::configured();
    const QString canonical = ServerAddress::normalized(configured);
    const QByteArray token = canonical.isEmpty() ? QByteArray() : AppSignIn::readToken(hooks, Platform::desktopDeviceAccount(), canonical);
    // requestUrl takes a path: the query goes on afterwards, or its "?" would be escaped.
    QUrl url = AppSignIn::requestUrl(hooks, configured, path.section(QLatin1Char('?'), 0, 0));
    if (path.contains(QLatin1Char('?'))) {
        url.setQuery(path.section(QLatin1Char('?'), 1), QUrl::StrictMode);
    }
    if (url.isEmpty() || token.isEmpty()) {
        done(401, {});
        return;
    }
    AppSignIn::send(network, method, url, token, body, 30000, context, [done](int status, const QByteArray &answer, const QByteArray &) {
        done(status, QJsonDocument::fromJson(answer).object());
    });
}

Mail mail(const QJsonObject &answer)
{
    const QJsonObject email = answer.value(QStringLiteral("email")).toObject();
    return {text(email, QStringLiteral("to")), text(email, QStringLiteral("subject")), text(email, QStringLiteral("body")),
            text(answer, QStringLiteral("invite_url"))};
}

std::optional<Target> target(const QString &typed)
{
    static const QRegularExpression username(QStringLiteral("^[a-z0-9][a-z0-9._-]{2,31}$"));
    static const QRegularExpression email(QStringLiteral(R"(^[^@\s<>"',;:()\[\]\\]{1,64}@[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?(?:\.[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)+$)"));
    const QString value = typed.trimmed();
    if (value.contains(QLatin1Char('@'))) {
        return email.match(value).hasMatch() ? std::optional<Target>(Target {true, value}) : std::nullopt;
    }
    const QString name = value.toLower();
    return username.match(name).hasMatch() ? std::optional<Target>(Target {false, name}) : std::nullopt;
}

QString refusal(int httpStatus, const QJsonObject &answer)
{
    const QString code = text(answer, QStringLiteral("error_code"));
    const QString detail = text(answer, QStringLiteral("detail"));
    if (code == QLatin1String("project_link_unknown_member")) {
        return i18n("No Thoughts account has this username. To invite someone new, type their email address.");
    }
    if (code == QLatin1String("project_link_unknown_project")) {
        return i18n("The server does not have this project yet. Sync, then share it.");
    }
    if (code == QLatin1String("project_link_exists") || code == QLatin1String("invitation_pending")) {
        return detail.isEmpty() ? i18n("This project is already shared with someone, or it was shared with you; only its owner can share it.") : detail;
    }
    if (code == QLatin1String("project_link_collision")) {
        return i18n("That person's Library already has a project or files with the same names, so it cannot get this one.");
    }
    if (code == QLatin1String("project_link_sandbox_off")) {
        return i18n("The server cannot share this project safely right now (its builds run without the sandbox).");
    }
    if (code == QLatin1String("invalid_email")) {
        return i18n("Type a valid email address, e.g. name@example.org.");
    }
    if (code == QLatin1String("sharing_unavailable")) {
        return i18n("This server cannot share projects between people yet.");
    }
    if (httpStatus == 0) {
        return i18n("The server could not be reached.");
    }
    if (httpStatus == 401) {
        return i18n("This computer is not signed in to the server. Sign In… in Server… first.");
    }
    return detail.isEmpty() ? i18n("The server answered HTTP %1.", httpStatus) : detail;
}
}

ProjectShareDialog::ProjectShareDialog(const QJsonObject &project, const AppSignIn::Hooks &hooks, QWidget *parent)
    : QDialog(parent)
    , m_projectId(project.value(QStringLiteral("id")).toString())
    , m_name(project.value(QStringLiteral("name")).toString())
    , m_hooks(hooks)
    , m_network(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("projectShareDialog"));
    setWindowTitle(i18n("Share “%1”", m_name));
    setMinimumWidth(460);
    auto *layout = new QVBoxLayout(this);
    m_status = new QLabel(i18n("Loading…"), this);
    m_status->setObjectName(QStringLiteral("projectShareStatus"));
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *people = new QWidget(this);
    m_people = new QVBoxLayout(people);
    m_people->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(people);
    m_note = new QLabel(this);
    m_note->setWordWrap(true);
    m_note->setVisible(false);
    layout->addWidget(m_note);
    // The people shared with before: one tap shares with them (no name to remember).
    m_known = new QWidget(this);
    m_known->setObjectName(QStringLiteral("projectShareKnownPeople"));
    m_knownLayout = new QHBoxLayout(m_known);
    m_knownLayout->setContentsMargins(0, 0, 0, 0);
    m_known->setVisible(false);
    layout->addWidget(m_known);
    m_input = new QLineEdit(this);
    m_input->setObjectName(QStringLiteral("projectShareInput"));
    m_input->setPlaceholderText(i18n("Username or email address"));
    layout->addWidget(m_input);
    auto *help = new QLabel(i18n("Someone with a Thoughts account: type their username. Anyone else: type their email address, "
                                 "and send them the invitation by email or as a link."),
                            this);
    help->setWordWrap(true);
    QFont small = help->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    help->setFont(small);
    layout->addWidget(help);
    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("projectShareError"));
    m_error->setWordWrap(true);
    m_error->setVisible(false);
    layout->addWidget(m_error);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    m_share = buttons->addButton(i18n("Share"), QDialogButtonBox::AcceptRole);
    m_share->setObjectName(QStringLiteral("projectShareButton"));
    m_share->setDefault(true);
    // Share keeps the dialog open: it shows the new person in the list.
    connect(m_share, &QPushButton::clicked, this, &ProjectShareDialog::share);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_input, &QLineEdit::returnPressed, this, &ProjectShareDialog::share);
    layout->addWidget(buttons);
    setSharing(false);
    load();
}

void ProjectShareDialog::request(const QByteArray &method, const QString &path, const QJsonObject *body,
                                 std::function<void(int status, const QJsonObject &answer)> done)
{
    ProjectShares::send(m_network, m_hooks, method, path, body, this, std::move(done));
}

void ProjectShareDialog::setSharing(bool enabled)
{
    m_input->setEnabled(enabled);
    m_share->setEnabled(enabled);
}

void ProjectShareDialog::load()
{
    // The link first (a share that ended says so; a server without links answers 404), then who has it.
    request("GET", QStringLiteral("/v1/project-links"), nullptr, [this](int linksStatus, const QJsonObject &links) {
        m_link.reset();
        if (linksStatus == 200) {
            const auto states = ProjectShares::linkStates(links);
            if (const auto found = states.constFind(m_projectId); found != states.constEnd()) {
                m_link = *found;
            }
        }
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("project_id"), m_projectId);
        request("GET", QStringLiteral("/v1/project-shares?") + query.toString(QUrl::FullyEncoded), nullptr, [this](int status, const QJsonObject &answer) {
            if (status != 200) {
                m_status->setText(ProjectShares::refusal(status, answer));
                setSharing(false);
                return;
            }
            render(ProjectShares::listing(answer));
        });
    });
}

void ProjectShareDialog::render(const ProjectShares::Listing &listing)
{
    while (QLayoutItem *item = m_people->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!listing.sharedWithYouBy.isEmpty()) {
        m_status->setText(i18n("%1 shared this project with you. Only %1 can share it with others.", listing.sharedWithYouBy));
        setSharing(false);
        return;
    }
    setSharing(true);
    // Not shared now, but it was: say so first (a pending invitation is not a share).
    QStringList status;
    if (listing.shares.isEmpty() && m_link && m_link->ended()) {
        status.append(ProjectShares::linkSentence(*m_link));
        const bool oneTap = std::any_of(listing.people.cbegin(), listing.people.cend(), [this](const ProjectShares::Person &person) {
            return person.username == m_link->peer;
        });
        if (m_link->role == QLatin1String("owner") && oneTap) {
            status.append(i18n("Tap %1 below to share again: what either of you changed since then comes over.", m_link->peer));
        }
    }
    if (!listing.shares.isEmpty()) {
        status.append(i18n("Shared with:"));
    } else if (!listing.invitations.isEmpty()) {
        status.append(i18n("Not shared yet. Invitation waiting:"));
    } else if (status.isEmpty()) {
        status.append(i18n("Only you have this project."));
    }
    m_status->setText(status.join(QStringLiteral("\n\n")));
    const auto row = [this](const QString &label, const QString &objectName) {
        auto *line = new QWidget;
        line->setObjectName(objectName);
        auto *layout = new QHBoxLayout(line);
        layout->setContentsMargins(0, 0, 0, 0);
        auto *text = new QLabel(label, line);
        text->setWordWrap(true);
        layout->addWidget(text, 1);
        m_people->addWidget(line);
        return layout;
    };
    for (const ProjectShares::Share &share : listing.shares) {
        QHBoxLayout *line = row(share.username, QStringLiteral("projectShareRow"));
        auto *stop = new QPushButton(i18n("Stop Sharing…"));
        stop->setObjectName(QStringLiteral("projectShareStop"));
        connect(stop, &QPushButton::clicked, this, [this, share] {
            const auto answer = QMessageBox::question(this, i18n("Stop sharing with %1?", share.username),
                                                      i18n("%1 keeps a copy of “%2”, but it no longer changes with yours, and their changes no longer reach you. "
                                                           "Sharing it with them again later picks up that copy.",
                                                           share.username, m_name),
                                                      QMessageBox::Cancel | QMessageBox::Ok, QMessageBox::Cancel);
            if (answer == QMessageBox::Ok) {
                act("DELETE", QStringLiteral("/v1/project-shares/") + QString::fromLatin1(QUrl::toPercentEncoding(share.linkId)));
            }
        });
        line->addWidget(stop);
    }
    for (const ProjectShares::Invitation &invitation : listing.invitations) {
        QHBoxLayout *line = row(ProjectShares::invitationLabel(invitation, listing.people), QStringLiteral("projectShareInvitationRow"));
        // An invitation's link is answered once: Send again makes a new one for the same address.
        auto *again = new QPushButton(i18n("Send Again"));
        again->setObjectName(QStringLiteral("projectShareSendAgain"));
        connect(again, &QPushButton::clicked, this, [this, invitation] { invite(invitation.email); });
        line->addWidget(again);
        auto *withdraw = new QPushButton(i18n("Withdraw"));
        withdraw->setObjectName(QStringLiteral("projectShareWithdraw"));
        connect(withdraw, &QPushButton::clicked, this, [this, invitation] {
            act("DELETE", QStringLiteral("/v1/project-invitations/") + QString::fromLatin1(QUrl::toPercentEncoding(invitation.id)));
        });
        line->addWidget(withdraw);
    }
    while (QLayoutItem *item = m_knownLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    if (!listing.people.isEmpty()) {
        m_knownLayout->addWidget(new QLabel(i18n("Share with:"), m_known));
        for (const ProjectShares::Person &person : listing.people) {
            auto *button = new QPushButton(person.username, m_known);
            button->setObjectName(QStringLiteral("projectShareKnownPerson"));
            button->setToolTip(person.email.isEmpty() ? i18n("Share with %1", person.username) : i18n("Share with %1 (%2)", person.username, person.email));
            connect(button, &QPushButton::clicked, this, [this, name = person.username] { shareWith(name); });
            m_knownLayout->addWidget(button);
        }
        m_knownLayout->addStretch();
    }
    m_known->setVisible(!listing.people.isEmpty());
    const bool noRoom = !listing.invitations.isEmpty() && listing.spareLibraries <= 0;
    m_note->setText(noRoom ? i18n("The server has no room prepared for a new account. The invited person can open the link, but can make "
                                  "their account only after the server's admin prepares one.")
                           : QString());
    m_note->setVisible(noRoom);
}

void ProjectShareDialog::share()
{
    const std::optional<ProjectShares::Target> target = ProjectShares::target(m_input->text());
    if (!target) {
        m_error->setText(i18n("Type a username (3 to 32 letters, digits, '.', '_' or '-') or an email address."));
        m_error->setVisible(true);
        return;
    }
    m_error->setVisible(false);
    if (target->email) {
        invite(target->value);
        return;
    }
    shareWith(target->value);
}

void ProjectShareDialog::shareWith(const QString &username)
{
    m_error->setVisible(false);
    setSharing(false);
    const QJsonObject body {{QStringLiteral("project_id"), m_projectId}, {QStringLiteral("username"), username}};
    request("POST", QStringLiteral("/v1/project-shares"), &body, [this, name = username](int status, const QJsonObject &answer) {
        setSharing(true);
        if (status != 200) {
            m_error->setText(ProjectShares::refusal(status, answer));
            m_error->setVisible(true);
            return;
        }
        m_input->clear();
        m_status->setText(i18n("Shared with %1. They get it with their next Sync.", name));
        load();
    });
}

void ProjectShareDialog::invite(const QString &email)
{
    setSharing(false);
    const QJsonObject body {{QStringLiteral("project_id"), m_projectId}, {QStringLiteral("email"), email}};
    request("POST", QStringLiteral("/v1/project-shares"), &body, [this](int status, const QJsonObject &answer) {
        setSharing(true);
        if (status != 200) {
            m_error->setText(ProjectShares::refusal(status, answer));
            m_error->setVisible(true);
            return;
        }
        m_input->clear();
        const ProjectShares::Mail invitation = ProjectShares::mail(answer);
        // An address that joined from an earlier invitation has an account: shared at once.
        if (invitation.to.isEmpty()) {
            const QString username = answer.value(QStringLiteral("username")).toString();
            if (!username.isEmpty()) {
                m_status->setText(i18n("Shared with %1. They get it with their next Sync.", username));
            }
        } else {
            chooseHowToSend(invitation);
        }
        load();
    });
}

void ProjectShareDialog::act(const QByteArray &method, const QString &path)
{
    request(method, path, nullptr, [this](int status, const QJsonObject &answer) {
        if (status < 200 || status > 299) {
            m_error->setText(ProjectShares::refusal(status, answer));
            m_error->setVisible(true);
        }
        load();
    });
}

void ProjectShareDialog::chooseHowToSend(const ProjectShares::Mail &mail)
{
    // The server sends nothing: until the invitation is sent, nobody receives anything.
    QMessageBox box(this);
    box.setObjectName(QStringLiteral("projectShareSendInvitation"));
    box.setWindowTitle(i18n("Send the Invitation"));
    box.setText(i18n("Send the invitation to %1.", mail.to));
    box.setInformativeText(i18n("The server does not email it: send it from your email app, or copy the link and send it any other way. "
                                "The link works once."));
    QPushButton *email = box.addButton(i18n("Email…"), QMessageBox::AcceptRole);
    email->setObjectName(QStringLiteral("projectShareEmail"));
    QPushButton *copy = box.addButton(i18n("Copy Link"), QMessageBox::ActionRole);
    copy->setObjectName(QStringLiteral("projectShareCopyLink"));
    box.addButton(i18n("Later"), QMessageBox::RejectRole);
    box.setDefaultButton(email);
    box.exec();
    if (box.clickedButton() == email) {
        const QByteArray encoded = QByteArray("mailto:") + QUrl::toPercentEncoding(mail.to, "@") + QByteArray("?subject=") + QUrl::toPercentEncoding(mail.subject)
            + QByteArray("&body=") + QUrl::toPercentEncoding(mail.body);
        const QUrl url = QUrl::fromEncoded(encoded);
        if (!QDesktopServices::openUrl(url)) {
            QApplication::clipboard()->setText(mail.body);
            QMessageBox::information(this, i18n("Send the Invitation"),
                                     i18n("No email app opened. The invitation is on the clipboard: paste it into a message to %1.", mail.to));
        }
    } else if (box.clickedButton() == copy) {
        QApplication::clipboard()->setText(mail.link.isEmpty() ? mail.body : mail.link);
        m_status->setText(i18n("Invitation link copied. Paste it to %1; it works once.", mail.to));
    }
}
}
