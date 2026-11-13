// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <QDialog>
#include <QTemporaryDir>
#include <QUrl>
#include <functional>

class QLabel;
class QLineEdit;
class QPushButton;
class QNetworkAccessManager;

namespace Thoughts {
struct PaperWebsite {
    QUrl pdfUrl;
    QUrl pageUrl;
    QString website;
    QString error;
    static PaperWebsite resolve(const QString &input);
};
QString usablePaperTitle(const QString &raw);
QString paperMetadataTitle(const QByteArray &body, bool json);

// Preparation only touches temporary files; the caller saves after confirmation.
class WebsiteImportDialog : public QDialog {
public:
    using Import = std::function<QString(const QString &path, const QString &name, const QUrl &link)>;
    WebsiteImportDialog(const QString &destination, Import import, QWidget *parent = nullptr, QNetworkAccessManager *network = nullptr,
                        const QString &link = {});
private:
    void showUrl(const QString &error = {});
    void prepare();
    void fetchTitle(int index = 0);
    void download();
    void fetch(const QUrl &url, qint64 limit, std::function<void(QByteArray, QString)> done, int redirects = 0);
    QNetworkAccessManager *m_network;
    QTemporaryDir m_temporary;
    Import m_import;
    QString m_destination;
    QString m_path;
    QString m_title;
    PaperWebsite m_paper;
    QLineEdit *m_link;
    QLineEdit *m_name;
    QLabel *m_details;
    QLabel *m_status;
    QPushButton *m_next;
    QPushButton *m_back;
    bool m_prepared = false;
};
}
