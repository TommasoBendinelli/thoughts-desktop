// SPDX-License-Identifier: GPL-2.0-or-later
// Report a Problem… (thoughts-server protocol/problem-reports.md): the person describes a problem or
// an idea; the report carries a picture of the Thoughts window and the app's own logs, both shown
// first and each left out with a click, and goes to the person who runs the server.
#pragma once

#include "appsignin.h"

#include <QDialog>
#include <QImage>
#include <QJsonObject>
#include <QList>

#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QNetworkAccessManager;
class QPlainTextEdit;
class QPushButton;

namespace Thoughts
{
namespace ProblemReports
{
struct Log {
    QString name;
    QString text;
};
// The longer side of a sent picture, in pixels.
constexpr int MaxPictureSide = 2560;
// What the logs of one report may hold together, in bytes (the server takes 8 MB).
constexpr qsizetype MaxLogBytes = 6 * 1024 * 1024;
// The window as it is on screen, before the dialog covers it.
QImage grabWindow(QWidget *window);
// PNG, or JPEG when the PNG is large; scaled to MaxPictureSide.
QByteArray encodedPicture(const QImage &image);
// The last `bytes` of `path`, from the start of a line; empty when it cannot be read.
QString fileTail(const QString &path, qsizetype bytes);
// Logs cut so that together they stay under MaxLogBytes (each keeps its newest part).
QList<Log> fitted(QList<Log> logs);
// The POST /v1/problem-reports body.
QJsonObject body(const QString &kind, const QString &description, const QByteArray &picture, const QList<Log> &logs);
// A refusal or failure in words for the dialog.
QString failure(int status, const QByteArray &answer);
}

class ProblemReportDialog : public QDialog
{
    Q_OBJECT
public:
    ProblemReportDialog(const QImage &window, std::function<QList<ProblemReports::Log>()> collectLogs, AppSignIn::Hooks hooks,
                        QWidget *parent = nullptr);

private:
    void send();
    void showLogs();
    void setBusy(bool busy);

    QImage m_window;
    QList<ProblemReports::Log> m_logs;
    AppSignIn::Hooks m_hooks;
    QNetworkAccessManager *m_network = nullptr;
    QComboBox *m_kind = nullptr;
    QPlainTextEdit *m_description = nullptr;
    QCheckBox *m_includePicture = nullptr;
    QCheckBox *m_includeLogs = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_send = nullptr;
};
}
