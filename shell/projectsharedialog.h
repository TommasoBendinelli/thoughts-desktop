// SPDX-License-Identifier: GPL-2.0-or-later
// Share… of a Library project (thoughts-server protocol/project-links.md 14): who has the project,
// Stop sharing, pending invitations with Send again and Withdraw, and sharing with a username or
// inviting an email address. The server sends no email: an invitation goes out from this
// computer's email app, or as a copied link.
#pragma once

#include "appsignin.h"

#include <QDateTime>
#include <QDialog>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QLocale>
#include <QPointer>

#include <functional>
#include <optional>

class QHBoxLayout;
class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QPushButton;
class QVBoxLayout;

namespace Thoughts
{
namespace ProjectShares
{
struct Share {
    QString linkId;
    QString username;
};
struct Invitation {
    QString id;
    QString email;
    QDateTime createdAt;
};
struct Person {
    QString username;
    QString email;
};
struct Listing {
    // Shared with before (or joined from an invitation), without this project now: one tap shares.
    QList<Person> people;
    QString name;
    QString sharedWithYouBy;
    QList<Share> shares;
    QList<Invitation> invitations;
    int spareLibraries = 0;
};
// GET /v1/project-shares of a 200 answer.
Listing listing(const QJsonObject &answer);
// The invitation the owner sends themselves: to, subject, body and the link (14.2); empty `to`
// when the answer has none (a share by username, or an address that already has an account).
struct Mail {
    QString to;
    QString subject;
    QString body;
    QString link;
};
Mail mail(const QJsonObject &answer);
// What the person typed: a username (lowercase) or an email address; nothing when neither.
struct Target {
    bool email = false;
    QString value;
};
std::optional<Target> target(const QString &typed);
// What a refusal means to the owner, by its error_code.
QString refusal(int httpStatus, const QJsonObject &answer);
// "a@b.c · invited 24 Nov, not accepted yet", and when the address belongs to someone with an
// account (people): "… · already has the account x: share with it instead".
QString invitationLabel(const Invitation &invitation, const QList<Person> &people, const QLocale &locale = QLocale());

// A Project's link with another person's Library (GET /v1/project-links, project-links.md 9.3):
// live while seeding or active; the owner's row of an ended link is "ended", the member's "frozen".
struct LinkState {
    QString linkId;
    QString projectId;
    QString role; // "owner" or "member"
    QString status;
    QString peer;
    QString endReason; // "revoke", "leave", "owner_removed_project"
    QDateTime createdAt;
    QDateTime endedAt;
    bool live() const;
    bool ended() const;
};
// One state per Project: a live link wins (the newest), else the one that ended last.
QHash<QString, LinkState> linkStates(const QJsonObject &answer);
// What the state means, in a sentence: "Sharing with x ended on 24 Nov. …".
QString linkSentence(const LinkState &state, const QLocale &locale = QLocale());
// A request to the server with this computer's device token (Sign In); 401 without one.
void send(QNetworkAccessManager *network, const AppSignIn::Hooks &hooks, const QByteArray &method, const QString &path,
          const QJsonObject *body, QObject *context, std::function<void(int status, const QJsonObject &answer)> done);
}

class ProjectShareDialog : public QDialog
{
    Q_OBJECT

public:
    // @p project: the Library project (its "id" and "name").
    ProjectShareDialog(const QJsonObject &project, const AppSignIn::Hooks &hooks, QWidget *parent = nullptr);

private:
    friend class ProjectShareDialogTest;
    void load();
    void render(const ProjectShares::Listing &listing);
    void share();
    void shareWith(const QString &username);
    void invite(const QString &email);
    void act(const QByteArray &method, const QString &path);
    void chooseHowToSend(const ProjectShares::Mail &mail);
    void request(const QByteArray &method, const QString &path, const QJsonObject *body,
                 std::function<void(int status, const QJsonObject &answer)> done);
    void setSharing(bool enabled);

    // The Project's link state (GET /v1/project-links), when the server has links.
    std::optional<ProjectShares::LinkState> m_link;
    QString m_projectId;
    QString m_name;
    AppSignIn::Hooks m_hooks;
    QNetworkAccessManager *m_network;
    QLabel *m_status;
    QVBoxLayout *m_people;
    QLabel *m_note;
    QWidget *m_known;
    QHBoxLayout *m_knownLayout;
    QLineEdit *m_input;
    QLabel *m_error;
    QPushButton *m_share;
};
}
