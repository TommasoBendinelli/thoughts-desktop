// SPDX-License-Identifier: GPL-2.0-or-later
#include "problemreportdialog.h"
#include "../part/thoughtsplatform.h"
#include "../part/thoughtsserveraddress.h"
#include <KLocalizedString>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSysInfo>
#include <QTabWidget>
#include <QVBoxLayout>
#include <QWidget>

namespace Thoughts
{
namespace ProblemReports
{
QImage grabWindow(QWidget *window)
{
    if (!window) return {};
    return window->window()->grab().toImage();
}

QByteArray encodedPicture(const QImage &image)
{
    if (image.isNull()) return {};
    QImage scaled = image;
    if (qMax(image.width(), image.height()) > MaxPictureSide) {
        scaled = image.scaled(MaxPictureSide, MaxPictureSide, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    scaled.save(&buffer, "PNG");
    if (bytes.size() > 6 * 1024 * 1024) {
        // A photo-like window (a scanned PDF) compresses badly as PNG.
        bytes.clear();
        buffer.seek(0);
        scaled.convertToFormat(QImage::Format_RGB32).save(&buffer, "JPEG", 85);
    }
    return bytes;
}

QString fileTail(const QString &path, qsizetype bytes)
{
    QFile file(path);
    if (bytes <= 0 || !file.open(QIODevice::ReadOnly)) return {};
    if (file.size() > bytes) {
        file.seek(file.size() - bytes);
        file.readLine(); // from the start of a line
    }
    return QString::fromUtf8(file.readAll());
}

QList<Log> fitted(QList<Log> logs)
{
    QList<Log> kept;
    qsizetype left = MaxLogBytes;
    for (Log &log : logs) {
        if (log.text.isEmpty() || left <= 0) continue;
        QByteArray utf8 = log.text.toUtf8();
        if (utf8.size() > left) {
            // The limit is reached: this log keeps its newest whole lines, and the logs after it are left out.
            utf8 = utf8.right(left);
            const qsizetype newline = utf8.indexOf('\n');
            utf8 = newline >= 0 ? utf8.mid(newline + 1) : QByteArray();
            if (!utf8.isEmpty()) kept.append({log.name, QString::fromUtf8(utf8)});
            break;
        }
        left -= utf8.size();
        kept.append(log);
    }
    return kept;
}

static QString appVersion()
{
#ifdef THOUGHTS_RELEASE_VERSION
    // A published release names itself (shell/CMakeLists.txt); a local build has the app's version.
    if (const QString release = QString::fromLatin1(THOUGHTS_RELEASE_VERSION); !release.isEmpty()) return release;
#endif
    return QCoreApplication::applicationVersion();
}

QJsonObject body(const QString &kind, const QString &description, const QByteArray &picture, const QList<Log> &logs)
{
    const QString version = appVersion();
    QJsonArray logArray;
    for (const Log &log : logs) {
        logArray.append(QJsonObject{{QStringLiteral("name"), log.name}, {QStringLiteral("text"), log.text}});
    }
    return {
        {QStringLiteral("kind"), kind},
        {QStringLiteral("description"), description.trimmed()},
        {QStringLiteral("client_time"), QDateTime::currentDateTime().toString(Qt::ISODate)},
        {QStringLiteral("app"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("thoughts-desktop")},
                     {QStringLiteral("version"), version},
                     {QStringLiteral("platform"), AppSignIn::platform()},
                     {QStringLiteral("os"), QString(QSysInfo::prettyProductName() + QStringLiteral(" (") + QSysInfo::currentCpuArchitecture() + QLatin1Char(')'))}}},
        {QStringLiteral("screenshot"), picture.isEmpty() ? QJsonValue() : QJsonValue(QJsonObject{{QStringLiteral("data"), QString::fromLatin1(picture.toBase64())}})},
        {QStringLiteral("logs"), logArray},
    };
}

QString failure(int status, const QByteArray &answer)
{
    const QString detail = QJsonDocument::fromJson(answer).object().value(QStringLiteral("detail")).toString();
    if (status == 0) return i18n("Could not reach the Thoughts server. Check the connection and try again.");
    if (status == 401) return i18n("Sign in to your Thoughts server first (File › Server…).");
    if (status == 404) return i18n("This Thoughts server cannot take problem reports yet.");
    if (status == 413) return detail.isEmpty() ? i18n("The report is too large. Leave out the logs and try again.") : detail;
    if (!detail.isEmpty()) return detail;
    return i18n("The server could not keep the report (%1). Try again later.", status);
}
}

using namespace ProblemReports;

ProblemReportDialog::ProblemReportDialog(const QImage &window, std::function<QList<Log>()> collectLogs, AppSignIn::Hooks hooks, QWidget *parent)
    : QDialog(parent)
    , m_window(window)
    , m_logs(fitted(collectLogs ? collectLogs() : QList<Log>{}))
    , m_hooks(std::move(hooks))
    , m_network(new QNetworkAccessManager(this))
{
    setObjectName(QStringLiteral("thoughtsProblemReportDialog"));
    setWindowTitle(i18n("Report Bug/Feature"));
    auto *layout = new QVBoxLayout(this);

    auto *intro = new QLabel(i18n("Your report goes to the person who runs this Thoughts server."), this);
    intro->setWordWrap(true);
    layout->addWidget(intro);

    m_kind = new QComboBox(this);
    m_kind->setObjectName(QStringLiteral("problemReportKind"));
    m_kind->addItem(i18n("Something does not work"), QStringLiteral("problem"));
    m_kind->addItem(i18n("An idea or a wish"), QStringLiteral("idea"));
    layout->addWidget(m_kind);

    m_description = new QPlainTextEdit(this);
    m_description->setObjectName(QStringLiteral("problemReportDescription"));
    m_description->setPlaceholderText(i18n("What happened, and what did you expect? The more exact, the easier to fix."));
    m_description->setMinimumSize(460, 140);
    layout->addWidget(m_description);

    auto *pictureRow = new QHBoxLayout;
    auto *thumbnail = new QLabel(this);
    thumbnail->setObjectName(QStringLiteral("problemReportPicture"));
    if (!m_window.isNull()) {
        thumbnail->setPixmap(QPixmap::fromImage(m_window.scaled(200, 130, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
        thumbnail->setToolTip(i18n("The Thoughts window as it was when you chose Report Bug/Feature…"));
    }
    m_includePicture = new QCheckBox(i18n("Include this picture of the Thoughts window"), this);
    m_includePicture->setObjectName(QStringLiteral("problemReportIncludePicture"));
    m_includePicture->setChecked(!m_window.isNull());
    m_includePicture->setEnabled(!m_window.isNull());
    connect(m_includePicture, &QCheckBox::toggled, thumbnail, &QWidget::setEnabled);
    pictureRow->addWidget(thumbnail);
    pictureRow->addWidget(m_includePicture, 1);
    layout->addLayout(pictureRow);

    qsizetype logBytes = 0;
    for (const Log &log : std::as_const(m_logs)) logBytes += log.text.toUtf8().size();
    auto *logRow = new QHBoxLayout;
    m_includeLogs = new QCheckBox(i18n("Include the app's logs (%1)", QLocale().formattedDataSize(logBytes)), this);
    m_includeLogs->setObjectName(QStringLiteral("problemReportIncludeLogs"));
    m_includeLogs->setToolTip(i18n("What Thoughts recorded recently: Sync runs and the developer log. Tokens and passwords are taken out."));
    m_includeLogs->setChecked(!m_logs.isEmpty());
    m_includeLogs->setEnabled(!m_logs.isEmpty());
    auto *show = new QPushButton(i18n("Show…"), this);
    show->setObjectName(QStringLiteral("problemReportShowLogs"));
    show->setEnabled(!m_logs.isEmpty());
    connect(show, &QPushButton::clicked, this, &ProblemReportDialog::showLogs);
    logRow->addWidget(m_includeLogs, 1);
    logRow->addWidget(show);
    layout->addLayout(logRow);

    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("problemReportStatus"));
    m_status->setWordWrap(true);
    m_status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_status);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_send = buttons->addButton(i18n("Send"), QDialogButtonBox::AcceptRole);
    m_send->setObjectName(QStringLiteral("problemReportSend"));
    m_send->setDefault(true);
    connect(m_send, &QPushButton::clicked, this, &ProblemReportDialog::send);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
    m_description->setFocus();
}

void ProblemReportDialog::showLogs()
{
    QDialog viewer(this);
    viewer.setWindowTitle(i18n("Logs in the Report"));
    auto *layout = new QVBoxLayout(&viewer);
    auto *tabs = new QTabWidget(&viewer);
    for (const Log &log : std::as_const(m_logs)) {
        auto *text = new QPlainTextEdit(tabs);
        text->setReadOnly(true);
        text->setLineWrapMode(QPlainTextEdit::NoWrap);
        text->setPlainText(log.text);
        tabs->addTab(text, log.name);
    }
    layout->addWidget(tabs);
    auto *close = new QDialogButtonBox(QDialogButtonBox::Close, &viewer);
    connect(close, &QDialogButtonBox::rejected, &viewer, &QDialog::reject);
    layout->addWidget(close);
    viewer.resize(820, 560);
    viewer.exec();
}

void ProblemReportDialog::setBusy(bool busy)
{
    m_send->setEnabled(!busy);
    m_description->setReadOnly(busy);
    m_kind->setEnabled(!busy);
    m_includePicture->setEnabled(!busy && !m_window.isNull());
    m_includeLogs->setEnabled(!busy && !m_logs.isEmpty());
}

void ProblemReportDialog::send()
{
    const QString description = m_description->toPlainText().trimmed();
    if (description.isEmpty()) {
        m_status->setText(i18n("Say in a few words what happened."));
        m_description->setFocus();
        return;
    }
    const QString configured = ServerAddress::configured();
    const QString canonical = ServerAddress::normalized(configured);
    const QByteArray token = canonical.isEmpty() ? QByteArray() : AppSignIn::readToken(m_hooks, Platform::desktopDeviceAccount(), canonical);
    const QUrl url = AppSignIn::requestUrl(m_hooks, configured, QStringLiteral("/v1/problem-reports"));
    if (url.isEmpty() || token.isEmpty()) {
        m_status->setText(failure(401, {}));
        return;
    }
    setBusy(true);
    m_status->setText(i18n("Sending…"));
    const QByteArray picture = m_includePicture->isChecked() ? encodedPicture(m_window) : QByteArray();
    const QJsonObject report = body(m_kind->currentData().toString(), description, picture, m_includeLogs->isChecked() ? m_logs : QList<Log>{});
    AppSignIn::send(m_network, "POST", url, token, &report, 120000, this, [this](int status, const QByteArray &answer, const QByteArray &) {
        if (status == 201) {
            // Not modal: the dialog closes now and the thanks stays until dismissed.
            auto *thanks = new QMessageBox(QMessageBox::Information, i18n("Report Sent"), i18n("Thank you. Your report was sent."), QMessageBox::Ok,
                                           parentWidget());
            thanks->setObjectName(QStringLiteral("problemReportThanks"));
            thanks->setAttribute(Qt::WA_DeleteOnClose);
            thanks->open();
            accept();
            return;
        }
        setBusy(false);
        m_status->setText(failure(status, answer));
    });
}
}
