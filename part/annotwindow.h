/*
    SPDX-FileCopyrightText: 2006 Chu Xiaodong <xiaodongchu@gmail.com>
    SPDX-FileCopyrightText: 2006 Pino Toscano <pino@kde.org>

    Work sponsored by the LiMux project of the city of Munich:
    SPDX-FileCopyrightText: 2017 Klarälvdalens Datakonsult AB a KDAB Group company <info@kdab.com>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef _ANNOTWINDOW_H_
#define _ANNOTWINDOW_H_

#include "okularpart_export.h"

#include <QColor>
#include <QFrame>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QVariantMap>

namespace Okular
{
class Annotation;
class Document;
}

namespace GuiUtils
{
class LatexRenderer;
}

class KTextEdit;
class MovableTitle;
class QCheckBox;
class QCloseEvent;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QMenu;
class QPushButton;
class QToolButton;

class OKULARPART_EXPORT AnnotWindow : public QFrame
{
    Q_OBJECT
public:
    AnnotWindow(QWidget *parent, Okular::Annotation *annot, Okular::Document *document, int page, bool useCommentThreadUi = false, bool detachedComment = false);
    // These display models belong to this window and are never attached to the PDF.
    AnnotWindow(QWidget *parent, const QList<QJsonObject> &thread, const QString &selectedId, Okular::Document *document, int page);
    void setDetachedThread(const QList<QJsonObject> &thread);
    ~AnnotWindow() override;

    void reloadInfo();

    Okular::Annotation *annotation() const;
    int pageNumber() const;

    void updateAnnotation(Okular::Annotation *a);
    void setRelatedInfo(const QVariantMap &info, const QString &status = QString());
    void setThreadActionInfo(const QVariantMap &info);
    void completeThreadActionOperation(bool success);
    void setRelatedOperationPending(bool pending);
    void completeRelatedOperation(bool success, const QString &message);
    void completeReplyOperation(bool success);
    bool hasUnsavedRelatedChanges() const;

private:
    MovableTitle *m_title;
    KTextEdit *textEdit;
    QColor m_color;
    GuiUtils::LatexRenderer *m_latexRenderer;
    Okular::Annotation *m_annot;
    QList<Okular::Annotation *> m_threadAnnotations;
    Okular::Document *m_document;
    int m_page;
    int m_prevCursorPos;
    int m_prevAnchorPos;
    QToolButton *m_relatedToggle = nullptr;
    QWidget *m_relatedPanel = nullptr;
    QComboBox *m_priorityEditor = nullptr;
    QCheckBox *m_locationEnabled = nullptr;
    QLineEdit *m_locationLabel = nullptr;
    QDoubleSpinBox *m_locationLatitude = nullptr;
    QDoubleSpinBox *m_locationLongitude = nullptr;
    QLineEdit *m_chatGptUrlEditor = nullptr;
    QPushButton *m_applyRelatedButton = nullptr;
    QPushButton *m_saveChatGptButton = nullptr;
    QPushButton *m_openChatGptButton = nullptr;
    QPushButton *m_openCodexButton = nullptr;
    QPushButton *m_openSourceButton = nullptr;
    QPushButton *m_openOverviewButton = nullptr;
    QWidget *m_replyFooter = nullptr;
    QPushButton *m_replyButton = nullptr;
    QWidget *m_replyComposerPanel = nullptr;
    KTextEdit *m_replyComposer = nullptr;
    QPushButton *m_replySendButton = nullptr;
    QLabel *m_relatedStatus = nullptr;
    QLabel *m_linkingAttemptLabel = nullptr;
    QLabel *m_linkingMethodLabel = nullptr;
    QLabel *m_commentTypeLabel = nullptr;
    QString m_originalPriority;
    QJsonObject m_originalLocation;
    bool m_originalLocationPresent = false;
    QString m_originalChatGptUrl;
    QString m_sourcePath;
    int m_sourceLine = 0;
    bool m_relatedInfoAvailable = false;
    bool m_relatedOperationPending = false;
    bool m_loadingRelatedInfo = false;
    QString m_threadRootId;
    bool m_threadNeedsAction = false;
    bool m_threadActionPending = false;
    bool m_threadedCommentUi = false;
    bool m_detachedComment = false;
    bool m_detachedRefreshBlocked = false;
    QLabel *m_detachedRefreshNotice = nullptr;
    QList<Okular::Annotation *> m_ownedThread;
    bool m_replyOperationPending = false;
    QSize m_relatedCollapsedSize;

    QVariantMap relatedMetadataPatch(QString *error) const;
    void updateRelatedActions();
    void fitThreadToContents();

public Q_SLOTS:
    void renderLatex(bool render);

protected:
    void closeEvent(QCloseEvent *event) override;
    void showEvent(QShowEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private Q_SLOTS:
    void slotUpdateUndoAndRedoInContextMenu(QMenu *menu);
    void slotOptionBtn();
    void slotsaveWindowText();
    void slotHandleContentsChangedByUndoRedo(Okular::Annotation *annot, const QString &contents, int cursorPos, int anchorPos);

Q_SIGNALS:
    void containsLatex(bool);
    void aboutToClose(Okular::Annotation *annotation, int page);
    void relatedMetadataSaveRequested(const QVariantMap &metadataPatch);
    void threadActionSaveRequested(const QString &rootId, bool needsAction);
    void relatedChatGptUrlSaveRequested(const QString &url, bool openAfterSave);
    void relatedOpenCodexRequested();
    void relatedOpenChatGptRequested(const QString &url);
    void relatedOpenSourceRequested(const QString &sourcePath, int line);
    void relatedOpenOverviewRequested();
    void replyRequested(const QString &contents);
    void detachedContentsSaveRequested(const QString &contents);
};

#endif
