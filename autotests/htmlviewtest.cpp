// SPDX-License-Identifier: GPL-2.0-or-later
// The reader's HTML view (thoughts-html-view-SPEC.md, "Desktop") on a Part with a temporary
// Library (testhome.cpp): availability, the remembered PDF | HTML switch, and, where a real web
// view exists (THOUGHTS_HTML_GUI_TEST=1 on macOS, not the offscreen platform), the edition in
// the web view: highlights, review mask, reveal, selection menu, answer feedback, new cards and
// comments from a selection. THOUGHTS_HTML_SHOTS=<dir> saves screenshots of each step.
#include "../part/htmldocumentview.h"
#include "../part/htmllivewebview.h"
#include "../part/htmledition.h"
#include "../part/fedorarepository.h"
#include "../part/htmlreader.h"
#include "../part/htmlattachmentreader.h"
#include "../core/htmlattachment.h"
#include "../core/htmllivepage.h"
#include "../part/annotationmodel.h"
#include "../part/documentfeedback.h"
#include "../part/documentfeedbackoverlay.h"
#include "../part/pageview.h"
#include "../part/readaloud.h"
#include "../part/part.h"
#include "../part/reviewcardfeedback.h"
#include "../part/reviewfeedbackeditor.h"
#include "../part/reviewfeedbackoverlay.h"
#include "../part/side_reviews.h"
#include "../part/sidebar.h"
#include "../part/snapshotdrawingdialog.h"
#include "../part/thoughtsplatform.h"
#include "../settings.h"

#include <KActionCollection>
#include <KToggleAction>

#include <QApplication>
#include <QClipboard>
#include <QStackedWidget>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScopeGuard>
#include <QScreen>
#include <QShortcut>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <cstdio>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QUuid>
#include <QWindow>

namespace
{
const QString CardId = QStringLiteral("4150ddf3-7842-4511-b735-05f9ec89972f");
const QString AttachmentPath = QStringLiteral("docs-ideas/tex/support/html/explainer.html");
const QString DocumentId = QStringLiteral("c1870925-b9d7-45f5-b112-ba599f2b5af4");
const QUrl LiveAddress(QStringLiteral("http://localhost:18770/"));
// A live page's stub (thoughts-server protocol/html-live-pages.md §1), as the attachment's bytes.
const QByteArray LiveStub = Thoughts::LivePage::stubHtml(LiveAddress, QStringLiteral("Training curves"));

QString sha(const QByteArray &bytes)
{
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QByteArray readFile(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool writeJson(const QString &path, const QJsonObject &object)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(object).toJson()) > 0;
}

QString sampleEditionDir()
{
    const QString chosen = qEnvironmentVariable("THOUGHTS_HTML_SAMPLE_EDITION");
    return chosen.isEmpty() ? QStringLiteral(KDESRCDIR "data/html-edition/synthetic-latex") : chosen;
}
}

namespace Okular
{
class HtmlViewTest : public QObject
{
    Q_OBJECT
private Q_SLOTS:
    void initTestCase();
    void availabilityAndRememberedSwitch();
    void importedAndPendingDocuments();
    void linksOpenTheLinkedDocument();
    void forwardedKeys();
    void attachmentLinksOpenOnlyOnTheReadersClick();
    void webViewReviewSelectionAndCards();
    void attachmentShownInPlaceOfTheDocument();
    void attachmentTabShowsThePageAlone();
    void attachmentFollowsItsNewestVersion();
    void attachmentForwardsReviewKeys();
    void attachmentCardFromASelection();
    void attachmentFeedbackFromASelection();
    void snapshotDrawingDialogTakesStrokes();
    void attachmentDrawingOnASnapshot();
    void attachmentCardReviewWithoutPageShowsStoredAnswer();
    void webViewAttachmentCardsAndReview();
    void webViewAttachmentFeedback();
    void livePageAsksThenLoadsWithoutTheBridge();
    void liveViewFollowsAServersRedirectElsewhere();
    void liveSnapshotAndGoLiveAgain();
    void livePageTextFeedback();
    void webTabsOfADocumentAndOfNone();

private:
    // A Library PDF (copy of data/file1.pdf) whose generation lists `state` for its edition.
    QString addLibraryPdf(const QString &stem, const QString &kind, const QJsonObject &htmlFields);
    void shot(Part &part, const QString &name, QWidget *popup = nullptr, Thoughts::HtmlDocumentView *view = nullptr);
    // The attachment fixture (or @p html) in the PDF's source bundle (as Sync installs it) -> its target.
    Thoughts::HtmlAttachmentTarget addAttachment(const QString &pdfPath, const QByteArray &page = {});
    QJsonObject attachmentCard(const QJsonObject &fields, const QString &question) const;
    QJsonValue callAttachment(Part &part, const QString &name, const QJsonArray &args = {});
    QJsonValue callViewer(Part &part, const QString &name, const QJsonArray &args = {});
    QJsonObject sampleCard(int index = 0) const;

    QString m_docs;
    QString m_editionSha;
    QJsonObject m_manifest;
    QJsonArray m_index;
    QJsonArray m_publications;
};

void HtmlViewTest::initTestCase()
{
    m_docs = qEnvironmentVariable("THOUGHTS_HOME");
    QVERIFY(!m_docs.isEmpty());
    QDir().mkpath(m_docs + QStringLiteral("/current_pdfs"));
    // The sample edition, installed into the HTML edition cache as the server's objects would be.
    const QString dir = sampleEditionDir();
    const QByteArray manifestBytes = readFile(dir + QStringLiteral("/manifest.json"));
    QVERIFY(!manifestBytes.isEmpty());
    m_manifest = QJsonDocument::fromJson(manifestBytes).object();
    m_editionSha = sha(manifestBytes);
    QHash<QString, QByteArray> objects;
    const QJsonObject files = m_manifest.value(QStringLiteral("files")).toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {
        const QByteArray bytes = readFile(dir + QLatin1Char('/') + it.key());
        QCOMPARE(sha(bytes), it.value().toString());
        objects.insert(it.value().toString(), bytes);
    }
    Thoughts::HtmlEdition::Store store(QDir(Thoughts::Platform::applicationDataRoot()).filePath(QStringLiteral("thoughts-server/html-editions")));
    QString error;
    QVERIFY2(store.installFromObjects(m_editionSha, manifestBytes, objects, &error), qPrintable(error));
}

QString HtmlViewTest::addLibraryPdf(const QString &stem, const QString &kind, const QJsonObject &htmlFields)
{
    const QString path = m_docs + QStringLiteral("/current_pdfs/") + stem + QStringLiteral(".pdf");
    QFile::remove(path);
    if (!QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), path)) return {};
    // One PDF version per document: edition states are kept per PDF SHA-256, and a listed state
    // never replaces a final one recorded for the same version (Store::adoptListed).
    QFile copy(path);
    if (!copy.open(QIODevice::Append)) return {};
    copy.write("%" + stem.toUtf8() + "\n");
    copy.close();
    const QString pdfSha = sha(readFile(path));
    QJsonObject entry{{QStringLiteral("filename"), QString(stem + QStringLiteral(".pdf"))}, {QStringLiteral("pdf_sha256"), pdfSha},
                      {QStringLiteral("document_id"), QStringLiteral("c1870925-b9d7-45f5-b112-ba599f2b5af4")}, {QStringLiteral("kind"), kind}};
    m_index.append(entry);
    QJsonObject publication = htmlFields;
    publication.insert(QStringLiteral("filename"), QString(stem + QStringLiteral(".pdf")));
    publication.insert(QStringLiteral("sha256"), pdfSha);
    m_publications.append(publication);
    // A verified generation (same revision in index and manifest), so comments are writable.
    const QJsonObject frontier{{QStringLiteral("library_revision"), QStringLiteral("test-revision-1")}, {QStringLiteral("source_index_sha256"), QString(64, QLatin1Char('5'))}};
    QJsonObject index{{QStringLiteral("pdfs"), m_index}};
    QJsonObject generation{{QStringLiteral("publications"), m_publications}};
    for (auto it = frontier.begin(); it != frontier.end(); ++it) {
        index.insert(it.key(), it.value());
        generation.insert(it.key(), it.value());
    }
    writeJson(m_docs + QStringLiteral("/current_pdfs/.thoughts-library-index.json"), index);
    writeJson(m_docs + QStringLiteral("/current_pdfs/.thoughts-generation.json"), generation);
    return path;
}

QJsonObject HtmlViewTest::sampleCard(int index) const
{
    const QStringList ids = m_manifest.value(QStringLiteral("items")).toObject().value(QStringLiteral("cards")).toObject().keys();
    const QString id = ids.value(index, CardId);
    return {{QStringLiteral("schema_version"), 3},
            {QStringLiteral("card_id"), id},
            {QStringLiteral("status"), QStringLiteral("approved")},
            {QStringLiteral("primary"), QJsonObject{{QStringLiteral("question"), QStringLiteral("What does the marked passage say?")},
                                                    {QStringLiteral("pdf_stem"), QStringLiteral("sample")},
                                                    {QStringLiteral("document_id"), QStringLiteral("c1870925-b9d7-45f5-b112-ba599f2b5af4")},
                                                    {QStringLiteral("page"), 1},
                                                    {QStringLiteral("rect"), QJsonArray{0.1, 0.1, 0.9, 0.2}}}}};
}

void HtmlViewTest::shot(Part &part, const QString &name, QWidget *popup, Thoughts::HtmlDocumentView *view)
{
    if (!view) view = part.m_htmlView;
    const QString dir = qEnvironmentVariable("THOUGHTS_HTML_SHOTS");
    if (dir.isEmpty()) return;
    QDir().mkpath(dir);
    QTest::qWait(400);
    // Qt draws its widgets; the web view is a native view, drawn by WKWebView's snapshot.
    QWidget *window = part.widget()->window();
    QImage image = window->grab().toImage();
    QWidget *container = view->webContainer();
    if (container && container->isVisible()) {
        QImage page;
        bool done = false;
        view->snapshotPage([&](const QImage &snapshot) {
            page = snapshot;
            done = true;
        });
        QTest::qWaitFor([&] { return done; }, 5000);
        if (!page.isNull()) {
            QPainter painter(&image);
            painter.drawImage(QRect(container->mapTo(window, QPoint(0, 0)), container->size()), page);
        }
    }
    if (popup) {
        const QImage menu = popup->grab().toImage();
        const QPoint at = popup->mapToGlobal(QPoint(0, 0)) - window->mapToGlobal(QPoint(0, 0));
        const QRect bounds = QRect(QPoint(0, 0), window->size()).united(QRect(at, popup->size()));
        QImage canvas(bounds.size() * image.devicePixelRatio(), QImage::Format_ARGB32_Premultiplied);
        canvas.setDevicePixelRatio(image.devicePixelRatio());
        canvas.fill(Qt::transparent);
        QPainter painter(&canvas);
        painter.drawImage(-bounds.topLeft(), image);
        painter.drawImage(at - bounds.topLeft(), menu);
        painter.end();
        image = canvas;
    }
    QVERIFY(image.save(dir + QLatin1Char('/') + name));
}

QJsonValue HtmlViewTest::callViewer(Part &part, const QString &name, const QJsonArray &args)
{
    bool done = false;
    QJsonValue value;
    part.m_htmlView->call(name, args, [&](bool ok, const QJsonValue &result, const QString &error) {
        if (!ok) qWarning() << name << error;
        value = result;
        done = true;
    });
    if (!QTest::qWaitFor([&] { return done; }, 10000)) qWarning() << "viewer call timed out:" << name;
    return value;
}

Thoughts::HtmlAttachmentTarget HtmlViewTest::addAttachment(const QString &pdfPath, const QByteArray &page)
{
    // <docs>/.runtime/target/source-bundle: the Library's bundle next to current_pdfs/.
    const QDir root(QFileInfo(pdfPath).absolutePath() + QStringLiteral("/../.runtime/target/source-bundle"));
    QDir().mkpath(root.filePath(QStringLiteral("objects")));
    QDir().mkpath(root.filePath(QStringLiteral("documents")));
    const QByteArray html = page.isEmpty() ? readFile(QStringLiteral(KDESRCDIR "data/html-attachment/explainer.html")) : page;
    const QByteArray pdf = readFile(pdfPath);
    auto write = [](const QString &path, const QByteArray &bytes) {
        QFile file(path);
        return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    };
    const QString filename = QFileInfo(pdfPath).fileName();
    const QJsonObject document{{QStringLiteral("filename"), filename}, {QStringLiteral("pdf_sha256"), sha(pdf)}};
    const QJsonObject resource{{QStringLiteral("kind"), QStringLiteral("html_attachment")}, {QStringLiteral("path"), AttachmentPath},
                               {QStringLiteral("sha256"), sha(html)}, {QStringLiteral("object"), sha(html)}, {QStringLiteral("bytes"), html.size()}};
    const QByteArray manifest = QJsonDocument(QJsonObject{{QStringLiteral("pdf"), document}, {QStringLiteral("resources"), QJsonArray{resource}}}).toJson();
    QJsonObject pointer = document;
    pointer.insert(QStringLiteral("artifact_sha256"), sha(manifest));
    if (html.isEmpty() || !write(root.filePath(QStringLiteral("objects/") + sha(html)), html) || !write(root.filePath(QStringLiteral("objects/") + sha(manifest)), manifest)
        || !write(root.filePath(QStringLiteral("documents/") + filename + QStringLiteral(".json")), QJsonDocument(pointer).toJson()))
        return {};
    QString error;
    const auto target = Thoughts::resolveHtmlAttachment(AttachmentPath, pdfPath, &error);
    if (!target.isValid()) qWarning() << "attachment fixture:" << error;
    return target;
}

QJsonObject HtmlViewTest::attachmentCard(const QJsonObject &fields, const QString &question) const
{
    // As ReviewStore::createHtmlAttachmentCard saves it (the shell's part, not linked here).
    QJsonObject primary = fields;
    for (const QString &key : {QStringLiteral("pdf_path")}) primary.remove(key);
    primary.insert(QStringLiteral("question"), question);
    if (!primary.contains(QStringLiteral("selected_text")))
        primary.insert(QStringLiteral("selected_text"), fields.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")));
    return {{QStringLiteral("schema_version"), 3},
            {QStringLiteral("card_id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
            {QStringLiteral("status"), QStringLiteral("approved")},
            {QStringLiteral("source_authority"), QStringLiteral("client-v1")},
            {QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("html_attachment")}}},
            {QStringLiteral("primary"), primary}};
}

QJsonValue HtmlViewTest::callAttachment(Part &part, const QString &name, const QJsonArray &args)
{
    bool done = false;
    QJsonValue value;
    part.m_attachmentView->call(name, args, [&](bool ok, const QJsonValue &result, const QString &error) {
        if (!ok) qWarning() << name << error;
        value = result;
        done = true;
    });
    if (!QTest::qWaitFor([&] { return done; }, 10000)) qWarning() << "attachment viewer call timed out:" << name;
    return value;
}

void HtmlViewTest::attachmentShownInPlaceOfTheDocument()
{
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    QCOMPARE(target.path, AttachmentPath);
    Part part(nullptr, {});
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    QCOMPARE(part.m_documentStack->currentWidget(), part.m_pageView.data());
    QString error;
    QVERIFY2(part.showHtmlAttachment(target, false, &error), qPrintable(error));
    QCOMPARE(part.m_documentStack->currentWidget(), part.m_attachmentView);
    QVariantMap state = part.htmlAttachmentState();
    QVERIFY(state.value(QStringLiteral("shown")).toBool());
    QCOMPARE(state.value(QStringLiteral("path")).toString(), AttachmentPath);
    QCOMPARE(state.value(QStringLiteral("sha256")).toString(), target.sha256);
    // Served on its own loopback origin (no file:// access, the attachment CSP).
    const QUrl url(state.value(QStringLiteral("url")).toString());
    QCOMPARE(url.host(), QStringLiteral("127.0.0.1"));
    QCOMPARE(part.widget()->property("thoughtsNativeDocumentView").toBool(), Thoughts::HtmlDocumentView::webViewAvailable());
    // The bar: Back and Open in Browser, no text size.
    QVERIFY(part.m_attachmentView->findChild<QToolButton *>(QStringLiteral("thoughtsHtmlOpenInBrowser"))->isVisibleTo(part.m_attachmentView));
    QVERIFY(!part.m_attachmentView->findChild<QToolButton *>(QStringLiteral("thoughtsHtmlTextLarger"))->isVisibleTo(part.m_attachmentView));
    QCOMPARE(part.m_attachmentView->findChild<QToolButton *>(QStringLiteral("thoughtsHtmlShowPdf"))->text(), QStringLiteral("Back"));
    // The same object again keeps the page (its scripts' state).
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("url")).toString(), url.toString());
    // Choosing a view of the document leaves the attachment; Back does too.
    part.setDocumentView(QStringLiteral("pdf"));
    QCOMPARE(part.m_documentStack->currentWidget(), part.m_pageView.data());
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    Q_EMIT part.m_attachmentView->showPdfRequested();
    QCOMPARE(part.m_documentStack->currentWidget(), part.m_pageView.data());
    QVERIFY(!part.widget()->property("thoughtsNativeDocumentView").toBool());
    // Cards made in attachments are not the HTML edition's ("Not shown in HTML view").
    QJsonObject fields = Thoughts::HtmlAttachmentReader::newCardFields(
        {{QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("one cent per page")}}}}, target);
    fields.insert(QStringLiteral("pdf_stem"), QStringLiteral("ideas"));
    const QJsonObject card = attachmentCard(fields, QStringLiteral("How much?"));
    part.setSidebarReviewCards(QJsonArray{card, sampleCard(0)});
    QCOMPARE(part.m_htmlReader->cards().size(), 1);
    QCOMPARE(part.m_attachmentReader->pageCards().size(), 1);
    // Closing the document closes its attachment.
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    part.closeUrl();
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("path")).toString().isEmpty());
}

void HtmlViewTest::attachmentTabShowsThePageAlone()
{
    // In the app (a receiver for requestHtmlAttachmentTab) a page opens in a tab of its own: the
    // document's Part asks for it and keeps its PDF; the tab's Part shows the page alone.
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    Part document(nullptr, {});
    document.openDocument(path);
    QVERIFY(document.m_document->isOpened());
    QList<QJsonObject> requests;
    QObject::connect(&document, &Part::requestHtmlAttachmentTab, &document, [&requests](const QJsonObject &request) { requests.append(request); });
    // A card's page and its feedback's go to the page's tab too; the PDF stays.
    QJsonObject fields = Thoughts::HtmlAttachmentReader::newCardFields(
        {{QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("one cent per page")}}}}, target);
    fields.insert(QStringLiteral("pdf_stem"), QStringLiteral("ideas"));
    const QJsonObject card = attachmentCard(fields, QStringLiteral("How much?"));
    QVERIFY(document.showHtmlAttachmentCard(card));
    QCOMPARE(requests.size(), 1);
    QCOMPARE(requests.constLast().value(QStringLiteral("path")).toString(), AttachmentPath);
    QCOMPARE(requests.constLast().value(QStringLiteral("card")).toObject(), card);
    QVERIFY(!document.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QCOMPARE(document.m_documentStack->currentWidget(), document.m_pageView.data());
    // Not during a review of this document: its page shows in the document's tab, as before.
    QString error;
    QVERIFY2(document.showHtmlAttachment(target, true, &error), qPrintable(error));
    document.m_attachmentReviewShown = true;
    document.refreshDocumentFeedback();
    auto *overlay = document.m_pageView->documentFeedback();
    QVERIFY(overlay->hasDocument());
    QJsonObject record = Thoughts::DocumentFeedback::create(Thoughts::ReviewCardFeedback::positive(), QString(),
                                                            Thoughts::DocumentFeedback::attachmentAnchor(fields, overlay->documentId(), overlay->pdfSha256()),
                                                            Thoughts::Platform::reviewOriginId());
    record.insert(QStringLiteral("view"), Thoughts::DocumentFeedback::attachmentView());
    overlay->setRecords({record});
    QVERIFY(!overlay->record(record.value(QStringLiteral("feedback_id")).toString()).isEmpty());
    document.showFeedback(record.value(QStringLiteral("feedback_id")).toString(), false);
    QCOMPARE(requests.size(), 1);
    document.m_attachmentReviewShown = false;
    document.hideHtmlAttachment();
    document.showFeedback(record.value(QStringLiteral("feedback_id")).toString(), true);
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.constLast().value(QStringLiteral("feedback_id")).toString(), record.value(QStringLiteral("feedback_id")).toString());
    QVERIFY(requests.constLast().value(QStringLiteral("edit")).toBool());
    QVERIFY(!document.htmlAttachmentState().value(QStringLiteral("shown")).toBool());

    // The page's tab: the page alone, with Close; its HTML edition is never loaded.
    Part page(nullptr, {});
    QSignalSpy closes(&page, &Part::requestCloseHtmlAttachmentTab);
    QVERIFY(page.openHtmlAttachmentTab(QUrl::fromLocalFile(path), {{QStringLiteral("path"), AttachmentPath}}));
    QVERIFY(page.isHtmlAttachmentTab());
    QCOMPARE(page.m_documentStack->currentWidget(), page.m_attachmentView);
    QVariantMap state = page.htmlAttachmentState();
    QVERIFY(state.value(QStringLiteral("shown")).toBool());
    QVERIFY(state.value(QStringLiteral("tab")).toBool());
    QCOMPARE(state.value(QStringLiteral("path")).toString(), AttachmentPath);
    QCOMPARE(page.htmlAttachmentTitle(), QStringLiteral("Agent paywall (test attachment)"));
    auto *close = page.m_attachmentView->findChild<QToolButton *>(QStringLiteral("thoughtsHtmlShowPdf"));
    QCOMPARE(close->text(), QStringLiteral("Close"));
    QVERIFY(page.m_htmlReader->documentPath().isEmpty());
    QVERIFY(!page.m_htmlViewAction->isEnabled());
    // Nothing shows the PDF in its place: not Back, not a choice of view.
    page.hideHtmlAttachment();
    page.setDocumentView(QStringLiteral("html"));
    QCOMPARE(page.m_documentStack->currentWidget(), page.m_attachmentView);
    close->click();
    QCOMPARE(closes.size(), 1);
    QCOMPARE(page.m_documentStack->currentWidget(), page.m_attachmentView);
    // Its own page again (a card, its feedback) stays in this tab; another page gets its own.
    const QString otherPage = QStringLiteral("docs-ideas/tex/support/html/other.html");
    QVERIFY(!page.wantsHtmlAttachmentTab(otherPage)); // no shell takes it here
    QSignalSpy pageRequests(&page, &Part::requestHtmlAttachmentTab);
    QVERIFY(!page.wantsHtmlAttachmentTab(AttachmentPath));
    QVERIFY(page.wantsHtmlAttachmentTab(otherPage));
    QVERIFY(page.showInHtmlAttachmentTab({{QStringLiteral("path"), AttachmentPath}, {QStringLiteral("card"), card}}));
    QCOMPARE(pageRequests.size(), 0);
    QVERIFY(page.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    // The window's last tab: an ordinary Part again, Back and the edition.
    page.leaveHtmlAttachmentTab();
    QVERIFY(!page.isHtmlAttachmentTab());
    QCOMPARE(close->text(), QStringLiteral("Back"));
    QCOMPARE(page.m_documentStack->currentWidget(), page.m_pageView.data());
    QCOMPARE(page.m_htmlReader->documentPath(), page.localFilePath());
}

void HtmlViewTest::attachmentForwardsReviewKeys()
{
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    Part part(nullptr, {});
    QWidget *widget = part.widget();
    widget->resize(800, 600);
    widget->show();
    part.openDocument(path);
    // S and Escape only: Read Aloud's bar belongs to the HTML edition.
    QCOMPARE(part.m_attachmentReader->loadOptions().value(QStringLiteral("forwardKeys")).toArray(),
             (QJsonArray{QStringLiteral("s"), QStringLiteral("Escape")}));
    QCOMPARE(part.m_attachmentReader->loadOptions().value(QStringLiteral("mode")).toString(), QStringLiteral("attachment"));
    const QJsonObject s{{QStringLiteral("type"), QStringLiteral("key")}, {QStringLiteral("key"), QStringLiteral("s")}, {QStringLiteral("code"), QStringLiteral("KeyS")},
                        {QStringLiteral("meta"), false}, {QStringLiteral("ctrl"), false}, {QStringLiteral("alt"), false}, {QStringLiteral("shift"), false},
                        {QStringLiteral("forwarded"), true}, {QStringLiteral("repeat"), false}};
    auto *panel = new QWidget(widget);
    auto *skip = new QShortcut(QKeySequence(Qt::Key_S), panel);
    skip->setContext(Qt::WindowShortcut);
    QSignalSpy skipped(skip, &QShortcut::activated);
    panel->show();
    // Not shown: a late message of a page that is gone does nothing.
    Q_EMIT part.m_attachmentView->viewerMessage(s);
    QCOMPARE(skipped.size(), 0);
    QString error;
    QVERIFY2(part.showHtmlAttachment(target, false, &error), qPrintable(error));
    // The review panel's Skip, from the page.
    Q_EMIT part.m_attachmentView->viewerMessage(s);
    QCOMPARE(skipped.size(), 1);
    // Not forwarded by the viewer (the page used it): the app does nothing.
    QJsonObject own = s;
    own.insert(QStringLiteral("forwarded"), false);
    Q_EMIT part.m_attachmentView->viewerMessage(own);
    QJsonObject repeat = s;
    repeat.insert(QStringLiteral("repeat"), true);
    Q_EMIT part.m_attachmentView->viewerMessage(repeat);
    QCOMPARE(skipped.size(), 1);

    // The page's own script can post any key message, marked as forwarded too. Only the keys
    // the app asked for reach the window's shortcuts, whatever else the window has.
    auto *other = new QShortcut(QKeySequence(Qt::Key_D), panel);
    other->setContext(Qt::WindowShortcut);
    auto *quit = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Q), panel);
    quit->setContext(Qt::WindowShortcut);
    auto *function = new QShortcut(QKeySequence(Qt::Key_F5), panel);
    function->setContext(Qt::WindowShortcut);
    auto *shifted = new QShortcut(QKeySequence(Qt::SHIFT | Qt::Key_S), panel);
    shifted->setContext(Qt::WindowShortcut);
    QSignalSpy fired(other, &QShortcut::activated);
    QSignalSpy quitFired(quit, &QShortcut::activated);
    QSignalSpy functionFired(function, &QShortcut::activated);
    QSignalSpy shiftedFired(shifted, &QShortcut::activated);
    const auto forged = [&](const QString &key, const QString &code, const QStringList &modifiers = {}) {
        QJsonObject message = s;
        message.insert(QStringLiteral("key"), key);
        message.insert(QStringLiteral("code"), code);
        for (const QString &modifier : modifiers) message.insert(modifier, true);
        Q_EMIT part.m_attachmentView->viewerMessage(message);
    };
    forged(QStringLiteral("d"), QStringLiteral("KeyD"));
    forged(QStringLiteral("q"), QStringLiteral("KeyQ"), {QStringLiteral("meta")});
    forged(QStringLiteral("q"), QStringLiteral("KeyQ"), {QStringLiteral("ctrl")});
    forged(QStringLiteral("F5"), QStringLiteral("F5"));
    forged(QStringLiteral("S"), QStringLiteral("KeyS"), {QStringLiteral("shift")});
    QCOMPARE(fired.size(), 0);
    QCOMPARE(quitFired.size(), 0);
    QCOMPARE(functionFired.size(), 0);
    QCOMPARE(shiftedFired.size(), 0);
    QCOMPARE(skipped.size(), 1);
    // The asked-for key still works.
    Q_EMIT part.m_attachmentView->viewerMessage(s);
    QCOMPARE(skipped.size(), 2);
}

// A link out of an attachment opens in the browser on the reader's click only.
class OpenedLinks : public QObject
{
    Q_OBJECT
public:
    QList<QUrl> urls;
public Q_SLOTS:
    void open(const QUrl &url) { urls.append(url); }
};

void HtmlViewTest::attachmentLinksOpenOnlyOnTheReadersClick()
{
    QVERIFY(Thoughts::HtmlDocumentView::isUserGesture(0));
    QVERIFY(Thoughts::HtmlDocumentView::isUserGesture(Thoughts::HtmlDocumentView::userGestureMilliseconds));
    QVERIFY(!Thoughts::HtmlDocumentView::isUserGesture(Thoughts::HtmlDocumentView::userGestureMilliseconds + 1));
    QVERIFY(!Thoughts::HtmlDocumentView::isUserGesture(-1)); // no input ever seen
#ifdef Q_OS_MACOS
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    Part part(nullptr, {});
    part.widget()->resize(800, 600);
    part.widget()->show();
    part.openDocument(path);
    // Whatever would open goes to this object, never to a browser.
    OpenedLinks opened;
    for (const char *scheme : {"https", "http", "mailto"}) QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), &opened, "open");
    const auto restore = qScopeGuard([] {
        for (const char *scheme : {"https", "http", "mailto"}) QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
    });
    const QUrl leak(QStringLiteral("https://collector.example.org/x?stolen=what+the+page+shows"));
    // The page navigated by itself (location = …): nothing opens.
    Q_EMIT part.m_attachmentView->externalLinkRequested(leak, false);
    QCOMPARE(opened.urls.size(), 0);
    // The reader clicked a link.
    Q_EMIT part.m_attachmentView->externalLinkRequested(QUrl(QStringLiteral("https://example.org/paper")), true);
    QCOMPARE(opened.urls, QList<QUrl>{QUrl(QStringLiteral("https://example.org/paper"))});
#endif
}

void HtmlViewTest::attachmentFollowsItsNewestVersion()
{
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("offscreen only: the newest version replaces the page (the web view restores the place)");
    }
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto first = addAttachment(path, QByteArrayLiteral("<!doctype html><title>Answers</title><p>The first version.</p>"));
    QVERIFY(first.isValid());
    Part page(nullptr, {});
    QVERIFY(page.openHtmlAttachmentTab(QUrl::fromLocalFile(path), {{QStringLiteral("path"), AttachmentPath}}));
    const auto sha256 = [&page] { return page.htmlAttachmentState().value(QStringLiteral("sha256")).toString(); };
    const auto url = [&page] { return page.htmlAttachmentState().value(QStringLiteral("url")).toString(); };
    QCOMPARE(sha256(), first.sha256);
    const QString firstUrl = url();
    QVERIFY(page.m_attachmentWatchedFiles.size() >= 1);
    QVERIFY(!page.keepsEarlierPageVersion());
    QSignalSpy kept(&page, &Part::keptVersionChanged);
    QTest::qWait(1000); // The file system's notifications start (FSEvents on macOS).
    const QString ready = QStringLiteral("A newer version of this page is ready.");

    // Sync brings a newer version (the PDF's bundle pointer is rewritten): as a PDF, the page keeps
    // the version on screen, the tab is marked and the bar offers the newest.
    const auto second = addAttachment(path, QByteArrayLiteral("<!doctype html><title>Answers</title><p>The second version, with the judge.</p>"));
    QVERIFY(second.isValid() && second.sha256 != first.sha256);
    QTRY_VERIFY_WITH_TIMEOUT(page.keepsEarlierPageVersion(), 10000);
    QCOMPARE(kept.size(), 1);
    QCOMPARE(kept.constLast().constFirst().toBool(), true);
    QCOMPARE(sha256(), first.sha256);
    QCOMPARE(page.m_attachmentView->notice(), ready);
    // Show Newest Version on the bar: the newest, where the reader was.
    Q_EMIT page.m_attachmentView->noticeClicked();
    QTRY_COMPARE_WITH_TIMEOUT(sha256(), second.sha256, 10000);
    QVERIFY(url() != firstUrl);
    QVERIFY(!page.keepsEarlierPageVersion());
    QCOMPARE(kept.size(), 2);
    QCOMPARE(kept.constLast().constFirst().toBool(), false);
    QVERIFY(page.m_attachmentView->notice() != ready);
    QVERIFY(page.isHtmlAttachmentTab());
    QCOMPARE(page.m_documentStack->currentWidget(), page.m_attachmentView);

    // Replaced by a rename (written beside it first), as an atomic save does. A version that goes
    // back to the one on screen leaves nothing waiting.
    const QString pointer = page.m_attachmentWatchedFiles.constLast();
    QVERIFY(QFileInfo::exists(pointer));
    const auto third = addAttachment(path, QByteArrayLiteral("<!doctype html><title>Answers</title><p>The third version.</p>"));
    QVERIFY(third.isValid());
    const QByteArray thirdPointer = readFile(pointer);
    QTRY_VERIFY_WITH_TIMEOUT(page.keepsEarlierPageVersion(), 10000);
    QTest::qWait(1200); // Apart from the last write: the file system's notifications merge close ones.
    QVERIFY(addAttachment(path, QByteArrayLiteral("<!doctype html><title>Answers</title><p>The second version, with the judge.</p>")).isValid());
    QTRY_VERIFY_WITH_TIMEOUT(!page.keepsEarlierPageVersion(), 10000);
    QCOMPARE(sha256(), second.sha256);
    QTest::qWait(1200);
    {
        QFile beside(pointer + QStringLiteral(".part"));
        QVERIFY(beside.open(QIODevice::WriteOnly));
        QCOMPARE(beside.write(thirdPointer), thirdPointer.size());
    }
    QCOMPARE(std::rename(QFile::encodeName(pointer + QStringLiteral(".part")).constData(), QFile::encodeName(pointer).constData()), 0);
    QTRY_VERIFY_WITH_TIMEOUT(page.keepsEarlierPageVersion(), 10000);
    QCOMPARE(sha256(), second.sha256);
    QVERIFY(page.showNewestPageVersion());
    QTRY_COMPARE_WITH_TIMEOUT(sha256(), third.sha256, 10000);
    QVERIFY(!page.showNewestPageVersion());

    // While one of its cards is reviewed nothing changes; once the review moves on the newest is offered.
    QTest::qWait(1200);
    page.m_attachmentReviewShown = true;
    const auto fourth = addAttachment(path, QByteArrayLiteral("<!doctype html><title>Answers</title><p>The fourth version.</p>"));
    QVERIFY(fourth.isValid());
    QTRY_VERIFY_WITH_TIMEOUT(page.m_attachmentFollowPending, 10000);
    QVERIFY(!page.keepsEarlierPageVersion());
    page.clearReviewMask();
    QTRY_VERIFY_WITH_TIMEOUT(page.keepsEarlierPageVersion(), 10000);
    QVERIFY(!page.m_attachmentFollowPending);
    QCOMPARE(sha256(), third.sha256);
    QCOMPARE(page.m_attachmentView->notice(), ready);
    QVERIFY(page.showNewestPageVersion());
    QTRY_COMPARE_WITH_TIMEOUT(sha256(), fourth.sha256, 10000);

    // Closed: nothing is watched any more, nothing waits.
    page.closeHtmlAttachment();
    QVERIFY(page.m_attachmentWatchedFiles.isEmpty());
    QVERIFY(!page.keepsEarlierPageVersion());
}

void HtmlViewTest::attachmentCardFromASelection()
{
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("drives the viewer messages by hand: offscreen only (webViewAttachmentCardsAndReview covers the web view)");
    }
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    Part part(nullptr, {});
    part.openDocument(path);
    QString error;
    // Without the server's html_attachment_cards_v1 no card is offered.
    QSettings().remove(QStringLiteral("thoughts-server/capabilities"));
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    const QJsonObject selection{{QStringLiteral("type"), QStringLiteral("selection")},
                                {QStringLiteral("text"), QStringLiteral("Every simulated agent pays the toll")},
                                {QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("Every simulated agent pays the toll")},
                                                                          {QStringLiteral("prefix"), QStringLiteral("tolls ")},
                                                                          {QStringLiteral("suffix"), QStringLiteral(" before it may read")}}},
                                {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("#simulator")}}}};
    Q_EMIT part.m_attachmentView->viewerMessage(selection);
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool());
    QSignalSpy requested(&part, &Part::requestHtmlReviewCard);
    part.requestAttachmentCard();
    QCOMPARE(requested.size(), 0);
    part.closeHtmlAttachment();

    QSettings().setValue(QStringLiteral("thoughts-server/capabilities"), QStringList{QStringLiteral("html_attachment_cards_v1")});
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("cards_supported")).toBool());
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool()); // nothing selected yet
    Q_EMIT part.m_attachmentView->viewerMessage(selection);
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool());
    part.requestAttachmentCard();
    QCOMPARE(requested.size(), 1);
    QCOMPARE(requested.first().at(0).toString(), path);
    const QJsonObject fields = QJsonObject::fromVariantMap(requested.first().at(1).toMap());
    QCOMPARE(fields.value(QStringLiteral("created_in")).toString(), QStringLiteral("html_attachment"));
    QCOMPARE(fields.value(QStringLiteral("html_attachment")).toObject(), QJsonObject({{QStringLiteral("path"), AttachmentPath}, {QStringLiteral("sha256"), target.sha256}}));
    QCOMPARE(fields.value(QStringLiteral("text_quote")).toObject(), selection.value(QStringLiteral("text_quote")).toObject());
    QCOMPARE(fields.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"));
    QCOMPARE(fields.value(QStringLiteral("pdf_stem")).toString(), QStringLiteral("ideas"));
    QCOMPARE(fields.value(QStringLiteral("pdf_sha256")).toString(), sha(readFile(path)));
    QCOMPARE(fields.value(QStringLiteral("document_id")).toString(), DocumentId);
    QCOMPARE(fields.value(QStringLiteral("selected_text")).toString(), QStringLiteral("Every simulated agent pays the toll"));
    for (const QString &absent : {QStringLiteral("page"), QStringLiteral("rect"), QStringLiteral("regions"), QStringLiteral("html_edition_sha256")}) QVERIFY(!fields.contains(absent));
    // A page state that is no location.hash is dropped.
    QJsonObject odd = selection;
    odd.insert(QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("simulator")}});
    QCOMPARE(Thoughts::HtmlAttachmentReader::newCardFields(odd, target).value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QString());
    // A collapsed selection: nothing to make a card of.
    Q_EMIT part.m_attachmentView->viewerMessage({{QStringLiteral("type"), QStringLiteral("selection")}, {QStringLiteral("empty"), true}});
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool());
    QSettings().remove(QStringLiteral("thoughts-server/capabilities"));
}

void HtmlViewTest::attachmentFeedbackFromASelection()
{
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("drives the viewer messages by hand: offscreen only (webViewAttachmentFeedback covers the web view)");
    }
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    Part part(nullptr, {});
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    const QSignalBlocker quietRepository(part.m_fedoraRepository);
    auto *overlay = part.m_pageView->documentFeedback();
    overlay->clear();
    QList<QJsonObject> written;
    overlay->setWriter([&written](const QJsonObject &record, QString *) {
        written.append(record);
        return true;
    });
    QSettings().remove(QStringLiteral("thoughts-server/capabilities"));
    QString error;
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    const QString quoted = QStringLiteral("Every simulated agent pays the toll");
    const QJsonObject selection{{QStringLiteral("type"), QStringLiteral("selection")},
                                {QStringLiteral("text"), quoted},
                                {QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), quoted},
                                                                          {QStringLiteral("prefix"), QString(QString(40, QLatin1Char('p')) + QStringLiteral("tolls "))},
                                                                          {QStringLiteral("suffix"), QString(QStringLiteral(" before it may read") + QString(40, QLatin1Char('s')))}}},
                                {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("#simulator")}}}};
    Q_EMIT part.m_attachmentView->viewerMessage(selection);
    const QJsonObject onSelection{{QStringLiteral("selection"), selection}, {QStringLiteral("items"), QJsonArray()}};
    const auto actions = [&part](const QJsonObject &message) {
        QMenu menu;
        part.addAttachmentMenuActions(&menu, message);
        QStringList names;
        for (const QAction *action : menu.actions()) names.append(action->isSeparator() ? QStringLiteral("|") : action->objectName());
        return names;
    };
    const auto trigger = [&part, &onSelection](const QString &name) {
        QMenu menu;
        part.addAttachmentMenuActions(&menu, onSelection);
        for (QAction *action : menu.actions()) {
            if (action->objectName() == name) action->trigger();
        }
    };

    // Feedback needs a Library document, as on the PDF.
    QCOMPARE(actions(onSelection), QStringList{QStringLiteral("attachmentCopy")});
    const QString pdfSha = sha(readFile(path));
    overlay->setDocument(DocumentId, pdfSha);
    overlay->setReviewVersion({{QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("source_built"), false}});
    const QStringList feedbackActions{QStringLiteral("attachmentFeedbackLike"), QStringLiteral("attachmentFeedbackDislike"), QStringLiteral("attachmentFeedbackQuestion"), QStringLiteral("attachmentFeedbackComment"), QStringLiteral("|")};
    const QStringList withoutCards = feedbackActions + QStringList{QStringLiteral("attachmentCopy")};
    QCOMPARE(actions(onSelection), withoutCards);
    part.m_attachmentCardsSupported = true;
    const QStringList withCards = feedbackActions + QStringList{QStringLiteral("attachmentNewCard"), QStringLiteral("attachmentCopy")};
    QCOMPARE(actions(onSelection), withCards);

    // Like: the record names the page, has no regions, and Annotations opens on its row.
    part.chooseSidebarDisplay(nullptr);
    QVERIFY(!part.m_showLeftPanel->isChecked());
    trigger(QStringLiteral("attachmentFeedbackLike"));
    QCOMPARE(written.size(), 1);
    const QJsonObject liked = written.constLast();
    QVERIFY2(Thoughts::DocumentFeedback::validationError(liked).isEmpty(), qPrintable(Thoughts::DocumentFeedback::validationError(liked)));
    QCOMPARE(liked.value(QStringLiteral("view")).toString(), QStringLiteral("html_attachment"));
    QCOMPARE(liked.value(QStringLiteral("polarity")).toString(), Thoughts::ReviewCardFeedback::positive());
    const QJsonObject anchor = liked.value(QStringLiteral("anchor")).toObject();
    QCOMPARE(anchor.value(QStringLiteral("html_attachment")).toObject(), QJsonObject({{QStringLiteral("path"), AttachmentPath}, {QStringLiteral("sha256"), target.sha256}}));
    QCOMPARE(anchor.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"));
    const QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
    QCOMPARE(pdf.value(QStringLiteral("document_id")).toString(), DocumentId);
    QCOMPARE(pdf.value(QStringLiteral("pdf_sha256")).toString(), pdfSha);
    QVERIFY(pdf.value(QStringLiteral("regions")).toArray().isEmpty());
    const QJsonObject quote = anchor.value(QStringLiteral("quote")).toObject();
    QCOMPARE(quote.value(QStringLiteral("exact")).toString(), quoted);
    QVERIFY(quote.value(QStringLiteral("prefix")).toString().endsWith(QStringLiteral("tolls ")));
    QCOMPARE(quote.value(QStringLiteral("prefix")).toString().size(), Thoughts::ReviewCardFeedback::contextLength);
    QVERIFY(quote.value(QStringLiteral("suffix")).toString().startsWith(QStringLiteral(" before")));
    QCOMPARE(quote.value(QStringLiteral("suffix")).toString().size(), Thoughts::ReviewCardFeedback::contextLength);
    QCOMPARE(part.m_attachmentReader->pageFeedback().size(), 1);
    // Never drawn on the PDF.
    QVERIFY(overlay->rectsOf(liked.value(QStringLiteral("feedback_id")).toString()).isEmpty());
    QVERIFY(part.m_showLeftPanel->isChecked());
    QCOMPARE(part.m_sidebar->currentItem(), part.m_reviewsWidget.data());
    auto *rows = part.m_reviewsWidget->findChild<QTreeView *>();
    QVERIFY(rows);
    QCOMPARE(rows->currentIndex().data(AnnotationModel::ItemIdRole).toString(), liked.value(QStringLiteral("feedback_id")).toString());
    QCOMPARE(rows->currentIndex().data(AnnotationModel::ItemTypeRole).toInt(), int(AnnotationModel::Feedback));
    QCOMPARE(rows->currentIndex().data(AnnotationModel::PageRole).toInt(), -1);
    QCOMPARE(rows->currentIndex().data(AnnotationModel::ReviewCardPagesLabelRole).toString(), QStringLiteral("HTML"));
    QCOMPARE(rows->currentIndex().data(Qt::DisplayRole).toString(), quoted);
    QCOMPARE(rows->currentIndex().data(AnnotationModel::ReviewCardStatusRole).toString(), QStringLiteral("explainer"));

    // Comment…: nothing is saved before Save.
    trigger(QStringLiteral("attachmentFeedbackComment"));
    QCOMPARE(written.size(), 1);
    auto *editor = part.widget()->findChild<Thoughts::ReviewFeedbackEditor *>(QStringLiteral("reviewFeedbackEditor"));
    QVERIFY(editor);
    Q_EMIT editor->saveRequested(Thoughts::ReviewCardFeedback::negative(), QStringLiteral(" Too long "));
    QCOMPARE(written.size(), 2);
    const QJsonObject commented = written.constLast();
    const QString commentedId = commented.value(QStringLiteral("feedback_id")).toString();
    QCOMPARE(commented.value(QStringLiteral("comment")).toString(), QStringLiteral("Too long"));
    QCOMPARE(rows->currentIndex().data(Qt::DisplayRole).toString(), QStringLiteral("Too long"));
    QCOMPARE(rows->currentIndex().data(AnnotationModel::ReviewCardStatusRole).toString(), quoted + QStringLiteral(" · explainer"));
    QCOMPARE(part.m_attachmentReader->pageFeedback().size(), 2);

    // A row brings the page back; a PDF row leaves it.
    part.hideHtmlAttachment();
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    Q_EMIT rows->clicked(rows->currentIndex());
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    part.showFeedback(QStringLiteral("not-an-attachment-record"), false);
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QVERIFY(part.showHtmlAttachment(target, false, &error));

    // A tap on a highlight and Edit Feedback… open the editor, for feedback only.
    const int editors = part.m_pageView->findChildren<Thoughts::ReviewFeedbackEditor *>().size();
    Q_EMIT part.m_attachmentView->viewerMessage({{QStringLiteral("type"), QStringLiteral("itemTapped")}, {QStringLiteral("kind"), QStringLiteral("feedback")}, {QStringLiteral("id"), commentedId}});
    QCOMPARE(part.m_pageView->findChildren<Thoughts::ReviewFeedbackEditor *>().size(), editors + 1);
    qDeleteAll(part.m_pageView->findChildren<Thoughts::ReviewFeedbackEditor *>());
    const QJsonObject onHighlight{{QStringLiteral("items"), QJsonArray{QJsonObject{{QStringLiteral("kind"), QStringLiteral("feedback")}, {QStringLiteral("id"), commentedId}},
                                                                      QJsonObject{{QStringLiteral("kind"), QStringLiteral("card")}, {QStringLiteral("id"), QStringLiteral("a-card")}}}}};
    QCOMPARE(actions(onHighlight), QStringList{QStringLiteral("attachmentEditFeedback")});

    // The app's menu only where the viewer gave it to the app.
    QObject::disconnect(part.m_attachmentReader, &Thoughts::HtmlAttachmentReader::contextMenuRequested, &part, nullptr);
    QSignalSpy menus(part.m_attachmentReader, &Thoughts::HtmlAttachmentReader::contextMenuRequested);
    Q_EMIT part.m_attachmentView->viewerMessage({{QStringLiteral("type"), QStringLiteral("contextmenu")}});
    QCOMPARE(menus.size(), 0);
    Q_EMIT part.m_attachmentView->viewerMessage({{QStringLiteral("type"), QStringLiteral("contextmenu")}, {QStringLiteral("app_menu"), true}});
    QCOMPARE(menus.size(), 1);

    // Delete: the PDF's Undo banner is behind the page, so the page's notice bar offers Undo.
    QVERIFY(part.m_pageView->deleteDocumentFeedback(commentedId));
    QVERIFY(written.constLast().value(QStringLiteral("deleted")).toBool());
    QCOMPARE(part.m_attachmentReader->pageFeedback().size(), 1);
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("notice")).toString(), QStringLiteral("Feedback deleted"));
    Q_EMIT part.m_attachmentView->noticeClicked();
    QVERIFY(!written.constLast().value(QStringLiteral("deleted")).toBool());
    QCOMPARE(part.m_attachmentReader->pageFeedback().size(), 2);
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("notice")).toString().isEmpty());

    // No feedback during a review of a card of this page, also when the row is tapped.
    const QJsonObject card = attachmentCard(Thoughts::HtmlAttachmentReader::newCardFields(selection, target), QStringLiteral("Who pays?"));
    QVERIFY(part.m_attachmentReader->presentReview(card, false));
    QCOMPARE(actions(onSelection), QStringList{QStringLiteral("attachmentCopy")});
    QVERIFY(actions(onHighlight).isEmpty());
    part.giveAttachmentFeedback(Thoughts::ReviewCardFeedback::positive(), false);
    QCOMPARE(written.size(), 4);
    part.m_attachmentReader->clearReview();
    part.closeHtmlAttachment();
}

void HtmlViewTest::webViewAttachmentFeedback()
{
    if (!Thoughts::HtmlDocumentView::webViewAvailable() || qEnvironmentVariable("THOUGHTS_HTML_GUI_TEST") != QLatin1String("1")) {
        QSKIP("needs a real web view: THOUGHTS_HTML_GUI_TEST=1 on the cocoa platform");
    }
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    Part part(nullptr, {});
    QWidget *widget = part.widget();
    widget->resize(1100, 760);
    widget->move(80, 60);
    widget->show();
    part.m_reviewsWidget->setMinimumWidth(300); // As wide as in the app, for the screenshots.
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    const QSignalBlocker quietRepository(part.m_fedoraRepository);
    auto *overlay = part.m_pageView->documentFeedback();
    QList<QJsonObject> written;
    overlay->setWriter([&written](const QJsonObject &record, QString *) {
        written.append(record);
        return true;
    });
    const QString pdfSha = sha(readFile(path));
    overlay->setDocument(DocumentId, pdfSha);
    overlay->setReviewVersion({{QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("source_built"), false}});
    QVERIFY(part.openHtmlAttachmentLink(QUrl(QStringLiteral("thoughts-html:") + AttachmentPath)));
    QTRY_VERIFY_WITH_TIMEOUT(part.htmlAttachmentState().value(QStringLiteral("ready")).toBool(), 20000);
    callAttachment(part, QStringLiteral("setPageState"), QJsonArray{QJsonObject{{QStringLiteral("hash"), QStringLiteral("#simulator")}}});
    QTRY_COMPARE_WITH_TIMEOUT(callAttachment(part, QStringLiteral("getPageState")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"), 5000);
    const QString answer = QStringLiteral("Every simulated agent pays the toll before it may read a single page of the site.");
    QTRY_VERIFY_WITH_TIMEOUT(callAttachment(part, QStringLiteral("selectQuote"), QJsonArray{QJsonObject{{QStringLiteral("exact"), answer}}}).toBool(), 5000);

    // Dislike takes the page's real selection, and the highlight is drawn on the page.
    part.giveAttachmentFeedback(Thoughts::ReviewCardFeedback::negative(), false);
    QTRY_COMPARE_WITH_TIMEOUT(written.size(), 1, 5000);
    const QString id = written.constFirst().value(QStringLiteral("feedback_id")).toString();
    QVERIFY2(Thoughts::DocumentFeedback::validationError(written.constFirst()).isEmpty(), qPrintable(Thoughts::DocumentFeedback::validationError(written.constFirst())));
    QCOMPARE(written.constFirst().value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), answer);
    const auto placed = [&part, id] {
        const QJsonObject placement = QJsonObject::fromVariantMap(part.htmlAttachmentState().value(QStringLiteral("placement")).toMap());
        return placement.value(QStringLiteral("feedback")).toObject().value(QStringLiteral("placed")).toArray().contains(QJsonValue(id));
    };
    QTRY_VERIFY_WITH_TIMEOUT(placed(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(part.htmlAttachmentState().value(QStringLiteral("selection")).toMap().isEmpty(), 5000);
    QCOMPARE(part.m_sidebar->currentItem(), part.m_reviewsWidget.data());
    auto *rows = part.m_reviewsWidget->findChild<QTreeView *>();
    QCOMPARE(rows->currentIndex().data(AnnotationModel::ItemIdRole).toString(), id);
    shot(part, QStringLiteral("f1-attachment-feedback.png"), nullptr, part.m_attachmentView);

    // A right click on it gives the app the menu; on the page's own controls the page keeps it.
    QObject::disconnect(part.m_attachmentReader, &Thoughts::HtmlAttachmentReader::contextMenuRequested, &part, nullptr);
    QSignalSpy menus(part.m_attachmentReader, &Thoughts::HtmlAttachmentReader::contextMenuRequested);
    const QString press = QStringLiteral("(function () {"
        "const range = [...CSS.highlights.get('tv-feedback-negative')][0];"
        "const r = range.getBoundingClientRect();"
        "const x = r.left + 12, y = r.top + r.height / 2;"
        "document.elementFromPoint(x, y).dispatchEvent(new MouseEvent('%1', {bubbles: true, cancelable: true, clientX: x, clientY: y}));"
        "})()");
    part.m_attachmentView->runScript(press.arg(QStringLiteral("contextmenu")));
    QTRY_COMPARE_WITH_TIMEOUT(menus.size(), 1, 5000);
    const QJsonArray under = menus.first().first().toJsonObject().value(QStringLiteral("items")).toArray();
    QCOMPARE(under.first().toObject().value(QStringLiteral("kind")).toString(), QStringLiteral("feedback"));
    part.m_attachmentView->runScript(QStringLiteral("document.getElementById('agents').dispatchEvent(new MouseEvent('contextmenu', {bubbles: true, cancelable: true}))"));
    QTest::qWait(300);
    QCOMPARE(menus.size(), 1);

    // A tap opens its editor.
    const int editors = part.m_pageView->findChildren<Thoughts::ReviewFeedbackEditor *>().size();
    part.m_attachmentView->runScript(press.arg(QStringLiteral("click")));
    QTRY_COMPARE_WITH_TIMEOUT(part.m_pageView->findChildren<Thoughts::ReviewFeedbackEditor *>().size(), editors + 1, 5000);
    qDeleteAll(part.m_pageView->findChildren<Thoughts::ReviewFeedbackEditor *>());

    // The page rebuilds the sentence on input: the highlight follows it.
    part.m_attachmentView->runScript(QStringLiteral("(function () { const el = document.getElementById('agents'); el.value = '70'; el.dispatchEvent(new Event('input', {bubbles: true})); })()"));
    QTest::qWait(800);
    QVERIFY(placed());

    // From another tab, the row brings back the tab of the passage.
    callAttachment(part, QStringLiteral("setPageState"), QJsonArray{QJsonObject{{QStringLiteral("hash"), QStringLiteral("#experiment")}}});
    QTRY_COMPARE_WITH_TIMEOUT(callAttachment(part, QStringLiteral("getPageState")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#experiment"), 5000);
    Q_EMIT rows->clicked(rows->currentIndex());
    QTRY_COMPARE_WITH_TIMEOUT(callAttachment(part, QStringLiteral("getPageState")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(placed(), 5000);
    shot(part, QStringLiteral("f3-attachment-row-clicked.png"), nullptr, part.m_attachmentView);
    part.closeHtmlAttachment();
}

namespace
{
// A press, moves and a release of the left button on @p widget at @p points, as the mouse draws.
void drag(QWidget *widget, const QList<QPointF> &points)
{
    const auto send = [widget](QEvent::Type type, const QPointF &at, Qt::MouseButton button, Qt::MouseButtons buttons) {
        QMouseEvent event(type, at, widget->mapToGlobal(at), button, buttons, Qt::NoModifier);
        QApplication::sendEvent(widget, &event);
    };
    send(QEvent::MouseButtonPress, points.constFirst(), Qt::LeftButton, Qt::LeftButton);
    for (qsizetype index = 1; index < points.size(); ++index) send(QEvent::MouseMove, points.at(index), Qt::NoButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, points.constLast(), Qt::LeftButton, Qt::NoButton);
}

// The row of @p id in @p view, in any group.
QModelIndex rowOf(const QAbstractItemView *view, const QString &id, const QModelIndex &parent = {})
{
    for (int row = 0; row < view->model()->rowCount(parent); ++row) {
        const QModelIndex index = view->model()->index(row, 0, parent);
        if (index.data(AnnotationModel::ItemIdRole).toString() == id) return index;
        if (const QModelIndex found = rowOf(view, id, index); found.isValid()) return found;
    }
    return {};
}

// @p x, @p y of the picture (0..1) in the dialog's canvas.
QPointF onPicture(const Thoughts::SnapshotDrawingDialog *dialog, double x, double y)
{
    const QRectF picture = dialog->pictureRect();
    return QPointF(picture.left() + x * picture.width(), picture.top() + y * picture.height());
}

template<typename Button = QPushButton> Button *dialogButton(const QWidget *dialog, const QString &name)
{
    return dialog->findChild<Button *>(name);
}
}

void HtmlViewTest::snapshotDrawingDialogTakesStrokes()
{
    // Draw on This Page takes a picture (the WKWebView snapshot in the app) and gives strokes
    // normalized to it; it writes nothing itself.
    QImage picture(800, 500, QImage::Format_RGB32);
    picture.fill(Qt::white);
    picture.setDevicePixelRatio(2.0);
    Thoughts::SnapshotDrawingDialog dialog(picture, {}, false, QStringLiteral("#2e7d32"), false);
    QCOMPARE(dialog.windowTitle(), QStringLiteral("Draw on This Page"));
    dialog.show();
    QTRY_VERIFY(!dialog.pictureRect().isEmpty());
    // As large as the page was on screen (its logical size), never larger.
    QVERIFY(dialog.pictureRect().width() <= 400.0 + 0.5);
    QCOMPARE(qRound(dialog.pictureRect().width() * 250.0 / dialog.pictureRect().height()), 400);
    QSignalSpy saves(&dialog, &Thoughts::SnapshotDrawingDialog::saveRequested);
    QSignalSpy chosen(&dialog, &Thoughts::SnapshotDrawingDialog::pencilChosen);
    auto *save = dialogButton(&dialog, QStringLiteral("snapshotDrawingSave"));
    QVERIFY(save);
    QVERIFY(!save->isEnabled());
    QVERIFY(!dialogButton(&dialog, QStringLiteral("snapshotDrawingDelete")));
    QCOMPARE(dialog.colour(), QStringLiteral("#2e7d32"));
    QVERIFY(dialogButton<QToolButton>(&dialog, QStringLiteral("snapshotColour_green"))->isChecked());

    // A stroke: its points on the picture, 0..1 from its top left, in the pencil's colour; one
    // beyond the picture's edge stays on it.
    drag(dialog.canvas(), {onPicture(&dialog, 0.2, 0.3), onPicture(&dialog, 0.4, 0.35), onPicture(&dialog, 1.3, 0.5)});
    QCOMPARE(dialog.strokes().size(), 1);
    const auto first = dialog.strokes().constFirst();
    QCOMPARE(first.colour, QStringLiteral("#2e7d32"));
    QCOMPARE(first.points.size(), 3);
    QVERIFY(std::abs(first.points.at(0).x() - 0.2) < 0.01 && std::abs(first.points.at(0).y() - 0.3) < 0.01);
    QVERIFY(std::abs(first.points.at(1).x() - 0.4) < 0.01 && std::abs(first.points.at(1).y() - 0.35) < 0.01);
    QCOMPARE(first.points.at(2).x(), 1.0);
    QVERIFY(save->isEnabled());
    // Outside the picture nothing starts.
    drag(dialog.canvas(), {QPointF(1.0, 1.0), QPointF(2.0, 2.0)});
    QCOMPARE(dialog.strokes().size(), 1);

    // Another colour, chosen here, is the pencil's everywhere.
    dialogButton<QToolButton>(&dialog, QStringLiteral("snapshotColour_red"))->click();
    QCOMPARE(chosen.size(), 1);
    QCOMPARE(chosen.constFirst().at(0).toString(), QStringLiteral("#d32f2f"));
    drag(dialog.canvas(), {onPicture(&dialog, 0.7, 0.7)});
    QCOMPARE(dialog.strokes().size(), 2);
    QCOMPARE(dialog.strokes().at(1).colour, QStringLiteral("#d32f2f"));
    QCOMPARE(dialog.strokes().at(1).points.size(), 1); // A dot.

    // The eraser takes whole strokes; Undo puts back what one drag took, then the strokes one by one.
    dialogButton<QToolButton>(&dialog, QStringLiteral("snapshotEraser"))->click();
    QVERIFY(dialog.erasing());
    drag(dialog.canvas(), {onPicture(&dialog, 0.3, 0.2), onPicture(&dialog, 0.3, 0.45)});
    QCOMPARE(dialog.strokes().size(), 1);
    QCOMPARE(dialog.strokes().constFirst().colour, QStringLiteral("#d32f2f"));
    dialogButton<QToolButton>(&dialog, QStringLiteral("snapshotUndo"))->click();
    QCOMPARE(dialog.strokes().size(), 2);
    dialog.undo();
    QCOMPARE(dialog.strokes().size(), 1);
    // Clear, and Undo of Clear.
    dialogButton<QToolButton>(&dialog, QStringLiteral("snapshotClear"))->click();
    QVERIFY(dialog.strokes().isEmpty());
    QVERIFY(!save->isEnabled());
    dialog.undo();
    QCOMPARE(dialog.strokes().size(), 1);

    // Save hands over the strokes; the owner closes the dialog once they are written.
    save->click();
    QCOMPARE(saves.size(), 1);
    QCOMPARE(saves.constFirst().at(0).value<QList<Thoughts::DocumentFeedback::Stroke>>().size(), 1);
    QVERIFY(dialog.isVisible());

    // A drawing holds at most 200 strokes.
    dialogButton<QToolButton>(&dialog, QStringLiteral("snapshotColour_blue"))->click();
    QVERIFY(dialog.colour().isEmpty());
    for (int index = dialog.strokes().size(); index < Thoughts::DocumentFeedback::maximumStrokes; ++index) drag(dialog.canvas(), {onPicture(&dialog, 0.5, 0.5)});
    QCOMPARE(dialog.strokes().size(), Thoughts::DocumentFeedback::maximumStrokes);
    QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("snapshotDrawingFull"))->isHidden());
    drag(dialog.canvas(), {onPicture(&dialog, 0.5, 0.5)});
    QCOMPARE(dialog.strokes().size(), Thoughts::DocumentFeedback::maximumStrokes);
    QVERIFY(!dialog.findChild<QLabel *>(QStringLiteral("snapshotDrawingFull"))->isHidden());

    // A drawing made before opens with its strokes, Save only once they change, and Delete.
    const QList<Thoughts::DocumentFeedback::Stroke> kept{{{QPointF(0.1, 0.1), QPointF(0.2, 0.2)}, QString()}};
    Thoughts::SnapshotDrawingDialog existing(picture, kept, true, QString(), false);
    existing.show();
    QTRY_VERIFY(!existing.pictureRect().isEmpty());
    QSignalSpy deletes(&existing, &Thoughts::SnapshotDrawingDialog::deleteRequested);
    QCOMPARE(existing.strokes().size(), 1);
    QVERIFY(!existing.changed());
    QVERIFY(!dialogButton(&existing, QStringLiteral("snapshotDrawingSave"))->isEnabled());
    drag(existing.canvas(), {onPicture(&existing, 0.8, 0.2), onPicture(&existing, 0.9, 0.3)});
    QVERIFY(existing.changed());
    QVERIFY(dialogButton(&existing, QStringLiteral("snapshotDrawingSave"))->isEnabled());
    dialogButton(&existing, QStringLiteral("snapshotDrawingDelete"))->click();
    QCOMPARE(deletes.size(), 1);
}

void HtmlViewTest::attachmentDrawingOnASnapshot()
{
    // The pencil on an HTML page did nothing (18 November): it draws on a snapshot of the page, kept
    // as document feedback of the page (document-feedback.md 10.1), and the PDF's other tools are
    // greyed out there.
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("drives the viewer messages by hand: offscreen only (the WKWebView snapshot itself runs in the app)");
    }
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    Part part(nullptr, {});
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    const QSignalBlocker quietRepository(part.m_fedoraRepository);
    // The document's own feedback is read once the first frame is up and again once its Library
    // identity is verified (openFile): both before this test's own.
    QTRY_VERIFY_WITH_TIMEOUT(!part.m_docsPdfIdentityWatcher || part.m_docsPdfIdentityWatcher->isFinished(), 20000);
    QTest::qWait(50);
    auto *overlay = part.m_pageView->documentFeedback();
    overlay->clear();
    QList<QJsonObject> written;
    overlay->setWriter([&written](const QJsonObject &record, QString *) {
        written.append(record);
        return true;
    });
    const QString pdfSha = sha(readFile(path));
    overlay->setDocument(DocumentId, pdfSha);
    overlay->setReviewVersion({{QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("source_built"), false}});
    Okular::Settings::setPencilColor(QString());
    Okular::Settings::setPencilEraser(false);

    QAction *pencil = part.actionCollection()->action(QStringLiteral("mouse_draw"));
    QAction *textBox = part.actionCollection()->action(QStringLiteral("mouse_text_box"));
    QAction *textSelect = part.actionCollection()->action(QStringLiteral("mouse_textselect"));
    QAction *areaSelect = part.actionCollection()->action(QStringLiteral("mouse_select"));
    QAction *zoom = part.actionCollection()->action(QStringLiteral("mouse_zoom"));
    QVERIFY(pencil && textBox && textSelect && areaSelect && zoom);
    const QList<QAction *> pdfOnly{textBox, textSelect, areaSelect, zoom};
    // On the PDF every tool works, with its own tip.
    for (QAction *tool : pdfOnly) QVERIFY2(tool->isEnabled(), qPrintable(tool->objectName()));
    QCOMPARE(textBox->toolTip(), QStringLiteral("Text box"));
    QCOMPARE(pencil->toolTip(), QStringLiteral("Pencil"));
    textSelect->trigger();
    QVERIFY(textSelect->isChecked());

    // On the page: the PDF's tools are greyed out, the pencil is not.
    QString error;
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    QCOMPARE(part.m_pageView->shownInstead(), PageView::ShownInstead::HtmlPage);
    for (QAction *tool : pdfOnly) {
        QVERIFY2(!tool->isEnabled(), qPrintable(tool->objectName()));
        QCOMPARE(tool->toolTip(), QStringLiteral("Works on PDF pages"));
    }
    QVERIFY(pencil->isEnabled());
    QCOMPARE(pencil->toolTip(), QStringLiteral("Draw on This Page"));
    // A reload of the PDF (updateActionState) keeps them so.
    part.m_pageView->updateActionState(true, false);
    QVERIFY(!textBox->isEnabled());
    QVERIFY(pencil->isEnabled());
    // The pencil takes a picture of the page instead of the PDF's tool: here, offscreen, there is
    // no page to take, and the PDF keeps its tool.
    pencil->trigger();
    QVERIFY(!pencil->isChecked());
    QVERIFY(textSelect->isChecked());
    QVERIFY(!part.widget()->findChild<Thoughts::SnapshotDrawingDialog *>());

    // The picture the app would take: the page as the user saw it.
    QImage picture(800, 500, QImage::Format_RGB32);
    picture.fill(QColor(QStringLiteral("#fafafa")));
    picture.setDevicePixelRatio(2.0);
    part.chooseSidebarDisplay(nullptr);
    part.openNewAttachmentDrawing(picture, target, {{QStringLiteral("hash"), QStringLiteral("#simulator")}});
    QPointer<Thoughts::SnapshotDrawingDialog> dialog = part.widget()->findChild<Thoughts::SnapshotDrawingDialog *>(QStringLiteral("snapshotDrawingDialog"));
    QVERIFY(dialog);
    QVERIFY(dialog->isVisible());
    QTRY_VERIFY(!dialog->pictureRect().isEmpty());
    QVERIFY(written.isEmpty());
    drag(dialog->canvas(), {onPicture(dialog, 0.25, 0.4), onPicture(dialog, 0.5, 0.45), onPicture(dialog, 0.75, 0.6)});
    // Cancel keeps nothing.
    QCOMPARE(dialog->strokes().size(), 1);
    dialogButton(dialog, QStringLiteral("snapshotDrawingSave"))->click();
    // Annotations opens on its row, as for other new feedback.
    QVERIFY(part.m_showLeftPanel->isChecked());
    QCOMPARE(part.m_sidebar->currentItem(), part.m_reviewsWidget.data());
    QCOMPARE(part.m_reviewsWidget->findChild<QTreeView *>()->currentIndex().data(AnnotationModel::ItemIdRole).toString(), written.value(0).value(QStringLiteral("feedback_id")).toString());
    QTRY_VERIFY(!dialog);

    // Saved: the picture is kept on this computer, and the record names it, the page and its state.
    QCOMPARE(written.size(), 1);
    const QJsonObject drawn = written.constLast();
    const QString id = drawn.value(QStringLiteral("feedback_id")).toString();
    QVERIFY2(Thoughts::DocumentFeedback::validationError(drawn).isEmpty(), qPrintable(Thoughts::DocumentFeedback::validationError(drawn)));
    QVERIFY(Thoughts::DocumentFeedback::isAttachmentDrawing(drawn));
    const QJsonObject anchor = drawn.value(QStringLiteral("anchor")).toObject();
    QCOMPARE(anchor.value(QStringLiteral("html_attachment")).toObject(), QJsonObject({{QStringLiteral("path"), AttachmentPath}, {QStringLiteral("sha256"), target.sha256}}));
    QCOMPARE(anchor.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"));
    QCOMPARE(anchor.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), QStringLiteral("Drawing"));
    QCOMPARE(anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("document_id")).toString(), DocumentId);
    QCOMPARE(anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("pdf_sha256")).toString(), pdfSha);
    QVERIFY(anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray().isEmpty());
    const QJsonObject snapshot = Thoughts::DocumentFeedback::snapshotOf(drawn);
    QCOMPARE(snapshot.value(QStringLiteral("media_type")).toString(), QStringLiteral("image/png"));
    QCOMPARE(snapshot.value(QStringLiteral("width")).toInt(), 800);
    QCOMPARE(snapshot.value(QStringLiteral("height")).toInt(), 500);
    const QByteArray kept = part.m_fedoraRepository->feedbackSnapshot(Thoughts::DocumentFeedback::snapshotSha256(drawn));
    QVERIFY(!kept.isEmpty());
    QCOMPARE(QImage::fromData(kept).size(), QSize(800, 500));
    const auto strokes = Thoughts::DocumentFeedback::strokesOf(drawn);
    QCOMPARE(strokes.size(), 1);
    QVERIFY(std::abs(strokes.constFirst().points.constFirst().x() - 0.25) < 0.01 && std::abs(strokes.constFirst().points.constFirst().y() - 0.4) < 0.01);
    QCOMPARE(drawn.value(QStringLiteral("review_version")).toObject().value(QStringLiteral("pdf_sha256")).toString(), pdfSha);

    // Never on the live page, never on the PDF.
    QVERIFY(part.m_attachmentReader->pageFeedback().isEmpty());
    QVERIFY(overlay->rectsOf(id).isEmpty());
    QImage page(300, 400, QImage::Format_RGB32);
    page.fill(Qt::white);
    QPainter painter(&page);
    overlay->paint(&painter, {1, 2}, [](int, const QRectF &rect) { return QRectF(rect.x() * 300.0, rect.y() * 400.0, rect.width() * 300.0, rect.height() * 400.0); }, false);
    painter.end();
    QImage blank(300, 400, QImage::Format_RGB32);
    blank.fill(Qt::white);
    QCOMPARE(page, blank);

    // In the Feedback list of the document, as other feedback of the page: "Drawing", in the page.
    QVERIFY(part.m_showLeftPanel->isChecked());
    auto *rows = part.m_reviewsWidget->findChild<QTreeView *>();
    QVERIFY(rows);
    const QModelIndex row = rowOf(rows, id);
    QVERIFY(row.isValid());
    QCOMPARE(row.data(AnnotationModel::ItemTypeRole).toInt(), int(AnnotationModel::Feedback));
    QCOMPARE(row.data(Qt::DisplayRole).toString(), QStringLiteral("Drawing"));
    QCOMPARE(row.data(AnnotationModel::ReviewCardPagesLabelRole).toString(), QStringLiteral("HTML"));
    QCOMPARE(row.data(AnnotationModel::ReviewCardStatusRole).toString(), QStringLiteral("explainer"));

    // Its row opens its snapshot with its strokes, not the page; more strokes are the next revision.
    part.hideHtmlAttachment();
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QCOMPARE(part.m_pageView->shownInstead(), PageView::ShownInstead::Nothing);
    Q_EMIT rows->clicked(rowOf(rows, id));
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    dialog = part.widget()->findChild<Thoughts::SnapshotDrawingDialog *>(QStringLiteral("snapshotDrawingDialog"));
    QVERIFY(dialog);
    QTRY_VERIFY(!dialog->pictureRect().isEmpty());
    QCOMPARE(dialog->strokes().size(), 1);
    QVERIFY(dialogButton(dialog, QStringLiteral("snapshotDrawingDelete")));
    QVERIFY(!dialogButton(dialog, QStringLiteral("snapshotDrawingSave"))->isEnabled());
    drag(dialog->canvas(), {onPicture(dialog, 0.1, 0.9), onPicture(dialog, 0.2, 0.8)});
    dialogButton(dialog, QStringLiteral("snapshotDrawingSave"))->click();
    QTRY_VERIFY(!dialog);
    QCOMPARE(written.size(), 2);
    const QJsonObject grown = written.constLast();
    QCOMPARE(grown.value(QStringLiteral("feedback_id")).toString(), id);
    QCOMPARE(grown.value(QStringLiteral("revision")).toInt(), 2);
    QCOMPARE(grown.value(QStringLiteral("anchor")), drawn.value(QStringLiteral("anchor")));
    QCOMPARE(Thoughts::DocumentFeedback::snapshotOf(grown), snapshot);
    QCOMPARE(Thoughts::DocumentFeedback::strokesOf(grown).size(), 2);
    QVERIFY(Thoughts::DocumentFeedback::validationError(grown).isEmpty());

    // Delete, as other feedback: the tombstone, with Undo.
    part.showFeedback(id, true);
    dialog = part.widget()->findChild<Thoughts::SnapshotDrawingDialog *>(QStringLiteral("snapshotDrawingDialog"));
    QVERIFY(dialog);
    QCOMPARE(dialog->strokes().size(), 2);
    dialogButton(dialog, QStringLiteral("snapshotDrawingDelete"))->click();
    QTRY_VERIFY(!dialog);
    QCOMPARE(written.size(), 3);
    QVERIFY(written.constLast().value(QStringLiteral("deleted")).toBool());
    QVERIFY(overlay->shownRecords().isEmpty());

    // Back on the PDF the tools work again; the HTML edition keeps no drawings either.
    for (QAction *tool : pdfOnly) QVERIFY2(tool->isEnabled(), qPrintable(tool->objectName()));
    QCOMPARE(textBox->toolTip(), QStringLiteral("Text box"));
    QCOMPARE(pencil->toolTip(), QStringLiteral("Pencil"));
    part.m_pageView->setShownInstead(PageView::ShownInstead::HtmlEdition);
    QVERIFY(!pencil->isEnabled());
    QCOMPARE(pencil->toolTip(), QStringLiteral("Works on PDF pages"));
    part.m_pageView->setShownInstead(PageView::ShownInstead::Nothing);
    QVERIFY(pencil->isEnabled());
    // On the PDF the pencil is the PDF's pencil again.
    pencil->trigger();
    QVERIFY(pencil->isChecked());
    QVERIFY(!part.widget()->findChild<Thoughts::SnapshotDrawingDialog *>());
    textSelect->trigger();
    part.closeHtmlAttachment();
}

void HtmlViewTest::attachmentCardReviewWithoutPageShowsStoredAnswer()
{
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("offscreen only: without a web view the page cannot mask the answer");
    }
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    Part part(nullptr, {});
    part.openDocument(path);
    QJsonObject fields = Thoughts::HtmlAttachmentReader::newCardFields(
        {{QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("one cent per page")}}}, {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("#experiment")}}}}, target);
    fields.insert(QStringLiteral("pdf_stem"), QStringLiteral("ideas"));
    const QJsonObject card = attachmentCard(fields, QStringLiteral("How much does an agent pay?"));
    QCOMPARE(Thoughts::HtmlAttachmentReader::storedAnswer(card), QStringLiteral("one cent per page"));
    QSignalSpy notFound(&part, &Part::reviewAnswerNotFound);
    part.setReviewFeedbackCard(card, {});
    // The review opens the page for the card (no PDF place needed) and, without a page, says so.
    QVERIFY(part.showReviewMasks({}, true));
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("for_review")).toBool());
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("review_card")).toString(), card.value(QStringLiteral("card_id")).toString());
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool());
    QTRY_COMPARE_WITH_TIMEOUT(notFound.size(), 1, 5000);
    QCOMPARE(notFound.first().at(0).toString(), card.value(QStringLiteral("card_id")).toString());
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("review_state")).toString(), QStringLiteral("not_found"));
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("notice")).toString(), QStringLiteral("Answer not found in the current page."));
    QCOMPARE(part.reviewMaskBlockCount(), 0);
    part.revealReviewMask();
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("review_revealed")).toBool());
    // The next card elsewhere: the page opened for the review goes.
    part.clearReviewMask();
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("review_card")).toString().isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool(), 2000);
    QCOMPARE(part.m_documentStack->currentWidget(), part.m_pageView.data());
    // A card whose attachment is gone and not cached: not presented (the review skips it).
    QJsonObject lost = card;
    QJsonObject primary = lost.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("html_attachment"), QJsonObject{{QStringLiteral("path"), QStringLiteral("docs-ideas/tex/support/html/gone.html")}, {QStringLiteral("sha256"), QString(64, QLatin1Char('f'))}});
    lost.insert(QStringLiteral("primary"), primary);
    part.setReviewFeedbackCard(lost, {});
    QVERIFY(!part.showReviewMasks({}, true));
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    part.clearReviewMask();
}

void HtmlViewTest::webViewAttachmentCardsAndReview()
{
    if (!Thoughts::HtmlDocumentView::webViewAvailable() || qEnvironmentVariable("THOUGHTS_HTML_GUI_TEST") != QLatin1String("1")) {
        QSKIP("needs a real web view: THOUGHTS_HTML_GUI_TEST=1 on the cocoa platform");
    }
    const QString path = addLibraryPdf(QStringLiteral("ideas"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path);
    QVERIFY(target.isValid());
    QSettings().setValue(QStringLiteral("thoughts-server/capabilities"), QStringList{QStringLiteral("html_attachment_cards_v1")});
    Part part(nullptr, {});
    QWidget *widget = part.widget();
    widget->resize(1000, 760);
    widget->move(80, 60);
    widget->show();
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    // A thoughts-html: link of the PDF opens the page here.
    QVERIFY(part.openHtmlAttachmentLink(QUrl(QStringLiteral("thoughts-html:") + AttachmentPath)));
    QTRY_VERIFY_WITH_TIMEOUT(part.htmlAttachmentState().value(QStringLiteral("ready")).toBool(), 20000);
    // No network for the page: WKScriptMessageHandler (WebView2's postMessage on Windows) carries
    // the viewer's messages.
#ifdef Q_OS_WIN
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("transport")).toString(), QStringLiteral("webview2"));
#else
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("transport")).toString(), QStringLiteral("webkit"));
#endif
    const QJsonObject info = callAttachment(part, QStringLiteral("info")).toObject();
    QCOMPARE(info.value(QStringLiteral("mode")).toString(), QStringLiteral("attachment"));
    QCOMPARE(info.value(QStringLiteral("version")).toString(), QStringLiteral("0.3.0"));
    // The page's own script rendered its text (400 ms after load) and routes tabs by #hash.
    callAttachment(part, QStringLiteral("setPageState"), QJsonArray{QJsonObject{{QStringLiteral("hash"), QStringLiteral("#simulator")}}});
    QTRY_COMPARE_WITH_TIMEOUT(callAttachment(part, QStringLiteral("getPageState")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"), 5000);
    const QString answer = QStringLiteral("Every simulated agent pays the toll before it may read a single page of the site.");
    QTRY_VERIFY_WITH_TIMEOUT(callAttachment(part, QStringLiteral("selectQuote"), QJsonArray{QJsonObject{{QStringLiteral("exact"), answer}}}).toBool(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool(), 5000);
    const QJsonObject selection = QJsonObject::fromVariantMap(part.htmlAttachmentState().value(QStringLiteral("selection")).toMap());
    QCOMPARE(selection.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"));
    shot(part, QStringLiteral("a1-attachment-selection.png"), nullptr, part.m_attachmentView);
    QSignalSpy requested(&part, &Part::requestHtmlReviewCard);
    part.requestAttachmentCard();
    QTRY_COMPARE_WITH_TIMEOUT(requested.size(), 1, 5000);
    QJsonObject fields = QJsonObject::fromVariantMap(requested.first().at(1).toMap());
    QCOMPARE(fields.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")).toString(), answer);
    QCOMPARE(fields.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"));
    QCOMPARE(fields.value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("sha256")).toString(), target.sha256);
    const QJsonObject card = attachmentCard(fields, QStringLiteral("What does every simulated agent do?"));
    const QString cardId = card.value(QStringLiteral("card_id")).toString();
    // The card is highlighted where its text is (also after the page re-renders the tab).
    part.setSidebarReviewCards(QJsonArray{card});
    const auto placed = [&part, cardId] {
        const QJsonObject placement = QJsonObject::fromVariantMap(part.htmlAttachmentState().value(QStringLiteral("placement")).toMap());
        return placement.value(QStringLiteral("cards")).toObject().value(QStringLiteral("placed")).toArray().contains(QJsonValue(cardId));
    };
    QTRY_VERIFY_WITH_TIMEOUT(placed(), 5000);
    part.m_attachmentView->runScript(QStringLiteral("window.__render && window.__render()"));
    QTest::qWait(800);
    QVERIFY(placed());
    shot(part, QStringLiteral("a2-attachment-highlight.png"), nullptr, part.m_attachmentView);

    // Review: from another tab, the card's page state comes back and its answer is masked.
    callAttachment(part, QStringLiteral("setPageState"), QJsonArray{QJsonObject{{QStringLiteral("hash"), QStringLiteral("#experiment")}}});
    QTest::qWait(600);
    part.setReviewFeedbackCard(card, {});
    QVERIFY(part.showReviewMasks({}, true));
    QTRY_COMPARE_WITH_TIMEOUT(part.htmlAttachmentState().value(QStringLiteral("review_state")).toString(), QStringLiteral("hidden"), 10000);
    QVERIFY(part.reviewMaskBlockCount() > 0);
    QCOMPARE(callAttachment(part, QStringLiteral("getPageState")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#simulator"));
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool());
    // The masked answer cannot be selected or copied.
    callAttachment(part, QStringLiteral("selectQuote"), QJsonArray{QJsonObject{{QStringLiteral("exact"), answer}}});
    QVERIFY(!callAttachment(part, QStringLiteral("copySelection")).toString().contains(QStringLiteral("simulated agent")));
    callAttachment(part, QStringLiteral("clearSelection"));
    shot(part, QStringLiteral("a3-attachment-masked.png"), nullptr, part.m_attachmentView);
    // S in the page reaches the review panel's Skip unless the page cancels it itself (the viewer
    // decides after the page's own listeners). Synthetic keydowns: nothing is typed into the system.
    {
        auto *panel = new QWidget(widget, Qt::Tool | Qt::FramelessWindowHint);
        auto *skip = new QShortcut(QKeySequence(Qt::Key_S), panel);
        skip->setContext(Qt::WindowShortcut);
        QSignalSpy skipped(skip, &QShortcut::activated);
        panel->resize(10, 10);
        panel->show();
        const auto keydown = [&](const QString &setup) {
            bool done = false;
            part.m_attachmentView->runScript(QStringLiteral("(function(){%1var e=new KeyboardEvent('keydown',{key:'s',code:'KeyS',bubbles:true,cancelable:true});"
                                                            "document.body.dispatchEvent(e);return e.defaultPrevented;})()")
                                                 .arg(setup),
                                             [&](const QVariant &) { done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        };
        keydown(QString());
        QTRY_COMPARE_WITH_TIMEOUT(skipped.size(), 1, 5000);
        keydown(QStringLiteral("document.addEventListener('keydown',function(e){if(e.key==='s')e.preventDefault();});"));
        QTest::qWait(500);
        QCOMPARE(skipped.size(), 1);
        delete panel;
    }
    part.revealReviewMask();
    QTRY_COMPARE_WITH_TIMEOUT(part.htmlAttachmentState().value(QStringLiteral("review_state")).toString(), QStringLiteral("revealed"), 5000);
    shot(part, QStringLiteral("a4-attachment-revealed.png"), nullptr, part.m_attachmentView);
    part.clearReviewMask();
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("shown")).toBool()); // opened by the link, stays

    // The page no longer has the text: "Answer not found", the review shows the stored answer.
    QJsonObject changed = card;
    QJsonObject primary = changed.value(QStringLiteral("primary")).toObject();
    primary.insert(QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("This sentence was removed from the page long ago.")}});
    primary.insert(QStringLiteral("selected_text"), QStringLiteral("This sentence was removed from the page long ago."));
    changed.insert(QStringLiteral("primary"), primary);
    changed.insert(QStringLiteral("card_id"), QUuid::createUuid().toString(QUuid::WithoutBraces));
    QSignalSpy notFound(&part, &Part::reviewAnswerNotFound);
    part.setReviewFeedbackCard(changed, {});
    QVERIFY(part.showReviewMasks({}, true));
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("review_state")).toString(), QStringLiteral("pending"));
    QTRY_COMPARE_WITH_TIMEOUT(notFound.size(), 1, 15000);
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("review_state")).toString(), QStringLiteral("not_found"));
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("notice")).toString(), QStringLiteral("Answer not found in the current page."));
    shot(part, QStringLiteral("a5-attachment-not-found.png"), nullptr, part.m_attachmentView);
    part.clearReviewMask();
    // The page cannot navigate the view away (a loopback URL: nothing is opened outside).
    QSignalSpy external(part.m_attachmentView, &Thoughts::HtmlDocumentView::externalLinkRequested);
    const QString shownUrl = part.htmlAttachmentState().value(QStringLiteral("url")).toString();
    part.m_attachmentView->runScript(QStringLiteral("location.href = 'http://127.0.0.1:9/elsewhere.html'"));
    QTest::qWait(1500);
    QTRY_VERIFY_WITH_TIMEOUT(part.htmlAttachmentState().value(QStringLiteral("ready")).toBool(), 20000);
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("url")).toString(), shownUrl);
    QCOMPARE(callAttachment(part, QStringLiteral("info")).toObject().value(QStringLiteral("mode")).toString(), QStringLiteral("attachment"));
    QCOMPARE(external.size(), 0);
    // Back to the PDF.
    Q_EMIT part.m_attachmentView->showPdfRequested();
    QCOMPARE(part.m_documentStack->currentWidget(), part.m_pageView.data());
    QSettings().remove(QStringLiteral("thoughts-server/capabilities"));
}

void HtmlViewTest::availabilityAndRememberedSwitch()
{
    const QString path = addLibraryPdf(QStringLiteral("sample"), QStringLiteral("source_linked"),
                                       {{QStringLiteral("html_status"), QStringLiteral("ready")}, {QStringLiteral("html_edition_sha256"), m_editionSha}});
    QVERIFY(!path.isEmpty());
    {
        Part part(nullptr, {});
        part.openDocument(path);
        QVERIFY(part.m_document->isOpened());
        QVariantMap state = part.htmlViewState();
        QCOMPARE(state.value(QStringLiteral("active")).toBool(), false);
        QCOMPARE(state.value(QStringLiteral("availability")).toString(), QStringLiteral("ready"));
        auto *action = qobject_cast<KToggleAction *>(part.actionCollection()->action(QStringLiteral("view_html_edition")));
        QVERIFY(action);
        QVERIFY(action->isEnabled());
        QVERIFY(!action->isChecked());
        part.setDocumentView(QStringLiteral("html"));
        QVERIFY(part.htmlViewActive());
        QVERIFY(action->isChecked());
        QCOMPARE(part.m_documentStack->currentWidget(), part.m_htmlView);
        QCOMPARE(part.widget()->property("thoughtsNativeDocumentView").toBool(), Thoughts::HtmlDocumentView::webViewAvailable());
        if (!Thoughts::HtmlDocumentView::webViewAvailable()) {
            QVERIFY(part.m_htmlView->message().contains(QStringLiteral("HTML not available")));
        }
    }
    {
        // Remembered for this document: opening it again shows the HTML view.
        Part part(nullptr, {});
        part.openDocument(path);
        QVERIFY(part.htmlViewActive());
        part.setDocumentView(QStringLiteral("pdf"));
        QVERIFY(!part.htmlViewActive());
        QCOMPARE(part.m_documentStack->currentWidget(), part.m_pageView.data());
    }
    {
        Part part(nullptr, {});
        part.openDocument(path);
        QVERIFY(!part.htmlViewActive());
    }
}

void HtmlViewTest::importedAndPendingDocuments()
{
    const QString imported = addLibraryPdf(QStringLiteral("imported"), QStringLiteral("unlinked"), {});
    const QString pending = addLibraryPdf(QStringLiteral("pending"), QStringLiteral("source_linked"), {{QStringLiteral("html_status"), QStringLiteral("pending")}});
    const QString failed = addLibraryPdf(QStringLiteral("failed"), QStringLiteral("source_linked"),
                                         {{QStringLiteral("html_status"), QStringLiteral("failed")}, {QStringLiteral("html_error"), QStringLiteral("make4ht: undefined control sequence")}});
    Part part(nullptr, {});
    part.openDocument(imported);
    part.setDocumentView(QStringLiteral("html"));
    QCOMPARE(part.htmlViewState().value(QStringLiteral("availability")).toString(), QStringLiteral("unavailable"));
    QCOMPARE(part.m_htmlView->message(), QStringLiteral("HTML not available: imported PDFs have no HTML edition"));
    part.setDocumentView(QStringLiteral("pdf"));

    part.openDocument(pending);
    part.setDocumentView(QStringLiteral("html"));
    QCOMPARE(part.htmlViewState().value(QStringLiteral("availability")).toString(), QStringLiteral("building"));
    QVERIFY(part.m_htmlView->message().startsWith(QStringLiteral("HTML is being built")));
    part.setDocumentView(QStringLiteral("pdf"));

    part.openDocument(failed);
    part.setDocumentView(QStringLiteral("html"));
    QCOMPARE(part.m_htmlView->message(), QStringLiteral("HTML not available: make4ht: undefined control sequence"));
    // The PDF is never affected: back to it, nothing remembered for a document without an edition.
    part.setDocumentView(QStringLiteral("pdf"));
    QVERIFY(part.m_document->isOpened());
}

void HtmlViewTest::linksOpenTheLinkedDocument()
{
    // thoughts-link: as the converter writes it (\SourceLink with the target's PDF and destination).
    const QString path = addLibraryPdf(QStringLiteral("sample"), QStringLiteral("source_linked"),
                                       {{QStringLiteral("html_status"), QStringLiteral("ready")}, {QStringLiteral("html_edition_sha256"), m_editionSha}});
    const QString target = m_docs + QStringLiteral("/current_pdfs/notes_notes_754b6dc3f872.pdf");
    QFile::remove(target);
    Part part(nullptr, {});
    part.openDocument(path);
    QSignalSpy opened(&part, &Part::requestOpenDocumentLink);
    const QString href = QStringLiteral("thoughts-link:source?path=docs-notes/notes.md&label=source-label%3Apart&destination=source-label%3Apart&filename=notes_notes_754b6dc3f872.pdf");
    const QJsonObject message{{QStringLiteral("type"), QStringLiteral("link")}, {QStringLiteral("kind"), QStringLiteral("thoughts")}, {QStringLiteral("href"), href}, {QStringLiteral("target"), href}};
    // Not on this computer yet: nothing opens.
    part.openHtmlLink(message);
    QCOMPARE(opened.size(), 0);
    QVERIFY(QFile::copy(QStringLiteral(KDESRCDIR "data/file1.pdf"), target));
    part.openHtmlLink(message);
    QCOMPARE(opened.size(), 1);
    QCOMPARE(opened.first().at(0).toUrl(), QUrl::fromLocalFile(target));
    QCOMPARE(opened.first().at(1).toString(), QStringLiteral("source-label:part"));
    // A PDF link by key and anchor; document-start is no destination.
    const QString pdfHref = QStringLiteral("thoughts-link:cross-pdf?folder=docs-x&key=notes_notes_754b6dc3f872&anchor=document-start");
    part.openHtmlLink({{QStringLiteral("kind"), QStringLiteral("thoughts")}, {QStringLiteral("target"), pdfHref}});
    QCOMPARE(opened.size(), 2);
    QCOMPARE(opened.last().at(1).toString(), QString());
    // Only http(s)/mailto web links leave the app; a javascript: target is ignored.
    part.openHtmlLink({{QStringLiteral("kind"), QStringLiteral("other")}, {QStringLiteral("target"), QStringLiteral("javascript:alert(1)")}});
    QCOMPARE(opened.size(), 2);
}

void HtmlViewTest::forwardedKeys()
{
    // Keys the page hands back (viewer forwardKeys) reach the app's shortcuts: the review's S
    // (also from a panel floating as a tool window), Read Aloud of the page's selection, Escape.
    qputenv("THOUGHTS_READ_ALOUD_KEY_STORE", "none");
    qputenv("THOUGHTS_OPENROUTER_SPEECH_URL", "http://127.0.0.1:9");
    qputenv("THOUGHTS_READ_ALOUD_ENGINE", "mock");
    const auto restoreEnvironment = qScopeGuard([] {
        qunsetenv("THOUGHTS_READ_ALOUD_KEY_STORE");
        qunsetenv("THOUGHTS_OPENROUTER_SPEECH_URL");
        qunsetenv("THOUGHTS_READ_ALOUD_ENGINE");
    });
    const QString path = addLibraryPdf(QStringLiteral("sample"), QStringLiteral("source_linked"),
                                       {{QStringLiteral("html_status"), QStringLiteral("ready")}, {QStringLiteral("html_edition_sha256"), m_editionSha}});
    QSettings().remove(QStringLiteral("html-view"));
    Part part(nullptr, {});
    QWidget *widget = part.widget();
    widget->resize(800, 600);
    widget->show();
    part.openDocument(path);
    part.setDocumentView(QStringLiteral("html"));
    QVERIFY(part.htmlViewActive());

    // What the page is asked to forward. On macOS Qt's Ctrl is ⌘, the page's Meta.
#ifdef Q_OS_MACOS
    const QString command = QStringLiteral("meta");
    const QString readAloudSpec = QStringLiteral("Meta+Shift+u");
#else
    const QString command = QStringLiteral("ctrl");
    const QString readAloudSpec = QStringLiteral("Ctrl+Shift+u");
#endif
    const QJsonArray forwarded = part.m_htmlReader->loadOptions().value(QStringLiteral("forwardKeys")).toArray();
    QVERIFY2(forwarded.contains(QStringLiteral("s")), QJsonDocument(forwarded).toJson().constData());
    QVERIFY(forwarded.contains(QStringLiteral("Escape")));
    QVERIFY2(forwarded.contains(readAloudSpec), QJsonDocument(forwarded).toJson().constData());

    const auto key = [](const QString &name, const QString &code, const QStringList &modifiers = {}, bool repeat = false) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("key")},
                           {QStringLiteral("key"), name},
                           {QStringLiteral("code"), code},
                           {QStringLiteral("meta"), modifiers.contains(QStringLiteral("meta"))},
                           {QStringLiteral("ctrl"), modifiers.contains(QStringLiteral("ctrl"))},
                           {QStringLiteral("alt"), modifiers.contains(QStringLiteral("alt"))},
                           {QStringLiteral("shift"), modifiers.contains(QStringLiteral("shift"))},
                           {QStringLiteral("forwarded"), true},
                           {QStringLiteral("repeat"), repeat}};
    };

    // The review panel's Skip: a window shortcut of a panel inside the reader, then floating.
    auto *panel = new QWidget(widget);
    auto *skip = new QShortcut(QKeySequence(Qt::Key_S), panel);
    skip->setContext(Qt::WindowShortcut);
    QSignalSpy skipped(skip, &QShortcut::activated);
    panel->show();
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("s"), QStringLiteral("KeyS")));
    QCOMPARE(skipped.size(), 1);
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("s"), QStringLiteral("KeyS"), {}, true)); // auto-repeat
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("S"), QStringLiteral("KeyS"), {QStringLiteral("shift")}));
    QJsonObject notForwarded = key(QStringLiteral("s"), QStringLiteral("KeyS"));
    notForwarded.remove(QStringLiteral("forwarded"));
    part.m_htmlReader->onViewerMessage(notForwarded);
    QCOMPARE(skipped.size(), 1);
    panel->setWindowFlags(Qt::Tool | Qt::FramelessWindowHint);
    panel->show();
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("s"), QStringLiteral("KeyS")));
    QCOMPARE(skipped.size(), 2);
    panel->hide(); // no review: S does nothing
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("s"), QStringLiteral("KeyS")));
    QCOMPARE(skipped.size(), 2);

    // Read Aloud (⌘⇧U) reads the page's selection; Escape stops it.
    part.m_htmlReader->m_lastSelection = QJsonObject{{QStringLiteral("text"), QStringLiteral("Words selected in the HTML view.")}};
    QSignalSpy events(&part, SIGNAL(developerEvent(QString, QJsonObject)));
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("U"), QStringLiteral("KeyU"), {command, QStringLiteral("shift")}));
    QVERIFY(part.m_htmlReadAloud);
    QVERIFY(part.m_htmlReadAloud->active());
    QCOMPARE(events.value(0).value(0).toString(), QStringLiteral("read_aloud.started"));
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("Escape"), QStringLiteral("Escape")));
    QVERIFY(!part.m_htmlReadAloud->active());

    // In the PDF view nothing is forwarded any more.
    part.setDocumentView(QStringLiteral("pdf"));
    panel->show();
    part.m_htmlReader->onViewerMessage(key(QStringLiteral("s"), QStringLiteral("KeyS")));
    QCOMPARE(skipped.size(), 2);
    delete panel;
}

void HtmlViewTest::webViewReviewSelectionAndCards()
{
    if (!Thoughts::HtmlDocumentView::webViewAvailable() || qEnvironmentVariable("THOUGHTS_HTML_GUI_TEST") != QLatin1String("1")) {
        QSKIP("needs a real web view: THOUGHTS_HTML_GUI_TEST=1 on the cocoa platform");
    }
    const QString path = addLibraryPdf(QStringLiteral("sample"), QStringLiteral("source_linked"),
                                       {{QStringLiteral("html_status"), QStringLiteral("ready")}, {QStringLiteral("html_edition_sha256"), m_editionSha}});
    QSettings().remove(QStringLiteral("html-view"));
    Part part(nullptr, {});
    QWidget *widget = part.widget();
    widget->resize(1000, 760);
    widget->move(80, 60);
    widget->show();
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    // Cards of the document (as the shell's sidebar list gives them): highlighted from their anchors.
    QJsonArray cards;
    const int cardCount = int(m_manifest.value(QStringLiteral("items")).toObject().value(QStringLiteral("cards")).toObject().size());
    for (int i = 0; i < cardCount; ++i) cards.append(sampleCard(i));
    part.setSidebarReviewCards(cards);
    part.setDocumentView(QStringLiteral("html"));
    QTRY_VERIFY_WITH_TIMEOUT(part.htmlViewState().value(QStringLiteral("ready")).toBool(), 20000);
    QTest::qWait(500);
    QCOMPARE(part.m_htmlReader->notShownCardIds(), QStringList());
    const QJsonObject info = callViewer(part, QStringLiteral("info")).toObject();
    QVERIFY2(!info.isEmpty(), "viewer info");
    shot(part, QStringLiteral("01-html-view.png"));
    // A document link click reaches the app (the page never navigates).
    QSignalSpy links(part.m_htmlReader, &Thoughts::HtmlReader::linkActivated);
    bool clicked = false;
    part.m_htmlView->runScript(QStringLiteral("(function(){var a=document.querySelector('a[data-thoughts-link]');if(!a)return false;a.click();return true;})()"),
                               [&](const QVariant &result) { clicked = result.toBool(); });
    QTest::qWait(300);
    if (clicked) {
        QTRY_COMPARE_WITH_TIMEOUT(links.size(), 1, 5000);
        QVERIFY(links.first().at(0).toJsonObject().value(QStringLiteral("href")).toString().startsWith(QStringLiteral("thoughts-link:")));
        QVERIFY(part.htmlViewState().value(QStringLiteral("ready")).toBool());
    }

    // Review: the shell gives the card, then asks for the mask.
    const QJsonObject card = sampleCard(0);
    const QString cardId = card.value(QStringLiteral("card_id")).toString();
    const QVariantList regions{QVariantMap{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QVariantList{0.1, 0.1, 0.9, 0.2}}}};
    part.setReviewFeedbackCard(card, regions);
    QVERIFY(part.showReviewMasks(regions, true));
    QTRY_VERIFY_WITH_TIMEOUT(part.reviewMaskBlockCount() > 0, 10000);
    QCOMPARE(part.htmlViewState().value(QStringLiteral("review_card")).toString(), cardId);
    // Masked text cannot be selected or copied.
    const QStringList answerKeys = part.m_htmlReader->cardAnchors(card);
    QVERIFY(!answerKeys.isEmpty());
    shot(part, QStringLiteral("02-masked.png"));
    part.setReviewMaskInverted(true);
    QTest::qWait(300);
    shot(part, QStringLiteral("02b-flipped.png"));
    part.setReviewMaskInverted(false);
    QVERIFY(part.focusReviewMaskBlock(0));

    // S in the page reaches the review panel's Skip shortcut (the viewer's forwardKeys), the
    // page's own text fields keep it. Synthetic keydowns: the test never types into the system.
    {
        auto *panel = new QWidget(widget, Qt::Tool | Qt::FramelessWindowHint);
        auto *skip = new QShortcut(QKeySequence(Qt::Key_S), panel);
        skip->setContext(Qt::WindowShortcut);
        QSignalSpy skipped(skip, &QShortcut::activated);
        panel->resize(10, 10);
        panel->show();
        const auto keydown = [&](const QString &target) {
            bool done = false;
            part.m_htmlView->runScript(QStringLiteral("(function(){%1var e=new KeyboardEvent('keydown',{key:'s',code:'KeyS',bubbles:true,cancelable:true});"
                                                      "t.dispatchEvent(e);return e.defaultPrevented;})()")
                                           .arg(target),
                                       [&](const QVariant &) { done = true; });
            QTRY_VERIFY_WITH_TIMEOUT(done, 5000);
        };
        keydown(QStringLiteral("var t=document.body;"));
        QTRY_COMPARE_WITH_TIMEOUT(skipped.size(), 1, 5000);
        keydown(QStringLiteral("var t=document.createElement('input');document.body.appendChild(t);t.focus();"));
        QTest::qWait(500);
        QCOMPARE(skipped.size(), 1);
        part.m_htmlView->runScript(QStringLiteral("(function(){var i=document.querySelector('body > input');if(i)i.remove();return true;})()"), [](const QVariant &) {});
        delete panel;
    }

    part.revealReviewMask();
    QTRY_VERIFY_WITH_TIMEOUT(part.htmlViewState().value(QStringLiteral("review_revealed")).toBool(), 5000);
    QTest::qWait(300);
    shot(part, QStringLiteral("03-revealed.png"));

    // A selection inside the revealed answer: Like / Dislike are offered and save view "html".
    QList<QJsonObject> written;
    part.m_pageView->reviewFeedback()->setWriter([&written](const QJsonObject &record, QString *) {
        written.append(record);
        return true;
    });
    // Select the first words of the answer through the viewer (as a mouse drag would).
    QString answerText;
    bool scripted = false;
    part.m_htmlView->runScript(QStringLiteral("(function(){var k=%1;var s=document.querySelector('span.tm[data-tm=start][data-key=\\''+k+'\\']'),"
                                              "e=document.querySelector('span.tm[data-tm=end][data-key=\\''+k+'\\']');if(!s||!e)return '';var r=document.createRange();"
                                              "r.setStartAfter(s);r.setEndBefore(e);return r.toString();})()")
                                   .arg(QString::fromUtf8(QJsonDocument(QJsonArray{answerKeys.first()}).toJson(QJsonDocument::Compact)).mid(1).chopped(1)),
                               [&](const QVariant &result) {
                                   answerText = result.toString();
                                   scripted = true;
                               });
    QTRY_VERIFY_WITH_TIMEOUT(scripted, 5000);
    const QString words = answerText.simplified().section(QLatin1Char(' '), 1, 2);
    QVERIFY2(!words.isEmpty(), qPrintable(answerText));
    // With the answer's words around it, so a phrase that also occurs elsewhere is found here.
    const QString answerSimple = answerText.simplified();
    const QString before = answerSimple.section(QLatin1Char(' '), 0, 0) + QLatin1Char(' ');
    const QString after = QLatin1Char(' ') + answerSimple.section(QLatin1Char(' '), 3, 8);
    callViewer(part, QStringLiteral("selectQuote"),
               QJsonArray{QJsonObject{{QStringLiteral("exact"), words}, {QStringLiteral("prefix"), before}, {QStringLiteral("suffix"), after}}});
    const QJsonObject selection = callViewer(part, QStringLiteral("getSelection")).toObject();
    QVERIFY2(!selection.value(QStringLiteral("text")).toString().trimmed().isEmpty(), "selection text");
    if (selection.value(QStringLiteral("answer")).toObject().value(QStringLiteral("card_id")).toString() != cardId)
        qWarning().noquote() << "viewer state:" << QJsonDocument(callViewer(part, QStringLiteral("info")).toObject()).toJson(QJsonDocument::Compact);
    QVERIFY2(selection.value(QStringLiteral("answer")).toObject().value(QStringLiteral("card_id")).toString() == cardId, QJsonDocument(selection).toJson().constData());
    // The menu the viewer's contextmenu message opens.
    QStringList menuActions;
    QTimer::singleShot(600, this, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (QAction *action : menu->actions()) if (!action->objectName().isEmpty()) menuActions << action->objectName();
        shot(part, QStringLiteral("04-selection-menu.png"), menu);
        for (QAction *action : menu->actions()) {
            if (action->objectName() == QLatin1String("htmlFeedbackLike")) {
                menu->close();
                action->trigger();
                return;
            }
        }
        menu->close();
    });
    Q_EMIT part.m_htmlReader->contextMenuRequested(QJsonObject{{QStringLiteral("type"), QStringLiteral("contextmenu")},
                                                              {QStringLiteral("point"), QJsonArray{selection.value(QStringLiteral("bounds")).toArray().at(0).toInt() + 40,
                                                                                                    selection.value(QStringLiteral("bounds")).toArray().at(1).toInt() + 24}},
                                                              {QStringLiteral("selection"), selection},
                                                              {QStringLiteral("items"), QJsonArray()}});
    QTRY_VERIFY_WITH_TIMEOUT(!menuActions.isEmpty(), 5000);
    QVERIFY2(menuActions.contains(QStringLiteral("htmlFeedbackLike")), qPrintable(menuActions.join(QLatin1Char(','))));
    QVERIFY(menuActions.contains(QStringLiteral("htmlFeedbackDislike")));
    QVERIFY(menuActions.contains(QStringLiteral("htmlCopy")));
    QVERIFY(menuActions.contains(QStringLiteral("htmlReadAloud")));
    QVERIFY(!menuActions.contains(QStringLiteral("AddReviewCard"))); // not during a review
    QTRY_COMPARE_WITH_TIMEOUT(written.size(), 1, 5000);
    const QJsonObject feedback = written.first();
    QCOMPARE(feedback.value(QStringLiteral("view")).toString(), QStringLiteral("html"));
    QCOMPARE(feedback.value(QStringLiteral("html_edition_sha256")).toString(), m_editionSha);
    QCOMPARE(feedback.value(QStringLiteral("polarity")).toString(), Thoughts::ReviewCardFeedback::positive());
    QVERIFY(feedback.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("pdf")).toObject().value(QStringLiteral("regions")).toArray().isEmpty());
    QVERIFY(!feedback.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString().isEmpty());
    QCOMPARE(Thoughts::ReviewCardFeedback::validationError(feedback), QString());
    QTest::qWait(300);
    shot(part, QStringLiteral("05-feedback-liked.png"));

    // The review follows the view: switching asks the overview to present the card again.
    QSignalSpy lost(&part, &Part::reviewPresentationLost);
    part.setDocumentView(QStringLiteral("pdf"));
    QCOMPARE(lost.size(), 1);
    QCOMPARE(lost.first().at(0).toString(), cardId);
    part.setReviewFeedbackCard(card, regions);
    QVERIFY(part.showReviewMasks(regions, false));
    QVERIFY(!part.m_htmlReviewShown);
    QVERIFY(part.reviewMaskBlockCount() > 0); // the PDF's mask
    QCOMPARE(part.htmlViewState().value(QStringLiteral("review_card")).toString(), QString());
    part.setDocumentView(QStringLiteral("html"));
    QCOMPARE(lost.size(), 2);
    QCOMPARE(part.reviewMaskBlockCount(), 0); // the PDF's mask went with the switch
    part.setReviewFeedbackCard(card, regions);
    QVERIFY(part.showReviewMasks(regions, false));
    QTRY_COMPARE_WITH_TIMEOUT(part.htmlViewState().value(QStringLiteral("review_card")).toString(), cardId, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(part.reviewMaskBlockCount() > 0, 10000);

    // After the review: a selection makes a card (page-less, placed at the next Sync).
    part.clearReviewMask();
    QCOMPARE(part.reviewMaskBlockCount(), 0);
    callViewer(part, QStringLiteral("clearSelection"));
    // Three words of a line outside the answer.
    QString bodyText;
    scripted = false;
    part.m_htmlView->runScript(QStringLiteral("document.body.innerText"), [&](const QVariant &result) {
        bodyText = result.toString();
        scripted = true;
    });
    QTRY_VERIFY_WITH_TIMEOUT(scripted, 5000);
    QString plainWords;
    for (const QString &line : bodyText.split(QLatin1Char('\n'))) {
        const QString simple = line.simplified();
        if (simple.count(QLatin1Char(' ')) >= 3 && !answerText.simplified().contains(simple.section(QLatin1Char(' '), 0, 2))) {
            plainWords = simple.section(QLatin1Char(' '), 0, 2);
            break;
        }
    }
    QVERIFY2(!plainWords.isEmpty(), qPrintable(bodyText));
    callViewer(part, QStringLiteral("selectQuote"), QJsonArray{QJsonObject{{QStringLiteral("exact"), plainWords}}});
    const QJsonObject plain = callViewer(part, QStringLiteral("getSelection")).toObject();
    QVERIFY(!plain.value(QStringLiteral("text")).toString().trimmed().isEmpty());
    QSignalSpy cardRequests(&part, &Part::requestHtmlReviewCard);
    menuActions.clear();
    QTimer::singleShot(600, this, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (QAction *action : menu->actions()) if (!action->objectName().isEmpty()) menuActions << action->objectName();
        shot(part, QStringLiteral("06-selection-menu-new-card.png"), menu);
        for (QAction *action : menu->actions()) {
            if (action->objectName() == QLatin1String("AddReviewCard")) {
                menu->close();
                action->trigger();
                return;
            }
        }
        menu->close();
    });
    Q_EMIT part.m_htmlReader->contextMenuRequested(QJsonObject{{QStringLiteral("type"), QStringLiteral("contextmenu")},
                                                              {QStringLiteral("point"), QJsonArray{plain.value(QStringLiteral("bounds")).toArray().at(0).toInt() + 40,
                                                                                                    plain.value(QStringLiteral("bounds")).toArray().at(1).toInt() + 24}},
                                                              {QStringLiteral("selection"), plain},
                                                              {QStringLiteral("items"), QJsonArray()}});
    QTRY_COMPARE_WITH_TIMEOUT(cardRequests.size(), 1, 5000);
    QVERIFY(menuActions.contains(QStringLiteral("htmlComment")));
    const QVariantMap fields = cardRequests.first().at(1).toMap();
    QCOMPARE(fields.value(QStringLiteral("created_in")).toString(), QStringLiteral("html"));
    QCOMPARE(fields.value(QStringLiteral("html_edition_sha256")).toString(), m_editionSha);
    QCOMPARE(fields.value(QStringLiteral("pdf_sha256")).toString(), sha(readFile(path)));
    QVERIFY(!fields.value(QStringLiteral("text_quote")).toMap().value(QStringLiteral("exact")).toString().isEmpty());
    if (m_manifest.value(QStringLiteral("source_map")).toString() != QLatin1String("exact")) {
        QVERIFY(!fields.contains(QStringLiteral("source_range"))); // LaTeX: placed by the quote
    }

    // A card made in the HTML view and not linked yet is placed by its quote.
    QJsonObject pendingCard{{QStringLiteral("card_id"), QStringLiteral("aaaaaaaa-1111-4222-8333-bbbbbbbbbbbb")},
                            {QStringLiteral("status"), QStringLiteral("approved")},
                            {QStringLiteral("primary"), QJsonObject{{QStringLiteral("question"), QStringLiteral("Pending?")},
                                                                    {QStringLiteral("created_in"), QStringLiteral("html")},
                                                                    {QStringLiteral("html_edition_sha256"), m_editionSha},
                                                                    {QStringLiteral("text_quote"), fields.value(QStringLiteral("text_quote")).toJsonObject()}}},
                            {QStringLiteral("source_link"), QJsonObject{{QStringLiteral("state"), QStringLiteral("pending_server_link")}}}};
    cards.append(pendingCard);
    QSignalSpy placed(part.m_htmlReader, &Thoughts::HtmlReader::itemsPlaced);
    part.setSidebarReviewCards(cards);
    QTRY_VERIFY_WITH_TIMEOUT(!placed.isEmpty(), 5000);
    const QJsonObject report = placed.last().at(0).toJsonObject();
    QVERIFY2(report.value(QStringLiteral("cards")).toObject().value(QStringLiteral("unplaced")).toArray().isEmpty(), QJsonDocument(report).toJson().constData());
    QTest::qWait(300);
    shot(part, QStringLiteral("07-pending-card-placed.png"));

    // Comment… on a selection: a logical comment the server places at the next Sync.
    callViewer(part, QStringLiteral("selectQuote"), QJsonArray{QJsonObject{{QStringLiteral("exact"), plainWords}}});
    const QJsonObject commentSelection = callViewer(part, QStringLiteral("getSelection")).toObject();
    QTimer::singleShot(600, this, [&] {
        if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
            dialog->setTextValue(QStringLiteral("A comment from the HTML view"));
            dialog->accept();
        }
    });
    part.createHtmlComment(commentSelection);
    QJsonObject comment;
    const QJsonObject projections = part.m_fedoraRepository->effectiveProjections();
    for (auto it = projections.begin(); it != projections.end(); ++it) {
        if (it.key().startsWith(QLatin1String("logical_comment\ncomments\n"))
            && it.value().toObject().value(QStringLiteral("contents")).toString() == QLatin1String("A comment from the HTML view"))
            comment = it.value().toObject();
    }
    QVERIFY2(!comment.isEmpty(), "the HTML comment is queued");
    const QJsonObject origin = comment.value(QStringLiteral("origin")).toObject();
    QCOMPARE(origin.value(QStringLiteral("created_in")).toString(), QStringLiteral("html"));
    QCOMPARE(origin.value(QStringLiteral("html_edition_sha256")).toString(), m_editionSha);
    QCOMPARE(origin.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")).toString().simplified(), plainWords);
    QVERIFY(!origin.contains(QStringLiteral("page_index")));
    QCOMPARE(comment.value(QStringLiteral("scope")).toObject().value(QStringLiteral("reason")).toString(), QStringLiteral("html_pending_server_link"));
    QTRY_VERIFY_WITH_TIMEOUT(callViewer(part, QStringLiteral("info")).toObject().value(QStringLiteral("items")).toInt() >= cardCount + 2, 5000);
    QTest::qWait(300);
    shot(part, QStringLiteral("07b-html-comment.png"));

    // Copy reads the page's selection (⌘C is Qt's); checked on the viewer call, not the system
    // clipboard (the test must not change the user's clipboard).
    callViewer(part, QStringLiteral("selectQuote"), QJsonArray{QJsonObject{{QStringLiteral("exact"), plainWords}}});
    QCOMPARE(callViewer(part, QStringLiteral("copySelection")).toString().simplified(), plainWords);

    // Text size and dark mode.
    part.m_htmlReader->setTextSize(130);
    part.m_htmlView->setTheme(QStringLiteral("dark"));
    QTest::qWait(500);
    shot(part, QStringLiteral("08-dark-130.png"));
    part.m_htmlView->setTheme(QStringLiteral("system"));
    part.m_htmlReader->setTextSize(100);

    // Back to the PDF.
    part.setDocumentView(QStringLiteral("pdf"));
    QCOMPARE(part.m_documentStack->currentWidget(), part.m_pageView.data());
}

// A site that sends a new reader through another address and back (Springer: idp.springer.com
// sets a cookie, then the book), or a DOI that redirects to its publisher: the live view follows
// a server's redirects of a load it allowed until the page shows, and then stays where it arrived.
// Before, the hop elsewhere was refused as a navigation of the page's own, and the page never showed.
void HtmlViewTest::liveViewFollowsAServersRedirectElsewhere()
{
    if (!Thoughts::HtmlLiveWebView::available() || qEnvironmentVariable("THOUGHTS_HTML_GUI_TEST") != QLatin1String("1")) {
        QSKIP("needs a real web view: THOUGHTS_HTML_GUI_TEST=1 on the cocoa or windows platform");
    }
    // Two origins (two ports): /book on the first sends to /authorize on the second, which sends
    // back; /doi on the first ends on the second's /article.
    QTcpServer first, second;
    QVERIFY(first.listen(QHostAddress::LocalHost) && second.listen(QHostAddress::LocalHost));
    const QString firstOrigin = QStringLiteral("http://127.0.0.1:%1").arg(first.serverPort());
    const QString secondOrigin = QStringLiteral("http://127.0.0.1:%1").arg(second.serverPort());
    const auto serve = [](QTcpServer *server, std::function<QByteArray(const QByteArray &path)> answer) {
        QObject::connect(server, &QTcpServer::newConnection, server, [server, answer] {
            while (QTcpSocket *socket = server->nextPendingConnection()) {
                QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, answer] {
                    const QByteArray request = socket->readAll();
                    const QByteArray path = request.split(' ').value(1);
                    socket->write(answer(path));
                    socket->disconnectFromHost();
                });
            }
        });
    };
    const auto redirect = [](const QString &to) -> QByteArray { return QByteArray("HTTP/1.1 302 Found\r\nLocation: ") + to.toUtf8() + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"; };
    const auto page = [](const QByteArray &title) -> QByteArray {
        const QByteArray body = "<!doctype html><title>" + title + "</title><p>" + title + "</p>";
        return QByteArray("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: ") + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    };
    serve(&first, [&](const QByteArray &path) -> QByteArray {
        if (path.startsWith("/book?cookie=set")) return page("The book");
        if (path == "/book") return redirect(secondOrigin + QStringLiteral("/authorize"));
        if (path == "/doi") return redirect(secondOrigin + QStringLiteral("/article"));
        return page("Elsewhere on the first");
    });
    serve(&second, [&](const QByteArray &path) -> QByteArray {
        if (path == "/authorize") return redirect(firstOrigin + QStringLiteral("/book?cookie=set"));
        return page("The article");
    });

    Thoughts::HtmlLiveWebView view;
    QSignalSpy committed(&view, &Thoughts::HtmlLiveWebView::loadCommitted);
    QSignalSpy finished(&view, &Thoughts::HtmlLiveWebView::loadFinished);
    QSignalSpy elsewhere(&view, &Thoughts::HtmlLiveWebView::linkRequested);
    QSignalSpy failed(&view, &Thoughts::HtmlLiveWebView::loadFailed);
    view.load(QUrl(firstOrigin + QStringLiteral("/book")));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() >= 1, 15000);
    QCOMPARE(finished.last().at(0).toUrl(), QUrl(firstOrigin + QStringLiteral("/book?cookie=set")));
    QVERIFY(committed.size() >= 1);
    QCOMPARE(elsewhere.size(), 0);
    QCOMPARE(failed.size(), 0);
    // The page itself still cannot leave for another site on its own.
    QVERIFY(!view.nativeAllows(QUrl(secondOrigin + QStringLiteral("/other")), true));

    // A DOI-like redirect that ends elsewhere: the page shows there, and its own site is now the second.
    finished.clear();
    view.load(QUrl(firstOrigin + QStringLiteral("/doi")));
    QTRY_VERIFY_WITH_TIMEOUT(finished.size() >= 1, 15000);
    QCOMPARE(finished.last().at(0).toUrl(), QUrl(secondOrigin + QStringLiteral("/article")));
    QCOMPARE(elsewhere.size(), 0);
    QVERIFY(view.nativeAllows(QUrl(secondOrigin + QStringLiteral("/other")), true));
}

void HtmlViewTest::livePageAsksThenLoadsWithoutTheBridge()
{
    // A live page (19 November: pages are embedded or live): its stub names an
    // address, which the reader loads instead, after asking once for its origin, in a view with
    // none of the page bridge.
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("drives the live page's load by hand: offscreen only (the WKWebView itself runs in the app)");
    }
    const QString path = addLibraryPdf(QStringLiteral("runs"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path, LiveStub);
    QVERIFY(target.isValid());
    QCOMPARE(Thoughts::LivePage::stubAddressOfFile(target.file), LiveAddress);
    Thoughts::LivePage::forgetOrigins();
    Part::setHtmlAttachmentsWithoutWebViewForTesting(true);
    OpenedLinks opened;
    for (const char *scheme : {"https", "http", "mailto"}) QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), &opened, "open");
    const auto restore = qScopeGuard([this] {
        Part::setHtmlAttachmentsWithoutWebViewForTesting(false);
        Thoughts::LivePage::forgetOrigins();
        for (const char *scheme : {"https", "http", "mailto"}) QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
        QDir(m_docs + QStringLiteral("/docs-ideas")).removeRecursively();
    });
    Part part(nullptr, {});
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    const auto button = [&part](const char *name) { return part.m_attachmentView->findChild<QToolButton *>(QString::fromLatin1(name)); };

    // First the question, in the page's place: nothing is loaded before Open.
    QString error;
    QVERIFY2(part.showHtmlAttachment(target, false, &error), qPrintable(error));
    QVariantMap state = part.htmlAttachmentState();
    QVERIFY(state.value(QStringLiteral("shown")).toBool());
    QVERIFY(state.value(QStringLiteral("live")).toBool());
    QCOMPARE(state.value(QStringLiteral("live_url")).toString(), LiveAddress.toString());
    QCOMPARE(state.value(QStringLiteral("live_state")).toString(), QStringLiteral("consent"));
    QCOMPARE(state.value(QStringLiteral("title")).toString(), QStringLiteral("Training curves"));
    QCOMPARE(state.value(QStringLiteral("message")).toString(), QStringLiteral("This page loads http://localhost:18770 from the network. Open it?"));
    QCOMPARE(state.value(QStringLiteral("message_action")).toString(), QStringLiteral("Open"));
    QCOMPARE(part.m_attachmentView->messageCancelText(), QStringLiteral("Cancel"));
    QVERIFY(!part.m_attachmentView->isLive());
    // Its bar, and what needs an embedded page.
    QCOMPARE(state.value(QStringLiteral("live_bar")).toString(), QStringLiteral("Live · http://localhost:18770/"));
    QVERIFY(button("thoughtsHtmlLiveReload") && !button("thoughtsHtmlLiveReload")->isHidden());
    QVERIFY(!state.value(QStringLiteral("snapshot_enabled")).toBool());
    QVERIFY(!state.value(QStringLiteral("new_card_enabled")).toBool());
    QCOMPARE(state.value(QStringLiteral("new_card_tip")).toString(), QStringLiteral("Works on embedded pages"));
    QVERIFY(part.m_attachmentReader->pageFeedback().isEmpty());
    // The PDF's tools are greyed out as on any page; the pencil draws on a snapshot of it.
    QCOMPARE(part.m_pageView->shownInstead(), PageView::ShownInstead::HtmlPage);
    QAction *pencil = part.actionCollection()->action(QStringLiteral("mouse_draw"));
    QAction *textSelect = part.actionCollection()->action(QStringLiteral("mouse_textselect"));
    QVERIFY(pencil && textSelect);
    QVERIFY(pencil->isEnabled());
    QCOMPARE(pencil->toolTip(), QStringLiteral("Draw on This Page"));
    QVERIFY(!textSelect->isEnabled());
    // A card made in the page is reviewed with its stored answer: no viewer masks a live page.
    const QJsonObject card = attachmentCard(Thoughts::HtmlAttachmentReader::newCardFields({{QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("0.12")}}}}, target),
                                            QStringLiteral("Loss?"));
    QVERIFY(part.m_attachmentReader->presentReview(card, false));
    QCOMPARE(part.m_attachmentReader->reviewState(), QStringLiteral("not_found"));
    part.m_attachmentReader->clearReview();

    // Open: remembered for the origin, then loaded (offscreen there is no web view, so it is not
    // reached), and said plainly with Retry.
    button("thoughtsHtmlViewMessageAction")->click();
    QVERIFY(Thoughts::LivePage::originAllowed(QStringLiteral("http://localhost:18770")));
    QVERIFY(part.m_attachmentView->isLive());
    QCOMPARE(part.m_attachmentView->liveUrl(), LiveAddress);
    QCOMPARE(part.m_attachmentView->transportKind(), QStringLiteral("live"));
    state = part.htmlAttachmentState();
    QCOMPARE(state.value(QStringLiteral("live_state")).toString(), QStringLiteral("unreachable"));
    // Why, as the web view said it (here: there is none).
    QCOMPARE(state.value(QStringLiteral("message")).toString(),
             QStringLiteral("This live page is at http://localhost:18770/, which this device cannot reach.\nthere is no web view for live pages here"));
    QCOMPARE(state.value(QStringLiteral("message_action")).toString(), QStringLiteral("Retry"));
    QVERIFY(!state.value(QStringLiteral("snapshot_enabled")).toBool());
    QCOMPARE(state.value(QStringLiteral("snapshot_tip")).toString(), QStringLiteral("Works once the page is shown."));

    // Loaded, as WebKit reports it: Take Snapshot where the page's folder is on this computer.
    Q_EMIT part.m_attachmentView->liveLoaded(LiveAddress);
    state = part.htmlAttachmentState();
    QCOMPARE(state.value(QStringLiteral("live_state")).toString(), QStringLiteral("loaded"));
    QVERIFY(!state.value(QStringLiteral("snapshot_enabled")).toBool());
    QCOMPARE(state.value(QStringLiteral("snapshot_tip")).toString(), QStringLiteral("This page's folder is not on this computer."));
    QVERIFY(QDir().mkpath(m_docs + QStringLiteral("/docs-ideas")));
    part.updateLivePageBar();
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("snapshot_enabled")).toBool());
    QVERIFY(button("thoughtsHtmlLiveTakeSnapshot")->isEnabled());
    // Still no cards or feedback, and no viewer is ever asked for anything.
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("new_card_enabled")).toBool());
    QVERIFY(!part.m_attachmentReader->isReady());
    QVERIFY(!part.m_attachmentView->isAttachment());

    // Reload: the address again (unreached here again).
    button("thoughtsHtmlLiveReload")->click();
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("unreachable"));
    // Retry likewise.
    button("thoughtsHtmlViewMessageAction")->click();
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("unreachable"));

    // Open in Browser: the page's address, not its stub.
    button("thoughtsHtmlOpenInBrowser")->click();
    QCOMPARE(opened.urls, QList<QUrl>{LiveAddress});
#ifdef Q_OS_MACOS
    // A link to another origin: the browser on the reader's click only.
    Q_EMIT part.m_attachmentView->externalLinkRequested(QUrl(QStringLiteral("https://thoughts.example.org/run/7")), true);
    Q_EMIT part.m_attachmentView->externalLinkRequested(QUrl(QStringLiteral("https://collector.example.org/x?shown=loss")), false);
    QCOMPARE(opened.urls, QList<QUrl>({LiveAddress, QUrl(QStringLiteral("https://thoughts.example.org/run/7"))}));
#endif

    // The same page again: kept as it is.
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("unreachable"));
    // Closed: the live view and its storage are gone.
    part.closeHtmlAttachment();
    QVERIFY(!part.m_attachmentView->isLive());
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("live_bar")).toString().isEmpty());

    // Cancel at the question: the document again, nothing loaded or remembered.
    Thoughts::LivePage::forgetOrigins();
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("consent"));
    button("thoughtsHtmlViewMessageCancel")->click();
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QVERIFY(!part.m_attachmentView->isLive());
    QVERIFY(!Thoughts::LivePage::originAllowed(QStringLiteral("http://localhost:18770")));
    // Asked again next time.
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("consent"));
    part.closeHtmlAttachment();

    // An embedded page shows no live bar.
    const QString other = addLibraryPdf(QStringLiteral("runs-embedded"), QStringLiteral("source_linked"), {});
    const auto embedded = addAttachment(other);
    Part plain(nullptr, {});
    plain.openDocument(other);
    QVERIFY(plain.showHtmlAttachment(embedded, false, &error));
    QVERIFY(!plain.htmlAttachmentState().value(QStringLiteral("live")).toBool());
    QVERIFY(plain.htmlAttachmentState().value(QStringLiteral("live_bar")).toString().isEmpty());
    QVERIFY(plain.htmlAttachmentState().value(QStringLiteral("snapshot_of")).toString().isEmpty());
    QVERIFY(plain.m_attachmentView->isAttachment());
}

void HtmlViewTest::liveSnapshotAndGoLiveAgain()
{
    // Take Snapshot: what the live page shows now becomes the page itself (an embedded page, the
    // same link), and Go Live Again writes its stub back.
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("drives the live page's load by hand: offscreen only (the serializer runs in the app's WKWebView)");
    }
    const QString path = addLibraryPdf(QStringLiteral("runs-snapshot"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path, LiveStub);
    QVERIFY(target.isValid());
    Thoughts::LivePage::forgetWrites();
    Thoughts::LivePage::allowOrigin(Thoughts::LivePage::origin(LiveAddress));
    Part::setHtmlAttachmentsWithoutWebViewForTesting(true);
    const QString library = m_docs + QLatin1Char('/') + AttachmentPath;
    const auto restore = qScopeGuard([this] {
        Part::setHtmlAttachmentsWithoutWebViewForTesting(false);
        Part::setLivePageAnswerForTesting(-1);
        Thoughts::LivePage::forgetOrigins();
        Thoughts::LivePage::forgetWrites();
        QDir(m_docs + QStringLiteral("/docs-ideas")).removeRecursively();
    });
    Part part(nullptr, {});
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    QSignalSpy syncs(&part, &Part::globalSyncRequested);
    QString error;
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    // The origin was agreed to before: no question.
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("unreachable"));
    Q_EMIT part.m_attachmentView->liveLoaded(LiveAddress);
    // As the page's serializer answers (part/html-live/snapshot.js).
    const QString serialized = QString::fromUtf8(
        QJsonDocument(QJsonObject{{QStringLiteral("html"), QStringLiteral("<html><head><title>TensorBoard</title><style>p{color:#333}</style></head><body><p>loss 0.12</p>"
                                                                          "<img src=\"data:image/png;base64,iVBORw0KGgo=\" style=\"width:300px;height:150px\"></body></html>")},
                                  {QStringLiteral("title"), QStringLiteral("TensorBoard")},
                                  {QStringLiteral("canvases"), 1}})
            .toJson());

    // The page's folder is not here: nothing to save it in.
    Part::setLivePageAnswerForTesting(1);
    QVERIFY(!part.saveLiveSnapshot(serialized, &error));
    QVERIFY2(error.contains(QStringLiteral("docs-ideas")), qPrintable(error));
    QVERIFY(QDir().mkpath(m_docs + QStringLiteral("/docs-ideas")));
    // Cancel at "Replace the live page with this snapshot?": the live page stays.
    Part::setLivePageAnswerForTesting(0);
    QVERIFY(!part.saveLiveSnapshot(serialized, &error));
    QVERIFY(error.isEmpty());
    QVERIFY(!QFileInfo::exists(library));
    QCOMPARE(syncs.count(), 0);
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("live")).toBool());
    // Replace: written as the page's file, sent by Sync, shown embedded at once.
    Part::setLivePageAnswerForTesting(1);
    QVERIFY2(part.saveLiveSnapshot(serialized, &error), qPrintable(error));
    const QByteArray snapshot = readFile(library);
    QCOMPARE(Thoughts::LivePage::snapshotSource(snapshot), LiveAddress);
    QVERIFY(!Thoughts::LivePage::stubAddress(snapshot).isValid());
    QVERIFY(QString::fromUtf8(snapshot).contains(QStringLiteral(">Snapshot of <a href=\"http://localhost:18770/\"")));
    QVERIFY(snapshot.contains("<p>loss 0.12</p>"));
    QCOMPARE(Thoughts::LivePage::title(snapshot), QStringLiteral("Training curves"));
    QCOMPARE(syncs.count(), 1);
    QVariantMap state = part.htmlAttachmentState();
    QVERIFY(state.value(QStringLiteral("shown")).toBool());
    QVERIFY(!state.value(QStringLiteral("live")).toBool());
    QCOMPARE(state.value(QStringLiteral("path")).toString(), AttachmentPath);
    QCOMPARE(state.value(QStringLiteral("sha256")).toString(), sha(snapshot));
    QCOMPARE(state.value(QStringLiteral("snapshot_of")).toString(), LiveAddress.toString());
    QCOMPARE(state.value(QStringLiteral("title")).toString(), QStringLiteral("Training curves"));
    QVERIFY(state.value(QStringLiteral("live_bar")).toString().isEmpty());
    QVERIFY(state.value(QStringLiteral("url")).toString().startsWith(QStringLiteral("http://127.0.0.1:"))); // sealed off again
    QVERIFY(part.m_attachmentView->isAttachment());
    QVERIFY(!part.m_attachmentView->isLive());
    QVERIFY(state.value(QStringLiteral("new_card_tip")).toString() != QStringLiteral("Works on embedded pages"));
    // The PDF's bundle still has the stub until it rebuilds; this computer shows what it wrote.
    QCOMPARE(Thoughts::resolveHtmlAttachment(AttachmentPath, path, &error).sha256, sha(LiveStub));
    QCOMPARE(part.resolveHtmlPage(AttachmentPath, &error).sha256, sha(snapshot));
    QCOMPARE(part.attachmentVersion(AttachmentPath), sha(snapshot));
    // Go Live Again is for snapshots only; Take Snapshot only for live pages.
    part.takeLiveSnapshot();
    QCOMPARE(syncs.count(), 1);

    // Go Live Again: the stub with the snapshot's address and the page's name, live again.
    QVERIFY(part.goLiveAgain());
    const QByteArray stub = readFile(library);
    QCOMPARE(Thoughts::LivePage::stubAddress(stub), LiveAddress);
    QCOMPARE(Thoughts::LivePage::title(stub), QStringLiteral("Training curves"));
    QCOMPARE(syncs.count(), 2);
    state = part.htmlAttachmentState();
    QVERIFY(state.value(QStringLiteral("live")).toBool());
    QCOMPARE(state.value(QStringLiteral("live_url")).toString(), LiveAddress.toString());
    QCOMPARE(state.value(QStringLiteral("live_state")).toString(), QStringLiteral("unreachable"));
    QVERIFY(state.value(QStringLiteral("snapshot_of")).toString().isEmpty());
    QVERIFY(!part.goLiveAgain());
}

void HtmlViewTest::livePageTextFeedback()
{
    // Text feedback on a live page (23 November: right-clicking a selection of a live page showed
    // only the web view's menu; html-live-pages.md §2.1): the reader reads the selection with its
    // own script in the page's isolated script world and shows its own menu; the record is
    // feedback in the page (document-feedback.md §8) on the live stub, with one picture of what
    // was shown (§12) where the server keeps pictures. The page still gets no bridge.
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("drives the live page's selection by hand: offscreen only (the WKWebView's menu and script world run in the app)");
    }
    const QString path = addLibraryPdf(QStringLiteral("runs-feedback"), QStringLiteral("source_linked"), {});
    const auto target = addAttachment(path, LiveStub);
    QVERIFY(target.isValid());
    QCOMPARE(target.sha256, sha(LiveStub));
    Thoughts::LivePage::forgetWrites();
    Thoughts::LivePage::allowOrigin(Thoughts::LivePage::origin(LiveAddress));
    Part::setHtmlAttachmentsWithoutWebViewForTesting(true);
    const QString capabilities = QStringLiteral("thoughts-server/capabilities");
    const auto restore = qScopeGuard([capabilities] {
        Part::setHtmlAttachmentsWithoutWebViewForTesting(false);
        Part::setLivePagePictureForTesting(QImage());
        Thoughts::LivePage::forgetOrigins();
        QSettings().remove(capabilities);
    });
    Part part(nullptr, {});
    part.openDocument(path);
    QVERIFY(part.m_document->isOpened());
    const QSignalBlocker quietRepository(part.m_fedoraRepository);
    auto *overlay = part.m_pageView->documentFeedback();
    overlay->clear();
    QList<QJsonObject> written;
    overlay->setWriter([&written](const QJsonObject &record, QString *) {
        written.append(record);
        return true;
    });
    QSettings().remove(capabilities);
    QString error;
    QVERIFY(part.showHtmlAttachment(target, false, &error));
    Q_EMIT part.m_attachmentView->liveLoaded(LiveAddress);
    // A load refreshes the document's feedback: start again from none for the checks below.
    overlay->clear();
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("loaded"));
    // Still no viewer and no cards (the selection's menu is the reader's own).
    QVERIFY(!part.m_attachmentView->isAttachment());
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("new_card_tip")).toString(), QStringLiteral("Works on embedded pages"));

    // The isolated script's answer (part/html-live/selection.js), as a viewer selection.
    QCOMPARE(Thoughts::HtmlAttachmentReader::liveSelectionArguments(),
             QJsonObject({{QStringLiteral("context"), Thoughts::ReviewCardFeedback::contextLength}, {QStringLiteral("longest"), Thoughts::ReviewCardFeedback::maximumQuote}}));
    const QString quoted = QStringLiteral("loss after epoch 7 is 0.12");
    const auto answer = [](const QJsonObject &fields) { return QString::fromUtf8(QJsonDocument(fields).toJson(QJsonDocument::Compact)); };
    const QJsonObject selection = Thoughts::HtmlAttachmentReader::liveSelection(
        answer({{QStringLiteral("exact"), QString(QStringLiteral("  ") + quoted + QStringLiteral(" "))},
                {QStringLiteral("prefix"), QString(QString(40, QLatin1Char('p')) + QStringLiteral("Training "))},
                {QStringLiteral("suffix"), QString(QStringLiteral(" and falling") + QString(40, QLatin1Char('s')))},
                {QStringLiteral("hash"), QStringLiteral("#curves")}}));
    QCOMPARE(selection.value(QStringLiteral("text_quote")).toObject().value(QStringLiteral("exact")).toString(), quoted);
    QCOMPARE(selection.value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(), QStringLiteral("#curves"));
    // Nothing (readable) selected, no answer, a page state that is no location.hash.
    QVERIFY(Thoughts::HtmlAttachmentReader::liveSelection(answer({{QStringLiteral("exact"), QStringLiteral("  ")}, {QStringLiteral("hash"), QStringLiteral("#x")}})).isEmpty());
    QVERIFY(Thoughts::HtmlAttachmentReader::liveSelection(QStringLiteral("the page was closed")).isEmpty());
    QCOMPARE(Thoughts::HtmlAttachmentReader::liveSelection(answer({{QStringLiteral("exact"), QStringLiteral("loss")}, {QStringLiteral("hash"), QStringLiteral("curves")}}))
                 .value(QStringLiteral("page_state")).toObject().value(QStringLiteral("hash")).toString(),
             QString());
    // Offscreen there is no web view to read it from: nothing, and the page is not asked.
    QJsonObject read{{QStringLiteral("unset"), true}};
    part.m_attachmentReader->readLiveSelection([&read](const QJsonObject &result) { read = result; });
    QVERIFY(read.isEmpty());

    const auto actions = [&part](const QJsonObject &chosen) {
        QMenu menu;
        part.addLiveSelectionMenuActions(&menu, chosen);
        QStringList names;
        for (const QAction *action : menu.actions()) names.append(action->isSeparator() ? QStringLiteral("|") : action->objectName());
        return names;
    };
    const auto trigger = [&part](const QJsonObject &chosen, const QString &name) {
        QMenu menu;
        part.addLiveSelectionMenuActions(&menu, chosen);
        for (QAction *action : menu.actions()) {
            if (action->objectName() == name) action->trigger();
        }
    };

    // Feedback needs a Library document, as on the PDF: Copy alone (the web view's own).
    QCOMPARE(actions(selection), QStringList{QStringLiteral("attachmentCopy")});
    const QString pdfSha = sha(readFile(path));
    overlay->setDocument(DocumentId, pdfSha);
    overlay->setReviewVersion({{QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("source_built"), false}});
    const QStringList feedbackActions{QStringLiteral("attachmentFeedbackLike"), QStringLiteral("attachmentFeedbackDislike"), QStringLiteral("attachmentFeedbackQuestion"),
                                      QStringLiteral("attachmentFeedbackComment"), QStringLiteral("|"), QStringLiteral("attachmentCopy")};
    QCOMPARE(actions(selection), feedbackActions);
    // The web view's own menu (one menu, 23 Nov 2026) asks whether to offer feedback, then gives the
    // item chosen; the selection is read then. Offscreen nothing can be read: the reader is told.
    QVERIFY(part.liveSelectionFeedbackOffered());
    const qsizetype before = written.size();
    part.chooseLiveSelection(QStringLiteral("like"));
    QCOMPARE(written.size(), before);
    QCOMPARE(part.m_attachmentView->notice(), QStringLiteral("The selected text could not be read. Select it again."));
    part.m_attachmentView->setNotice(QString(), QString());
    // No review cards on a live page, also where the server keeps them.
    part.m_attachmentCardsSupported = true;
    QCOMPARE(actions(selection), feedbackActions);
    // A selection the script cannot read (a form field, a frame): Copy alone.
    QCOMPARE(actions(QJsonObject()), QStringList{QStringLiteral("attachmentCopy")});

    // Like, where the server has no document_feedback_pictures_v1: the record goes without one.
    QImage shown(3000, 1500, QImage::Format_ARGB32_Premultiplied);
    shown.fill(Qt::transparent);
    {
        QPainter painter(&shown);
        painter.fillRect(QRect(1000, 500, 1000, 500), QColor(20, 90, 200));
    }
    Part::setLivePagePictureForTesting(shown);
    trigger(selection, QStringLiteral("attachmentFeedbackLike"));
    QCOMPARE(written.size(), 1);
    const QJsonObject liked = written.constLast();
    QVERIFY2(Thoughts::DocumentFeedback::validationError(liked).isEmpty(), qPrintable(Thoughts::DocumentFeedback::validationError(liked)));
    QCOMPARE(liked.value(QStringLiteral("view")).toString(), QStringLiteral("html_attachment"));
    QCOMPARE(liked.value(QStringLiteral("polarity")).toString(), Thoughts::ReviewCardFeedback::positive());
    QVERIFY(!liked.contains(QStringLiteral("kind")));
    QVERIFY(!liked.contains(QStringLiteral("pictures")));
    const QJsonObject anchor = liked.value(QStringLiteral("anchor")).toObject();
    // The page's path and the page as stored: the live stub.
    QCOMPARE(anchor.value(QStringLiteral("html_attachment")).toObject(), QJsonObject({{QStringLiteral("path"), AttachmentPath}, {QStringLiteral("sha256"), sha(LiveStub)}}));
    QCOMPARE(anchor.value(QStringLiteral("page_state")).toObject(), QJsonObject({{QStringLiteral("hash"), QStringLiteral("#curves")}}));
    const QJsonObject pdf = anchor.value(QStringLiteral("pdf")).toObject();
    QCOMPARE(pdf.value(QStringLiteral("document_id")).toString(), DocumentId);
    QCOMPARE(pdf.value(QStringLiteral("pdf_sha256")).toString(), pdfSha);
    QVERIFY(pdf.value(QStringLiteral("regions")).toArray().isEmpty());
    const QJsonObject quote = anchor.value(QStringLiteral("quote")).toObject();
    QCOMPARE(quote.value(QStringLiteral("exact")).toString(), quoted);
    QCOMPARE(quote.value(QStringLiteral("prefix")).toString().size(), Thoughts::ReviewCardFeedback::contextLength);
    QVERIFY(quote.value(QStringLiteral("prefix")).toString().endsWith(QStringLiteral("pTraining ")));
    QCOMPARE(quote.value(QStringLiteral("suffix")).toString().size(), Thoughts::ReviewCardFeedback::contextLength);
    QVERIFY(quote.value(QStringLiteral("suffix")).toString().startsWith(QStringLiteral(" and falling")));
    // Listed as any page's feedback (on this version of the page), never drawn on the PDF.
    QCOMPARE(part.m_attachmentReader->pageFeedback().size(), 1);
    QVERIFY(overlay->rectsOf(liked.value(QStringLiteral("feedback_id")).toString()).isEmpty());
    QVERIFY(Thoughts::DocumentFeedback::shownOn(liked, pdfSha, part.attachmentVersion(AttachmentPath)));
    // Given on this load of the live page (its address and when it loaded): a Reload makes it from
    // an earlier version, as feedback on an earlier PDF (html-live-pages.md §2.1, checked below).
    const QJsonObject liveLoad = anchor.value(QStringLiteral("live")).toObject();
    QCOMPARE(liveLoad.value(QStringLiteral("address")).toString(), LiveAddress.toString());
    QVERIFY(!liveLoad.value(QStringLiteral("loaded_at")).toString().isEmpty());
    // Its row brings the live page back (the quote is not looked for in it).
    part.hideHtmlAttachment();
    QVERIFY(!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    part.showFeedback(liked.value(QStringLiteral("feedback_id")).toString(), false);
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("shown")).toBool());
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("live")).toBool());
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("loaded"));

    // Dislike with document_feedback_pictures_v1: one picture of what was shown, opaque JPEG at
    // most 2048 pixels, kept on this computer first (Sync uploads it before the record).
    QSettings().setValue(capabilities, QStringList{QStringLiteral("document_feedback_v1"), Thoughts::DocumentFeedback::picturesCapability()});
    trigger(selection, QStringLiteral("attachmentFeedbackDislike"));
    QCOMPARE(written.size(), 2);
    const QJsonObject disliked = written.constLast();
    QVERIFY2(Thoughts::DocumentFeedback::validationError(disliked).isEmpty(), qPrintable(Thoughts::DocumentFeedback::validationError(disliked)));
    QCOMPARE(disliked.value(QStringLiteral("polarity")).toString(), Thoughts::ReviewCardFeedback::negative());
    QCOMPARE(disliked.value(QStringLiteral("anchor")).toObject(), anchor);
    const QJsonArray pictures = Thoughts::DocumentFeedback::picturesOf(disliked);
    QCOMPARE(pictures.size(), 1);
    const QJsonObject picture = pictures.first().toObject();
    QCOMPARE(picture.value(QStringLiteral("media_type")).toString(), QStringLiteral("image/jpeg"));
    QCOMPARE(picture.value(QStringLiteral("width")).toInt(), Thoughts::DocumentFeedback::maximumPictureSide);
    QCOMPARE(picture.value(QStringLiteral("height")).toInt(), Thoughts::DocumentFeedback::maximumPictureSide / 2);
    const QByteArray kept = part.m_fedoraRepository->feedbackSnapshot(picture.value(QStringLiteral("object_sha256")).toString());
    QVERIFY(!kept.isEmpty());
    QImage keptImage;
    QVERIFY(keptImage.loadFromData(kept, "JPEG"));
    QCOMPARE(keptImage.size(), QSize(2048, 1024));
    // What was transparent is the page's white, not black.
    QVERIFY(qGray(keptImage.pixel(50, 50)) > 240);
    QVERIFY(qBlue(keptImage.pixel(1024, 512)) > 150 && qRed(keptImage.pixel(1024, 512)) < 80);

    // No picture at hand (the web view gave none): the feedback is kept without it.
    Part::setLivePagePictureForTesting(QImage());
    trigger(selection, QStringLiteral("attachmentFeedbackLike"));
    QCOMPARE(written.size(), 3);
    QVERIFY(!written.constLast().contains(QStringLiteral("pictures")));
    Part::setLivePagePictureForTesting(shown);

    // Comment…: the picture is in the editor, to keep or remove. It is not something the user
    // wrote: closed with nothing written, nothing is saved.
    trigger(selection, QStringLiteral("attachmentFeedbackComment"));
    QCOMPARE(written.size(), 3);
    QPointer<Thoughts::ReviewFeedbackEditor> editor = part.widget()->findChild<Thoughts::ReviewFeedbackEditor *>(QStringLiteral("reviewFeedbackEditor"));
    QVERIFY(editor);
    QVERIFY(editor->isNew());
    QCOMPARE(editor->pictures(), pictures);
    editor->close();
    QCOMPARE(written.size(), 3);
    delete editor.data();
    trigger(selection, QStringLiteral("attachmentFeedbackComment"));
    editor = part.widget()->findChild<Thoughts::ReviewFeedbackEditor *>(QStringLiteral("reviewFeedbackEditor"));
    QVERIFY(editor);
    QCOMPARE(editor->pictures(), pictures);
    Q_EMIT editor->saveRequested(Thoughts::ReviewCardFeedback::positive(), QStringLiteral(" Why does it fall? "));
    QCOMPARE(written.size(), 4);
    const QJsonObject commented = written.constLast();
    QVERIFY2(Thoughts::DocumentFeedback::validationError(commented).isEmpty(), qPrintable(Thoughts::DocumentFeedback::validationError(commented)));
    QCOMPARE(commented.value(QStringLiteral("comment")).toString(), QStringLiteral("Why does it fall?"));
    QCOMPARE(Thoughts::DocumentFeedback::kindOf(commented), Thoughts::DocumentFeedback::other());
    QCOMPARE(commented.value(QStringLiteral("anchor")).toObject(), anchor);
    QCOMPARE(Thoughts::DocumentFeedback::picturesOf(commented), pictures);
    QCOMPARE(part.m_attachmentReader->pageFeedback().size(), 4);

    // None during a review of one of the page's cards.
    const QJsonObject card = attachmentCard(Thoughts::HtmlAttachmentReader::newCardFields(selection, target), QStringLiteral("Loss at epoch 7?"));
    QVERIFY(part.m_attachmentReader->presentReview(card, false));
    QCOMPARE(actions(selection), QStringList{QStringLiteral("attachmentCopy")});
    part.giveLiveFeedback(Thoughts::ReviewCardFeedback::positive(), false, QString(), selection);
    QCOMPARE(written.size(), 4);
    part.m_attachmentReader->clearReview();
    // Nor once another page is shown (the menu was for this one).
    part.closeHtmlAttachment();
    part.saveLiveFeedback(Thoughts::ReviewCardFeedback::positive(), selection, false, QString(), shown);
    QCOMPARE(written.size(), 4);
    part.giveLiveFeedback(Thoughts::ReviewCardFeedback::positive(), false, QString(), selection);
    QCOMPARE(written.size(), 4);

    // An embedded page as before: the viewer's selection, and no picture of the page.
    const QString other = addLibraryPdf(QStringLiteral("runs-feedback-embedded"), QStringLiteral("source_linked"), {});
    const auto embedded = addAttachment(other);
    QVERIFY(embedded.isValid());
    Part plain(nullptr, {});
    plain.openDocument(other);
    const QSignalBlocker quietPlainRepository(plain.m_fedoraRepository);
    auto *plainOverlay = plain.m_pageView->documentFeedback();
    plainOverlay->clear();
    plainOverlay->setWriter([&written](const QJsonObject &record, QString *) {
        written.append(record);
        return true;
    });
    const QString otherSha = sha(readFile(other));
    plainOverlay->setDocument(DocumentId, otherSha);
    plainOverlay->setReviewVersion({{QStringLiteral("pdf_sha256"), otherSha}, {QStringLiteral("source_built"), false}});
    QVERIFY(plain.showHtmlAttachment(embedded, false, &error));
    QVERIFY(plain.m_attachmentView->isAttachment());
    const QJsonObject viewerSelection{{QStringLiteral("type"), QStringLiteral("selection")},
                                      {QStringLiteral("text"), QStringLiteral("Every simulated agent pays the toll")},
                                      {QStringLiteral("text_quote"), QJsonObject{{QStringLiteral("exact"), QStringLiteral("Every simulated agent pays the toll")}}},
                                      {QStringLiteral("page_state"), QJsonObject{{QStringLiteral("hash"), QStringLiteral("#simulator")}}}};
    Q_EMIT plain.m_attachmentView->viewerMessage(viewerSelection);
    {
        QMenu menu;
        plain.addAttachmentMenuActions(&menu, {{QStringLiteral("selection"), viewerSelection}, {QStringLiteral("items"), QJsonArray()}});
        for (QAction *action : menu.actions()) {
            if (action->objectName() == QLatin1String("attachmentFeedbackLike")) action->trigger();
        }
    }
    QCOMPARE(written.size(), 5);
    const QJsonObject embeddedLike = written.constLast();
    QCOMPARE(embeddedLike.value(QStringLiteral("anchor")).toObject().value(QStringLiteral("html_attachment")).toObject().value(QStringLiteral("sha256")).toString(), embedded.sha256);
    QVERIFY(!embeddedLike.contains(QStringLiteral("pictures")));
    plain.closeHtmlAttachment();

    // Reload: a new load of the live page. Its likes, dislikes and questions are from an earlier
    // version now (listed behind "N from earlier versions"); a comment of kind other follows its
    // text until resolved, as on the PDF.
    if (!part.htmlAttachmentState().value(QStringLiteral("shown")).toBool()) QVERIFY(part.showHtmlAttachment(target, false, &error));
    QVERIFY(part.htmlAttachmentState().value(QStringLiteral("live")).toBool());
    const QString loadedVersion = part.attachmentVersion(AttachmentPath);
    QVERIFY(Thoughts::DocumentFeedback::shownOn(liked, pdfSha, loadedVersion));
    QTest::qWait(5); // a later millisecond
    Q_EMIT part.m_attachmentView->liveLoaded(LiveAddress);
    const QString reloadedVersion = part.attachmentVersion(AttachmentPath);
    QVERIFY(reloadedVersion != loadedVersion);
    QVERIFY(reloadedVersion.startsWith(sha(LiveStub)));
    QVERIFY(!Thoughts::DocumentFeedback::shownOn(liked, pdfSha, reloadedVersion));
    QVERIFY(!Thoughts::DocumentFeedback::shownOn(disliked, pdfSha, reloadedVersion));
    QJsonObject note = liked;
    note.insert(QStringLiteral("polarity"), Thoughts::ReviewCardFeedback::neutral());
    note.insert(QStringLiteral("kind"), Thoughts::DocumentFeedback::other());
    QVERIFY(Thoughts::DocumentFeedback::shownOn(note, pdfSha, reloadedVersion));
    // An embedded page has no load: its version is the page itself.
    QCOMPARE(Thoughts::DocumentFeedback::attachmentVersionOf(embedded.sha256), embedded.sha256);
}

void HtmlViewTest::webTabsOfADocumentAndOfNone()
{
    // Web tabs (a site opened from a link belongs in a tab, and its selection gets the PDF's
    // feedback): a web address as a live page of the document whose link
    // opened it, with no stub in the Library and no question for its origin; its feedback names
    // its address as the page. An agent note's link has no document: no feedback.
    QCOMPARE(Thoughts::LivePage::webPagePath(QUrl(QStringLiteral("https://logs.example.org/run/7?x=1#:~:text=loss"))), QStringLiteral("https://logs.example.org/run/7?x=1"));
    QVERIFY(Thoughts::LivePage::webPagePath(QUrl(QStringLiteral("ftp://logs.example.org/"))).isEmpty());
    QVERIFY(Thoughts::LivePage::webPagePath(QUrl(QString(QStringLiteral("https://logs.example.org/") + QString(1100, QLatin1Char('a'))))).isEmpty());
    QVERIFY(Thoughts::LivePage::isWebPagePath(QStringLiteral("http://localhost:8000/")));
    QVERIFY(!Thoughts::LivePage::isWebPagePath(AttachmentPath));
    if (Thoughts::HtmlDocumentView::webViewAvailable()) {
        QSKIP("drives the web tab's load by hand: offscreen only");
    }
    const QString path = addLibraryPdf(QStringLiteral("runs-web"), QStringLiteral("source_linked"), {});
    Thoughts::LivePage::forgetOrigins();
    Part::setHtmlAttachmentsWithoutWebViewForTesting(true);
    OpenedLinks opened;
    for (const char *scheme : {"https", "http", "mailto"}) QDesktopServices::setUrlHandler(QString::fromLatin1(scheme), &opened, "open");
    const auto restore = qScopeGuard([] {
        Part::setHtmlAttachmentsWithoutWebViewForTesting(false);
        Part::setLivePagePictureForTesting(QImage());
        for (const char *scheme : {"https", "http", "mailto"}) QDesktopServices::unsetUrlHandler(QString::fromLatin1(scheme));
    });
    const QUrl address(QStringLiteral("http://localhost:8000/report?page=7#:~:text=results"));
    const QString webPath = QStringLiteral("http://localhost:8000/report?page=7");
    Part part(nullptr, {});
    part.setProperty("thoughtsHtmlAttachmentTab", webPath);
    QVERIFY(part.openHtmlAttachmentTab(QUrl::fromLocalFile(path), {{QStringLiteral("path"), webPath}, {QStringLiteral("address"), address.toString()}}));
    QVERIFY(part.m_document->isOpened());
    QVERIFY(part.isWebTab());
    QVariantMap state = part.htmlAttachmentState();
    QVERIFY(state.value(QStringLiteral("web")).toBool());
    QVERIFY(state.value(QStringLiteral("live")).toBool());
    QCOMPARE(state.value(QStringLiteral("path")).toString(), webPath);
    QCOMPARE(state.value(QStringLiteral("sha256")).toString(), Thoughts::LivePage::webPageSha256(webPath));
    // The address itself, its text fragment too, and nothing asked: the reader clicked the link.
    QCOMPARE(state.value(QStringLiteral("live_url")).toString(), address.toString());
    QVERIFY(!Thoughts::LivePage::originAllowed(QStringLiteral("http://localhost:8000")));
    // Offscreen it is not reached: why, and Retry.
    QCOMPARE(state.value(QStringLiteral("live_state")).toString(), QStringLiteral("unreachable"));
    QCOMPARE(state.value(QStringLiteral("message")).toString(), address.toDisplayString() + QStringLiteral(" could not be loaded.\nthere is no web view for live pages here"));
    QCOMPARE(state.value(QStringLiteral("message_action")).toString(), QStringLiteral("Retry"));
    // Its bar: the address, Reload, Open in Browser and Close; no Take Snapshot (no Library page).
    QCOMPARE(state.value(QStringLiteral("live_bar")).toString(), webPath);
    const auto button = [&part](const char *name) { return part.m_attachmentView->findChild<QToolButton *>(QString::fromLatin1(name)); };
    QVERIFY(!button("thoughtsHtmlLiveReload")->isHidden());
    QVERIFY(button("thoughtsHtmlLiveTakeSnapshot")->isHidden());
    QCOMPARE(button("thoughtsHtmlShowPdf")->text(), QStringLiteral("Close"));
    QCOMPARE(part.htmlAttachmentTitle(), QStringLiteral("localhost"));

    // Loaded: named after the page (offscreen its title cannot be read: its host).
    Q_EMIT part.m_attachmentView->liveLoaded(address);
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_state")).toString(), QStringLiteral("loaded"));
    QCOMPARE(part.htmlAttachmentTitle(), QStringLiteral("localhost"));

    // The PDF's feedback on its selection, in the document the link was in.
    auto *overlay = part.m_pageView->documentFeedback();
    overlay->clear();
    QList<QJsonObject> written;
    overlay->setWriter([&written](const QJsonObject &record, QString *) {
        written.append(record);
        return true;
    });
    const QString pdfSha = sha(readFile(path));
    overlay->setDocument(DocumentId, pdfSha);
    overlay->setReviewVersion({{QStringLiteral("pdf_sha256"), pdfSha}, {QStringLiteral("source_built"), false}});
    QVERIFY(part.liveSelectionFeedbackOffered());
    const QJsonObject selection = Thoughts::HtmlAttachmentReader::liveSelection(QString::fromUtf8(QJsonDocument(QJsonObject{
        {QStringLiteral("exact"), QStringLiteral("watchdog reset at 12:04")}, {QStringLiteral("prefix"), QStringLiteral("log 7: ")}, {QStringLiteral("suffix"), QStringLiteral(" then")},
        {QStringLiteral("hash"), QString()}}).toJson(QJsonDocument::Compact)));
    QMenu menu;
    part.addLiveSelectionMenuActions(&menu, selection);
    QStringList names;
    for (const QAction *action : menu.actions()) names.append(action->isSeparator() ? QStringLiteral("|") : action->objectName());
    QCOMPARE(names, QStringList({QStringLiteral("attachmentFeedbackLike"), QStringLiteral("attachmentFeedbackDislike"), QStringLiteral("attachmentFeedbackQuestion"),
                                 QStringLiteral("attachmentFeedbackComment"), QStringLiteral("|"), QStringLiteral("attachmentCopy")}));
    QImage shown(400, 300, QImage::Format_RGB32);
    shown.fill(Qt::white);
    Part::setLivePagePictureForTesting(shown);
    for (QAction *action : menu.actions()) {
        if (action->objectName() == QLatin1String("attachmentFeedbackLike")) action->trigger();
    }
    QCOMPARE(written.size(), 1);
    const QJsonObject liked = written.constLast();
    QVERIFY2(Thoughts::DocumentFeedback::validationError(liked).isEmpty(), qPrintable(Thoughts::DocumentFeedback::validationError(liked)));
    QCOMPARE(liked.value(QStringLiteral("view")).toString(), QStringLiteral("html_attachment"));
    const QJsonObject anchor = liked.value(QStringLiteral("anchor")).toObject();
    QCOMPARE(anchor.value(QStringLiteral("html_attachment")).toObject(),
             QJsonObject({{QStringLiteral("path"), webPath}, {QStringLiteral("sha256"), Thoughts::LivePage::webPageSha256(webPath)}}));
    QCOMPARE(anchor.value(QStringLiteral("live")).toObject().value(QStringLiteral("address")).toString(), address.toString());
    QCOMPARE(anchor.value(QStringLiteral("pdf")).toObject().value(QStringLiteral("document_id")).toString(), DocumentId);
    QCOMPARE(anchor.value(QStringLiteral("quote")).toObject().value(QStringLiteral("exact")).toString(), QStringLiteral("watchdog reset at 12:04"));
    // On this load of the page, as feedback on a live page.
    QVERIFY(Thoughts::DocumentFeedback::shownOn(liked, pdfSha, part.attachmentVersion(webPath)));
    // Its row is this page, already shown.
    part.showFeedback(liked.value(QStringLiteral("feedback_id")).toString(), false);
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("path")).toString(), webPath);

    // A link the reader clicks stays in the tab, the page's own and another site's; mail goes
    // outside; one the page follows by itself is refused.
    const QUrl other(QStringLiteral("https://wiki.example.org/watchdog"));
    part.openAttachmentLink(other, true);
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_url")).toString(), other.toString());
    QCOMPARE(part.property("thoughtsHtmlAttachmentTab").toString(), other.toString());
    QCOMPARE(part.htmlAttachmentTitle(), QStringLiteral("wiki.example.org"));
    QVERIFY(opened.urls.isEmpty());
    part.openAttachmentLink(QUrl(QStringLiteral("mailto:ops@example.org")), true);
    QCOMPARE(opened.urls, QList<QUrl>{QUrl(QStringLiteral("mailto:ops@example.org"))});
    part.openAttachmentLink(QUrl(QStringLiteral("https://collector.example.org/?x")), false);
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("live_url")).toString(), other.toString());
    QCOMPARE(opened.urls.size(), 1);
    // Open in Browser: what the tab shows now.
    button("thoughtsHtmlOpenInBrowser")->click();
    QCOMPARE(opened.urls.constLast(), other);

    // An agent note's link: a web tab of no document, without feedback.
    Part note(nullptr, {});
    QVERIFY(note.openHtmlAttachmentTab(QUrl(), {{QStringLiteral("path"), webPath}, {QStringLiteral("address"), address.toString()}}));
    QVERIFY(!note.m_document->isOpened());
    QVERIFY(note.isWebTab());
    QVERIFY(note.htmlAttachmentState().value(QStringLiteral("live")).toBool());
    Q_EMIT note.m_attachmentView->liveLoaded(address);
    QVERIFY(!note.liveSelectionFeedbackOffered());
}
}

QTEST_MAIN(Okular::HtmlViewTest)
#include "htmlviewtest.moc"
