// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "okularpart_export.h"

#include <QPointer>
#include <QScrollArea>
#include <QString>

#include <functional>

class QBoxLayout;
class QComboBox;
class QResizeEvent;
class QLabel;
class QMenu;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace Thoughts
{
class ReadingOverlayLayer;

// @p text with a place to break in every long run without spaces, so a quote of a formula or an
// address wraps in a narrow label instead of widening it.
OKULARPART_EXPORT QString breakableText(const QString &text);

// The Overlay tab of the Annotations sidebar (reading-overlays.md §4): the open document's reading
// overlays, newest first. Each has its title, the agent's label and its date, an eye that shows or
// hides it on the page (kept on this computer) and a menu with Open Conversation (when the
// conversation is known) and Delete Overlay; below, the agent's summary and the marks in page order
// (the kind's colour, the start of the quote, the comment), those not found in the PDF on screen
// last under "Not in this version". Clicking a mark asks the page to go to it and flash it;
// right-clicking it offers Delete Mark.
class OKULARPART_EXPORT ReadingOverlayList : public QScrollArea
{
    Q_OBJECT
public:
    explicit ReadingOverlayList(QWidget *parent = nullptr);

    // The overlays listed: those of @p layer, followed as they change.
    void setLayer(ReadingOverlayLayer *layer);
    // How many overlays are shown on the page (not hidden with their eye).
    int shownCount() const;
    // Scrolls to @p overlayId's entry and marks it as the current one.
    bool revealOverlay(const QString &overlayId);
    QString revealedOverlay() const;
    // revealOverlay(), and the mark's row in view and marked too (a mark popup's byline).
    bool revealMark(const QString &overlayId, const QString &markId);
    QString revealedMark() const;
    // The menu a right-click on a mark's row opens: Delete Mark.
    QMenu *markMenu(const QString &overlayId, const QString &markId, QWidget *parent);
    // Lists the overlays as they are now (it happens on its own shortly after every change).
    void rebuild();

Q_SIGNALS:
    // A mark was clicked.
    void markRequested(const QString &overlayId, const QString &markId);
    // The number of overlays shown on the page changed.
    void shownCountChanged(int count);
    // Something could not be done (a deletion that could not be queued).
    void failed(const QString &message);

private:
    void scheduleRebuild();
    QWidget *overlayEntry(const QString &overlayId);
    QWidget *markRow(const QString &overlayId, const QString &markId);

    QPointer<ReadingOverlayLayer> m_layer;
    QTimer *m_rebuildTimer = nullptr;
    QString m_revealedMark;
    QString m_revealed;
    int m_shownCount = 0;
};

// Above the Overlay tab's list: a request to paste to an agent, which makes a reading overlay of
// the open document. Pick its kind (readingoverlayrequests.h, remembered), then Copy Request puts
// it on the clipboard.
class OKULARPART_EXPORT ReadingOverlayRequestBar : public QWidget
{
    Q_OBJECT
public:
    // The open document: its name, its Library PDF's file name and its Library document ID.
    struct Document {
        QString name;
        QString filename;
        QString documentId;
    };

    explicit ReadingOverlayRequestBar(QWidget *parent = nullptr);

    void setDocumentProvider(std::function<Document()> provider);
    // The request Copy Request copies now; empty when no document is open.
    QString request() const;
    // Copies the request (what Copy Request does); false when there is none.
    bool copyRequest();

Q_SIGNALS:
    void copied(const QString &request);

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    // One row when the kind's longest name and the button fit side by side, else two.
    void arrange();

    QBoxLayout *m_layout = nullptr;
    QComboBox *m_kind = nullptr;
    QPushButton *m_copy = nullptr;
    std::function<Document()> m_provider;
};
}
