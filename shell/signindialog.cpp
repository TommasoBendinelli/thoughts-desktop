// SPDX-License-Identifier: GPL-2.0-or-later
#include "signindialog.h"

#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"

#include <KLocalizedString>

#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPushButton>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

namespace Thoughts
{

namespace
{
constexpr int readyTimeoutMs = 10000;
// A sign-in costs the server a password check and may wait up to 10 seconds for one.
constexpr int signInTimeoutMs = 45000;
constexpr int devicesTimeoutMs = 15000;

QString thisComputer()
{
#if defined(Q_OS_MACOS)
    return i18n("This Mac");
#elif defined(Q_OS_WIN)
    return i18n("This PC");
#else
    return i18n("This computer");
#endif
}

QString signOutOfThisComputer()
{
#if defined(Q_OS_MACOS)
    return i18n("Sign Out of This Mac");
#elif defined(Q_OS_WIN)
    return i18n("Sign Out of This PC");
#else
    return i18n("Sign Out of This Computer");
#endif
}

QString environmentAddress()
{
    return qEnvironmentVariable("THOUGHTS_SERVER_URL");
}

void showMessage(QLabel *label, const QString &text)
{
    label->setText(text);
    label->setVisible(!text.isEmpty());
}

QString devicePath(const QString &deviceId)
{
    return QStringLiteral("/v1/apps/devices/%1/sign-out").arg(deviceId);
}

bool usableDeviceId(const QString &deviceId)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9._-]{1,128}$"));
    return pattern.match(deviceId).hasMatch();
}
}

SignInDialog::SignInDialog(const AppSignIn::Hooks &hooks, QWidget *parent)
    : QDialog(parent)
    , m_hooks(hooks)
    , m_network(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("thoughtsSignInDialog"));
    setWindowTitle(i18n("Sign In"));
    setMinimumWidth(440);
    auto *layout = new QVBoxLayout(this);

    m_intro = new QLabel(this);
    m_intro->setObjectName(QStringLiteral("thoughtsSignInIntro"));
    m_intro->setWordWrap(true);
    m_intro->setTextFormat(Qt::PlainText);
    m_intro->hide();
    layout->addWidget(m_intro);

    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_address = new QLineEdit(this);
    m_address->setObjectName(QStringLiteral("thoughtsSignInServer"));
    m_address->setPlaceholderText(ServerAddress::defaultUrl());
    m_address->setAccessibleName(i18n("Server address"));
    form->addRow(i18n("Server:"), m_address);
    m_addressStatus = new QLabel(this);
    m_addressStatus->setObjectName(QStringLiteral("thoughtsSignInServerStatus"));
    m_addressStatus->setWordWrap(true);
    m_addressStatus->setTextFormat(Qt::PlainText);
    form->addRow(QString(), m_addressStatus);

    m_username = new QLineEdit(this);
    m_username->setObjectName(QStringLiteral("thoughtsSignInUsername"));
    m_username->setMaxLength(64);
    m_username->setAccessibleName(i18n("Username"));
    form->addRow(i18n("Username:"), m_username);

    const auto passwordField = [this](const QString &name, const QString &accessibleName) {
        auto *field = new QLineEdit(this);
        field->setObjectName(name);
        field->setEchoMode(QLineEdit::Password);
        field->setMaxLength(AppSignIn::maximumPasswordLength);
        field->setAccessibleName(accessibleName);
        return field;
    };
    m_passwordLabel = new QLabel(i18n("Password:"), this);
    m_password = passwordField(QStringLiteral("thoughtsSignInPassword"), i18n("Password"));
    m_passwordLabel->setBuddy(m_password);
    form->addRow(m_passwordLabel, m_password);
    m_newPasswordLabel = new QLabel(i18n("Choose a password:"), this);
    m_newPassword = passwordField(QStringLiteral("thoughtsSignInNewPassword"), i18n("Choose a password"));
    m_newPasswordLabel->setBuddy(m_newPassword);
    form->addRow(m_newPasswordLabel, m_newPassword);
    m_againLabel = new QLabel(i18n("Again:"), this);
    m_again = passwordField(QStringLiteral("thoughtsSignInNewPasswordAgain"), i18n("The new password again"));
    m_againLabel->setBuddy(m_again);
    form->addRow(m_againLabel, m_again);
    m_passwordHint = new QLabel(this);
    m_passwordHint->setObjectName(QStringLiteral("thoughtsSignInPasswordHint"));
    m_passwordHint->setWordWrap(true);
    m_passwordHint->setTextFormat(Qt::PlainText);
    form->addRow(QString(), m_passwordHint);
    for (QWidget *widget : {static_cast<QWidget *>(m_newPasswordLabel), static_cast<QWidget *>(m_newPassword), static_cast<QWidget *>(m_againLabel),
                            static_cast<QWidget *>(m_again), static_cast<QWidget *>(m_passwordHint)}) {
        widget->hide();
    }

    // The name this computer is listed by, small: most people keep it.
    auto *deviceLabel = new QLabel(AppSignIn::deviceNameLabel() + QLatin1Char(':'), this);
    m_deviceName = new QLineEdit(AppSignIn::defaultDeviceName(), this);
    m_deviceName->setObjectName(QStringLiteral("thoughtsSignInDeviceName"));
    m_deviceName->setMaxLength(AppSignIn::maximumDeviceNameLength);
    m_deviceName->setAccessibleName(AppSignIn::deviceNameLabel());
    deviceLabel->setBuddy(m_deviceName);
    QFont small = deviceLabel->font();
    small.setPointSizeF(small.pointSizeF() * 0.9);
    deviceLabel->setFont(small);
    m_deviceName->setFont(small);
    form->addRow(deviceLabel, m_deviceName);
    layout->addLayout(form);

    // No account (protocol/app-sign-in.md 5): where to ask for one, once the server has answered.
    m_access = new QLabel(this);
    m_access->setObjectName(QStringLiteral("thoughtsSignInAccess"));
    m_access->setWordWrap(true);
    m_access->setTextFormat(Qt::RichText);
    m_access->setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::LinksAccessibleByKeyboard);
    m_access->setOpenExternalLinks(false);
    m_access->hide();
    layout->addWidget(m_access);
    connect(m_access, &QLabel::linkActivated, this, &SignInDialog::requestAccess);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("thoughtsSignInError"));
    m_error->setWordWrap(true);
    m_error->setTextFormat(Qt::PlainText);
    m_error->hide();
    layout->addWidget(m_error);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    auto *useToken = buttons->addButton(i18n("Use a Device Token…"), QDialogButtonBox::HelpRole);
    useToken->setObjectName(QStringLiteral("thoughtsSignInUseToken"));
    useToken->setAutoDefault(false);
    connect(useToken, &QPushButton::clicked, this, &SignInDialog::deviceTokenRequested);
    m_signIn = buttons->addButton(i18n("Sign In"), QDialogButtonBox::AcceptRole);
    m_signIn->setObjectName(QStringLiteral("thoughtsSignInButton"));
    m_signIn->setDefault(true);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_signIn, &QPushButton::clicked, this, &SignInDialog::signIn);
    layout->addWidget(buttons);

    m_checkTimer = new QTimer(this);
    m_checkTimer->setSingleShot(true);
    connect(m_checkTimer, &QTimer::timeout, this, &SignInDialog::checkServer);
    connect(m_address, &QLineEdit::textEdited, this, [this] {
        scheduleCheck(400);
    });
    for (QLineEdit *field : {m_username, m_password, m_newPassword, m_again, m_deviceName}) {
        connect(field, &QLineEdit::textChanged, this, &SignInDialog::updateState);
    }

    if (ServerAddress::overriddenByEnvironment()) {
        m_address->setText(environmentAddress());
        m_address->setReadOnly(true);
        m_address->setToolTip(i18n("THOUGHTS_SERVER_URL sets the server for this session."));
    } else {
        const QString configured = ServerAddress::configured();
        m_address->setText(ServerAddress::normalized(configured).isEmpty() ? ServerAddress::defaultUrl() : configured);
    }
    m_username->setFocus(Qt::OtherFocusReason);
    scheduleCheck(0);
}

void SignInDialog::fill(const AppSignIn::ConnectLink &link)
{
    showMessage(m_intro, i18n("A link asks Thoughts to sign in to the server below. Sign in only if you know it."));
    if (ServerAddress::overriddenByEnvironment()) {
        if (ServerAddress::normalized(link.server) != ServerAddress::normalized(environmentAddress())) {
            showMessage(m_error, i18n("The link names %1, but THOUGHTS_SERVER_URL sets %2 for this session.", link.server, environmentAddress()));
        }
    } else {
        m_address->setText(link.server);
    }
    m_username->setText(link.username);
    m_invite = link.invite;
    const bool invite = inviteMode();
    m_passwordLabel->setVisible(!invite);
    m_password->setVisible(!invite);
    m_newPasswordLabel->setVisible(invite);
    m_newPassword->setVisible(invite);
    m_againLabel->setVisible(invite);
    m_again->setVisible(invite);
    m_passwordHint->setVisible(invite);
    if (invite) {
        m_newPassword->setFocus(Qt::OtherFocusReason);
    } else if (link.username.isEmpty()) {
        m_username->setFocus(Qt::OtherFocusReason);
    } else {
        m_password->setFocus(Qt::OtherFocusReason);
    }
    scheduleCheck(0);
}

void SignInDialog::setAddress(const QString &address)
{
    if (!ServerAddress::overriddenByEnvironment()) {
        m_address->setText(address);
        scheduleCheck(0);
    }
}

QString SignInDialog::address() const
{
    const QString text = (ServerAddress::overriddenByEnvironment() ? environmentAddress() : m_address->text()).trimmed();
    // "thoughts.example.org" means the https address of that host.
    return text.isEmpty() || text.contains(QLatin1String("://")) ? text : QStringLiteral("https://") + text;
}

void SignInDialog::scheduleCheck(int delayMs)
{
    m_check.reset();
    m_accessContact.clear();
    m_checkTimer->start(delayMs);
    updateState();
}

void SignInDialog::checkServer()
{
    // The answer to an earlier address no longer counts.
    const quint64 sequence = ++m_checkSequence;
    if (m_checkReply) {
        m_checkReply->abort();
    }
    QString error;
    const QString canonical = ServerAddress::normalized(address(), &error);
    if (canonical.isEmpty()) {
        m_addressStatus->setText(m_address->text().trimmed().isEmpty() ? QString() : error);
        updateState();
        return;
    }
    const QUrl url = AppSignIn::requestUrl(m_hooks, canonical, QStringLiteral("/v1/ready"));
    if (url.isEmpty()) {
        m_addressStatus->clear();
        updateState();
        return;
    }
    m_addressStatus->setText(i18n("Checking…"));
    m_checkReply = AppSignIn::send(m_network, "GET", url, {}, nullptr, readyTimeoutMs, this, [this, sequence, canonical](int status, const QByteArray &body, const QByteArray &) {
        if (sequence != m_checkSequence) {
            return;
        }
        m_checkedAddress = canonical;
        m_check = AppSignIn::serverCheckFromAnswer(status, body);
        m_accessContact = AppSignIn::accessContactFromAnswer(status, body);
        m_addressStatus->setText(AppSignIn::serverCheckText(*m_check));
        updateState();
    });
}

void SignInDialog::updateState()
{
    const QString canonical = ServerAddress::normalized(address());
    const bool refused = m_check && m_checkedAddress == canonical && (*m_check == AppSignIn::ServerCheck::NotFound || *m_check == AppSignIn::ServerCheck::TooOld);
    bool passwordReady = false;
    if (inviteMode()) {
        const QString password = m_newPassword->text();
        const bool length = password.size() >= AppSignIn::minimumPasswordLength && password.size() <= AppSignIn::maximumPasswordLength;
        const bool same = password == m_again->text();
        QString hint;
        if (!length) {
            hint = i18n("At least %1 characters.", AppSignIn::minimumPasswordLength);
        } else if (!m_again->text().isEmpty() && !same) {
            hint = i18n("The passwords do not match.");
        }
        m_passwordHint->setText(hint);
        passwordReady = length && same;
    } else {
        passwordReady = !m_password->text().isEmpty();
    }
    m_signIn->setEnabled(!m_busy && !canonical.isEmpty() && !refused && !m_username->text().trimmed().isEmpty() && passwordReady
                         && !AppSignIn::cleanDeviceName(m_deviceName->text()).isEmpty());
    // An invite already is a login; a server not found has nobody to ask.
    const bool found = m_check && m_checkedAddress == canonical && *m_check == AppSignIn::ServerCheck::Found;
    if (found && !inviteMode()) {
        m_access->setText(m_accessContact.isEmpty() ? i18n("No account? Ask the person who runs this server for an invite.")
                                                    : i18n("No account? <a href=\"request\">Request access</a>"));
        m_access->setToolTip(m_accessContact.isEmpty() ? QString() : i18n("Writes an email to %1 asking for an invite.", m_accessContact));
        m_access->show();
    } else {
        m_access->hide();
    }
}

void SignInDialog::requestAccess()
{
    const QString canonical = ServerAddress::normalized(address());
    if (m_accessContact.isEmpty() || canonical.isEmpty() || canonical != m_checkedAddress) {
        return;
    }
    const QUrl url = AppSignIn::accessRequestUrl(m_accessContact, canonical, m_username->text());
    if (m_hooks.openUrl) {
        m_hooks.openUrl(url);
    } else {
        QDesktopServices::openUrl(url);
    }
}

void SignInDialog::setBusy(bool busy)
{
    m_busy = busy;
    m_address->setEnabled(!busy);
    for (QLineEdit *field : {m_username, m_password, m_newPassword, m_again, m_deviceName}) {
        field->setEnabled(!busy);
    }
    updateState();
}

void SignInDialog::signIn()
{
    updateState();
    if (!m_signIn->isEnabled()) {
        return;
    }
    QString error;
    const QString canonical = ServerAddress::normalized(address(), &error);
    const QUrl url = AppSignIn::requestUrl(m_hooks, canonical, QStringLiteral("/v1/apps/sign-in"));
    if (canonical.isEmpty() || url.isEmpty()) {
        showMessage(m_error, canonical.isEmpty() ? error : AppSignIn::errorMessage(0, {}, {}, {}));
        return;
    }
    QJsonObject body {{QStringLiteral("username"), m_username->text().trimmed().toLower()},
                      {QStringLiteral("device_name"), AppSignIn::cleanDeviceName(m_deviceName->text())},
                      {QStringLiteral("platform"), AppSignIn::platform()}};
    if (inviteMode()) {
        body.insert(QStringLiteral("invite"), m_invite);
        body.insert(QStringLiteral("new_password"), m_newPassword->text());
    } else {
        body.insert(QStringLiteral("password"), m_password->text());
    }
    showMessage(m_error, i18n("Signing in…"));
    setBusy(true);
    AppSignIn::send(m_network, "POST", url, {}, &body, signInTimeoutMs, this, [this, canonical](int status, const QByteArray &answer, const QByteArray &retryAfter) {
        setBusy(false);
        QString error;
        const std::optional<AppSignIn::SignedIn> signedIn = AppSignIn::signedInFromAnswer(status, answer, retryAfter, &error);
        if (!signedIn) {
            showMessage(m_error, error);
            return;
        }
        if (!AppSignIn::storeSignedIn(m_hooks, canonical, *signedIn, &error)) {
            showMessage(m_error, i18n("Signed in, but the tokens could not be stored: %1", error));
            // The server issued a device this computer cannot use: end it again.
            const QUrl signOut = AppSignIn::requestUrl(m_hooks, canonical, devicePath(signedIn->appDeviceId));
            if (!signOut.isEmpty() && usableDeviceId(signedIn->appDeviceId)) {
                AppSignIn::send(m_network, "POST", signOut, signedIn->appToken, nullptr, devicesTimeoutMs, m_network, [](int, const QByteArray &, const QByteArray &) {});
            }
            return;
        }
        // Passwords are not kept.
        m_password->clear();
        m_newPassword->clear();
        m_again->clear();
        showMessage(m_error, {});
        Q_EMIT this->signedIn(canonical, signedIn->username);
        accept();
    });
}

ServerAccountSection::ServerAccountSection(const AppSignIn::Hooks &hooks, QWidget *parent)
    : QWidget(parent)
    , m_hooks(hooks)
    , m_network(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("thoughtsServerAccount"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *top = new QHBoxLayout;
    auto *texts = new QVBoxLayout;
    m_address = new QLabel(this);
    m_address->setObjectName(QStringLiteral("thoughtsServerAccountAddress"));
    m_address->setTextFormat(Qt::PlainText);
    m_address->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_address->setWordWrap(true);
    m_state = new QLabel(this);
    m_state->setObjectName(QStringLiteral("thoughtsServerAccountState"));
    m_state->setTextFormat(Qt::PlainText);
    m_state->setWordWrap(true);
    texts->addWidget(m_address);
    texts->addWidget(m_state);
    top->addLayout(texts, 1);
    m_signIn = new QPushButton(this);
    m_signIn->setObjectName(QStringLiteral("thoughtsServerAccountSignIn"));
    m_signIn->setAutoDefault(false);
    top->addWidget(m_signIn, 0, Qt::AlignTop);
    layout->addLayout(top);

    m_devicesBox = new QWidget(this);
    m_devicesBox->setObjectName(QStringLiteral("thoughtsServerAccountDevices"));
    auto *devices = new QVBoxLayout(m_devicesBox);
    devices->setContentsMargins(0, 6, 0, 0);
    auto *heading = new QLabel(i18n("Devices"), m_devicesBox);
    QFont bold = heading->font();
    bold.setBold(true);
    heading->setFont(bold);
    devices->addWidget(heading);
    m_devicesLayout = new QVBoxLayout;
    devices->addLayout(m_devicesLayout);
    m_signOutThisComputer = new QPushButton(signOutOfThisComputer(), m_devicesBox);
    m_signOutThisComputer->setObjectName(QStringLiteral("thoughtsServerAccountSignOutThisComputer"));
    m_signOutThisComputer->setAutoDefault(false);
    devices->addWidget(m_signOutThisComputer, 0, Qt::AlignLeft);
    m_devicesBox->hide();
    layout->addWidget(m_devicesBox);

    m_error = new QLabel(this);
    m_error->setObjectName(QStringLiteral("thoughtsServerAccountError"));
    m_error->setTextFormat(Qt::PlainText);
    m_error->setWordWrap(true);
    m_error->hide();
    layout->addWidget(m_error);

    connect(m_signIn, &QPushButton::clicked, this, &ServerAccountSection::signInRequested);
    connect(m_signOutThisComputer, &QPushButton::clicked, this, [this] {
        if (m_current) {
            confirmSignOut(*m_current);
        }
    });
    refresh();
}

void ServerAccountSection::refresh()
{
    const quint64 sequence = ++m_sequence;
    m_configured = ServerAddress::configured();
    const QString canonical = ServerAddress::normalized(m_configured);
    m_address->setText(i18n("Server: %1", canonical.isEmpty() ? m_configured : canonical));
    m_token = canonical.isEmpty() ? QByteArray() : AppSignIn::readToken(m_hooks, Thoughts::Platform::desktopDeviceAccount(), canonical);
    m_current.reset();
    m_state->setText(m_token.isEmpty() ? i18n("Not signed in.") : i18n("Signed in."));
    m_signIn->setText(m_token.isEmpty() ? i18n("Sign In…") : i18n("Sign In to Another Server…"));
    m_devicesBox->hide();
    showMessage(m_error, {});
    const QUrl url = AppSignIn::requestUrl(m_hooks, canonical, QStringLiteral("/v1/ready"));
    if (url.isEmpty()) {
        return;
    }
    AppSignIn::send(m_network, "GET", url, {}, nullptr, readyTimeoutMs, this, [this, sequence](int status, const QByteArray &body, const QByteArray &) {
        if (sequence != m_sequence) {
            return;
        }
        const AppSignIn::ServerCheck check = AppSignIn::serverCheckFromAnswer(status, body);
        if (check == AppSignIn::ServerCheck::TooOld && m_token.isEmpty()) {
            m_state->setText(AppSignIn::serverCheckText(check));
        }
        if (check == AppSignIn::ServerCheck::Found && !m_token.isEmpty()) {
            loadDevices();
        }
        Q_EMIT serverChecked(check);
    });
}

void ServerAccountSection::loadDevices()
{
    const quint64 sequence = m_sequence;
    const QUrl url = AppSignIn::requestUrl(m_hooks, m_configured, QStringLiteral("/v1/apps/devices"));
    if (url.isEmpty()) {
        return;
    }
    AppSignIn::send(m_network, "GET", url, m_token, nullptr, devicesTimeoutMs, this, [this, sequence](int status, const QByteArray &body, const QByteArray &retryAfter) {
        if (sequence != m_sequence) {
            return;
        }
        if (status == 401) {
            // Signed out from another device, or disabled on the server.
            m_state->setText(i18n("The server no longer accepts this computer's token. Sign in again."));
            m_signIn->setText(i18n("Sign In…"));
            return;
        }
        const std::optional<QList<AppSignIn::Device>> devices = status == 200 ? AppSignIn::devicesFromAnswer(body) : std::nullopt;
        if (!devices) {
            showMessage(m_error, AppSignIn::errorMessage(status, body, retryAfter, i18n("The list of devices could not be loaded.")));
            return;
        }
        showDevices(*devices);
    });
}

void ServerAccountSection::showDevices(const QList<AppSignIn::Device> &devices)
{
    while (QLayoutItem *item = m_devicesLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_current.reset();
    for (const AppSignIn::Device &device : devices) {
        if (device.current) {
            m_current = device;
        }
        auto *row = new QWidget(m_devicesBox);
        row->setObjectName(QStringLiteral("thoughtsServerAccountDevice"));
        row->setProperty("deviceId", device.id);
        auto *rowLayout = new QHBoxLayout(row);
        rowLayout->setContentsMargins(0, 0, 0, 0);
        QStringList details;
        if (device.current) {
            details.append(thisComputer());
        }
        details.append(AppSignIn::platformLabel(device.platform));
        if (device.lastSeenAt.isValid()) {
            details.append(i18n("last seen %1", QLocale().toString(device.lastSeenAt.toLocalTime(), QLocale::ShortFormat)));
        }
        auto *text = new QLabel(row);
        text->setObjectName(QStringLiteral("thoughtsServerAccountDeviceText"));
        text->setTextFormat(Qt::PlainText);
        text->setWordWrap(true);
        text->setText((device.name.isEmpty() ? AppSignIn::platformLabel(device.platform) : device.name) + QLatin1Char('\n')
                      + details.join(QStringLiteral(" · ")));
        rowLayout->addWidget(text, 1);
        if (!device.current) {
            auto *signOut = new QPushButton(i18n("Sign Out"), row);
            signOut->setObjectName(QStringLiteral("thoughtsServerAccountDeviceSignOut"));
            signOut->setAutoDefault(false);
            signOut->setEnabled(usableDeviceId(device.id));
            connect(signOut, &QPushButton::clicked, this, [this, device] {
                confirmSignOut(device);
            });
            rowLayout->addWidget(signOut, 0, Qt::AlignTop);
        }
        m_devicesLayout->addWidget(row);
    }
    if (devices.isEmpty()) {
        auto *none = new QLabel(i18n("No signed-in devices."), m_devicesBox);
        m_devicesLayout->addWidget(none);
    }
    m_signOutThisComputer->setVisible(m_current.has_value() && usableDeviceId(m_current->id));
    if (m_current && !m_current->username.isEmpty()) {
        m_state->setText(i18n("Signed in as %1.", m_current->username));
    }
    m_devicesBox->show();
}

void ServerAccountSection::confirmSignOut(const AppSignIn::Device &device)
{
    const QString name = device.name.isEmpty() ? AppSignIn::platformLabel(device.platform) : device.name;
    const QString text = device.current
        ? i18n("Thoughts on this computer stops syncing with %1 until you sign in again.", ServerAddress::origin(m_configured))
        : i18n("%1 can no longer reach this Library until it signs in again.", name);
    auto *box = new QMessageBox(QMessageBox::Question, device.current ? signOutOfThisComputer() : i18n("Sign Out"), text, QMessageBox::Cancel, this);
    box->setObjectName(QStringLiteral("thoughtsServerAccountSignOutConfirm"));
    box->setAttribute(Qt::WA_DeleteOnClose);
    QPushButton *confirm = box->addButton(i18n("Sign Out"), QMessageBox::DestructiveRole);
    confirm->setObjectName(QStringLiteral("thoughtsServerAccountSignOutConfirmButton"));
    box->setDefaultButton(QMessageBox::Cancel);
    connect(box, &QMessageBox::buttonClicked, this, [this, confirm, device](QAbstractButton *button) {
        if (button == confirm) {
            signOut(device);
        }
    });
    box->open();
}

void ServerAccountSection::signOut(const AppSignIn::Device &device)
{
    const QUrl url = AppSignIn::requestUrl(m_hooks, m_configured, devicePath(device.id));
    if (url.isEmpty() || !usableDeviceId(device.id)) {
        return;
    }
    showMessage(m_error, {});
    const quint64 sequence = m_sequence;
    AppSignIn::send(m_network, "POST", url, m_token, nullptr, devicesTimeoutMs, this, [this, sequence, device](int status, const QByteArray &body, const QByteArray &retryAfter) {
        // A 401 for this computer: its token was ended already.
        const bool done = status == 204 || status == 200 || (device.current && status == 401);
        if (!done) {
            if (sequence == m_sequence) {
                showMessage(m_error, AppSignIn::errorMessage(status, body, retryAfter, i18n("Signing out did not work.")));
            }
            return;
        }
        if (device.current) {
            QString error;
            const bool removed = AppSignIn::removeStoredTokens(m_hooks, &error);
            refresh();
            if (!removed) {
                showMessage(m_error, i18n("Signed out, but the tokens on this computer could not be removed: %1", error));
            }
            Q_EMIT signedOut();
            return;
        }
        refresh();
    });
}

}
