// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "appsignin.h"

#include <QDialog>
#include <QPointer>
#include <QWidget>

#include <optional>

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace Thoughts
{

// Sign In (protocol/app-sign-in.md section 4): the server address, checked as it is typed, a
// username and a password, or with an invite a new password twice, and the name this computer is
// listed by. Signing in stores the tokens the server issues, saves the address and announces
// signedIn; the app then connects without a restart.
class SignInDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SignInDialog(const AppSignIn::Hooks &hooks, QWidget *parent = nullptr);

    // Fills in what a thoughts://connect link names. It never signs in by itself, and the
    // server is shown, so a link to an unexpected server is visible.
    void fill(const AppSignIn::ConnectLink &link);
    void setAddress(const QString &address);
    bool inviteMode() const
    {
        return !m_invite.isEmpty();
    }

Q_SIGNALS:
    // The tokens are stored and recorded for @p address, which is the saved server now.
    void signedIn(const QString &address, const QString &username);
    // "Use a Device Token…": the person has a device token, or the server is too old for signing in.
    void deviceTokenRequested();

private:
    void scheduleCheck(int delayMs);
    void checkServer();
    void updateState();
    void signIn();
    void setBusy(bool busy);
    QString address() const;

    AppSignIn::Hooks m_hooks;
    QNetworkAccessManager *m_network;
    QLabel *m_intro;
    QLineEdit *m_address;
    QLabel *m_addressStatus;
    QLineEdit *m_username;
    QLabel *m_passwordLabel;
    QLineEdit *m_password;
    QLabel *m_newPasswordLabel;
    QLineEdit *m_newPassword;
    QLabel *m_againLabel;
    QLineEdit *m_again;
    QLabel *m_passwordHint;
    QLineEdit *m_deviceName;
    QLabel *m_error;
    QPushButton *m_signIn;
    QTimer *m_checkTimer;
    QPointer<QNetworkReply> m_checkReply;
    quint64 m_checkSequence = 0;
    // The address the last answer was for, and what it said.
    QString m_checkedAddress;
    std::optional<AppSignIn::ServerCheck> m_check;
    QString m_invite;
    bool m_busy = false;
};

// The top of the Server dialog: which server this computer uses and whether it is signed in,
// Sign In… (Sign In to Another Server… when signed in), and with app_sign_in_v1 the devices
// signed in to this Library, each with Sign Out, and Sign Out of This Mac.
class ServerAccountSection : public QWidget
{
    Q_OBJECT

public:
    explicit ServerAccountSection(const AppSignIn::Hooks &hooks, QWidget *parent = nullptr);
    // Reads the address and the token again and asks the server.
    void refresh();
    bool signedIn() const
    {
        return !m_token.isEmpty();
    }

Q_SIGNALS:
    void signInRequested();
    // The server answered /v1/ready; TooOld means a device token is the only way in.
    void serverChecked(Thoughts::AppSignIn::ServerCheck check);
    // This computer signed out and its tokens are removed.
    void signedOut();

private:
    void loadDevices();
    void showDevices(const QList<AppSignIn::Device> &devices);
    void confirmSignOut(const AppSignIn::Device &device);
    void signOut(const AppSignIn::Device &device);

    AppSignIn::Hooks m_hooks;
    QNetworkAccessManager *m_network;
    QLabel *m_address;
    QLabel *m_state;
    QPushButton *m_signIn;
    QWidget *m_devicesBox;
    QVBoxLayout *m_devicesLayout;
    QPushButton *m_signOutThisComputer;
    QLabel *m_error;
    QString m_configured;
    QByteArray m_token;
    std::optional<AppSignIn::Device> m_current;
    quint64 m_sequence = 0;
};

}
