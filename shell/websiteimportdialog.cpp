// SPDX-License-Identifier: GPL-2.0-or-later
#include "websiteimportdialog.h"

#include <KLocalizedString>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextDocumentFragment>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <poppler-qt6.h>
#include <memory>

namespace Thoughts {
PaperWebsite PaperWebsite::resolve(const QString &input)
{
    const QString raw = input.trimmed();
    const QUrl url(raw.contains(QLatin1String("://")) ? raw : QStringLiteral("https://") + raw, QUrl::StrictMode);
    const auto invalid = [](const QString &error) { return PaperWebsite{{}, {}, {}, error}; };
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) || !url.userInfo().isEmpty() || url.port() != -1
        || url.host().isEmpty())
        return invalid(i18n("Paste a web link, for example an arXiv, OpenReview or PDF link."));
    const QString host = url.host().toLower();
    if (host == QLatin1String("openreview.net") || host == QLatin1String("www.openreview.net")) {
        QString path = url.path();
        while (path.endsWith(QLatin1Char('/'))) path.chop(1);
        const auto ids = QUrlQuery(url).allQueryItemValues(QStringLiteral("id"), QUrl::FullyDecoded);
        if ((path != QLatin1String("/forum") && path != QLatin1String("/pdf")) || ids.size() != 1 ||
            !QRegularExpression(QStringLiteral("^[A-Za-z0-9_-]+$")).match(ids.value(0)).hasMatch())
            return invalid(i18n("Use an OpenReview paper or PDF link with a paper ID."));
        return {QUrl(QStringLiteral("https://openreview.net/pdf?id=") + ids.first()),
                QUrl(QStringLiteral("https://openreview.net/forum?id=") + ids.first()), QStringLiteral("OpenReview"), {}};
    }
    if (host == QLatin1String("arxiv.org") || host == QLatin1String("www.arxiv.org") || host == QLatin1String("export.arxiv.org")) {
        const auto match = QRegularExpression(QStringLiteral("^/(?:abs|pdf|html)/((?:[0-9]{4}\\.[0-9]{4,5}|[a-zA-Z-]+(?:\\.[A-Z]{2})?/[0-9]{7})(?:v[1-9][0-9]*)?)(?:\\.pdf)?/?$")).match(url.path());
        if (!match.hasMatch()) return invalid(i18n("Use an arXiv abstract, HTML, or PDF link."));
        return {QUrl(QStringLiteral("https://arxiv.org/pdf/") + match.captured(1)),
                QUrl(QStringLiteral("https://arxiv.org/abs/") + match.captured(1)), QStringLiteral("arXiv"), {}};
    }
    // Any other link is downloaded as-is; the download must turn out to be a PDF.
    QUrl direct = url;
    direct.setScheme(QStringLiteral("https"));
    return {direct, direct, url.host(), {}};
}

QString singleLineName(const QString &text)
{
    return text.simplified();
}

QString documentNameProblem(const QString &name)
{
    if (name.isEmpty()) return i18n("Enter a document name.");
    if (name.size() > 150) return i18n("The name has %1 characters; shorten it to 150 or fewer.", name.size());
    for (const QChar c : name)
        if (c.unicode() < 32) return i18n("The name contains an invisible control character; retype the name without it.");
    return {};
}

QString usablePaperTitle(const QString &raw)
{
    const QString title = singleLineName(raw);
    const QStringList generic{QStringLiteral("untitled"), QStringLiteral("untitled document"), QStringLiteral("document"),
                              QStringLiteral("paper"), QStringLiteral("main"), QStringLiteral("main.pdf")};
    if (generic.contains(title.toLower()) || QRegularExpression(QStringLiteral("^(?:arxiv[ :]+)?[0-9]{4}\\.[0-9]{4,5}(?:v[0-9]+)?(?:\\.pdf)?$"), QRegularExpression::CaseInsensitiveOption).match(title).hasMatch()) return {};
    for (const QChar c : title) if (c.unicode() < 32) return {};
    return title;
}

QString paperMetadataTitle(const QByteArray &body, bool json)
{
    if (json) {
        const auto notes = QJsonDocument::fromJson(body).object().value(QStringLiteral("notes")).toArray();
        if (notes.size() != 1) return {};
        const auto title = notes.first().toObject().value(QStringLiteral("content")).toObject().value(QStringLiteral("title"));
        return usablePaperTitle(title.isObject() ? title.toObject().value(QStringLiteral("value")).toString() : title.toString());
    }
    // Only citation metadata identifies a paper; generic titles can be sign-in pages.
    auto tags = QRegularExpression(QStringLiteral("<meta\\b(?:[^>\"']|\"[^\"]*\"|'[^']*')*>"), QRegularExpression::CaseInsensitiveOption).globalMatch(QString::fromUtf8(body));
    while (tags.hasNext()) {
        const QString tag = tags.next().captured();
        QString name, content;
        auto attrs = QRegularExpression(QStringLiteral("([A-Za-z_][A-Za-z0-9_-]*)\\s*=\\s*(?:\"([^\"]*)\"|'([^']*)'|([^\\s>]+))")).globalMatch(tag);
        while (attrs.hasNext()) {
            const auto attr = attrs.next();
            const QString value = !attr.captured(2).isNull() ? attr.captured(2) : !attr.captured(3).isNull() ? attr.captured(3) : attr.captured(4);
            if (attr.captured(1).compare(QLatin1String("name"), Qt::CaseInsensitive) == 0) name = value;
            if (attr.captured(1).compare(QLatin1String("content"), Qt::CaseInsensitive) == 0) content = value;
        }
        if (name.compare(QLatin1String("citation_title"), Qt::CaseInsensitive) == 0) {
            content.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
            content.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
            return usablePaperTitle(QTextDocumentFragment::fromHtml(content).toPlainText());
        }
    }
    return {};
}

WebsiteImportDialog::WebsiteImportDialog(const QString &destination, Import import, QWidget *parent, QNetworkAccessManager *network, const QString &link)
    : QDialog(parent), m_network(network ? network : new QNetworkAccessManager(this)), m_import(std::move(import)), m_destination(destination)
{
    setObjectName(QStringLiteral("websiteImportDialog"));
    resize(540, 260);
    auto *layout = new QVBoxLayout(this);
    m_link = new QLineEdit(this);
    m_link->setObjectName(QStringLiteral("websiteImportUrl"));
    m_link->setPlaceholderText(i18n("https://arxiv.org/abs/… or a link to a PDF"));
    layout->addWidget(m_link);
    m_name = new QLineEdit(this);
    m_name->setObjectName(QStringLiteral("websiteImportName"));
    m_name->setPlaceholderText(i18n("Document name"));
    m_name->setAccessibleName(i18n("Document name"));
    layout->addWidget(m_name);
    m_details = new QLabel(this);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setWordWrap(true);
    layout->addWidget(m_details);
    m_status = new QLabel(this);
    m_status->setObjectName(QStringLiteral("websiteImportStatus"));
    m_status->setTextFormat(Qt::PlainText);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    m_back = buttons->addButton(i18n("Open browser"), QDialogButtonBox::ActionRole);
    m_next = buttons->addButton(i18n("Next"), QDialogButtonBox::ActionRole);
    m_next->setObjectName(QStringLiteral("websiteImportNext"));
    m_next->setDefault(true);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_back, &QPushButton::clicked, this, [this] {
        if (m_prepared) { showUrl(); return; }
        const auto paper = PaperWebsite::resolve(m_link->text());
        if (!paper.error.isEmpty()) m_status->setText(paper.error);
        else if (!QDesktopServices::openUrl(paper.pageUrl)) m_status->setText(i18n("The browser could not be opened."));
    });
    connect(m_next, &QPushButton::clicked, this, [this] {
        if (!m_prepared) { prepare(); return; }
        // A title pasted over several lines is one name.
        const QString name = singleLineName(m_name->text());
        if (name != m_name->text()) m_name->setText(name);
        const QString problem = documentNameProblem(name);
        if (!problem.isEmpty()) { m_status->setText(problem); return; }
        const QString error = m_import(m_path, name, m_paper.pageUrl);
        if (!error.isEmpty()) { m_status->setText(error); return; }
        accept();
    });
    showUrl();
    if (!link.trimmed().isEmpty()) {
        // A link pasted in the import chooser starts downloading right away.
        m_link->setText(link.trimmed());
        QTimer::singleShot(0, this, [this] { prepare(); });
    }
}

void WebsiteImportDialog::showUrl(const QString &error)
{
    if (!m_path.isEmpty()) QFile::remove(m_path);
    m_path.clear(); m_title.clear(); m_prepared = false;
    setWindowTitle(i18n("Import from a link"));
    m_link->setVisible(true); m_link->setEnabled(true);
    m_name->hide();
    m_details->setText(i18n("Paste a link to a paper (arXiv, OpenReview) or to a PDF. Next downloads the PDF so you can confirm it.\nDestination: %1", m_destination));
    m_status->setText(error);
    m_next->setText(i18n("Next")); m_next->setEnabled(true);
    m_back->setText(i18n("Open browser")); m_back->setEnabled(true);
}

void WebsiteImportDialog::prepare()
{
    m_paper = PaperWebsite::resolve(m_link->text());
    if (!m_paper.error.isEmpty()) { m_status->setText(m_paper.error); return; }
    if (!m_temporary.isValid()) { m_status->setText(i18n("A temporary download folder could not be created.")); return; }
    m_link->setEnabled(false); m_next->setEnabled(false); m_back->setEnabled(false);
    m_status->setText(i18n("Fetching title and downloading PDF…"));
    fetchTitle();
}

void WebsiteImportDialog::fetchTitle(int index)
{
    // Direct PDF links have no paper page to read a title from; the PDF's own title is used.
    if (m_paper.website != QLatin1String("OpenReview") && m_paper.website != QLatin1String("arXiv")) { download(); return; }
    QList<QUrl> urls;
    if (m_paper.website == QLatin1String("OpenReview")) {
        urls << QUrl(QStringLiteral("https://api2.openreview.net/notes?") + m_paper.pdfUrl.query())
             << QUrl(QStringLiteral("https://api.openreview.net/notes?") + m_paper.pdfUrl.query());
    }
    urls << m_paper.pageUrl;
    if (index >= urls.size()) { download(); return; }
    const bool json = index < urls.size() - 1;
    fetch(urls.at(index), 2 * 1024 * 1024, [this, index, json](const QByteArray &bytes, const QString &) {
        m_title = paperMetadataTitle(bytes, json);
        if (m_title.isEmpty()) fetchTitle(index + 1); else download();
    });
}

void WebsiteImportDialog::download()
{
    fetch(m_paper.pdfUrl, 100 * 1024 * 1024, [this](const QByteArray &bytes, const QString &error) {
        if (!error.isEmpty()) { showUrl(error); return; }
        if (!bytes.startsWith("%PDF-")) { showUrl(i18n("This link opens a web page, not a PDF. Use a direct PDF link, or save the page and choose it as a file.")); return; }
        auto pdf = Poppler::Document::loadFromData(bytes);
        if (!pdf || pdf->isLocked() || pdf->numPages() <= 0) { showUrl(i18n("The downloaded PDF could not be read or is password protected.")); return; }
        m_path = m_temporary.filePath(QStringLiteral("paper.pdf"));
        QFile file(m_path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) { showUrl(i18n("The downloaded PDF could not be saved.")); return; }
        file.close();
        if (m_title.isEmpty()) m_title = usablePaperTitle(pdf->info(QStringLiteral("Title")));
        m_prepared = true;
        setWindowTitle(i18n("Confirm import"));
        m_link->hide(); m_name->show(); m_name->setText(singleLineName(m_title)); m_name->setFocus();
        m_details->setText(i18n("Website: %1\nFile size: %2\nPages: %3\nDestination: %4", m_paper.website, QLocale().formattedDataSize(bytes.size()), pdf->numPages(), m_destination));
        m_status->setText(m_title.isEmpty() ? i18n("The title could not be found. Enter a document name to continue.") : i18n("You can edit the name before importing."));
        m_next->setText(i18n("Import")); m_next->setEnabled(true);
        m_back->setText(i18n("Back")); m_back->setEnabled(true);
    });
}

void WebsiteImportDialog::fetch(const QUrl &url, qint64 limit, std::function<void(QByteArray, QString)> done, int redirects)
{
    if (url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty() || !url.isValid() || redirects > 5) {
        done({}, i18n("The website redirected to an unsupported address or too many times.")); return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    request.setRawHeader("User-Agent", "Thoughts/3.0 (Desktop; user-requested paper import)");
    auto *reply = m_network->get(request);
    reply->setReadBufferSize(65536);
    auto bytes = std::make_shared<QByteArray>();
    auto tooLarge = std::make_shared<bool>(false);
    auto consume = [reply, bytes, tooLarge, limit] {
        if (*tooLarge) return;
        const QByteArray chunk = reply->readAll();
        if (bytes->size() + chunk.size() > limit || reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > limit) {
            *tooLarge = true; reply->abort();
        } else bytes->append(chunk);
    };
    connect(reply, &QNetworkReply::readyRead, this, consume);
    // Also bound slow responses that keep sending tiny chunks before the inactivity timeout.
    QTimer::singleShot(120000, reply, [reply] { if (reply->isRunning()) reply->abort(); });
    connect(this, &QDialog::finished, reply, [reply] { reply->blockSignals(true); reply->abort(); reply->deleteLater(); });
    connect(reply, &QNetworkReply::finished, this, [this, reply, bytes, tooLarge, limit, done, redirects, url, consume] {
        consume();
        reply->deleteLater();
        if (*tooLarge) { done({}, i18n("The download exceeds the size limit (100 MB for PDFs).")); return; }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            const auto redirect = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
            if (redirect.isEmpty()) { done({}, i18n("The website returned an empty redirect.")); return; }
            fetch(url.resolved(redirect), limit, done, redirects + 1); return;
        }
        if (status == 401 || status == 403) { done({}, i18n("The website requires sign-in or a browser check. Open it in your browser, download the PDF, then use Import document….")); return; }
        if (status == 404) { done({}, i18n("The paper was not found; check the link.")); return; }
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            done({}, i18n("The download failed: %1. Check your connection and try again.", reply->errorString())); return;
        }
        done(*bytes, {});
    });
}
}
