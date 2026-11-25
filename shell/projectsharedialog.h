// SPDX-License-Identifier: GPL-2.0-or-later
// Share… of a Library project (thoughts-server protocol/project-links.md 14): who has the project,
// Stop sharing, pending invitations with Send again and Withdraw, and sharing with a username or
// inviting an email address. The server sends no email: an invitation goes out from this
// computer's email app, or as a copied link.
#pragma once

#include "appsignin.h"

#include <QDialog>
#include <QJsonObject>
#include <QList>
#include <QPointer>

#include <optional>

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
};
struct Listing {
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
    void invite(const QString &email);
    void act(const QByteArray &method, const QString &path);
    void chooseHowToSend(const ProjectShares::Mail &mail);
    void request(const QByteArray &method, const QString &path, const QJsonObject *body,
                 std::function<void(int status, const QJsonObject &answer)> done);
    void setSharing(bool enabled);

    QString m_projectId;
    QString m_name;
    AppSignIn::Hooks m_hooks;
    QNetworkAccessManager *m_network;
    QLabel *m_status;
    QVBoxLayout *m_people;
    QLabel *m_note;
    QLineEdit *m_input;
    QLabel *m_error;
    QPushButton *m_share;
};
}
