// SPDX-License-Identifier: GPL-2.0-or-later
// The reader's HTML view (thoughts-html-view-SPEC.md, "Desktop") on a Part with a temporary
// Library (testhome.cpp): availability, the remembered PDF | HTML switch, and, where a real web
// view exists (THOUGHTS_HTML_GUI_TEST=1 on macOS, not the offscreen platform), the edition in
// the web view: highlights, review mask, reveal, selection menu, answer feedback, new cards and
// comments from a selection. THOUGHTS_HTML_SHOTS=<dir> saves screenshots of each step.
#include "../part/htmldocumentview.h"
#include "../part/htmledition.h"
#include "../part/fedorarepository.h"
#include "../part/htmlreader.h"
#include "../part/htmlattachmentreader.h"
#include "../core/htmlattachment.h"
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
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputDialog>
#include <QMenu>
#include <QPainter>
#include <QScopeGuard>
#include <QScreen>
#include <QShortcut>
#include <QSettings>
#include <QSignalSpy>
#include <QTest>
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
    void attachmentForwardsReviewKeys();
    void attachmentCardFromASelection();
    void attachmentFeedbackFromASelection();
    void attachmentCardReviewWithoutPageShowsStoredAnswer();
    void webViewAttachmentCardsAndReview();
    void webViewAttachmentFeedback();

private:
    // A Library PDF (copy of data/file1.pdf) whose generation lists `state` for its edition.
    QString addLibraryPdf(const QString &stem, const QString &kind, const QJsonObject &htmlFields);
    void shot(Part &part, const QString &name, QWidget *popup = nullptr, Thoughts::HtmlDocumentView *view = nullptr);
    // The attachment fixture in the PDF's source bundle (as Sync installs it) -> its target.
    Thoughts::HtmlAttachmentTarget addAttachment(const QString &pdfPath);
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

Thoughts::HtmlAttachmentTarget HtmlViewTest::addAttachment(const QString &pdfPath)
{
    // <docs>/.runtime/target/source-bundle: the Library's bundle next to current_pdfs/.
    const QDir root(QFileInfo(pdfPath).absolutePath() + QStringLiteral("/../.runtime/target/source-bundle"));
    QDir().mkpath(root.filePath(QStringLiteral("objects")));
    QDir().mkpath(root.filePath(QStringLiteral("documents")));
    const QByteArray html = readFile(QStringLiteral(KDESRCDIR "data/html-attachment/explainer.html"));
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
    // No network for the page: WKScriptMessageHandler carries the viewer's messages.
    QCOMPARE(part.htmlAttachmentState().value(QStringLiteral("transport")).toString(), QStringLiteral("webkit"));
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
}

QTEST_MAIN(Okular::HtmlViewTest)
#include "htmlviewtest.moc"
