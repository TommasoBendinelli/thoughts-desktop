/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../shell/reviewcardeditor.h"
#include <QTest>
#include <QCryptographicHash>
#include <QDialogButtonBox>
#include <QFile>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QJsonArray>
#include <QApplication>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QUuid>

using namespace Thoughts;
class ReviewCardEditorTest : public QObject
{
    Q_OBJECT
private:
    const QString path = QStringLiteral(KDESRCDIR "data/simple-multipage.pdf");
    ReviewCard card() {
        QFile pdf(path);
        if (!pdf.open(QIODevice::ReadOnly)) return {};
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(pdf.readAll(), QCryptographicHash::Sha256).toHex());
        return {QJsonObject{{QStringLiteral("schema_version"), 4}, {QStringLiteral("status"), QStringLiteral("approved")},
            {QStringLiteral("primary"), QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("pdf_sha256"), hash},
                {QStringLiteral("question"), QStringLiteral("What is on these pages?")},
                {QStringLiteral("regions"), QJsonArray{QJsonObject{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QJsonArray{.1, .2, .5, .4}}},
                                                       QJsonObject{{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QJsonArray{.1, .1, .7, .3}}}}}}}}};
    }
    QPoint point(QWidget *canvas, double x, double y) {
        QSizeF size(595.276, 841.89);
        size.scale(QSizeF(canvas->width() - 24, canvas->height() - 24), Qt::KeepAspectRatio);
        return QPoint(qRound((canvas->width() - size.width()) / 2 + x * size.width()), qRound((canvas->height() - size.height()) / 2 + y * size.height()));
    }
    void drag(QWidget *canvas, QPoint from, QPoint to) {
        QTest::mousePress(canvas, Qt::LeftButton, Qt::NoModifier, from);
        QTest::mouseMove(canvas, to);
        QTest::mouseRelease(canvas, Qt::LeftButton, Qt::NoModifier, to);
    }
private Q_SLOTS:
    void editQuestionAndResizeAcrossPages() {
        const auto original = card();
        int saves = 0;
        QVariantList savedRegions;
        QString savedQuestion;
        ReviewCardEditor editor(original, path, [&](const auto &regions, const auto &question, int pages, QString *) {
            ++saves; savedRegions = regions; savedQuestion = question; return pages >= 2;
        });
        editor.show();
        QVERIFY(QTest::qWaitForWindowExposed(&editor));
        auto *canvas = editor.findChild<QWidget *>(QStringLiteral("reviewCardRegionCanvas"));
        auto *question = editor.findChild<QPlainTextEdit *>(QStringLiteral("reviewCardQuestion"));
        auto *save = editor.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save);
        QVERIFY(canvas && question && save->isEnabled());
        QCOMPARE(editor.regions(), original.regions());
        question->setPlainText(QString());
        QVERIFY(!save->isEnabled());
        question->setPlainText(QStringLiteral("Revised question?"));
        drag(canvas, point(canvas, .5, .4), point(canvas, .65, .5));
        const auto edited = editor.regions().first().toMap().value(QStringLiteral("rect")).toList();
        QVERIFY(qAbs(edited[2].toDouble() - .65) < .004);
        QVERIFY(qAbs(edited[3].toDouble() - .5) < .004);
        QCOMPARE(editor.regions().at(1), original.regions().at(1));
        editor.findChild<QSpinBox *>(QStringLiteral("reviewCardPage"))->setValue(2);
        drag(canvas, point(canvas, .3, .2), point(canvas, .4, .3));
        QVERIFY(editor.regions().at(1) != original.regions().at(1));
        drag(canvas, point(canvas, .2, .6), point(canvas, .7, .8));
        QCOMPARE(editor.regions().size(), 3);
        editor.findChild<QPushButton *>(QStringLiteral("reviewCardRemoveArea"))->click();
        QCOMPARE(editor.regions().size(), 2);
        const QString screenshot = qEnvironmentVariable("THOUGHTS_CARD_EDITOR_SCREENSHOT");
        if (!screenshot.isEmpty()) QVERIFY(editor.grab().save(screenshot));
        save->click();
        QCOMPARE(saves, 1);
        QCOMPARE(savedQuestion, QStringLiteral("Revised question?"));
        QCOMPARE(savedRegions, editor.regions());
        QCOMPARE(editor.result(), int(QDialog::Accepted));
    }
    void cancelAndConflictsDoNotDiscardDraft() {
        int saves = 0;
        ReviewCardEditor editor(card(), path, [&](const auto &, const auto &, int, QString *error) {
            ++saves; *error = QStringLiteral("This card changed during editing."); return false;
        });
        editor.show();
        auto *question = editor.findChild<QPlainTextEdit *>(QStringLiteral("reviewCardQuestion"));
        question->setPlainText(QStringLiteral("Keep this draft"));
        editor.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
        QCOMPARE(saves, 1);
        QVERIFY(editor.isVisible());
        QCOMPARE(editor.question(), QStringLiteral("Keep this draft"));
        QVERIFY(editor.findChild<QLabel *>(QStringLiteral("reviewCardEditError"))->text().contains(QStringLiteral("changed")));
        editor.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Cancel)->click();
        QCOMPARE(saves, 1);
        QCOMPARE(editor.result(), int(QDialog::Rejected));
    }
    void stalePdfCannotBeEdited() {
        auto stale = card();
        auto primary = stale.record.value(QStringLiteral("primary")).toObject();
        primary.insert(QStringLiteral("pdf_sha256"), QString(64, QLatin1Char('f')));
        stale.record.insert(QStringLiteral("primary"), primary);
        ReviewCardEditor editor(stale, path, {});
        QVERIFY(!editor.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isEnabled());
        QVERIFY(!editor.findChild<QLabel *>(QStringLiteral("reviewCardEditError"))->text().isEmpty());
    }
    void areaFailureTextNamesTheArea() {
        const QList<ReviewArea> areas{{1, QRectF(.1, .1, .5, .1)}, {5, QRectF(.1, .5, .5, .1)}, {6, QRectF(.1, .5, .5, .1)}};
        QCOMPARE(reviewAreaFailureText(areas, 1, QStringLiteral("no text")), QStringLiteral("Area 2 of 3 (page 5) can't be linked to source text: no text"));
        QCOMPARE(reviewAreaFailureText({areas.first()}, 0, QStringLiteral("no text")), QStringLiteral("The area on page 1 can't be linked to source text: no text"));
        QCOMPARE(reviewAreaFailureText(areas, -1, QStringLiteral("Sync first.")), QStringLiteral("The card can't be linked to source text: Sync first."));
        // The first area keeps its text-selection kind only while it is unchanged.
        const ReviewArea selection{2, QRectF(QPointF(.1, .2), QPointF(.6, .3)), true};
        const QVariantList regions{QVariantMap{{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QVariantList{.1, .2, .6, .3}}},
                                   QVariantMap{{QStringLiteral("page"), 2}, {QStringLiteral("rect"), QVariantList{.1, .2, .6, .35}}}};
        const auto parsed = reviewAreasFromRegions(regions, selection);
        QCOMPARE(parsed.size(), 2);
        QVERIFY(parsed[0].textSelection);
        QVERIFY(!parsed[1].textSelection);
    }
    void exactStartingAreasAreNotMerged() {
        ReviewCardEditorOptions options;
        options.title = QStringLiteral("Add Review Card");
        options.saveText = QStringLiteral("Add Card");
        options.useAreas = true;
        // Two touching areas would be one mask block; as answer areas they stay two.
        options.areas = {QVariantMap{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QVariantList{.1, .2, .5, .3}}},
                         QVariantMap{{QStringLiteral("page"), 1}, {QStringLiteral("rect"), QVariantList{.1, .3, .5, .4}}}};
        options.page = 1;
        ReviewCardEditor editor(card(), path, {}, options);
        QCOMPARE(editor.windowTitle(), QStringLiteral("Add Review Card"));
        QCOMPARE(editor.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->text(), QStringLiteral("Add Card"));
        QCOMPARE(editor.regions(), options.areas);
        QCOMPARE(editor.areas().size(), 2);
    }
    void newCardIsNotSavedWhileAnAreaCannotBeLinked() {
        // A PDF without installed source information: every Add Card is refused with the reason.
        QTemporaryDir docs;
        QFile pdf(path);
        QVERIFY(pdf.open(QIODevice::ReadOnly));
        const QString hash = QString::fromLatin1(QCryptographicHash::hash(pdf.readAll(), QCryptographicHash::Sha256).toHex());
        const ReviewArea first{1, QRectF(QPointF(.1, .1), QPointF(.6, .2))};
        QStringList messages;
        int step = 0;
        QTimer driver;
        driver.setInterval(30);
        QObject::connect(&driver, &QTimer::timeout, this, [&] {
            QWidget *modal = QApplication::activeModalWidget();
            if (!modal) return;
            if (auto *box = qobject_cast<QMessageBox *>(modal); box && box->objectName() == QLatin1String("reviewAreaFailure")) {
                messages.append(box->text());
                // A card with several areas is never saved for placing its markers by hand.
                QVERIFY(!box->findChild<QPushButton *>(QStringLiteral("reviewAreaPlaceByHand")));
                // Clicks are queued: the driver's timer does not fire again inside its own slot.
                QMetaObject::invokeMethod(box->findChild<QPushButton *>(step == 0 ? QStringLiteral("reviewAreaAdjust") : QStringLiteral("reviewAreaCancel")),
                                          "click", Qt::QueuedConnection);
                ++step;
                return;
            }
            auto *editor = qobject_cast<ReviewCardEditor *>(modal);
            if (!editor || !editor->isVisible()) return;
            if (editor->findChild<QWidget *>(QStringLiteral("reviewCardRegionCanvas"))->width() < 300) return;
            if (step == 0 && editor->areas().size() == 1) {
                // A second area on page 2.
                editor->findChild<QSpinBox *>(QStringLiteral("reviewCardPage"))->setValue(2);
                auto *canvas = editor->findChild<QWidget *>(QStringLiteral("reviewCardRegionCanvas"));
                drag(canvas, point(canvas, .2, .2), point(canvas, .6, .3));
                QCOMPARE(editor->areas().size(), 2);
                QMetaObject::invokeMethod(editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save), "click", Qt::QueuedConnection);
            } else if (step == 1 && messages.size() == 1 && editor->findChild<QLabel *>(QStringLiteral("reviewCardEditError"))->text() == messages.value(0)) {
                // Adjust Areas kept the editor open with both areas and the reason; try again.
                QCOMPARE(editor->areas().size(), 2);
                ++step;
                QMetaObject::invokeMethod(editor->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save), "click", Qt::QueuedConnection);
            }
        });
        driver.start();
        NewReviewCardAreas result;
        const bool chosen = chooseNewReviewCardAreas(nullptr, docs.path(), docs.filePath(QStringLiteral("bundle")), path, hash,
            QUuid::createUuid().toString(QUuid::WithoutBraces), first, QStringLiteral("What is shown?"), &result);
        driver.stop();
        QVERIFY(!chosen);
        QCOMPARE(step, 3);
        QCOMPARE(messages.size(), 2);
        QCOMPARE(messages[0], QStringLiteral("The card can't be linked to source text: Sync this document to install its verified source information."));
    }
};
QTEST_MAIN(ReviewCardEditorTest)
#include "reviewcardeditortest.moc"
